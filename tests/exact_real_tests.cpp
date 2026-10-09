#include "quidra/abi/exact_codes.hpp"
#include "quidra/abi/layout.hpp"
#include "quidra/abi/runtime_entry_points.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

// The small form of the exact type `real` (abi::ExactRealValue with a
// nonzero denominator) and its general form, a managed runtime node, give
// the same values. Every small operation is checked against the node
// operation on the same operands: a small result must equal the node
// result demoted, and the node result of an operation the small path
// declines (an overflow of the 64-bit words, a division by zero) is the
// general path's. Demotion keeps the canonical invariant: a value with a
// small form is stored in it, in lowest terms.

namespace {

using quidra::abi::ExactRealValue;
namespace opcode = quidra::abi::exact_binary_opcode;

int failures = 0;

void expect(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "exact real test failed: %s\n", what.c_str());
        ++failures;
    }
}

constexpr long long max64 = std::numeric_limits<long long>::max();

std::string text_of(void* node) {
    char* text = quidra_bigreal_text(node, 60);
    std::string out = text ? text : "";
    quidra_managed_release(text, nullptr);
    return out;
}

std::string small_text(const ExactRealValue& value) {
    char* text = quidra_real_small_text(value.numerator, value.denominator, 60);
    std::string out = text ? text : "";
    quidra_managed_release(text, nullptr);
    return out;
}

void release_node(void* node) {
    quidra_managed_release(node, reinterpret_cast<void*>(&quidra_bigreal_drop));
}

std::string describe(const ExactRealValue& value) {
    return std::to_string(value.numerator) + "/" + std::to_string(value.denominator);
}

bool canonical(const ExactRealValue& value) {
    if (value.denominator <= 0) return false;
    if (value.numerator == std::numeric_limits<long long>::min()) return false;
    if (value.numerator == 0) return value.denominator == 1;
    auto a = static_cast<unsigned long long>(value.numerator < 0 ? -value.numerator : value.numerator);
    auto b = static_cast<unsigned long long>(value.denominator);
    while (b) {
        const auto t = a % b;
        a = b;
        b = t;
    }
    return a == 1;
}

// The node result of left OP right, demoted: the general path.
ExactRealValue node_binary(const ExactRealValue& left, const ExactRealValue& right, int operation,
                           std::string& text) {
    void* a = quidra_real_promote(left.numerator, left.denominator);
    void* b = quidra_real_promote(right.numerator, right.denominator);
    void* result = quidra_bigreal_binary(a, b, operation, 0, 0);
    text = text_of(result);
    release_node(a);
    release_node(b);
    ExactRealValue out{};
    quidra_real_demote(result, &out);
    return out;
}

const char* operation_name(int operation) {
    switch (operation) {
        case opcode::add: return "+";
        case opcode::subtract: return "-";
        case opcode::multiply: return "*";
        default: return "/";
    }
}

void differential(const std::vector<ExactRealValue>& values) {
    for (const auto& left : values) {
        expect(canonical(left), "operand " + describe(left) + " is canonical");
        for (const auto& right : values) {
            for (const int operation : {opcode::add, opcode::subtract, opcode::multiply, opcode::divide}) {
                if (operation == opcode::divide && right.numerator == 0) continue;
                const auto what = describe(left) + " " + operation_name(operation) + " " + describe(right);
                std::string node_text;
                const auto general = node_binary(left, right, operation, node_text);
                ExactRealValue small{};
                if (quidra_real_small_binary(left.numerator, left.denominator, right.numerator,
                                             right.denominator, operation, &small)) {
                    expect(canonical(small), what + ": small result is canonical");
                    expect(general.denominator != 0 && general.numerator == small.numerator &&
                               general.denominator == small.denominator,
                           what + ": small result " + describe(small) + " equals the node result");
                    expect(small_text(small) == node_text, what + ": text " + node_text);
                } else if (general.denominator != 0) {
                    // Declined by the small path, small after reduction.
                    expect(canonical(general), what + ": demoted result is canonical");
                    expect(small_text(general) == node_text, what + ": demoted text " + node_text);
                } else {
                    release_node(reinterpret_cast<void*>(static_cast<std::uintptr_t>(general.numerator)));
                }
            }
        }
    }
}

