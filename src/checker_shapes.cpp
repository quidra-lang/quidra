// Brace shapes (L17): the axes of `tensor<ELEM>{AXIS, ...}` in a binding or a
// match case, checked as one scope unit. `nat N` and `nat N = EXPR` declare
// new read-only nat shape variables; a bare name refers to one of them or to
// an existing nat value; any other expression is an unnamed nat constraint;
// `_` is an extent of any length. The result, a ShapePlan, tells the
// lowering what to evaluate before and after the initializer and what to
// check.

#include "quidra/checker.hpp"

#include "quidra/abi/symbols.hpp"
#include "quidra/language.hpp"
#include "constant_integer_eval.hpp"
#include "numeric_literal_policy.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace quidra {
namespace {

using numeric_policy::NumericLiteralCategory;
using numeric_policy::numeric_literal_family;

Type simple(TypeKind kind) { return Type::simple(kind); }
bool poisoned(const Type& type) { return type.kind == TypeKind::Invalid; }

// Independent literal and wildcard axes are already represented by their codes.
void clear_redundant_shape_key(Type& type) {
    if (type.tensor_shape_prefix.empty()) { type.tensor_shape_key.clear(); return; }
    std::string key = "<";
    for (std::size_t axis = 0; axis < type.tensor_shape_prefix.size(); ++axis) {
        const auto extent = type.tensor_shape_prefix[axis];
        if (extent < -1) return;
        if (axis) key += ",";
        key += extent == -1 ? "_" : std::to_string(extent);
    }
    key += ">";
    if (type.tensor_shape_key == key) type.tensor_shape_key.clear();
}

bool declares(const ShapeAxis& axis) {
    return axis.kind == ShapeAxisKind::Declare || axis.kind == ShapeAxisKind::DeclareEqual;
}

// The unqualified names an expression reads.
void collect_names(const Expr& expression, std::vector<std::string>& names) {
    std::visit(
        [&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, NameExpr>) {
                if (!node.this_qualifier) names.push_back(node.name);
            } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                collect_names(*node.operand, names);
            } else if constexpr (std::is_same_v<T, BinaryExpr>) {
                collect_names(*node.left, names);
                collect_names(*node.right, names);
            } else if constexpr (std::is_same_v<T, CallExpr>) {
                for (const auto& argument : node.args) collect_names(*argument.value, names);
            } else if constexpr (std::is_same_v<T, MethodCallExpr>) {
                collect_names(*node.receiver, names);
                for (const auto& argument : node.args) collect_names(*argument.value, names);
            } else if constexpr (std::is_same_v<T, MemberExpr>) {
                collect_names(*node.base, names);
            } else if constexpr (std::is_same_v<T, IndexExpr>) {
                collect_names(*node.base, names);
                for (const auto& item : node.items) {
                    if (item.index) collect_names(*item.index, names);
                    if (item.start) collect_names(*item.start, names);
                    if (item.stop) collect_names(*item.stop, names);
                    if (item.step) collect_names(*item.step, names);
                }
            } else if constexpr (std::is_same_v<T,ArrayComprehensionExpr>) {
                collect_names(*node.source, names);
                std::vector<std::string> inner;
                if (node.condition) collect_names(*node.condition, inner);
                collect_names(*node.value, inner);
                for (const auto& name : inner) if (name != node.name) names.push_back(name);
            } else if constexpr ((std::is_same_v<T,ArrayExpr> || std::is_same_v<T,TupleExpr>)) {
                for (const auto& element : node.elements) collect_names(*element, names);
            } else if constexpr (std::is_same_v<T, TryExpr>) {
                collect_names(*node.value, names);
            } else if constexpr (std::is_same_v<T, IfExpr>) {
                for (const auto& condition : node.conditions) collect_names(*condition, names);
                for (const auto& value : node.values) collect_names(*value, names);
                collect_names(*node.otherwise, names);
            } else if constexpr (std::is_same_v<T, StringTemplateExpr>) {
                for (const auto& part : node.expressions) collect_names(*part, names);
            }
        },
        expression.data);
}

// `nat(e)` with one positional operand: the conversion an extent over another
// integer kind is written with.
const Expr* nat_conversion_operand(const Expr& expression) {
    const auto* call = std::get_if<CallExpr>(&expression.data);
    if (!call || call->callee != "nat" || !call->type_arguments.empty() ||
        call->args.size() != 1 || call->args[0].name || call->args[0].writable) {
        return nullptr;
    }
    return call->args[0].value.get();
}

