// Autograd lowering: a tensor's .grad, the autograd methods of a tensor
// (track, untrack, retrack, clear_grad, backward, is_tracked, has_grad)
// the methods of an autograd.Target (has_grad, clear_grad, gradient) and
// autograd.target(). backward collects the targets reached from a
// non-tensor argument into a hidden array local, walking the value's type
// as reflection does (reflection_lowering.cpp).

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include "quidra/standard_classes.hpp"
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace quidra::lowering {

ValueId AutogradLowering::lower_tensor_grad(const Expr& e, const MemberExpr& n) {
    const auto tensor_type=type_of(checked_,*n.base);
    const bool base_owned=lifetime_.expression_owns_result(*n.base);
    auto tensor=lowerer_.lower(*n.base),out=builder_.fresh();
    builder_.emit(TensorGrad{
        out,tensor,type_of(checked_,e),
        {static_cast<std::uint32_t>(e.span.start.line),
        static_cast<std::uint32_t>(e.span.start.column)}});
    if(base_owned) release_owned(builder_,tensor,tensor_type);
    return out;
}

std::optional<ValueId> AutogradLowering::try_autograd_target_method(
    const Expr& e, const MethodCallExpr& n, const Type& receiver_type) {
    if(receiver_type.kind==TypeKind::Class &&
       receiver_type.class_name==standard_class::autograd_target){
        auto target=lowerer_.lower(*n.receiver);
        const bool owned=lifetime_.expression_owns_result(*n.receiver);
        const auto finish=[&](ValueId result){
            if(owned) release_owned(builder_,target,receiver_type);
            return result;
        };
        if(n.method=="has_grad"){
            auto out=builder_.fresh();
            builder_.emit(AutogradTargetHasGrad{out,target});
            return finish(out);
        }
        if(n.method=="clear_grad"){
            builder_.emit(AutogradTargetClearGrad{
                target,
                {static_cast<std::uint32_t>(e.span.start.line),
                static_cast<std::uint32_t>(e.span.start.column)}});
            finish(0);
            return 0;
        }
        if(n.method=="gradient"){
            auto out=builder_.fresh();
            builder_.emit(AutogradTargetGradient{
                out,target,type_of(checked_,e),
                {static_cast<std::uint32_t>(e.span.start.line),
                static_cast<std::uint32_t>(e.span.start.column)}});
            return finish(out);
        }
    }
    return std::nullopt;
}

std::optional<ValueId> AutogradLowering::try_tensor_autograd_method(
    const Expr& e, const MethodCallExpr& n, ValueId receiver,
    const Type& receiver_type, bool receiver_owned) {
    auto finish=[&](ValueId out){
        if(receiver_owned) release_owned(builder_,receiver,receiver_type);
        return out;
    };
    if(n.method=="track" || n.method=="untrack" || n.method=="retrack"){
        auto out=builder_.fresh();
        ValueId target=0;
        if(n.method=="track" && !n.args.empty())
            target=lowerer_.lower(*n.args[0].value);
        const int mode=n.method=="track"?ir::tensor_track_mode::track:
                       n.method=="retrack"?ir::tensor_track_mode::retrack:
                       ir::tensor_track_mode::untrack;
        builder_.emit(TensorTrack{
            out,receiver,target,type_of(checked_,e),mode,
            {static_cast<std::uint32_t>(e.span.start.line),
            static_cast<std::uint32_t>(e.span.start.column)}});
        return finish(out);
    }
    if(n.method=="clear_grad"){
        builder_.emit(TensorClearGrad{
            receiver,
            {static_cast<std::uint32_t>(e.span.start.line),
            static_cast<std::uint32_t>(e.span.start.column)}});
        if(receiver_owned) release_owned(builder_,receiver,receiver_type);
        return 0;
    }
    if(n.method=="backward"){
        std::vector<TensorBackwardTarget> targets;
        const auto autograd_target_type=
            Type::class_type(standard_class::autograd_target);
        const auto autograd_target_array=
            Type::array(autograd_target_type);
        std::optional<std::string> dynamic_destination;
        auto track=builder_.const_bool(false);
        for(const auto& argument:n.args){
            if(!argument.writable){
                track=lowerer_.lower(*argument.value);
                continue;
            }
            auto target=lowerer_.lower(*argument.value);
            const auto target_type=type_of(checked_,*argument.value);
            if(target_type.kind==TypeKind::Tensor){
                targets.push_back(TensorBackwardTarget{target,false});
            }else{
                if(!dynamic_destination){
                    dynamic_destination=reflection_.collected_array_local(
                        autograd_target_type,"backward.targets.result");
                }
                const auto plan=checked_.traversal_plans.find(argument.value.get());
                if(plan==checked_.traversal_plans.end()) throw std::logic_error("backward lacks a checked traversal plan");
                reflection_.collect_traversal_values(target,plan->second.tree,*dynamic_destination,true);
            }
        }
        ValueId dynamic_targets=0;
        if(dynamic_destination){
            dynamic_targets=builder_.fresh();
            builder_.emit(LoadLocal{
                dynamic_targets,*dynamic_destination,
                autograd_target_array});
        }
        builder_.emit(TensorBackward{
            receiver,std::move(targets),dynamic_targets,track,
            {static_cast<std::uint32_t>(e.span.start.line),
            static_cast<std::uint32_t>(e.span.start.column)}});
        if(dynamic_targets!=0)
            release_owned(builder_,dynamic_targets,autograd_target_array);
        if(receiver_owned) release_owned(builder_,receiver,receiver_type);
        return 0;
    }
    if(n.method=="is_tracked"){
        auto out=builder_.fresh();
        builder_.emit(TensorIsTracked{out,receiver});
        return finish(out);
    }
    if(n.method=="has_grad"){
        auto out=builder_.fresh();
        builder_.emit(TensorHasGrad{out,receiver});
        return finish(out);
    }
    return std::nullopt;
}

ValueId AutogradLowering::lower_autograd_target() {
    auto out=builder_.fresh();
    builder_.emit(AutogradTargetCreate{out});
    return out;
}

} // namespace quidra::lowering
