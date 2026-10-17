#include "quidra/checker.hpp"
#include "quidra/lexer.hpp"
#include "quidra/parser.hpp"

#include <iostream>
#include <string>
#include <utility>

namespace {
int failures = 0;
int cases = 0;

void check(const std::string& name, const std::string& source, bool should_accept) {
    ++cases;
    bool accepted = false;
    bool initialization_error = false;
    std::string diagnostics;
    const auto record = [&](const quidra::Diagnostic& diagnostic) {
        diagnostics += diagnostic.code + ": " + diagnostic.message + "\n";
        initialization_error |= diagnostic.code == "UNINITIALIZED" ||
                                diagnostic.code == "UNINITIALIZED_ARGUMENT";
    };
    try {
        auto program = quidra::Parser(quidra::Lexer(source).scan()).parse();
        (void)quidra::Checker().check({std::move(program), {}});
        accepted = true;
    } catch (const quidra::CompileError& error) {
        record(error.diagnostic());
    } catch (const quidra::CompileErrors& errors) {
        for (const auto& diagnostic : errors.diagnostics()) record(diagnostic);
    }
    const bool passed = should_accept ? accepted : !accepted && initialization_error;
    if (!passed) {
        ++failures;
        std::cerr << "FAIL " << name << ": "
                  << (accepted ? "accepted uninitialized selector\n" : diagnostics);
    }
}
} // namespace

int main() {
    const std::string values = "int[2] values = [10, 20]\n";
    for (const std::string type : {"int", "nat", "bool"}) {
        const std::string first = type == "bool" ? "true" : "1";
        const std::string second = type == "bool" ? "false" : "0";
        const std::string declaration = type + "[2] selector\n";
        const std::string select = "int[] result = values[selector]\n";
        const std::string first_write = "selector[0] = " + first + "\n";
        const std::string second_write = "selector[1] = " + second + "\n";

        check(type + " uninitialized", values + declaration + select, false);
        check(type + " partial", values + declaration + first_write + select, false);
        check(type + " initialized", values + declaration + first_write + second_write + select, true);
        check(type + " initialized literal", values + type + "[2] selector = [" +
              first + ", " + second + "]\n" + select, true);
        // Escaped references use runtime initialization checks, matching
        // ordinary scalar indexing through references.
        check(type + " borrowed uninitialized runtime checked", values + declaration +
              type + "[2] &alias = &selector\nint[] result = values[alias]\n", true);
        check(type + " initialized through alias", values + declaration +
              type + "[2] &alias = &selector\n" +
              "alias[0] = " + first + "\nalias[1] = " + second + "\n" + select, true);
        check(type + " partially initialized through alias runtime checked", values + declaration +
              type + "[2] &alias = &selector\n" +
              "alias[0] = " + first + "\n" + select, true);
        check(type + " borrowed initialized", values + declaration + first_write + second_write +
              type + "[2] &alias = &selector\nint[] result = values[alias]\n", true);
        // Unknown caller-provided element initialization retains the existing
        // runtime check, as for scalar indexing through a reference parameter.
        check(type + " reference parameter runtime checked",
              "int[] select(int[2] values, " + type + "[2] &selector)\n"
              "    return values[selector]\n" + values + declaration +
              "int[] result = select(values, &selector)\n", true);
        check(type + " reference parameter initialized",
              "int[] select(int[2] values, " + type + "[2] &selector)\n"
              "    return values[selector]\n" + values + declaration + first_write + second_write +
              "int[] result = select(values, &selector)\n", true);
        // No element is read from an empty array. In particular an empty
        // boolean mask is valid only for an empty input, not for values above.
        check(type + " empty selector", "int[0] values\n" + type +
              "[0] selector\nint[] result = values[selector]\n", true);
    }
    // indices() needs only allocation metadata, not initialized elements.
    check("indices reads no elements", "int[2] values\nnat[] indices = values.indices()\n", true);
    // Preserve the pre-existing element read check on the selected source.
    check("uninitialized source", "int[2] values\nint[] selector = [0]\n"
          "int[] result = values[selector]\n", false);

    std::cout << (cases - failures) << '/' << cases << " selector initialization contracts passed\n";
    return failures ? 1 : 0;
}
