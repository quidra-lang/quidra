#include "llvm_backend/small_rational.hpp"

#include "llvm_backend/runtime_abi.hpp"
#include "quidra/abi/exact_codes.hpp"
#include "quidra/abi/layout.hpp"

#include <array>
#include <cstdint>
#include <limits>

namespace quidra::llvm_backend {

namespace {

// The helpers below spell the ABI as numbers: a real is { i64, i64 }, the
// numerator first, and the binary operations are numbered as the runtime
// numbers them.
static_assert(abi::exact_real_layout::numerator_offset == 0 &&
              abi::exact_real_layout::denominator_offset == 8 &&
              abi::exact_real_layout::bytes == 16);
static_assert(abi::exact_binary_opcode::add == 1 && abi::exact_binary_opcode::subtract == 2 &&
              abi::exact_binary_opcode::multiply == 3);

constexpr std::array<const llvm_text::LlvmCallee*, 13> declarations{{
    &runtime_abi::exact_real_support::real_promote,
    &runtime_abi::exact_real_support::real_demote,
    &runtime_abi::exact_real_support::real_small_binary,
    &runtime_abi::exact_real_support::real_small_text,
    &runtime_abi::exact_real_support::real_small_try_float64,
    &runtime_abi::exact_real_support::real_small_try_float32,
    &runtime_abi::exact_real_support::real_small_to_float64,
    &runtime_abi::exact_real_support::real_small_to_float32,
    &runtime_abi::exact_real_support::real_small_try_i64,
    &runtime_abi::exact_real_support::real_small_try_u64,
    &runtime_abi::exact_real_support::real_small_try_bigint,
    &runtime_abi::exact_real_support::real_small_to_bigint,
    &runtime_abi::exact_real_support::real_small_cast_fits,
}};

constexpr std::string_view helpers = R"LLVM(
define internal void @quidra.real.retain({ i64, i64 } %v) alwaysinline {
entry:
  %d = extractvalue { i64, i64 } %v, 1
  %general = icmp eq i64 %d, 0
  br i1 %general, label %node, label %done
node:
  %n = extractvalue { i64, i64 } %v, 0
  %p = inttoptr i64 %n to ptr
  call void @quidra_managed_retain(ptr %p)
  br label %done
done:
  ret void
}

define internal void @quidra.real.release({ i64, i64 } %v) alwaysinline {
entry:
  %d = extractvalue { i64, i64 } %v, 1
  %general = icmp eq i64 %d, 0
  br i1 %general, label %node, label %done
node:
  %n = extractvalue { i64, i64 } %v, 0
  %p = inttoptr i64 %n to ptr
  call void @quidra_managed_release(ptr %p, ptr @quidra_bigreal_drop)
  br label %done
done:
  ret void
}

define internal { i64, i64 } @quidra.real.adopt_node(ptr %node) {
entry:
  %slot = alloca { i64, i64 }, align 8
  call void @quidra_real_demote(ptr %node, ptr %slot)
  %v = load { i64, i64 }, ptr %slot, align 8
  ret { i64, i64 } %v
}

define internal { i64, i64 } @quidra.real.literal(ptr %text) {
entry:
  %node = call ptr @quidra_bigreal_literal(ptr %text)
  %v = call { i64, i64 } @quidra.real.adopt_node(ptr %node)
  ret { i64, i64 } %v
}

define internal { i64, i64 } @quidra.real.parse(ptr %text) {
entry:
  %node = call ptr @quidra_bigreal_parse(ptr %text)
  %failed = icmp eq ptr %node, null
  br i1 %failed, label %none, label %value
none:
  ret { i64, i64 } zeroinitializer
value:
  %v = call { i64, i64 } @quidra.real.adopt_node(ptr %node)
  ret { i64, i64 } %v
}

define internal i1 @quidra.real.is_none({ i64, i64 } %v) alwaysinline {
entry:
  %n = extractvalue { i64, i64 } %v, 0
  %d = extractvalue { i64, i64 } %v, 1
  %n.zero = icmp eq i64 %n, 0
  %d.zero = icmp eq i64 %d, 0
  %none = and i1 %n.zero, %d.zero
  ret i1 %none
}

define internal { i64, i64 } @quidra.real.unary(ptr %provider, i32 %opcode, { i64, i64 } %v) {
entry:
  %n = extractvalue { i64, i64 } %v, 0
  %d = extractvalue { i64, i64 } %v, 1
  %input = call ptr @quidra_real_promote(i64 %n, i64 %d)
  %node = call ptr @qcore_exact_real_unary(ptr %provider, i32 %opcode, ptr %input)
  call void @quidra_managed_release(ptr %input, ptr @quidra_bigreal_drop)
  %r = call { i64, i64 } @quidra.real.adopt_node(ptr %node)
  ret { i64, i64 } %r
}

define internal ptr @quidra.real.text({ i64, i64 } %v, i32 %significant) {
entry:
  %n = extractvalue { i64, i64 } %v, 0
  %d = extractvalue { i64, i64 } %v, 1
  %general = icmp eq i64 %d, 0
  br i1 %general, label %node, label %small
small:
  %small.text = call ptr @quidra_real_small_text(i64 %n, i64 %d, i32 %significant)
  ret ptr %small.text
node:
  %p = inttoptr i64 %n to ptr
  %node.text = call ptr @quidra_bigreal_text(ptr %p, i32 %significant)
  ret ptr %node.text
}

define internal { i64, i64 } @quidra.real.neg({ i64, i64 } %v, i64 %line, i64 %column) alwaysinline {
entry:
  %n = extractvalue { i64, i64 } %v, 0
  %d = extractvalue { i64, i64 } %v, 1
  %general = icmp eq i64 %d, 0
  br i1 %general, label %node, label %small
small:
  %m = sub i64 0, %n
  %r = insertvalue { i64, i64 } %v, i64 %m, 0
  ret { i64, i64 } %r
node:
  %p = inttoptr i64 %n to ptr
  %result = call ptr @quidra_bigreal_neg(ptr %p, i64 %line, i64 %column)
  %s = call { i64, i64 } @quidra.real.adopt_node(ptr %result)
  ret { i64, i64 } %s
}

define internal { i64, i64 } @quidra.real.binary({ i64, i64 } %a, { i64, i64 } %b, i32 %op, i64 %line, i64 %column) alwaysinline {
entry:
  %an = extractvalue { i64, i64 } %a, 0
  %ad = extractvalue { i64, i64 } %a, 1
  %bn = extractvalue { i64, i64 } %b, 0
  %bd = extractvalue { i64, i64 } %b, 1
  %a.integer = icmp eq i64 %ad, 1
  %b.integer = icmp eq i64 %bd, 1
  %integers = and i1 %a.integer, %b.integer
  br i1 %integers, label %integer, label %slow
integer:
  switch i32 %op, label %slow [ i32 1, label %add
                                 i32 2, label %sub
                                 i32 3, label %mul ]
add:
  %add.pair = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %an, i64 %bn)
  %add.value = extractvalue { i64, i1 } %add.pair, 0
  %add.overflow = extractvalue { i64, i1 } %add.pair, 1
  br i1 %add.overflow, label %slow, label %result
sub:
  %sub.pair = call { i64, i1 } @llvm.ssub.with.overflow.i64(i64 %an, i64 %bn)
  %sub.value = extractvalue { i64, i1 } %sub.pair, 0
  %sub.overflow = extractvalue { i64, i1 } %sub.pair, 1
  br i1 %sub.overflow, label %slow, label %result
mul:
  %mul.pair = call { i64, i1 } @llvm.smul.with.overflow.i64(i64 %an, i64 %bn)
  %mul.value = extractvalue { i64, i1 } %mul.pair, 0
  %mul.overflow = extractvalue { i64, i1 } %mul.pair, 1
  br i1 %mul.overflow, label %slow, label %result
result:
  %value = phi i64 [ %add.value, %add ], [ %sub.value, %sub ], [ %mul.value, %mul ]
  %minimum = icmp eq i64 %value, -9223372036854775808
  br i1 %minimum, label %slow, label %done
done:
  %r = insertvalue { i64, i64 } %a, i64 %value, 0
  ret { i64, i64 } %r
slow:
  %s = call { i64, i64 } @quidra.real.binary.slow(i64 %an, i64 %ad, i64 %bn, i64 %bd, i32 %op, i64 %line, i64 %column)
  ret { i64, i64 } %s
}

define internal { i64, i64 } @quidra.real.binary.slow(i64 %an, i64 %ad, i64 %bn, i64 %bd, i32 %op, i64 %line, i64 %column) {
entry:
  %slot = alloca { i64, i64 }, align 8
  %a.small = icmp ne i64 %ad, 0
  %b.small = icmp ne i64 %bd, 0
  %small = and i1 %a.small, %b.small
  br i1 %small, label %rational, label %node
rational:
  %ok = call i1 @quidra_real_small_binary(i64 %an, i64 %ad, i64 %bn, i64 %bd, i32 %op, ptr %slot)
  br i1 %ok, label %rational.done, label %node
rational.done:
  %rv = load { i64, i64 }, ptr %slot, align 8
  ret { i64, i64 } %rv
node:
  %na = call ptr @quidra_real_promote(i64 %an, i64 %ad)
  %nb = call ptr @quidra_real_promote(i64 %bn, i64 %bd)
  %nr = call ptr @quidra_bigreal_binary(ptr %na, ptr %nb, i32 %op, i64 %line, i64 %column)
  call void @quidra_managed_release(ptr %na, ptr @quidra_bigreal_drop)
  call void @quidra_managed_release(ptr %nb, ptr @quidra_bigreal_drop)
  call void @quidra_real_demote(ptr %nr, ptr %slot)
  %nv = load { i64, i64 }, ptr %slot, align 8
  ret { i64, i64 } %nv
}

define internal i32 @quidra.real.compare({ i64, i64 } %a, { i64, i64 } %b, i64 %line, i64 %column) alwaysinline {
entry:
  %an = extractvalue { i64, i64 } %a, 0
  %ad = extractvalue { i64, i64 } %a, 1
  %bn = extractvalue { i64, i64 } %b, 0
  %bd = extractvalue { i64, i64 } %b, 1
  %a.small = icmp ne i64 %ad, 0
  %b.small = icmp ne i64 %bd, 0
  %small = and i1 %a.small, %b.small
  br i1 %small, label %rational, label %node
rational:
  %an.wide = sext i64 %an to i128
  %ad.wide = sext i64 %ad to i128
  %bn.wide = sext i64 %bn to i128
  %bd.wide = sext i64 %bd to i128
  %left = mul i128 %an.wide, %bd.wide
  %right = mul i128 %bn.wide, %ad.wide
  %less = icmp slt i128 %left, %right
  %greater = icmp sgt i128 %left, %right
  %less.i = zext i1 %less to i32
  %greater.i = zext i1 %greater to i32
  %order = sub i32 %greater.i, %less.i
  ret i32 %order
node:
  %r = call i32 @quidra.real.compare.slow(i64 %an, i64 %ad, i64 %bn, i64 %bd, i64 %line, i64 %column)
  ret i32 %r
}

define internal i32 @quidra.real.compare.slow(i64 %an, i64 %ad, i64 %bn, i64 %bd, i64 %line, i64 %column) {
entry:
  %na = call ptr @quidra_real_promote(i64 %an, i64 %ad)
  %nb = call ptr @quidra_real_promote(i64 %bn, i64 %bd)
  %r = call i32 @quidra_bigreal_compare(ptr %na, ptr %nb, i64 %line, i64 %column)
  call void @quidra_managed_release(ptr %na, ptr @quidra_bigreal_drop)
  call void @quidra_managed_release(ptr %nb, ptr @quidra_bigreal_drop)
  ret i32 %r
}

define internal { i64, i64 } @quidra.real.pow({ i64, i64 } %a, { i64, i64 } %b, i64 %line, i64 %column) {
entry:
  %an = extractvalue { i64, i64 } %a, 0
  %ad = extractvalue { i64, i64 } %a, 1
  %bn = extractvalue { i64, i64 } %b, 0
  %bd = extractvalue { i64, i64 } %b, 1
  %na = call ptr @quidra_real_promote(i64 %an, i64 %ad)
  %nb = call ptr @quidra_real_promote(i64 %bn, i64 %bd)
  %nr = call ptr @quidra_bigreal_pow(ptr %na, ptr %nb, i64 %line, i64 %column)
  call void @quidra_managed_release(ptr %na, ptr @quidra_bigreal_drop)
  call void @quidra_managed_release(ptr %nb, ptr @quidra_bigreal_drop)
  %r = call { i64, i64 } @quidra.real.adopt_node(ptr %nr)
  ret { i64, i64 } %r
}

define internal { i64, i64 } @quidra.real.from_i64(i64 %x) alwaysinline {
entry:
  %minimum = icmp eq i64 %x, -9223372036854775808
  br i1 %minimum, label %node, label %small
small:
  %s0 = insertvalue { i64, i64 } undef, i64 %x, 0
  %s = insertvalue { i64, i64 } %s0, i64 1, 1
  ret { i64, i64 } %s
node:
  %p = call ptr @quidra_bigreal_from_i64(i64 %x)
  %r = call { i64, i64 } @quidra.real.adopt_node(ptr %p)
  ret { i64, i64 } %r
}

define internal { i64, i64 } @quidra.real.from_u64(i64 %x) alwaysinline {
entry:
  %large = icmp slt i64 %x, 0
  br i1 %large, label %node, label %small
small:
  %s0 = insertvalue { i64, i64 } undef, i64 %x, 0
  %s = insertvalue { i64, i64 } %s0, i64 1, 1
  ret { i64, i64 } %s
node:
  %p = call ptr @quidra_bigreal_from_u64(i64 %x)
  %r = call { i64, i64 } @quidra.real.adopt_node(ptr %p)
  ret { i64, i64 } %r
}

define internal { i64, i64 } @quidra.real.from_float64(double %x) {
entry:
  %p = call ptr @quidra_bigreal_from_float64(double %x)
  %r = call { i64, i64 } @quidra.real.adopt_node(ptr %p)
  ret { i64, i64 } %r
}

define internal { i64, i64 } @quidra.real.from_bigint(i64 %w) {
entry:
  %tag = and i64 %w, 1
  %boxed = icmp ne i64 %tag, 0
  br i1 %boxed, label %node, label %small
small:
  %v = ashr i64 %w, 1
  %s0 = insertvalue { i64, i64 } undef, i64 %v, 0
  %s = insertvalue { i64, i64 } %s0, i64 1, 1
  ret { i64, i64 } %s
node:
  %a = and i64 %w, -2
  %x = inttoptr i64 %a to ptr
  %p = call ptr @quidra_bigreal_from_bigint(ptr %x)
  %r = call { i64, i64 } @quidra.real.adopt_node(ptr %p)
  ret { i64, i64 } %r
}

define internal i64 @quidra.real.to_bigint({ i64, i64 } %v, i64 %line, i64 %column) {
entry:
  %n = extractvalue { i64, i64 } %v, 0
  %d = extractvalue { i64, i64 } %v, 1
  %general = icmp eq i64 %d, 0
  br i1 %general, label %node, label %small
small:
  %integral = icmp eq i64 %d, 1
  br i1 %integral, label %integer, label %fraction
integer:
  %iw = call i64 @quidra.int.from_i64(i64 %n)
  ret i64 %iw
fraction:
  %s = call ptr @quidra_real_small_to_bigint(i64 %n, i64 %d, i64 %line, i64 %column)
  %sw = call i64 @quidra.int.adopt(ptr %s)
  ret i64 %sw
node:
  %p = inttoptr i64 %n to ptr
  %r = call ptr @quidra_bigreal_to_bigint(ptr %p, i64 %line, i64 %column)
  %rw = call i64 @quidra.int.adopt(ptr %r)
  ret i64 %rw
}

define internal i1 @quidra.real.try_bigint({ i64, i64 } %v, ptr %out) {
entry:
  %n = extractvalue { i64, i64 } %v, 0
  %d = extractvalue { i64, i64 } %v, 1
  %general = icmp eq i64 %d, 0
  br i1 %general, label %node, label %small
small:
  %integral = icmp eq i64 %d, 1
  br i1 %integral, label %integer, label %fail
integer:
  %iw = call i64 @quidra.int.from_i64(i64 %n)
  store i64 %iw, ptr %out, align 8
  ret i1 true
fail:
  store i64 0, ptr %out, align 8
  ret i1 false
node:
  %p = inttoptr i64 %n to ptr
  %r = call ptr @quidra_bigreal_try_bigint(ptr %p)
  %none = icmp eq ptr %r, null
  br i1 %none, label %fail, label %adopt
adopt:
  %rw = call i64 @quidra.int.adopt(ptr %r)
  store i64 %rw, ptr %out, align 8
  ret i1 true
}

define internal i1 @quidra.real.try_i64({ i64, i64 } %v, i32 %bits, ptr %out) {
entry:
  %n = extractvalue { i64, i64 } %v, 0
  %d = extractvalue { i64, i64 } %v, 1
  %general = icmp eq i64 %d, 0
  br i1 %general, label %node, label %small
small:
  %s = call i1 @quidra_real_small_try_i64(i64 %n, i64 %d, i32 %bits, ptr %out)
  ret i1 %s
node:
  %p = inttoptr i64 %n to ptr
  %r = call i1 @quidra_bigreal_try_i64(ptr %p, i32 %bits, ptr %out)
  ret i1 %r
}

define internal i1 @quidra.real.try_u64({ i64, i64 } %v, i32 %bits, ptr %out) {
entry:
  %n = extractvalue { i64, i64 } %v, 0
  %d = extractvalue { i64, i64 } %v, 1
  %general = icmp eq i64 %d, 0
  br i1 %general, label %node, label %small
small:
  %s = call i1 @quidra_real_small_try_u64(i64 %n, i64 %d, i32 %bits, ptr %out)
  ret i1 %s
node:
  %p = inttoptr i64 %n to ptr
  %r = call i1 @quidra_bigreal_try_u64(ptr %p, i32 %bits, ptr %out)
  ret i1 %r
}

define internal i1 @quidra.real.try_float64({ i64, i64 } %v, ptr %out) {
entry:
  %n = extractvalue { i64, i64 } %v, 0
  %d = extractvalue { i64, i64 } %v, 1
  %general = icmp eq i64 %d, 0
  br i1 %general, label %node, label %small
small:
  %s = call i1 @quidra_real_small_try_float64(i64 %n, i64 %d, ptr %out)
  ret i1 %s
node:
  %p = inttoptr i64 %n to ptr
  %r = call i1 @quidra_bigreal_try_float64(ptr %p, ptr %out)
  ret i1 %r
}

define internal i1 @quidra.real.try_float32({ i64, i64 } %v, ptr %out) {
entry:
  %n = extractvalue { i64, i64 } %v, 0
  %d = extractvalue { i64, i64 } %v, 1
  %general = icmp eq i64 %d, 0
  br i1 %general, label %node, label %small
small:
  %s = call i1 @quidra_real_small_try_float32(i64 %n, i64 %d, ptr %out)
  ret i1 %s
node:
  %p = inttoptr i64 %n to ptr
  %r = call i1 @quidra_bigreal_try_float32(ptr %p, ptr %out)
  ret i1 %r
}

define internal double @quidra.real.to_float64({ i64, i64 } %v, i64 %line, i64 %column) {
entry:
  %n = extractvalue { i64, i64 } %v, 0
  %d = extractvalue { i64, i64 } %v, 1
  %general = icmp eq i64 %d, 0
  br i1 %general, label %node, label %small
small:
  %s = call double @quidra_real_small_to_float64(i64 %n, i64 %d, i64 %line, i64 %column)
  ret double %s
node:
  %p = inttoptr i64 %n to ptr
  %r = call double @quidra_bigreal_to_float64(ptr %p, i64 %line, i64 %column)
  ret double %r
}

define internal float @quidra.real.to_float32({ i64, i64 } %v, i64 %line, i64 %column) {
entry:
  %n = extractvalue { i64, i64 } %v, 0
  %d = extractvalue { i64, i64 } %v, 1
  %general = icmp eq i64 %d, 0
  br i1 %general, label %node, label %small
small:
  %s = call float @quidra_real_small_to_float32(i64 %n, i64 %d, i64 %line, i64 %column)
  ret float %s
node:
  %p = inttoptr i64 %n to ptr
  %r = call float @quidra_bigreal_to_float32(ptr %p, i64 %line, i64 %column)
  ret float %r
}

define internal i1 @quidra.real.cast_fits({ i64, i64 } %v, i32 %target_kind, i32 %bits) {
entry:
  %n = extractvalue { i64, i64 } %v, 0
  %d = extractvalue { i64, i64 } %v, 1
  %general = icmp eq i64 %d, 0
  br i1 %general, label %node, label %small
small:
  %s = call i1 @quidra_real_small_cast_fits(i64 %n, i64 %d, i32 %target_kind, i32 %bits)
  ret i1 %s
node:
  %p = inttoptr i64 %n to ptr
  %r = call i1 @quidra_exact_numeric_cast_fits(ptr %p, i32 2, i32 %target_kind, i32 %bits)
  ret i1 %r
}
)LLVM";

