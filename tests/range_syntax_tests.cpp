#include "quidra/frontend.hpp"
#include "quidra/source_tools.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace quidra;

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
ConcreteProgram expand(std::string_view source) {
    const auto root = std::filesystem::temp_directory_path() / "quidra-range-syntax" / "main.qui";
    return expand_generics(load_program_with_root_source(root, source, root.parent_path(), 20, false));
}
void generic_bounds() {
    const auto concrete = expand(
        "int ceiling<T>()\n    return 7\n"
        "int bounded<T>(0 < int value < ceiling<T>())\n"
        "    0 < local_value < ceiling<T>() := value\n"
        "    0 < local_value < ceiling<T>() = value\n"
        "    return local_value\n"
        "void shape<T>(tensor<T>{0 < nat N < ceiling<T>()} data)\n    void\n"
        "class Window<T>(0 < int width < ceiling<T>())\n"
        "    void method(int height > width)\n        void\n"
        "result := bounded<int>(3)\n"
        "shape<real32>(tensor.ones<real32>([3]))\n"
        "window := Window<int>(3)\n");
    bool scalar = false, shape = false, constructor = false;
    const auto check_call = [&](const std::optional<RangeConstraint>& constraint) {
        require(constraint && constraint->upper, "generic upper bound was dropped");
        const auto* call = std::get_if<CallExpr>(&constraint->upper->data);
        require(call && call->type_arguments.empty(), "generic range bound call was not specialized");
        require(std::any_of(concrete.program.functions.begin(), concrete.program.functions.end(),
            [&](const FunctionDecl& function) { return function.name == call->callee; }),
            "specialized range bound has no concrete function");
    };
    for (const auto& function : concrete.program.functions) {
        if (function.parameters.size() != 1) continue;
        const auto& parameter = function.parameters.front();
        if (parameter.name == "value" && parameter.range_constraint) {
            scalar = true;
            check_call(parameter.range_constraint);
            const auto& binding = std::get<BindingStmt>(function.body.at(0)->data);
            const auto& assignment = std::get<AssignStmt>(function.body.at(1)->data);
            check_call(binding.range_constraint);
            check_call(assignment.range_constraint);
            require(binding.range_constraint->upper != assignment.range_constraint->upper,
                    "different executable constraints share a mutable bound expression");
        }
        if (parameter.name == "data" && parameter.type.brace_shape) {
            shape = true;
            check_call(parameter.type.shape_axes.at(0).range_constraint);
        }
    }
    for (const auto& declaration : concrete.program.classes) {
        for (const auto& method : declaration.methods) {
            if (!method.is_constructor || method.parameters.empty() || method.parameters.front().name != "width") continue;
            constructor = true;
            check_call(method.parameters.front().range_constraint);
        }
    }
    require(scalar && shape && constructor, "generic scalar, shape or class-header constraint disappeared");
}
void imported_bounds() {
    const auto directory = std::filesystem::temp_directory_path() / "quidra-range-syntax-import";
    std::filesystem::create_directories(directory);
    struct Cleanup { std::filesystem::path directory; ~Cleanup() { std::filesystem::remove_all(directory); } } cleanup{directory};
    {
        std::ofstream module(directory / "limits.qui");
        module << "int ceiling()\n    return 7\n"
                  "const 0 < int LIMIT < ceiling() := 3\n"
                  "void bounded(0 < int value < ceiling())\n    void\n"
                  "void shape(tensor<real32>{0 < nat N < ceiling()} data)\n    void\n";
    }
    const auto concrete = expand_generics(load_program_with_root_source(directory / "main.qui",
        "import limits = \"./limits.qui\"\nlimits.bounded(3)\n"
        "limits.shape(tensor.ones<real32>([3]))\n", directory, 20, false));
    bool scalar = false, shape = false;
    for (const auto& function : concrete.program.functions) {
        if (function.parameters.size() != 1) continue;
        const auto& parameter = function.parameters.front();
        const auto check = [&](const std::optional<RangeConstraint>& range) {
            require(range && range->upper, "imported bound was dropped");
            const auto& call = std::get<CallExpr>(range->upper->data);
            require(call.callee != "ceiling" && call.callee.ends_with(".ceiling"),
                    "imported bound function was not qualified");
        };
        if (parameter.name == "value" && parameter.range_constraint) { scalar = true; check(parameter.range_constraint); }
        if (parameter.name == "data" && parameter.type.brace_shape) { shape = true; check(parameter.type.shape_axes.at(0).range_constraint); }
    }
    require(scalar && shape, "imported scalar or shape contract disappeared");
    bool constant = false;
    for (const auto& statement : concrete.program.statements) {
        const auto* guard = std::get_if<MainGuardStmt>(&statement->data);
        if (!guard || !guard->active) continue;
        for (const auto& child : guard->body) {
            const auto* binding = std::get_if<BindingStmt>(&child->data);
            if (!binding || binding->name != "LIMIT") continue;
            constant = binding->range_constraint && binding->range_constraint->upper &&
                       std::get<CallExpr>(binding->range_constraint->upper->data).callee.ends_with(".ceiling");
        }
    }
    require(constant, "imported constrained constant lost its active module precondition");
}
void prototypes_and_inspection() {
    (void)expand("void bounded(0 < int value <= 5);\n"
                 "void bounded(0 < int value <= 5)\n    void\n");
    bool rejected = false;
    try {
        (void)expand("void bounded(0 < int value <= 5);\n"
                     "void bounded(0 < int value <= 6)\n    void\n");
    } catch (const CompileErrors& errors) {
        rejected = std::any_of(errors.diagnostics().begin(), errors.diagnostics().end(),
            [](const Diagnostic& value) { return value.code == "PROTOTYPE_MISMATCH"; });
    } catch (const CompileError& error) { rejected = error.diagnostic().code == "PROTOTYPE_MISMATCH"; }
    require(rejected, "prototype accepted different scalar range bounds");
    rejected = false;
    try {
        (void)expand("void shaped(tensor<real32>{0 < nat N < #[1, 2]} data);\n"
                     "void shaped(tensor<real32>{0 < nat N < #[1, 2, 3]} data)\n    void\n");
    } catch (const CompileErrors& errors) {
        rejected = std::any_of(errors.diagnostics().begin(), errors.diagnostics().end(),
            [](const Diagnostic& value) { return value.code == "PROTOTYPE_MISMATCH"; });
    } catch (const CompileError& error) { rejected = error.diagnostic().code == "PROTOTYPE_MISMATCH"; }
    require(rejected, "prototype shape key collapsed distinct bound expressions");
    const auto source = "0 < value < ceiling() := 3\n"
                        "void shape(tensor<real32>{0 < nat N < ceiling()} data)\n    void\n";
    const auto inspection = inspect_syntax_source(source);
    require(std::count_if(inspection.nodes.begin(), inspection.nodes.end(),
        [](const SourceNode& node) { return node.kind == "RangeConstraint"; }) == 2,
        "source inspection omitted scalar or shape constraints");
}
}
int main() {
    try {
        generic_bounds();
        imported_bounds();
        prototypes_and_inspection();
        std::cout << "range syntax preservation tests passed\n";
    } catch (const CompileErrors& errors) {
        for (const auto& error : errors.diagnostics()) std::cerr << error.code << ": " << error.message << '\n';
        return 1;
    } catch (const CompileError& error) {
        std::cerr << error.diagnostic().code << ": " << error.diagnostic().message << '\n';
        return 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