std::string article(const std::string& word) {
    return std::string(word.empty() || std::string_view("aeiou").find(word.front()) ==
                                             std::string_view::npos
                           ? "a "
                           : "an ") +
           word;
}

} // namespace

bool Checker::is_shape_variable(const std::string& name) const {
    return shape_variables_.contains(name) && variables_.contains(name);
}

void Checker::forget_shape_variable(const std::string& name) {
    shape_variables_.erase(name);
}

void Checker::note_shape_variable_read(const std::string& name) {
    if (!is_shape_variable(name)) return;
    const auto& ref = shape_variables_.at(name);
    ref.plan->variables[ref.index].read = true;
}

// An array extent nat(e) over an integer e is captured as e, which the
// extent check then requires to be non-negative: today's capture of an
// integer extent (Q54, Q64).
void Checker::note_extent_capture_operand(const Expr& expression) {
    const auto* operand = nat_conversion_operand(expression);
    if (!operand) return;
    const auto found = raw_types_.find(operand);
    if (found == raw_types_.end() || !is_integer_family_type(found->second) ||
        found->second.kind == TypeKind::Nat) {
        return;
    }
    extent_capture_operands_[&expression] = operand;
}

void Checker::settle_extent_conversion(const Expr& expression, Type& type) {
    const auto nat = simple(TypeKind::Nat);
    if (!nat_conversion_operand(expression) || type.kind != TypeKind::Union ||
        !type.union_name.empty() || case_index(type, nat) < 0 ||
        case_index(type, simple(TypeKind::Error)) < 0) {
        return;
    }
    expr_types_[&expression] = nat;
    type = nat;
}

// Every axis form but literals and `_` belongs to the shape of a binding or
// a match case until signatures, constructors, extern declarations and fn
// types take shape variables.
void Checker::reject_transitional_brace_axes(const TypeName& source) const {
    if (!source.brace_shape || &source == brace_shape_unit_ || signature_shape_resolution_) return;
    for (const auto& axis : source.shape_axes) {
        if (axis.kind == ShapeAxisKind::Discard) continue;
        if (axis.kind == ShapeAxisKind::Unnamed && axis.value &&
            std::holds_alternative<IntegerExpr>(axis.value->data)) {
            continue;
        }
        error("SHAPE_VARIABLE",
              "This axis form arrives with shape variables; write an integer literal, or the "
              "old '<...>' form, for now.",
              axis.span);
    }
}

// `nat N` over a name that is already visible (L17 REVISION 2: SHADOWING),
// with the hint that fits what the name is.
void Checker::reject_visible_shape_variable_name(const std::string& name, SourceSpan span) {
    if (name.starts_with(abi::symbol_namespace::internal_prefix)) {
        error("SHADOWING", compiler_name_message(name), span);
    }
    const auto declared = [&](const std::string& what) {
        error("SHADOWING",
              "'" + name + "' already names " + what + "; 'nat " + name +
                  "' would declare a new shape variable. Choose another name.",
              span);
    };
    if (const auto found = variables_.find(name); found != variables_.end()) {
        const auto& type = found->second;
        if (type.kind == TypeKind::Nat) {
            error("SHADOWING",
                  "'" + name + "' already names a nat value; 'nat " + name +
                      "' would declare a new shape variable. To require equality with it, "
                      "write the bare name '" + name + "'.",
                  span);
        }
        if (is_integer_family_type(type)) {
            error("SHADOWING",
                  "'" + name + "' already names " + article(type_name(type)) +
                      " binding; 'nat " + name +
                      "' would declare a new shape variable. To require equality with its "
                      "value, write 'nat(" + name + ")'.",
                  span);
        }
        declared(article(type_name(type)) + " binding");
    }
    if (is_builtin_text_constant(name)) declared("a built-in text constant");
    if (declaration_name_visible(name)) {
        const auto scoped = current_module_namespace_.empty()
                                ? name
                                : current_module_namespace_ + "." + name;
        declared(functions_.contains(scoped) ? "a function"
                 : enum_types_.contains(scoped) ? "an enum"
                                                : "a class");
    }
    if (member_name_visible(name)) declared("a method");
    if (is_reserved_value_name(name)) declared("a built-in name");
}

