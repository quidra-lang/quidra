#include "semantics/interface_contracts.hpp"
#include "ir/fact_sources.hpp"
#include "quidra/lexer.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace quidra::semantics {
namespace {
struct SemanticToken { TokenKind kind; std::string text; };
class OriginalTokens {
public:
    explicit OriginalTokens(std::string_view text) : tokens_(Lexer(text).scan()), size_(text.size()) {}
    CanonicalInterfacePayload all(const std::vector<ImportDecl>& imports) const {
        std::vector<SourceSpan> excluded;
        for (const auto& import : imports) excluded.push_back(import.span);
        return range(0,size_,true,excluded);
    }
    CanonicalInterfacePayload range(const SourceSpan& span, bool closing = false) const {
        return range(span.start.offset,span.end.offset,closing);
    }
private:
    std::vector<Token> tokens_;
    std::size_t size_;
    CanonicalInterfacePayload range(std::size_t begin, std::size_t end, bool whole = false,
                                   const std::vector<SourceSpan>& excluded = {}) const {
        if (begin > end || end > size_) throw std::invalid_argument("original interface range exceeds source");
        std::vector<SemanticToken> fields;
        for (const auto& token : tokens_) {
            const bool closing = whole && token.span.start.offset == end &&
                (token.kind == TokenKind::Newline || token.kind == TokenKind::Dedent);
            if (token.kind == TokenKind::Eof || token.span.start.offset < begin ||
                (token.span.start.offset >= end && !closing)) continue;
            if (std::any_of(excluded.begin(),excluded.end(),[&](const auto& span) {
                return token.span.start.offset >= span.start.offset && token.span.start.offset < span.end.offset;
            })) continue;
            auto text = token.text;
            if (token.kind == TokenKind::Indent || token.kind == TokenKind::Dedent || token.kind == TokenKind::Newline)
                text.clear();
            fields.push_back({token.kind,std::move(text)});
        }
        return {"quidra-interface-tokens/1",ir::canonical_encode_value(fields).bytes};
    }
};
FunctionInterface function(const FunctionDecl& declaration, bool parent_private, const OriginalTokens& tokens) {
    if (!declaration.declaration) throw std::invalid_argument("original interface capture lacks declaration identity");
    FunctionInterface result;
    result.declaration = *declaration.declaration;
    result.is_private = parent_private || declaration.is_private;
    result.external_symbol = declaration.external_symbol;
    if (declaration.foreign_export)
        result.export_c = ForeignInterface{declaration.foreign_export->abi,declaration.foreign_export->symbol};
    for (const auto& parameter : declaration.parameters)
        result.parameter_defaults.push_back(parameter.default_value ?
            std::optional<CanonicalInterfacePayload>(tokens.range(parameter.default_value->span)) : std::nullopt);
    if (!result.declaration.generic_parameters.empty()) {
        result.summary_state = OriginalSummaryState::NotCheckedGeneric;
        if (!declaration.external_symbol) result.generic_body = tokens.range(declaration.span,true);
    }
    return result;
}
StorageInitializationContract storage(const StorageEffect& value) {
    return {{value.required.begin(),value.required.end()},{value.writes.begin(),value.writes.end()},
            {value.initializes.begin(),value.initializes.end()},{value.invalidates.begin(),value.invalidates.end()}};
}
InitializationContract initialization(const FunctionType& value) {
    InitializationContract result;
    result.receiver = storage(value.receiver_effect);
    for (const auto& [name,effect] : value.reference_effects) result.references.emplace(name,storage(effect));
    result.return_initialized_fields.insert(value.return_initialized_fields.begin(),value.return_initialized_fields.end());
    return result;
}
bool same_module(const DeclarationIdentity& declaration, const ModuleSelector& module) {
    return declaration.package == module.package && declaration.module_path == module.module_path;
}
} // namespace

