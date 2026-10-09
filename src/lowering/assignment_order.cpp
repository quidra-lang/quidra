// AssignmentOrder: see assignment_order.hpp.

#include "lowering/assignment_order.hpp"

#include "lowering/syntax_queries.hpp"
#include "quidra/language.hpp"
#include "quidra/standard_classes.hpp"

#include <set>
#include <string>
#include <unordered_set>

namespace quidra::lowering {
namespace {

// Calls `visit` on `expression` and every subexpression, parents first.
template <class Visit>
void each_expression(const Expr& expression, Visit& visit) {
    visit(expression);
    const auto each = [&](const ExprPtr& item) {
        if (item) each_expression(*item, visit);
    };
    const auto& data = expression.data;
    if (const auto* node = std::get_if<StringTemplateExpr>(&data)) {
        for (const auto& item : node->expressions) each(item);
    } else if (const auto* node = std::get_if<ArrayExpr>(&data)) {
        for (const auto& item : node->elements) each(item);
    } else if (const auto* node = std::get_if<IndexExpr>(&data)) {
        each(node->base);
        for (const auto& item : node->items) {
            each(item.index);
            each(item.start);
            each(item.stop);
            each(item.step);
        }
    } else if (const auto* node = std::get_if<MemberExpr>(&data)) {
        each(node->base);
    } else if (const auto* node = std::get_if<UnaryExpr>(&data)) {
        each(node->operand);
    } else if (const auto* node = std::get_if<BinaryExpr>(&data)) {
        each(node->left);
        each(node->right);
    } else if (const auto* node = std::get_if<TryExpr>(&data)) {
        each(node->value);
    } else if (const auto* node = std::get_if<IfExpr>(&data)) {
        for (const auto& item : node->conditions) each(item);
        for (const auto& item : node->values) each(item);
        each(node->otherwise);
    } else if (const auto* node = std::get_if<CallExpr>(&data)) {
        for (const auto& argument : node->args) each(argument.value);
    } else if (const auto* node = std::get_if<MethodCallExpr>(&data)) {
        each(node->receiver);
        for (const auto& argument : node->args) each(argument.value);
    }
}

// The receiver's fields and every shared region, as pseudo-names in the read
// and write sets of a side.
constexpr char receiver_fields[] = "$receiver";
constexpr char shared_regions[] = "$shared";

// One side of a plain assignment: its target subexpressions, or its
// right-hand side.
struct Side {
    std::vector<const Expr*> expressions;
    std::set<std::string> reads;
    std::set<std::string> writes;
    bool calls{};
    bool may_fail{};
};

class Classifier {
public:
    Classifier(const CheckedProgram& checked, const BorrowInference& borrows)
        : checked_(checked), borrows_(borrows) {}

