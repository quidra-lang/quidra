// Pure questions about the AST: is a name the storage root of an
// expression, does an expression contain a writable argument or mention a
// name, may a block replace an array reference, may a statement return.
// They emit nothing; replacement queries may also read checked effects.

#include "range_constraint_traversal.hpp"
#include "lowering/syntax_queries.hpp"
#include "semantics/effect_summary.hpp"
#include <algorithm>

namespace quidra::lowering {

std::string source_key(const NameExpr& name) {
    if (name.self_reference) return "$receiver";
    return name.this_qualifier ? "this." + name.name : name.name;
}

const NameExpr* local_name_expr(const Expr& expression) {
    const auto* name = std::get_if<NameExpr>(&expression.data);
    return name && !name->this_qualifier && !name->self_reference ? name : nullptr;
}

bool storage_root_is(const Expr& expression, const std::string& name) {
    if (const auto* node = std::get_if<NameExpr>(&expression.data)) return source_key(*node) == name;
    if (const auto* node = std::get_if<MemberExpr>(&expression.data)) return storage_root_is(*node->base, name);
    if (const auto* node = std::get_if<IndexExpr>(&expression.data)) return storage_root_is(*node->base, name);
    return false;
}

bool expression_runs_code(const Expr& expression) {
    const auto& data = expression.data;
    if (std::holds_alternative<CallExpr>(data) || std::holds_alternative<MethodCallExpr>(data) ||
        std::holds_alternative<TryExpr>(data) || std::holds_alternative<IfExpr>(data) ||
        std::holds_alternative<ArrayComprehensionExpr>(data))
        return true;
    const auto any = [](const ExprPtr& item) { return item && expression_runs_code(*item); };
    if (const auto* node = std::get_if<MemberExpr>(&data)) return any(node->base);
    if (const auto* node = std::get_if<IndexExpr>(&data)) {
        if (any(node->base)) return true;
        for (const auto& item : node->items)
            if (any(item.index) || any(item.start) || any(item.stop) || any(item.step)) return true;
        return false;
    }
    if (const auto* node = std::get_if<UnaryExpr>(&data)) return any(node->operand);
    if (const auto* node = std::get_if<BinaryExpr>(&data)) return any(node->left) || any(node->right);
    if (const auto* node = std::get_if<ComparisonChainExpr>(&data)) {
        for (const auto& item : node->operands)
            if (any(item)) return true;
        return false;
    }
    if (const auto* node = std::get_if<StringTemplateExpr>(&data)) {
        for (const auto& item : node->expressions)
            if (any(item)) return true;
        return false;
    }
    if (const auto* node = std::get_if<TupleExpr>(&data)) {
        for (const auto& item : node->elements)
            if (any(item)) return true;
    }
    if (const auto* node = std::get_if<ArrayExpr>(&data)) {
        for (const auto& item : node->elements)
            if (any(item)) return true;
    }
    return false;
}

bool expression_contains_writable_argument(const Expr& expression) {
    if (const auto* node = std::get_if<CallExpr>(&expression.data)) {
        for (const auto& arg : node->args) {
            if (arg.writable || expression_contains_writable_argument(*arg.value)) return true;
        }
        return false;
    }
    if (const auto* node = std::get_if<MethodCallExpr>(&expression.data)) {
        if (expression_contains_writable_argument(*node->receiver)) return true;
        for (const auto& arg : node->args) {
            if (arg.writable || expression_contains_writable_argument(*arg.value)) return true;
        }
        return false;
    }
    if (const auto* node = std::get_if<BinaryExpr>(&expression.data)) {
        return expression_contains_writable_argument(*node->left) ||
               expression_contains_writable_argument(*node->right);
    }
    if (const auto* node = std::get_if<ComparisonChainExpr>(&expression.data)) {
        for (const auto& operand : node->operands)
            if (expression_contains_writable_argument(*operand)) return true;
        return false;
    }
    if (const auto* node = std::get_if<UnaryExpr>(&expression.data)) {
        return expression_contains_writable_argument(*node->operand);
    }
    if (const auto* node = std::get_if<IndexExpr>(&expression.data)) {
        if (expression_contains_writable_argument(*node->base)) return true;
        for (const auto& item : node->items) {
            if ((item.index && expression_contains_writable_argument(*item.index)) ||
                (item.start && expression_contains_writable_argument(*item.start)) ||
                (item.stop && expression_contains_writable_argument(*item.stop)) ||
                (item.step && expression_contains_writable_argument(*item.step))) return true;
        }
        return false;
    }
    if (const auto* node = std::get_if<MemberExpr>(&expression.data)) {
        return expression_contains_writable_argument(*node->base);
    }
    if (const auto* node = std::get_if<TupleExpr>(&expression.data)) {
        for (const auto& item : node->elements)
            if (expression_contains_writable_argument(*item)) return true;
        return false;
    }
    if (const auto* node = std::get_if<ArrayComprehensionExpr>(&expression.data)) {
        bool found = false;
        visit_array_comprehension(*node, [&](const Expr& child) { found |= expression_contains_writable_argument(child); });
        return found;
    }
    if (const auto* node = std::get_if<ArrayExpr>(&expression.data)) {
        for (const auto& item : node->elements)
            if (expression_contains_writable_argument(*item)) return true;
        return false;
    }
    if (const auto* node = std::get_if<StringTemplateExpr>(&expression.data)) {
        for (const auto& item : node->expressions)
            if (expression_contains_writable_argument(*item)) return true;
        return false;
    }
    if (const auto* node = std::get_if<TryExpr>(&expression.data)) {
        return expression_contains_writable_argument(*node->value);
    }
    if (const auto* node = std::get_if<IfExpr>(&expression.data)) {
        for (const auto& condition : node->conditions)
            if (expression_contains_writable_argument(*condition)) return true;
        for (const auto& value : node->values)
            if (expression_contains_writable_argument(*value)) return true;
        return expression_contains_writable_argument(*node->otherwise);
    }
    return false;
}

bool expression_mentions_name(
    const Expr& expression, const std::string& name) {
    if (const auto* node = std::get_if<NameExpr>(&expression.data))
        return source_key(*node) == name;
    if (const auto* node = std::get_if<StringTemplateExpr>(&expression.data)) {
        for (const auto& item : node->expressions)
            if (expression_mentions_name(*item, name)) return true;
        return false;
    }
    if (const auto* node = std::get_if<TupleExpr>(&expression.data)) {
        for (const auto& item : node->elements)
            if (expression_mentions_name(*item, name)) return true;
        return false;
    }
    if (const auto* node = std::get_if<ArrayComprehensionExpr>(&expression.data)) {
        if (node->name == name) return expression_mentions_name(*node->source, name);
        bool found = false;
        visit_array_comprehension(*node, [&](const Expr& child) { found |= expression_mentions_name(child, name); });
        return found;
    }
    if (const auto* node = std::get_if<ArrayExpr>(&expression.data)) {
        for (const auto& item : node->elements)
            if (expression_mentions_name(*item, name)) return true;
        return false;
    }
    if (const auto* node = std::get_if<IndexExpr>(&expression.data)) {
        if (expression_mentions_name(*node->base, name)) return true;
        for (const auto& item : node->items) {
            if ((item.index && expression_mentions_name(*item.index, name)) ||
                (item.start && expression_mentions_name(*item.start, name)) ||
                (item.stop && expression_mentions_name(*item.stop, name)) ||
                (item.step && expression_mentions_name(*item.step, name)))
                return true;
        }
        return false;
    }
    if (const auto* node = std::get_if<MemberExpr>(&expression.data))
        return expression_mentions_name(*node->base, name);
    if (const auto* node = std::get_if<UnaryExpr>(&expression.data))
        return expression_mentions_name(*node->operand, name);
    if (const auto* node = std::get_if<BinaryExpr>(&expression.data))
        return expression_mentions_name(*node->left, name) ||
               expression_mentions_name(*node->right, name);
    if (const auto* node = std::get_if<ComparisonChainExpr>(&expression.data)) {
        for (const auto& operand : node->operands)
            if (expression_mentions_name(*operand, name)) return true;
        return false;
    }
    if (const auto* node = std::get_if<TryExpr>(&expression.data))
        return expression_mentions_name(*node->value, name);
    if (const auto* node = std::get_if<IfExpr>(&expression.data)) {
        for (const auto& condition : node->conditions)
            if (expression_mentions_name(*condition, name)) return true;
        for (const auto& value : node->values)
            if (expression_mentions_name(*value, name)) return true;
        return expression_mentions_name(*node->otherwise, name);
    }
    if (const auto* node = std::get_if<CallExpr>(&expression.data)) {
        for (const auto& argument : node->args)
            if (expression_mentions_name(*argument.value, name)) return true;
        return false;
    }
    if (const auto* node = std::get_if<MethodCallExpr>(&expression.data)) {
        if (expression_mentions_name(*node->receiver, name)) return true;
        for (const auto& argument : node->args)
            if (expression_mentions_name(*argument.value, name)) return true;
        return false;
    }
    return false;
}

bool statement_mentions_name(
    const Stmt& statement, const std::string& name) {
    const auto& data = statement.data;
    bool bound_mentions = false;
    visit_statement_range_bounds(statement, [&](const Expr& bound) {
        bound_mentions |= expression_mentions_name(bound, name);
    });
    if (bound_mentions) return true;
    bool pattern_mentions=false;std::visit([&](const auto& node){if constexpr(requires{node.pattern;})if(node.pattern)visit_pattern(*node.pattern,[&](const Pattern& p){pattern_mentions|=p.name==name || (p.place && expression_mentions_name(*p.place,name));});},data);if(pattern_mentions)return true;
    if (const auto* node = std::get_if<BindingStmt>(&data))
        return node->name == name ||
               (node->value && expression_mentions_name(*node->value, name));
    if (const auto* node = std::get_if<AssignStmt>(&data))
        return expression_mentions_name(*node->target, name) ||
               expression_mentions_name(*node->value, name);
    if (const auto* node = std::get_if<RebindStmt>(&data))
        return node->name == name ||
               expression_mentions_name(*node->target, name);
    if (const auto* node = std::get_if<ReturnStmt>(&data))
        return node->value && expression_mentions_name(*node->value, name);
    if (const auto* node = std::get_if<ExprStmt>(&data))
        return expression_mentions_name(*node->value, name);
    if (const auto* node = std::get_if<IfStmt>(&data)) {
        if (expression_mentions_name(*node->condition, name)) return true;
        for (const auto& item : node->then_body)
            if (statement_mentions_name(*item, name)) return true;
        for (const auto& item : node->else_body)
            if (statement_mentions_name(*item, name)) return true;
        return false;
    }
    if (const auto* node = std::get_if<ParallelStmt>(&data)) {
        for(const auto& item:node->body)
            if(statement_mentions_name(*item,name)) return true;
        return false;
    }
    if (const auto* node = std::get_if<WhileStmt>(&data)) {
        if (expression_mentions_name(*node->condition, name)) return true;
        for (const auto& item : node->body)
            if (statement_mentions_name(*item, name)) return true;
        return false;
    }
    if (const auto* node = std::get_if<ForStmt>(&data)) {
        bool mentions=node->name==name;
        visit_for_header(*node,[&](const Expr& operand){mentions|=expression_mentions_name(operand,name);});
        for(const auto& binding:node->collections) mentions|=binding.name==name;
        if(mentions) return true;
        for (const auto& item : node->body)
            if (statement_mentions_name(*item, name)) return true;
        return false;
    }
    if (const auto* node = std::get_if<MatchStmt>(&data)) {
        if (expression_mentions_name(*node->value, name)) return true;
        for (const auto& current : node->cases) {
            bool mentions = false;
            visit_type_range_bounds(current.type, [&](const Expr& bound) {
                mentions |= expression_mentions_name(bound, name);
            });
            if (mentions) return true;
            if (current.binder && *current.binder == name) return true;
            for (const auto& item : current.body)
                if (statement_mentions_name(*item, name)) return true;
        }
    }
    return false;
}

bool expression_may_replace_array_reference(
    const Expr& expression,
    const std::unordered_set<std::string>& parameters,const CheckedProgram* checked) {
    const auto rooted = [&](const Expr& candidate) {
        return std::any_of(
            parameters.begin(), parameters.end(),
            [&](const std::string& name) { return storage_root_is(candidate, name); });
    };
    if(checked && checked->effects && (std::holds_alternative<CallExpr>(expression.data) || std::holds_alternative<MethodCallExpr>(expression.data)))
        for(const auto& [candidate,_]:checked->expr_types)
            if(const auto* name=std::get_if<NameExpr>(&candidate->data);name && parameters.contains(source_key(*name)) && semantics::reference_call_writes(*checked,expression,*candidate))return true;
    if (const auto* node = std::get_if<StringTemplateExpr>(&expression.data)) {
        return std::any_of(
            node->expressions.begin(), node->expressions.end(),
            [&](const auto& item) {
                return expression_may_replace_array_reference(*item, parameters,checked);
            });
    }
    if (const auto* node = std::get_if<TupleExpr>(&expression.data)) {
        return std::any_of(
            node->elements.begin(), node->elements.end(),
            [&](const auto& item) {
                return expression_may_replace_array_reference(*item, parameters,checked);
            });
    }
    if (const auto* node = std::get_if<ArrayComprehensionExpr>(&expression.data)) {
        auto outer = parameters; outer.erase(node->name);
        if (expression_may_replace_array_reference(*node->source, parameters,checked)) return true;
        bool found = false;
        if (node->condition) found |= expression_may_replace_array_reference(*node->condition, outer,checked);
        found |= expression_may_replace_array_reference(*node->value, outer,checked);
        return found;
    }
    if (const auto* node = std::get_if<ArrayExpr>(&expression.data)) {
        return std::any_of(
            node->elements.begin(), node->elements.end(),
            [&](const auto& item) {
                return expression_may_replace_array_reference(*item, parameters,checked);
            });
    }
    if (const auto* node = std::get_if<IndexExpr>(&expression.data)) {
        if (expression_may_replace_array_reference(*node->base, parameters,checked)) return true;
        for (const auto& item : node->items) {
            if ((item.index && expression_may_replace_array_reference(*item.index, parameters,checked)) ||
                (item.start && expression_may_replace_array_reference(*item.start, parameters,checked)) ||
                (item.stop && expression_may_replace_array_reference(*item.stop, parameters,checked)) ||
                (item.step && expression_may_replace_array_reference(*item.step, parameters,checked))) {
                return true;
            }
        }
        return false;
    }
    if (const auto* node = std::get_if<MemberExpr>(&expression.data)) {
        return expression_may_replace_array_reference(*node->base, parameters,checked);
    }
    if (const auto* node = std::get_if<UnaryExpr>(&expression.data)) {
        return expression_may_replace_array_reference(*node->operand, parameters,checked);
    }
    if (const auto* node = std::get_if<BinaryExpr>(&expression.data)) {
        return expression_may_replace_array_reference(*node->left, parameters,checked) ||
               expression_may_replace_array_reference(*node->right, parameters,checked);
    }
    if (const auto* node = std::get_if<ComparisonChainExpr>(&expression.data)) {
        for (const auto& operand : node->operands)
            if (expression_may_replace_array_reference(*operand, parameters,checked)) return true;
        return false;
    }
    if (const auto* node = std::get_if<TryExpr>(&expression.data)) {
        return expression_may_replace_array_reference(*node->value, parameters,checked);
    }
    if (const auto* node = std::get_if<IfExpr>(&expression.data)) {
        for (const auto& condition : node->conditions)
            if (expression_may_replace_array_reference(*condition, parameters,checked)) return true;
        for (const auto& value : node->values)
            if (expression_may_replace_array_reference(*value, parameters,checked)) return true;
        return expression_may_replace_array_reference(*node->otherwise, parameters,checked);
    }
    if (const auto* node = std::get_if<CallExpr>(&expression.data)) {
        for (const auto& argument : node->args) {
            if (argument.writable && rooted(*argument.value)) return true;
            if (expression_may_replace_array_reference(*argument.value, parameters,checked)) return true;
        }
        return false;
    }
    if (const auto* node = std::get_if<MethodCallExpr>(&expression.data)) {
        if (expression_may_replace_array_reference(*node->receiver, parameters,checked)) return true;
        for (const auto& argument : node->args) {
            if (argument.writable && rooted(*argument.value)) return true;
            if (expression_may_replace_array_reference(*argument.value, parameters,checked)) return true;
        }
    }
    return false;
}

bool block_may_replace_array_reference(
    const std::vector<StmtPtr>& body,
    const std::unordered_set<std::string>& parameters,const CheckedProgram* checked) {
    const auto rooted = [&](const Expr& candidate) {
        return std::any_of(
            parameters.begin(), parameters.end(),
            [&](const std::string& name) { return storage_root_is(candidate, name); });
    };
    for (const auto& statement : body) {
        const auto& data = statement->data;
        bool replaces = false;
        visit_statement_range_bounds(*statement, [&](const Expr& bound) {
            replaces |= expression_may_replace_array_reference(bound, parameters, checked);
        });
        if (replaces) return true;
        if (const auto* node = std::get_if<BindingStmt>(&data)) {
            if (!node->value) continue;
            if ((node->reference || node->reference_initializer) && rooted(*node->value))
                return true;
            if (expression_may_replace_array_reference(*node->value, parameters,checked))
                return true;
        } else if (const auto* node = std::get_if<AssignStmt>(&data)) {
            if(node->pattern) {
                bool writes=false;
                visit_pattern(*node->pattern,[&](const Pattern& leaf){if(leaf.place && rooted(*leaf.place))writes=true;});
                if(writes || expression_may_replace_array_reference(*node->value,parameters,checked))return true;
                continue;
            }
            if (const auto* target = std::get_if<NameExpr>(&node->target_expr()->data);
                target && parameters.contains(source_key(*target))) {
                return true;
            }
            if (expression_may_replace_array_reference(*node->target_expr(), parameters,checked) ||
                expression_may_replace_array_reference(*node->value, parameters,checked)) {
                return true;
            }
        } else if (const auto* node = std::get_if<RebindStmt>(&data)) {
            if (parameters.contains(node->name) || rooted(*node->target) ||
                expression_may_replace_array_reference(*node->target, parameters,checked)) {
                return true;
            }
        } else if (const auto* node = std::get_if<ReturnStmt>(&data)) {
            if (node->value &&
                expression_may_replace_array_reference(*node->value, parameters,checked)) {
                return true;
            }
        } else if (const auto* node = std::get_if<ExprStmt>(&data)) {
            if (expression_may_replace_array_reference(*node->value, parameters,checked))
                return true;
        } else if (const auto* node = std::get_if<IfStmt>(&data)) {
            if (expression_may_replace_array_reference(*node->condition, parameters,checked) ||
                block_may_replace_array_reference(node->then_body, parameters,checked) ||
                block_may_replace_array_reference(node->else_body, parameters,checked)) {
                return true;
            }
        } else if (const auto* node = std::get_if<MainGuardStmt>(&data)) {
            if (node->active && block_may_replace_array_reference(node->body, parameters,checked))
                return true;
        } else if (const auto* node = std::get_if<ParallelStmt>(&data)) {
            if(block_may_replace_array_reference(node->body, parameters,checked)) return true;
        } else if (const auto* node = std::get_if<WhileStmt>(&data)) {
            if (expression_may_replace_array_reference(*node->condition, parameters,checked) ||
                block_may_replace_array_reference(node->body, parameters,checked)) {
                return true;
            }
        } else if (const auto* node = std::get_if<ForStmt>(&data)) {
            bool replaces=false;
            visit_for_header(*node,[&](const Expr& operand){replaces|=expression_may_replace_array_reference(operand,parameters,checked);});
            if (replaces || block_may_replace_array_reference(node->body, parameters,checked)) {
                return true;
            }
        } else if (const auto* node = std::get_if<MatchStmt>(&data)) {
            if (expression_may_replace_array_reference(*node->value, parameters,checked))
                return true;
            for (const auto& match_case : node->cases) {
                visit_type_range_bounds(match_case.type, [&](const Expr& bound) {
                    replaces |= expression_may_replace_array_reference(bound, parameters, checked);
                });
                if (replaces) return true;
                if (block_may_replace_array_reference(match_case.body, parameters,checked))
                    return true;
            }
        }
    }
    return false;
}

bool statement_may_return(const Stmt& statement) {
    const auto& data = statement.data;
    if (std::holds_alternative<ReturnStmt>(data)) return true;
    if (const auto* node = std::get_if<IfStmt>(&data)) {
        return block_may_return(node->then_body) ||
               block_may_return(node->else_body);
    }
    if (const auto* node = std::get_if<WhileStmt>(&data))
        return block_may_return(node->body);
    if (const auto* node = std::get_if<ForStmt>(&data))
        return block_may_return(node->body);
    if (const auto* node = std::get_if<MatchStmt>(&data)) {
        for (const auto& current : node->cases)
            if (block_may_return(current.body)) return true;
    }
    return false;
}

bool block_may_return(const std::vector<StmtPtr>& statements) {
    for (const auto& statement : statements)
        if (statement_may_return(*statement)) return true;
    return false;
}

} // namespace quidra::lowering
