// Source-level keys of the census; see census_keys.hpp and README.md (Inputs).
//
// The keys read the checked program: the AST with the checker's types, call
// resolutions, method calls and receiver effects. Writes are found with the
// lowering's own mutation query (BorrowInference::expression_mutates_parameter:
// `&` arguments, receivers whose receiver_effect writes, file handles, tensor
// state), extended as each key states. Every answer errs toward listing a
// site.
#include "census_keys.hpp"
#include "target_copy_census.hpp"
#include "range_constraint_traversal.hpp"

#include "lowering/assignment_order.hpp"
#include "lowering/borrow_inference.hpp"
#include "lowering/syntax_queries.hpp"
#include "semantics/argument_isolation.hpp"
#include "quidra/language.hpp"
#include "quidra/member_function_names.hpp"
#include "quidra/standard_classes.hpp"
#include "quidra/traversal_plan.hpp"

#include <initializer_list>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace quidra::golden {
namespace {

template<class Field> bool reference_field(const Field& field) {
    if constexpr(requires{field.reference;})return field.reference;
    return false;
}

using lowering::source_key;

// The binding a place's storage belongs to: the name at the base of its
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

std::string root_key(const Expr& expression) {
    if(const auto* projection=std::get_if<TupleProjectionExpr>(&expression.data))return projection->subject;
    const auto* root = storage_root(expression);
    return root ? source_key(std::get<NameExpr>(root->data)) : std::string();
}

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
    } else if (const auto* node = std::get_if<ArrayComprehensionExpr>(&data)) {
        visit_array_comprehension(*node, [&](const Expr& child) { each_expression(child, visit); });
    } else if (const auto* node = std::get_if<ArrayExpr>(&data)) {
        for (const auto& item : node->elements) each(item);
    } else if (const auto* node = std::get_if<TupleExpr>(&data)) {
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
    } else if (const auto* node = std::get_if<ComparisonChainExpr>(&data)) {
        for (const auto& item : node->operands) each(item);
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

// Calls `visit` on every expression of `statement` and of the statements
// nested in it.
template <class Visit>
void each_statement_expression(const Stmt& statement, Visit& visit) {
    const auto expression = [&](const ExprPtr& item) {
        if (item) each_expression(*item, visit);
    };
    const auto block = [&](const std::vector<StmtPtr>& body) {
        for (const auto& item : body) each_statement_expression(*item, visit);
    };
    const auto pattern = [&](const std::optional<Pattern>& value) {
        if(value)visit_pattern(*value,[&](const Pattern& leaf){expression(leaf.place);});
    };
    const auto& data = statement.data;
    visit_statement_range_bounds(statement, [&](const Expr& bound) { each_expression(bound, visit); });
    if (const auto* node = std::get_if<BindingStmt>(&data)) {
        pattern(node->pattern);
        expression(node->value);
    } else if (const auto* node = std::get_if<AssignStmt>(&data)) {
        pattern(node->pattern);
        if(!node->pattern && node->target_expr())each_expression(*node->target_expr(),visit);
        expression(node->value);
    } else if (const auto* node = std::get_if<RebindStmt>(&data)) {
        expression(node->target);
    } else if (const auto* node = std::get_if<ReturnStmt>(&data)) {
        expression(node->value);
    } else if (const auto* node = std::get_if<ExprStmt>(&data)) {
        expression(node->value);
    } else if (const auto* node = std::get_if<IfStmt>(&data)) {
        expression(node->condition);
        block(node->then_body);
        block(node->else_body);
    } else if (const auto* node = std::get_if<WhileStmt>(&data)) {
        expression(node->condition);
        block(node->body);
    } else if (const auto* node = std::get_if<ParallelStmt>(&data)) {
        block(node->body);
    } else if (const auto* node = std::get_if<ForStmt>(&data)) {
        pattern(node->pattern);
        visit_for_header(*node,[&](const Expr& operand){each_expression(operand,visit);});
        block(node->body);
    } else if (const auto* node = std::get_if<MatchStmt>(&data)) {
        expression(node->value);
        for (const auto& match_case : node->cases){visit_type_range_bounds(match_case.type,[&](const Expr& bound){each_expression(bound,visit);});pattern(match_case.pattern);block(match_case.body);}
    } else if (const auto* node = std::get_if<MainGuardStmt>(&data)) {
        block(node->body);
    }
}

// Calls `visit` on `statement` and every statement nested in it.
template <class Visit>
void each_statement(const Stmt& statement, Visit& visit) {
    visit(statement);
    const auto block = [&](const std::vector<StmtPtr>& body) {
        for (const auto& item : body) each_statement(*item, visit);
    };
    const auto& data = statement.data;
    if (const auto* node = std::get_if<IfStmt>(&data)) {
        block(node->then_body);
        block(node->else_body);
    } else if (const auto* node = std::get_if<WhileStmt>(&data)) {
        block(node->body);
    } else if (const auto* node = std::get_if<ForStmt>(&data)) {
        block(node->body);
    } else if (const auto* node = std::get_if<MatchStmt>(&data)) {
        for (const auto& match_case : node->cases) block(match_case.body);
    } else if (const auto* node = std::get_if<MainGuardStmt>(&data)) {
        block(node->body);
    } else if (const auto* node = std::get_if<ParallelStmt>(&data)) {
        block(node->body);
    }
}

bool effect_writes(const StorageEffect& effect) {
    return !effect.writes.empty() || !effect.initializes.empty() || !effect.invalidates.empty();
}

// A declared type as the census compares it with a checked type's name.
std::string declared_name(const TypeName& type) {
    std::string name(canonical_builtin_type_name(type.name).value_or(type.name));
    for (std::size_t i = 0; i < type.array_depth; ++i) name += "[]";
    return name;
}

// What a binding in scope is.
struct Binding {
    // A `&` parameter, a reference local or a reference loop variable.
    bool reference{};
    // A reference local (`T &r = &place`).
    bool reference_local{};
    // For a reference local: the root binding of the storage it designates,
    // empty when the initializer names no place.
    std::string target_root;
};

class SourceKeys {
public:
    SourceKeys(const CheckedProgram& checked, CensusKeys& keys)
        : checked_(checked), borrows_(checked), isolation_(checked, false), keys_(keys) {}

    void run() {
        const auto& program = checked_.program;
        keys_["autograd-target-copy"]=target_copy_sites(checked_);
        for (const auto& function : program.functions) {
            in_method_ = nullptr;
            walk_function(function, function.source_file);
        }
        for (const auto& class_decl : program.classes) {
            for (const auto& method : class_decl.methods) {
                in_method_ = &class_decl;
                walk_function(method, method.source_file.empty() ? class_decl.source_file
                                                                 : method.source_file);
            }
        }
        in_method_ = nullptr;
        file_ = program.root_source_file;
        scopes_.assign(1, {});
        walk_block(program.statements);
    }

private:
    // ----- types

    const Type* type_of(const Expr& expression) const {
        const auto found = checked_.expr_types.find(&expression);
        return found == checked_.expr_types.end() ? nullptr : &found->second;
    }

    const ClassTypeInfo* class_info(const Type& type) const {
        if (type.kind != TypeKind::Class) return nullptr;
        const auto found = checked_.classes.find(type.class_name);
        return found == checked_.classes.end() ? nullptr : &found->second;
    }

    bool type_can_hold(const Type& holder, const Type& held, std::unordered_set<std::string>& active) const {
        if (holder == held) return true;
        if (holder.kind == TypeKind::Array)
            return holder.first && type_can_hold(*holder.first, held, active);
        if (holder.kind == TypeKind::Tuple) {
            for(const auto& item:holder.parameters)if(type_can_hold(item,held,active))return true;
            return false;
        }
        if (holder.kind == TypeKind::Union) {
            for (const auto& item : holder.cases)
                if (type_can_hold(item, held, active)) return true;
            return false;
        }
        const auto* info = class_info(holder);
        if (!info || !active.insert(holder.class_name).second) return false;
        for (const auto& field : info->fields)
            if (type_can_hold(field.type, held, active)) return true;
        return false;
    }
    bool can_hold(const Type* holder, const Type& held) const {
        if (!holder) return true;
        std::unordered_set<std::string> active;
        return type_can_hold(*holder, held, active);
    }

    bool holds_shared_region(const Type& type, std::unordered_set<std::string>& active) const {
        if (type.kind == TypeKind::Array) return type.first && holds_shared_region(*type.first, active);
        if (type.kind == TypeKind::Tuple) {
            for(const auto& item:type.parameters)if(holds_shared_region(item,active))return true;
            return false;
        }
        if (type.kind == TypeKind::Union) {
            for (const auto& item : type.cases)
                if (holds_shared_region(item, active)) return true;
            return false;
        }
        const auto* info = class_info(type);
        if (!info) return false;
        if (!active.insert(type.class_name).second) return false;
        for (const auto& field : info->fields)
            if (reference_field(field) || holds_shared_region(field.type, active)) return true;
        return false;
    }
    bool expression_holds_shared_region(const Expr& expression) const {
        const auto* type = type_of(expression);
        if (!type) return true;
        std::unordered_set<std::string> active;
        return holds_shared_region(*type, active);
    }

    // ----- calls

    const StorageEffect* receiver_effect(const Expr& call) const {
        const auto method = checked_.method_calls.find(&call);
        if (method == checked_.method_calls.end()) return nullptr;
        const auto signature = checked_.functions.find(method->second.internal_name);
        return signature == checked_.functions.end() ? nullptr : &signature->second.receiver_effect;
    }
    // A user method call (explicit or implicit receiver) that may write its receiver.
    bool writing_method_call(const Expr& call) const {
        if (!checked_.method_calls.contains(&call)) return false;
        const auto* effect = receiver_effect(call);
        return !effect || effect_writes(*effect);
    }
    // An implicit-receiver call inside a method: a CallExpr the checker
    // resolved to a method of the receiver.
    bool implicit_receiver_call(const Expr& call) const {
        return std::holds_alternative<CallExpr>(call.data) && checked_.method_calls.contains(&call);
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

    // ----- scopes

    const Binding* lookup(const std::string& name) const {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
            if (const auto found = scope->find(name); found != scope->end()) return &found->second;
        }
        return nullptr;
    }
    bool reference_binding(const std::string& name) const {
        const auto* binding = lookup(name);
        return binding && binding->reference;
    }
    bool receiver_field(const Expr& name) const { return checked_.field_accesses.contains(&name); }

    std::string site(const SourceSpan& span) const {
        return (file_.empty() ? std::string("-") : file_) + ":" + std::to_string(span.start.line) +
               ":" + std::to_string(span.start.column);
    }
    void list(const char* key, const SourceSpan& span) { keys_[key].push_back(site(span)); }

    // ----- walk

    void walk_function(const FunctionDecl& function, const std::string& file) {
        file_ = file;
        scopes_.assign(1, {});
        for (const auto& parameter : function.parameters) {
            scopes_.back()[parameter.name] = Binding{parameter.writable, false, {}};
        }
        for (const auto& parameter : function.parameters)
            visit_parameter_range_bounds(parameter, [&](const Expr& bound) { walk_expression(bound); });
        visit_type_range_bounds(function.return_type, [&](const Expr& bound) { walk_expression(bound); });
        walk_block(function.body);
    }

    void walk_block(const std::vector<StmtPtr>& body) {
        scopes_.emplace_back();
        for (const auto& statement : body) walk_statement(*statement);
        scopes_.pop_back();
    }

    void walk_statement(const Stmt& statement) {
        const auto expression = [&](const ExprPtr& item) {
            if (item) walk_expression(*item);
        };
        const auto& data = statement.data;
        visit_statement_range_bounds(statement, [&](const Expr& bound) { walk_expression(bound); });
        if (const auto* node = std::get_if<BindingStmt>(&data)) {
            expression(node->value);
            if(node->pattern){
                const auto found=checked_.pattern_expansions.find(&statement);
                if(found!=checked_.pattern_expansions.end())walk_pattern(found->second);
                return;
            }
            Binding binding;
            binding.reference = node->reference || node->reference_initializer;
            binding.reference_local = binding.reference;
            if (binding.reference && node->value) binding.target_root = root_key(*node->value);
            scopes_.back()[node->name] = binding;
        } else if (const auto* node = std::get_if<AssignStmt>(&data)) {
            if(node->pattern){
                expression(node->value);
                const auto found=checked_.pattern_expansions.find(&statement);
                if(found!=checked_.pattern_expansions.end())walk_pattern(found->second);
            }else {
                if(node->target_expr())walk_expression(*node->target_expr());
                expression(node->value);
            }
            if (node->compound_op.empty()) {
                assign_order(*node, statement.span);
            }
        } else if (const auto* node = std::get_if<RebindStmt>(&data)) {
            expression(node->target);
        } else if (const auto* node = std::get_if<ReturnStmt>(&data)) {
            expression(node->value);
        } else if (const auto* node = std::get_if<ExprStmt>(&data)) {
            expression(node->value);
        } else if (const auto* node = std::get_if<IfStmt>(&data)) {
            expression(node->condition);
            walk_block(node->then_body);
            walk_block(node->else_body);
        } else if (const auto* node = std::get_if<WhileStmt>(&data)) {
            expression(node->condition);
            walk_block(node->body);
        } else if (const auto* node = std::get_if<ForStmt>(&data)) {
            visit_for_header(*node,[&](const Expr& operand){walk_expression(operand);});
            walk_for(*node, statement.span);
        } else if (const auto* node = std::get_if<MatchStmt>(&data)) {
            expression(node->value);
            for (const auto& match_case : node->cases) {
                scopes_.emplace_back();
                visit_type_range_bounds(match_case.type, [&](const Expr& bound) { walk_expression(bound); });
                if(match_case.pattern){
                    const auto found=checked_.control_patterns.find(&*match_case.pattern);
                    if(found!=checked_.control_patterns.end())walk_pattern(found->second);
                }else if (match_case.binder) scopes_.back()[*match_case.binder] = Binding{};
                walk_block(match_case.body);
                scopes_.pop_back();
            }
        } else if (const auto* node = std::get_if<ParallelStmt>(&data)) {
            walk_block(node->body);
        } else if (const auto* node = std::get_if<MainGuardStmt>(&data)) {
            const auto saved = file_;
            if (!node->source_file.empty()) file_ = node->source_file;
            walk_block(node->body);
            file_ = saved;
        }
    }

    void walk_expression(const Expr& root) {
        const auto visit = [&](const Expr& expression) {
            if (expression.contextual_default_type && expression.contextual_default_type->name != "auto") {
                const auto* type = type_of(expression);
                if (type && type_name(*type) != declared_name(*expression.contextual_default_type))
                    list("typed-constant-retyped", expression.span);
            }
            isolation_copies(expression);
            if(const auto* call=std::get_if<MethodCallExpr>(&expression.data);call&&call->method=="backward")backward_targets(*call);
        };
        each_expression(root, visit);
    }

    // ----- argument isolation

    // The by-value arguments a call passes as a copy because the call may
    // write their storage (D12): borrowed parameters of functions and
    // methods, given storage of exactly the parameter's type, that the
    // effect analysis isolates; the decision of the call lowering.
    void isolation_copies(const Expr& expression) {
        if (!checked_.effects) return;
        std::string callee;
        std::size_t offset = 0;
        const std::vector<CallArg>* args = nullptr;
        if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
            args = &call->args;
            if (const auto method = checked_.method_calls.find(&expression);
                method != checked_.method_calls.end()) {
                callee = method->second.internal_name;
                offset = 1;
            } else if (const auto found = checked_.call_resolutions.find(&expression);
                       found != checked_.call_resolutions.end() &&
                       found->second.kind == CallKind::Function) {
                callee = found->second.target;
            }
        } else if (const auto* call = std::get_if<MethodCallExpr>(&expression.data)) {
            args = &call->args;
            if (const auto method = checked_.method_calls.find(&expression);
                method != checked_.method_calls.end()) {
                callee = method->second.internal_name;
                offset = 1;
            }
        }
        if (callee.empty()) return;
        const auto signature = checked_.functions.find(callee);
        if (signature == checked_.functions.end()) return;
        const auto& parameters = signature->second.parameters;
        std::vector<bool> filled(parameters.size(), false);
        for (std::size_t i = 0; i < offset && i < filled.size(); ++i) filled[i] = true;
        std::size_t positional = offset;
        for (const auto& argument : *args) {
            std::size_t target = parameters.size();
            if (argument.name) {
                for (std::size_t i = 0; i < parameters.size(); ++i)
                    if (parameters[i].name == *argument.name) target = i;
            } else {
                while (positional < filled.size() && filled[positional]) ++positional;
                target = positional++;
            }
            if (target >= parameters.size()) continue;
            filled[target] = true;
            const auto& parameter = parameters[target];
            if (parameter.writable || argument.writable || !storage_root(*argument.value)) continue;
            if (!borrows_.parameter_is_borrowed(callee, target)) continue;
            const auto raw = checked_.raw_types.find(argument.value.get());
            const auto* type = type_of(*argument.value);
            if (raw == checked_.raw_types.end() || !type || raw->second != parameter.type ||
                *type != parameter.type)
                continue;
            if (isolation_.copies(*argument.value)) list("argument-isolation-copy", argument.value->span);
        }
    }

    // ----- autograd

    void backward_targets(const MethodCallExpr& call) {
        const auto* receiver = type_of(*call.receiver);
        if (!receiver || receiver->kind != TypeKind::Tensor) return;
        for (const auto& argument : call.args) {
            if (!argument.writable || argument.name) continue;
            const auto* type = type_of(*argument.value);
            if (!type || (type->kind != TypeKind::Class&&type->kind != TypeKind::Array&&type->kind != TypeKind::Tuple)) continue;
            try {
                make_traversal_plan(*type,Type::class_type(standard_class::autograd_target),checked_.classes,{true,true,{}});
            }catch(const TraversalError&) {list("backward-unvisitable-target",argument.value->span);}
        }
    }

    // ----- collection loops

    void walk_pattern(const PatternExpansion& expansion) {
        scopes_.back()[expansion.subject]=Binding{};
        for(const auto& binding:expansion.bindings)walk_statement(*binding);
    }

    void walk_for(const ForStmt& loop, const SourceSpan& span) {
        if (loop.writable) reference_loop(loop, span);
        scopes_.emplace_back();
        if(loop.pattern){
            const auto found=checked_.control_patterns.find(&*loop.pattern);
            if(found!=checked_.control_patterns.end())walk_pattern(found->second);
        }else scopes_.back()[loop.name] = Binding{loop.writable, false, {}};
        for(const auto& binding:loop.collections) if(binding.name!="_") scopes_.back()[binding.name]=Binding{};
        walk_block(loop.body);
        scopes_.pop_back();
    }

    void reference_loop(const ForStmt& loop, const SourceSpan& span) {
        const auto* iterated = type_of(*loop.iterable);
        const auto root = root_key(*loop.iterable);
        const auto* root_expression = storage_root(*loop.iterable);
        const Type* element = iterated && iterated->kind == TypeKind::Array && iterated->first
                                  ? iterated->first.get() : nullptr;

        // B0.3: every reference loop over a reference binding, with the body
        // statements that may replace or resize the array it designates.
        const auto* binding = std::holds_alternative<NameExpr>(loop.iterable->data) ? lookup(root) : nullptr;
        if (binding && binding->reference && iterated) {
            list("reference-loop-alias-call", span);
            for (const auto& statement : loop.body) {
                if (may_resize_iterated(*statement, root, *binding, *iterated))
                    list("reference-loop-alias-call.statements", statement->span);
            }
        }

        // F2b: reference loops whose body may reach the current element
        // through another path.
        const bool receiver_rooted =
            (root_expression && receiver_field(*root_expression)) || reference_binding(root);
        const bool shared = element && [&] {
            std::unordered_set<std::string> active;
            return holds_shared_region(*element, active);
        }();
        bool aliased = false;
        const auto visit = [&](const Expr& expression) {
            if (aliased) return;
            if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
                const auto key = source_key(*name);
                if (key == root) { aliased = true; return; }
                if (key != loop.name && reference_binding(key) && (!element || can_hold(type_of(expression), *element)))
                    aliased = true;
                return;
            }
            const auto writable_alias = [&](const CallArg& argument) {
                return argument.writable && root_key(*argument.value) != loop.name &&
                       (!element || can_hold(type_of(*argument.value), *element));
            };
            if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
                for (const auto& argument : call->args) aliased = aliased || writable_alias(argument);
                if (receiver_rooted && implicit_receiver_call(expression) && writing_method_call(expression))
                    aliased = true;
                if (shared && user_call(expression)) {
                    for (const auto& argument : call->args)
                        aliased = aliased || expression_holds_shared_region(*argument.value);
                }
            } else if (const auto* call = std::get_if<MethodCallExpr>(&expression.data)) {
                for (const auto& argument : call->args) aliased = aliased || writable_alias(argument);
                if (writing_method_call(expression) && root_key(*call->receiver) != loop.name &&
                    (!element || can_hold(type_of(*call->receiver), *element)))
                    aliased = true;
                if (shared && user_call(expression)) {
                    aliased = aliased || expression_holds_shared_region(*call->receiver);
                    for (const auto& argument : call->args)
                        aliased = aliased || expression_holds_shared_region(*argument.value);
                }
            }
        };
        for (const auto& statement : loop.body) each_statement_expression(*statement, visit);
        if (aliased) list("reference-loop-current-element-alias", span);
    }

    // B0.3's statements: (a) a call that may write storage of the iterated
    // array's type through another `&` argument or a writing receiver; (b) an
    // assignment to another reference binding whose type can hold the array,
    // or to storage reached through one; (c) an assignment whose target may
    // contain a reference local's target (every assignment when the target
    // is not a place).
    bool may_resize_iterated(const Stmt& statement, const std::string& root, const Binding& binding,
                             const Type& iterated) const {
        bool found = false;
        const auto visit = [&](const Expr& expression) {
            if (found) return;
            const auto other_writable = [&](const CallArg& argument) {
                const auto key = root_key(*argument.value);
                return argument.writable && key != root && can_hold(type_of(*argument.value), iterated);
            };
            if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
                for (const auto& argument : call->args) found = found || other_writable(argument);
                if (in_method_ && implicit_receiver_call(expression) && writing_method_call(expression)) {
                    const auto receiver = Type::class_type(in_method_->name);
                    found = found || can_hold(&receiver, iterated);
                }
            } else if (const auto* call = std::get_if<MethodCallExpr>(&expression.data)) {
                for (const auto& argument : call->args) found = found || other_writable(argument);
                if (writing_method_call(expression) && root_key(*call->receiver) != root &&
                    can_hold(type_of(*call->receiver), iterated))
                    found = true;
            }
        };
        each_statement_expression(statement, visit);
        if (found) return true;
        const auto assignments = [&](const Stmt& inner) {
            const auto* assign = std::get_if<AssignStmt>(&inner.data);
            if (!assign || found) return;
            const auto target = [&](const Expr& place) {
                const auto* target_root = storage_root(place);
                const auto key = target_root ? source_key(std::get<NameExpr>(target_root->data)) : std::string();
                if (target_root && key != root && reference_binding(key) &&
                    can_hold(type_of(*target_root), iterated))
                    found = true;
                if (binding.reference_local && (binding.target_root.empty() || key == binding.target_root))
                    found = true;
            };
            if(assign->pattern)visit_pattern(*assign->pattern,[&](const Pattern& leaf){if(leaf.place)target(*leaf.place);});
            else if(assign->target_expr())target(*assign->target_expr());
        };
        each_statement(statement, assignments);
        return found;
    }

    // ----- assignment order

    // Plain assignments whose target subexpressions (the index and slice
    // operands on the target's path) and right-hand side interact, in two
    // tiers, by the lowering's own query (lowering/assignment_order.hpp), so
    // the sites the lowering reorders are the sites listed: tier 1 when either side may write a variable the other reads or writes,
    // or both contain a call; tier 2 when one side may fail and the other
    // contains a call or may fail too.
    void assign_order(const AssignStmt& assignment, const SourceSpan& span) {
        const auto tier = lowering::AssignmentOrder(checked_, borrows_).classify(assignment);
        if (tier == lowering::AssignmentOrder::Tier::none) return;
        list("assign-order", span);
        list(tier == lowering::AssignmentOrder::Tier::every_execution ? "assign-order.tier1"
                                                                      : "assign-order.tier2",
             span);
    }

    const CheckedProgram& checked_;
    lowering::BorrowInference borrows_;
    semantics::ArgumentIsolation isolation_;
    CensusKeys& keys_;
    std::vector<std::unordered_map<std::string, Binding>> scopes_;
    std::string file_;
    const ClassDecl* in_method_{};

};

} // namespace

void source_keys(const CheckedProgram& checked, CensusKeys& keys,
                 std::map<std::string, std::size_t>& kinds) {
    SourceKeys(checked, keys).run();
    for (const auto& [expression, type] : checked.expr_types) {
        if (numeric_info(type)) {
            ++kinds[type_name(type)];
        } else if (type.kind == TypeKind::Tensor && type.first && numeric_info(*type.first)) {
            ++kinds["tensor." + type_name(*type.first)];
        }
    }
}

} // namespace quidra::golden
