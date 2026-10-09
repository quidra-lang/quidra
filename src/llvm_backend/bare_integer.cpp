#include "llvm_backend/bare_integer.hpp"

#include "llvm_backend/runtime_abi.hpp"
#include "quidra/abi/layout.hpp"
#include "quidra/abi/runtime_failure.hpp"
#include "quidra/abi/string_codes.hpp"

#include <array>
#include <limits>

namespace quidra::llvm_backend {

namespace {

// The helpers below spell the word layout as numbers: the boxed bit is the
// low bit, and the inline range is [-2^62, 2^62 - 1], whose words are the
// i64 values with the low bit clear.
static_assert(abi::string_build_part_kind::text == 0 &&
              abi::string_build_part_kind::signed_integer == 1);
// A word that does not fit nat fails as a conversion of a value out of range.
static_assert(abi::conversion_type_natural == 258 &&
              static_cast<int>(abi::ConversionSubject::value) == 0 &&
              static_cast<int>(abi::ConversionReason::out_of_range) == 0);
static_assert(abi::bare_integer_layout::boxed_bit == 1 &&
              abi::bare_integer_layout::inline_min == -4611686018427387904LL &&
              abi::bare_integer_layout::inline_max == 4611686018427387903LL &&
              abi::bare_integer_layout::bytes == 8);

constexpr std::array<const llvm_text::LlvmCallee*, 26> declarations{{
    &runtime_abi::bare_integer_support::int_add,
    &runtime_abi::bare_integer_support::int_sub,
    &runtime_abi::bare_integer_support::int_mul,
    &runtime_abi::bare_integer_support::int_div,
    &runtime_abi::bare_integer_support::int_rem,
    &runtime_abi::bare_integer_support::int_neg,
    &runtime_abi::bare_integer_support::int_pow,
    &runtime_abi::bare_integer_support::int_compare,
    &runtime_abi::bare_integer_support::int_text,
    &runtime_abi::bare_integer_support::int_parse,
    &runtime_abi::bare_integer_support::int_literal,
    &runtime_abi::bare_integer_support::int_from_i64,
    &runtime_abi::bare_integer_support::int_from_u64,
    &runtime_abi::bare_integer_support::int_to_i64_checked,
    &runtime_abi::bare_integer_support::int_to_u64_checked,
    &runtime_abi::bare_integer_support::int_try_i64,
    &runtime_abi::bare_integer_support::int_try_u64,
    &runtime_abi::bare_integer_support::int_to_float64,
    &runtime_abi::bare_integer_support::int_to_float32,
    &runtime_abi::bare_integer_support::int_try_float64,
    &runtime_abi::bare_integer_support::int_try_float32,
    &runtime_abi::bare_integer_support::int_cast_fits,
    &runtime_abi::bare_integer_support::int_adopt,
    &runtime_abi::bare_integer_support::nat_sub,
    &runtime_abi::bare_integer_support::task_all_int,
    &runtime_abi::bare_integer_support::int_format,
}};

constexpr std::string_view helpers = R"LLVM(
define internal void @quidra.int.retain(i64 %w) alwaysinline {
entry:
  %tag = and i64 %w, 1
  %boxed = icmp ne i64 %tag, 0
  br i1 %boxed, label %box, label %done
box:
  %a = and i64 %w, -2
  %p = inttoptr i64 %a to ptr
  call void @quidra_managed_retain(ptr %p)
  br label %done
done:
  ret void
}

define internal void @quidra.int.release(i64 %w) alwaysinline {
entry:
  %tag = and i64 %w, 1
  %boxed = icmp ne i64 %tag, 0
  br i1 %boxed, label %box, label %done
box:
  %a = and i64 %w, -2
  %p = inttoptr i64 %a to ptr
  call void @quidra_managed_release(ptr %p, ptr @quidra_bigint_drop)
  br label %done
done:
  ret void
}

define internal i64 @quidra.int.add(i64 %a, i64 %b) alwaysinline {
entry:
  %or = or i64 %a, %b
  %tag = and i64 %or, 1
  %inline = icmp eq i64 %tag, 0
  br i1 %inline, label %fast, label %slow
fast:
  %pair = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %a, i64 %b)
  %sum = extractvalue { i64, i1 } %pair, 0
  %overflow = extractvalue { i64, i1 } %pair, 1
  br i1 %overflow, label %slow, label %done
done:
  ret i64 %sum
slow:
  %r = call i64 @quidra_int_add(i64 %a, i64 %b)
  ret i64 %r
}

define internal i64 @quidra.int.sub(i64 %a, i64 %b) alwaysinline {
entry:
  %or = or i64 %a, %b
  %tag = and i64 %or, 1
  %inline = icmp eq i64 %tag, 0
  br i1 %inline, label %fast, label %slow
fast:
  %pair = call { i64, i1 } @llvm.ssub.with.overflow.i64(i64 %a, i64 %b)
  %difference = extractvalue { i64, i1 } %pair, 0
  %overflow = extractvalue { i64, i1 } %pair, 1
  br i1 %overflow, label %slow, label %done
done:
  ret i64 %difference
slow:
  %r = call i64 @quidra_int_sub(i64 %a, i64 %b)
  ret i64 %r
}

define internal i64 @quidra.int.mul(i64 %a, i64 %b) alwaysinline {
entry:
  %or = or i64 %a, %b
  %tag = and i64 %or, 1
  %inline = icmp eq i64 %tag, 0
  br i1 %inline, label %fast, label %slow
fast:
  %av = ashr i64 %a, 1
  %pair = call { i64, i1 } @llvm.smul.with.overflow.i64(i64 %av, i64 %b)
  %product = extractvalue { i64, i1 } %pair, 0
  %overflow = extractvalue { i64, i1 } %pair, 1
  br i1 %overflow, label %slow, label %done
done:
  ret i64 %product
slow:
  %r = call i64 @quidra_int_mul(i64 %a, i64 %b)
  ret i64 %r
}

define internal i64 @quidra.int.div(i64 %a, i64 %b, i64 %line, i64 %column) alwaysinline {
entry:
  %zero = icmp eq i64 %b, 0
  br i1 %zero, label %fail, label %nonzero
fail:
  call void @quidra_fail_at(ptr @.code.divzero, ptr @.msg.divzero, i64 %line, i64 %column)
  unreachable
nonzero:
  %or = or i64 %a, %b
  %tag = and i64 %or, 1
  %inline = icmp eq i64 %tag, 0
  br i1 %inline, label %fast, label %slow
fast:
  %q = sdiv i64 %a, %b
  %over = icmp eq i64 %q, 4611686018427387904
  br i1 %over, label %slow, label %done
done:
  %w = shl i64 %q, 1
  ret i64 %w
slow:
  %r = call i64 @quidra_int_div(i64 %a, i64 %b, i64 %line, i64 %column)
  ret i64 %r
}

define internal i64 @quidra.int.rem(i64 %a, i64 %b, i64 %line, i64 %column) alwaysinline {
entry:
  %zero = icmp eq i64 %b, 0
  br i1 %zero, label %fail, label %nonzero
fail:
  call void @quidra_fail_at(ptr @.code.divzero, ptr @.msg.divzero, i64 %line, i64 %column)
  unreachable
nonzero:
  %or = or i64 %a, %b
  %tag = and i64 %or, 1
  %inline = icmp eq i64 %tag, 0
  br i1 %inline, label %fast, label %slow
fast:
  %w = srem i64 %a, %b
  ret i64 %w
slow:
  %r = call i64 @quidra_int_rem(i64 %a, i64 %b, i64 %line, i64 %column)
  ret i64 %r
}

define internal i64 @quidra.int.neg(i64 %a, i64 %line, i64 %column) alwaysinline {
entry:
  %tag = and i64 %a, 1
  %inline = icmp eq i64 %tag, 0
  br i1 %inline, label %fast, label %slow
fast:
  %pair = call { i64, i1 } @llvm.ssub.with.overflow.i64(i64 0, i64 %a)
  %negated = extractvalue { i64, i1 } %pair, 0
  %overflow = extractvalue { i64, i1 } %pair, 1
  br i1 %overflow, label %slow, label %done
done:
  ret i64 %negated
slow:
  %r = call i64 @quidra_int_neg(i64 %a, i64 %line, i64 %column)
  ret i64 %r
}

define internal i64 @quidra.int.pow(i64 %a, i64 %b, i64 %line, i64 %column) {
entry:
  %r = call i64 @quidra_int_pow(i64 %a, i64 %b, i64 %line, i64 %column)
  ret i64 %r
}

define internal i32 @quidra.int.compare(i64 %a, i64 %b) alwaysinline {
entry:
  %or = or i64 %a, %b
  %tag = and i64 %or, 1
  %inline = icmp eq i64 %tag, 0
  br i1 %inline, label %fast, label %slow
fast:
  %greater = icmp sgt i64 %a, %b
  %less = icmp slt i64 %a, %b
  %g = zext i1 %greater to i32
  %l = zext i1 %less to i32
  %c = sub i32 %g, %l
  ret i32 %c
slow:
  %r = call i32 @quidra_int_compare(i64 %a, i64 %b)
  ret i32 %r
}

define internal i1 @quidra.int.eq(i64 %a, i64 %b) alwaysinline {
entry:
  %both = and i64 %a, %b
  %tag = and i64 %both, 1
  %decided = icmp eq i64 %tag, 0
  br i1 %decided, label %word, label %slow
word:
  %r = icmp eq i64 %a, %b
  ret i1 %r
slow:
  %c = call i32 @quidra_int_compare(i64 %a, i64 %b)
  %s = icmp eq i32 %c, 0
  ret i1 %s
}

define internal i1 @quidra.int.ne(i64 %a, i64 %b) alwaysinline {
entry:
  %both = and i64 %a, %b
  %tag = and i64 %both, 1
  %decided = icmp eq i64 %tag, 0
  br i1 %decided, label %word, label %slow
word:
  %r = icmp ne i64 %a, %b
  ret i1 %r
slow:
  %c = call i32 @quidra_int_compare(i64 %a, i64 %b)
  %s = icmp ne i32 %c, 0
  ret i1 %s
}
@ORDERING@
define internal i64 @quidra.nat.sub(i64 %a, i64 %b, i64 %line, i64 %column) alwaysinline {
entry:
  %or = or i64 %a, %b
  %tag = and i64 %or, 1
  %inline = icmp eq i64 %tag, 0
  br i1 %inline, label %fast, label %slow
fast:
  %difference = sub i64 %a, %b
  %negative = icmp slt i64 %difference, 0
  br i1 %negative, label %slow, label %done
done:
  ret i64 %difference
slow:
  %r = call i64 @quidra_nat_sub(i64 %a, i64 %b, i64 %line, i64 %column)
  ret i64 %r
}

define internal i64 @quidra.nat.from_int(i64 %w, i64 %line, i64 %column) alwaysinline {
entry:
  %tag = and i64 %w, 1
  %boxed = icmp ne i64 %tag, 0
  br i1 %boxed, label %box, label %small
small:
  %negative = icmp slt i64 %w, 0
  br i1 %negative, label %fail, label %done
box:
  %c = call i32 @quidra_int_compare(i64 %w, i64 0)
  %box.negative = icmp slt i32 %c, 0
  br i1 %box.negative, label %fail, label %shared
shared:
  call void @quidra.int.retain(i64 %w)
  br label %done
done:
  ret i64 %w
fail:
  call void @quidra_runtime_conversion_fail(i32 258, i32 0, i32 0, i64 %line, i64 %column)
  unreachable
}

define internal i64 @quidra.nat.from_i64(i64 %v, i64 %line, i64 %column) alwaysinline {
entry:
  %negative = icmp slt i64 %v, 0
  br i1 %negative, label %fail, label %value
value:
  %w = call i64 @quidra.int.from_i64(i64 %v)
  ret i64 %w
fail:
  call void @quidra_runtime_conversion_fail(i32 258, i32 0, i32 0, i64 %line, i64 %column)
  unreachable
}

define internal ptr @quidra.int.text(i64 %w) {
entry:
  %t = call ptr @quidra_int_text(i64 %w)
  ret ptr %t
}

define internal void @quidra.int.print(i64 %w) {
entry:
  %tag = and i64 %w, 1
  %boxed = icmp ne i64 %tag, 0
  br i1 %boxed, label %box, label %small
small:
  %v = ashr i64 %w, 1
  %printed = call i32 (ptr, ...) @printf(ptr @.fmt.int.raw, i64 %v)
  ret void
box:
  %t = call ptr @quidra_int_text(i64 %w)
  %printed.text = call i32 (ptr, ...) @printf(ptr @.fmt.string.raw, ptr %t)
  call void @quidra_managed_release(ptr %t, ptr null)
  ret void
}

define internal ptr @quidra.int.build_part(i64 %w, ptr %kind, ptr %value) {
entry:
  %tag = and i64 %w, 1
  %boxed = icmp ne i64 %tag, 0
  br i1 %boxed, label %box, label %small
small:
  %v = ashr i64 %w, 1
  store i8 1, ptr %kind, align 1
  store i64 %v, ptr %value, align 8
  ret ptr null
box:
  %t = call ptr @quidra_int_text(i64 %w)
  %bits = ptrtoint ptr %t to i64
  store i8 0, ptr %kind, align 1
  store i64 %bits, ptr %value, align 8
  ret ptr %t
}

define internal ptr @quidra.int.format(i64 %w, i32 %width, i32 %fractional, i32 %significant, i32 %zero) {
entry:
  %t = call ptr @quidra_int_format(i64 %w, i32 %width, i32 %fractional, i32 %significant, i32 %zero)
  ret ptr %t
}

define internal i1 @quidra.int.parse(ptr %text, ptr %out) {
entry:
  %ok = call i1 @quidra_int_parse(ptr %text, ptr %out)
  ret i1 %ok
}

define internal i64 @quidra.int.literal(ptr %text, ptr %cache) {
entry:
  %cached = load atomic i64, ptr %cache monotonic, align 8
  %ready = icmp ne i64 %cached, 0
  br i1 %ready, label %done, label %make
done:
  ret i64 %cached
make:
  %w = call i64 @quidra_int_literal(ptr %text)
  store atomic i64 %w, ptr %cache monotonic, align 8
  ret i64 %w
}

define internal i64 @quidra.int.adopt(ptr %big) {
entry:
  %w = call i64 @quidra_int_adopt(ptr %big)
  ret i64 %w
}

define internal i64 @quidra.int.from_i64(i64 %v) alwaysinline {
entry:
  %pair = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %v, i64 %v)
  %w = extractvalue { i64, i1 } %pair, 0
  %overflow = extractvalue { i64, i1 } %pair, 1
  br i1 %overflow, label %slow, label %done
done:
  ret i64 %w
slow:
  %r = call i64 @quidra_int_from_i64(i64 %v)
  ret i64 %r
}