std::string support_text() {
    std::string out = "\n";
    for (const auto* callee : declarations) llvm_text::append_declaration(out, *callee);
    out += helpers;
    return out;
}

} // namespace

std::string_view exact_real_support_text() {
    static const std::string text = support_text();
    return text;
}

std::span<const llvm_text::LlvmCallee* const> exact_real_support_declarations() {
    return declarations;
}

bool small_rational_literal(std::string_view spelling, SmallRational& out) {
    // digits [ "." digits ] [ ("e" | "E") [ "+" | "-" ] digits ], read as
    // quidra_bigreal_literal reads it; any other text takes the runtime path.
    std::size_t i = 0;
    unsigned long long mantissa = 0;
    int scale = 0;  // the value is mantissa * 10^-scale
    bool any_digit = false;
    constexpr auto limit = std::numeric_limits<unsigned long long>::max() / 10;
    auto digit = [&](char c) {
        if (mantissa > limit) return false;
        const auto next = mantissa * 10 + static_cast<unsigned>(c - '0');
        if (next < mantissa * 10) return false;
        mantissa = next;
        return true;
    };
    for (; i < spelling.size() && spelling[i] >= '0' && spelling[i] <= '9'; ++i) {
        any_digit = true;
        if (!digit(spelling[i])) return false;
    }
    if (i < spelling.size() && spelling[i] == '.') {
        ++i;
        for (; i < spelling.size() && spelling[i] >= '0' && spelling[i] <= '9'; ++i) {
            any_digit = true;
            if (!digit(spelling[i])) return false;
            if (++scale > 18) return false;
        }
    }
    if (!any_digit) return false;
    if (i < spelling.size() && (spelling[i] == 'e' || spelling[i] == 'E')) {
        ++i;
        bool negative = false;
        if (i < spelling.size() && (spelling[i] == '+' || spelling[i] == '-')) {
            negative = spelling[i] == '-';
            ++i;
        }
        if (i == spelling.size()) return false;
        int exponent = 0;
        for (; i < spelling.size() && spelling[i] >= '0' && spelling[i] <= '9'; ++i) {
            exponent = exponent * 10 + (spelling[i] - '0');
            if (exponent > 40) return false;
        }
        scale += negative ? exponent : -exponent;
    }
    if (i != spelling.size()) return false;
    // A positive power of ten multiplies the numerator; a negative one is
    // the denominator.
    unsigned long long denominator = 1;
    while (scale < 0) {
        if (mantissa > limit) return false;
        mantissa *= 10;
        ++scale;
    }
    while (scale > 0) {
        if (denominator > limit) return false;
        denominator *= 10;
        --scale;
    }
    auto a = mantissa, b = denominator;
    while (b) {
        const auto t = a % b;
        a = b;
        b = t;
    }
    const auto g = mantissa == 0 ? denominator : a;
    mantissa /= g;
    denominator /= g;
    constexpr auto max = static_cast<unsigned long long>(std::numeric_limits<long long>::max());
    if (mantissa > max || denominator > max) return false;
    out.numerator = static_cast<long long>(mantissa);
    out.denominator = static_cast<long long>(denominator);
    return true;
}

} // namespace quidra::llvm_backend