    void summarize(Side& side) const {
        const auto visit = [&](const Expr& expression) {
            const auto& data = expression.data;
            if (const auto* name = std::get_if<NameExpr>(&data)) {
                side.reads.insert(source_key(*name));
                if (checked_.field_accesses.contains(&expression)) side.reads.insert(receiver_fields);
            }
            if ((std::holds_alternative<NameExpr>(data) || std::holds_alternative<MemberExpr>(data) ||
                 std::holds_alternative<IndexExpr>(data)) &&
                expression_holds_shared_region(expression)) {
                side.reads.insert(shared_regions);
            }
            if (const auto* index = std::get_if<IndexExpr>(&data)) {
                bool slice = false;
                for (const auto& item : index->items) slice = slice || item.slice;
                if (slice || !checked_.bounds_proven.contains(&expression)) side.may_fail = true;
            } else if (const auto* binary = std::get_if<BinaryExpr>(&data)) {
                const auto* left = type_of(*binary->left);
                const bool integer = left && is_integer_family_type(*left);
                const auto& op = binary->op;
                if (integer && (op == "+" || op == "-" || op == "*" || op == "/" || op == "%" || op == "^"))
                    side.may_fail = true;
                if (left && is_real(*left) && op == "^") side.may_fail = true;
            } else if (const auto* unary = std::get_if<UnaryExpr>(&data)) {
                const auto* type = type_of(*unary->operand);
                if (type && unary->op == "-" && is_signed_integer(*type)) side.may_fail = true;
            } else if (std::holds_alternative<TryExpr>(data)) {
                side.may_fail = true;
            } else if (std::holds_alternative<CallExpr>(data) ||
                       std::holds_alternative<MethodCallExpr>(data)) {
                if (effectful_call(expression)) side.calls = true;
                const auto kind = builtin(expression);
                if (!kind || *kind != BuiltinCallable::Len) side.may_fail = true;
                if (implicit_receiver_call(expression) && writing_method_call(expression))
                    side.writes.insert(receiver_fields);
                if (user_call(expression)) {
                    bool shares = false;
                    if (const auto* call = std::get_if<CallExpr>(&data)) {
                        for (const auto& argument : call->args)
                            shares = shares || expression_holds_shared_region(*argument.value);
                    } else {
                        const auto& method = std::get<MethodCallExpr>(data);
                        shares = expression_holds_shared_region(*method.receiver);
                        for (const auto& argument : method.args)
                            shares = shares || expression_holds_shared_region(*argument.value);
                    }
                    if (shares) side.writes.insert(shared_regions);
                }
            }
        };
        for (const auto* expression : side.expressions) each_expression(*expression, visit);
    }

    void writes(Side& side, const std::set<std::string>& names) const {
        for (const auto& name : names) {
            if (name == receiver_fields || name == shared_regions) continue;
            for (const auto* expression : side.expressions) {
                if (borrows_.expression_mutates_parameter(*expression, name)) {
                    side.writes.insert(name);
                    break;
                }
            }
        }
    }

private:
    const Type* type_of(const Expr& expression) const {
        const auto found = checked_.expr_types.find(&expression);
        return found == checked_.expr_types.end() ? nullptr : &found->second;
    }

    const ClassTypeInfo* class_info(const Type& type) const {
        if (type.kind != TypeKind::Class) return nullptr;
        const auto found = checked_.classes.find(type.class_name);
        return found == checked_.classes.end() ? nullptr : &found->second;
    }

    bool holds_shared_region(const Type& type, std::unordered_set<std::string>& active) const {
        if (type.kind == TypeKind::Array) return type.first && holds_shared_region(*type.first, active);
        if (type.kind == TypeKind::Union) {
            for (const auto& item : type.cases)
                if (holds_shared_region(item, active)) return true;
            return false;
        }
        const auto* info = class_info(type);
        if (!info) return false;
        if (standard_class::is_ref_cell_instance(type.class_name, info->standard_library)) return true;
        if (!active.insert(type.class_name).second) return false;
        for (const auto& field : info->fields)
            if (holds_shared_region(field.type, active)) return true;
        return false;
    }
    bool expression_holds_shared_region(const Expr& expression) const {
        const auto* type = type_of(expression);
        if (!type) return true;
        std::unordered_set<std::string> active;
        return holds_shared_region(*type, active);
    }

