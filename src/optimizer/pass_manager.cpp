// PassManager (pass_manager.hpp) and ir::optimize (quidra/optimizer.hpp).
#include "optimizer/pass_manager.hpp"
#include "quidra/optimizer.hpp"
#include "quidra/ir/value_allocator.hpp"
#include "ir/analysis/analysis_cache.hpp"
#include "ir/analysis/summary_facts.hpp"
#include "ir/clone_contracts.hpp"
#include "ir/facts/entry.hpp"

#include "optimizer/function_index.hpp"
#include "optimizer/pass_context.hpp"
#include "optimizer/pipeline.hpp"
#include "optimizer/rule_registry.hpp"
#include "optimizer/rule_dispatch.hpp"
#include "optimizer/helper_specialization.hpp"
#include "optimizer/inlining.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <variant>

namespace quidra::optimizer {
namespace {

bool discard_callsite_meets(ir::Module& module) {
    bool changed = false;
    for (auto& function : module.functions)
        if (function.validated_entry && function.validated_entry->origin == ir::facts::EntryFactOrigin::AllDirectCallSites) {
            function.validated_entry.reset();
            changed = true;
        }
    return changed;
}

// Runs the group's passes in order; when one reports a rewrite, starts again
// from the first pass.
bool rewrite_to_fixpoint(const FixpointGroup& group, ir::Module& module,
                         const PassContext& context) {
    bool changed = false;
    for (;;) {
        bool restart = false;
        for (const auto& pass : group.passes) {
            if (pass.enabled(context) && pass.run(module, context)) {
                // A rewrite may introduce another caller. A whole-call-site
                // meet must never become a premise of its own revalidation.
                discard_callsite_meets(module);
                if (context.analyses) context.analyses->invalidate_all();
                changed = restart = true;
                break;
            }
        }
        if (!restart) return changed;
    }
}

void refresh_summaries(ir::Module& module) {
    if (module.summaries) {
        const auto facts = ir::analysis::infer_ir_summary_facts(module, *module.summaries);
        module.summaries = std::make_shared<const ir::analysis::ModuleSummaries>(
            ir::analysis::apply_ir_summary_facts(module, *module.summaries, facts));
    }
}

void import_contracts(ir::Module& module, const ir::OptimizerOptions& options,
                      std::span<const ir::CloneReference> clones,
                      std::span<const ir::SynthesizedFunctionReference> synthesized = {}) {
    if (!module.summaries && !module.fact_sources && !module.binding_origins) return;
    const bool had_summaries = static_cast<bool>(module.summaries);
    const bool had_sources = static_cast<bool>(module.fact_sources);
    const bool had_origins = static_cast<bool>(module.binding_origins);
    auto imported = ir::bind_clone_contracts(module, clones, synthesized);
    if (options.diagnostics)
        for (const auto& issue : imported.unavailable)
            options.diagnostics->push_back({ir::OptimizerDiagnostic::Kind::UnavailableAnalysis,
                issue.function, "clone contract component " + std::to_string(static_cast<unsigned>(issue.component)) +
                " unavailable (reason " + std::to_string(static_cast<unsigned>(issue.reason)) + ")", {}, {}});
    if (had_summaries) module.summaries = std::move(imported.summaries);
    if (had_sources) module.fact_sources = std::move(imported.fact_sources);
    if (had_origins) module.binding_origins = std::move(imported.binding_origins);
}

void report_bindings(const RuleRegistry& registry, const ir::OptimizerOptions& options) {
    if (!options.diagnostics) return;
    for (const auto& difference : registry.binding_differences()) {
        options.diagnostics->push_back({ir::OptimizerDiagnostic::Kind::BindingDifference,
            difference.operation, {}, difference.legacy, difference.exact});
    }
    for (const auto& issue : registry.declarations().issues) {
        options.diagnostics->push_back({ir::OptimizerDiagnostic::Kind::BindingIssue,
            issue.reference, issue.selector + ": " + issue.message, {}, {}});
    }
}

void bind_policy_facts(ir::Module& module, const RuleRegistry& registry) {
    if (registry.declarations().setters.empty()) return;
    auto sources = module.fact_sources ? *module.fact_sources : ir::facts::FactSources{};
    std::map<std::string, std::map<std::string, std::set<std::string>>> values;
    for (const auto& setter : registry.declarations().setters) {
        values[setter.function][setter.extension].insert(setter.policy);
    }
    for (const auto& [function, identities] : values) {
        auto& assignments = sources.policies.setters[function];
        std::erase_if(assignments, [&](const auto& assignment) {
            return identities.contains(assignment.identity);
        });
        for (const auto& [identity, policies] : identities)
            if (policies.size() == 1) assignments.push_back({identity, *policies.begin()});
        sources.policies.writes_policy[function] = true;
    }
    module.fact_sources = std::make_shared<const ir::facts::FactSources>(std::move(sources));
}

} // namespace

bool run_helper_specialization(ir::Module& module, const PassContext& context) {
    if (!context.analyses || !interproc_on(context)) return false;
    bool changed = false;
    std::uint64_t operations = 0;
    constexpr std::uint64_t maximum_operations = 2000000;
    constexpr std::size_t maximum_rounds = 32;
    for (std::size_t round = 0; round < maximum_rounds && operations < maximum_operations; ++round) {
        ir::facts::EntryFactsMap prior;
        for (const auto& function : module.functions)
            if (function.validated_entry) prior.emplace(function.name, function.validated_entry);
        // Specialized clone obligations are fixed; original meets are computed
        // again from the actual current callers with no stale entry premise.
        changed = discard_callsite_meets(module) || changed;
        context.analyses->invalidate_all();
        SpecializationOptions options;
        options.maximum_operations = maximum_operations - operations;
        auto report = HelperSpecialization::run(module, *context.analyses, prior, options);
        operations += report.operations;
        if (context.options.diagnostics)
            for (const auto& unavailable : report.unavailable)
                context.options.diagnostics->push_back({ir::OptimizerDiagnostic::Kind::UnavailableAnalysis,
                    unavailable.function, unavailable.reason, {}, {}});
        for (auto& function : module.functions)
            if (const auto entry = report.entries.find(function.name); entry != report.entries.end())
                function.validated_entry = entry->second;
        changed = report.changed() || changed;
        std::vector<ir::CloneReference> clones;
        for (const auto& origin : report.clones) clones.push_back({origin.clone, origin.source});
        if (!clones.empty()) import_contracts(module, context.options, clones);
        if (report.rewritten_calls || report.generated_clones) {
            refresh_summaries(module);
            context.analyses->invalidate_all();
        } else break;
    }
    return changed;
}

bool run_budgeted_inlining(ir::Module& module, const PassContext& context) {
    if (!interproc_on(context)) return false;
    InliningOptions options;
    std::uint64_t work = 0;
    const auto tick = [&]() {
        if (work == options.maximum_operations) return false;
        ++work;
        return true;
    };
    const auto exhausted = [&]() {
        if (context.options.diagnostics)
            context.options.diagnostics->push_back({ir::OptimizerDiagnostic::Kind::UnavailableAnalysis,
                {}, "inlining candidate budget exhausted", {}, {}});
        return false;
    };
    // A typed reachability candidate selects the larger instruction budget.
    // It does not certify motion, error order, aliasing or a final step region.
    std::map<std::string, std::vector<std::size_t>, std::less<>> names;
    std::vector<bool> candidate(module.functions.size());
    std::vector<std::vector<std::size_t>> callers(module.functions.size());
    std::vector<std::size_t> pending;
    const auto mark = [&](std::size_t function) {
        if (!candidate[function]) {
            candidate[function] = true;
            pending.push_back(function);
        }
    };
    for (std::size_t f = 0; f < module.functions.size(); ++f) {
        if (!tick()) return exhausted();
        names[module.functions[f].name].push_back(f);
    }
    for (std::size_t f = 0; f < module.functions.size(); ++f) {
        const auto& fn = module.functions[f];
        if (fn.result.kind == TypeKind::Tensor ||
            std::any_of(fn.parameters.begin(), fn.parameters.end(), [](const auto& p) { return p.type.kind == TypeKind::Tensor; }))
            mark(f);
        if (const auto* summary = module.summaries ? module.summaries->find(fn.name) : nullptr;
            summary && summary->effects_complete && (summary->effects &
                (ir::analysis::effect_bit::DeviceWork | ir::analysis::effect_bit::MayFailDeferred)))
            mark(f);
        for (const auto& block : fn.blocks) for (const auto& instruction : block.instructions) {
            if (!tick()) return exhausted();
            if (const auto* call = std::get_if<ir::Call>(&instruction)) {
                const auto target = names.find(call->callee);
                if (target != names.end() && target->second.size() == 1)
                    callers[target->second.front()].push_back(f);
                if (call->result.kind == TypeKind::Tensor) mark(f);
            }
            const bool tensor = std::visit([](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (ir::InstructionTraits<T>::domain == ir::Domain::tensor ||
                              ir::InstructionTraits<T>::domain == ir::Domain::autograd) return true;
                else if constexpr (std::is_same_v<T, ir::Unary>) return node.type.kind == TypeKind::Tensor;
                else if constexpr (std::is_same_v<T, ir::Binary>) return node.result_type.kind == TypeKind::Tensor;
                else return false;
            }, instruction);
            if (tensor) mark(f);
        }
    }
    for (std::size_t p = 0; p < pending.size(); ++p)
        for (const auto caller : callers[pending[p]]) {
            if (!tick()) return exhausted();
            mark(caller);
        }
    for (const auto& fn : module.functions)
        for (std::size_t b = 0; b < fn.blocks.size(); ++b)
            for (std::size_t i = 0; i < fn.blocks[b].instructions.size(); ++i) {
                if (!tick()) return exhausted();
                const auto* call = std::get_if<ir::Call>(&fn.blocks[b].instructions[i]);
                if (!call) continue;
                const auto target = names.find(call->callee);
                if (target != names.end() && target->second.size() == 1 && candidate[target->second.front()])
                    options.hot_sites.push_back({fn.name, b, i});
            }
    options.maximum_operations -= work;
    const auto report = BudgetedInliner::run(module, context.registry, std::move(options));
    if (context.options.diagnostics)
        for (const auto& issue : report.unavailable)
            context.options.diagnostics->push_back({ir::OptimizerDiagnostic::Kind::UnavailableAnalysis,
                issue.caller, issue.callee + ": " + issue.reason, {}, {}});
    return report.changed();
}

bool run_rule_dispatch(ir::Module& module, const PassContext& context) {
    const auto report = RuleDispatchPass::run(module, context.registry, context.analyses);
    if (context.options.diagnostics) {
        for (const auto& unavailable : report.unavailable) {
            context.options.diagnostics->push_back({ir::OptimizerDiagnostic::Kind::UnavailableRule,
                unavailable.reference, unavailable.reason, {}, {}});
        }
    }
    return report.changed();
}

ir::Module PassManager::run(ir::Module module) const {
    // Also normalize imported or hand-built modules at the boundary.
    for (auto& function : module.functions) ir::seed_value_counter(function);
    refresh_summaries(module);
    auto registry = RuleRegistry::build(module, options_.exact_binding);
    bind_policy_facts(module, registry);
    FunctionIndex functions(module);
    report_bindings(registry, options_);
    ir::analysis::AnalysisCache analyses(module);
    const PassContext context{registry, functions, options_, &analyses};
    for (const auto& entry : pipeline) {
        const auto function_count = module.functions.size();
        bool changed = false;
        bool contracts_changed = false;
        bool entries_validated = false;
        if (const auto* group = std::get_if<FixpointGroup>(&entry)) {
            changed = rewrite_to_fixpoint(*group, module, context);
            contracts_changed = changed;
        } else if (const auto& pass = std::get<Pass>(entry); pass.enabled(context)) {
            changed = pass.run(module, context);
            entries_validated = pass.run == run_helper_specialization;
            contracts_changed = changed && (pass.run == run_rule_dispatch || pass.run == run_helper_specialization || pass.run == run_budgeted_inlining ||
                module.functions.size() != function_count);
        }
        if (changed) {
            analyses.invalidate_all();
        }
        if (contracts_changed) {
            if (!entries_validated) discard_callsite_meets(module);
            std::vector<ir::SynthesizedFunctionReference> synthesized;
            if (module.summaries)
                for (std::size_t index = function_count; index < module.functions.size(); ++index)
                    if (!module.summaries->find(module.functions[index].name))
                        synthesized.push_back({module.functions[index].name});
            if (!synthesized.empty()) import_contracts(module, options_, {}, synthesized);
            for (auto& function : module.functions) ir::seed_value_counter(function);
            refresh_summaries(module);
            registry = RuleRegistry::build(module, options_.exact_binding);
            bind_policy_facts(module, registry);
            functions = FunctionIndex(module);
            if (!entries_validated && interproc_on(context) && run_helper_specialization(module, context)) {
                analyses.invalidate_all();
                registry = RuleRegistry::build(module, options_.exact_binding);
                bind_policy_facts(module, registry);
                functions = FunctionIndex(module);
            }
        }
    }
    return module;
}

} // namespace quidra::optimizer

namespace quidra::ir {

Module optimize(Module module, OptimizerOptions options) {
    return optimizer::PassManager{options}.run(std::move(module));
}

} // namespace quidra::ir