define internal i64 @quidra.int.from_u64(i64 %v) alwaysinline {
entry:
  %small = icmp ult i64 %v, 4611686018427387904
  br i1 %small, label %fast, label %slow
fast:
  %w = shl i64 %v, 1
  ret i64 %w
slow:
  %r = call i64 @quidra_int_from_u64(i64 %v)
  ret i64 %r
}

define internal i64 @quidra.int.to_i64(i64 %w, i32 %bits, i64 %line, i64 %column) alwaysinline {
entry:
  %slot = alloca i64, align 8
  %tag = and i64 %w, 1
  %inline = icmp eq i64 %tag, 0
  br i1 %inline, label %small, label %slow
small:
  %v = ashr i64 %w, 1
  %b = zext i32 %bits to i64
  %shift = sub i64 64, %b
  %up = shl i64 %v, %shift
  %back = ashr i64 %up, %shift
  %fits = icmp eq i64 %back, %v
  br i1 %fits, label %done, label %fail
done:
  ret i64 %v
slow:
  %ok = call i1 @quidra_int_try_i64(i64 %w, i32 %bits, ptr %slot)
  br i1 %ok, label %boxed, label %fail
boxed:
  %r = load i64, ptr %slot, align 8
  ret i64 %r
fail:
  %never.i = call i64 @quidra_int_to_i64_checked(i64 %w, i32 %bits, i64 %line, i64 %column)
  unreachable
}