    const StorageEffect* receiver_effect(const Expr& call) const {
        const auto method = checked_.method_calls.find(&call);
        if (method == checked_.method_calls.end()) return nullptr;
        const auto signature = checked_.functions.find(method->second.internal_name);
        return signature == checked_.functions.end() ? nullptr : &signature->second.receiver_effect;
    }
    // A user method call (explicit or implicit receiver) that may write its
    // receiver.
    bool writing_method_call(const Expr& call) const {
        if (!checked_.method_calls.contains(&call)) return false;
        const auto* effect = receiver_effect(call);
        return !effect || !effect->writes.empty() || !effect->initializes.empty() ||
               !effect->invalidates.empty();
    }
    // An implicit-receiver call inside a method: a CallExpr the checker
    // resolved to a method of the receiver.
    bool implicit_receiver_call(const Expr& call) const {
        return std::holds_alternative<CallExpr>(call.data) && checked_.method_calls.contains(&call);
    }
    std::optional<BuiltinCallable> builtin(const Expr& call) const {
        const auto found = checked_.call_resolutions.find(&call);
        if (found == checked_.call_resolutions.end() || found->second.kind != CallKind::Builtin)
            return std::nullopt;
        return found->second.builtin;
    }
    bool numeric_cast(const Expr& call) const {
        const auto found = checked_.call_resolutions.find(&call);
        return found != checked_.call_resolutions.end() && found->second.kind == CallKind::NumericCast;
    }
    // A call that can print, read or run user code: every call but numeric
    // casts and the pure built-ins len and range.
    bool effectful_call(const Expr& expression) const {
        if (std::holds_alternative<MethodCallExpr>(expression.data)) return true;
        if (!std::holds_alternative<CallExpr>(expression.data) || numeric_cast(expression)) return false;
        const auto kind = builtin(expression);
        return !kind || (*kind != BuiltinCallable::Len && *kind != BuiltinCallable::Range);
    }
    // A call whose callee is user or package code (not a built-in or a cast).
    bool user_call(const Expr& expression) const {
        if (std::holds_alternative<MethodCallExpr>(expression.data))
            return checked_.method_calls.contains(&expression);
        if (!std::holds_alternative<CallExpr>(expression.data)) return false;
        const auto found = checked_.call_resolutions.find(&expression);
        return found == checked_.call_resolutions.end() ||
               (found->second.kind != CallKind::Builtin && found->second.kind != CallKind::NumericCast);
    }

    const CheckedProgram& checked_;
    const BorrowInference& borrows_;
};

} // namespace

std::vector<const Expr*> AssignmentOrder::target_subexpressions(const Expr& target) {
    // The path from the target down to its root, outermost first.
    std::vector<const IndexExpr*> indices;
    for (const Expr* current = &target; current;) {
        if (const auto* member = std::get_if<MemberExpr>(&current->data)) {
            current = member->base.get();
        } else if (const auto* index = std::get_if<IndexExpr>(&current->data)) {
            indices.push_back(index);
            current = index->base.get();
        } else {
            current = nullptr;
        }
    }
    std::vector<const Expr*> out;
    for (auto index = indices.rbegin(); index != indices.rend(); ++index) {
        for (const auto& item : (*index)->items) {
            for (const auto* part : {item.index.get(), item.start.get(), item.stop.get(), item.step.get()})
                if (part) out.push_back(part);
        }
    }
    return out;
}

AssignmentOrder::Tier AssignmentOrder::classify(const AssignStmt& assignment) const {
    if (!assignment.compound_op.empty()) return Tier::none;
    Side target;
    target.expressions = target_subexpressions(*assignment.target);
    if (target.expressions.empty()) return Tier::none;
    Side value;
    value.expressions.push_back(assignment.value.get());
    const Classifier classifier(checked_, borrows_);
    classifier.summarize(target);
    classifier.summarize(value);
    std::set<std::string> names = target.reads;
    names.insert(value.reads.begin(), value.reads.end());
    classifier.writes(target, names);
    classifier.writes(value, names);
    const auto meets = [](const std::set<std::string>& left, const Side& right) {
        for (const auto& name : left)
            if (right.reads.contains(name) || right.writes.contains(name)) return true;
        return false;
    };
    if (meets(value.writes, target) || meets(target.writes, value) || (target.calls && value.calls))
        return Tier::every_execution;
    if ((target.may_fail && (value.calls || value.may_fail)) ||
        (value.may_fail && (target.calls || target.may_fail)))
        return Tier::failure_order;
    return Tier::none;
}

} // namespace quidra::lowering
