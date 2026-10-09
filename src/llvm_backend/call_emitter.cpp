#include "llvm_backend/call_emitter.hpp"

#include "llvm_backend/foreign_abi.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "quidra/abi/call_depth.hpp"
#include "quidra/abi/layout.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void CallEmitter::emit(const ir::FunctionRef& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    builder_.select(symbols_.value(n.out),"true",{ptr,LlvmOperand::global(resolver_.call_symbol(n.function))},{ptr,"null"});
}

void CallEmitter::emit(const ir::IndirectCall& n,const ir::Instruction&){
    const bool has_result=n.result.kind!=TypeKind::Void&&n.result.kind!=TypeKind::Never;
    if(has_result)symbols_.set_type(n.out,n.result);
    builder_.store({i64,sites_.line(n.line)},"@.quidra.source.line",Align::none);
    builder_.store({i64,sites_.column(n.column)},"@.quidra.source.column",Align::none);
    // The names the call refers to, alive until the call is written.
    const auto result=has_result?symbols_.value(n.out):std::string{};
    const auto callee=symbols_.value(n.callee);
    std::vector<std::string> arguments;
    arguments.reserve(n.args.size());
    for(const auto argument:n.args) arguments.push_back(symbols_.value(argument));
    LlvmCall call(llvm_type(n.result),callee);
    if(has_result) call.result(result);
    for(std::size_t i=0;i<n.args.size();++i) call.argument({llvm_type(n.parameter_types[i]),arguments[i]});
    builder_.call(call);
    if(n.result.kind==TypeKind::Never)builder_.unreachable();
}

void CallEmitter::emit(const ir::Call& n,const ir::Instruction&){
    if(n.result.kind!=TypeKind::Void&&n.result.kind!=TypeKind::Never) symbols_.set_type(n.out,n.result);
    const auto& parameters=callable_functions_.at(n.callee)->parameters;
    const bool external=external_symbols_.contains(n.callee);
    std::vector<std::string> call_values;
    std::vector<std::string> ffi_lengths(n.args.size());
    call_values.reserve(n.args.size());
    for(std::size_t i=0;i<n.args.size();++i){
        const auto& parameter=parameters[i];
        const bool ffi_borrowed_managed=external&&
            (parameter.type.kind==TypeKind::String||
             parameter.type.kind==TypeKind::Bin||
             parameter.type.kind==TypeKind::Tensor);
        const bool ffi_borrowed_buffer=ffi_borrowed_managed&&
            parameter.type.kind!=TypeKind::Tensor;
        std::string argument;
        if(ffi_borrowed_managed){
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
        builder_.call(runtime_abi::prelude::fail_at,{{ptr,"@.code.stack"},{ptr,"@.msg.stack"},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
        builder_.unreachable();
        builder_.block(ok);
        self_depth_argument=next;
    } else if(recursive_callees_.contains(n.callee)) {
        builder_.store({i64,sites_.line(n.line)},"@.quidra.source.line",Align::none);
        builder_.store({i64,sites_.column(n.column)},"@.quidra.source.column",Align::none);
    }
    const bool has_result=n.result.kind!=TypeKind::Void&&n.result.kind!=TypeKind::Never;
    const auto result=has_result?symbols_.value(n.out):std::string{};
    const auto callee=resolver_.direct_call_symbol(n.callee);
    LlvmCall call(llvm_type(n.result),LlvmOperand::global(callee));
    if(has_result) call.result(result);
    if(external) call.result_attributes(c_abi_return_attribute(n.result));
    for(std::size_t i=0;i<n.args.size();++i){
        const auto& parameter=parameters[i];
        const bool ffi_borrowed_buffer=external&&
            (parameter.type.kind==TypeKind::String||parameter.type.kind==TypeKind::Bin);
        const bool ffi_borrowed_tensor=external&&
            parameter.type.kind==TypeKind::Tensor;
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
    builder_.call(call);
    if(n.result.kind==TypeKind::Never || n.no_normal_return)
        builder_.unreachable();
}

} // namespace quidra::llvm_backend
