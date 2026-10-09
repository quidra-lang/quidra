#include "constant_numeric_eval.hpp"
#include "ieee_decimal.hpp"
#include "quidra/abi/runtime_entry_points.hpp"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <string>

// Decimal text is rounded once, to the nearest value of the target IEEE
// format, ties to even, subnormals included (ieee_decimal.hpp), by the
// compiler for literals and by the runtime's real parsers. These tests pin
// the rounding at the places where it is easy to get wrong: halfway cases,
// the subnormal range and its lower end, the overflow threshold, and
// binary32 values whose binary64 approximation lies exactly halfway between
// two binary32 values. The expected encodings were computed with exact
// rational arithmetic.

namespace {

using namespace quidra::ieee_decimal;

int failures = 0;

void expect(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "numeric formats test failed: %s\n", what.c_str());
        ++failures;
    }
}

Rounded rounded(const char* text, const Format& format) {
    Decimal decimal;
    expect(parse(text, decimal), std::string("parse ") + text);
    return round(decimal, format);
}

enum Flag { none, overflow, underflow };

void expect_bits(const char* text, const Format& format, const char* name, std::uint64_t bits,
                 Flag flag = none) {
    const auto result = rounded(text, format);
    char message[256];
    std::snprintf(message, sizeof message, "%s as %s: %016llx (overflow %d, underflow %d)", text, name,
                  static_cast<unsigned long long>(result.bits), result.overflow, result.underflow_to_zero);
    expect(result.bits == bits && result.overflow == (flag == overflow) &&
               result.underflow_to_zero == (flag == underflow),
           message);
}

void halfway_and_boundary_cases() {
    expect_bits("0.1", binary64, "binary64", 0x3fb999999999999aULL);
    expect_bits("0.1", binary32, "binary32", 0x3dcccccdULL);
    expect_bits("2.2250738585072011e-308", binary64, "binary64", 0x000fffffffffffffULL);
    expect_bits("2.2250738585072012e-308", binary64, "binary64", 0x0010000000000000ULL);
    expect_bits("4.9406564584124654e-324", binary64, "binary64", 0x0000000000000001ULL);
    expect_bits("2.4703282292062328e-324", binary64, "binary64", 0x0000000000000001ULL);
    expect_bits("2.4703282292062327e-324", binary64, "binary64", 0, underflow);
    expect_bits("1.0e-400", binary64, "binary64", 0, underflow);
    expect_bits("1.7976931348623158e308", binary64, "binary64", 0x7fefffffffffffffULL);
    expect_bits("1.7976931348623159e308", binary64, "binary64", 0x7ff0000000000000ULL, overflow);
    expect_bits("1.0e400", binary64, "binary64", 0x7ff0000000000000ULL, overflow);
    expect_bits("9007199254740993", binary64, "binary64", 0x4340000000000000ULL);
    expect_bits("123456789012345678901234567890", binary64, "binary64", 0x45f8ee90ff6c373eULL);
    expect_bits("-0.0", binary64, "binary64", 0x8000000000000000ULL);
    expect_bits("00012.3400e2", binary64, "binary64", 0x4093480000000000ULL);

    // Just above the binary32 midpoint 1 + 2^-24: the binary64 value is the
    // midpoint itself, so rounding through binary64 would give 1.0.
    expect_bits("1.00000005960464477550", binary32, "binary32", 0x3f800001ULL);
    expect_bits("1.000000059604644775390625", binary32, "binary32", 0x3f800000ULL);
    expect_bits("1.000000178813934326171875", binary32, "binary32", 0x3f800002ULL);
    // The binary32 overflow threshold 2^128 - 2^103 rounds to infinity
    // (ties to even); a decimal below it whose binary64 value is the
    // threshold still rounds to the largest finite value.
    expect_bits("3.4028235677973366e38", binary32, "binary32", 0x7f7fffffULL);
    expect_bits("3.40282356779733661637539395458142568448e38", binary32, "binary32", 0x7f800000ULL,
                overflow);
    expect_bits("1.4e-45", binary32, "binary32", 0x00000001ULL);
    expect_bits(
        "7.006492321624085354618647916449580656401309709382578858785341419448955413429303e-46",
        binary32, "binary32", 0, underflow);
    expect_bits(
        "7.006492321624085354618647916449580656401309709382578858785341419448955413429304e-46",
        binary32, "binary32", 0x00000001ULL);
    expect_bits("1.17549435e-38", binary32, "binary32", 0x00800000ULL);
    expect_bits("1.1754942e-38", binary32, "binary32", 0x007fffffULL);
    expect_bits("9007199254740993", binary32, "binary32", 0x5a000000ULL);

    expect_bits("65504.0", binary16, "binary16", 0x7bffULL);
    expect_bits("65519.99", binary16, "binary16", 0x7bffULL);
    expect_bits("65520.0", binary16, "binary16", 0x7c00ULL, overflow);
    expect_bits("5.9604644775390625e-8", binary16, "binary16", 0x0001ULL);
    expect_bits("2.98023223876953125e-8", binary16, "binary16", 0, underflow);
    expect_bits("2.98023223876953126e-8", binary16, "binary16", 0x0001ULL);
    expect_bits("0.1", binary16, "binary16", 0x2e66ULL);
    expect_bits("1.0", bfloat16, "bfloat16", 0x3f80ULL);
    expect_bits("3.140625", bfloat16, "bfloat16", 0x4049ULL);
    expect_bits("1.00390625", bfloat16, "bfloat16", 0x3f80ULL);
    expect_bits("1.01171875", bfloat16, "bfloat16", 0x3f82ULL);
}

