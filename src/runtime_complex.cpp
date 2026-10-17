// runtime_complex.cpp: complex values. The fixed-width complex types
// (com16, com16b, com32, com64) cross into the runtime as their two parts
// widened exactly to binary64, and a result is written as two binary64
// values, each part already rounded once to the part type. An exact com
// value is a managed box of two `real` values (abi::complex_box_layout),
// which holds its own reference to each.

#include "runtime_report.hpp"
#include "quidra/abi/exact_codes.hpp"
#include "quidra/abi/half_format.hpp"
#include "quidra/abi/layout.hpp"
#include "quidra/abi/runtime_entry_points.hpp"
#include "quidra/abi/runtime_failure.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace abi = quidra::abi;

namespace {

using quidra::abi::ExactRealValue;

// The part dtype of a complex dtype code.
int part_dtype(int kind) { return abi::complex_part_dtype_code(kind); }

// `value` rounded once to the real dtype `part` (binary64, binary32 or a
// 16-bit format), as binary64; an infinity beyond the part's range.
double round_part(double value, int part) {
    switch (part) {
        case QCORE_DTYPE_FLOAT32: return static_cast<double>(static_cast<float>(value));
        case QCORE_DTYPE_FLOAT16:
            return abi::half::value_of(abi::half::round_to(value, abi::half::binary16),
                                       abi::half::binary16);
        case QCORE_DTYPE_BFLOAT16:
            return abi::half::value_of(abi::half::round_to(value, abi::half::bfloat16),
                                       abi::half::bfloat16);
        default: return value;
    }
}

bool decline(abi::ConversionReason reason) {
    quidra::runtime::record_conversion_reason(reason);
    return false;
}

// Each part rounded once to the parts of the complex dtype `kind`, into
// out[0] and out[1]; declined (OUT_OF_RANGE) when a finite part rounds
// beyond them.
bool rounded(double re, double im, int kind, double* out) {
    const auto part = part_dtype(kind);
    out[0] = round_part(re, part);
    out[1] = round_part(im, part);
    if (std::isnan(re) || std::isnan(im) ||
        (std::isfinite(re) && !std::isfinite(out[0])) ||
        (std::isfinite(im) && !std::isfinite(out[1])))
        return decline(abi::ConversionReason::out_of_range);
    return true;
}

// An integer rounded once to binary64 for a later rounding to at most 24
// bits: beyond 2^53 it keeps its 53 leading bits with the dropped ones ORed
// into the last kept one, which rounds to 24 or fewer bits exactly as the
// integer itself does.
double integer_for_rounding(unsigned long long magnitude) {
    int bits = 0;
    for (auto rest = magnitude; rest != 0; rest >>= 1) ++bits;
    if (bits <= 53) return static_cast<double>(magnitude);
    const int shift = bits - 53;
    const auto dropped = magnitude & ((1ULL << shift) - 1ULL);
    const auto kept = (magnitude >> shift) | (dropped != 0 ? 1ULL : 0ULL);
    return std::ldexp(static_cast<double>(kept), shift);
}

// --- Exact parts -----------------------------------------------------------

bool small(const ExactRealValue& value) { return value.denominator != 0; }

void* node_of(const ExactRealValue& value) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(value.numerator));
}

void retain_part(const ExactRealValue& value) {
    if (!small(value) && value.numerator != 0) quidra_managed_retain(node_of(value));
}

void release_part(const ExactRealValue& value) {
    if (!small(value) && value.numerator != 0)
        quidra_managed_release(node_of(value), reinterpret_cast<void*>(&quidra_bigreal_drop));
}

// The general form of a value (a node the caller releases).
void* promote(const ExactRealValue& value) {
    return quidra_real_promote(value.numerator, value.denominator);
}

ExactRealValue adopt(void* node) {
    ExactRealValue out{};
    quidra_real_demote(node, &out);
    return out;
}

ExactRealValue zero_part() { return ExactRealValue{0, 1}; }

