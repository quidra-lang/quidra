#pragma once

// Correctly rounded conversion of decimal text to IEEE 754 binary formats.
//
// Owns: the one conversion that real literals (the compiler) and the
// runtime's real parsers share. The decimal value is taken exactly, as a
// fraction of integers, and rounded once to the nearest value of the target
// format, ties to even, subnormals included. Nothing passes through another
// binary format on the way, so a binary32 result is never rounded twice.
//
// Self-contained (standard library only), so that both the compiler and the
// runtime library can include it.

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace quidra::ieee_decimal {

// An IEEE 754 binary interchange format.
struct Format {
    int precision;     // significand bits, the implicit bit included
    int min_exponent;  // exponent of the smallest normal value
    int max_exponent;  // exponent of the largest finite value
    int width;         // bits of the encoding
};

inline constexpr Format binary16{11, -14, 15, 16};
inline constexpr Format bfloat16{8, -126, 127, 16};
inline constexpr Format binary32{24, -126, 127, 32};
inline constexpr Format binary64{53, -1022, 1023, 64};

// A decimal number as written: an optional sign, digits with at most one
// decimal point, and an optional exponent. `digits` holds the digits without
// the point; the value is digits × 10^(exponent - fraction_digits).
struct Decimal {
    bool negative{};
    std::string_view integer_digits;
    std::string_view fraction_digits;
    long long exponent{};
};

// Reads `text` as [+-]digits[.digits][(e|E)[+-]digits], where the digits
// before or after the point (not both) may be empty. Returns false for any
// other text. Exponents beyond ±10^15 are clamped; they round to zero or
// overflow in every format either way.
inline bool parse(std::string_view text, Decimal& out) {
    std::size_t i = 0;
    out = Decimal{};
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        out.negative = text[i] == '-';
        ++i;
    }
    const auto digit = [&](std::size_t at) { return at < text.size() && text[at] >= '0' && text[at] <= '9'; };
    const auto integer_begin = i;
    while (digit(i)) ++i;
    out.integer_digits = text.substr(integer_begin, i - integer_begin);
    if (i < text.size() && text[i] == '.') {
        ++i;
        const auto fraction_begin = i;
        while (digit(i)) ++i;
        out.fraction_digits = text.substr(fraction_begin, i - fraction_begin);
    }
    if (out.integer_digits.empty() && out.fraction_digits.empty()) return false;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        bool negative_exponent = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            negative_exponent = text[i] == '-';
            ++i;
        }
        if (!digit(i)) return false;
        constexpr long long exponent_limit = 1000000000000000LL;
        long long exponent = 0;
        while (digit(i)) {
            if (exponent < exponent_limit) exponent = exponent * 10 + (text[i] - '0');
            ++i;
        }
        out.exponent = negative_exponent ? -exponent : exponent;
    }
    return i == text.size();
}

namespace detail {

// A non-negative integer of arbitrary size: little-endian 32-bit limbs, no
// leading zero limb.
class Natural {
public:
    explicit Natural(std::uint64_t value = 0) {
        while (value) {
            limbs_.push_back(static_cast<std::uint32_t>(value));
            value >>= 32;
        }
    }

    bool zero() const { return limbs_.empty(); }

    int bit_length() const {
        if (limbs_.empty()) return 0;
        return static_cast<int>(32 * (limbs_.size() - 1)) +
               static_cast<int>(std::bit_width(limbs_.back()));
    }

    void multiply_add(std::uint32_t factor, std::uint32_t addend) {
        std::uint64_t carry = addend;
        for (auto& limb : limbs_) {
            const auto product = static_cast<std::uint64_t>(limb) * factor + carry;
            limb = static_cast<std::uint32_t>(product);
            carry = product >> 32;
        }
        if (carry) limbs_.push_back(static_cast<std::uint32_t>(carry));
        trim();
    }

