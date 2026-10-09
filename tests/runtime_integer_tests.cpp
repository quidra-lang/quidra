#include "quidra/abi/exact_codes.hpp"
#include "quidra/abi/layout.hpp"
#include "quidra/abi/runtime_entry_points.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <string>
#include <tuple>
#include <vector>

// Arbitrary-precision integer words (abi::bare_integer_layout) against the
// boxed big integers they replace. Every word operation of the runtime is
// checked against the boxed operation on the same values: the texts agree,
// and every result word is canonical (inline exactly when the value is
// within the inline range). The inline fast paths that generated code takes
// (addition and subtraction of the words with an overflow check,
// multiplication of an untagged operand by a word, division and remainder
// on the words) are modelled here and must give the runtime's words.

namespace {

namespace layout = quidra::abi::bare_integer_layout;
namespace opcode = quidra::abi::exact_binary_opcode;

int failures = 0;

void expect(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "runtime integer test failed: %s\n", what.c_str());
        ++failures;
    }
}

std::string take_text(char* text) {
    std::string out = text ? text : "";
    quidra_managed_release(text, nullptr);
    return out;
}

std::string word_text(long long word) { return take_text(quidra_int_text(word)); }

void release_word(long long word) {
    if ((word & layout::boxed_bit) == 0) return;
    auto* box = reinterpret_cast<void*>(static_cast<std::uintptr_t>(word) &
                                        ~static_cast<std::uintptr_t>(layout::boxed_bit));
    quidra_managed_release(box, reinterpret_cast<void*>(&quidra_bigint_drop));
}

bool is_inline(long long word) { return (word & layout::boxed_bit) == 0; }

// The value of a decimal text as an int64, when it is one.
bool text_i64(const std::string& text, long long& out) {
    errno = 0;
    char* end = nullptr;
    out = std::strtoll(text.c_str(), &end, 10);
    return errno == 0 && end && *end == '\0' && !text.empty();
}

// A word is canonical: inline exactly when its value is within the inline
// range, and an inline word's value is its arithmetic half.
bool canonical(long long word) {
    long long value = 0;
    const auto text = word_text(word);
    const bool small = text_i64(text, value) && value >= layout::inline_min &&
                       value <= layout::inline_max;
    if (small != is_inline(word)) return false;
    return !small || (word >> 1) == value;
}

long long word_of(const std::string& text) {
    long long word = 0;
    expect(quidra_int_parse(text.c_str(), &word), "parse " + text);
    return word;
}

std::string boxed_text(const std::string& left, const std::string& right, int operation) {
    void* a = quidra_bigint_parse(left.c_str());
    void* b = quidra_bigint_parse(right.c_str());
    void* r = quidra_bigint_binary(a, b, operation, 0, 0);
    const auto text = take_text(quidra_bigint_text(r));
    for (void* p : {a, b, r}) quidra_managed_release(p, reinterpret_cast<void*>(&quidra_bigint_drop));
    return text;
}

int boxed_compare(const std::string& left, const std::string& right) {
    void* a = quidra_bigint_parse(left.c_str());
    void* b = quidra_bigint_parse(right.c_str());
    const int c = quidra_bigint_compare(a, b);
    quidra_managed_release(a, reinterpret_cast<void*>(&quidra_bigint_drop));
    quidra_managed_release(b, reinterpret_cast<void*>(&quidra_bigint_drop));
    return c;
}

bool is_zero(const std::string& text) { return text == "0"; }

