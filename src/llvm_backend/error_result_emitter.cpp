#include "llvm_backend/error_result_emitter.hpp"

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void ErrorResultEmitter::void_result(ir::ValueId out_id, const Type& result_type,
                                     const std::string& success, const std::string& prefix) {
    symbols_.set_type(out_id,result_type);
    const auto result=symbols_.value(out_id);
    union_box_.allocate(result,result_type);
    const auto ok=names_.label(prefix+".ok"),bad=names_.label(prefix+".error"),done=names_.label(prefix+".done");
    builder_.br(success,ok,bad);
    builder_.block(ok);
    union_box_.store_tag(result,case_index(result_type,Type::simple(TypeKind::Void)));
    builder_.br(done);
    builder_.block(bad);
    union_box_.store_tag(result,case_index(result_type,Type::simple(TypeKind::Error)));
    const auto payload=names_.value(prefix+".error.payload");
    union_box_.payload_slot(payload,result);
    builder_.store({ptr,"@.err.file"},payload,Align::none);
    builder_.br(done);
    builder_.block(done);
}

void ErrorResultEmitter::string_result(ir::ValueId out_id, const Type& result_type,
                                       const std::string& raw, const std::string& prefix) {
    symbols_.set_type(out_id,result_type);
    const auto result=symbols_.value(out_id),oktest=names_.value(prefix+".ok");
    union_box_.allocate(result,result_type);
    builder_.icmp(oktest,IntPredicate::ne,ptr,raw,"null");
    const auto ok=names_.label(prefix+".ok"),bad=names_.label(prefix+".error"),done=names_.label(prefix+".done");
    builder_.br(oktest,ok,bad);
    builder_.block(ok);
    union_box_.store_tag(result,case_index(result_type,Type::simple(TypeKind::String)));
    const auto payload=names_.value(prefix+".string.payload");
    union_box_.payload_slot(payload,result);
    builder_.store({ptr,raw},payload,Align::none);
    builder_.br(done);
    builder_.block(bad);
    union_box_.store_tag(result,case_index(result_type,Type::simple(TypeKind::Error)));
    const auto error_payload=names_.value(prefix+".error.payload");
    union_box_.payload_slot(error_payload,result);
    builder_.store({ptr,"@.err.file"},error_payload,Align::none);
    builder_.br(done);
    builder_.block(done);
}

void ErrorResultEmitter::output_result(ir::ValueId out_id, const Type& result_type, const std::string& status) {
    symbols_.set_type(out_id,result_type);
    const auto result=symbols_.value(out_id),failed=names_.value("output.failed");
    const auto error_label=names_.label("output.error"),ok_label=names_.label("output.ok"),done_label=names_.label("output.done");
    union_box_.allocate(result,result_type);
    builder_.icmp(failed,IntPredicate::ne,i32,status,0);
    builder_.br(failed,error_label,ok_label);
    builder_.block(ok_label);
    union_box_.store_tag(result,case_index(result_type,Type::simple(TypeKind::Void)));
    builder_.br(done_label);
    builder_.block(error_label);
    union_box_.store_tag(result,case_index(result_type,Type::simple(TypeKind::Error)));
    const auto payload=names_.value("output.error.payload");
    union_box_.payload_slot(payload,result);
    builder_.store({ptr,"@.err.output"},payload,Align::none);
    builder_.br(done_label);
    builder_.block(done_label);
}

} // namespace quidra::llvm_backend