define internal i64 @quidra.int.to_u64(i64 %w, i32 %bits, i64 %line, i64 %column) alwaysinline {
entry:
  %slot = alloca i64, align 8
  %tag = and i64 %w, 1
  %inline = icmp eq i64 %tag, 0
  br i1 %inline, label %small, label %slow
small:
  %v = ashr i64 %w, 1
  %negative = icmp slt i64 %v, 0
  br i1 %negative, label %fail, label %range
range:
  %b = zext i32 %bits to i64
  %shift = sub i64 64, %b
  %up = shl i64 %v, %shift
  %back = lshr i64 %up, %shift
  %fits = icmp eq i64 %back, %v
  br i1 %fits, label %done, label %fail
done:
  ret i64 %v
slow:
  %ok = call i1 @quidra_int_try_u64(i64 %w, i32 %bits, ptr %slot)
  br i1 %ok, label %boxed, label %fail
boxed:
  %r = load i64, ptr %slot, align 8
  ret i64 %r
fail:
  %never.u = call i64 @quidra_int_to_u64_checked(i64 %w, i32 %bits, i64 %line, i64 %column)
  unreachable
}

define internal i1 @quidra.int.try_i64(i64 %w, i32 %bits, ptr %out) alwaysinline {
entry:
  %tag = and i64 %w, 1
  %inline = icmp eq i64 %tag, 0
  br i1 %inline, label %small, label %slow
small:
  %v = ashr i64 %w, 1
  %b = zext i32 %bits to i64
  %shift = sub i64 64, %b
  %up = shl i64 %v, %shift
  %back = ashr i64 %up, %shift
  %fits = icmp eq i64 %back, %v
  %stored = select i1 %fits, i64 %v, i64 0
  store i64 %stored, ptr %out, align 8
  ret i1 %fits
slow:
  %r = call i1 @quidra_int_try_i64(i64 %w, i32 %bits, ptr %out)
  ret i1 %r
}

