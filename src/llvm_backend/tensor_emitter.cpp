#include "llvm_backend/tensor_emitter.hpp"

#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "quidra/abi/runtime_failure.hpp"
#include "quidra/abi/tensor_codes.hpp"
#include "quidra/abi/tensor_index.hpp"
#include "quidra/ir/dtype.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

namespace {

// The runtime's code of a tensor element type (quidra_tensor_create, casts).
int dtype_code_of(const Type& element) { return abi::dtype_code(ir::dtype_of(element)); }

// quidra_tensor_binary's operation for a binary operator.
int binary_opcode(std::string_view op) {
    if (op == "+") return abi::tensor_binary_opcode::add;
    if (op == "-") return abi::tensor_binary_opcode::subtract;
    if (op == "*") return abi::tensor_binary_opcode::multiply;
    if (op == "/") return abi::tensor_binary_opcode::divide;
    if (op == "%") return abi::tensor_binary_opcode::remainder;
    if (op == "^") return abi::tensor_binary_opcode::power;
    throw std::logic_error("unsupported tensor binary operator");
}

// quidra_tensor_compare's operation for a comparison operator.
int comparison_opcode(std::string_view op) {
    if (op == "==") return abi::tensor_comparison_opcode::equal;
    if (op == "!=") return abi::tensor_comparison_opcode::not_equal;
    if (op == "<") return abi::tensor_comparison_opcode::less;
    if (op == "<=") return abi::tensor_comparison_opcode::less_equal;
    if (op == ">") return abi::tensor_comparison_opcode::greater;
    if (op == ">=") return abi::tensor_comparison_opcode::greater_equal;
    throw std::logic_error("unsupported tensor comparison operator");
}

} // namespace

void TensorEmitter::reserve(const ir::TensorBinary& n,const ir::Instruction& ins){
    const bool left_tensor=n.left_type.kind==TypeKind::Tensor;
    const bool right_tensor=n.right_type.kind==TypeKind::Tensor;
    if(left_tensor!=right_tensor){
        const auto& scalar_type=left_tensor?n.right_type:n.left_type;
        scratch_.reserve(ins,llvm_type(scalar_type),Align::none);
    }
}

void TensorEmitter::reserve(const ir::TensorCompare& n,const ir::Instruction& ins){
    const bool left_tensor=n.left_type.kind==TypeKind::Tensor;
    const bool right_tensor=n.right_type.kind==TypeKind::Tensor;
    if(left_tensor!=right_tensor){
        const auto& scalar_type=left_tensor?n.right_type:n.left_type;
        scratch_.reserve(ins,llvm_type(scalar_type),Align::none);
    }
}

void TensorEmitter::reserve(const ir::TensorIndex& n,const ir::Instruction& ins){
    scratch_.reserve(ins,LlvmType::array(n.items.size()*abi::tensor_index_spec::fields,i64),Align::eight);
}

void TensorEmitter::reserve(const ir::TensorSet& n,const ir::Instruction& ins){
    scratch_.reserve(ins,LlvmType::array(n.indices.size(),i64),Align::eight);
    scratch_.reserve(ins,llvm_type(n.element_type),Align::none);
}

