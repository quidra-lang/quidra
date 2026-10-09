// HTTP lowering: http.get and a response's header().

#include "lowering/lowerer.hpp"

namespace quidra::lowering {

ValueId HttpLowering::lower_http_get(const Expr& e, const CallExpr& n) {
    auto url=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(HttpGet{out,url,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,url);
    return out;
}

ValueId HttpLowering::lower_http_header(const Expr& e, const CallExpr& n) {
    auto response=load_receiver(builder_,enclosing_class_),name=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(HttpHeader{out,response,name,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,name);
    return out;
}

} // namespace quidra::lowering