ExactRealValue exact_binary(const ExactRealValue& left, const ExactRealValue& right, int operation,
                            unsigned long long line, unsigned long long column) {
    if (small(left) && small(right)) {
        ExactRealValue out{};
        if (quidra_real_small_binary(left.numerator, left.denominator, right.numerator,
                                     right.denominator, operation, &out))
            return out;
    }
    void* a = promote(left);
    void* b = promote(right);
    void* result = quidra_bigreal_binary(a, b, operation, line, column);
    quidra_managed_release(a, reinterpret_cast<void*>(&quidra_bigreal_drop));
    quidra_managed_release(b, reinterpret_cast<void*>(&quidra_bigreal_drop));
    return adopt(result);
}

int exact_compare(const ExactRealValue& left, const ExactRealValue& right) {
#if defined(__SIZEOF_INT128__)
    if (small(left) && small(right)) {
        __extension__ using WideInteger = __int128;
        const WideInteger a = static_cast<WideInteger>(left.numerator) * right.denominator;
        const WideInteger b = static_cast<WideInteger>(right.numerator) * left.denominator;
        return a < b ? -1 : a > b ? 1 : 0;
    }
#endif
    // MSVC lacks native 128-bit integers: compare via exact big reals instead
    // of truncating cross-products or relying on floating approximation.
    void* a = promote(left);
    void* b = promote(right);
    const int order = quidra_bigreal_compare(a, b, 0, 0);
    quidra_managed_release(a, reinterpret_cast<void*>(&quidra_bigreal_drop));
    quidra_managed_release(b, reinterpret_cast<void*>(&quidra_bigreal_drop));
    return order;
}

bool exact_is_zero(const ExactRealValue& value) {
    return small(value) && value.numerator == 0;
}

ExactRealValue exact_neg(const ExactRealValue& value, unsigned long long line,
                         unsigned long long column) {
    if (small(value)) return ExactRealValue{-value.numerator, value.denominator};
    return adopt(quidra_bigreal_neg(node_of(value), line, column));
}

// The binary64 value of an exact part, if it has one (declines record why).
bool exact_try_float64(const ExactRealValue& value, double& out) {
    if (small(value)) return quidra_real_small_try_float64(value.numerator, value.denominator, &out);
    return quidra_bigreal_try_float64(node_of(value), &out);
}

bool exact_try_float32(const ExactRealValue& value, float& out) {
    if (small(value)) return quidra_real_small_try_float32(value.numerator, value.denominator, &out);
    return quidra_bigreal_try_float32(node_of(value), &out);
}

// An exact part rounded to the real dtype `part`, as binary64; nothing when
// the runtime declines it (the reason recorded) or it is beyond the range.
bool exact_to_part(const ExactRealValue& value, int part, double& out) {
    if (part == QCORE_DTYPE_FLOAT32) {
        float single = 0.0F;
        if (!exact_try_float32(value, single)) return false;
        out = single;
        return true;
    }
    if(part==QCORE_DTYPE_FLOAT16||part==QCORE_DTYPE_BFLOAT16)
        return quidra_half_try_real(value.numerator,value.denominator,
                                    part==QCORE_DTYPE_BFLOAT16?1:0,&out);
    double wide = 0.0;
    if (!exact_try_float64(value, wide)) return false;
    out = round_part(wide, part);
    if (std::isnan(wide) || (std::isfinite(wide) && !std::isfinite(out))) {
        quidra::runtime::record_conversion_reason(abi::ConversionReason::out_of_range);
        return false;
    }
    return true;
}

ExactRealValue exact_from_float64(double value) { return adopt(quidra_bigreal_from_float64(value)); }

struct ComBox {
    ExactRealValue re;
    ExactRealValue im;
};
static_assert(sizeof(ComBox) == abi::complex_box_layout::bytes &&
                  offsetof(ComBox, im) == abi::complex_box_layout::imaginary_offset,
              "a com box is two exact reals");

ComBox* box_of(void* value) { return static_cast<ComBox*>(value); }

// A new box that takes over (does not retain) both parts.
void* adopt_box(ExactRealValue re, ExactRealValue im) {
    auto* box = static_cast<ComBox*>(quidra_managed_alloc(sizeof(ComBox)));
    box->re = re;
    box->im = im;
    return box;
}

// --- Text ------------------------------------------------------------------

std::string take_text(char* text) {
    std::string out = text ? text : "";
    if (text) quidra_managed_release(text, nullptr);
    return out;
}