    void multiply_power_of_five(long long exponent) {
        constexpr std::uint32_t five_13 = 1220703125U;  // 5^13
        while (exponent >= 13) {
            multiply_add(five_13, 0);
            exponent -= 13;
        }
        std::uint32_t rest = 1;
        while (exponent-- > 0) rest *= 5;
        if (rest != 1) multiply_add(rest, 0);
    }

    void shift_left(int bits) {
        if (limbs_.empty() || bits == 0) return;
        const auto whole = static_cast<std::size_t>(bits / 32);
        const auto part = bits % 32;
        if (part) {
            std::uint32_t carry = 0;
            for (auto& limb : limbs_) {
                const auto next = limb >> (32 - part);
                limb = (limb << part) | carry;
                carry = next;
            }
            if (carry) limbs_.push_back(carry);
        }
        limbs_.insert(limbs_.begin(), whole, 0U);
    }

    void shift_right_one() {
        std::uint32_t carry = 0;
        for (auto i = limbs_.size(); i > 0; --i) {
            const auto limb = limbs_[i - 1];
            limbs_[i - 1] = (limb >> 1) | (carry << 31);
            carry = limb & 1U;
        }
        trim();
    }

    // *this -= other; requires *this >= other.
    void subtract(const Natural& other) {
        std::int64_t borrow = 0;
        for (std::size_t i = 0; i < limbs_.size(); ++i) {
            std::int64_t value = static_cast<std::int64_t>(limbs_[i]) - borrow -
                                 (i < other.limbs_.size() ? static_cast<std::int64_t>(other.limbs_[i]) : 0);
            borrow = value < 0 ? 1 : 0;
            if (value < 0) value += (std::int64_t{1} << 32);
            limbs_[i] = static_cast<std::uint32_t>(value);
        }
        trim();
    }

    friend int compare(const Natural& left, const Natural& right) {
        if (left.limbs_.size() != right.limbs_.size()) {
            return left.limbs_.size() < right.limbs_.size() ? -1 : 1;
        }
        for (auto i = left.limbs_.size(); i > 0; --i) {
            if (left.limbs_[i - 1] != right.limbs_[i - 1]) {
                return left.limbs_[i - 1] < right.limbs_[i - 1] ? -1 : 1;
            }
        }
        return 0;
    }

private:
    void trim() {
        while (!limbs_.empty() && limbs_.back() == 0) limbs_.pop_back();
    }

    std::vector<std::uint32_t> limbs_;
};

inline std::uint64_t encode(const Format& format, bool negative, std::uint64_t significand,
                            int biased_exponent) {
    const auto fraction_bits = format.precision - 1;
    const auto sign = negative ? std::uint64_t{1} << (format.width - 1) : 0;
    return sign | (static_cast<std::uint64_t>(biased_exponent) << fraction_bits) |
           (significand & ((std::uint64_t{1} << fraction_bits) - 1));
}

inline std::uint64_t infinity_bits(const Format& format, bool negative) {
    return encode(format, negative, 0, 2 * format.max_exponent + 1);
}

} // namespace detail

// The result of rounding a decimal number to a format.
struct Rounded {
    // The encoding in the format's width (the low `width` bits).
    std::uint64_t bits{};
    // The value is beyond the largest finite value after rounding; `bits`
    // holds the infinity of its sign.
    bool overflow{};
    // The decimal value is not zero, but it rounds to zero.
    bool underflow_to_zero{};
};

