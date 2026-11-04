// Check lowering: test.check and test.equal, each a TestAssert of its
// condition.

#include "lowering/lowerer.hpp"

namespace quidra::lowering {

ValueId CheckLowering::lower_test_check(const CallExpr& n) {
    auto condition=lowerer_.lower_argument(*n.args[0].value);
    builder_.emit(TestAssert{condition});
    return 0;
}

ValueId CheckLowering::lower_test_equal(const CallExpr& n) {
    auto actual=lowerer_.lower_argument(*n.args[0].value),expected_value=lowerer_.lower_argument(*n.args[1].value);
    auto equal=builder_.fresh();
    builder_.emit(Binary{
        equal,"==",actual,expected_value,lowerer_.argument_type(*n.args[0].value),Type::simple(TypeKind::Bool)});
    builder_.emit(TestAssert{equal});
    lowerer_.release_argument(*n.args[0].value,actual);
    lowerer_.release_argument(*n.args[1].value,expected_value);
    return 0;
}

} // namespace quidra::lowering
