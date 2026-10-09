// Bin lowering: the bin type's own methods (bin.fill).

#include "lowering/lowerer.hpp"
#include <optional>

namespace quidra::lowering {

std::optional<ValueId> BinLowering::try_bin_type_method(const MethodCallExpr& n) {
    const auto* receiver_name=std::get_if<NameExpr>(&n.receiver->data);
    if(receiver_name && receiver_name->name=="bin" && n.method=="fill"){
        auto length=lowerer_.lower_int64(*n.args[0].value),fill=lowerer_.lower_int64(*n.args[1].value),out=builder_.fresh();
        builder_.emit(BinAlloc{out,length,fill});
        return out;
    }
    return std::nullopt;
}

} // namespace quidra::lowering