// A bare axis name that is not a shape variable of its own shape (REVISION
// 2): an existing shape variable or nat value means equality; any other
// binding is an error, and so is a name that names nothing.
void Checker::resolve_bare_axis_name(const ShapeAxis& axis) {
    const auto& name = axis.name;
    if (const auto found = variables_.find(name); found != variables_.end()) {
        const auto& type = found->second;
        if (poisoned(type) || type.kind == TypeKind::Nat) return;
        if (is_integer_family_type(type)) {
            error("SHAPE_VARIABLE",
                  "'" + name + "' is " + type_name(type) +
                      "; a bare axis name refers only to a nat value: write 'nat(" + name + ")'.",
                  axis.span);
        }
        error("SHAPE_VARIABLE", "'" + name + "' is not a nat value.", axis.span);
    }
    if (is_builtin_text_constant(name)) {
        error("SHAPE_VARIABLE",
              "'" + name +
                  "' is a built-in text constant, not a nat value; choose another axis name.",
              axis.span);
    }
    if (declaration_name_visible(name) || is_reserved_value_name(name) ||
        member_name_visible(name) || visible_function_name(name)) {
        error("SHAPE_VARIABLE", "'" + name + "' is not a nat value.", axis.span);
    }
    error("SHAPE_VARIABLE",
          "'" + name + "' is not declared: write 'nat " + name +
              "' to declare a shape variable, or use an existing nat value.",
          axis.span);
}

// An axis expression is nat: integer literals materialize into nat, nat(e)
// converts another integer kind, and anything else is an error.
Type Checker::check_shape_axis_expression(const Expr& expression) {
    const auto nat = simple(TypeKind::Nat);
    if (numeric_literal_family(expression) != NumericLiteralCategory::None) {
        return check_expr(expression, &nat);
    }
    auto type = check_expr(expression);
    if (legacy_signature_axes_.contains(&expression) && is_integer_family_type(type)) return type;
    if (poisoned(type) || type.kind == TypeKind::Nat) return type;
    // nat(e) of a signed integer: the axis captures e, whose negative value
    // stops with the extent check (Q54), so no error remains.
    settle_extent_conversion(expression, type);
    if (type.kind == TypeKind::Nat) return type;
    error("TYPE_MISMATCH", "A tensor axis is a nat value: write 'nat(n)'.", expression.span);
}