OriginalModuleInterfaces capture_original_interfaces(const Program& program, const ModuleSelector& module,
                                                     std::string_view source, const CanonicalSourceType& type) {
    if (!type) throw std::invalid_argument("original interface capture needs canonical source type adapter");
    OriginalTokens tokens(source);
    OriginalModuleInterfaces result;
    result.module = module;
    // Import paths are resolution provenance. The environment encodes their
    // aliases, visibility and resolved logical destinations separately.
    result.original_tokens = tokens.all(program.imports);
    auto& contracts = result.contracts;
    contracts.declarations_complete = contracts.classes_complete = contracts.constants_complete =
        contracts.aliases_complete = contracts.enums_complete = true;
    for (const auto& declaration : program.functions) contracts.functions.push_back(function(declaration,false,tokens));
    for (const auto& declaration : program.classes) {
        ClassInterface value;
        value.source_name = declaration.name;
        value.is_private = declaration.is_private;
        value.generic_parameters = declaration.type_parameters;
        value.generic_constraints = declaration.type_constraints;
        value.generic_constraints.resize(value.generic_parameters.size());
        for (const auto& field : declaration.fields)
            value.fields.push_back({field.name,{type(field.type),std::nullopt},field.is_const,field.is_private,
                field.reference,field.default_value ?
                    std::optional<CanonicalInterfacePayload>(tokens.range(field.default_value->span)) : std::nullopt});
        contracts.classes.push_back(std::move(value));
        result.bindings.push_back({InterfaceBindingKind::Class,declaration.name,{}});
        for (const auto& method : declaration.methods)
            contracts.functions.push_back(function(method,declaration.is_private,tokens));
    }
    for (const auto& declaration : program.enums) {
        EnumInterface value{declaration.name,declaration.is_private,{}};
        for (const auto& variant : declaration.variants)
            value.variants.push_back({variant.name,variant.payload ?
                std::optional<SignatureType>(SignatureType{type(*variant.payload),std::nullopt}) : std::nullopt});
        contracts.enums.push_back(std::move(value));
        result.bindings.push_back({InterfaceBindingKind::Enum,declaration.name,{}});
    }
    for (const auto& statement : program.statements) {
        const auto* binding = std::get_if<BindingStmt>(&statement->data);
        if (!binding || !binding->is_const || binding->is_local || !binding->value) continue;
        contracts.constants.push_back({binding->name,binding->is_private,binding->literal_constant,
            {type(binding->declared_type),std::nullopt},tokens.range(binding->value->span)});
        result.bindings.push_back({InterfaceBindingKind::Constant,binding->name,{}});
    }
    for (const auto& import : program.imports) result.imports.push_back({import.alias,import.is_public,std::nullopt});
    contracts.reexports_complete = result.imports.empty();
    // Conservative closed reads for inferred class layouts, initializer
    // references and imported type names. Future capture can narrow this
    // environment without replacing a required dependency by a purity guess.
    contracts.signature_environment_required = !program.imports.empty() || !program.classes.empty() ||
        !contracts.constants.empty();
    const auto dependent_type = [&](const auto& self, const TypeName& value) -> bool {
        if (value.brace_shape || !value.dimension_expressions.empty() || !value.tensor_shape_expressions.empty()) return true;
        return std::any_of(value.arguments.begin(),value.arguments.end(),[&](const auto& item) { return self(self,item); }) ||
            std::any_of(value.function_parameters.begin(),value.function_parameters.end(),[&](const auto& item) { return self(self,item); });
    };
    for (const auto& declaration : program.functions) {
        contracts.signature_environment_required |= dependent_type(dependent_type,declaration.return_type);
        for (const auto& parameter : declaration.parameters)
            contracts.signature_environment_required |= parameter.default_value != nullptr || dependent_type(dependent_type,parameter.type);
    }
    return result;
}
void bind_interface_name(OriginalModuleInterfaces& source, InterfaceBindingKind kind,
                         std::string_view original, std::string qualified) {
    const auto found = std::find_if(source.bindings.begin(),source.bindings.end(),[&](const auto& binding) {
        return binding.kind == kind && binding.original_name == original;
    });
    if (found == source.bindings.end()) throw std::invalid_argument("original interface binding is absent");
    found->qualified_name = std::move(qualified);
}
void bind_interface_import(OriginalModuleInterfaces& source, std::string_view alias, ModuleSelector destination) {
    const auto found = std::find_if(source.imports.begin(),source.imports.end(),[&](const auto& import) { return import.alias == alias; });
    if (found == source.imports.end()) throw std::invalid_argument("original interface import is absent");
    found->destination = std::move(destination);
}
void finalize_original_interfaces(InterfaceSources& sources) {
    for (auto& source : sources.modules) {
        auto& contracts = source.contracts;
        contracts.reexports.clear();
        contracts.reexports_complete = true;
        for (const auto& import : source.imports) if (import.is_public) {
            if (import.destination) contracts.reexports.push_back({import.alias,*import.destination});
            else contracts.reexports_complete = false;
        }
        struct EnvironmentModule {
            ModuleSelector module;
            CanonicalInterfacePayload tokens;
            std::vector<InterfaceImportSource> imports;
        };
        std::vector<EnvironmentModule> environment;
        std::vector<ModuleSelector> visited;
        bool complete = true;
        std::function<void(const OriginalModuleInterfaces&)> append = [&](const auto& module) {
            if (std::find(visited.begin(),visited.end(),module.module) != visited.end()) return;
            visited.push_back(module.module);
            environment.push_back({module.module,module.original_tokens,module.imports});
            for (const auto& import : module.imports) {
                if (!import.destination) { complete = false; continue; }
                const auto found = std::find_if(sources.modules.begin(),sources.modules.end(),[&](const auto& candidate) {
                    return candidate.module == *import.destination;
                });
                if (found == sources.modules.end()) { complete = false; continue; }
                append(*found);
            }
        };
        append(source);
        std::sort(environment.begin(),environment.end(),[](const auto& a, const auto& b) {
            return std::tie(a.module.package,a.module.module_path) < std::tie(b.module.package,b.module.module_path);
        });
        if (complete) contracts.generic_environment = CanonicalInterfacePayload{
            "quidra-interface-environment/1",ir::canonical_encode_value(environment).bytes};
        else contracts.generic_environment.reset();
    }
}

