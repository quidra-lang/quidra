#include "semantics/interface_fingerprint.hpp"
#include "ir/fact_sources.hpp"
#include "platform/sha256.hpp"

#include <algorithm>
#include <tuple>

namespace quidra::semantics {
namespace {
// Every input field needs an explicit signature/summary/lookup role. Growing
// these capture models requires updating that classification in this file.
static_assert(ir::fields::aggregate_arity<InterfaceContracts>() == 15);
static_assert(ir::fields::aggregate_arity<FunctionInterface>() == 12);
DeclarationIdentity original(DeclarationIdentity declaration) {
    declaration.source_file.clear();
    declaration.generic_args.clear();
    declaration.specialization_of.clear();
    return declaration;
}
bool selected(const DeclarationIdentity& declaration, const ModuleSelector& module) {
    return declaration.package == module.package && declaration.module_path == module.module_path;
}
void payload(const CanonicalInterfacePayload& value) {
    if (value.format.empty() || value.fields.empty())
        throw std::invalid_argument("interface payload requires versioned complete fields");
}
ir::Digest digest(std::string_view domain, std::string_view fields) {
    platform::Sha256 hash;
    hash.update(domain); hash.update(fields);
    return {hash.finish()};
}
class Capture {
public:
    explicit Capture(const InterfaceContracts& contracts) : contracts_(contracts) {}
    ModuleFingerprints result;
    bool signature_complete{true}, summary_complete{true};
    void missing(MissingComponent component, const std::string& key, bool signature, bool summary) {
        result.missing.push_back({component,key});
        signature_complete &= !signature;
        summary_complete &= !summary;
    }
    Type type(Type value, const std::string& key, bool signature, bool summary, std::size_t depth = 0) {
        if (depth >= 128) throw std::invalid_argument("interface type nesting exceeds 128");
        const auto normalize = [&](std::string& name) {
            if (name.empty()) return;
            const auto found = contracts_.named_types.find(name);
            if (found == contracts_.named_types.end()) {
                missing(MissingComponent::NamedType,key,signature,summary);
                name.clear(); // This capture cannot yield a usable digest.
            } else name = ir::canonical_encode_value(found->second).bytes;
        };
        normalize(value.class_name); normalize(value.union_name);
        if (value.first) value.first = std::make_shared<Type>(type(*value.first,key,signature,summary,depth + 1));
        for (auto& item : value.cases) item = type(std::move(item),key,signature,summary,depth + 1);
        for (auto& item : value.parameters) item = type(std::move(item),key,signature,summary,depth + 1);
        return value;
    }
    SignatureType type(SignatureType value, const std::string& key) {
        if (value.syntax.empty()) throw std::invalid_argument("interface type lacks original canonical syntax");
        if (value.semantic) value.semantic = type(*value.semantic,key,true,false);
        return value;
    }
    EffectSummary effect(EffectSummary value, const std::string& key) {
        // The entire diagnostic cause is excluded, including its description
        // and optional presence. Remaining SourceSpans are attribution slots.
        value.first_hidden_mutation.reset();
        for (auto& [path,item] : value.reference_shared_reads) { (void)path; item = type(item,key,false,true); }
        for (auto& [path,item] : value.reference_shared_writes) { (void)path; item = type(item,key,false,true); }
        for (auto& [path,item] : value.reference_shared_read_origins) { (void)path; item = type(item,key,false,true); }
        for (auto& [path,item] : value.reference_shared_write_origins) { (void)path; item = type(item,key,false,true); }
        return value;
    }
private:
    const InterfaceContracts& contracts_;
};
// The anchor vector gives public parameter coordinates stable symbol IDs
// before encoding expressions. Every actual ExtentOrigin field is retained;
// expression_axes uses anchor ordinals instead of checker-local symbol IDs.
std::string origins(std::vector<ExtentOrigin> values) {
    std::map<std::int64_t,std::pair<std::size_t,std::size_t>> coordinates;
    for (const auto& value : values) for (const auto& [id,coordinate] : value.expression_axes) {
        const auto [found,inserted] = coordinates.emplace(id,coordinate);
        if (!inserted && found->second != coordinate)
            throw std::invalid_argument("one result extent symbol has conflicting input coordinates");
    }
    std::vector<std::pair<std::pair<std::size_t,std::size_t>,std::int64_t>> ordered;
    for (const auto& [id,coordinate] : coordinates) ordered.push_back({coordinate,id});
    std::sort(ordered.begin(),ordered.end());
    std::map<std::int64_t,std::int64_t> ordinals;
    std::vector<ShapeExtent> anchors;
    for (const auto& [coordinate,id] : ordered) {
        (void)coordinate;
        ordinals.emplace(id,static_cast<std::int64_t>(anchors.size()) + 1);
        anchors.push_back(ShapeExtent::symbol(id));
    }
    for (auto& value : values) {
        decltype(value.expression_axes) mapped;
        for (const auto& [id,coordinate] : value.expression_axes) mapped.emplace(ordinals.at(id),coordinate);
        value.expression_axes = std::move(mapped);
    }
    struct OriginFields { std::vector<ShapeExtent> anchors; std::vector<ExtentOrigin> values; };
    return ir::canonical_encode_value(OriginFields{std::move(anchors),std::move(values)}).bytes;
}
struct SignatureFunction {
    DeclarationIdentity declaration;
    bool is_private{};
    std::optional<ForeignInterface> export_c;
    std::optional<std::string> external_symbol;
    std::vector<std::optional<CanonicalInterfacePayload>> defaults;
    std::optional<CanonicalInterfacePayload> generic_body;
};
struct SummaryInstance {
    std::vector<std::string> arguments;
    bool shape_entry{};
    EffectSummary effects;
    ir::analysis::IrSummaryFacts ir_facts;
    bool pure_tensor{};
    std::vector<BorrowParameterContract> borrow;
    std::string result_extents;
    std::optional<InitializationContract> initialization;
};
struct SummaryFunction {
    DeclarationIdentity declaration;
    OriginalSummaryState state{OriginalSummaryState::Missing};
    std::optional<CanonicalInterfacePayload> opaque_body;
    std::optional<EffectSummary> source;
    std::optional<std::string> source_extents;
    std::optional<std::vector<BorrowParameterContract>> source_borrow;
    std::optional<InitializationContract> initialization;
    std::map<std::vector<std::string>,InitializationContract> instance_initialization;
    std::vector<std::string> instances; // Full canonical fields, not digests.
};
struct SignatureFields {
    ModuleSelector module;
    std::vector<SignatureFunction> functions;
    std::vector<ClassInterface> classes;
    std::vector<AliasInterface> aliases;
    std::vector<ConstantInterface> constants;
    std::vector<EnumInterface> enums;
    std::vector<PublicImportInterface> reexports;
    std::optional<CanonicalInterfacePayload> generic_environment;
};
struct SummaryFields {
    ModuleSelector module;
    std::vector<SummaryFunction> functions;
    std::optional<CanonicalInterfacePayload> generic_environment;
};
} // namespace

ModuleFingerprints fingerprint_module(const ir::Module& module, const ModuleSelector& selector,
                                      const InterfaceContracts& contracts) {
    Capture capture(contracts);
    if (!contracts.declarations_complete) capture.missing(MissingComponent::DeclarationCatalog,"",true,true);
    if (!contracts.classes_complete) capture.missing(MissingComponent::ClassCatalog,"",true,false);
    if (!contracts.aliases_complete) capture.missing(MissingComponent::AliasCatalog,"",true,false);
    if (!contracts.constants_complete) capture.missing(MissingComponent::ConstantCatalog,"",true,false);
    if (!contracts.enums_complete) capture.missing(MissingComponent::EnumCatalog,"",true,false);
    if (!contracts.reexports_complete) capture.missing(MissingComponent::ReexportCatalog,"",true,false);
    std::map<std::string,DeclarationIdentity> declarations;
    const auto add = [&](const DeclarationIdentity& declaration) {
        if (selected(declaration,selector)) declarations.emplace(declaration_key(declaration),original(declaration));
    };
    for (const auto& declaration : module.declaration_catalog) add(declaration);
    for (const auto& function : module.functions) if (function.declaration && !function.dispatch_stub) add(*function.declaration);
    std::map<std::string,const FunctionInterface*> interfaces;
    for (const auto& contract : contracts.functions) {
        if (!selected(contract.declaration,selector)) continue;
        add(contract.declaration);
        if (!interfaces.emplace(declaration_key(contract.declaration),&contract).second)
            throw std::invalid_argument("duplicate original function interface contract");
    }
    SignatureFields signature{selector,{},{},{},{},{},{},std::nullopt};
    SummaryFields summary{selector,{},std::nullopt};
    bool need_generic_environment = false;
    for (const auto& [key,declaration] : declarations) {
        const auto found = interfaces.find(key);
        if (found == interfaces.end()) {
            capture.missing(MissingComponent::Visibility,key,true,true);
            continue;
        }
        const auto& contract = *found->second;
        if (contract.is_private) continue;
        ++capture.result.exported_functions;
        if (contract.parameter_defaults.size() != declaration.parameters.size())
            throw std::invalid_argument("default argument contract dimensions disagree");
        for (const auto& value : contract.parameter_defaults) if (value) payload(*value);
        if (contract.generic_body) payload(*contract.generic_body);
        const bool generic = !declaration.generic_parameters.empty();
        const bool unchecked = generic && contract.summary_state == OriginalSummaryState::NotCheckedGeneric;
        need_generic_environment |= generic;
        if (generic && !declaration.external && !contract.generic_body)
            capture.missing(MissingComponent::GenericBody,key,true,unchecked);
        SignatureFunction signed_function{declaration,false,contract.export_c,contract.external_symbol,
                                          contract.parameter_defaults,contract.generic_body};
        SummaryFunction summarized{declaration,contract.summary_state,
                                   unchecked ? contract.generic_body : std::nullopt,
                                   std::nullopt,std::nullopt,contract.source_borrow,
                                   contract.initialization,contract.instance_initialization,{}};
        if (contract.source_summary) summarized.source = capture.effect(*contract.source_summary,key);
        if (contract.source_result_extents) summarized.source_extents = origins(*contract.source_result_extents);
        if (!unchecked && !contract.initialization && contract.instance_initialization.empty())
            capture.missing(MissingComponent::Initialization,key,false,true);
        if (generic && !unchecked && !contract.source_summary)
            capture.missing(MissingComponent::GenericSummary,key,false,true);
        std::map<std::string,std::string> instance_fields;
        bool have_effect = contract.source_summary.has_value() || unchecked;
        bool have_borrow = contract.source_borrow.has_value() || unchecked;
        bool have_extents = contract.source_result_extents.has_value() || unchecked;
        std::set<std::string> external_symbols;
        for (const auto& function : module.functions) {
            if (!function.declaration || function.dispatch_stub || !function.source_function.empty() ||
                declaration_key(*function.declaration) != key) continue;
            if (function.external_symbol) external_symbols.insert(*function.external_symbol);
        }
        // Concrete metadata is looked up by existing typed internal bindings,
        // but their generated names and semantic lookup keys never get hashed.
        for (const auto& function : module.functions) {
            if (!function.declaration || function.dispatch_stub || !function.source_function.empty() ||
                declaration_key(*function.declaration) != key) continue;
            const ir::analysis::FunctionSummary* effects = nullptr;
            if (module.summaries) {
                const auto item = module.summaries->by_function.find(function.name);
                if (item != module.summaries->by_function.end()) effects = &item->second;
            }
            if (!effects || !effects->semantic) { capture.missing(MissingComponent::FunctionSummary,key,false,true); continue; }
            have_effect = true;
            if (effects->semantic_parameter_for_ir_parameter.size() != function.parameters.size()) {
                capture.missing(MissingComponent::BorrowContract,key,false,true); continue;
            }
            std::vector<BorrowParameterContract> borrow;
            for (std::size_t i = 0; i < function.parameters.size(); ++i) {
                const auto& parameter = function.parameters[i];
                borrow.push_back({effects->semantic_parameter_for_ir_parameter[i],parameter.writable,
                                  parameter.borrowed,parameter.is_const,parameter.target_escapes});
            }
            have_borrow = true;
            std::optional<std::string> result_extents;
            if (module.fact_sources) {
                const auto fact = module.fact_sources->functions.find(function.name);
                if (fact != module.fact_sources->functions.end()) result_extents = origins(fact->second.result_extents);
            }
            if (!result_extents && contract.source_result_extents) result_extents = origins(*contract.source_result_extents);
            if (!result_extents) { capture.missing(MissingComponent::ResultExtents,key,false,true); continue; }
            have_extents = true;
            auto initialization = contract.initialization;
            if (const auto init = contract.instance_initialization.find(function.declaration->generic_args);
                init != contract.instance_initialization.end()) initialization = init->second;
            if (!initialization) capture.missing(MissingComponent::Initialization,key,false,true);
            SummaryInstance instance{function.declaration->generic_args,function.shape_wrapper,
                capture.effect(*effects->semantic,key),static_cast<const ir::analysis::IrSummaryFacts&>(*effects),
                effects->pure_tensor,std::move(borrow),std::move(*result_extents),std::move(initialization)};
            const auto instance_key = ir::canonical_encode_value(instance.arguments).bytes;
            const auto fields = ir::canonical_encode_value(instance).bytes;
            const auto [old,inserted] = instance_fields.emplace(instance_key,fields);
            if (!inserted && old->second != fields)
                capture.missing(MissingComponent::ConflictingInstances,key,false,true);
        }
        if (!have_effect) capture.missing(MissingComponent::FunctionSummary,key,false,true);
        if (!have_borrow) capture.missing(MissingComponent::BorrowContract,key,false,true);
        if (!have_extents) capture.missing(MissingComponent::ResultExtents,key,false,true);
        if (declaration.external && !signed_function.external_symbol) {
            if (external_symbols.size() == 1) signed_function.external_symbol = *external_symbols.begin();
            else capture.missing(MissingComponent::ExternalSymbol,key,true,false);
        }
        for (auto& [arguments,fields] : instance_fields) { (void)arguments; summarized.instances.push_back(std::move(fields)); }
        signature.functions.push_back(std::move(signed_function));
        summary.functions.push_back(std::move(summarized));
    }
    const auto by_name = [](const auto& a, const auto& b) { return a.source_name < b.source_name; };
    for (auto value : contracts.classes) if (!value.is_private) {
        need_generic_environment |= !value.generic_parameters.empty();
        for (auto& field : value.fields) {
            field.type = capture.type(std::move(field.type),value.source_name);
            if (field.default_value) payload(*field.default_value);
        }
        for (const auto index : value.init_mask_fields) if (index >= value.fields.size())
            throw std::invalid_argument("class initialization mask field is out of range");
        for (auto& [arguments,instance] : value.instances) {
            (void)arguments;
            for (auto& field : instance.fields) field.type = capture.type(std::move(field.type),value.source_name);
            for (const auto index : instance.init_mask_fields) if (index >= instance.fields.size())
                throw std::invalid_argument("class instance initialization mask field is out of range");
        }
        signature.classes.push_back(std::move(value)); ++capture.result.exported_classes;
    }
    for (auto value : contracts.aliases) if (!value.is_private) {
        value.type = capture.type(std::move(value.type),value.source_name);
        signature.aliases.push_back(std::move(value)); ++capture.result.exported_aliases;
    }
    for (auto value : contracts.constants) if (!value.is_private) {
        payload(value.initializer); value.type = capture.type(std::move(value.type),value.source_name);
        signature.constants.push_back(std::move(value)); ++capture.result.exported_constants;
    }
    for (auto value : contracts.enums) if (!value.is_private) {
        for (auto& variant : value.variants) if (variant.payload)
            variant.payload = capture.type(std::move(*variant.payload),value.source_name);
        signature.enums.push_back(std::move(value)); ++capture.result.exported_enums;
    }
    signature.reexports = contracts.reexports;
    capture.result.exported_reexports = signature.reexports.size();
    if (need_generic_environment || contracts.signature_environment_required) {
        if (!contracts.generic_environment)
            capture.missing(MissingComponent::GenericEnvironment,"",true,need_generic_environment);
        else {
            payload(*contracts.generic_environment);
            signature.generic_environment = contracts.generic_environment;
            if (need_generic_environment) summary.generic_environment = contracts.generic_environment;
        }
    }
    std::sort(signature.classes.begin(),signature.classes.end(),by_name);
    std::sort(signature.aliases.begin(),signature.aliases.end(),by_name);
    std::sort(signature.constants.begin(),signature.constants.end(),by_name);
    std::sort(signature.enums.begin(),signature.enums.end(),by_name);
    std::sort(signature.reexports.begin(),signature.reexports.end(),[](const auto& a, const auto& b) { return a.alias < b.alias; });
    const auto unique_names = [](const auto& values) {
        for (std::size_t i = 1; i < values.size(); ++i)
            if (values[i - 1].source_name == values[i].source_name)
                throw std::invalid_argument("duplicate original interface declaration name");
    };
    unique_names(signature.classes); unique_names(signature.aliases); unique_names(signature.constants);
    unique_names(signature.enums);
    for (std::size_t i = 1; i < signature.reexports.size(); ++i)
        if (signature.reexports[i - 1].alias == signature.reexports[i].alias)
            throw std::invalid_argument("duplicate public import alias");
    if (capture.signature_complete)
        capture.result.signature = digest("quidra-module-signature/1",ir::canonical_encode_value(signature).bytes);
    if (capture.summary_complete)
        capture.result.summary = digest("quidra-module-summary/1",ir::canonical_encode_value(summary).bytes);
    std::sort(capture.result.missing.begin(),capture.result.missing.end(),[](const auto& a, const auto& b) {
        return std::tie(a.component,a.declaration) < std::tie(b.component,b.declaration);
    });
    capture.result.missing.erase(std::unique(capture.result.missing.begin(),capture.result.missing.end()),capture.result.missing.end());
    return std::move(capture.result);
}
} // namespace quidra::semantics