ShapePlan& Checker::check_brace_shape(const TypeName& source, Type& type, bool has_value) {
    auto& plan = shape_plans_[&source];
    plan = ShapePlan{};
    plan.ellipsis = source.tensor_shape_ellipsis;
    const auto& axes = source.shape_axes;
    plan.axes.resize(axes.size());

    // 1. The unit's declarations, before any name is resolved: a later axis
    //    may name a variable that an earlier one uses.
    std::unordered_map<std::string, std::size_t> unit;
    for (std::size_t i = 0; i < axes.size(); ++i) {
        const auto& axis = axes[i];
        if (!declares(axis)) continue;
        if (is_discard_name(axis.name)) {
            error("DISCARD",
                  axis.kind == ShapeAxisKind::Declare
                      ? std::string(discard_typed_message)
                      : std::string("'_' names nothing: write the axis expression without "
                                    "'nat _ ='."),
                  axis.span);
        }
        if (unit.contains(axis.name)) {
            error("SHAPE_VARIABLE",
                  "'" + axis.name + "' is declared twice; declare it once with 'nat " +
                      axis.name + "' and write the bare '" + axis.name +
                      "' for an equal axis.",
                  axis.span);
        }
        reject_visible_shape_variable_name(axis.name, axis.span);
        if (axis.kind == ShapeAxisKind::Declare && !has_value) {
            error("SHAPE_VARIABLE",
                  "'" + axis.name +
                      "' has no value: without an initializer, write a literal, an "
                      "expression, 'nat " + axis.name + " = ...' or an existing nat value.",
                  axis.span);
        }
        unit[axis.name] = plan.variables.size();
        plan.variables.push_back(ShapeVariable{
            axis.name, i, axis.kind == ShapeAxisKind::DeclareEqual ? axis.value.get() : nullptr,
            false});
    }

    // 2. Every expression and bare name, with the unit's variables visible
    //    as read-only nat values (reading one marks it read).
    std::vector<std::pair<std::string, std::optional<Type>>> hidden;
    std::vector<std::pair<std::string, std::optional<ShapeVariableRef>>> hidden_refs;
    for (const auto& [name, index] : unit) {
        const auto found = variables_.find(name);
        hidden.emplace_back(name, found == variables_.end() ? std::nullopt
                                                            : std::optional<Type>{found->second});
        const auto ref = shape_variables_.find(name);
        hidden_refs.emplace_back(name, ref == shape_variables_.end()
                                           ? std::nullopt
                                           : std::optional<ShapeVariableRef>{ref->second});
        variables_[name] = simple(TypeKind::Nat);
        shape_variables_[name] = ShapeVariableRef{&plan, index, axes[plan.variables[index].axis].span};
    }
    std::vector<std::string> initialized_names;
    for (const auto& [name, index] : unit) {
        if (initialized_.insert(name).second) initialized_names.push_back(name);
    }
    const auto restore = [&]() {
        for (const auto& [name, type_before] : hidden) {
            if (type_before) variables_[name] = *type_before;
            else variables_.erase(name);
        }
        for (const auto& [name, ref] : hidden_refs) {
            if (ref) shape_variables_[name] = *ref;
            else shape_variables_.erase(name);
        }
        for (const auto& name : initialized_names) initialized_.erase(name);
    };

    // The unit variables each expression reads.
    std::vector<std::vector<std::size_t>> reads(axes.size());
    try {
        for (std::size_t i = 0; i < axes.size(); ++i) {
            const auto& axis = axes[i];
            auto& current = plan.axes[i];
            const auto own_reads = [&](const Expr& expression) {
                std::vector<std::string> names;
                collect_names(expression, names);
                std::vector<std::size_t> out;
                for (const auto& name : names) {
                    if (const auto found = unit.find(name); found != unit.end())
                        out.push_back(found->second);
                }
                std::sort(out.begin(), out.end());
                out.erase(std::unique(out.begin(), out.end()), out.end());
                return out;
            };
            switch (axis.kind) {
                case ShapeAxisKind::Discard:
                case ShapeAxisKind::Declare:
                    break;
                case ShapeAxisKind::DeclareEqual: {
                    check_shape_axis_expression(*axis.value);
                    reads[i] = own_reads(*axis.value);
                    current.check = ShapeAxisCheck::Value;
                    current.value = axis.value.get();
                    current.variable = axis.name;
                    current.variable_index = unit.at(axis.name);
                    current.after_initializer = !reads[i].empty();
                    break;
                }
                case ShapeAxisKind::Bare: {
                    if (const auto found = unit.find(axis.name); found != unit.end()) {
                        current.check = ShapeAxisCheck::Variable;
                        current.variable = axis.name;
                        current.variable_index = found->second;
                        plan.variables[found->second].read = true;
                        reads[i] = {found->second};
                        break;
                    }
                    resolve_bare_axis_name(axis);
                    check_expr(*axis.value);
                    current.check = ShapeAxisCheck::Value;
                    current.value = axis.value.get();
                    if (is_shape_variable(axis.name)) current.variable = axis.name;
                    break;
                }
                case ShapeAxisKind::Unnamed: {
                    if (std::holds_alternative<IntegerExpr>(axis.value->data)) {
                        // A literal axis: today's literal constraint, an int
                        // capture checked against the extent.
                        const auto int_type = simple(TypeKind::Int);
                        check_expr(*axis.value, &int_type);
                    } else {
                        check_shape_axis_expression(*axis.value);
                    }
                    reads[i] = own_reads(*axis.value);
                    current.check = ShapeAxisCheck::Value;
                    current.value = axis.value.get();
                    current.after_initializer = !reads[i].empty();
                    break;
                }
            }
            if (current.value) {
                current.capture = current.value;
                if (const auto* operand = nat_conversion_operand(*current.value)) {
                    const auto found = raw_types_.find(operand);
                    if (found != raw_types_.end() && is_integer_family_type(found->second) &&
                        found->second.kind != TypeKind::Nat) {
                        current.capture = operand;
                    }
                }
                if (axis.kind != ShapeAxisKind::Bare || !is_shape_variable(axis.name)) {
                    if (const auto known =
                            constant_eval::integer(*current.capture, &const_integer_values_)) {
                        if (*known < 0) {
                            error("INVALID_TYPE", "Array/tensor extents cannot be negative.",
                                  current.value->span);
                        }
                        if (current.capture == current.value ||
                            std::holds_alternative<IntegerExpr>(current.capture->data)) {
                            current.constant = *known;
                        }
                    }
                }
            }
        }

        for (std::size_t axis = 0; axis < reads.size(); ++axis)
        plan.axes[axis].dependencies = reads[axis];

    // 3. `nat N = EXPR` axes in dependency order: a cycle is an error.
        std::vector<int> state(plan.variables.size(), 0);
        std::function<void(std::size_t)> visit = [&](std::size_t variable) {
            if (state[variable] == 2) return;
            const auto axis = plan.variables[variable].axis;
            if (state[variable] == 1) {
                error("SHAPE_VARIABLE",
                      "'" + plan.variables[variable].name + "' is defined by itself.",
                      axes[axis].span);
            }
            state[variable] = 1;
            if (plan.variables[variable].value) {
                for (const auto other : reads[axis]) {
                    if (state[other] == 1) {
                        if (other == variable) {
                            error("SHAPE_VARIABLE",
                                  "'" + plan.variables[variable].name + "' is defined by itself.",
                                  axes[axis].span);
                        }
                        error("SHAPE_VARIABLE",
                              "'" + plan.variables[other].name + "' and '" +
                                  plan.variables[variable].name +
                                  "' are defined by each other.",
                              axes[axis].span);
                    }
                    visit(other);
                }
            }
            state[variable] = 2;
        };
        for (std::size_t v = 0; v < plan.variables.size(); ++v) visit(v);

        // After the initializer: every expression over the unit's own
        // variables, a `nat N = EXPR` before the axes that read N, left to
        // right among independent ones.
        std::set<std::size_t> pending;
        for (std::size_t i = 0; i < axes.size(); ++i) {
            if (plan.axes[i].check == ShapeAxisCheck::Value && plan.axes[i].after_initializer)
                pending.insert(i);
        }
        std::unordered_set<std::size_t> done_variables;
        for (const auto& variable : plan.variables) {
            if (!variable.value || !plan.axes[variable.axis].after_initializer)
                done_variables.insert(static_cast<std::size_t>(&variable - plan.variables.data()));
        }
        while (!pending.empty()) {
            bool progressed = false;
            for (const auto axis : pending) {
                const bool ready = std::all_of(reads[axis].begin(), reads[axis].end(),
                                               [&](std::size_t v) { return done_variables.contains(v); });
                if (!ready) continue;
                plan.after_order.push_back(axis);
                if (axes[axis].kind == ShapeAxisKind::DeclareEqual)
                    done_variables.insert(unit.at(axes[axis].name));
                pending.erase(axis);
                progressed = true;
                break;
            }
            if (!progressed) break;  // a cycle, reported above
        }
    } catch (...) {
        restore();
        throw;
    }
    restore();

    // 4. The type: constant extents fold into the shape codes, and a shape
    //    whose axes relate gets an identity key.
    for (std::size_t i = 0; i < axes.size() && i < type.tensor_shape_prefix.size(); ++i) {
        const auto& current = plan.axes[i];
        if (current.constant) {
            type.tensor_shape_prefix[i] = *current.constant;
        } else if (current.check == ShapeAxisCheck::Variable &&
                   plan.axes[plan.variables[current.variable_index].axis].constant) {
            type.tensor_shape_prefix[i] =
                *plan.axes[plan.variables[current.variable_index].axis].constant;
        }
    }
    // Only the unit's own variables relate axes in a binding's shape: a
    // shape over literals, `_` and existing values keeps today's codes, so
    // a migrated shape keeps today's type.
    bool relates = signature_shape_unit_;
    std::unordered_map<std::size_t, int> uses;
    for (std::size_t i = 0; i < axes.size(); ++i) {
        for (const auto v : reads[i]) ++uses[v];
    }
    for (const auto& [variable, count] : uses) {
        if (count > 0) relates = true;
    }
    if (relates) {
        // Numbered by first occurrence; a name that refers to a binding
        // carries where that binding is declared.
        std::unordered_map<std::size_t, std::string> numbers;
        int next = 0;
        const auto number_of = [&](std::size_t variable) {
            if (const auto found = numbers.find(variable); found != numbers.end())
                return found->second;
            return numbers[variable] = "$" + std::to_string(next++);
        };
        const auto identity = [&](const std::string& name) -> std::string {
            if (const auto found = unit.find(name); found != unit.end()) return number_of(found->second);
            if (const auto parameter = signature_parameter_keys_.find(name);
                parameter != signature_parameter_keys_.end()) return parameter->second;
            std::string where = name;
            if (const auto ref = shape_variables_.find(name);
                ref != shape_variables_.end() && variables_.contains(name)) {
                where += "@" + std::to_string(ref->second.span.start.line) + ":" +
                         std::to_string(ref->second.span.start.column);
            } else if (const auto declaration = binding_declarations_.find(name);
                       declaration != binding_declarations_.end() && declaration->second) {
                where += "@" + std::to_string(declaration->second->span.start.line) + ":" +
                         std::to_string(declaration->second->span.start.column);
            }
            return where;
        };
        std::function<std::string(const Expr&)> spell = [&](const Expr& expression) -> std::string {
            if (const auto* literal = std::get_if<IntegerExpr>(&expression.data))
                return std::to_string(literal->value);
            if (const auto* name = std::get_if<NameExpr>(&expression.data))
                return name->this_qualifier ? "this." + name->name : identity(name->name);
            if (const auto* operand = nat_conversion_operand(expression)) return spell(*operand);
            if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
                std::string out = call->callee + "(";
                for (std::size_t i = 0; i < call->args.size(); ++i) {
                    if (i) out += ",";
                    out += spell(*call->args[i].value);
                }
                return out + ")";
            }
            if (const auto* unary = std::get_if<UnaryExpr>(&expression.data))
                return unary->op + "(" + spell(*unary->operand) + ")";
            if (const auto* binary = std::get_if<BinaryExpr>(&expression.data))
                return "(" + spell(*binary->left) + binary->op + spell(*binary->right) + ")";
            return "<extent>";
        };
        std::string key = "<";
        for (std::size_t i = 0; i < axes.size(); ++i) {
            if (i) key += ",";
            const auto& axis = axes[i];
            const auto& current = plan.axes[i];
            if (axis.kind == ShapeAxisKind::Discard) {
                key += "_";
            } else if (axis.kind == ShapeAxisKind::Declare) {
                const auto variable = unit.at(axis.name);
                key += uses[variable] > 0 ? number_of(variable) : "_";
            } else if (current.check == ShapeAxisCheck::Variable) {
                const auto& variable = plan.variables[current.variable_index];
                key += variable.value ? spell(*variable.value) : number_of(current.variable_index);
            } else if (current.constant) {
                key += std::to_string(*current.constant);
            } else if (current.value) {
                key += spell(*current.value);
            }
        }
        type.tensor_shape_key = key + ">";
    }

    return plan;
}

