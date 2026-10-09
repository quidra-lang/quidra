// Tensor lowering: the methods of a tensor (device transfer, transpose,
// contiguous, reshape, gather, scatter, shape, device, item, all, any).
// The receiver is lowered once, before the method is chosen; the autograd
// methods are tried first (autograd_lowering.cpp); an owned receiver is
// released after the call. Also tensor.create, tensor.zeros and tensor.ones
// (without a shape argument, the shape comes from the binding's declared
// extents) and gpu.sync.

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include <optional>
#include <stdexcept>

namespace quidra::lowering {

ValueId TensorLowering::lower_runtime_array(const Expr& argument, bool& converted) {
    const auto runtime_type=Type::array(Type::simple(TypeKind::Int64));
    converted=type_of(checked_,argument)!=runtime_type;
    // A literal shape ([2, 3]) is built as int64 elements directly, so that
    // it stays a constant array instead of a converted copy.
    if(const auto* literal=std::get_if<ArrayExpr>(&argument.data);
       literal && converted && !checked_.fail_fast_expressions.contains(&argument)){
        auto raw=checked_.raw_types.at(&argument);
        if(raw.kind==TypeKind::Array && raw.first && is_integer_family_type(*raw.first)){
            std::vector<ValueId> values;
            values.reserve(literal->elements.size());
            for(const auto& element:literal->elements)
                values.push_back(lowerer_.lower_int64(*element));
            raw.first=std::make_shared<Type>(Type::simple(TypeKind::Int64));
            auto made=builder_.fresh();
            builder_.emit(ArrayMake{made,std::move(values),raw});
            if(raw==runtime_type) return made;
            auto out=lowerer_.convert(made,raw,runtime_type);
            builder_.emit(Release{made,raw});
            return out;
        }
    }
    return lowerer_.lower_into(argument,runtime_type);
}

void TensorLowering::release_runtime_array(const Expr& argument, ValueId value, bool converted) {
    if(converted) builder_.emit(Release{value,Type::array(Type::simple(TypeKind::Int64))});
    else lifetime_.release_temporary(argument,value);
}

std::optional<ValueId> TensorLowering::try_tensor_method(
    const Expr& e, const MethodCallExpr& n, const Type& receiver_type) {
    if(receiver_type.kind==TypeKind::Tensor){
        auto receiver=lowerer_.lower(*n.receiver);
        const bool receiver_owned=lifetime_.expression_owns_result(*n.receiver);
        auto finish=[&](ValueId out){
            if(receiver_owned) builder_.emit(Release{receiver,receiver_type});
            return out;
        };
        if (const auto out = autograd_.try_tensor_autograd_method(
                e, n, receiver, receiver_type, receiver_owned))
            return *out;
        if(n.method=="gpu"){
            auto gpu=lowerer_.lower_int64(*n.args[0].value),out=builder_.fresh();
            builder_.emit(TensorTransfer{
                out,receiver,gpu,receiver_type,
                static_cast<std::uint32_t>(e.span.start.line),
                static_cast<std::uint32_t>(e.span.start.column)});
            return finish(out);
        }
        if(n.method=="cpu"){
            auto out=builder_.fresh();
            builder_.emit(TensorTransfer{
                out,receiver,std::nullopt,receiver_type,
                static_cast<std::uint32_t>(e.span.start.line),
                static_cast<std::uint32_t>(e.span.start.column)});
            return finish(out);
        }
        if(n.method=="transpose"){
            auto axis0=lowerer_.lower_int64(*n.args[0].value);
            auto axis1=lowerer_.lower_int64(*n.args[1].value);
            auto out=builder_.fresh();
            builder_.emit(TensorTranspose{
                out,receiver,axis0,axis1,type_of(checked_,e),
                static_cast<std::uint32_t>(e.span.start.line),
                static_cast<std::uint32_t>(e.span.start.column)});
            return finish(out);
        }
        if(n.method=="contiguous"){
            auto out=builder_.fresh();
            builder_.emit(TensorContiguous{
                out,receiver,receiver_type,
                static_cast<std::uint32_t>(e.span.start.line),
                static_cast<std::uint32_t>(e.span.start.column)});
            return finish(out);
        }
        if(n.method=="reshape"){
            bool shape_converted=false;
            auto shape=lower_runtime_array(*n.args[0].value,shape_converted),out=builder_.fresh();
            builder_.emit(TensorReshape{
                out,receiver,shape,receiver_type,
                static_cast<std::uint32_t>(e.span.start.line),
                static_cast<std::uint32_t>(e.span.start.column)});
            release_runtime_array(*n.args[0].value,shape,shape_converted);
            return finish(out);
        }
        if(n.method=="gather"){
            bool indices_converted=false,shape_converted=false;
            auto indices=lower_runtime_array(*n.args[0].value,indices_converted);
            auto shape=lower_runtime_array(*n.args[1].value,shape_converted);
            auto out=builder_.fresh();
            builder_.emit(TensorGather{
                out,receiver,indices,shape,type_of(checked_,e),
                static_cast<std::uint32_t>(e.span.start.line),
                static_cast<std::uint32_t>(e.span.start.column)});
            release_runtime_array(*n.args[0].value,indices,indices_converted);
            release_runtime_array(*n.args[1].value,shape,shape_converted);
            return finish(out);
        }
        if(n.method=="scatter"){
            bool indices_converted=false,shape_converted=false;
            auto indices=lower_runtime_array(*n.args[0].value,indices_converted);
            auto shape=lower_runtime_array(*n.args[1].value,shape_converted);
            auto out=builder_.fresh();
            builder_.emit(TensorScatter{
                out,receiver,indices,shape,type_of(checked_,e),
                static_cast<std::uint32_t>(e.span.start.line),
                static_cast<std::uint32_t>(e.span.start.column)});
            release_runtime_array(*n.args[0].value,indices,indices_converted);
            release_runtime_array(*n.args[1].value,shape,shape_converted);
            return finish(out);
        }
        if(n.method=="shape"){
            // The runtime writes the extents as int64; the result's
            // elements are words.
            const auto result_type=type_of(checked_,e);
            auto runtime_type=Type::array(Type::simple(TypeKind::Int64),result_type.length);
            auto extents=builder_.fresh();
            builder_.emit(TensorShape{extents,receiver,runtime_type});
            auto out=lowerer_.convert(extents,runtime_type,result_type);
            builder_.emit(Release{extents,runtime_type});
            return finish(out);
        }
        if(n.method=="device"){
            auto out=builder_.fresh();
            builder_.emit(TensorDevice{out,receiver});
            return finish(lowerer_.bare_from_int64(out,type_of(checked_,e),true));
        }
        if(n.method=="is_contiguous"){
            auto out=builder_.fresh();
            builder_.emit(TensorIsContiguous{out,receiver});
            return finish(out);
        }
        if(n.method=="item"){
            auto out=builder_.fresh();
            builder_.emit(TensorItem{
                out,receiver,*receiver_type.first,
                static_cast<std::uint32_t>(e.span.start.line),
                static_cast<std::uint32_t>(e.span.start.column)});
            return finish(out);
        }
        if(n.method=="all" || n.method=="any"){
            auto out=builder_.fresh();
            builder_.emit(TensorBoolReduce{
                out,receiver,n.method=="all",
                static_cast<std::uint32_t>(e.span.start.line),
                static_cast<std::uint32_t>(e.span.start.column)});
            return finish(out);
        }
    }
    return std::nullopt;
}

ValueId TensorLowering::lower_tensor_create(
    const Expr& e, const CallExpr& n, const CallResolution& resolution) {
    const auto shape_type=Type::array(Type::simple(TypeKind::Int64));
    ValueId shape{};
    std::optional<ValueId> gpu;
    std::optional<std::size_t> shape_argument;
    bool generated=false;
    bool shape_converted=false;
    for(std::size_t i=0;i<n.args.size();++i){
        const auto& argument=n.args[i];
        if(argument.name && *argument.name=="gpu"){
            gpu=lowerer_.lower_int64(*argument.value);
        }else{
            shape=lower_runtime_array(*argument.value,shape_converted);
            shape_argument=i;
        }
    }
    if(!shape_argument){
        const auto* found=shapes_.initializer_shape(&e);
        if(!found)
            throw std::logic_error("missing contextual tensor shape capture");
        shape=shape_constraints_.generated_shape_array(*found,e.span);
        generated=true;
    }
    auto out=builder_.fresh();
    const auto type=checked_.raw_types.at(&e);
    const int fill_mode=
        *resolution.builtin==BuiltinCallable::TensorZeros ? ir::tensor_fill_mode::zeros :
        *resolution.builtin==BuiltinCallable::TensorOnes ? ir::tensor_fill_mode::ones :
        ir::tensor_fill_mode::uninitialized;
    builder_.emit(TensorCreate{
        out,shape,gpu,type,fill_mode,
        static_cast<std::uint32_t>(e.span.start.line),
        static_cast<std::uint32_t>(e.span.start.column)});
    if(generated) builder_.emit(Release{shape,shape_type});
    else release_runtime_array(*n.args[*shape_argument].value,shape,shape_converted);
    return out;
}

ValueId TensorLowering::lower_gpu_sync(const Expr& e, const CallExpr& n) {
    auto index=lowerer_.lower_int64(*n.args[0].value);
    builder_.emit(GpuSync{index,static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)});
    return 0;
}
} // namespace quidra::lowering
