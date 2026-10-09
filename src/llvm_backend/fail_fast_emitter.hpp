#pragma once

// FailFastEmitter: a fail-fast guard of the generated program: when the
// condition holds, quidra_fail_at reports the failure code and message at the
// source line and column (or, for a numeric conversion,
// quidra_runtime_conversion_fail its destination type, subject and reason),
// and the program stops (unreachable); otherwise control continues at the
// next label.
//
// Owns no state. Each guard allocates its two labels (prefix.fail, then
// prefix.ok) from the function's TemporaryNames.

#include "llvm_backend/statement_attribution.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_text/llvm_builder.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace quidra::llvm_backend {

class FailFastEmitter {
public:
    FailFastEmitter(llvm_text::LlvmBuilder& builder,TemporaryNames& names,const StatementAttribution& sites):builder_(builder),names_(names),sites_(sites){}

    void fail_if(const std::string& condition,const std::string& code,const std::string& message,
                 const std::string& prefix,std::uint32_t line=0,std::uint32_t column=0);
    // The guard of a numeric conversion: `type` is the destination's
    // conversion type code (abi::conversion_type_name) and `subject` an
    // abi::ConversionSubject; reason(fail_label) emits, inside the failure
    // block, whatever the reason needs and returns its i32 operand (an
    // abi::ConversionReason), naming its values after the label so that no
    // temporary name is allocated.
    void fail_conversion_if(const std::string& condition,int type,int subject,
                            const std::function<std::string(const std::string&)>& reason,
                            const std::string& prefix,std::uint32_t line,std::uint32_t column);

private:
    llvm_text::LlvmBuilder& builder_;
    TemporaryNames& names_;
    const StatementAttribution& sites_;
};

} // namespace quidra::llvm_backend
