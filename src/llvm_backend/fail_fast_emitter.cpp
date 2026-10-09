#include "llvm_backend/fail_fast_emitter.hpp"

#include "llvm_backend/runtime_abi.hpp"

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void FailFastEmitter::fail_if(const std::string& condition,const std::string& code,const std::string& message,
                              const std::string& prefix,std::uint32_t line,std::uint32_t column){
    const auto bad=names_.label(prefix+".fail"),ok=names_.label(prefix+".ok");
    builder_.br(condition,bad,ok);
    builder_.block(bad);
    builder_.call(runtime_abi::prelude::fail_at,{{ptr,code},{ptr,message},{i64,sites_.line(line)},{i64,sites_.column(column)}});
    builder_.unreachable();
    builder_.block(ok);
}

void FailFastEmitter::fail_conversion_if(const std::string& condition,int type,int subject,
                                         const std::function<std::string(const std::string&)>& reason,
                                         const std::string& prefix,std::uint32_t line,std::uint32_t column){
    const auto bad=names_.label(prefix+".fail"),ok=names_.label(prefix+".ok");
    builder_.br(condition,bad,ok);
    builder_.block(bad);
    const auto code=reason(bad);
    builder_.call(runtime_abi::failure::conversion_fail,{{i32,type},{i32,subject},{i32,code},{i64,sites_.line(line)},{i64,sites_.column(column)}});
    builder_.unreachable();
    builder_.block(ok);
}

} // namespace quidra::llvm_backend