void decimal_syntax() {
    Decimal decimal;
    for (const char* text : {"1", "1.", ".5", "-1.5e-3", "+2E+8", "0.0"}) {
        expect(parse(text, decimal), std::string("accepts ") + text);
    }
    for (const char* text : {"", ".", "e5", "1e", "1e+", "1.5 ", " 1.5", "0x1p3", "inf", "nan", "1.2.3"}) {
        expect(!parse(text, decimal), std::string("rejects \"") + text + "\"");
    }
}

// Shortest-enough decimal text of every sampled finite value reads back as
// the same value, subnormals included.
void round_trips() {
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    const auto next = [&] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    char text[64];
    for (int i = 0; i < 200000; ++i) {
        const auto bits = next();
        const auto value64 = std::bit_cast<double>(bits >> (i % 4 == 0 ? 12 : 0));
        if (value64 - value64 == 0.0) {
            std::snprintf(text, sizeof text, "%.17g", value64);
            expect(rounded(text, binary64).bits == std::bit_cast<std::uint64_t>(value64),
                   std::string("binary64 round trip of ") + text);
        }
        const auto value32 = std::bit_cast<float>(static_cast<std::uint32_t>(bits >> (i % 4 == 0 ? 41 : 32)));
        if (value32 - value32 == 0.0F) {
            std::snprintf(text, sizeof text, "%.9g", static_cast<double>(value32));
            expect(rounded(text, binary32).bits == std::bit_cast<std::uint32_t>(value32),
                   std::string("binary32 round trip of ") + text);
        }
    }
}

void literal_values() {
    using quidra::Type;
    using quidra::TypeKind;
    using quidra::constant_eval::real_literal_value;
    const auto real32 = Type::simple(TypeKind::Real32);
    const auto real64 = Type::simple(TypeKind::Real64);
    expect(real_literal_value("1.00000005960464477550", real32) ==
               static_cast<double>(std::bit_cast<float>(0x3f800001U)),
           "a binary32 literal is rounded once");
    expect(real_literal_value("0.1", real64) == 0.1, "a binary64 literal");
    expect(!real_literal_value("3.4028235677973367e38", real32), "binary32 overflow");
    expect(real_literal_value("3.4028235677973366e38", real32) ==
               static_cast<double>(std::bit_cast<float>(0x7f7fffffU)),
           "the largest binary32 value");
    expect(real_literal_value("4.9406564584124654e-324", real64) ==
               std::bit_cast<double>(std::uint64_t{1}),
           "the smallest binary64 subnormal");
    expect(!real_literal_value("1.0", Type::simple(TypeKind::Real)), "no IEEE format");
}

// float32.parse, float.parse and scan read real text with the same rounding.
// Subnormal values are accepted (text that prints a subnormal reads back);
// values beyond the finite range and nonzero values that round to zero are
// rejected; leading white space and the forms strtod reads besides decimals
// keep their historical reading.
void runtime_parsers() {
    const auto parse64 = [](const char* text, std::uint64_t bits) {
        double value = 0.0;
        expect(quidra_parse_float64(text, &value) && std::bit_cast<std::uint64_t>(value) == bits,
               std::string("real64 parses ") + text);
    };
    const auto parse32 = [](const char* text, std::uint32_t bits) {
        float value = 0.0F;
        expect(quidra_parse_float32(text, &value) && std::bit_cast<std::uint32_t>(value) == bits,
               std::string("real32 parses ") + text);
    };
    const auto reject = [](const char* text) {
        double value64 = 0.0;
        float value32 = 0.0F;
        expect(!quidra_parse_float64(text, &value64), std::string("real64 rejects ") + text);
        expect(!quidra_parse_float32(text, &value32), std::string("real32 rejects ") + text);
    };
    parse64("4.9406564584124654e-324", 0x0000000000000001ULL);
    parse64("2.2250738585072011e-308", 0x000fffffffffffffULL);
    parse64("0.1", 0x3fb999999999999aULL);
    parse64(" 1.5", 0x3ff8000000000000ULL);
    parse64("+2.5", 0x4004000000000000ULL);
    parse64("-0.0", 0x8000000000000000ULL);
    parse64("0x1p3", 0x4020000000000000ULL);
    parse32("1.4e-45", 0x00000001U);
    parse32("1.1754942e-38", 0x007fffffU);
    parse32("1.00000005960464477550", 0x3f800001U);
    parse32("3.4028235677973366e38", 0x7f7fffffU);
    reject("1.0e-400");
    reject("1.0e400");
    reject("inf");
    reject("nan");
    reject("1.5 ");
    reject("1.5x");
    reject("");
    float value32 = 0.0F;
    expect(!quidra_parse_float32("3.40282356779733661637539395458142568448e38", &value32),
           "float32 rejects the overflow threshold");
    expect(!quidra_parse_float32("7.0e-46", &value32), "float32 rejects a value that rounds to zero");
}

} // namespace

int main() {
    halfway_and_boundary_cases();
    decimal_syntax();
    round_trips();
    literal_values();
    runtime_parsers();
    if (failures != 0) {
        std::fprintf(stderr, "%d numeric formats test(s) failed\n", failures);
        return 1;
    }
    std::puts("numeric formats tests passed");
    return 0;
}