std::string part_text(double value, int part, int integer_width, int fractional, int significant,
                      int zero) {
    if (part == QCORE_DTYPE_FLOAT16 || part == QCORE_DTYPE_BFLOAT16) {
        const auto& format =
            part == QCORE_DTYPE_FLOAT16 ? abi::half::binary16 : abi::half::bfloat16;
        return take_text(quidra_half_format(abi::half::round_to(value, format),
                                            part == QCORE_DTYPE_FLOAT16 ? 0 : 1, integer_width,
                                            fractional, significant, zero));
    }
    if (part == QCORE_DTYPE_FLOAT32)
        return take_text(quidra_format_number32(static_cast<float>(value), integer_width, fractional, significant, zero));
    if (integer_width < 0 && fractional < 0 && significant < 0 && zero == 0)
        return take_text(quidra_format_float(value));
    return take_text(quidra_format_number(value, integer_width, fractional, significant, zero));
}

std::string exact_text(const ExactRealValue& value, int significant) {
    if (small(value))
        return take_text(quidra_real_small_text(value.numerator, value.denominator, significant));
    return take_text(quidra_bigreal_text(node_of(value), significant));
}

// re+imi or re-|im|i: the imaginary part's sign joins the parts.
char* join_parts(std::string re, std::string im) {
    std::string text = std::move(re);
    if (!im.empty() && im.front() == '-') {
        text += im;
    } else {
        text += '+';
        text += im;
    }
    text += 'i';
    return quidra_runtime_copy_text(text.data(), text.size());
}

// The position of the sign that starts the imaginary part of `text`: the
// last + or - that is neither the first character nor after an exponent
// mark; npos when there is none.
std::size_t imaginary_sign(const std::string& text) {
    for (std::size_t i = text.size(); i-- > 1;) {
        if ((text[i] == '+' || text[i] == '-') && text[i - 1] != 'e' && text[i - 1] != 'E')
            return i;
    }
    return std::string::npos;
}

bool split_complex_text(const char* text, std::string& re, std::string& im) {
    if (!text) return false;
    const std::string all(text);
    if (all.size() < 4 || all.back() != 'i') return false;
    const auto body = all.substr(0, all.size() - 1);
    const auto sign = imaginary_sign(body);
    if (sign == std::string::npos) return false;
    re = body.substr(0, sign);
    im = body.substr(sign);
    return !re.empty() && im.size() > 1;
}

bool parse_part(const std::string& text, int part, double& out) {
    if (part == QCORE_DTYPE_FLOAT16 || part == QCORE_DTYPE_BFLOAT16) {
        unsigned short bits = 0;
        if (!quidra_half_parse(text.c_str(), part == QCORE_DTYPE_FLOAT16 ? 0 : 1, &bits))
            return false;
        out = abi::half::value_of(bits, part == QCORE_DTYPE_FLOAT16 ? abi::half::binary16
                                                                    : abi::half::bfloat16);
        return true;
    }
    if (part == QCORE_DTYPE_FLOAT32) {
        float single = 0.0F;
        if (!quidra_parse_float32(text.c_str(), &single)) return false;
        out = single;
        return true;
    }
    return quidra_parse_float64(text.c_str(), &out);
}

[[noreturn]] void complex_overflow(int kind, abi::RealOperation operation, unsigned long long line,
                                   unsigned long long column) {
    quidra_runtime_real_fail(kind, static_cast<int>(operation), false, line, column);
    std::abort();
}

} // namespace

// --- Fixed-width complex values ----------------------------------------------
//
// A result is written to out[0] (the real part) and out[1] (the imaginary
// part), each a value of the part type as binary64; a conversion or a parse
// returns false when it declines (a conversion records why).

extern "C" char* quidra_complex_text(double re, double im, int kind, int integer_width,
                                     int fractional, int significant, int zero) {
    const auto part = part_dtype(kind);
    return join_parts(part_text(re, part, integer_width, fractional, significant, zero),
                      part_text(im, part, integer_width, fractional, significant, zero));
}

