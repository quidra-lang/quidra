// Random lowering: random.generator and the methods of a generator (int,
// float, bool); a method's receiver is the enclosing class's $receiver.

#include "lowering/lowerer.hpp"

namespace quidra::lowering {

ValueId RandomLowering::lower_random_generator(const CallExpr& n) {
    auto seed=lowerer_.lower_int64(*n.args[0].value),out=builder_.fresh();
    builder_.emit(RandomGenerator{out,seed});
    return out;
}

ValueId RandomLowering::lower_random_int(const Expr& e, const CallExpr& n) {
    auto generator=load_receiver(builder_,enclosing_class_);
    // The generator draws an int64 within [start, end); the result is an int.
    auto start=lowerer_.lower_int64(*n.args[0].value),end=lowerer_.lower_int64(*n.args[1].value),out=builder_.fresh();
    builder_.emit(RandomInt{out,generator,start,end,static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)});
    return lowerer_.bare_from_int64(out,Type::simple(TypeKind::Int),false);
}

ValueId RandomLowering::lower_random_float() {
    auto generator=load_receiver(builder_,enclosing_class_),out=builder_.fresh();
    builder_.emit(RandomFloat{out,generator});
    return out;
}

ValueId RandomLowering::lower_random_bool() {
    auto generator=load_receiver(builder_,enclosing_class_),out=builder_.fresh();
    builder_.emit(RandomBool{out,generator});
    return out;
}

} // namespace quidra::lowering
