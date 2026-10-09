#include "llvm_backend/http_emitter.hpp"

#include "llvm_backend/runtime_abi.hpp"
#include "quidra/standard_classes.hpp"

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void HttpEmitter::emit(const ir::HttpGet& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.result_type);
    const auto raw=names_.value("http.get.raw"),ok=names_.value("http.get.ok"),result=symbols_.value(n.out);
    builder_.call(raw,runtime_abi::http::get,{{ptr,symbols_.value(n.url)}});
    union_box_.allocate(result,n.result_type);
    builder_.icmp(ok,IntPredicate::ne,ptr,raw,"null");
    const auto yes=names_.label("http.get.ok"),bad=names_.label("http.get.error"),done=names_.label("http.get.done");
    builder_.br(ok,yes,bad);
    builder_.block(yes);
    union_box_.store_tag(result,case_index(n.result_type,Type::class_type(standard_class::http_response)));
    const auto good_payload=names_.value("http.get.response");
    union_box_.payload_slot(good_payload,result);
    builder_.store({ptr,raw},good_payload,Align::none);
    builder_.br(done);
    builder_.block(bad);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::Error)));
    const auto message=names_.value("http.get.message"),error_payload=names_.value("http.get.error.payload");
    builder_.call(message,runtime_abi::http::last_error_copy,{});
    union_box_.payload_slot(error_payload,result);
    builder_.store({ptr,message},error_payload,Align::none);
    builder_.br(done);
    builder_.block(done);
}

void HttpEmitter::emit(const ir::HttpHeader& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.result_type);
    const auto raw=names_.value("http.header.raw"),present=names_.value("http.header.present"),result=symbols_.value(n.out);
    builder_.call(raw,runtime_abi::http::header,{{ptr,symbols_.value(n.response)},{ptr,symbols_.value(n.name)}});
    union_box_.allocate(result,n.result_type);
    builder_.icmp(present,IntPredicate::ne,ptr,raw,"null");
    const auto yes=names_.label("http.header.value"),missing=names_.label("http.header.none"),done=names_.label("http.header.done");
    builder_.br(present,yes,missing);
    builder_.block(yes);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::String)));
    const auto value_payload=names_.value("http.header.value.payload");
    union_box_.payload_slot(value_payload,result);
    builder_.store({ptr,raw},value_payload,Align::none);
    builder_.br(done);
    builder_.block(missing);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::None)));
    builder_.br(done);
    builder_.block(done);
}

} // namespace quidra::llvm_backend
