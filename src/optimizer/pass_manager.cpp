// PassManager (pass_manager.hpp) and ir::optimize (quidra/optimizer.hpp).
#include "optimizer/pass_manager.hpp"
#include "quidra/optimizer.hpp"

#include "optimizer/function_index.hpp"
#include "optimizer/pass_context.hpp"
#include "optimizer/pipeline.hpp"
#include "optimizer/rule_registry.hpp"

#include <utility>
#include <variant>

namespace quidra::optimizer {
namespace {

// Runs the group's passes in order; when one reports a rewrite, starts again
// from the first pass.
void rewrite_to_fixpoint(const FixpointGroup& group, ir::Module& module,
                         const PassContext& context) {
    for (const auto& pass : group.passes) {
        if (pass.enabled(context) && pass.run(module, context)) {
            rewrite_to_fixpoint(group, module, context);
            return;
        }
    }
}

} // namespace

ir::Module PassManager::run(ir::Module module) const {
    const auto registry = RuleRegistry::build(module);
    const FunctionIndex functions(module);
    const PassContext context{registry, functions};
    for (const auto& entry : pipeline) {
        if (const auto* group = std::get_if<FixpointGroup>(&entry)) {
            rewrite_to_fixpoint(*group, module, context);
        } else if (const auto& pass = std::get<Pass>(entry); pass.enabled(context)) {
            pass.run(module, context);
        }
    }
    return module;
}

} // namespace quidra::optimizer

namespace quidra::ir {

Module optimize(Module module) {
    return optimizer::PassManager{}.run(std::move(module));
}

} // namespace quidra::ir