define internal i1 @quidra.int.try_u64(i64 %w, i32 %bits, ptr %out) alwaysinline {
entry:
  %tag = and i64 %w, 1
  %inline = icmp eq i64 %tag, 0
  br i1 %inline, label %small, label %slow
small:
  %v = ashr i64 %w, 1
  %nonnegative = icmp sge i64 %v, 0
  %b = zext i32 %bits to i64
  %shift = sub i64 64, %b
  %up = shl i64 %v, %shift
  %back = lshr i64 %up, %shift
  %in.range = icmp eq i64 %back, %v
  %fits = and i1 %nonnegative, %in.range
  %stored = select i1 %fits, i64 %v, i64 0
  store i64 %stored, ptr %out, align 8
  ret i1 %fits
slow:
  %r = call i1 @quidra_int_try_u64(i64 %w, i32 %bits, ptr %out)
  ret i1 %r
}

define internal double @quidra.int.to_float64(i64 %w, i64 %line, i64 %column) alwaysinline {
entry:
  %slot = alloca double, align 8
  %tag = and i64 %w, 1
  %inline = icmp eq i64 %tag, 0
  br i1 %inline, label %small, label %slow
small:
  %v = ashr i64 %w, 1
  %f = sitofp i64 %v to double
  ret double %f
slow:
  %ok = call i1 @quidra_int_try_float64(i64 %w, ptr %slot)
  br i1 %ok, label %boxed, label %fail
boxed:
  %r = load double, ptr %slot, align 8
  ret double %r
fail:
  %never.d = call double @quidra_int_to_float64(i64 %w, i64 %line, i64 %column)
  unreachable
}