// Checked int64 arithmetic, without compiler builtins.
bool checked_add(long long a, long long b, long long& out) {
    constexpr long long max = std::numeric_limits<long long>::max();
    constexpr long long min = std::numeric_limits<long long>::min();
    if ((b > 0 && a > max - b) || (b < 0 && a < min - b)) return false;
    out = a + b;
    return true;
}
bool checked_sub(long long a, long long b, long long& out) {
    constexpr long long max = std::numeric_limits<long long>::max();
    constexpr long long min = std::numeric_limits<long long>::min();
    if ((b < 0 && a > max + b) || (b > 0 && a < min + b)) return false;
    out = a - b;
    return true;
}
bool checked_mul(long long a, long long b, long long& out) {
    if (a == 0 || b == 0) {
        out = 0;
        return true;
    }
    const auto magnitude = [](long long v) {
        return v < 0 ? static_cast<unsigned long long>(-(v + 1)) + 1ULL
                     : static_cast<unsigned long long>(v);
    };
    const auto ua = magnitude(a), ub = magnitude(b);
    if (ua > std::numeric_limits<unsigned long long>::max() / ub) return false;
    const auto product = ua * ub;
    constexpr auto limit = static_cast<unsigned long long>(std::numeric_limits<long long>::max());
    if ((a < 0) != (b < 0)) {
        if (product > limit + 1ULL) return false;
        out = product == limit + 1ULL ? std::numeric_limits<long long>::min()
                                      : -static_cast<long long>(product);
        return true;
    }
    if (product > limit) return false;
    out = static_cast<long long>(product);
    return true;
}

// The fast paths of the generated code (bare_integer.cpp) on two words,
// when they apply: true with the result word, false when the generated code
// calls the runtime.
bool fast_path(long long a, long long b, int operation, long long& out) {
    if (!is_inline(a) || !is_inline(b)) return false;
    switch (operation) {
        case opcode::add: return checked_add(a, b, out);
        case opcode::subtract: return checked_sub(a, b, out);
        case opcode::multiply: return checked_mul(a >> 1, b, out);
        case opcode::divide: {
            if (b == 0) return false;
            const long long q = a / b;
            if (q == (1LL << 62)) return false;
            out = static_cast<long long>(static_cast<unsigned long long>(q) << 1);
            return true;
        }
        case opcode::remainder:
            if (b == 0) return false;
            out = a % b;
            return true;
        default: return false;
    }
}

long long runtime_binary(long long a, long long b, int operation) {
    switch (operation) {
        case opcode::add: return quidra_int_add(a, b);
        case opcode::subtract: return quidra_int_sub(a, b);
        case opcode::multiply: return quidra_int_mul(a, b);
        case opcode::divide: return quidra_int_div(a, b, 0, 0);
        default: return quidra_int_rem(a, b, 0, 0);
    }
}

const char* operation_name(int operation) {
    switch (operation) {
        case opcode::add: return "+";
        case opcode::subtract: return "-";
        case opcode::multiply: return "*";
        case opcode::divide: return "/";
        default: return "%";
    }
}

void check_pair(const std::string& left, const std::string& right) {
    const long long a = word_of(left), b = word_of(right);
    for (int operation : {opcode::add, opcode::subtract, opcode::multiply, opcode::divide,
                          opcode::remainder}) {
        if ((operation == opcode::divide || operation == opcode::remainder) && is_zero(right))
            continue;
        const auto what = left + " " + operation_name(operation) + " " + right;
        const long long word = runtime_binary(a, b, operation);
        expect(word_text(word) == boxed_text(left, right, operation), what + " value");
        expect(canonical(word), what + " canonical");
        long long quick = 0;
        if (fast_path(a, b, operation, quick)) expect(quick == word, what + " fast path");
        release_word(word);
    }
    const int c = quidra_int_compare(a, b);
    expect(c == boxed_compare(left, right), left + " <=> " + right);
    if (is_inline(a) || is_inline(b)) {
        // One inline word decides equality by the words alone.
        expect((a == b) == (c == 0), left + " == " + right + " by words");
    }
    if (is_inline(a) && is_inline(b)) {
        expect((a < b) == (c < 0), left + " < " + right + " by words");
    }
    release_word(a);
    release_word(b);
}