void TensorEmitter::emit(const ir::TensorCreate& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    builder_.call(symbols_.value(n.out),runtime_abi::tensor::create,{{ptr,symbols_.value(n.shape)},{i32,dtype_code_of(*n.type.first)},{i32,n.fill_mode},{i1,n.gpu?"1":"0"},{i64,n.gpu?symbols_.value(*n.gpu):"0"},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

void TensorEmitter::emit(const ir::TensorTransfer& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    if(n.gpu)
        builder_.call(symbols_.value(n.out),runtime_abi::tensor::to_gpu,{{ptr,symbols_.value(n.tensor)},{i64,symbols_.value(*n.gpu)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    else
        builder_.call(symbols_.value(n.out),runtime_abi::tensor::to_cpu,{{ptr,symbols_.value(n.tensor)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

void TensorEmitter::emit(const ir::TensorReshape& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    builder_.call(symbols_.value(n.out),runtime_abi::tensor::reshape,{{ptr,symbols_.value(n.tensor)},{ptr,symbols_.value(n.shape)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

void TensorEmitter::emit(const ir::TensorGather& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    builder_.call(symbols_.value(n.out),runtime_abi::tensor::gather,{{ptr,symbols_.value(n.tensor)},{ptr,symbols_.value(n.indices)},{ptr,symbols_.value(n.shape)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

void TensorEmitter::emit(const ir::TensorScatter& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    builder_.call(symbols_.value(n.out),runtime_abi::tensor::scatter,{{ptr,symbols_.value(n.tensor)},{ptr,symbols_.value(n.indices)},{ptr,symbols_.value(n.shape)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

void TensorEmitter::emit(const ir::TensorTranspose& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    builder_.call(symbols_.value(n.out),runtime_abi::tensor::transpose,{{ptr,symbols_.value(n.tensor)},{i64,symbols_.value(n.axis0)},{i64,symbols_.value(n.axis1)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

void TensorEmitter::emit(const ir::TensorContiguous& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    builder_.call(symbols_.value(n.out),runtime_abi::tensor::contiguous,{{ptr,symbols_.value(n.tensor)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

void TensorEmitter::emit(const ir::TensorShape& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    if(n.type.length>=0)
        builder_.call(symbols_.value(n.out),runtime_abi::tensor::shape_fixed,{{ptr,symbols_.value(n.tensor)},{i64,n.type.length}});
    else
        builder_.call(symbols_.value(n.out),runtime_abi::tensor::shape,{{ptr,symbols_.value(n.tensor)}});
}

void TensorEmitter::emit(const ir::TensorDevice& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Int64));
    builder_.call(symbols_.value(n.out),runtime_abi::tensor::device,{{ptr,symbols_.value(n.tensor)}});
}

void TensorEmitter::emit(const ir::TensorIsContiguous& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bool));
    builder_.call(symbols_.value(n.out),runtime_abi::tensor::is_contiguous,{{ptr,symbols_.value(n.tensor)}});
}

void TensorEmitter::emit(const ir::TensorItem& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.element_type);
    const auto slot=names_.value("tensor.item");
    builder_.call(slot,runtime_abi::tensor::item_ptr,{{ptr,symbols_.value(n.tensor)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    builder_.load(symbols_.value(n.out),llvm_type(n.element_type),slot,Align::one);
}

void TensorEmitter::emit(const ir::TensorCast& n,const ir::Instruction&){
    const bool fallible =
        n.result_type.kind==TypeKind::Union && n.result_type.union_name.empty() &&
        case_index(n.result_type,Type::simple(TypeKind::Error))>=0;
    if(!fallible){
        symbols_.set_type(n.out,n.target_type);
        builder_.call(symbols_.value(n.out),runtime_abi::tensor::cast,{{ptr,symbols_.value(n.tensor)},{i32,dtype_code_of(*n.target_type.first)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }else{
        symbols_.set_type(n.out,n.result_type);
        const auto raw=names_.value("tensor.cast.raw");
        builder_.call(raw,runtime_abi::tensor::try_cast,{{ptr,symbols_.value(n.tensor)},{i32,dtype_code_of(*n.target_type.first)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
        const auto ok=names_.value("tensor.cast.ok");
        builder_.icmp(ok,IntPredicate::ne,ptr,raw,"null");
        const auto result=symbols_.value(n.out);
        union_box_.allocate(result,n.result_type);
        const auto yes=names_.label("tensor.cast.value");
        const auto bad=names_.label("tensor.cast.error");
        const auto done=names_.label("tensor.cast.done");
        builder_.br(ok,yes,bad);
        builder_.block(yes);
        union_box_.store_tag(result,case_index(n.result_type,n.target_type));
        const auto payload=names_.value("tensor.cast.payload");
        union_box_.payload_slot(payload,result);
        builder_.store({ptr,raw},payload,Align::none);
        builder_.br(done);
        builder_.block(bad);
        union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::Error)));
        const auto error_payload=names_.value("tensor.cast.error.payload");
        union_box_.payload_slot(error_payload,result);
        // A tensor element fails only outside the destination's range.
        const auto message=error_payload+".message";
        builder_.call(message,runtime_abi::failure::conversion_message,
                      {{i32,dtype_code_of(*n.target_type.first)},
                       {i32,static_cast<int>(abi::ConversionSubject::tensor_element)},
                       {i32,static_cast<int>(abi::ConversionReason::out_of_range)}});
        builder_.store({ptr,message},error_payload,Align::none);
        builder_.br(done);
        builder_.block(done);
    }
}

void TensorEmitter::emit(const ir::TensorCompare& n,const ir::Instruction& ins){
    symbols_.set_type(n.out,n.result_type);
    const bool left_tensor=n.left_type.kind==TypeKind::Tensor;
    const bool right_tensor=n.right_type.kind==TypeKind::Tensor;
    if(left_tensor && right_tensor){
        builder_.call(symbols_.value(n.out),runtime_abi::tensor::compare,{{ptr,symbols_.value(n.left)},{ptr,symbols_.value(n.right)},{ptr,"null"},{i32,abi::scalar_side::none},{i32,comparison_opcode(n.op)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }else{
        const auto scalar=left_tensor?n.right:n.left;
        const auto scalar_type=left_tensor?n.right_type:n.left_type;
        const auto& slot=scratch_.instruction_slot(ins);
        builder_.store({llvm_type(scalar_type),symbols_.value(scalar)},slot,Align::one);
        const auto tensor=left_tensor?n.left:n.right;
        const int side=left_tensor?abi::scalar_side::right:abi::scalar_side::left;
        builder_.call(symbols_.value(n.out),runtime_abi::tensor::compare,{{ptr,symbols_.value(tensor)},{ptr,"null"},{ptr,slot},{i32,side},{i32,comparison_opcode(n.op)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }
}

void TensorEmitter::emit(const ir::TensorBoolReduce& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bool));
    builder_.call(symbols_.value(n.out),runtime_abi::tensor::bool_reduce,{{ptr,symbols_.value(n.tensor)},{i1,n.all?"1":"0"},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

void TensorEmitter::emit(const ir::TensorBinary& n,const ir::Instruction& ins){
    symbols_.set_type(n.out,n.result_type);
    const bool left_tensor=n.left_type.kind==TypeKind::Tensor;
    const bool right_tensor=n.right_type.kind==TypeKind::Tensor;
    if(left_tensor && right_tensor){
        builder_.call(symbols_.value(n.out),runtime_abi::tensor::binary,{{ptr,symbols_.value(n.left)},{ptr,symbols_.value(n.right)},{ptr,"null"},{i32,abi::scalar_side::none},{i32,binary_opcode(n.op)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }else{
        const auto scalar=n.left_type.kind==TypeKind::Tensor?n.right:n.left;
        const auto scalar_type=n.left_type.kind==TypeKind::Tensor?n.right_type:n.left_type;
        const auto& slot=scratch_.instruction_slot(ins);
        builder_.store({llvm_type(scalar_type),symbols_.value(scalar)},slot,Align::one);
        const auto tensor=n.left_type.kind==TypeKind::Tensor?n.left:n.right;
        const int side=n.left_type.kind==TypeKind::Tensor?abi::scalar_side::right:abi::scalar_side::left;
        builder_.call(symbols_.value(n.out),runtime_abi::tensor::binary,{{ptr,symbols_.value(tensor)},{ptr,"null"},{ptr,slot},{i32,side},{i32,binary_opcode(n.op)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }
}

void TensorEmitter::emit(const ir::TensorIndex& n,const ir::Instruction& ins){
    symbols_.set_type(n.out,n.type);
    namespace spec=abi::tensor_index_spec;
    const auto count=n.items.size();
    const auto& specs=scratch_.instruction_slot(ins);
    constexpr long long missing=spec::missing;
    for(std::size_t i=0;i<count;++i){
        const auto& item=n.items[i];
        const auto base=i*spec::fields;
        const auto emit_slot=[&](std::size_t offset,const std::string& value_text){
            const auto slot=names_.value("tensor.index.slot");
            builder_.getelementptr(slot,Inbounds::yes,LlvmType::array(count*spec::fields,i64),specs,{{i64,0},{i64,base+offset}});
            builder_.store({i64,value_text},slot,Align::eight);
        };
        emit_slot(spec::kind_field,std::to_string(item.slice?spec::slice:spec::index));
        if(item.slice){
            emit_slot(spec::first_part_field,item.start?symbols_.value(*item.start):std::to_string(missing));
            emit_slot(spec::first_part_field+1,item.stop?symbols_.value(*item.stop):std::to_string(missing));
            emit_slot(spec::first_part_field+2,item.step?symbols_.value(*item.step):std::to_string(missing));
        }else{
            emit_slot(spec::first_part_field,item.index?symbols_.value(*item.index):std::to_string(missing));
            emit_slot(spec::first_part_field+1,std::to_string(missing));
            emit_slot(spec::first_part_field+2,std::to_string(missing));
        }
    }
    builder_.call(symbols_.value(n.out),runtime_abi::tensor::index,{{ptr,symbols_.value(n.tensor)},{ptr,specs},{i64,count},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

void TensorEmitter::emit(const ir::TensorSet& n,const ir::Instruction& ins){
    const auto count=n.indices.size();
    const auto& indices=scratch_.instruction_slot(ins,0);
    for(std::size_t i=0;i<count;++i){
        const auto slot=names_.value("tensor.set.index");
        builder_.getelementptr(slot,Inbounds::yes,LlvmType::array(count,i64),indices,{{i64,0},{i64,i}});
        builder_.store({i64,symbols_.value(n.indices[i])},slot,Align::eight);
    }
    const auto& scalar=scratch_.instruction_slot(ins,1);
    builder_.store({llvm_type(n.element_type),symbols_.value(n.value)},scalar,Align::one);
    builder_.call(runtime_abi::tensor::set,{{ptr,symbols_.value(n.tensor)},{ptr,indices},{i64,count},{ptr,scalar},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

void TensorEmitter::emit(const ir::GpuSync& n,const ir::Instruction&){
    builder_.call(runtime_abi::tensor::gpu_sync,{{i64,symbols_.value(n.index)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

} // namespace quidra::llvm_backend