define internal float @quidra.int.to_float32(i64 %w, i64 %line, i64 %column) alwaysinline {
entry:
  %slot = alloca float, align 4
  %tag = and i64 %w, 1
  %inline = icmp eq i64 %tag, 0
  br i1 %inline, label %small, label %slow
small:
  %v = ashr i64 %w, 1
  %d = sitofp i64 %v to double
  %f = fptrunc double %d to float
  ret float %f
slow:
  %ok = call i1 @quidra_int_try_float32(i64 %w, ptr %slot)
  br i1 %ok, label %boxed, label %fail
boxed:
  %r = load float, ptr %slot, align 4
  ret float %r
fail:
  %never.f = call float @quidra_int_to_float32(i64 %w, i64 %line, i64 %column)
  unreachable
}

define internal i1 @quidra.int.try_float64(i64 %w, ptr %out) alwaysinline {
entry:
  %tag = and i64 %w, 1
  %inline = icmp eq i64 %tag, 0
  br i1 %inline, label %small, label %slow
small:
  %v = ashr i64 %w, 1
  %f = sitofp i64 %v to double
  store double %f, ptr %out, align 8
  ret i1 true
slow:
  %r = call i1 @quidra_int_try_float64(i64 %w, ptr %out)
  ret i1 %r
}