// After the declaration: the unit's variables are read-only nat bindings of
// the enclosing scope.
void Checker::declare_shape_variables(const TypeName& source, const ShapePlan& plan) {
    auto& stored = shape_plans_.at(&source);
    (void)plan;
    for (std::size_t i = 0; i < stored.variables.size(); ++i) {
        const auto& variable = stored.variables[i];
        variables_[variable.name] = simple(TypeKind::Nat);
        initialized_.insert(variable.name);
        maybe_initialized_.erase(variable.name);
        untracked_initialized_.erase(variable.name);
        binding_declarations_.erase(variable.name);
        const_bindings_.insert(variable.name);
        shape_variables_[variable.name] =
            ShapeVariableRef{&stored, i, source.shape_axes[variable.axis].span};
    }
}

// The result and every written parameter form one order-independent unit.
void Checker::check_signature_shape(const FunctionDecl& source, FunctionType& type,
                                    std::size_t parameter_offset) {
    auto aggregate = std::make_shared<TypeName>();
    aggregate->name = "tensor";
    aggregate->brace_shape = true;
    aggregate->span = source.span;
    Type fact = Type::tensor(Type::simple(TypeKind::Real32));
    SignatureShapeUnit unit{aggregate, {}};
    const auto append = [&](const TypeName& syntax, const Type& checked,
                            std::size_t parameter) {
        if (checked.kind != TypeKind::Tensor ||
            (!syntax.brace_shape && syntax.tensor_shape_prefix.empty())) return;
        auto axes = syntax.shape_axes;
        if (!syntax.brace_shape) {
            for (std::size_t axis = 0; axis < syntax.tensor_shape_expressions.size(); ++axis) {
                const auto& expression = syntax.tensor_shape_expressions[axis];
                axes.push_back({expression ? ShapeAxisKind::Unnamed : ShapeAxisKind::Discard,
                                {}, expression, syntax.span});
                if (expression) legacy_signature_axes_.insert(expression.get());
            }
        }
        const auto offset = aggregate->shape_axes.size();
        for (const auto& axis : axes) {
            if (parameter == 0 && axis.kind == ShapeAxisKind::Declare) {
                error("SHAPE_VARIABLE", "'" + axis.name +
                      "' in the result shape cannot be inferred: write 'nat " +
                      axis.name + " = EXPR', or declare it in a parameter with 'nat " +
                      axis.name + "'.", axis.span);
            }
            aggregate->shape_axes.push_back(axis);
        }
        fact.tensor_shape_prefix.insert(fact.tensor_shape_prefix.end(),
                                        checked.tensor_shape_prefix.begin(),
                                        checked.tensor_shape_prefix.end());
        unit.parts.push_back({&syntax, parameter, offset, axes.size(),
            parameter == 0 ? std::string{} : source.parameters.at(parameter - 1).name, syntax.tensor_shape_ellipsis});
    };
    append(source.return_type, type.result, 0);
    for (std::size_t p = 0; p < source.parameters.size(); ++p)
        append(source.parameters[p].type, type.parameters.at(p + parameter_offset).type, p + 1);
    fact.length = static_cast<long long>(aggregate->shape_axes.size());
    aggregate->tensor_rank = fact.length;
    // All ordinary parameters are already available when axis expressions run.
    variables_.clear();
    initialized_.clear();
    const_bindings_.clear();
    reset_current_effect_state();
    for (const auto& parameter : type.parameters) {
        variables_[parameter.name] = parameter.type;
        initialized_.insert(parameter.name);
        if (parameter.is_const) const_bindings_.insert(parameter.name);
    }
    current_module_namespace_ = source.module_namespace;
    const auto check_function_types = [&](const auto& self, const TypeName& syntax, Type& checked) -> void {
        if (syntax.name != "func" || checked.kind != TypeKind::Function) return;
        self(self, syntax.arguments.front(), *checked.first);
        for (std::size_t p = 0; p < syntax.function_parameters.size(); ++p)
            self(self, syntax.function_parameters[p], checked.parameters[p]);
        check_function_type_shape(syntax, checked);
    };
    check_function_types(check_function_types, source.return_type, type.result);
    for (std::size_t p = 0; p < source.parameters.size(); ++p)
        check_function_types(check_function_types, source.parameters[p].type,
                             type.parameters.at(p + parameter_offset).type);
    if (unit.parts.empty()) return;
    const auto previous_unit = signature_shape_unit_;
    const auto previous_parameters = signature_parameter_keys_;
    signature_shape_unit_ = true;
    signature_parameter_keys_.clear();
    for (std::size_t p = 0; p < type.parameters.size(); ++p)
        signature_parameter_keys_[type.parameters[p].name] = "#parameter" + std::to_string(p);
    try {
        check_brace_shape(*aggregate, fact, true);
    } catch (...) {
        signature_shape_unit_ = previous_unit;
        signature_parameter_keys_ = previous_parameters;
        throw;
    }
    signature_shape_unit_ = previous_unit;
    signature_parameter_keys_ = previous_parameters;
    auto& plan = shape_plans_.at(aggregate.get());
    for (const auto& part : unit.parts) {
        if (!part.parameter || !source.parameters.at(part.parameter - 1).writable) continue;
        for (auto& variable : plan.variables)
            if (!variable.value && variable.axis >= part.offset && variable.axis < part.offset + part.size)
                variable.read = true;
    }
    // The aggregate's expression maps retain source AST identity. Per-type
    // keys are divided only at top-level commas, keeping common numbering.
    std::vector<std::string> keys;
    if (!fact.tensor_shape_key.empty()) {
        std::size_t start = 1; int depth = 0;
        const auto& key = fact.tensor_shape_key;
        for (std::size_t i = 1; i < key.size(); ++i) {
            if (key[i] == '(' || key[i] == '[') ++depth;
            if (key[i] == ')' || key[i] == ']') --depth;
            if ((key[i] == ',' && depth == 0) || i + 1 == key.size()) {
                keys.push_back(key.substr(start, i - start)); start = i + 1;
            }
        }
    }
    for (const auto& part : unit.parts) {
        auto& target = part.parameter == 0 ? type.result
            : type.parameters.at(part.parameter - 1 + parameter_offset).type;
        for (std::size_t axis = 0; axis < part.size; ++axis)
            target.tensor_shape_prefix[axis] = fact.tensor_shape_prefix[part.offset + axis];
        if (!keys.empty()) {
            target.tensor_shape_key = "<";
            for (std::size_t axis = 0; axis < part.size; ++axis) {
                if (axis) target.tensor_shape_key += ",";
                target.tensor_shape_key += keys.at(part.offset + axis);
            }
            target.tensor_shape_key += ">";
            clear_redundant_shape_key(target);
        }
    }
    signature_shapes_[&source] = std::move(unit);
}

