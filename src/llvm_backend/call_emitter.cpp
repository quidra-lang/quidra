#include "llvm_backend/call_emitter.hpp"

#include "llvm_backend/foreign_abi.hpp"
#include "llvm_backend/owned_result_contracts.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "llvm_backend/type_helpers.hpp"
#include "quidra/ir/dtype.hpp"
#include "quidra/abi/call_depth.hpp"
#include "quidra/abi/layout.hpp"
#include "quidra/numeric_types.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void CallEmitter::reserve(const ir::Call& n,const ir::Instruction& ins) {
    if(tuple_indirect_result(n.result)) scratch_.reserve(ins,llvm_type(n.result),Align::eight);
    const auto& target=*callable_functions_.at(n.callee);
    if(target.external_symbol)
        for(const auto& parameter:target.parameters)
            if(is_foreign_tensor_array(parameter.type))
                scratch_.reserve(ins,LlvmType::array(abi::tensor_array_borrow_layout::words,i64),Align::eight);
    if(!node_values_ || !node_values_->active())return;
    const auto contract=node_call_contract(n,target);
    acceptance_->reserve(ins,contract.owning_arguments.size());
}
void CallEmitter::reserve(const ir::IndirectCall& n,const ir::Instruction& ins) {
    if(tuple_indirect_result(n.result)) scratch_.reserve(ins,llvm_type(n.result),Align::eight);
    if(!node_values_ || !node_values_->active())return;
    if(n.args.size()!=n.parameter_types.size())throw std::logic_error("node indirect parameter count mismatch");
    std::size_t owning=0;for(const auto& type:n.parameter_types)if(node_owns_value(type))++owning;
    acceptance_->reserve(ins,owning);
}
void CallEmitter::emit(const ir::FunctionRef& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    if(node_values_ && node_values_->active()) {
        const auto& target=*callable_functions_.at(n.function);
        // Checker function values exclude extern/reference signatures. Keep
        // internal IR producers to the same by-value ownership contract.
        if(target.external_symbol)throw std::logic_error("parallel function value cannot name extern ABI");
        for(const auto& parameter:target.parameters)
            if(parameter.writable || (parameter.borrowed && node_owns_value(parameter.type)))
                throw std::logic_error("parallel function value requires consuming value parameters");
    }
    builder_.select(symbols_.value(n.out),"true",{ptr,LlvmOperand::global(resolver_.call_symbol(n.function))},{ptr,"null"});
}

void CallEmitter::emit(const ir::IndirectCall& n,const ir::Instruction& ins){
    const bool has_result=n.result.kind!=TypeKind::Void&&n.result.kind!=TypeKind::Never;
    if(has_result)symbols_.set_type(n.out,n.result);
    builder_.store({i64,sites_.line(n.position.line)},"@.quidra.source.line",Align::none);
    builder_.store({i64,sites_.column(n.position.column)},"@.quidra.source.column",Align::none);
    // The names the call refers to, alive until the call is written.
    const auto result=has_result?symbols_.value(n.out):std::string{};
    const auto callee=symbols_.value(n.callee);
    std::vector<std::string> arguments;
    arguments.reserve(n.args.size());
    for(const auto argument:n.args) arguments.push_back(symbols_.value(argument));
    const bool indirect=tuple_indirect_result(n.result);
    LlvmCall call(indirect?void_type:llvm_type(n.result),callee);
    if(indirect) call.argument(ptr," "+tuple_result_attribute(n.result),scratch_.instruction_slot(ins));
    if(has_result && !indirect) call.result(result);
    for(std::size_t i=0;i<n.args.size();++i) call.argument({llvm_type(n.parameter_types[i]),arguments[i]});
    std::string ticket;
    if(node_values_ && node_values_->active()) {
        std::vector<CallArgumentOwnerLocation> owners;
        for(std::size_t i=0;i<n.args.size();++i)
            if(node_owns_value(n.parameter_types[i]))owners.push_back(node_values_->argument(n.args[i],n.parameter_types[i]));
        ticket=acceptance_->begin(ins,callee,owners,true);
    }
    builder_.call(call);
    if(node_values_ && node_values_->active())acceptance_->end(ticket);
    if(indirect) builder_.load(result,llvm_type(n.result),scratch_.instruction_slot(ins),Align::eight);
    if(has_result)symbols_.acquire_owned(n.out,n.result,result);
    if(n.result.kind==TypeKind::Never)builder_.unreachable();
}

