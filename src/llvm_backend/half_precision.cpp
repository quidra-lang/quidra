#include "llvm_backend/half_precision.hpp"

#include "llvm_backend/runtime_abi.hpp"
#include "llvm_text/llvm_callee.hpp"

#include <array>
#include <string>

namespace quidra::llvm_backend {

namespace {

constexpr std::array<const llvm_text::LlvmCallee*, 8> declarations{{
    &runtime_abi::half_support::try_real,
    &runtime_abi::half_support::text,
    &runtime_abi::half_support::parse,
    &runtime_abi::half_support::real16_from_float64,
    &runtime_abi::half_support::real16_to_float32,
    &runtime_abi::half_support::real16b_from_float64,
    &runtime_abi::half_support::real16b_from_int64,
    &runtime_abi::half_support::real16b_from_uint64,
}};

// bfloat16 is the high half of a binary32. Narrowing adds 0x7FFF plus the
// lowest kept bit and drops the low half: round to nearest, ties to even;
// a carry out of the significand increments the exponent, and a value
// beyond the largest finite one becomes the infinity of its sign. A NaN
// becomes the quiet NaN 0x7FC0 (the rounding would turn some into an
// infinity).
constexpr std::string_view helpers = R"LLVM(
define internal float @quidra.half.bf16.widen(i16 %b) alwaysinline {
entry:
  %w = zext i16 %b to i32
  %s = shl i32 %w, 16
  %f = bitcast i32 %s to float
  ret float %f
}

define internal i16 @quidra.half.bf16.narrow(float %f) alwaysinline {
entry:
  %bits = bitcast float %f to i32
  %kept = lshr i32 %bits, 16
  %lowest = and i32 %kept, 1
  %bias = add i32 %lowest, 32767
  %rounded = add i32 %bits, %bias
  %high = lshr i32 %rounded, 16
  %narrow = trunc i32 %high to i16
  %nan = fcmp uno float %f, %f
  %result = select i1 %nan, i16 32704, i16 %narrow
  ret i16 %result
}
)LLVM";

std::string support_text() {
    std::string out = "\n";
    for (const auto* callee : declarations) llvm_text::append_declaration(out, *callee);
    out += helpers;
    return out;
}

} // namespace

// Round binary64 directly into binary16 integer bits in the runtime and bitcast
// to LLVM half. Windows' JIT linker has no __truncdfhf2, which fptrunc
// double->half would otherwise emit; a float intermediate also risks double
// rounding at binary16 midpoints. The C helper uses the ABI's nearest-even
// binary64->binary16 rounding and handles signed infinities and zero.
std::string real16_from_float64(llvm_text::LlvmBuilder& builder, TemporaryNames& names,
                                const std::string& value) {
    using namespace llvm_text;
    using namespace llvm_text::types;
    const auto raw = names.value("half.from.float64.raw");
    builder.call(raw, runtime_abi::half_support::real16_from_float64, {{double_type, value}});
    const auto bits = names.value("half.from.float64.bits");
    builder.cast(bits, CastOp::trunc, {i32, raw}, i16);
    const auto result = names.value("half.from.float64.value");
    builder.cast(result, CastOp::bitcast, {i16, bits}, half_type);
    return result;
}

std::string real16_to_float32(llvm_text::LlvmBuilder& builder, TemporaryNames& names,
                              const std::string& value) {
    using namespace llvm_text;
    using namespace llvm_text::types;
    const auto bits = half_bits(builder, names, Type::simple(TypeKind::Real16), value);
    const auto result = names.value("half.to.float32");
    builder.call(result, runtime_abi::half_support::real16_to_float32, {{i32, bits}});
    return result;
}

std::string half_bits(llvm_text::LlvmBuilder& builder, TemporaryNames& names, const Type& type,
                      const std::string& value) {
    using namespace llvm_text;
    using namespace llvm_text::types;
    std::string bits = value;
    if (type.kind == TypeKind::Real16) {
        bits = names.value("half.bits");
        builder.cast(bits, CastOp::bitcast, {half_type, value}, i16);
    }
    const auto widened = names.value("half.bits.wide");
    builder.cast(widened, CastOp::zext, {i16, bits}, i32);
    return widened;
}

std::span<const llvm_text::LlvmCallee* const> half_support_declarations() {
    return declarations;
}

std::string_view half_support_text() {
    static const std::string text = support_text();
    return text;
}

} // namespace quidra::llvm_backend
