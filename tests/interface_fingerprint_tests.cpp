#include "semantics/interface_fingerprint.hpp"
#include "semantics/interface_contracts.hpp"
#include "ir/fact_sources.hpp"
#include "ir/summaries.hpp"
#include "quidra/lexer.hpp"
#include "quidra/parser.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace quidra;
using namespace quidra::semantics;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Fixture {
    ir::Module module;
    InterfaceContracts contracts;
    EffectSummary effect;
    Fixture() {
        DeclarationIdentity declaration;
        declaration.package = "example";
        declaration.module_path = "api";
        declaration.source_name = "read";
        declaration.source_file = "api.qui";
        declaration.signature = "(tensor<real32>)";
        declaration.parameters.push_back({"value","tensor<real32>",false,false});
        declaration.result_type = "tensor<real32>";
        module.declaration_catalog.push_back(declaration);
        ir::Function function;
        function.name = "internal.first";
        function.declaration = declaration;
        function.parameters.push_back({"value",Type::tensor(Type::simple(TypeKind::Real32)),false,false,false,false});
        function.result = function.parameters[0].type;
        module.functions.push_back(std::move(function));
        FunctionInterface contract;
        contract.declaration = declaration;
        contract.parameter_defaults = {std::nullopt};
        contract.initialization = InitializationContract{};
        contracts.functions.push_back(std::move(contract));
        contracts.declarations_complete = contracts.classes_complete =
            contracts.constants_complete = contracts.aliases_complete =
            contracts.enums_complete = contracts.reexports_complete = true;
        effect.exposed_reads.resize(1);
        effect.exposed_writes.resize(1);
        effect.result = {ResultProvenance::Kind::Parameter,0};
        effect.reference_links.push_back({{StorageRoot::parameter(0),{}},
            {StorageRoot::shared("value"),{}},true,{{2,1,3},{4,1,5}}});
        rebuild();
    }
    void rebuild() {
        EffectSummaries semantic;
        semantic.set("semantic.read",effect);
        ir::analysis::SummaryBindings bindings;
        bindings.emplace(module.functions[0].name,ir::analysis::SemanticBinding{"semantic.read",{0}});
        module.summaries = std::make_shared<const ir::analysis::ModuleSummaries>(
            ir::analysis::adapt_summaries(module,semantic,bindings));
        auto facts = std::make_shared<ir::facts::FactSources>();
        ExtentOrigin origin;
        origin.kind = ExtentOrigin::Kind::Expr;
        origin.expression = ShapeExtent::symbol(77);
        origin.expression_axes.emplace(77,std::pair<std::size_t,std::size_t>{0,1});
        facts->functions.emplace(module.functions[0].name,ir::facts::FunctionFactSource{{origin},{0}});
        module.fact_sources = std::move(facts);
    }
    ModuleFingerprints fingerprint() const {
        return fingerprint_module(module,{"example","api"},contracts);
    }
};
void essential_contract_changes() {
    Fixture fixture;
    const auto unavailable = fingerprint_module(fixture.module,{"example","api"});
    require(!unavailable.signature && !unavailable.summary && !unavailable.missing.empty(),
            "missing producer snapshots yielded usable fingerprints");
    const auto before = fixture.fingerprint();
    require(before.signature && before.summary && before.missing.empty(),"complete fixture unavailable");
    fixture.module.functions[0].name = "internal.renamed";
    fixture.module.functions[0].source_file = "moved.qui";
    fixture.module.functions[0].source_line = 90;
    fixture.module.functions[0].declaration->source_file = "moved.qui";
    fixture.effect.reference_links[0].site = {{200,90,3},{400,90,5}};
    fixture.effect.first_hidden_mutation = EffectCause{{{5,2,3},{8,2,6}},"diagnostic only"};
    fixture.rebuild();
    const auto moved = fixture.fingerprint();
    require(moved.signature == before.signature && moved.summary == before.summary,
            "generated names/provenance/diagnostic cause entered fingerprints");
    record_resource(fixture.effect.resources,{ResourceKind::Clock,ResourceUse::Shared,ResourceOrigin::Direct,{}});
    fixture.rebuild();
    const auto resource = fixture.fingerprint();
    require(resource.signature == moved.signature && resource.summary != moved.summary,
            "external resource contract was omitted");
    fixture.effect.exposed_writes[0].insert({StorageRoot::parameter(0),{PathStep::any_element()}});
    fixture.rebuild();
    const auto writes = fixture.fingerprint();
    require(writes.summary != resource.summary,"storage write contract was omitted");
    fixture.module.functions[0].parameters[0].borrowed = true;
    fixture.rebuild();
    const auto borrowed = fixture.fingerprint();
    require(borrowed.summary != writes.summary,"caller borrow ABI was omitted");
    fixture.contracts.functions[0].export_c = ForeignInterface{"C","read_public"};
    const auto exported = fixture.fingerprint();
    require(exported.signature != borrowed.signature,"C export contract was omitted");
    fixture.contracts.functions[0].initialization->receiver.initializes.insert("field");
    require(fixture.fingerprint().summary != exported.summary,"initialization contract was omitted");
    fixture.contracts.constants.push_back({"LIMIT",false,false,{"int",Type::simple(TypeKind::Int64)},
        {"test-value/1",ir::canonical_encode_value(std::int64_t{3}).bytes}});
    const auto constant = fixture.fingerprint();
    fixture.contracts.constants[0].initializer.fields = ir::canonical_encode_value(std::int64_t{4}).bytes;
    require(fixture.fingerprint().signature != constant.signature,"constant value was omitted");
}
void original_snapshots() {
    const auto capture = [](std::string_view text) {
        auto program = Parser(Lexer(text).scan()).parse();
        const auto identity = [](FunctionDecl& function, std::string owner) {
            DeclarationIdentity declaration;
            declaration.package = "example";
            declaration.module_path = "original";
            declaration.source_name = function.name;
            declaration.owner_class = std::move(owner);
            declaration.kind = function.is_constructor ? DeclarationIdentity::Kind::Constructor :
                (declaration.owner_class.empty() ? DeclarationIdentity::Kind::Function : DeclarationIdentity::Kind::Method);
            declaration.generic_parameters = function.type_parameters;
            declaration.generic_constraints = function.type_constraints;
            declaration.result_type = function.return_type.name;
            for (const auto& parameter : function.parameters)
                declaration.parameters.push_back({parameter.name,parameter.type.name,parameter.writable,parameter.is_const});
            function.declaration = std::move(declaration);
        };
        for (auto& function : program.functions) identity(function,{});
        for (auto& declaration : program.classes)
            for (auto& method : declaration.methods) identity(method,declaration.name);
        return capture_original_interfaces(program,{"example","original"},text,
            [](const TypeName& type) { return type.name; });
    };
    const std::string source =
        "public import dep = \"./dependency.qui\"\n"
        "private const int LIMIT = 3\n"
        "enum State\n    Ready\n    Value(int)\n"
        "private class Record()\n    const int value = 7\n    private string label = \"\"\n"
        "T identity<T>(T value, int step = LIMIT)\n    return value\n";
    auto original = capture(source);
    require(original.contracts.functions.size() == 2 && original.contracts.enums.size() == 1 &&
            original.contracts.classes.size() == 1 && original.contracts.constants.size() == 1,
            "original public/private declarations were dropped");
    const auto& function = original.contracts.functions[0];
    require(function.summary_state == OriginalSummaryState::NotCheckedGeneric && function.generic_body &&
            function.parameter_defaults[1] && !function.source_summary && !function.source_borrow,
            "unused generic fabricated checked facts or omitted its full original payload");
    require(original.contracts.enums[0].variants[0].name == "Ready" &&
            original.contracts.enums[0].variants[1].payload->syntax == "int" &&
            original.contracts.classes[0].fields[0].is_const &&
            original.contracts.classes[0].fields[1].is_private,
            "ordered enum or field visibility/const contract was omitted");
    auto alternate = capture("// provenance only\n\n" + source);
    require(original.original_tokens.fields == alternate.original_tokens.fields &&
            function.generic_body->fields == alternate.contracts.functions[0].generic_body->fields,
            "comments or blank lines entered original canonical tokens");
    const auto no_final_newline = capture(std::string_view(source).substr(0,source.size() - 1));
    require(original.original_tokens.fields == no_final_newline.original_tokens.fields &&
            function.generic_body->fields == no_final_newline.contracts.functions[0].generic_body->fields,
            "physical final newline changed logical original tokens");
    const auto finalize = [](OriginalModuleInterfaces value, ModuleSelector target) {
        InterfaceSources sources;
        bind_interface_import(value,"dep",target);
        sources.modules.push_back(std::move(value));
        OriginalModuleInterfaces dependency;
        dependency.module = std::move(target);
        dependency.original_tokens = {"quidra-interface-tokens/1",ir::canonical_encode_value(std::int64_t{9}).bytes};
        sources.modules.push_back(std::move(dependency));
        finalize_original_interfaces(sources);
        return sources;
    };
    auto sources = finalize(std::move(original),{"example","dependency"});
    auto moved_path = source;
    moved_path.replace(moved_path.find("./dependency.qui"),std::string("./dependency.qui").size(),"./relocated.qui");
    const auto moved = finalize(capture(moved_path),{"example","dependency"});
    require(sources.modules[0].contracts.generic_environment->fields ==
            moved.modules[0].contracts.generic_environment->fields,
            "import resolution provenance entered the logical dependency environment");
    ir::Module empty;
    const auto fingerprint = fingerprint_module(empty,{"example","original"},sources.modules[0].contracts);
    require(fingerprint.signature && fingerprint.summary && fingerprint.missing.empty(),
            "opaque unused generic contract cannot be fingerprinted");
    auto changed = sources.modules[0].contracts;
    changed.functions[0].generic_body->fields += "body change";
    const auto body = fingerprint_module(empty,{"example","original"},changed);
    require(body.signature != fingerprint.signature && body.summary != fingerprint.summary,
            "unused original generic body change was ignored");
    std::swap(changed.enums[0].variants[0],changed.enums[0].variants[1]);
    require(fingerprint_module(empty,{"example","original"},changed).signature != body.signature,
            "enum tag order did not affect public signature");
    changed.functions[0].generic_body.reset();
    const auto missing = fingerprint_module(empty,{"example","original"},changed);
    require(!missing.signature && !missing.summary,"missing generic body yielded usable fingerprints");
}
} // namespace
int main() {
    try {
        essential_contract_changes();
        original_snapshots();
        std::cout << "Module fingerprints: unavailable inputs and public semantic contracts verified\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