void CallEmitter::emit(const ir::Call& n,const ir::Instruction& ins){
    if(n.result.kind!=TypeKind::Void&&n.result.kind!=TypeKind::Never) symbols_.set_type(n.out,n.result);
    const auto& parameters=callable_functions_.at(n.callee)->parameters;
    const bool external=external_symbols_.contains(n.callee);
    std::vector<std::string> call_values;
    std::vector<std::string> ffi_lengths(n.args.size());
    call_values.reserve(n.args.size());
    std::vector<std::string> array_views(n.args.size());
    std::size_t array_scratch=tuple_indirect_result(n.result)?1:0;
    for(std::size_t i=0;i<n.args.size();++i){
        const auto& parameter=parameters[i];
        const bool ffi_borrowed_managed=external&&
            (parameter.type.kind==TypeKind::String||
             parameter.type.kind==TypeKind::Bin||
             parameter.type.kind==TypeKind::Tensor);
        const bool ffi_borrowed_buffer=ffi_borrowed_managed&&
            parameter.type.kind!=TypeKind::Tensor;
        std::string argument;
        if(external && is_foreign_tensor_array(parameter.type)) {
            if(!n.args[i].writable_address)
                throw std::logic_error("external tensor array is missing its borrowed storage address");
            const auto& view=scratch_.instruction_slot(ins,array_scratch++);
            const bool writable=parameter.writable && !parameter.is_const;
            const auto clone=writable?clone_name(parameter.type):std::string("null");
            const auto drop=writable?drop_name(parameter.type):std::string("null");
            builder_.call(runtime_abi::tensor::array_borrow_begin,
                {{ptr,view},{ptr,symbols_.value(*n.args[i].writable_address)},
                 {i32,abi::dtype_code(ir::dtype_of(*parameter.type.first->first))},
                 {i1,writable?"true":"false"},{ptr,clone},{ptr,drop},
                 {i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
            argument=view;
            array_views[i]=view;
        }else if(ffi_borrowed_managed){
            if(!n.args[i].writable_address)
                throw std::logic_error("external borrowed managed value is missing its typed storage address");
            const auto borrowed=names_.value("ffi.borrowed.value");
            builder_.load(borrowed,ptr,symbols_.value(*n.args[i].writable_address),Align::none);
            argument=borrowed;
        }else if(parameter.writable){
            argument=symbols_.value(*n.args[i].writable_address);
        }else{
            argument=symbols_.value(n.args[i].value);
        }
        if(ffi_borrowed_buffer&&parameter.type.kind==TypeKind::Bin){
            const auto length=names_.value("ffi.bin.length");
            const auto data=names_.value("ffi.bin.data");
            builder_.call(length,runtime_abi::bin::byte_length,{{ptr,argument}});
            builder_.getelementptr(data,Inbounds::yes,i8,argument,{{i64,abi::bin_layout::payload_offset}});
            ffi_lengths[i]=length;
            argument=data;
        }else if(ffi_borrowed_buffer&&parameter.type.kind==TypeKind::String){
            const auto length=names_.value("ffi.string.length");
            builder_.call(length,runtime_abi::c_library::strlen,{{ptr,argument}});
            ffi_lengths[i]=length;
        }
        call_values.push_back(std::move(argument));
    }
    std::optional<std::string> self_depth_argument;
    if(self_depth_recursive_&&n.callee==fn_.name) {
        const auto next=names_.value("call.depth.next");
        const auto too_deep=names_.value("call.depth.too.deep");
        const auto fail=names_.label("call.depth.fail");
        const auto ok=names_.label("call.depth.ok");
        builder_.binary(next,BinaryOp::add,i64,"%quidra.depth",1);
        builder_.icmp(too_deep,IntPredicate::ugt,i64,next,abi::self_call_depth_limit);
        builder_.br(too_deep,fail,ok);
        builder_.block(fail);
        builder_.call(runtime_abi::prelude::fail_at,{{ptr,"@.code.stack"},{ptr,"@.msg.stack"},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
        builder_.unreachable();
        builder_.block(ok);
        self_depth_argument=next;
    } else if(recursive_callees_.contains(n.callee)) {
        builder_.store({i64,sites_.line(n.position.line)},"@.quidra.source.line",Align::none);
        builder_.store({i64,sites_.column(n.position.column)},"@.quidra.source.column",Align::none);
    }
    const bool has_result=n.result.kind!=TypeKind::Void&&n.result.kind!=TypeKind::Never;
    const auto result=has_result?symbols_.value(n.out):std::string{};
    const auto callee=resolver_.direct_call_symbol(n.callee);
    const bool indirect=tuple_indirect_result(n.result);
    LlvmCall call(indirect?void_type:llvm_type(n.result),LlvmOperand::global(callee));
    if(indirect) call.argument(ptr," "+tuple_result_attribute(n.result),scratch_.instruction_slot(ins));
    if(has_result && !indirect) call.result(result);
    if(external) call.result_attributes(c_abi_return_attribute(n.result));
    for(std::size_t i=0;i<n.args.size();++i){
        const auto& parameter=parameters[i];
        const bool ffi_borrowed_buffer=external&&
            (parameter.type.kind==TypeKind::String||parameter.type.kind==TypeKind::Bin);
        const bool ffi_borrowed_tensor=external&&
            (parameter.type.kind==TypeKind::Tensor || is_foreign_tensor_array(parameter.type));
        if(ffi_borrowed_buffer){
            call.argument(llvm_type(parameter.type),c_abi_parameter_attribute(parameter.type,parameter.is_const),
                          call_values[i]);
            call.argument({i64,ffi_lengths[i]});
        }else if(ffi_borrowed_tensor){
            call.argument(ptr,c_abi_parameter_attribute(parameter.type,parameter.is_const),call_values[i]);
        }else if(parameter.writable){
            call.argument({ptr,call_values[i]});
        }else{
            call.argument(llvm_type(parameter.type),external?c_abi_parameter_attribute(parameter.type):"",
                          call_values[i]);
        }
    }
    if(self_depth_argument) call.argument({i64,*self_depth_argument});
    std::string ticket;
    if(node_values_ && node_values_->active()) {
        const auto contract=node_call_contract(n,*callable_functions_.at(n.callee));
        std::vector<CallArgumentOwnerLocation> owners;
        for(const auto argument:contract.owning_arguments)
            owners.push_back(node_values_->argument(n.args[argument].value,parameters[argument].type));
        ticket=acceptance_->begin(ins,resolver_.call_symbol(n.callee),owners);
    }
    builder_.call(call);
    if(node_values_ && node_values_->active())acceptance_->end(ticket);
    if(indirect) builder_.load(result,llvm_type(n.result),scratch_.instruction_slot(ins),Align::eight);
    if(has_result)symbols_.acquire_owned(n.out,n.result,result);
    if(external&&n.result.kind!=TypeKind::Never&&!n.no_normal_return)
        builder_.call(runtime_abi::program::foreign_leave,{});
    if(n.result.kind==TypeKind::Never || n.no_normal_return)
        builder_.unreachable();
    else if(external&&is_fixed_real(n.result))
        check_foreign_real(n,result);
    // A real tensor the extern function could write (a `&` argument that is
    // not const) is validated, unless the package declared the function
    // (quidra_tensor_validate_package reads its op flags).
    if(external&&n.result.kind!=TypeKind::Never&&!n.no_normal_return){
        for(std::size_t i=0;i<n.args.size();++i){
            const auto& parameter=parameters[i];
            if(!array_views[i].empty()) {
                if(parameter.writable && !parameter.is_const) {
                    const auto symbol=pool_.intern(external_symbols_.at(n.callee));
                    const auto name=pool_.intern(callable_functions_.at(n.callee)->name);
                    builder_.call(runtime_abi::tensor::array_validate_package,
                        {{ptr,array_views[i]},{ptr,LlvmOperand::global(symbol)},{ptr,LlvmOperand::global(name)},
                         {i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
                }
                builder_.call(runtime_abi::tensor::array_borrow_end,{{ptr,array_views[i]}});
                continue;
            }
            if(parameter.type.kind!=TypeKind::Tensor||parameter.is_const||!parameter.writable||
               !parameter.type.first||!is_fixed_real(*parameter.type.first)||!n.args[i].writable_address)
                continue;
            const auto written=names_.value("ffi.written.tensor");
            builder_.load(written,ptr,symbols_.value(*n.args[i].writable_address),Align::none);
            const auto symbol=pool_.intern(external_symbols_.at(n.callee));
            const auto name=pool_.intern(callable_functions_.at(n.callee)->name);
            builder_.call(runtime_abi::finite_support::validate_package,
                          {{ptr,written},{ptr,LlvmOperand::global(symbol)},{ptr,LlvmOperand::global(name)},
                           {i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
        }
    }
}

void CallEmitter::check_foreign_real(const ir::Call& n,const std::string& result){
    // x - x is NaN exactly when x is NaN or an infinity.
    const auto type=llvm_type(n.result);
    const auto difference=names_.value("ffi.real.difference"),bad=names_.value("ffi.real.bad");
    builder_.binary(difference,BinaryOp::fsub,type,result,result);
    builder_.fcmp(bad,FloatPredicate::uno,type,difference,difference);
    const auto fail=names_.label("ffi.real.fail"),ok=names_.label("ffi.real.ok");
    builder_.br(bad,fail,ok);
    builder_.block(fail);
    const auto name=pool_.intern(callable_functions_.at(n.callee)->name);
    builder_.call(runtime_abi::finite_support::extern_nonfinite,
                  {{i32,abi::dtype_code(*numeric_kind_info(n.result.kind)->dtype)},
                   {ptr,LlvmOperand::global(name)},
                   {i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
    builder_.unreachable();
    builder_.block(ok);
}

} // namespace quidra::llvm_backend
