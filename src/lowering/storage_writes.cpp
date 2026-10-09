// Storage writes (storage_writes.hpp): the places an expression or a
// statement may write, from BorrowInference's mutation queries, the
// checker's receiver effects and the types that can hold a place's storage.

#include "lowering/storage_writes.hpp"
#include "lowering/checked_types.hpp"
#include "lowering/syntax_queries.hpp"
#include "quidra/standard_classes.hpp"
#include <initializer_list>
#include <string_view>

namespace quidra::lowering {

namespace {

// The binding an expression's storage belongs to: the name at the base of its
// member and index chain, or null when the chain starts at a value.
const Expr* storage_root(const Expr& expression) {
    const Expr* current = &expression;
    for (;;) {
        if (const auto* member = std::get_if<MemberExpr>(&current->data)) {
            current = member->base.get();
        } else if (const auto* index = std::get_if<IndexExpr>(&current->data)) {
            current = index->base.get();
        } else {
            break;
        }
    }
    return std::holds_alternative<NameExpr>(current->data) ? current : nullptr;
}

// Whether `test` holds for `expression` or any of its subexpressions.
template <class Test>
bool any_subexpression(const Expr& expression, const Test& test) {
    if (test(expression)) return true;
    const auto any = [&](const ExprPtr& item) {
        return item && any_subexpression(*item, test);
    };
    if (const auto* node = std::get_if<StringTemplateExpr>(&expression.data)) {
        for (const auto& item : node->expressions) if (any(item)) return true;
    } else if (const auto* node = std::get_if<ArrayExpr>(&expression.data)) {
        for (const auto& item : node->elements) if (any(item)) return true;
    } else if (const auto* node = std::get_if<IndexExpr>(&expression.data)) {
        if (any(node->base)) return true;
        for (const auto& item : node->items) {
            if (any(item.index) || any(item.start) || any(item.stop) || any(item.step)) return true;
        }
    } else if (const auto* node = std::get_if<MemberExpr>(&expression.data)) {
        return any(node->base);
    } else if (const auto* node = std::get_if<UnaryExpr>(&expression.data)) {
        return any(node->operand);
    } else if (const auto* node = std::get_if<BinaryExpr>(&expression.data)) {
        return any(node->left) || any(node->right);
    } else if (const auto* node = std::get_if<TryExpr>(&expression.data)) {
        return any(node->value);
    } else if (const auto* node = std::get_if<IfExpr>(&expression.data)) {
        for (const auto& item : node->conditions) if (any(item)) return true;
        for (const auto& item : node->values) if (any(item)) return true;
        return any(node->otherwise);
    } else if (const auto* node = std::get_if<CallExpr>(&expression.data)) {
        for (const auto& argument : node->args) if (any(argument.value)) return true;
    } else if (const auto* node = std::get_if<MethodCallExpr>(&expression.data)) {
        if (any(node->receiver)) return true;
        for (const auto& argument : node->args) if (any(argument.value)) return true;
    }
    return false;
}

// Whether `test` holds for an expression of `statement` or of a statement
// nested in it, or any subexpression of one.
template <class Test>
bool any_statement_expression(const Stmt& statement, const Test& test) {
    const auto expression = [&](const ExprPtr& item) {
        return item && any_subexpression(*item, test);
    };
    const auto block = [&](const std::vector<StmtPtr>& body) {
        for (const auto& item : body)
            if (any_statement_expression(*item, test)) return true;
        return false;
    };
    const auto& data = statement.data;
    if (const auto* node = std::get_if<BindingStmt>(&data)) return expression(node->value);
    if (const auto* node = std::get_if<AssignStmt>(&data))
        return expression(node->target) || expression(node->value);
    if (const auto* node = std::get_if<RebindStmt>(&data)) return expression(node->target);
    if (const auto* node = std::get_if<ReturnStmt>(&data)) return expression(node->value);
    if (const auto* node = std::get_if<ExprStmt>(&data)) return expression(node->value);
    if (const auto* node = std::get_if<IfStmt>(&data))
        return expression(node->condition) || block(node->then_body) || block(node->else_body);
    if (const auto* node = std::get_if<WhileStmt>(&data))
        return expression(node->condition) || block(node->body);
    if (const auto* node = std::get_if<ForStmt>(&data))
        return expression(node->iterable) || block(node->body);
    if (const auto* node = std::get_if<MatchStmt>(&data)) {
        if (expression(node->value)) return true;
        for (const auto& match_case : node->cases)
            if (block(match_case.body)) return true;
        return false;
    }
    if (const auto* node = std::get_if<MainGuardStmt>(&data)) return block(node->body);
    return false;
}

// Whether `test` holds for `statement` or a statement nested in it.
template <class Test>
bool any_statement(const Stmt& statement, const Test& test) {
    if (test(statement)) return true;
    const auto block = [&](const std::vector<StmtPtr>& body) {
        for (const auto& item : body)
            if (any_statement(*item, test)) return true;
        return false;
    };
    const auto& data = statement.data;
    if (const auto* node = std::get_if<IfStmt>(&data))
        return block(node->then_body) || block(node->else_body);
    if (const auto* node = std::get_if<WhileStmt>(&data)) return block(node->body);
    if (const auto* node = std::get_if<ForStmt>(&data)) return block(node->body);
    if (const auto* node = std::get_if<MatchStmt>(&data)) {
        for (const auto& match_case : node->cases)
            if (block(match_case.body)) return true;
        return false;
    }
    if (const auto* node = std::get_if<MainGuardStmt>(&data)) return block(node->body);
    return false;
}

// Whether two places name the same binding through the same fields.
bool same_member_path(const Expr& left, const Expr& right) {
    if (const auto* a = std::get_if<NameExpr>(&left.data)) {
        const auto* b = std::get_if<NameExpr>(&right.data);
        return b && source_key(*a) == source_key(*b);
    }
    if (const auto* a = std::get_if<MemberExpr>(&left.data)) {
        const auto* b = std::get_if<MemberExpr>(&right.data);
        return b && a->name == b->name && same_member_path(*a->base, *b->base);
    }
    return false;
}

bool effect_writes(const StorageEffect& effect) {
    return !effect.writes.empty() || !effect.initializes.empty() ||
           !effect.invalidates.empty();
}

} // namespace

bool StorageWrites::value_may_write_target_root(
    const Expr& target, const Expr& value) const {
    const auto path = target_path(target);
    return path && expression_may_write_path(value, *path);
}

std::optional<StorageWrites::RootedPath> StorageWrites::target_path(
    const Expr& target) const {
    RootedPath path;
    const Expr* current = &target;
    for (;;) {
        if (const auto* member = std::get_if<MemberExpr>(&current->data)) {
            current = member->base.get();
        } else if (const auto* index = std::get_if<IndexExpr>(&current->data)) {
            current = index->base.get();
        } else {
            break;
        }
        const auto type = checked_.expr_types.find(current);
        if (type == checked_.expr_types.end()) return std::nullopt;
        path.types.push_back(type->second);
    }
    const auto* name = std::get_if<NameExpr>(&current->data);
    if (!name || current == &target) return std::nullopt;
    path.root = current;
    path.name = source_key(*name);
    return path;
}

bool StorageWrites::expression_may_write_path(
    const Expr& expression, const RootedPath& path) const {
    if (borrows_.expression_mutates_parameter(expression, path.name)) return true;
    const bool receiver_rooted = path_receiver_rooted(path);
    const bool shared = path_holds_shared_region(path);
    return any_subexpression(expression, [&](const Expr& node) {
        return node_may_write_path(node, path, receiver_rooted, shared);
    });
}

bool StorageWrites::path_receiver_rooted(const RootedPath& path) const {
    // An implicit-receiver call may write any field of the receiver, and a
    // reference may designate one.
    return is_receiver_field(*path.root) || is_reference_binding(path.name, *path.root);
}

bool StorageWrites::path_holds_shared_region(const RootedPath& path) const {
    // A ref.Cell on the path shares its storage with every copy of it.
    for (const auto& type : path.types) {
        std::unordered_set<std::string> active;
        if (type_holds_shared_region(type, active)) return true;
    }
    return false;
}

bool StorageWrites::node_may_write_path(
    const Expr& node, const RootedPath& path, bool receiver_rooted,
    bool shared) const {
    // Another binding that may alias the path, written by a writable
    // argument or a writing receiver.
    const auto aliased_write = [&](const Expr& written) {
        const auto* root = storage_root(written);
        if (!root) return false;
        const auto name = source_key(std::get<NameExpr>(root->data));
        return name != path.name && path_may_be_aliased_by(path, name, *root) &&
               borrows_.expression_mutates_parameter(node, name);
    };
    if (const auto* call = std::get_if<CallExpr>(&node.data)) {
        if (receiver_rooted) {
            if (const auto method = checked_.method_calls.find(&node);
                method != checked_.method_calls.end()) {
                const auto signature = checked_.functions.find(method->second.internal_name);
                if (signature == checked_.functions.end() ||
                    effect_writes(signature->second.receiver_effect))
                    return true;
            }
        }
        if (shared) {
            const auto resolution = checked_.call_resolutions.find(&node);
            const bool may_write =
                resolution == checked_.call_resolutions.end() ||
                (resolution->second.kind != CallKind::Builtin &&
                 resolution->second.kind != CallKind::NumericCast);
            if (may_write) {
                for (const auto& argument : call->args)
                    if (expression_type_holds_shared_region(*argument.value)) return true;
            }
        }
        for (const auto& argument : call->args)
            if (argument.writable && aliased_write(*argument.value)) return true;
        return false;
    }
    if (const auto* call = std::get_if<MethodCallExpr>(&node.data)) {
        if (shared) {
            if (expression_type_holds_shared_region(*call->receiver)) return true;
            for (const auto& argument : call->args)
                if (expression_type_holds_shared_region(*argument.value)) return true;
        }
        if (aliased_write(*call->receiver)) return true;
        for (const auto& argument : call->args)
            if (argument.writable && aliased_write(*argument.value)) return true;
    }
    return false;
}

StorageWrites::IterableWrite StorageWrites::statement_writes_iterable(
    const Stmt& statement, const Expr& iterable) const {
    const auto path = iterable_path(iterable);
    if (!path) return IterableWrite::in_place;
    if (!statement_may_write_path(statement, *path)) return IterableWrite::none;
    return classify_iterable_write(statement, iterable, *path);
}

std::optional<StorageWrites::RootedPath> StorageWrites::iterable_path(
    const Expr& iterable) const {
    const auto own = checked_.expr_types.find(&iterable);
    if (own == checked_.expr_types.end()) return std::nullopt;
    RootedPath path;
    if (const auto* name = std::get_if<NameExpr>(&iterable.data)) {
        path.root = &iterable;
        path.name = source_key(*name);
    } else if (auto found = target_path(iterable)) {
        path = std::move(*found);
    } else {
        return std::nullopt;
    }
    // The array itself, and its elements: a reference to one element writes
    // the array in place.
    path.types.push_back(own->second);
    if (own->second.kind == TypeKind::Array && own->second.first)
        path.types.push_back(*own->second.first);
    return path;
}

bool StorageWrites::statement_may_write_path(
    const Stmt& statement, const RootedPath& path) const {
    if (borrows_.statement_mutates_parameter(statement, path.name)) return true;
    const bool receiver_rooted = path_receiver_rooted(path);
    const bool shared = path_holds_shared_region(path);
    if (any_statement_expression(statement, [&](const Expr& node) {
            return node_may_write_path(node, path, receiver_rooted, shared);
        }))
        return true;
    // An assignment or a writable loop through another binding that may
    // alias the path.
    return statement_writes_through_alias(statement, path, true);
}

bool StorageWrites::statement_writes_through_alias(
    const Stmt& statement, const RootedPath& path, bool writable_loops) const {
    return any_statement(statement, [&](const Stmt& inner) {
        const Expr* written = nullptr;
        if (const auto* node = std::get_if<AssignStmt>(&inner.data)) {
            written = node->target.get();
        } else if (const auto* loop = std::get_if<ForStmt>(&inner.data);
                   writable_loops && loop && loop->writable) {
            written = loop->iterable.get();
        }
        if (!written) return false;
        const auto* root = storage_root(*written);
        if (!root) return false;
        const auto name = source_key(std::get<NameExpr>(root->data));
        return name != path.name && path_may_be_aliased_by(path, name, *root);
    });
}

bool StorageWrites::names_reference_binding(const Expr& iterable) const {
    const auto* name = std::get_if<NameExpr>(&iterable.data);
    return name && is_reference_binding(name->name, iterable);
}

bool StorageWrites::statement_may_reshape_iterable(
    const Stmt& statement, const Expr& iterable) const {
    const auto* name = std::get_if<NameExpr>(&iterable.data);
    const auto type = checked_.expr_types.find(&iterable);
    if (!name || type == checked_.expr_types.end()) return true;
    // The array only: a binding that can hold its elements but not the
    // array cannot replace or resize it.
    const RootedPath path{&iterable, name->name, {type->second}};
    const bool receiver_rooted = path_receiver_rooted(path);
    const bool shared = path_holds_shared_region(path);
    if (any_statement_expression(statement, [&](const Expr& node) {
            return node_may_write_path(node, path, receiver_rooted, shared);
        }))
        return true;
    return statement_writes_through_alias(statement, path, false);
}

StorageWrites::IterableWrite StorageWrites::classify_iterable_write(
    const Stmt& statement, const Expr& iterable, const RootedPath& path) const {
    if (!statement_may_write_path(statement, path)) return IterableWrite::none;
    const auto blocks = [&](const Expr* condition,
                            std::initializer_list<const std::vector<StmtPtr>*> bodies) {
        if (condition && expression_may_write_path(*condition, path))
            return IterableWrite::in_place;
        auto kind = IterableWrite::none;
        for (const auto* body : bodies) {
            for (const auto& item : *body) {
                const auto inner = classify_iterable_write(*item, iterable, path);
                if (inner == IterableWrite::in_place) return inner;
                if (inner == IterableWrite::replaces) kind = inner;
            }
        }
        // The statement may write, so a write the nested statements do not
        // show is taken as in place.
        return kind == IterableWrite::none ? IterableWrite::in_place : kind;
    };
    const auto& data = statement.data;
    if (const auto* node = std::get_if<AssignStmt>(&data)) {
        // A plain assignment to the iterated binding, or to a binding or
        // field the array is reached through, replaces the array and leaves
        // the old one intact, when its value writes nothing on the path.
        if (!node->compound_op.empty()) return IterableWrite::in_place;
        bool prefix = false;
        for (const Expr* current = &iterable; current;) {
            if (same_member_path(*node->target, *current)) { prefix = true; break; }
            if (const auto* member = std::get_if<MemberExpr>(&current->data)) {
                current = member->base.get();
            } else if (const auto* index = std::get_if<IndexExpr>(&current->data)) {
                current = index->base.get();
            } else {
                current = nullptr;
            }
        }
        if (!prefix || expression_may_write_path(*node->value, path))
            return IterableWrite::in_place;
        return IterableWrite::replaces;
    }
    if (const auto* node = std::get_if<IfStmt>(&data))
        return blocks(node->condition.get(), {&node->then_body, &node->else_body});
    if (const auto* node = std::get_if<WhileStmt>(&data))
        return blocks(node->condition.get(), {&node->body});
    if (const auto* node = std::get_if<ForStmt>(&data)) {
        if (node->writable) return IterableWrite::in_place;
        return blocks(node->iterable.get(), {&node->body});
    }
    if (const auto* node = std::get_if<MatchStmt>(&data)) {
        if (expression_may_write_path(*node->value, path)) return IterableWrite::in_place;
        auto kind = IterableWrite::none;
        for (const auto& match_case : node->cases) {
            for (const auto& item : match_case.body) {
                const auto inner = classify_iterable_write(*item, iterable, path);
                if (inner == IterableWrite::in_place) return inner;
                if (inner == IterableWrite::replaces) kind = inner;
            }
        }
        return kind == IterableWrite::none ? IterableWrite::in_place : kind;
    }
    return IterableWrite::in_place;
}

bool StorageWrites::is_reference_binding(const std::string& name, const Expr& root) const {
    if (is_receiver_field(root)) return false;
    if (scope_.is_source_reference(name)) return true;
    if (const auto* function = builder_.function()) {
        for (const auto& parameter : function->parameters)
            if (parameter.writable && parameter.name == name) return true;
    }
    return false;
}

bool StorageWrites::is_receiver_field(const Expr& root) const {
    return checked_.field_accesses.contains(&root);
}

bool StorageWrites::path_may_be_aliased_by(
    const RootedPath& path, const std::string& name, const Expr& root) const {
    if (!is_reference_binding(name, root) && !is_reference_binding(path.name, *path.root))
        return false;
    const auto type = checked_.expr_types.find(&root);
    if (type == checked_.expr_types.end()) return true;
    for (const auto& held : path.types) {
        std::unordered_set<std::string> active;
        if (type_can_hold(type->second, held, active)) return true;
    }
    return false;
}

bool StorageWrites::type_can_hold(
    const Type& holder, const Type& held,
    std::unordered_set<std::string>& active) const {
    if (holder == held) return true;
    if (holder.kind == TypeKind::Array)
        return holder.first && type_can_hold(*holder.first, held, active);
    if (holder.kind == TypeKind::Union) {
        for (const auto& item : holder.cases)
            if (type_can_hold(item, held, active)) return true;
        return false;
    }
    if (holder.kind != TypeKind::Class || !active.insert(holder.class_name).second)
        return false;
    const auto found = checked_.classes.find(holder.class_name);
    if (found == checked_.classes.end()) return false;
    for (const auto& field : found->second.fields)
        if (type_can_hold(field.type, held, active)) return true;
    return false;
}

bool StorageWrites::type_holds_shared_region(
    const Type& type, std::unordered_set<std::string>& active) const {
    if (type.kind == TypeKind::Array)
        return type.first && type_holds_shared_region(*type.first, active);
    if (type.kind == TypeKind::Union) {
        for (const auto& item : type.cases)
            if (type_holds_shared_region(item, active)) return true;
        return false;
    }
    if (type.kind != TypeKind::Class) return false;
    const auto found = checked_.classes.find(type.class_name);
    if (found != checked_.classes.end() &&
        standard_class::is_ref_cell_instance(type.class_name, found->second.standard_library))
        return true;
    if (!active.insert(type.class_name).second) return false;
    if (found == checked_.classes.end()) return false;
    for (const auto& field : found->second.fields)
        if (type_holds_shared_region(field.type, active)) return true;
    return false;
}

bool StorageWrites::expression_type_holds_shared_region(const Expr& expression) const {
    const auto type = checked_.expr_types.find(&expression);
    if (type == checked_.expr_types.end()) return true;
    std::unordered_set<std::string> active;
    return type_holds_shared_region(type->second, active);
}

} // namespace quidra::lowering