void check_conversions(const std::string& text) {
    const long long word = word_of(text);
    void* box = quidra_bigint_parse(text.c_str());
    for (int bits : {8, 16, 32, 63, 64}) {
        long long a = 0, b = 0;
        const bool word_ok = quidra_int_try_i64(word, bits, &a);
        const bool box_ok = quidra_bigint_try_i64(box, bits, &b);
        expect(word_ok == box_ok && a == b, "try_i64 " + text + " bits " + std::to_string(bits));
        unsigned long long ua = 0, ub = 0;
        const bool uword_ok = quidra_int_try_u64(word, bits, &ua);
        const bool ubox_ok = quidra_bigint_try_u64(box, bits, &ub);
        expect(uword_ok == ubox_ok && ua == ub, "try_u64 " + text + " bits " + std::to_string(bits));
        expect(quidra_int_cast_fits(word, 3, bits) == quidra_exact_numeric_cast_fits(box, 1, 3, bits),
               "cast_fits signed " + text);
        expect(quidra_int_cast_fits(word, 4, bits) == quidra_exact_numeric_cast_fits(box, 1, 4, bits),
               "cast_fits unsigned " + text);
    }
    double fa = 0.0, fb = 0.0;
    const bool f_ok = quidra_int_try_float64(word, &fa);
    expect(f_ok == quidra_bigint_try_float64(box, &fb) && fa == fb, "try_float64 " + text);
    float ga = 0.0F, gb = 0.0F;
    const bool g_ok = quidra_int_try_float32(word, &ga);
    expect(g_ok == quidra_bigint_try_float32(box, &gb) && ga == gb, "try_float32 " + text);
    const long long negated = quidra_int_neg(word, 0, 0);
    expect(word_text(negated) == boxed_text("0", text, opcode::subtract), "neg " + text);
    expect(canonical(negated), "neg canonical " + text);
    release_word(negated);
    // adopt takes a managed big integer and gives its canonical word.
    void* copy = quidra_bigint_parse(text.c_str());
    const long long adopted = quidra_int_adopt(copy);
    expect(word_text(adopted) == text && canonical(adopted), "adopt " + text);
    release_word(adopted);
    quidra_managed_release(box, reinterpret_cast<void*>(&quidra_bigint_drop));
    release_word(word);
}

std::string decimal(long long value) { return std::to_string(value); }

} // namespace

