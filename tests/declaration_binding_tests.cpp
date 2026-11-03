#include "optimizer/declaration_binding.hpp"
#include "optimizer/rule_registry.hpp"
#include "quidra/compiler_extension.hpp"
#include "quidra/diagnostic.hpp"
#include <iostream>
#include <stdexcept>

using namespace quidra;
using namespace quidra::optimizer;
static void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
static DeclarationIdentity declaration(std::string name, std::string type = "tensor<real32>") {
    DeclarationIdentity result;
    result.package = "generic"; result.source_name = std::move(name);
    result.signature = "(" + type + ")"; result.parameters = {{"value", type, false, false}};
    result.result_type = type; result.source_file = "/generic/package/main.qui";
    return result;
}
static CompilerExtensionRegistration descriptor(std::string selectors, bool legacy = false) {
    return parse_compiler_extension_descriptor("generic", "graph", "/generic/package", "/generic/package/graph.toml",
        "[extension]\nversion = 2\nphase = \"" + std::string(legacy ? "tensor-region" : "tensor-program") +
        "\"\n[operation.op]\nfunction = \"" + selectors + "\"\ntraits = \"tensor,pure\"\n");
}
static ir::Function instance(std::string name, DeclarationIdentity identity) {
    ir::Function result; result.name = std::move(name); result.source_file = identity.source_file;
    result.result = Type::tensor(Type::simple(TypeKind::Real32));
    result.parameters.push_back({"value", result.result}); result.declaration = std::move(identity);
    return result;
}
static void rejected(const ir::Module& module, std::string_view fragment) {
    try { (void)RuleRegistry::build(module); }
    catch (const CompileError& e) {
        check(e.diagnostic().code == "PACKAGE_COMPILER_EXTENSION", "binding error lost diagnostic category");
        check(std::string_view(e.what()).find(fragment) != std::string_view::npos, "wrong binding error"); return;
    }
    throw std::runtime_error("invalid binding accepted");
}
int main() {
    try {
        ir::Module module;
        auto real = declaration("work", "tensor<T>"); real.generic_parameters = {"T"}; real.generic_constraints = {"real"};
        auto integer = real; integer.generic_constraints = {"integer"};
        module.declaration_catalog = {real, integer};
        module.compiler_extensions = {descriptor("work")};
        rejected(module, "ambiguous selector");
        module.compiler_extensions = {descriptor("work<T: real>(tensor<T>)")};
        auto unused = RuleRegistry::build(module);
        check(unused.bindings().operation_functions.at("generic.graph:op").empty(), "unused generic rejected");
        check(unused.declarations().operations.at("generic.graph:op").declarations.size() == 1, "original declaration missing");
        auto clone = real; clone.generic_args = {"real32"}; clone.specialization_of = declaration_key(real);
        module.functions = {instance("$spec.arbitrary", clone), instance("other.generated", clone),
                            instance("generic.work", declaration("different"))};
        module.functions[1].declaration->source_file = "/different/provenance.qui";
        check(same_declaration(real, *module.functions[1].declaration) &&
              declaration_key(real) == declaration_key(*module.functions[1].declaration), "provenance became binding identity");
        auto registry = RuleRegistry::build(module);
        const auto& calls = registry.bindings().operation_functions.at("generic.graph:op");
        check(calls == std::vector<std::string>{"$spec.arbitrary", "other.generated"}, "generic specialization identity lost");
        check(registry.normalized_tables().size() == 2 && registry.normalized_tables()[1].normalized.has_value(), "full tables not in one registry");
        auto foreign = real; foreign.package = "other"; module.declaration_catalog.push_back(foreign);
        check(RuleRegistry::build(module).bindings().operation_functions.at("generic.graph:op") == calls, "package collision bound");
        module.compiler_extensions = {descriptor("missing")}; rejected(module, "names no declaration");
        module.declaration_catalog = {declaration("first"), declaration("second", "tensor<real64>")};
        module.compiler_extensions = {descriptor("first|second")}; rejected(module, "incompatible parameter/result");
        module.declaration_catalog = {declaration("first")};
        auto invalid = descriptor("first");
        invalid = parse_compiler_extension_descriptor("generic", "graph", "/generic/package", "/generic/package/graph.toml",
            "[extension]\nversion = 2\nphase = \"tensor-program\"\n[operation.op]\nfunction = \"first\"\ntraits = \"tensor,pure\"\nreads = \"absent\"\n");
        module.compiler_extensions = {std::move(invalid)}; rejected(module, "not an eligible tensor parameter");
        auto relu = declaration("relu"); auto overmatch = declaration("forward_relu");
        module.declaration_catalog = {relu, overmatch};
        module.functions = {instance("alias.relu", relu), instance("$gf_alias_forward_relu_kernel", overmatch)};
        module.compiler_extensions = {descriptor("relu", true)};
        const auto old = RuleRegistry::build(module);
        check(old.bindings().operation_functions.at("generic.graph:op").size() == 2, "legacy matching changed");
        const auto verify = RuleRegistry::build(module, DeclarationBindingMode::Verify);
        check(verify.bindings().operation_functions.at("generic.graph:op").size() == 2 && verify.binding_differences().size() == 1,
              "verify changed legacy behavior or lost difference");
        const auto exact = RuleRegistry::build(module, DeclarationBindingMode::On);
        check(exact.bindings().operation_functions.at("generic.graph:op") == std::vector<std::string>{"alias.relu"}, "exact legacy binding overmatched");
        std::cout << "declaration binding tests passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