define internal i1 @quidra.int.try_float32(i64 %w, ptr %out) alwaysinline {
entry:
  %tag = and i64 %w, 1
  %inline = icmp eq i64 %tag, 0
  br i1 %inline, label %small, label %slow
small:
  %v = ashr i64 %w, 1
  %d = sitofp i64 %v to double
  %f = fptrunc double %d to float
  store float %f, ptr %out, align 4
  ret i1 true
slow:
  %r = call i1 @quidra_int_try_float32(i64 %w, ptr %out)
  ret i1 %r
}

define internal void @quidra.int.task_all(ptr %operations, ptr %output, i64 %line, i64 %column) {
entry:
  call void @quidra_task_all_int(ptr %operations, ptr %output, i64 %line, i64 %column)
  ret void
}

define internal i1 @quidra.int.cast_fits(i64 %w, i32 %target_kind, i32 %bits) {
entry:
  %r = call i1 @quidra_int_cast_fits(i64 %w, i32 %target_kind, i32 %bits)
  ret i1 %r
}
)LLVM";

// The ordering helper @quidra.int.<name>: icmp <predicate> on two inline
// words, the runtime comparison otherwise.
std::string ordering_helper(std::string_view name, std::string_view predicate) {
    std::string out = "\ndefine internal i1 @quidra.int.";
    out += name;
    out += R"LLVM((i64 %a, i64 %b) alwaysinline {
entry:
  %or = or i64 %a, %b
  %tag = and i64 %or, 1
  %inline = icmp eq i64 %tag, 0
  br i1 %inline, label %word, label %slow
word:
  %r = icmp )LLVM";
    out += predicate;
    out += R"LLVM( i64 %a, %b
  ret i1 %r
slow:
  %c = call i32 @quidra_int_compare(i64 %a, i64 %b)
  %s = icmp )LLVM";
    out += predicate;
    out += R"LLVM( i32 %c, 0
  ret i1 %s
}
)LLVM";
    return out;
}

std::string support_text() {
    std::string out = "\n";
    for (const auto* callee : declarations) llvm_text::append_declaration(out, *callee);
    std::string body(helpers);
    const std::string ordering = ordering_helper("lt", "slt") + ordering_helper("le", "sle") +
                                 ordering_helper("gt", "sgt") + ordering_helper("ge", "sge");
    const auto marker = body.find("@ORDERING@\n");
    body.replace(marker, std::string_view("@ORDERING@\n").size(), ordering + "\n");
    out += body;
    return out;
}

} // namespace

std::string_view bare_integer_support_text() {
    static const std::string text = support_text();
    return text;
}

std::span<const llvm_text::LlvmCallee* const> bare_integer_support_declarations() {
    return declarations;
}

bool bare_integer_inline_literal(std::string_view spelling, long long& word) {
    std::size_t i = 0;
    bool negative = false;
    if (i < spelling.size() && (spelling[i] == '+' || spelling[i] == '-')) {
        negative = spelling[i] == '-';
        ++i;
    }
    if (i == spelling.size()) return false;
    constexpr auto limit = static_cast<unsigned long long>(abi::bare_integer_layout::inline_max) + 1ULL;
    unsigned long long magnitude = 0;
    for (; i < spelling.size(); ++i) {
        const char c = spelling[i];
        if (c < '0' || c > '9') return false;
        const auto digit = static_cast<unsigned long long>(c - '0');
        if (magnitude > (limit - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
    }
    if (!negative && magnitude == limit) return false;
    const long long value = negative ? -static_cast<long long>(magnitude) : static_cast<long long>(magnitude);
    word = static_cast<long long>(static_cast<unsigned long long>(value) << 1);
    return true;
}

} // namespace quidra::llvm_backend