// The printed form re+imi or re-imi, each part read as its part type reads
// it (rounded once); false for any other text, with the reason recorded
// (INVALID_TEXT, or a part's OUT_OF_RANGE).
extern "C" bool quidra_complex_parse(const char* text, int kind, double* out) {
    std::string re_text, im_text;
    const auto part = part_dtype(kind);
    if (!split_complex_text(text, re_text, im_text)) {
        quidra::runtime::record_failure_reason(0);
        return false;
    }
    return parse_part(re_text, part, out[0]) && parse_part(im_text, part, out[1]);
}

// z ^ w on the principal branch, exp(w * Log z), evaluated in binary64
// through the platform's libm and rounded once per part to the part type
// (deterministic per platform, not correctly rounded). 0 ^ w is 0 when the
// real part of w is positive and has no value otherwise.
extern "C" void quidra_complex_pow(double re, double im, double exponent_re, double exponent_im,
                                   int kind, double* out, unsigned long long line,
                                   unsigned long long column) {
    if (re == 0.0 && im == 0.0) {
        if (exponent_re == 0.0 && exponent_im == 0.0)
            quidra::runtime::report_failure(abi::FailureReason::zero_power_zero, {}, line, column);
        if (exponent_re <= 0.0)
            quidra::runtime::report_failure(abi::FailureReason::zero_power_negative, {}, line,
                                            column);
        out[0] = 0.0;
        out[1] = 0.0;
        return;
    }
    const double log_modulus = std::log(std::hypot(re, im));
    const double argument = std::atan2(im, re);
    const double x = exponent_re * log_modulus - exponent_im * argument;
    const double y = exponent_re * argument + exponent_im * log_modulus;
    const double scale = std::exp(x);
    const auto part = part_dtype(kind);
    out[0] = round_part(scale * std::cos(y), part);
    out[1] = round_part(scale * std::sin(y), part);
    if (std::isnan(out[0]) || std::isnan(out[1]))
        complex_overflow(kind, abi::RealOperation::power, line, column);
}

// A conversion into the complex dtype `kind`: from the parts of a complex
// value or a real (imaginary part 0), widened to binary64; each part
// rounded once.
extern "C" bool quidra_complex_from_parts(double re, double im, int kind, double* out) {
    return rounded(re, im, kind, out);
}

// From a fixed-width integer (`is_signed`: its bits are a long long).
extern "C" bool quidra_complex_from_integer(long long value, bool is_signed, int kind,
                                            double* out) {
    const auto part = part_dtype(kind);
    double re = 0.0;
    if (part == QCORE_DTYPE_FLOAT64) {
        re = is_signed ? static_cast<double>(value)
                       : static_cast<double>(static_cast<unsigned long long>(value));
    } else if (part == QCORE_DTYPE_FLOAT32) {
        re = is_signed ? static_cast<double>(static_cast<float>(value))
                       : static_cast<double>(static_cast<float>(static_cast<unsigned long long>(value)));
    } else {
        const auto magnitude = is_signed && value < 0 ? 0ULL - static_cast<unsigned long long>(value)
                                                      : static_cast<unsigned long long>(value);
        const auto wide = integer_for_rounding(magnitude);
        re = is_signed && value < 0 ? -wide : wide;
    }
    return rounded(re, 0.0, kind, out);
}

// From an exact real (its two words).
extern "C" bool quidra_complex_from_real(long long numerator, long long denominator, int kind,
                                         double* out) {
    out[1] = 0.0;
    return exact_to_part(ExactRealValue{numerator, denominator}, part_dtype(kind), out[0]);
}

// From an exact com value.
extern "C" bool quidra_complex_from_com(void* value, int kind, double* out) {
    const auto* box = box_of(value);
    return exact_to_part(box->re, part_dtype(kind), out[0]) &&
           exact_to_part(box->im, part_dtype(kind), out[1]);
}

// --- Exact com values -------------------------------------------------------

extern "C" void quidra_com_drop(void* value) {
    auto* box = box_of(value);
    release_part(box->re);
    release_part(box->im);
}

// A com value of the two parts, which the box retains.
extern "C" void* quidra_com_make(long long re_numerator, long long re_denominator,
                                 long long im_numerator, long long im_denominator) {
    const ExactRealValue re{re_numerator, re_denominator};
    const ExactRealValue im{im_numerator, im_denominator};
    retain_part(re);
    retain_part(im);
    return adopt_box(re, im);
}

