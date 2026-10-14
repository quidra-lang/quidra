#include "optimizer/declaration_binding.hpp"
#include "ir/facts/entry.hpp"
#include "quidra/diagnostic.hpp"
#include "quidra/member_function_names.hpp"

#include <algorithm>
#include <set>
#include <string_view>
#include <tuple>

namespace quidra::optimizer {
namespace {
using Kind = CompilerExtensionTableKind;
using Strings = std::vector<std::string>;
using StringMap = std::map<std::string, std::string>;
std::string compact(std::string_view input) {
    std::string result;
    for (char c : input) if (c != ' ' && c != '\t' && c != '\r' && c != '\n') result += c;
    return result;
}
[[noreturn]] void error(const CompilerExtensionRegistration& extension, std::string_view table,
                        std::string message) {
    throw CompileError({"PACKAGE_COMPILER_EXTENSION", extension.package + "." + extension.name +
        ":" + std::string(table) + ": " + std::move(message), {}});
}
std::string generic_name(const DeclarationIdentity& identity) {
    std::string result;
    if (!identity.generic_parameters.empty()) {
        result += '<';
        for (std::size_t i = 0; i < identity.generic_parameters.size(); ++i) {
            if (i) result += ',';
            result += identity.generic_parameters[i];
            if (i < identity.generic_constraints.size() && !identity.generic_constraints[i].empty())
                result += ':' + compact(identity.generic_constraints[i]);
        }
        result += '>';
    }
    return result;
}
std::string source_spelling(const DeclarationIdentity& identity) {
    std::string result = identity.module_path;
    if (!result.empty()) result += '.';
    if (!identity.owner_class.empty()) result += identity.owner_class + '.';
    result += identity.source_name;
    return result;
}
bool matches(const CompilerDeclarationSelector& selector, const DeclarationIdentity& identity,
             const std::string& package, bool legacy) {
    if (identity.package != package || static_cast<unsigned>(selector.kind) != static_cast<unsigned>(identity.kind)) return false;
    auto name = compact(selector.name);
    for (const auto prefix : {member_function_name::method_prefix, member_function_name::constructor_prefix})
        if (name.starts_with(prefix)) { name.erase(0, prefix.size()); break; }
    if ((legacy || selector.kind != CompilerDeclarationSelector::Kind::Function) && name.starts_with(package + '.'))
        name.erase(0, package.size() + 1);
    const auto spelling = source_spelling(identity);
    if (name != spelling && name != spelling + generic_name(identity)) return false;
    return !selector.signature || compact(*selector.signature) == compact(identity.signature);
}
std::string abstract_type(std::string_view input, const DeclarationIdentity& identity) {
    std::string result;
    for (std::size_t start = 0; start < input.size();) {
        const char c = input[start];
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!letter) { if (c != ' ' && c != '\t') result += c; ++start; continue; }
        auto end = start + 1;
        while (end < input.size() && ((input[end] >= 'a' && input[end] <= 'z') ||
               (input[end] >= 'A' && input[end] <= 'Z') || (input[end] >= '0' && input[end] <= '9') || input[end] == '_')) ++end;
        const auto token = input.substr(start, end - start);
        const auto found = std::find(identity.generic_parameters.begin(), identity.generic_parameters.end(), token);
        if (found == identity.generic_parameters.end()) result += token;
        else result += "@generic" + std::to_string(found - identity.generic_parameters.begin());
        start = end;
    }
    return result;
}
bool contracts_match(const DeclarationIdentity& left, const DeclarationIdentity& right) {
    if (left.parameters.size() != right.parameters.size() || left.mutation_marker != right.mutation_marker ||
        left.generic_constraints != right.generic_constraints || left.generic_parameters.size() != right.generic_parameters.size()) return false;
    for (std::size_t i = 0; i < left.parameters.size(); ++i) {
        const auto& a = left.parameters[i]; const auto& b = right.parameters[i];
        if (a.writable != b.writable || a.is_const != b.is_const ||
            abstract_type(a.type, left) != abstract_type(b.type, right)) return false;
    }
    return abstract_type(left.result_type, left) == abstract_type(right.result_type, right);
}
std::vector<DeclarationIdentity> resolve(const CompilerExtensionRegistration& extension,
    std::string_view table, const std::vector<CompilerDeclarationSelector>& selectors,
    const std::vector<DeclarationIdentity>& catalog, bool strict) {
    std::vector<DeclarationIdentity> result;
    for (const auto& selector : selectors) {
        std::vector<DeclarationIdentity> found;
        for (const auto& declaration : catalog) if (matches(selector, declaration, extension.package, !strict) &&
            std::none_of(found.begin(), found.end(), [&](const auto& other) { return same_declaration(declaration, other); }))
            found.push_back(declaration);
        if (found.empty()) error(extension, table, "selector '" + selector.spelling + "' names no declaration");
        if (strict && found.size() != 1) {
            std::string message = "ambiguous selector '" + selector.spelling + "'; candidates:";
            Strings candidates;
            for (const auto& declaration : found) candidates.push_back(source_spelling(declaration) + generic_name(declaration) + declaration.signature);
            std::sort(candidates.begin(), candidates.end());
            for (const auto& candidate : candidates) message += " " + candidate;
            error(extension, table, std::move(message));
        }
        for (auto& declaration : found) if (std::none_of(result.begin(), result.end(),
            [&](const auto& other) { return same_declaration(declaration, other); })) result.push_back(std::move(declaration));
    }
    if (strict) for (std::size_t i = 1; i < result.size(); ++i)
        if (!contracts_match(result.front(), result[i])) error(extension, table, "selected declarations have incompatible parameter/result contracts");
    return result;
}
bool tensor_type(std::string_view type) { return type.starts_with("tensor<"); }
void validate_contract_fields(const CompilerExtensionRegistration& extension, const BoundOperation& operation) {
    const auto& fields = operation.contract.fields;
    for (const auto& declaration : operation.declarations) {
        const auto parameter = [&](const std::string& name) -> const DeclarationParameter* {
            const auto found = std::find_if(declaration.parameters.begin(), declaration.parameters.end(),
                [&](const auto& p) { return p.name == name; });
            return found == declaration.parameters.end() ? nullptr : &*found;
        };
        const auto tensor = [&](const std::string& name, std::string_view key, bool writable = false) {
            if (name == "result") {
                if (!tensor_type(declaration.result_type) && key != "writes") error(extension, operation.contract.id + "." + std::string(key), "result is not a tensor");
                return;
            }
            const auto* p = parameter(name);
            if (!p || !tensor_type(p->type) || (writable && !p->writable)) error(extension, "operation." + operation.contract.id + "." + std::string(key), "'" + name + "' is not an eligible tensor parameter");
        };
        for (const auto key : {"data", "reads"}) if (const auto found = fields.find(key); found != fields.end())
            for (const auto& name : std::get<Strings>(found->second)) tensor(name, key);
        if (const auto found = fields.find("writes"); found != fields.end()) for (const auto& [name, mode] : std::get<StringMap>(found->second)) {
            (void)mode; tensor(name, "writes", name != "result");
        }
        if (const auto found = fields.find("inplace"); found != fields.end()) for (const auto& [name, result] : std::get<StringMap>(found->second)) {
            tensor(name, "inplace"); tensor(result, "inplace");
        }
    }
}
std::map<std::string, BoundOperation, std::less<>> full_operations(
    const std::vector<CompilerExtensionRegistration>& extensions, const std::vector<DeclarationIdentity>& catalog) {
    std::map<std::string, BoundOperation, std::less<>> result;
    for (const auto& extension : extensions) {
        if (extension.schema_revision != 2) continue;
        if (!extension.model) error(extension, "extension", "full descriptor model is unavailable");
        const auto prefix = extension.package + '.' + extension.name;
        for (const auto& [name, table] : extension.model->tables) {
            if (table.kind != Kind::Operation) continue;
            BoundOperation operation;
            operation.reference = prefix + ':' + table.id; operation.extension = prefix; operation.contract = table;
            operation.declarations = resolve(extension, name,
                std::get<std::vector<CompilerDeclarationSelector>>(table.fields.at("function")), catalog, true);
            validate_contract_fields(extension, operation);
            if (!result.emplace(operation.reference, std::move(operation)).second) error(extension, name, "duplicate extension operation identity");
        }
    }
    for (const auto& extension : extensions) {
        if (extension.schema_revision != 2) continue;
        const auto prefix = extension.package + '.' + extension.name;
        for (const auto& [name, table] : extension.model->tables) {
            if (table.kind == Kind::ExecutionPolicy || table.kind == Kind::Query) {
                (void)resolve(extension, name, std::get<std::vector<CompilerDeclarationSelector>>(table.fields.at("function")), catalog, true);
                continue;
            }
            if (table.kind != Kind::Kernel && table.kind != Kind::Derivative &&
                table.kind != Kind::Specialization && table.kind != Kind::Backend && table.kind != Kind::Memory) continue;
            const auto& operation = result.at(prefix + ':' + std::get<std::string>(table.fields.at("operation")));
            for (const auto& declaration : operation.declarations) {
                const auto parameter = [&](const std::string& value) -> const DeclarationParameter* {
                    const auto found = std::find_if(declaration.parameters.begin(), declaration.parameters.end(),
                        [&](const auto& p) { return p.name == value; });
                    return found == declaration.parameters.end() ? nullptr : &*found;
                };
                const auto tensor = [&](const std::string& value, const std::string& key) {
                    const auto* p = parameter(value);
                    if (!p || !tensor_type(p->type)) error(extension, name + '.' + key, "'" + value + "' must name a tensor parameter");
                };
                if (table.kind == Kind::Kernel || table.kind == Kind::Derivative) {
                    if (declaration.kind != DeclarationIdentity::Kind::Function) error(extension, name, "requires explicit free-function surface");
                    const auto effects = operation.contract.fields.find("effects");
                    if (effects != operation.contract.fields.end()) {
                        const auto& values = std::get<Strings>(effects->second);
                        if (std::find(values.begin(), values.end(), "reads-state") != values.end())
                            error(extension, name, "hidden-state reads violate explicit surface");
                    }
                    const auto layout_key = table.kind == Kind::Kernel ? "params" : "metadata";
                    if (const auto found = table.fields.find(layout_key); found != table.fields.end())
                        for (const auto& field : std::get<std::vector<CompilerPodField>>(found->second)) {
                            const auto* p = parameter(field.name);
                            if (!p || tensor_type(p->type) || p->writable)
                                error(extension, name + '.' + layout_key, "POD field must name a scalar value parameter");
                        }
                }
                if (table.kind == Kind::Derivative) {
                    for (const auto& value : std::get<Strings>(table.fields.at("differentiable"))) tensor(value, "differentiable");
                    std::set<std::string> extras;
                    for (const auto& [kernel_name, kernel] : extension.model->tables) {
                        (void)kernel_name;
                        if (kernel.kind != Kind::Kernel || std::get<std::string>(kernel.fields.at("operation")) != operation.contract.id) continue;
                        if (const auto found = kernel.fields.find("outputs_extra"); found != kernel.fields.end())
                            for (const auto& [value, role] : std::get<StringMap>(found->second)) { (void)role; extras.insert(value); }
                    }
                    for (const auto& value : std::get<Strings>(table.fields.at("saves"))) {
                        if (value == "result") {
                            if (!tensor_type(declaration.result_type)) error(extension, name + ".saves", "result is not a tensor");
                        } else if (!extras.contains(value)) tensor(value, "saves");
                    }
                } else if (table.kind == Kind::Kernel) {
                    std::set<std::string> extras;
                    if (const auto found = table.fields.find("outputs_extra"); found != table.fields.end())
                        for (const auto& [value, role] : std::get<StringMap>(found->second)) {
                            (void)role;
                            if (value == "result" || parameter(value)) error(extension, name + ".outputs_extra", "extra output collides with a parameter or result");
                            extras.insert(value);
                        }
                    for (const auto& value : std::get<Strings>(table.fields.at("tensors"))) {
                        if (value == "result") {
                            if (!tensor_type(declaration.result_type)) error(extension, name + ".tensors", "result is not a tensor");
                        } else if (!extras.contains(value)) tensor(value, "tensors");
                    }
                } else if (const auto found = table.fields.find("same_shape"); found != table.fields.end())
                    for (const auto& value : std::get<Strings>(found->second)) tensor(value, "same_shape");
            }
        }
    }
    return result;
}
} // namespace

