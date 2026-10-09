// Concurrency lowering: the methods of an atomic.Counter (add, load),
// atomic.counter() and task.all. The counter is lowered once, before the
// method is chosen, and an owned counter is released after the call.

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include "quidra/standard_classes.hpp"
#include <optional>

namespace quidra::lowering {

std::optional<ValueId> ConcurrencyLowering::try_counter_method(
    const Expr& e, const MethodCallExpr& n, const Type& receiver_type) {
    if(receiver_type.kind==TypeKind::Class &&
       receiver_type.class_name==standard_class::atomic_counter){
        auto counter=lowerer_.lower(*n.receiver);
        const bool owned=lifetime_.expression_owns_result(*n.receiver);
        const auto finish=[&](ValueId result){
            if(owned) builder_.emit(Release{counter,receiver_type});
            return result;
        };
        if(n.method=="add"){
            // The counter holds an int64; its operands and results are ints.
            auto delta=lowerer_.lower_int64(*n.args[0].value),out=builder_.fresh();
            builder_.emit(AtomicCounterAdd{out,counter,delta,static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)});
            return finish(lowerer_.bare_from_int64(out,type_of(checked_,e),false));
        }
        if(n.method=="load"){
            auto out=builder_.fresh();
            builder_.emit(AtomicCounterLoad{out,counter});
            return finish(lowerer_.bare_from_int64(out,type_of(checked_,e),false));
        }
    }
    return std::nullopt;
}

ValueId ConcurrencyLowering::lower_atomic_counter(const CallExpr& n) {
    auto initial=lowerer_.lower_int64(*n.args[0].value),out=builder_.fresh();
    builder_.emit(AtomicCounterCreate{out,initial});
    return out;
}

ValueId ConcurrencyLowering::lower_task_all(const Expr& e, const CallExpr& n) {
    auto operations=lowerer_.lower(*n.args[0].value);
    const auto result=checked_.raw_types.at(&e);
    const auto result_type =
        result.kind==TypeKind::Array && result.first
            ? *result.first
            : Type::simple(TypeKind::Void);
    const auto out=result_type.kind==TypeKind::Void ? 0 : builder_.fresh();
    ValueId shared=0;
    Type shared_type=Type::simple(TypeKind::Void);
    if(n.args.size()==2){
        shared=lowerer_.lower(*n.args[1].value);
        shared_type=type_of(checked_,*n.args[1].value);
    }
    builder_.emit(TaskAll{
        out,operations,shared,result_type,shared_type,
        static_cast<std::uint32_t>(e.span.start.line),
        static_cast<std::uint32_t>(e.span.start.column)});
    lifetime_.release_temporary(*n.args[0].value,operations);
    if(n.args.size()==2) lifetime_.release_temporary(*n.args[1].value,shared);
    return out;
}
} // namespace quidra::lowering
