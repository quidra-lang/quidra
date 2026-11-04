#include "quidra/checker.hpp"
#include "quidra/language.hpp"
#include <algorithm>
#include <functional>
#include <set>

namespace quidra {
void Checker::validate_parallel_for(const Stmt& statement,const ForStmt& loop,
    const std::unordered_map<std::string,Type>& outer) {
    const auto reject=[&](std::string message,SourceSpan span) {
        error("PARALLEL_FOR",std::move(message),span);
    };
    const auto scalar=[](const Type& type) {
        return is_fixed_integer(type)||is_bare_integer(type)||
            type.kind==TypeKind::Bool||type.kind==TypeKind::Real16||
            type.kind==TypeKind::Real16b||type.kind==TypeKind::Real32||type.kind==TypeKind::Real64;
    };
    if(loop.writable||loop.pattern||loop.index||!loop.declares||loop.legacy)
        reject("parallel for requires declared value bindings without references, patterns or an index binding.",statement.span);
    const auto item=binding_types_.at(&statement);
    if(loop.range) {
        if(!is_integer_family_type(item))
            reject("parallel for initially supports integer ranges; real recurrence needs an explicit rounding proof.",statement.span);
    } else if(raw_types_.at(loop.iterable.get()).kind!=TypeKind::Array)
        reject("parallel for initially supports array value snapshots, not bins or structural traversal.",statement.span);
    if(!scalar(item))reject("parallel for requires scalar iteration values.",statement.span);
    for(const auto& extra:loop.collections)
        if(extra.index||!extra.declares||!scalar(*raw_types_.at(extra.iterable.get()).first))
            reject("Every parallel collection binding must declare a scalar value without an index binding.",extra.span);
    CheckedProgram::ParallelForPlan plan;
    plan.worker_arithmetic=true;
    std::set<std::string> captures,outputs,locals;
    locals.insert(loop.name);
    for(const auto& extra:loop.collections)locals.insert(extra.name);
    // Discover every output first: reading it anywhere in this body would
    // introduce an inter-iteration dependence, even before its first write.
    std::function<void(const std::vector<StmtPtr>&)> discover;
    discover=[&](const auto& body) {
        for(const auto& stmt:body) {
            if(const auto* assign=std::get_if<AssignStmt>(&stmt->data)) {
                if(const auto* index=std::get_if<IndexExpr>(&assign->target_expr()->data)) {
                    const auto* root=std::get_if<NameExpr>(&index->base->data);
                    const auto* coordinate=index->items.size()==1 && index->items[0].index
                        ?std::get_if<NameExpr>(&index->items[0].index->data):nullptr;
                    if(!loop.range||!root||!coordinate||root->this_qualifier||root->self_reference||
                       coordinate->this_qualifier||coordinate->self_reference||
                       coordinate->name!=loop.name||index->items[0].slice||
                       !outer.contains(root->name)||reference_paths_.contains(root->name)||borrowed_.contains(root->name)||
                       !unknown_reference_targets_.empty()||
                       std::any_of(reference_paths_.begin(),reference_paths_.end(),[&](const auto& alias){return alias.second.first==root->name;}))
                        reject("A shared write must be a direct array element indexed by this integer iteration value.",stmt->span);
                    const auto& type=outer.at(root->name);
                    if(type.kind!=TypeKind::Array||!scalar(*type.first)||
                       (is_bare_integer(*type.first)&&
                        (!std::holds_alternative<NameExpr>(assign->value->data)||
                         std::get<NameExpr>(assign->value->data).name!=loop.name||*type.first!=item)))
                        reject("Shared output elements require a fixed scalar, or the unchanged inline integer iteration value.",stmt->span);
                    if(!assign->compound_op.empty())reject("A parallel output cannot read its previous element value.",stmt->span);
                    if(!outputs.insert(root->name).second)
                        reject("Each parallel output requires one syntactic write per iteration.",stmt->span);
                }
            }
            if(const auto* branch=std::get_if<IfStmt>(&stmt->data)){discover(branch->then_body);discover(branch->else_body);}
            if(const auto* nested=std::get_if<WhileStmt>(&stmt->data))discover(nested->body);
            if(const auto* nested=std::get_if<ForStmt>(&stmt->data))discover(nested->body);
        }
    };
    discover(loop.body);
    std::function<void(const Expr&)> expression;
    expression=[&](const Expr& expr) {
        if(const auto* name=std::get_if<NameExpr>(&expr.data)) {
            if(name->this_qualifier||name->self_reference)
                reject("A parallel iteration cannot capture the current instance or one of its fields.",expr.span);
            if(outputs.contains(name->name))reject("A parallel output cannot also be read or aliased by an iteration.",expr.span);
            if(outer.contains(name->name)&&!locals.contains(name->name)) {
                if(!scalar(outer.at(name->name))||reference_paths_.contains(name->name))
                    reject("A parallel capture requires a scalar value with no reference alias.",expr.span);
                captures.insert(name->name);
            }
            return;
        }
        if(std::holds_alternative<IntegerExpr>(expr.data)||std::holds_alternative<RealLiteralExpr>(expr.data)||
           std::holds_alternative<BoolExpr>(expr.data)||std::holds_alternative<LiteralConstantExpr>(expr.data))return;
        if(const auto* unary=std::get_if<UnaryExpr>(&expr.data)) {
            expression(*unary->operand);
            if(is_bare_integer(raw_types_.at(&expr)))plan.worker_arithmetic=false;
            return;
        }
        if(const auto* chain=std::get_if<ComparisonChainExpr>(&expr.data)) {
            for(const auto& operand:chain->operands)expression(*operand);
            return;
        }
        if(const auto* binary=std::get_if<BinaryExpr>(&expr.data)) {
            expression(*binary->left);expression(*binary->right);
            if(is_bare_integer(raw_types_.at(&expr)))plan.worker_arithmetic=false;
            return;
        }
        if(const auto* call=std::get_if<CallExpr>(&expr.data)) {
            const auto resolution=call_resolutions_.find(&expr);
            if(resolution!=call_resolutions_.end() && resolution->second.kind==CallKind::NumericCast && scalar(expr_types_.at(&expr))) {
                for(const auto& argument:call->args) {
                    if(argument.writable)reject("A parallel scalar conversion cannot borrow writable storage.",expr.span);
                    expression(*argument.value);
                }
                if(is_bare_integer(expr_types_.at(&expr)))plan.worker_arithmetic=false;
                return;
            }
            if(resolution!=call_resolutions_.end() && resolution->second.kind==CallKind::Function &&
               !method_calls_.contains(&expr) && !functions_.at(resolution->second.target).external && scalar(expr_types_.at(&expr))) {
                for(const auto& argument:call->args) {
                    if(argument.writable || !scalar(expr_types_.at(argument.value.get())))
                        reject("A parallel pure call requires scalar value arguments.",expr.span);
                    expression(*argument.value);
                }
                plan.pure_calls.push_back(&expr);
                plan.worker_arithmetic=false;
                return;
            }
        }
        // Calls, projections and reference-producing expressions need the
        // same whole-call worker/ownership proof as an ordinary parallel node.
        reject("This expression has no proven parallel iteration worker/ownership contract.",expr.span);
    };
    std::function<void(const std::vector<StmtPtr>&,unsigned)> body;
    body=[&](const auto& statements,unsigned nested) {
        const auto before=locals;
        for(const auto& stmt:statements) {
            if(const auto* binding=std::get_if<BindingStmt>(&stmt->data)) {
                if(binding->reference||binding->pattern||!scalar(binding_types_.at(stmt.get())))
                    reject("A parallel iteration local must be a scalar value.",stmt->span);
                if(binding->value)expression(*binding->value);
                locals.insert(binding->name);
            } else if(const auto* assign=std::get_if<AssignStmt>(&stmt->data)) {
                if(assign->pattern)reject("Destructured parallel output needs a disjoint-place proof.",stmt->span);
                if(const auto* name=std::get_if<NameExpr>(&assign->target_expr()->data)) {
                    if(name->this_qualifier || name->self_reference || !locals.contains(name->name))reject("A parallel iteration cannot assign captured scalar storage.",stmt->span);
                } else if(nested!=0)
                    reject("An inner loop cannot repeatedly write the outer parallel output element.",stmt->span);
                else if(!std::holds_alternative<IndexExpr>(assign->target_expr()->data))
                    reject("This parallel assignment has no disjoint element proof.",stmt->span);
                expression(*assign->value);
                if(!assign->compound_op.empty() && is_bare_integer(raw_types_.at(assign->target_expr())))
                    plan.worker_arithmetic=false;
            } else if(const auto* branch=std::get_if<IfStmt>(&stmt->data)) {
                expression(*branch->condition);body(branch->then_body,nested);body(branch->else_body,nested);
            } else if(const auto* control=std::get_if<LoopControlStmt>(&stmt->data)) {
                if(!control->is_continue && nested==0)
                    reject("break cannot leave a parallel iteration group; an inner ordinary loop may break.",stmt->span);
            } else if(const auto* inner=std::get_if<WhileStmt>(&stmt->data)) {
                expression(*inner->condition);body(inner->body,nested+1);
            } else if(const auto* inner=std::get_if<ForStmt>(&stmt->data)) {
                if(inner->parallel||!inner->range||!inner->declares||inner->pattern||inner->writable)
                    reject("An inner parallel iteration loop initially requires an ordinary integer value range.",stmt->span);
                visit_for_header(*inner,expression);
                const auto saved=locals;locals.insert(inner->name);body(inner->body,nested+1);locals=saved;
                plan.worker_arithmetic=false;
            } else if(const auto* value=std::get_if<ExprStmt>(&stmt->data)) {
                if(!std::holds_alternative<VoidExpr>(value->value->data))expression(*value->value);
            } else if(std::holds_alternative<ReturnStmt>(stmt->data))
                reject("return cannot leave a parallel iteration group.",stmt->span);
            else reject("This statement has no proven parallel iteration contract.",stmt->span);
        }
        locals=before;
    };
    body(loop.body,0);
    for(const auto& output:outputs)captures.insert(output);
    for(const auto& name:captures)
        if(const auto binding=binding_declarations_.find(name);binding!=binding_declarations_.end())
            plan.capture_bindings.emplace(name,binding->second);
    plan.captures.assign(captures.begin(),captures.end());
    plan.outputs.assign(outputs.begin(),outputs.end());
    parallel_for_plans_[&loop]=std::move(plan);
}
}