namespace detail {

// The digits of `decimal` without leading and trailing zeros, and the
// power of ten that scales them to the value.
struct Significant {
    std::string_view integer;
    std::string_view fraction;
    std::size_t count{};
    long long exponent{};
};

inline Significant significant(const Decimal& decimal) {
    Significant result{decimal.integer_digits, decimal.fraction_digits, 0, 0};
    while (!result.integer.empty() && result.integer.front() == '0') result.integer.remove_prefix(1);
    if (result.integer.empty()) {
        while (!result.fraction.empty() && result.fraction.front() == '0') result.fraction.remove_prefix(1);
    }
    long long trailing = 0;
    while (!result.fraction.empty() && result.fraction.back() == '0') {
        result.fraction.remove_suffix(1);
        ++trailing;
    }
    if (result.fraction.empty()) {
        while (!result.integer.empty() && result.integer.back() == '0') {
            result.integer.remove_suffix(1);
            ++trailing;
        }
    }
    result.count = result.integer.size() + result.fraction.size();
    result.exponent =
        decimal.exponent - static_cast<long long>(decimal.fraction_digits.size()) + trailing;
    return result;
}

// value = digits × 10^exponent, exactly, rounded to `format`.
inline Rounded round_exact(const Format& format, bool negative, const Significant& digits) {
    Rounded result;
    if (digits.count == 0) {
        result.bits = encode(format, negative, 0, 0);
        return result;
    }
    // 10^(exponent + count - 1) <= value < 10^(exponent + count). Values
    // beyond 10^310 overflow and values below 10^-330 round to zero in every
    // format, so the exact arithmetic below stays bounded.
    const auto magnitude = digits.exponent + static_cast<long long>(digits.count);
    if (magnitude > 310) {
        result.bits = infinity_bits(format, negative);
        result.overflow = true;
        return result;
    }
    if (magnitude < -330) {
        result.bits = encode(format, negative, 0, 0);
        result.underflow_to_zero = true;
        return result;
    }

    // value = numerator / denominator × 2^exponent2.
    Natural numerator;
    const auto append = [&](std::string_view text) {
        for (const char c : text) numerator.multiply_add(10U, static_cast<std::uint32_t>(c - '0'));
    };
    append(digits.integer);
    append(digits.fraction);
    Natural denominator(1);
    if (digits.exponent >= 0) {
        numerator.multiply_power_of_five(digits.exponent);
    } else {
        denominator.multiply_power_of_five(-digits.exponent);
    }
    const auto exponent2 = digits.exponent;

    // The binary exponent e of the value: 2^e <= value < 2^(e+1).
    long long e = static_cast<long long>(numerator.bit_length()) - denominator.bit_length() + exponent2;
    {
        // Compare numerator × 2^exponent2 with 2^e × denominator.
        Natural left = numerator;
        Natural right = denominator;
        const long long shift = e - exponent2;
        if (shift >= 0) {
            right.shift_left(static_cast<int>(shift));
        } else {
            left.shift_left(static_cast<int>(-shift));
        }
        if (compare(left, right) < 0) --e;
    }

    // The significand q = floor(value / 2^scale), q < 2^precision.
    const long long scale = std::max<long long>(e, format.min_exponent) - (format.precision - 1);
    const long long shift = exponent2 - scale;
    Natural remainder = numerator;
    Natural divisor = denominator;
    if (shift >= 0) {
        remainder.shift_left(static_cast<int>(shift));
    } else {
        divisor.shift_left(static_cast<int>(-shift));
    }
    std::uint64_t q = 0;
    Natural step = divisor;
    step.shift_left(format.precision);
    for (int bit = format.precision; bit >= 0; --bit) {
        if (compare(remainder, step) >= 0) {
            remainder.subtract(step);
            q |= std::uint64_t{1} << bit;
        }
        step.shift_right_one();
    }

    // Round to nearest, ties to even: compare 2 × remainder with the divisor.
    remainder.shift_left(1);
    const auto half = compare(remainder, divisor);
    if (half > 0 || (half == 0 && (q & 1U))) ++q;

    long long leading = scale + format.precision - 1;
    if (q == (std::uint64_t{1} << format.precision)) {
        q >>= 1;
        ++leading;
    }
    if (q == 0) {
        result.bits = encode(format, negative, 0, 0);
        result.underflow_to_zero = true;
        return result;
    }
    if (q < (std::uint64_t{1} << (format.precision - 1))) {
        result.bits = encode(format, negative, q, 0);  // subnormal
        return result;
    }
    if (leading > format.max_exponent) {
        result.bits = infinity_bits(format, negative);
        result.overflow = true;
        return result;
    }
    result.bits = encode(format, negative, q, static_cast<int>(leading + format.max_exponent));
    return result;
}

// Exact powers of ten that binary64 holds: 10^0 .. 10^22.
inline constexpr double binary64_powers_of_ten[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

// When digits ≤ 2^53 and |exponent| ≤ 22, both operands are exact in
// binary64 and one IEEE multiplication or division rounds the exact value
// correctly (Clinger's fast path). Requires binary64 arithmetic without
// excess precision.
inline bool binary64_fast_path(const Significant& digits, double& out) {
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD == 0
    if (digits.count > 16 || digits.exponent < -22 || digits.exponent > 22) return false;
    std::uint64_t value = 0;
    for (const char c : digits.integer) value = value * 10 + static_cast<std::uint64_t>(c - '0');
    for (const char c : digits.fraction) value = value * 10 + static_cast<std::uint64_t>(c - '0');
    if (value > (std::uint64_t{1} << 53)) return false;
    const auto x = static_cast<double>(value);
    out = digits.exponent >= 0 ? x * binary64_powers_of_ten[digits.exponent]
                               : x / binary64_powers_of_ten[-digits.exponent];
    return true;
#else
    (void)digits;
    (void)out;
    return false;
#endif
}

} // namespace detail

// `decimal` rounded to the nearest value of `format`, ties to even.
inline Rounded round(const Decimal& decimal, const Format& format) {
    const auto digits = detail::significant(decimal);
    if (digits.count != 0 && (format.width == 64 || format.width == 32)) {
        double fast = 0.0;
        if (detail::binary64_fast_path(digits, fast)) {
            // The fast path's values lie in [1e-22, 2^53 × 1e22], inside the
            // normal range of both formats.
            const auto bits = std::bit_cast<std::uint64_t>(fast);
            if (format.width == 64) {
                return Rounded{bits | (decimal.negative ? std::uint64_t{1} << 63 : 0), false, false};
            }
            // binary32: rounding the binary64 result again gives the
            // correctly rounded binary32 value unless the binary64 result is
            // exactly halfway between two binary32 values, where the exact
            // decimal decides.
            constexpr std::uint64_t below_binary32 = (std::uint64_t{1} << 29) - 1;
            if ((bits & below_binary32) != (std::uint64_t{1} << 28)) {
                const auto narrowed = std::bit_cast<std::uint32_t>(static_cast<float>(fast));
                return Rounded{narrowed | (decimal.negative ? std::uint64_t{1} << 31 : 0), false, false};
            }
        }
    }
    return detail::round_exact(format, decimal.negative, digits);
}

// The binary64 value of `rounded` (a binary64 result).
inline double binary64_value(const Rounded& rounded) { return std::bit_cast<double>(rounded.bits); }

// The binary32 value of `rounded` (a binary32 result).
inline float binary32_value(const Rounded& rounded) {
    return std::bit_cast<float>(static_cast<std::uint32_t>(rounded.bits));
}

// The value of a finite `rounded` result of `format`, exactly, in binary64,
// which holds every value of the formats above.
inline double binary64_value(const Rounded& rounded, const Format& format) {
    const auto fraction_bits = format.precision - 1;
    const auto biased = static_cast<int>((rounded.bits >> fraction_bits) &
                                         ((std::uint64_t{1} << (format.width - 1 - fraction_bits)) - 1));
    auto significand = rounded.bits & ((std::uint64_t{1} << fraction_bits) - 1);
    if (biased != 0) significand |= std::uint64_t{1} << fraction_bits;
    const auto exponent = (biased != 0 ? biased : 1) - format.max_exponent - fraction_bits;
    const auto magnitude = std::ldexp(static_cast<double>(significand), exponent);
    return (rounded.bits >> (format.width - 1)) & 1U ? -magnitude : magnitude;
}

} // namespace quidra::ieee_decimal
