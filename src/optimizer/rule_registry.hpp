#pragma once

// RuleRegistry: the package descriptor tables the optimizer reads, read once
// for a module: which functions implement which package operations
// (OperationBindings), the tables and fusion patterns of each extension
// (ExtensionTables) and the conditional rules (ConditionalRule); and the
// helpers that read descriptor tables. Descriptor values are opaque strings
// to Core: they are compared and split, never given a domain meaning.

#include "optimizer/rule_stage.hpp"
#include "optimizer/static_tensor_facts.hpp"

#include "quidra/ir/module.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace quidra::optimizer {

// A fusion table of an extension (`[fusion.<id>]`): the chain of operations
// it fuses, in order, and the operation that replaces the chain, all as
// `<extension>:<id>` references.
struct FusionPattern {
    std::string reference;
    std::vector<std::string> operations;
    std::optional<std::string> replacement;
};

// An execution-policy table (`[execution_policy.<id>]`) that a function
// implements: a call of the function sets the extension's policy to id.
struct ExecutionPolicySetter {
    std::string extension;
    std::string value;
};

// The package operations the functions of a module implement, read from the
// operation and execution-policy tables of its tensor-region extensions. An
// operation table binds the functions it names that return a tensor, when
// its traits are pure and tensor and not stateful. Operations are
// `<extension>:<id>` references. Like the registry, the bindings are moved,
// never copied.
struct OperationBindings {
    // Function name -> the extensions whose operations it implements
    // (sorted, unique).
    std::unordered_map<std::string, std::vector<std::string>> call_extensions;
    // Function name -> the operations it implements (sorted, unique).
    std::unordered_map<std::string, std::vector<std::string>> call_operations;
    // Operation -> the functions that implement it (sorted, unique).
    std::unordered_map<std::string, std::vector<std::string>> operation_functions;
    // Operation -> its traits list.
    std::unordered_map<std::string, std::string> operation_traits;
    // Function name -> the execution policies a call of it sets.
    std::unordered_map<std::string, std::vector<ExecutionPolicySetter>>
        execution_policy_setters;

    OperationBindings(
        std::unordered_map<std::string, std::vector<std::string>> extensions_by_call,
        std::unordered_map<std::string, std::vector<std::string>> operations_by_call,
        std::unordered_map<std::string, std::vector<std::string>> functions_by_operation,
        std::unordered_map<std::string, std::string> traits_by_operation,
        std::unordered_map<std::string, std::vector<ExecutionPolicySetter>> setters_by_call)
        : call_extensions(std::move(extensions_by_call)),
          call_operations(std::move(operations_by_call)),
          operation_functions(std::move(functions_by_operation)),
          operation_traits(std::move(traits_by_operation)),
          execution_policy_setters(std::move(setters_by_call)) {}

    OperationBindings(const OperationBindings&) = delete;
    OperationBindings& operator=(const OperationBindings&) = delete;
    OperationBindings(OperationBindings&&) = default;
    OperationBindings& operator=(OperationBindings&&) = default;

    // Whether the function call names implements operation.
    bool call_has_operation(const ir::Call& call, const std::string& operation) const;
};

// The descriptor tables of every extension of a module, by extension
// identity: the names of its tables (without `extension`; sorted, unique)
// and its fusion patterns, longest chain first and then by reference, so
// that a longer chain is tried before a prefix of it. Every extension is
// included, whatever its phase. Chain fusion tries the extensions in the
// iteration order of fusions, so the tables are moved, never copied.
struct ExtensionTables {
    std::unordered_map<std::string, std::vector<std::string>> tables;
    std::unordered_map<std::string, std::vector<FusionPattern>> fusions;

    ExtensionTables(
        std::unordered_map<std::string, std::vector<std::string>> tables_by_extension,
        std::unordered_map<std::string, std::vector<FusionPattern>> fusions_by_extension)
        : tables(std::move(tables_by_extension)), fusions(std::move(fusions_by_extension)) {}