void Checker::bind_signature_shape_variables(const FunctionDecl& source) {
    const auto found = signature_shapes_.find(&source);
    if (found == signature_shapes_.end()) return;
    const auto& syntax = *found->second.source;
    declare_shape_variables(syntax, shape_plans_.at(&syntax));
}

void Checker::check_function_type_shape(const TypeName& source, Type& type) {
    auto aggregate = std::make_shared<TypeName>();
    aggregate->name = "tensor";
    aggregate->brace_shape = true;
    aggregate->span = source.span;
    Type fact = Type::tensor(Type::simple(TypeKind::Real32));
    struct Part { Type* type; std::size_t offset; std::size_t size; };
    std::vector<Part> parts;
    SignatureShapeUnit unit{aggregate, {}};
    const auto append = [&](const TypeName& syntax, Type& current, std::size_t parameter) {
        if (current.kind != TypeKind::Tensor ||
            (!syntax.brace_shape && syntax.tensor_shape_prefix.empty())) return;
        auto axes = syntax.shape_axes;
        if (!syntax.brace_shape) {
            for (const auto& expression : syntax.tensor_shape_expressions) {
                axes.push_back({expression ? ShapeAxisKind::Unnamed : ShapeAxisKind::Discard,
                                {}, expression, syntax.span});
                if (expression) legacy_signature_axes_.insert(expression.get());
            }
        }
        const auto offset = aggregate->shape_axes.size();
        for (const auto& axis : axes) {
            if (parameter == 0 && axis.kind == ShapeAxisKind::Declare)
                error("SHAPE_VARIABLE", "'" + axis.name + "' in the result shape cannot be inferred: write 'nat " +
                    axis.name + " = EXPR', or declare it in a parameter.", axis.span);
            aggregate->shape_axes.push_back(axis);
        }
        fact.tensor_shape_prefix.insert(fact.tensor_shape_prefix.end(), current.tensor_shape_prefix.begin(),
                                       current.tensor_shape_prefix.end());
        parts.push_back({&current, offset, axes.size()});
        unit.parts.push_back({&syntax, parameter, offset, axes.size(), {}, syntax.tensor_shape_ellipsis});
    };
    append(source.arguments.front(), *type.first, 0);
    for (std::size_t p = 0; p < source.function_parameters.size(); ++p)
        append(source.function_parameters[p], type.parameters[p], p + 1);
    if (parts.empty()) return;
    fact.length = static_cast<long long>(aggregate->shape_axes.size());
    aggregate->tensor_rank = fact.length;
    const auto previous_unit = signature_shape_unit_;
    const auto previous_parameters = signature_parameter_keys_;
    signature_shape_unit_ = true;
    signature_parameter_keys_.clear();
    try {
        check_brace_shape(*aggregate, fact, true);
    } catch (...) {
        signature_shape_unit_ = previous_unit;
        signature_parameter_keys_ = previous_parameters;
        throw;
    }
    signature_shape_unit_ = previous_unit;
    signature_parameter_keys_ = previous_parameters;
    std::vector<std::string> keys;
    if (!fact.tensor_shape_key.empty()) {
        const auto& key = fact.tensor_shape_key;
        std::size_t start = 1;
        int depth = 0;
        for (std::size_t i = 1; i < key.size(); ++i) {
            if (key[i] == '(' || key[i] == '[') ++depth;
            if (key[i] == ')' || key[i] == ']') --depth;
            if ((key[i] == ',' && depth == 0) || i + 1 == key.size()) {
                keys.push_back(key.substr(start, i - start));
                start = i + 1;
            }
        }
    }
    for (const auto& part : parts) {
        part.type->tensor_shape_key = "<";
        for (std::size_t axis = 0; axis < part.size; ++axis) {
            part.type->tensor_shape_prefix[axis] = fact.tensor_shape_prefix[part.offset + axis];
            if (axis) part.type->tensor_shape_key += ",";
            part.type->tensor_shape_key += keys.at(part.offset + axis);
        }
        part.type->tensor_shape_key += ">";
        clear_redundant_shape_key(*part.type);
    }
    function_shape_units_[&source] = std::move(unit);
}

} // namespace quidra