void validate_extension_declarations(const std::vector<CompilerExtensionRegistration>& extensions,
                                     const std::vector<DeclarationIdentity>& catalog) {
    (void)full_operations(extensions, catalog);
}
DeclarationBindings bind_declarations(const ir::Module& module, DeclarationBindingMode mode) {
    DeclarationBindings result;
    result.operations = full_operations(module.compiler_extensions, module.declaration_catalog);
    for (const auto& extension : module.compiler_extensions) {
        const auto prefix = extension.package + '.' + extension.name;
        const bool full = extension.schema_revision == 2;
        if (!full && mode == DeclarationBindingMode::Off) continue;
        for (const auto& [name, raw] : extension.tables) {
            const bool setter = name.starts_with("execution_policy.");
            const bool operation = name.starts_with("operation.");
            if (!setter && !operation) continue;
            if (!raw.contains("function")) continue;
            std::vector<CompilerDeclarationSelector> selectors;
            if (full) selectors = std::get<std::vector<CompilerDeclarationSelector>>(extension.model->tables.at(name).fields.at("function"));
            else {
                CompilerDeclarationSelector selector;
                selector.spelling = selector.name = raw.at("function");
                if (selector.name.starts_with(member_function_name::method_prefix)) selector.kind = CompilerDeclarationSelector::Kind::Method;
                else if (selector.name.starts_with(member_function_name::constructor_prefix)) selector.kind = CompilerDeclarationSelector::Kind::Constructor;
                selectors.push_back(std::move(selector));
            }
            std::vector<DeclarationIdentity> declarations;
            try { declarations = resolve(extension, name, selectors, module.declaration_catalog, full); }
            catch (const CompileError& e) {
                if (full) throw;
                result.issues.push_back({prefix + ':' + name, raw.at("function"), e.what()}); continue;
            }
            Strings functions;
            for (const auto& function : module.functions) {
                // A caller-fact clone has a stronger entry obligation. Ordinary
                // operation/setter bindings may enter only the original ABI.
                if ((function.validated_entry && function.validated_entry->origin == ir::facts::EntryFactOrigin::SpecializedCalls) ||
                    !function.declaration || (!full && operation && function.result.kind != TypeKind::Tensor)) continue;
                if (std::any_of(declarations.begin(), declarations.end(), [&](const auto& identity) { return same_declaration(identity, *function.declaration); }))
                    {
                        if (full && std::count_if(module.functions.begin(), module.functions.end(),
                            [&](const auto& candidate) { return candidate.name == function.name; }) != 1)
                            error(extension, name, "ambiguous lowered function target '" + function.name + "'");
                        functions.push_back(function.name);
                    }
            }
            std::sort(functions.begin(), functions.end()); functions.erase(std::unique(functions.begin(), functions.end()), functions.end());
            if (setter) {
                const auto policy = name.substr(std::string("execution_policy.").size());
                for (const auto& function : functions) result.setters.push_back({function, prefix, policy});
            } else {
                const auto id = name.substr(std::string("operation.").size());
                const auto reference = prefix + ':' + id;
                if (full) result.operations.at(reference).functions = std::move(functions);
                else if (raw.contains("traits") && ("," + compact(raw.at("traits")) + ",").find(",tensor,") != std::string::npos &&
                    ("," + compact(raw.at("traits")) + ",").find(",pure,") != std::string::npos &&
                    ("," + compact(raw.at("traits")) + ",").find(",stateful,") == std::string::npos) {
                    BoundOperation bound; bound.reference = reference; bound.extension = prefix;
                    bound.declarations = std::move(declarations); bound.functions = std::move(functions);
                    bound.contract.kind = Kind::Operation; bound.contract.id = id;
                    bound.contract.fields["traits"] = raw.at("traits");
                    result.operations.emplace(reference, std::move(bound));
                }
            }
        }
    }
    return result;
}
} // namespace quidra::optimizer