    ExtensionTables(const ExtensionTables&) = delete;
    ExtensionTables& operator=(const ExtensionTables&) = delete;
    ExtensionTables(ExtensionTables&&) = default;
    ExtensionTables& operator=(ExtensionTables&&) = default;
};

// Package descriptors may provide executable single-operation replacement
// rules. Core evaluates only generic tensor facts and call contracts;
// operation ids and replacement semantics remain package-owned.
//
//   [specialization.<id>]  shape/dtype/layout/tracking refinement
//   [backend.<id>]         device/layout backend selection
//   [memory.<id>]          last-use/ownership guarded reuse target
//
// Optional constraints are dtype, rank, shape, device, layout, tracked,
// last_use, and owned. An unknown fact never satisfies a constraint.
struct RuleConstraints {
    std::optional<std::string> policy;
    std::optional<std::string> dtype;
    std::optional<long long> rank;
    std::vector<long long> shape;
    std::optional<StaticDevice> device;
    std::optional<bool> contiguous;
    std::optional<bool> tracked;
    std::optional<bool> last_use;
    std::optional<bool> owned;

    // Whether the call `call` of function meets every constraint that is
    // set: policy against the policies known before the call
    // (active_policies, by extension identity), the others against fact,
    // the static facts of its first tensor argument value, and last_use,
    // checked last, against the uses of value after the call. Defined with
    // ConditionalRulePass, which matches the rules.
    bool match(const std::string& extension,
               const std::unordered_map<std::string, std::string>& active_policies,
               const StaticTensorFacts& fact, const ir::Function& function,
               const ir::Instruction& call, ir::ValueId value) const;
};

struct ConditionalRule {
    RuleStage stage{};
    std::string reference;
    std::string extension;
    std::string operation;
    std::string replacement;
    RuleConstraints constraints;
};

// Everything above for one module. The bindings and the extension tables
// are read when the registry is built, the conditional rules when they are
// first asked for, that is when the conditional rule pass first runs. The
// registry holds unordered maps whose iteration order reaches the output,
// so it is moved, never copied; it reads the module's extensions, so the
// module must outlive it.
class RuleRegistry {
public:
    static RuleRegistry build(const ir::Module& module);

    RuleRegistry(const RuleRegistry&) = delete;
    RuleRegistry& operator=(const RuleRegistry&) = delete;
    RuleRegistry(RuleRegistry&&) = default;
    RuleRegistry& operator=(RuleRegistry&&) = default;

    const OperationBindings& bindings() const { return bindings_; }
    const ExtensionTables& extension_tables() const { return extension_tables_; }

    // The conditional rules of the tensor-region extensions, ordered by
    // stage and then by reference. A table without an operation or a
    // replacement, or with a constraint value it cannot read, is left out.
    const std::vector<ConditionalRule>& conditional_rules() const;

private:
    RuleRegistry(const ir::Module& module, OperationBindings bindings,
                 ExtensionTables extension_tables)
        : module_(&module), bindings_(std::move(bindings)),
          extension_tables_(std::move(extension_tables)) {}

    const ir::Module* module_;
    OperationBindings bindings_;
    ExtensionTables extension_tables_;
    mutable std::optional<std::vector<ConditionalRule>> conditional_rules_;
};

// Whether source_file lies inside package_root (lexically).
bool source_belongs_to_package(const std::string& source_file, const std::string& package_root);

// Whether the comma-separated traits list contains expected.
bool descriptor_trait(const std::string& traits, const std::string& expected);

// The traits of a comma-separated list, trimmed, empty ones left out.
std::vector<std::string> descriptor_traits(const std::string& traits);

// Whether a trait is one a replacement must keep (differentiable,
// higher-order, *-sensitive, effect:*).
bool compiler_safety_trait(const std::string& trait);

} // namespace quidra::optimizer
