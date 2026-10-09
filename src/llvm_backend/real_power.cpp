#include "llvm_backend/real_power.hpp"

#include "llvm_text/llvm_module.hpp"
#include "quidra/abi/runtime_failure.hpp"

#include <string>

namespace quidra::llvm_backend {

namespace {

// The helper of one format: `type` is double or float, `suffix` the
// intrinsic's (f64, f32), `pow` the library function.
std::string power_helper_text(std::string_view name, std::string_view type,
                              std::string_view suffix, std::string_view pow) {
    std::string text = R"LLVM(
define internal @T @@NAME(@T %a, @T %b, i64 %line, i64 %column) {
entry:
  %zero = fcmp oeq @T %a, 0.0
  br i1 %zero, label %zero.base, label %nonzero.base
zero.base:
  %bzero = fcmp oeq @T %b, 0.0
  br i1 %bzero, label %fail.zero, label %zero.exponent
zero.exponent:
  %bnegative = fcmp olt @T %b, 0.0
  br i1 %bnegative, label %fail.negative, label %compute
nonzero.base:
  %anegative = fcmp olt @T %a, 0.0
  br i1 %anegative, label %negative.base, label %compute
negative.base:
  %whole = call @T @llvm.trunc.@S(@T %b)
  %fractional = fcmp une @T %whole, %b
  br i1 %fractional, label %fail.base, label %compute
compute:
  %r = call @T @@POW(@T %a, @T %b)
  ret @T %r
fail.zero:
  call void @quidra_fail_at(ptr @.power.code, ptr @.power.zero, i64 %line, i64 %column)
  unreachable
fail.negative:
  call void @quidra_fail_at(ptr @.power.code, ptr @.power.negative, i64 %line, i64 %column)
  unreachable
fail.base:
  call void @quidra_fail_at(ptr @.power.code, ptr @.power.base, i64 %line, i64 %column)
  unreachable
}
)LLVM";
    const auto replace_all = [&](std::string_view key, std::string_view value) {
        for (auto at = text.find(key); at != std::string::npos; at = text.find(key, at + value.size()))
            text.replace(at, key.size(), value);
    };
    replace_all("@NAME", name);
    replace_all("@POW", pow);
    replace_all("@T", type);
    replace_all("@S", suffix);
    return text;
}

std::string support_text() {
    using abi::FailureReason;
    std::string out = "\n";
    llvm_text::append_string_constant(
        out, ".power.code", abi::spelling(abi::RuntimeFailureCode::power_domain));
    llvm_text::append_string_constant(
        out, ".power.zero", abi::failure_reason_info(FailureReason::zero_power_zero).message);
    llvm_text::append_string_constant(
        out, ".power.negative", abi::failure_reason_info(FailureReason::zero_power_negative).message);
    llvm_text::append_string_constant(
        out, ".power.base",
        abi::failure_reason_info(FailureReason::negative_base_fractional_exponent).message);
    out += "declare double @llvm.trunc.f64(double)\n";
    out += "declare float @llvm.trunc.f32(float)\n";
    out += power_helper_text(power_helper::real64, "double", "f64", "pow");
    out += power_helper_text(power_helper::real32, "float", "f32", "powf");
    return out;
}

} // namespace

std::string_view real_power_support_text() {
    static const std::string text = support_text();
    return text;
}

} // namespace quidra::llvm_backend