// From binary64 parts (a fixed-width real, integer or complex value).
extern "C" void* quidra_com_from_float64(double re, double im) {
    return adopt_box(exact_from_float64(re), exact_from_float64(im));
}

extern "C" void* quidra_com_from_integer(long long value, bool is_signed) {
    void* node = is_signed ? quidra_bigreal_from_i64(value)
                           : quidra_bigreal_from_u64(static_cast<unsigned long long>(value));
    return adopt_box(adopt(node), zero_part());
}

// +, -, * and / (abi::exact_binary_opcode) of two com values.
extern "C" void* quidra_com_binary(void* left, void* right, int operation, unsigned long long line,
                                   unsigned long long column) {
    const auto& a = *box_of(left);
    const auto& b = *box_of(right);
    using namespace abi::exact_binary_opcode;
    if (operation == add || operation == subtract) {
        return adopt_box(exact_binary(a.re, b.re, operation, line, column),
                         exact_binary(a.im, b.im, operation, line, column));
    }
    const auto product = [&](const ExactRealValue& x, const ExactRealValue& y) {
        return exact_binary(x, y, multiply, line, column);
    };
    const auto combine = [&](ExactRealValue x, ExactRealValue y, int op) {
        const auto result = exact_binary(x, y, op, line, column);
        release_part(x);
        release_part(y);
        return result;
    };
    if (operation == multiply) {
        // (ac - bd) + (ad + bc)i, exactly.
        auto re = combine(product(a.re, b.re), product(a.im, b.im), subtract);
        auto im = combine(product(a.re, b.im), product(a.im, b.re), add);
        return adopt_box(re, im);
    }
    // (a + bi) / (c + di) = ((ac + bd) + (bc - ad)i) / (c^2 + d^2), exactly.
    if (exact_is_zero(b.re) && exact_is_zero(b.im))
        quidra::runtime::report_failure(abi::FailureReason::real_zero_divisor,
                                        abi::FailureArgs{.type = "com"}, line, column,
                                        {.provenance = true});
    auto modulus = combine(product(b.re, b.re), product(b.im, b.im), add);
    auto re_numerator = combine(product(a.re, b.re), product(a.im, b.im), add);
    auto im_numerator = combine(product(a.im, b.re), product(a.re, b.im), subtract);
    auto re = exact_binary(re_numerator, modulus, divide, line, column);
    auto im = exact_binary(im_numerator, modulus, divide, line, column);
    release_part(re_numerator);
    release_part(im_numerator);
    release_part(modulus);
    return adopt_box(re, im);
}

extern "C" void* quidra_com_neg(void* value, unsigned long long line, unsigned long long column) {
    const auto& box = *box_of(value);
    return adopt_box(exact_neg(box.re, line, column), exact_neg(box.im, line, column));
}

extern "C" bool quidra_com_equal(void* left, void* right) {
    const auto& a = *box_of(left);
    const auto& b = *box_of(right);
    return exact_compare(a.re, b.re) == 0 && exact_compare(a.im, b.im) == 0;
}

// The text of a com value: each part as `real` prints it (to `significant`
// digits where a part is not a terminating decimal), joined by its sign.
extern "C" char* quidra_com_text(void* value, int significant) {
    const auto& box = *box_of(value);
    return join_parts(exact_text(box.re, significant), exact_text(box.im, significant));
}

// The printed form re+imi or re-imi whose parts real.parse reads (decimals,
// p/q); null for any other text.
extern "C" void* quidra_com_parse(const char* text) {
    std::string re_text, im_text;
    if (!split_complex_text(text, re_text, im_text)) return nullptr;
    if (im_text.front() == '+') im_text.erase(0, 1);
    void* re = quidra_bigreal_parse(re_text.c_str());
    if (!re) return nullptr;
    void* im = quidra_bigreal_parse(im_text.c_str());
    if (!im) {
        quidra_managed_release(re, reinterpret_cast<void*>(&quidra_bigreal_drop));
        return nullptr;
    }
    return adopt_box(adopt(re), adopt(im));
}
