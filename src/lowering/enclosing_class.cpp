// load_receiver: the receiver of the method or constructor being lowered.

#include "lowering/enclosing_class.hpp"

namespace quidra::lowering {

ValueId load_receiver(ir::FunctionBuilder& builder, const EnclosingClass& enclosing_class) {
    auto out=builder.fresh();
    const auto type=Type::class_type(enclosing_class.name());
    builder.emit(LoadLocal{out,"$receiver",type});
    return out;
}

} // namespace quidra::lowering
