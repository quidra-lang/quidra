// Console lowering: print and flush. A fail-fast argument of print is
// consumed first: an error fails the program instead of being printed.

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"

namespace quidra::lowering {

ValueId ConsoleLowering::lower_print(const Expr& e, const CallExpr& n) {
    const auto argument_type=type_of(checked_,*n.args[0].value);
    const bool fail_fast_argument =
        checked_.fail_fast_expressions.contains(n.args[0].value.get());
    auto v=fail_fast_argument
        ? lowerer_.lower_into(*n.args[0].value,argument_type)
        : lowerer_.lower(*n.args[0].value);
    auto out=builder_.fresh();
    builder_.emit(Print{v,argument_type,out,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,v);
    return out;
}

ValueId ConsoleLowering::lower_flush(const Expr& e) {
    auto out=builder_.fresh();
    builder_.emit(Flush{out,checked_.raw_types.at(&e)});
    return out;
}

} // namespace quidra::lowering
