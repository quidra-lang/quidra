// The descriptor tables and their helpers (rule_registry.hpp).
#include "optimizer/rule_registry.hpp"

#include "quidra/compiler_extension.hpp"
#include "quidra/member_function_names.hpp"

#include <algorithm>
#include <filesystem>
#include <map>

namespace quidra::optimizer {

using ir::Function;
using ir::Module;

namespace {

std::string compiler_safe_name(std::string name) {
    for (auto& c : name) {
        const bool alphanumeric =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9');
        if (!alphanumeric && c != '_') c = '_';
    }
    return name;
}

std::optional<std::string> field_value(const std::map<std::string, std::string>& fields,
                                       const std::string& name) {
    const auto found = fields.find(name);
    if (found == fields.end() || found->second.empty())
        return std::nullopt;
    return found->second;
}

std::vector<long long> parse_shape(const std::string& text) {
    std::vector<long long> shape;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto comma = text.find(',', start);
        const auto end =
            comma == std::string::npos ? text.size() : comma;
        auto token = text.substr(start, end - start);
        const auto first = token.find_first_not_of(" \t\r");
        const auto last = token.find_last_not_of(" \t\r");
        if (first == std::string::npos) return std::vector<long long>{};
        token = token.substr(first, last - first + 1);
        try {
            shape.push_back(std::stoll(token));
        } catch (...) {
            return std::vector<long long>{};
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return shape;
}

} // namespace

bool source_belongs_to_package(
    const std::string& source_file, const std::string& package_root) {
    if (source_file.empty() || package_root.empty()) return false;
    const auto source =
        std::filesystem::path(source_file).lexically_normal();
    const auto root =
        std::filesystem::path(package_root).lexically_normal();
    const auto relative = source.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) return false;
    return *relative.begin() != "..";
}

bool descriptor_trait(
    const std::string& traits, const std::string& expected) {
    std::size_t start = 0;
    while (start <= traits.size()) {
        const auto comma = traits.find(',', start);
        const auto end =
            comma == std::string::npos ? traits.size() : comma;
        auto first = start;
        auto last = end;
        while (first < last &&
               (traits[first] == ' ' || traits[first] == '\t' ||
                traits[first] == '\r')) ++first;
        while (last > first &&
               (traits[last - 1] == ' ' || traits[last - 1] == '\t' ||
                traits[last - 1] == '\r')) --last;
        if (traits.substr(first, last - first) == expected) return true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

std::vector<std::string> descriptor_traits(const std::string& traits) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= traits.size()) {
        const auto comma = traits.find(',', start);
        const auto end =
            comma == std::string::npos ? traits.size() : comma;
        auto first = start;
        auto last = end;
        while (first < last &&
               (traits[first] == ' ' || traits[first] == '\t' ||
                traits[first] == '\r')) ++first;
        while (last > first &&
               (traits[last - 1] == ' ' || traits[last - 1] == '\t' ||
                traits[last - 1] == '\r')) --last;
        if (first < last)
            result.push_back(traits.substr(first, last - first));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return result;
}

bool compiler_safety_trait(const std::string& trait) {
    return trait == "differentiable" ||
           trait == "higher-order" ||
           trait.ends_with("-sensitive") ||
           trait.starts_with("effect:");
}

namespace {

// Whether function is the one an operation table of extension names
// (configured, its `function` field).
bool compiler_operation_matches(
    const Function& function,
    const CompilerExtensionRegistration& extension,
    const std::string& configured) {
    if (configured.empty()) return false;

    // Package imports may be aliased by the consumer, so compiled names cannot
    // be keyed to the manifest package name. Ownership is already proven by
    // source_belongs_to_package(); match the descriptor's source-relative
    // declaration spelling after removing optional compiler/package prefixes.
    std::string relative = configured;
    for (const auto prefix :
         {member_function_name::method_prefix, member_function_name::constructor_prefix}) {
        if (relative.starts_with(prefix)) {
            relative.erase(0, prefix.size());
            break;
        }
    }
    const auto package_prefix = extension.package + ".";
    if (relative.starts_with(package_prefix))
        relative.erase(0, package_prefix.size());
    if (relative.empty()) return false;

    if (function.name == configured ||
        function.name.ends_with("." + relative))
        return true;

    // Generic/overload specializations retain the sanitized source spelling
    // inside their generated name. Restricting candidates to the extension's
    // package root above prevents cross-package collisions.
    const auto marker = "_" + compiler_safe_name(relative) + "_";
    return function.name.find(marker) != std::string::npos;
}

// The operation ids of a fusion table's `operations` list, trimmed, empty
// ones left out.
std::vector<std::string> split_operation_list(const std::string& text) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto comma = text.find(',', start);
        const auto end =
            comma == std::string::npos ? text.size() : comma;
        auto token = text.substr(start, end - start);
        const auto first = token.find_first_not_of(" \t\r");
        const auto last = token.find_last_not_of(" \t\r");
        if (first != std::string::npos)
            result.push_back(token.substr(first, last - first + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return result;
}

} // namespace

bool OperationBindings::call_has_operation(const ir::Call& call,
                                           const std::string& operation) const {
    const auto found = call_operations.find(call.callee);
    return found != call_operations.end() &&
           std::find(
               found->second.begin(), found->second.end(),
               operation) != found->second.end();
}

namespace {

// The bindings, the extension tables and the conditional rules of module
// (rule_registry.hpp: OperationBindings, ExtensionTables and
// RuleRegistry::conditional_rules).
OperationBindings bind_operations(const Module& module) {
    std::unordered_map<std::string, std::vector<std::string>> call_extensions;
    std::unordered_map<std::string, std::vector<std::string>> call_operations;
    std::unordered_map<std::string, std::vector<std::string>> operation_functions;
    std::unordered_map<std::string, std::string> operation_traits;
    std::unordered_map<std::string, std::vector<ExecutionPolicySetter>>
        execution_policy_setters;
    for (const auto& extension : module.compiler_extensions) {
        if (extension.phase != "tensor-region") continue;
        const auto identity = extension.package + "." + extension.name;
        for (const auto& [table, fields] : extension.tables) {
            if (table.starts_with("execution_policy.")) {
                const auto policy =
                    table.substr(std::string("execution_policy.").size());
                const auto configured = fields.find("function");
                if (policy.empty() || configured == fields.end() ||
                    configured->second.empty()) {
                    continue;
                }
                for (const auto& function : module.functions) {
                    if (!source_belongs_to_package(
                            function.source_file, extension.package_root) ||
                        !compiler_operation_matches(
                            function, extension, configured->second)) {
                        continue;
                    }
                    execution_policy_setters[function.name].push_back(
                        ExecutionPolicySetter{identity, policy});
                }
                continue;
            }
            if (!table.starts_with("operation.")) continue;
            const auto configured = fields.find("function");
            const auto traits = fields.find("traits");
            if (configured == fields.end() || configured->second.empty() ||
                traits == fields.end() ||
                !descriptor_trait(traits->second, "pure") ||
                !descriptor_trait(traits->second, "tensor") ||
                descriptor_trait(traits->second, "stateful")) {
                continue;
            }
            const auto operation =
                table.substr(std::string("operation.").size());
            if (operation.empty()) continue;

            for (const auto& function : module.functions) {
                if (function.result.kind != TypeKind::Tensor ||
                    !source_belongs_to_package(
                        function.source_file, extension.package_root) ||
                    !compiler_operation_matches(
                        function, extension, configured->second)) {
                    continue;
                }
                const auto reference = identity + ":" + operation;
                operation_traits[reference] = traits->second;
                call_extensions[function.name].push_back(identity);
                call_operations[function.name].push_back(reference);
                operation_functions[reference].push_back(function.name);
            }
        }
    }
    for (auto& [_, identities] : call_extensions) {
        std::sort(identities.begin(), identities.end());
        identities.erase(
            std::unique(identities.begin(), identities.end()),
            identities.end());
    }
    for (auto& [_, operations] : call_operations) {
        std::sort(operations.begin(), operations.end());
        operations.erase(
            std::unique(operations.begin(), operations.end()),
            operations.end());
    }
    for (auto& [_, functions] : operation_functions) {
        std::sort(functions.begin(), functions.end());
        functions.erase(
            std::unique(functions.begin(), functions.end()),
            functions.end());
    }
    return OperationBindings{std::move(call_extensions), std::move(call_operations),
                             std::move(operation_functions), std::move(operation_traits),
                             std::move(execution_policy_setters)};
}

ExtensionTables index_extension_tables(const Module& module) {
    std::unordered_map<std::string, std::vector<std::string>>
        extension_tables;
    std::unordered_map<std::string, std::vector<FusionPattern>>
        extension_fusions;
    for (const auto& extension : module.compiler_extensions) {
        const auto identity = extension.package + "." + extension.name;
        auto& tables = extension_tables[identity];
        for (const auto& [table, fields] : extension.tables) {
            if (table == "extension") continue;
            tables.push_back(table);
            if (!table.starts_with("fusion.")) continue;
            const auto operations = fields.find("operations");
            if (operations == fields.end()) continue;
            auto names = split_operation_list(operations->second);
            if (names.empty()) continue;
            FusionPattern pattern;
            pattern.reference = identity + ":" + table;
            for (const auto& name : names)
                pattern.operations.push_back(identity + ":" + name);
            if (const auto replacement = fields.find("replacement");
                replacement != fields.end() &&
                !replacement->second.empty()) {
                pattern.replacement =
                    identity + ":" + replacement->second;
            }
            extension_fusions[identity].push_back(std::move(pattern));
        }
        std::sort(tables.begin(), tables.end());
        tables.erase(std::unique(tables.begin(), tables.end()), tables.end());
    }
    // Prefer the longest package-declared chain. A shorter prefix must not
    // consume a graph before a more specific fusion has a chance to match.
    for (auto& [_, patterns] : extension_fusions) {
        std::sort(
            patterns.begin(), patterns.end(),
            [](const FusionPattern& left, const FusionPattern& right) {
                if (left.operations.size() != right.operations.size())
                    return left.operations.size() > right.operations.size();
                return left.reference < right.reference;
            });
    }
    return ExtensionTables{std::move(extension_tables), std::move(extension_fusions)};
}

std::vector<ConditionalRule> parse_conditional_rules(const Module& module) {
    std::vector<ConditionalRule> conditional_rules;
    for (const auto& extension : module.compiler_extensions) {
        if (extension.phase != "tensor-region") continue;
        const auto identity = extension.package + "." + extension.name;
        for (const auto& [table, fields] : extension.tables) {
            std::optional<RuleStage> stage;
            if (table.starts_with("specialization."))
                stage = RuleStage::Specialization;
            else if (table.starts_with("backend."))
                stage = RuleStage::BackendSelection;
            else if (table.starts_with("memory."))
                stage = RuleStage::MemoryReuse;
            else
                continue;

            const auto source = field_value(fields, "operation");
            const auto target = field_value(fields, "replacement");
            if (!source || !target) continue;

            ConditionalRule rule;
            rule.stage = *stage;
            rule.reference = identity + ":" + table;
            rule.extension = identity;
            rule.operation = identity + ":" + *source;
            rule.replacement = identity + ":" + *target;
            rule.constraints.policy = field_value(fields, "policy");
            rule.constraints.dtype = field_value(fields, "dtype");
            if (const auto rank = field_value(fields, "rank")) {
                try {
                    rule.constraints.rank = std::stoll(*rank);
                } catch (...) {
                    continue;
                }
            }
            if (const auto shape = field_value(fields, "shape")) {
                rule.constraints.shape = parse_shape(*shape);
                if (rule.constraints.shape.empty() && !shape->empty()) continue;
            }
            if (const auto device = field_value(fields, "device")) {
                if (*device == "cpu")
                    rule.constraints.device = StaticDevice::Cpu;
                else if (*device == "gpu")
                    rule.constraints.device = StaticDevice::Gpu;
                else
                    continue;
            }
            if (const auto layout = field_value(fields, "layout")) {
                if (*layout == "contiguous")
                    rule.constraints.contiguous = true;
                else if (*layout == "strided")
                    rule.constraints.contiguous = false;
                else
                    continue;
            }
            if (const auto tracked = field_value(fields, "tracked")) {
                if (*tracked == "true")
                    rule.constraints.tracked = true;
                else if (*tracked == "false")
                    rule.constraints.tracked = false;
                else
                    continue;
            }
            if (const auto last_use = field_value(fields, "last_use")) {
                if (*last_use == "true")
                    rule.constraints.last_use = true;
                else if (*last_use == "false")
                    rule.constraints.last_use = false;
                else
                    continue;
            }
            if (const auto owned = field_value(fields, "owned")) {
                if (*owned == "true")
                    rule.constraints.owned = true;
                else if (*owned == "false")
                    rule.constraints.owned = false;
                else
                    continue;
            }
            conditional_rules.push_back(std::move(rule));
        }
    }
    std::sort(
        conditional_rules.begin(), conditional_rules.end(),
        [](const ConditionalRule& left, const ConditionalRule& right) {
            if (left.stage != right.stage) return left.stage < right.stage;
            return left.reference < right.reference;
        });
    return conditional_rules;
}

} // namespace

RuleRegistry RuleRegistry::build(const Module& module) {
    auto bindings = bind_operations(module);
    auto extension_tables = index_extension_tables(module);
    return RuleRegistry(module, std::move(bindings), std::move(extension_tables));
}

const std::vector<ConditionalRule>& RuleRegistry::conditional_rules() const {
    if (!conditional_rules_) conditional_rules_ = parse_conditional_rules(*module_);
    return *conditional_rules_;
}

} // namespace quidra::optimizer