void boundaries() {
    ExactRealValue out{};
    // INT64_MAX + 1 leaves the small form; the node keeps the exact value.
    expect(!quidra_real_small_binary(max64, 1, 1, 1, opcode::add, &out), "INT64_MAX + 1 is not small");
    std::string text;
    auto general = node_binary({max64, 1}, {1, 1}, opcode::add, text);
    expect(general.denominator == 0, "INT64_MAX + 1 stays a node");
    expect(text == "9223372036854775808.0", "INT64_MAX + 1 prints exactly, got " + text);
    // ... and comes back to the small form when it fits again.
    void* node = reinterpret_cast<void*>(static_cast<std::uintptr_t>(general.numerator));
    void* one = quidra_real_promote(1, 1);
    void* back = quidra_bigreal_binary(node, one, opcode::subtract, 0, 0);
    release_node(node);
    release_node(one);
    ExactRealValue demoted{};
    quidra_real_demote(back, &demoted);
    expect(demoted.numerator == max64 && demoted.denominator == 1, "INT64_MAX + 1 - 1 demotes to INT64_MAX");
    // -INT64_MAX - 1 is INT64_MIN, which the small form excludes.
    expect(!quidra_real_small_binary(-max64, 1, 1, 1, opcode::subtract, &out), "INT64_MIN is not small");
    general = node_binary({-max64, 1}, {1, 1}, opcode::subtract, text);
    expect(general.denominator == 0, "INT64_MIN stays a node");
    if (general.denominator == 0)
        release_node(reinterpret_cast<void*>(static_cast<std::uintptr_t>(general.numerator)));
    // A product beyond 64 bits whose quotient is small again.
    expect(!quidra_real_small_binary(max64, 1, 2, 1, opcode::multiply, &out), "INT64_MAX * 2 is not small");
    // Cross reduction keeps products small: (2^62/3) * (3/2^61) = 2.
    expect(quidra_real_small_binary(1LL << 62, 3, 3, 1LL << 61, opcode::multiply, &out) &&
               out.numerator == 2 && out.denominator == 1,
           "cross-reduced product is 2");
    // Division by zero is left to the general path, which reports it.
    expect(!quidra_real_small_binary(1, 1, 0, 1, opcode::divide, &out), "division by zero is not small");
}

void conversions() {
    double d = 0.0;
    expect(quidra_real_small_try_float64(1, 3, &d) && d == 1.0 / 3.0, "1/3 as real64");
    float f = 0.0F;
    expect(quidra_real_small_try_float32(1, 10, &f) && f == 0.1F, "1/10 as real32");
    long long i = 0;
    expect(quidra_real_small_try_i64(-7, 1, 8, &i) && i == -7, "-7 as int8");
    expect(!quidra_real_small_try_i64(300, 1, 8, &i), "300 is no int8");
    expect(!quidra_real_small_try_i64(1, 2, 64, &i), "1/2 is no integer");
    unsigned long long u = 0;
    expect(quidra_real_small_try_u64(255, 1, 8, &u) && u == 255, "255 as nat8");
    expect(!quidra_real_small_try_u64(-1, 1, 64, &u), "-1 is no nat64");
    expect(quidra_real_small_cast_fits(5, 1, 3, 8), "5 fits int8");
    expect(!quidra_real_small_cast_fits(5, 2, 1, 0), "5/2 is no bigint");
    // The small path and the node path observe the same value.
    for (const auto& [n, den] : std::vector<std::pair<long long, long long>>{
             {1, 3}, {-22, 7}, {max64, 1}, {1, max64}, {123456789, 1000}}) {
        void* node = quidra_real_promote(n, den);
        double from_node = 0.0, from_small = 0.0;
        expect(quidra_bigreal_try_float64(node, &from_node) &&
                   quidra_real_small_try_float64(n, den, &from_small) && from_node == from_small,
               "real64 of " + std::to_string(n) + "/" + std::to_string(den));
        expect(text_of(node) == small_text({n, den}),
               "text of " + std::to_string(n) + "/" + std::to_string(den));
        release_node(node);
    }
}

} // namespace

int main() {
    std::vector<ExactRealValue> values{
        {0, 1}, {1, 1}, {-1, 1}, {2, 1}, {1, 2}, {-1, 2}, {1, 3}, {2, 3}, {-5, 7},
        {22, 7}, {1, 10}, {3, 10}, {1, 1000000007}, {1LL << 31, 1}, {(1LL << 31) + 2, 3},
        {1LL << 62, 1}, {-(1LL << 62), 1}, {(1LL << 62) - 1, 1LL << 61}, {max64, 1},
        {-max64, 1}, {1, max64}, {max64 - 1, max64}, {999999999999999999LL, 1000000000000000000LL}};
    differential(values);
    boundaries();
    conversions();
    if (failures) {
        std::fprintf(stderr, "exact real tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("exact real tests passed\n");
    return 0;
}
