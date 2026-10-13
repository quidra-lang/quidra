// Borrow inference (borrow_inference.hpp): the constructor classifies the
// parameters of every function and method; the mutation queries follow
// every way a body can reach a name's storage.

#include "lowering/borrow_inference.hpp"
#include "lowering/checked_types.hpp"
#include "lowering/syntax_queries.hpp"
#include "quidra/member_function_names.hpp"
#include "quidra/standard_classes.hpp"
#include "semantics/effect_summary.hpp"

namespace quidra::lowering {

bool BorrowInference::expression_mutates_parameter(const Expr& expression, const std::string& name) const {
    if(checked_.effects && (std::holds_alternative<CallExpr>(expression.data) || std::holds_alternative<MethodCallExpr>(expression.data)))
        for(const auto& [candidate,_]:checked_.expr_types)
            if(const auto* root=std::get_if<NameExpr>(&candidate->data);root && source_key(*root)==name && semantics::reference_call_writes(checked_,expression,*candidate))return true;
    if (const auto* node = std::get_if<StringTemplateExpr>(&expression.data)) {
        for (const auto& item : node->expressions) if (expression_mutates_parameter(*item, name)) return true;
        return false;
    }
    if (const auto* node = std::get_if<TupleExpr>(&expression.data)) {
        for (const auto& item : node->elements) if (expression_mutates_parameter(*item, name)) return true;
        return false;
    }
    if (const auto* node = std::get_if<ArrayComprehensionExpr>(&expression.data)) {
        if (expression_mutates_parameter(*node->source, name)) return true;
        if (node->name == name) return false;
        return (node->condition && expression_mutates_parameter(*node->condition, name)) ||
               expression_mutates_parameter(*node->value, name);
    }
    if (const auto* node = std::get_if<ArrayExpr>(&expression.data)) {
        for (const auto& item : node->elements) if (expression_mutates_parameter(*item, name)) return true;
        return false;
    }
    if (const auto* node = std::get_if<IndexExpr>(&expression.data)) {
        if (expression_mutates_parameter(*node->base, name)) return true;
        for (const auto& item : node->items) {
            if (item.index && expression_mutates_parameter(*item.index, name)) return true;
            if (item.start && expression_mutates_parameter(*item.start, name)) return true;
            if (item.stop && expression_mutates_parameter(*item.stop, name)) return true;
            if (item.step && expression_mutates_parameter(*item.step, name)) return true;
        }
        return false;
    }
    if (const auto* node = std::get_if<MemberExpr>(&expression.data))
        return expression_mutates_parameter(*node->base, name);
    if (const auto* node = std::get_if<UnaryExpr>(&expression.data))
        return expression_mutates_parameter(*node->operand, name);
    if (const auto* node = std::get_if<BinaryExpr>(&expression.data))
        return expression_mutates_parameter(*node->left, name) || expression_mutates_parameter(*node->right, name);
    if (const auto* node = std::get_if<ComparisonChainExpr>(&expression.data)) {
        for (const auto& operand : node->operands)
            if (expression_mutates_parameter(*operand, name)) return true;
        return false;
    }
    if (const auto* node = std::get_if<TryExpr>(&expression.data))
        return expression_mutates_parameter(*node->value, name);
    if (const auto* node = std::get_if<IfExpr>(&expression.data)) {
        for (const auto& condition : node->conditions)
            if (expression_mutates_parameter(*condition, name)) return true;
        for (const auto& value : node->values)
            if (expression_mutates_parameter(*value, name)) return true;
        return expression_mutates_parameter(*node->otherwise, name);
    }
    if (const auto* node = std::get_if<CallExpr>(&expression.data)) {
        for (const auto& argument : node->args) {
            if (argument.writable && storage_root_is(*argument.value, name)) return true;
            if (expression_mutates_parameter(*argument.value, name)) return true;
        }
        return false;
    }
    if (const auto* node = std::get_if<MethodCallExpr>(&expression.data)) {
        if (storage_root_is(*node->receiver, name)) {
            const auto receiver_type = type_of(checked_, *node->receiver);
            // Autograd state is per descriptor (logical leaf identity and
            // gradient slot): track() gives an untracked receiver its
            // identity, clear_grad() empties its slot. A borrowed parameter
            // is the caller's descriptor, so these must see an owned copy,
            // exactly like `value = value` makes it one. untrack() and
            // retrack() only build a new descriptor and leave the
            // receiver's state alone, so they keep the borrow.
            if (receiver_type.kind == TypeKind::Tensor &&
                (node->method == "track" || node->method == "clear_grad")) {
                return true;
            }
            if (receiver_type.kind == TypeKind::Class &&
                receiver_type.class_name == standard_class::file_handle &&
                (node->method == "read" || node->method == "read_line" ||
                 node->method == "read_bin" || node->method == "write" ||
                 node->method == "write_line" || node->method == "flush" ||
                 node->method == "seek" || node->method == "close")) {
                return true;
            }
            if (const auto call = checked_.method_calls.find(&expression); call != checked_.method_calls.end()) {
                const auto signature = checked_.functions.find(call->second.internal_name);
                if (signature != checked_.functions.end()) {
                    const auto& effect = signature->second.receiver_effect;
                    if (!effect.writes.empty() || !effect.initializes.empty() || !effect.invalidates.empty()) return true;
                }
            }
        }
        if (expression_mutates_parameter(*node->receiver, name)) return true;
        for (const auto& argument : node->args) {
            if (argument.writable && storage_root_is(*argument.value, name)) return true;
            if (expression_mutates_parameter(*argument.value, name)) return true;
        }
    }
    return false;
}

bool BorrowInference::block_mutates_parameter(const std::vector<StmtPtr>& body, const std::string& name) const {
    for (const auto& statement : body)
        if (statement_mutates_parameter(*statement, name)) return true;
    return false;
}

bool BorrowInference::statement_mutates_parameter(const Stmt& statement, const std::string& name) const {
    const auto& data = statement.data;
    if (const auto* node = std::get_if<BindingStmt>(&data)) {
        if (node->value) {
            if ((node->reference || node->reference_initializer) && storage_root_is(*node->value, name)) return true;
            if (expression_mutates_parameter(*node->value, name)) return true;
        }
    } else if (const auto* node = std::get_if<AssignStmt>(&data)) {
        if(node->pattern){bool writes=false;visit_pattern(*node->pattern,[&](const Pattern& p){if(p.place && (storage_root_is(*p.place,name)||expression_mutates_parameter(*p.place,name)))writes=true;});return writes || expression_mutates_parameter(*node->value,name);}
        if (storage_root_is(*node->target, name)) return true;
        if (expression_mutates_parameter(*node->target, name) || expression_mutates_parameter(*node->value, name)) return true;
    } else if (const auto* node = std::get_if<RebindStmt>(&data)) {
        if (storage_root_is(*node->target, name) || expression_mutates_parameter(*node->target, name)) return true;
    } else if (const auto* node = std::get_if<ReturnStmt>(&data)) {
        if (node->value && expression_mutates_parameter(*node->value, name)) return true;
    } else if (const auto* node = std::get_if<ExprStmt>(&data)) {
        if (expression_mutates_parameter(*node->value, name)) return true;
    } else if (const auto* node = std::get_if<IfStmt>(&data)) {
        if (expression_mutates_parameter(*node->condition, name) || block_mutates_parameter(node->then_body, name) || block_mutates_parameter(node->else_body, name)) return true;
    } else if (const auto* node = std::get_if<ParallelStmt>(&data)) {
        if(block_mutates_parameter(node->body,name)) return true;
    } else if (const auto* node = std::get_if<WhileStmt>(&data)) {
        if (expression_mutates_parameter(*node->condition, name) || block_mutates_parameter(node->body, name)) return true;
    } else if (const auto* node = std::get_if<ForStmt>(&data)) {
        bool mutates=false;
        visit_for_header(*node,[&](const Expr& operand){mutates|=expression_mutates_parameter(operand,name);});
        if ((node->writable && storage_root_is(*node->iterable,name)) || mutates ||
            block_mutates_parameter(node->body,name)) return true;
    } else if (const auto* node = std::get_if<MatchStmt>(&data)) {
        if (expression_mutates_parameter(*node->value, name)) return true;
        for (const auto& match_case : node->cases) if (block_mutates_parameter(match_case.body, name)) return true;
    }
    return false;
}

BorrowInference::BorrowInference(const CheckedProgram& checked) : checked_(checked) {
    std::unordered_set<std::string> function_value_targets;
    for (const auto& [expression, name] : checked_.function_references) {
        (void)expression;
        function_value_targets.insert(name);
    }
    // Whether the body observes the address of const parameter `index`, or
    // of a part of it; unknown without summaries, so then it does.
    const auto address_observed = [&](const std::string& internal_name, std::size_t index) {
        if (!checked_.effects) return true;
        const auto* summary = checked_.effects->find(internal_name);
        if (!summary || index >= summary->const_parameter_address_observed.size()) return true;
        return static_cast<bool>(summary->const_parameter_address_observed[index]);
    };
    const auto classify = [&](const std::string& internal_name, const FunctionDecl& declaration, std::size_t offset) {
        const auto signature = checked_.functions.find(internal_name);
        if (signature == checked_.functions.end()) return;
        for (std::size_t i = 0; i < declaration.parameters.size(); ++i) {
            const auto signature_index = i + offset;
            if (signature_index >= signature->second.parameters.size()) continue;
            const auto& parameter = signature->second.parameters[signature_index];
            if (parameter.writable || !clone_needing(parameter.type)) continue;
            // Function values have a consuming by-value ABI: an indirect
            // caller cannot know a concrete target's inferred borrows. Keep
            // its direct entry and any checked shape entry on that same ABI.
            if (function_value_targets.contains(internal_name)) continue;
            // `&b == &a` is false for a const parameter b: it has storage of
            // its own, a copy.
            if (parameter.is_const && address_observed(internal_name, signature_index)) continue;
            if(checked_.effects)if(const auto* summary=checked_.effects->find(internal_name);
                summary && summary->escaping_locals.contains(parameter.name))continue;
            if (!block_mutates_parameter(declaration.body, declaration.parameters[i].name))
                borrowed_[internal_name].insert(signature_index);
        }
    };
    for (const auto& function : checked_.program.functions) classify(function.name, function, 0);
    for (const auto& class_decl : checked_.program.classes)
        for (const auto& method : class_decl.methods)
            classify(member_function_name::method(class_decl.name, method.name), method, 1);
}

bool BorrowInference::parameter_is_borrowed(const std::string& callee, std::size_t index) const {
    const auto found = borrowed_.find(callee);
    return found != borrowed_.end() && found->second.contains(index);
}

} // namespace quidra::lowering