std::shared_ptr<const InterfaceSources> enrich_interface_contracts(
    const InterfaceSources& source, const CheckedProgram& checked, const ir::Module& module) {
    auto result = std::make_shared<InterfaceSources>(source);
    for (auto& original : result->modules) {
        for (auto& contract : original.contracts.functions) {
            for (const auto& function : module.functions) {
                if (!function.declaration || function.dispatch_stub || !function.source_function.empty() ||
                    !same_module(*function.declaration,original.module) ||
                    declaration_key(*function.declaration) != declaration_key(contract.declaration)) continue;
                const auto summary = module.summaries ? module.summaries->by_function.find(function.name) :
                    decltype(module.summaries->by_function)::const_iterator{};
                if (module.summaries && summary != module.summaries->by_function.end() && summary->second.semantic) {
                    const auto information = checked.functions.find(summary->second.semantic_key);
                    if (information != checked.functions.end())
                        contract.instance_initialization[function.declaration->generic_args] = initialization(information->second);
                    if (function.declaration->generic_args.empty() && contract.declaration.generic_parameters.empty()) {
                        contract.source_summary = *summary->second.semantic;
                        contract.summary_state = OriginalSummaryState::Checked;
                        if (information != checked.functions.end()) {
                            contract.initialization = initialization(information->second);
                            contract.source_result_extents = information->second.result_extents;
                        }
                        if (summary->second.semantic_parameter_for_ir_parameter.size() == function.parameters.size()) {
                            std::vector<BorrowParameterContract> borrow;
                            for (std::size_t i = 0; i < function.parameters.size(); ++i) {
                                const auto& parameter = function.parameters[i];
                                borrow.push_back({summary->second.semantic_parameter_for_ir_parameter[i],parameter.writable,
                                    parameter.borrowed,parameter.is_const,parameter.target_escapes});
                            }
                            contract.source_borrow = std::move(borrow);
                        }
                    }
                }
            }
        }
        for (const auto& binding : original.bindings) {
            if (binding.qualified_name.empty()) continue;
            if (binding.kind == InterfaceBindingKind::Class) {
                const auto information = checked.classes.find(binding.qualified_name);
                const auto contract = std::find_if(original.contracts.classes.begin(),original.contracts.classes.end(),
                    [&](const auto& value) { return value.source_name == binding.original_name; });
                if (contract == original.contracts.classes.end()) continue;
                original.contracts.named_types[binding.qualified_name] = {original.module,binding.original_name,{}};
                if (information != checked.classes.end() && contract->generic_parameters.empty()) {
                    if (information->second.fields.size() != contract->fields.size())
                        throw std::invalid_argument("checked original class field dimensions disagree");
                    for (std::size_t i = 0; i < contract->fields.size(); ++i)
                        contract->fields[i].type.semantic = information->second.fields[i].type;
                    if (checked.initialization_masked_classes.contains(binding.qualified_name))
                        for (std::size_t i = 0; i < contract->fields.size(); ++i) contract->init_mask_fields.insert(i);
                }
            } else if (binding.kind == InterfaceBindingKind::Enum) {
                original.contracts.named_types[binding.qualified_name] = {original.module,binding.original_name,{}};
            } else {
                for (const auto& statement : checked.program.statements) {
                    const auto* value = std::get_if<BindingStmt>(&statement->data);
                    if (!value || value->name != binding.qualified_name) continue;
                    const auto type = checked.binding_types.find(statement.get());
                    const auto contract = std::find_if(original.contracts.constants.begin(),original.contracts.constants.end(),
                        [&](const auto& item) { return item.source_name == binding.original_name; });
                    if (type != checked.binding_types.end() && contract != original.contracts.constants.end())
                        contract->type.semantic = type->second;
                }
            }
        }
    }
    // Methods retain typed original identities through class instantiation.
    // The class-parameter prefix is explicit metadata, never a decoded name.
    for (auto& original : result->modules) for (const auto& declaration : checked.program.classes) {
        const DeclarationIdentity* identity = nullptr;
        for (const auto& method : declaration.methods)
            if (method.declaration && same_module(*method.declaration,original.module)) {
                identity = &*method.declaration; break;
            }
        if (!identity) continue;
        const auto contract = std::find_if(original.contracts.classes.begin(),original.contracts.classes.end(),
            [&](const auto& value) { return value.source_name == identity->owner_class; });
        if (contract == original.contracts.classes.end() || contract->generic_parameters.empty()) continue;
        const auto count = contract->generic_parameters.size();
        if (identity->generic_args.size() < count) continue;
        std::vector<std::string> arguments(identity->generic_args.begin(),identity->generic_args.begin() + count);
        original.contracts.named_types[declaration.name] = {original.module,contract->source_name,arguments};
        const auto information = checked.classes.find(declaration.name);
        if (information == checked.classes.end()) continue;
        if (information->second.fields.size() != contract->fields.size())
            throw std::invalid_argument("checked generic class field dimensions disagree");
        ClassInterface::Instance instance{contract->fields,{}};
        for (std::size_t i = 0; i < instance.fields.size(); ++i)
            instance.fields[i].type.semantic = information->second.fields[i].type;
        if (checked.initialization_masked_classes.contains(declaration.name))
            for (std::size_t i = 0; i < instance.fields.size(); ++i) instance.init_mask_fields.insert(i);
        contract->instances[std::move(arguments)] = std::move(instance);
    }
    // Every selected module can refer to original named types of dependencies.
    std::map<std::string,NamedTypeIdentity> names;
    for (const auto& source_module : result->modules)
        names.insert(source_module.contracts.named_types.begin(),source_module.contracts.named_types.end());
    for (auto& destination : result->modules) destination.contracts.named_types = names;
    return result;
}
} // namespace quidra::semantics
