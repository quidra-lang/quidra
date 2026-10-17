#include "quidra/abi/exact_codes.hpp"
#include "quidra/abi/layout.hpp"
#include "quidra/abi/runtime_entry_points.hpp"
#include "quidra/native_extension.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>

namespace {
int failures = 0;
void expect(bool condition, const std::string& name) {
    if (!condition) { std::fprintf(stderr, "infinity test failed: %s\n", name.c_str()); ++failures; }
}
struct Node {
    void* value{};
    explicit Node(void* p) : value(p) {}
    ~Node() { if (value) quidra_managed_release(value, reinterpret_cast<void*>(&quidra_bigreal_drop)); }
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
};
std::string text(void* p, int precision = quidra::abi::exact_real_canonical_text) {
    char* raw = quidra_bigreal_text(p, precision);
    std::string result = raw ? raw : "";
    quidra_managed_release(raw, nullptr);
    return result;
}
void binary(void* a, void* b, int op, const char* expected) {
    Node result(quidra_bigreal_binary(a, b, op, 1, 1));
    expect(text(result.value) == expected, std::string("binary -> ") + expected);
}
}
int main(int argc, char** argv) {
    Node positive(quidra_bigreal_parse("inf"));
    Node negative(quidra_bigreal_parse("-inf"));
    expect(positive.value != nullptr, "exact real must parse inf");
    expect(negative.value != nullptr, "exact real must parse -inf");
    if (!positive.value || !negative.value) return 1;
    Node zero(quidra_bigreal_literal("0.0"));
    Node one(quidra_bigreal_literal("1.0"));
    Node minus_two(quidra_bigreal_literal("-2.0"));
    namespace op = quidra::abi::exact_binary_opcode;
    if (argc == 2) {
        const std::string mode = argv[1];
        if (mode == "zero-zero") binary(zero.value, zero.value, op::divide, "unreachable");
        else if (mode == "inf-minus-inf") binary(positive.value, positive.value, op::subtract, "unreachable");
        else if (mode == "opposite-infinities") binary(positive.value, negative.value, op::add, "unreachable");
        else if (mode == "inf-div-inf") binary(positive.value, negative.value, op::divide, "unreachable");
        else if (mode == "zero-times-inf") binary(zero.value, positive.value, op::multiply, "unreachable");
        else if (mode == "nan-input") {
            Node invalid(quidra_bigreal_from_float64(std::numeric_limits<double>::quiet_NaN()));
        } else return 2;
        return 3; // An invalid operation must have stopped the program.
    }
    expect(text(positive.value) == "∞", "exact positive infinity display");
    expect(text(negative.value) == "-∞", "exact negative infinity display");
    expect(text(positive.value, 4) == "∞", "significant digits preserve infinity");
    for (const char* input : {"inf", "+inf", "∞", "+∞", "-inf", "-∞"}) {
        Node parsed(quidra_bigreal_parse(input));
        expect(parsed.value != nullptr, std::string("parse ") + input);
        if (parsed.value) expect(text(parsed.value) == (input[0] == '-' ? "-∞" : "∞"), "parse/display round trip");
    }
    expect(quidra_bigreal_parse("nan") == nullptr, "NaN text stays rejected");
    expect(quidra_bigreal_parse("inf junk") == nullptr, "infinity parser consumes all text");
    binary(positive.value, one.value, op::add, "∞");
    binary(one.value, negative.value, op::add, "-∞");
    binary(negative.value, negative.value, op::add, "-∞");
    binary(positive.value, negative.value, op::subtract, "∞");
    binary(positive.value, minus_two.value, op::multiply, "-∞");
    binary(negative.value, negative.value, op::multiply, "∞");
    binary(one.value, positive.value, op::divide, "0.0");
    binary(minus_two.value, negative.value, op::divide, "0.0");
    binary(positive.value, minus_two.value, op::divide, "-∞");
    binary(one.value, zero.value, op::divide, "∞");
    binary(minus_two.value, zero.value, op::divide, "-∞");
    binary(positive.value, zero.value, op::divide, "∞");
    expect(quidra_bigreal_compare(negative.value, positive.value, 1, 1) < 0, "-inf < inf");
    expect(quidra_bigreal_compare(positive.value, positive.value, 1, 1) == 0, "inf == inf");
    expect(quidra_bigreal_compare(positive.value, one.value, 1, 1) > 0, "inf > finite");
    expect(quidra_bigreal_compare(one.value, negative.value, 1, 1) > 0, "finite > -inf");
    Node negated(quidra_bigreal_neg(positive.value, 1, 1));
    expect(text(negated.value) == "-∞", "negation");
    for (double input : {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        Node from_ieee(quidra_bigreal_from_float64(input));
        expect(text(from_ieee.value) == (std::signbit(input) ? "-∞" : "∞"), "IEEE -> exact infinity");
        double result = 0.0;
        expect(quidra_bigreal_try_float64(from_ieee.value, &result) && result == input, "exact -> binary64 infinity");
        float result32 = 0.0F;
        expect(quidra_bigreal_try_float32(from_ieee.value, &result32) && result32 == input, "exact -> binary32 infinity");
        long long integer = 0;
        expect(!quidra_bigreal_try_i64(from_ieee.value, 64, &integer), "infinity cannot become an integer");
    }
    Node huge(quidra_bigreal_literal("1.0e999"));
    expect(quidra_bigreal_compare(positive.value, huge.value, 1, 1) > 0, "exact infinity > arbitrary finite real");
    double narrowing = 0.0;
    expect(!quidra_bigreal_try_float64(huge.value, &narrowing), "checked finite narrowing remains checked");
    // Existing small-rational ABI remains unchanged: infinity is a managed node.
    quidra_managed_retain(positive.value);
    quidra::abi::ExactRealValue value{};
    quidra_real_demote(positive.value, &value);
    expect(value.denominator == 0, "infinity is stored in general node form");
    if (value.denominator == 0) {
        Node stored(reinterpret_cast<void*>(static_cast<std::uintptr_t>(value.numerator)));
        char* formatted = quidra_real_format(value.numerator, value.denominator, -1, 3, -1, 0);
        expect(std::string(formatted) == "∞", "formatted exact infinity");
        quidra_managed_release(formatted, nullptr);
    }
    std::fprintf(stdout, "%d exact infinity failures\n", failures);
    return failures ? 1 : 0;
}
