#include "quidra/checker.hpp"
#include "quidra/lexer.hpp"
#include "quidra/parser.hpp"
#include <cstdio>
#include <string>
#include <utility>

int main() {
    int cases = 0, failures = 0;
    const auto check = [&](const std::string& type, const std::string& expression,
                           const std::string& expected_error = "") {
        ++cases;
        bool accepted = false, matched = false;
        std::string diagnostics;
        const auto record = [&](const quidra::Diagnostic& d) {
            matched |= d.code == expected_error;
            diagnostics += d.code + ": " + d.message + "\n";
        };
        try {
            auto program = quidra::Parser(quidra::Lexer(type + " result = " + expression + "\n").scan()).parse();
            (void)quidra::Checker().check({std::move(program), {}});
            accepted = true;
        } catch (const quidra::CompileError& error) {
            record(error.diagnostic());
        } catch (const quidra::CompileErrors& errors) {
            for (const auto& diagnostic : errors.diagnostics()) record(diagnostic);
        }
        if (expected_error.empty() ? !accepted : accepted || !matched) {
            ++failures;
            std::fprintf(stderr, "FAIL %s %s: expected %s, got %s\n", type.c_str(), expression.c_str(),
                         expected_error.empty() ? "accept" : expected_error.c_str(),
                         accepted ? "accept" : diagnostics.c_str());
        }
    };
    for (const std::string type : {"real16", "real16b", "real32", "real64"}) {
        check(type, "1.0 / 0.0");
        check(type, "1.0 / -0.0");
        check(type, "0.0 ^ -2.0");
        check(type, "(-0.0) ^ -3.0");
        check(type, "1.0 / (1.0 / 0.0)");
        const std::string large = type == "real16" ? "60000.0" : type == "real64" ? "1.0e308" : "3.0e38";
        check(type, large + " * 10.0");
        check(type, "0.0 / 0.0", "DIVIDE_BY_ZERO");
        check(type, "(1.0 / 0.0) - (1.0 / 0.0)", "REAL_RANGE");
        check(type, "((1.0 / 0.0) - (1.0 / 0.0)) ^ 0.0", "REAL_RANGE");
        check(type, "0.0 ^ 0.0", "POWER_DOMAIN");
        check(type, "(-1.0) ^ 0.5", "POWER_DOMAIN");
    }
    check("real", "0.0 ^ -2.0");
    check("real", "(-0.0) ^ -3.0");
    check("real", "0.0 ^ 0.0", "POWER_DOMAIN");
    check("real16", "65536.0", "REAL_RANGE");
    check("int", "1 / 0", "DIVIDE_BY_ZERO");
    check("int", "0 ^ 0", "POWER_DOMAIN");
    std::printf("%d/%d constant infinity contracts passed\n", cases - failures, cases);
    return failures ? 1 : 0;
}
