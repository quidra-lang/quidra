// JSON lowering: json.parse and the methods of a JSON value (kind, size,
// get, at, the typed reads, encode, equal); a method's receiver is the
// enclosing JSON class's $receiver.

#include "lowering/lowerer.hpp"

namespace quidra::lowering {

ValueId JsonLowering::lower_json_parse(const Expr& e, const CallExpr& n) {
    auto text=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(JsonParse{out,text,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,text);
    return out;
}

ValueId JsonLowering::lower_json_kind() {
    auto value=load_receiver(builder_,enclosing_class_),out=builder_.fresh();
    builder_.emit(JsonKind{out,value});
    return out;
}

ValueId JsonLowering::lower_json_size(const Expr& e) {
    auto value=load_receiver(builder_,enclosing_class_),out=builder_.fresh();
    builder_.emit(JsonSize{out,value,checked_.raw_types.at(&e)});
    return out;
}

ValueId JsonLowering::lower_json_get(const Expr& e, const CallExpr& n) {
    auto value=load_receiver(builder_,enclosing_class_),key=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(JsonGet{out,value,key,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,key);
    return out;
}

ValueId JsonLowering::lower_json_at(const Expr& e, const CallExpr& n) {
    auto value=load_receiver(builder_,enclosing_class_),index=lowerer_.lower_int64(*n.args[0].value),out=builder_.fresh();
    builder_.emit(JsonAt{out,value,index,checked_.raw_types.at(&e)});
    return out;
}

ValueId JsonLowering::lower_json_text(const Expr& e) {
    auto value=load_receiver(builder_,enclosing_class_),out=builder_.fresh();
    builder_.emit(JsonText{out,value,checked_.raw_types.at(&e)});
    return out;
}

ValueId JsonLowering::lower_json_integer(const Expr& e) {
    // An exact read of the number's preserved text.
    auto value=load_receiver(builder_,enclosing_class_),out=builder_.fresh();
    builder_.emit(JsonBigInt{out,value,checked_.raw_types.at(&e)});
    return out;
}

ValueId JsonLowering::lower_json_number(const Expr& e) {
    auto value=load_receiver(builder_,enclosing_class_),out=builder_.fresh();
    builder_.emit(JsonNumber{out,value,checked_.raw_types.at(&e)});
    return out;
}

ValueId JsonLowering::lower_json_big_real(const Expr& e) {
    auto value=load_receiver(builder_,enclosing_class_),out=builder_.fresh();
    builder_.emit(JsonBigReal{out,value,checked_.raw_types.at(&e)});
    return out;
}

ValueId JsonLowering::lower_json_boolean(const Expr& e) {
    auto value=load_receiver(builder_,enclosing_class_),out=builder_.fresh();
    builder_.emit(JsonBoolean{out,value,checked_.raw_types.at(&e)});
    return out;
}

ValueId JsonLowering::lower_json_encode() {
    auto value=load_receiver(builder_,enclosing_class_),out=builder_.fresh();
    builder_.emit(JsonEncode{out,value});
    return out;
}

ValueId JsonLowering::lower_json_equal(const CallExpr& n) {
    auto left=load_receiver(builder_,enclosing_class_),right=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(JsonEqual{out,left,right});
    lifetime_.release_temporary(*n.args[0].value,right);
    return out;
}

} // namespace quidra::lowering
