// Check lowering: test.check and test.equal, each a TestAssert of its
// condition.

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"

namespace quidra::lowering {

ValueId CheckLowering::lower_test_check(const CallExpr& n) {
    auto condition=lowerer_.lower(*n.args[0].value);
    builder_.emit(TestAssert{condition});
    return 0;
}

ValueId CheckLowering::lower_test_equal(const CallExpr& n) {
    auto actual=lowerer_.lower(*n.args[0].value),expected_value=lowerer_.lower(*n.args[1].value);
    auto equal=builder_.fresh();
    builder_.emit(Binary{
        equal,"==",actual,expected_value,type_of(checked_,*n.args[0].value),Type::simple(TypeKind::Bool)});
    builder_.emit(TestAssert{equal});
    lifetime_.release_temporary(*n.args[0].value,actual);
    lifetime_.release_temporary(*n.args[1].value,expected_value);
    return 0;
}

} // namespace quidra::lowering