int main() {
    const long long min62 = layout::inline_min, max62 = layout::inline_max;
    const long long min64 = std::numeric_limits<long long>::min();
    const long long max64 = std::numeric_limits<long long>::max();

    // The codec: inline exactly within [-2^62, 2^62 - 1].
    for (long long value : {0LL, 1LL, -1LL, 2LL, -2LL, min62, max62, min62 + 1, max62 - 1,
                            min62 - 1, max62 + 1, min64, max64, min64 + 1, max64 - 1}) {
        const long long word = quidra_int_from_i64(value);
        const bool inside = value >= min62 && value <= max62;
        expect(is_inline(word) == inside, "inline " + decimal(value));
        if (inside) expect((word >> 1) == value, "inline value " + decimal(value));
        expect(word_text(word) == decimal(value), "text " + decimal(value));
        long long back = 0;
        expect(quidra_int_try_i64(word, 64, &back) && back == value, "round trip " + decimal(value));
        release_word(word);
    }
    for (unsigned long long value : {0ULL, 1ULL, static_cast<unsigned long long>(max62),
                                     static_cast<unsigned long long>(max62) + 1ULL,
                                     static_cast<unsigned long long>(max64),
                                     std::numeric_limits<unsigned long long>::max()}) {
        const long long word = quidra_int_from_u64(value);
        expect(word_text(word) == std::to_string(value), "from_u64 " + std::to_string(value));
        expect(canonical(word), "from_u64 canonical " + std::to_string(value));
        release_word(word);
    }

    // Operations around the inline boundaries and beyond 64 bits.
    std::vector<std::string> values{
        "0", "1", "-1", "2", "-2", "3", "-7", "1000000007",
        decimal(min62), decimal(max62), decimal(min62 + 1), decimal(max62 - 1),
        decimal(min62 - 1), decimal(max62 + 1), decimal(min64), decimal(max64),
        "18446744073709551616", "-18446744073709551617",
        "340282366920938463463374607431768211456", "-123456789012345678901234567890",
        "2147483647", "-2147483648", "2147483648", "4294967296"};
    for (const auto& left : values)
        for (const auto& right : values) check_pair(left, right);
    for (const auto& value : values) check_conversions(value);

    // Random operands, inline and boxed, from a fixed seed.
    std::mt19937_64 random(20261009);
    std::vector<std::string> random_values;
    for (int i = 0; i < 120; ++i) {
        const auto raw = static_cast<long long>(random());
        switch (i % 4) {
            case 0: random_values.push_back(decimal(raw >> 33)); break;
            case 1: random_values.push_back(decimal(raw >> 1)); break;
            case 2: random_values.push_back(decimal(raw)); break;
            default: random_values.push_back(decimal(raw) + std::to_string(random() % 1000000000ULL)); break;
        }
    }
    for (std::size_t i = 0; i + 1 < random_values.size(); ++i)
        check_pair(random_values[i], random_values[i + 1]);

    // Powers keep the boxed values; a negative exponent is left to the
    // failure path, which ends the process, and is not run here.
    for (const auto& base : {"2", "-3", "10", "4611686018427387903"}) {
        for (const auto& exponent : {"0", "1", "2", "62", "63", "100"}) {
            const long long b = word_of(base), e = word_of(exponent);
            const long long word = quidra_int_pow(b, e, 0, 0);
            void* pb = quidra_bigint_parse(base);
            void* pe = quidra_bigint_parse(exponent);
            void* pr = quidra_bigint_pow(pb, pe, 0, 0);
            expect(word_text(word) == take_text(quidra_bigint_text(pr)),
                   std::string(base) + " ^ " + exponent);
            expect(canonical(word), std::string(base) + " ^ " + exponent + " canonical");
            for (void* p : {pb, pe, pr})
                quidra_managed_release(p, reinterpret_cast<void*>(&quidra_bigint_drop));
            release_word(word);
            release_word(b);
            release_word(e);
        }
    }

    // nat subtraction: the difference when it is not negative (a negative
    // one is left to the failure path, which ends the process).
    for (const auto& [left, right, difference] :
         {std::tuple{"5", "3", "2"}, std::tuple{"7", "7", "0"},
          std::tuple{"18446744073709551616", "1", "18446744073709551615"},
          std::tuple{"4611686018427387904", "4611686018427387903", "1"}}) {
        const long long a = word_of(left), b = word_of(right);
        const long long word = quidra_nat_sub(a, b, 0, 0);
        expect(word_text(word) == difference, std::string(left) + " - " + right + " in nat");
        expect(canonical(word), std::string(left) + " - " + right + " canonical");
        release_word(word);
        release_word(a);
        release_word(b);
    }

    // A task result crosses threads detached from the managed table and is
    // attached again as the same canonical word.
    for (const auto& text : {"0", "-1", "4611686018427387904", "-340282366920938463463374607431768211456"}) {
        const long long word = word_of(text);
        const long long moved = quidra_int_attach(quidra_int_detach(word));
        expect(word_text(moved) == text, std::string("detach/attach ") + text);
        expect(canonical(moved), std::string("detach/attach canonical ") + text);
        release_word(moved);
    }

    // Parsing accepts what the boxed parser accepts.
    for (const auto& text : {"+5", "-0", "007", "", "-", "1a", " 1", "99999999999999999999999"}) {
        long long word = 0;
        void* box = quidra_bigint_parse(text);
        expect(quidra_int_parse(text, &word) == (box != nullptr), std::string("parse '") + text + "'");
        if (box) {
            expect(word_text(word) == take_text(quidra_bigint_text(box)), std::string("parse value ") + text);
            quidra_managed_release(box, reinterpret_cast<void*>(&quidra_bigint_drop));
            release_word(word);
        }
    }

    // A big literal's box lives for the program: releasing it does nothing.
    const long long literal = quidra_int_literal("123456789012345678901234567890");
    expect(!is_inline(literal), "big literal is boxed");
    release_word(literal);
    release_word(literal);
    expect(word_text(literal) == "123456789012345678901234567890", "big literal survives release");
    expect(quidra_int_literal("42") == 84, "small literal is its word");

    if (failures != 0) {
        std::fprintf(stderr, "%d runtime integer test(s) failed\n", failures);
        return 1;
    }
    std::puts("runtime integer tests passed");
    return 0;
}
