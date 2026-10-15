// Mutation markers (L1, L7). The definitions are checked against their
// effect summaries, which carry callee effects through every call, so a
// caller that does not expose a callee's change is itself `.` (transitive)
// and one whose changed state is fresh is not. The calls are checked against
// the callees' declarations: a `.` declaration is a promise every caller
// sees, whatever its body does. Constructors follow the same rules with the
// value they create as fresh state; a plain declaration `T x` runs no
// constructor and never needs the marker.

#include "semantics/mutation_markers.hpp"

#include "quidra/language.hpp"
#include "quidra/member_function_names.hpp"
#include "semantics/effect_summary.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

namespace quidra::semantics {
namespace {

constexpr char marker_code[] = "MUTATION_MARKER";

// The name a declaration was written with: generic instances carry a
// compiler name, so theirs is read from the source.
std::string declared_name(const CheckedProgram& checked, const FunctionDecl& declaration) {
    const auto& name = declaration.name;
    if (!declaration.is_constructor && !name.starts_with("__quidra_")) {
        const auto dot = name.rfind('.');
        return dot == std::string::npos ? name : name.substr(dot + 1);
    }
    const auto source = checked.program.source_texts.find(declaration.source_file);
    if (source == checked.program.source_texts.end()) return declaration.is_constructor ? "class generation" : name;
    const auto& text = source->second;
    const auto start = std::min(declaration.span.start.offset, text.size());
    const auto end = std::min(declaration.span.end.offset, text.size());
    const std::string_view header{text.data() + start, end - start};
    if (declaration.is_constructor) {
        const auto keyword = header.find("class");
        if (keyword == std::string::npos) return "class generation";
        const auto first = header.find_first_not_of(" \t", keyword + 5);
        if (first == std::string::npos) return "class generation";
        auto last = first;
        while (last < header.size() &&
               (std::isalnum(static_cast<unsigned char>(header[last])) || header[last] == '_' || header[last] == '.'))
            ++last;
        return first < last ? std::string{header.substr(first, last - first)} : "class generation";
    }
    const auto paren = header.find_first_of("(<");
    if (paren == std::string::npos) return name;
    std::size_t last = paren;
    while (last > 0 && header[last - 1] == ' ') --last;
    std::size_t first = last;
    while (first > 0 && (std::isalnum(static_cast<unsigned char>(header[first - 1])) ||
                         header[first - 1] == '_'))
        --first;
    return first < last ? std::string{header.substr(first, last - first)} : name;
}

// A short spelling of an expression for the corrected call in a message.
std::string spell(const Expr& expression, int depth = 0) {
    if (depth > 6) return "...";
    const auto arguments = [&](const std::vector<CallArg>& args) {
        std::string out;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i) out += ", ";
            if (args[i].name) out += *args[i].name + " = ";
            if (args[i].writable) out += "&";
            out += args[i].value ? spell(*args[i].value, depth + 1) : "...";
        }
        return out;
    };
    const auto base = [&](const Expr& inner) {
        const auto text = spell(inner, depth + 1);
        const bool operator_expression = std::holds_alternative<BinaryExpr>(inner.data) ||
                                         std::holds_alternative<UnaryExpr>(inner.data);
        return operator_expression ? "(" + text + ")" : text;
    };
    if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        auto shown = name->name;
        if (const auto dot = shown.rfind('.'); dot != std::string::npos && !name->this_qualifier &&
                                               shown.starts_with("$std."))
            shown = shown.substr(dot + 1);
        return shown;
    }
    if (const auto* literal = std::get_if<IntegerExpr>(&expression.data))
        return literal->spelling.empty() ? std::to_string(literal->value) : literal->spelling;
    if (const auto* literal = std::get_if<RealLiteralExpr>(&expression.data)) return literal->spelling;
    if (const auto* literal = std::get_if<BoolExpr>(&expression.data))
        return literal->value ? "true" : "false";
    if (const auto* literal = std::get_if<StringExpr>(&expression.data)) {
        if (literal->value.size() <= 24 && literal->value.find_first_of("\"\n{}") == std::string::npos)
            return "\"" + literal->value + "\"";
        return "\"...\"";
    }
    if (const auto* member = std::get_if<MemberExpr>(&expression.data))
        return base(*member->base) + "." + member->name;
    if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
        auto callee = call->callee;
        // Qualified internal names print as the last segment.
        if (const auto dot = callee.rfind('.'); dot != std::string::npos) callee = callee.substr(dot + 1);
        if (callee.starts_with("__quidra_")) callee = "...";
        return callee + "(" + arguments(call->args) + ")";
    }
    if (const auto* method = std::get_if<MethodCallExpr>(&expression.data))
        return base(*method->receiver) + "." + method->method + "(" + arguments(method->args) + ")";
    if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
        if (index->items.size() == 1 && !index->items.front().slice && index->items.front().index)
            return base(*index->base) + "[" + spell(*index->items.front().index, depth + 1) + "]";
        return base(*index->base) + "[...]";
    }
    if (const auto* unary = std::get_if<UnaryExpr>(&expression.data))
        return unary->op + spell(*unary->operand, depth + 1);
    if (const auto* binary = std::get_if<BinaryExpr>(&expression.data))
        return spell(*binary->left, depth + 1) + " " + binary->op + " " +
               spell(*binary->right, depth + 1);
    return "...";
}

// What a call reaches, for the marker: a declaration (function, method or
// constructor) or a method of a built-in type.
struct Callee {
    // The callee's internal name; empty for a method of a built-in type.
    std::string internal;
    // The name as declared: "increment", "construct", "write".
    std::string shown;
    bool marked{};
    bool constructor{};
};

class MarkerChecker {
public:
    explicit MarkerChecker(const CheckedProgram& checked) : checked_(checked) {
        for (const auto& function : checked_.program.functions)
            declarations_.emplace(function.name, &function);
        for (const auto& class_decl : checked_.program.classes) {
            std::size_t constructors = 0;
            for (const auto& method : class_decl.methods) {
                const auto name = method.is_constructor
                                      ? member_function_name::constructor(class_decl.name, constructors++)
                                      : member_function_name::method(class_decl.name, method.name);
                declarations_.emplace(name, &method);
            }
        }
    }

    std::vector<Diagnostic> run() {
        // Definitions (R1, R7).
        for (const auto& function : checked_.program.functions) definition(function.name, function);
        for (const auto& class_decl : checked_.program.classes) {
            // The standard library declares its markers: a query that
            // updates a cache inside its receiver changes no value.
            if (class_decl.standard_library) continue;
            std::size_t constructors = 0;
            for (const auto& method : class_decl.methods) {
                const auto name = method.is_constructor
                                      ? member_function_name::constructor(class_decl.name, constructors++)
                                      : member_function_name::method(class_decl.name, method.name);
                definition(name, method);
            }
        }
        // Calls (R3), defaults (R9, parameter defaults), fn values (R6).
        for (const auto& function : checked_.program.functions) body(function);
        for (const auto& class_decl : checked_.program.classes) {
            for (const auto& field : class_decl.fields) {
                if (field.default_value)
                    default_value(*field.default_value, "A field default", field.name,
                                  "move the call into the class initialization body");
            }
            for (const auto& method : class_decl.methods) body(method);
        }
        for (const auto& statement : checked_.program.statements) stmt(*statement);
        for (const auto& [expression, name] : checked_.function_references) {
            const auto callee = declared(name);
            if (callee && callee->marked)
                report(Diagnostic{"FUNCTION_REFERENCE_SIGNATURE",
                                  "fn values cannot refer to '.' functions: '" + callee->shown +
                                      "' is declared '." + callee->shown + "'.",
                                  expression->span});
        }
        std::vector<Diagnostic> out;
        out.reserve(diagnostics_.size());
        for (auto& [key, diagnostic] : diagnostics_) out.push_back(std::move(diagnostic));
        return out;
    }

private:
    using Key = std::tuple<std::size_t, std::size_t, std::string, std::string>;

    void report(Diagnostic diagnostic) {
        Key key{diagnostic.span.start.line, diagnostic.span.start.column, diagnostic.code,
                diagnostic.message};
        diagnostics_.emplace(std::move(key), std::move(diagnostic));
    }

    const FunctionDecl* declaration(const std::string& internal) const {
        const auto found = declarations_.find(internal);
        return found == declarations_.end() ? nullptr : found->second;
    }

    std::optional<Callee> declared(const std::string& internal) const {
        const auto* found = declaration(internal);
        if (!found) return std::nullopt;
        const auto* effects = summary(internal);
        const bool generation_changes_state = found->is_constructor && found->name == "$initialize" &&
            effects && effects->first_hidden_mutation.has_value();
        return Callee{internal, declared_name(checked_, *found), found->mutation_marker || generation_changes_state, found->is_constructor};
    }

    const EffectSummary* summary(const std::string& internal) const {
        return checked_.effects ? checked_.effects->find(internal) : nullptr;
    }

    // R1, R7: a body that changes existing state its `&` parameters do not
    // expose is declared `.`. An extern's marker is its declaration.
    void definition(const std::string& internal, const FunctionDecl& declaration) {
        if (declaration.external_symbol || declaration.mutation_marker || declaration.is_prototype ||
            (declaration.is_constructor && declaration.name == "$initialize")) return;
        const auto* effects = summary(internal);
        if (!effects || !effects->first_hidden_mutation) return;
        const auto name = declared_name(checked_, declaration);
        report(Diagnostic{marker_code,
                          "'" + name + "' may change existing state (" +
                              effects->first_hidden_mutation->description + "); declare it as '." +
                              name + "'.",
                          declaration.span});
    }

    void body(const FunctionDecl& function) {
        for (const auto& parameter : function.parameters) {
            if (parameter.default_value)
                default_value(*parameter.default_value, "A parameter default", parameter.name,
                              "pass the value as an argument");
        }
        for (const auto& statement : function.body) stmt(*statement);
    }

    // R9 and its parameter twin: a default runs at every construction or
    // call that omits it, so it may not change existing state.
    void default_value(const Expr& expression, const char* what, const std::string& name,
                       const char* advice) {
        bool reported = false;
        const std::function<void(const Expr&)> visit = [&](const Expr& node) {
            if (reported) return;
            if (const auto callee = callee_of(node)) {
                bool hidden = callee->marked;
                if (!hidden && !callee->internal.empty()) {
                    const auto* effects = summary(callee->internal);
                    hidden = effects && effects->first_hidden_mutation;
                }
                if (hidden) {
                    report(Diagnostic{marker_code,
                                      std::string(what) + " cannot change existing state: '" + name +
                                          "' calls '." + callee->shown + "'; " + advice + ".",
                                      node.span});
                    reported = true;
                    return;
                }
            }
            each_child(node, visit);
        };
        visit(expression);
    }

    // What the call `expression` reaches, when it is a call that can need
    // the marker.
    std::optional<Callee> callee_of(const Expr& expression) const {
        if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
            if (const auto method = checked_.method_calls.find(&expression);
                method != checked_.method_calls.end())
                return declared(method->second.internal_name);
            const auto resolution = checked_.call_resolutions.find(&expression);
            if (resolution == checked_.call_resolutions.end()) return std::nullopt;
            if (resolution->second.kind == CallKind::Function) return declared(resolution->second.target);
            if (resolution->second.kind == CallKind::Constructor &&
                resolution->second.type.kind == TypeKind::Class) {
                const auto info = checked_.classes.find(resolution->second.target);
                if (info == checked_.classes.end()) return std::nullopt;
                std::string internal;
                if (!call->constructor.empty()) {
                    if (const auto instance = info->second.constructor_instances.find(call->constructor);
                        instance != info->second.constructor_instances.end())
                        internal = instance->second;
                } else if (info->second.constructors.size() == 1) {
                    internal = info->second.constructors.front();
                }
                if (internal.empty()) return std::nullopt;
                return declared(internal);
            }
            return std::nullopt;
        }
        if (const auto* call = std::get_if<MethodCallExpr>(&expression.data)) {
            if (const auto method = checked_.method_calls.find(&expression);
                method != checked_.method_calls.end())
                return declared(method->second.internal_name);
            // A method of a built-in type.
            const Type* receiver = nullptr;
            if (const auto found = checked_.expr_types.find(call->receiver.get());
                found != checked_.expr_types.end())
                receiver = &found->second;
            if (!receiver) return std::nullopt;
            std::string_view kind;
            if (receiver->kind == TypeKind::Tensor) kind = "tensor";
            else if (receiver->kind == TypeKind::Class) kind = receiver->class_name;
            else return std::nullopt;
            if (!standard_method_effect(kind, call->method)) return std::nullopt;
            return Callee{{}, call->method, true, false};
        }
        return std::nullopt;
    }

    static bool marked(const Expr& expression) {
        if (const auto* call = std::get_if<CallExpr>(&expression.data)) return call->mutation_marker;
        if (const auto* call = std::get_if<MethodCallExpr>(&expression.data)) return call->mutation_marker;
        return false;
    }

    // Visits the expressions directly inside `expression`.
    template <class Visit>
    static void each_child(const Expr& expression, const Visit& visit) {
        const auto each = [&](const ExprPtr& item) {
            if (item) visit(*item);
        };
        if (const auto* node = std::get_if<StringTemplateExpr>(&expression.data)) {
            for (const auto& item : node->expressions) each(item);
        } else if (const auto* node = std::get_if<TupleExpr>(&expression.data)) {
            for (const auto& item : node->elements) each(item);
        } else if (const auto* node = std::get_if<ArrayComprehensionExpr>(&expression.data)) {
            visit_array_comprehension(*node, [&](const Expr& child) { visit(child); });
        } else if (const auto* node = std::get_if<ArrayExpr>(&expression.data)) {
            for (const auto& item : node->elements) each(item);
        } else if (const auto* node = std::get_if<IndexExpr>(&expression.data)) {
            each(node->base);
            for (const auto& item : node->items) {
                each(item.index);
                each(item.start);
                each(item.stop);
                each(item.step);
            }
        } else if (const auto* node = std::get_if<MemberExpr>(&expression.data)) {
            each(node->base);
        } else if (const auto* node = std::get_if<UnaryExpr>(&expression.data)) {
            each(node->operand);
        } else if (const auto* node = std::get_if<BinaryExpr>(&expression.data)) {
            each(node->left);
            each(node->right);
        } else if (const auto* node = std::get_if<ComparisonChainExpr>(&expression.data)) {
            for (const auto& item : node->operands) each(item);
        } else if (const auto* node = std::get_if<TryExpr>(&expression.data)) {
            each(node->value);
        } else if (const auto* node = std::get_if<IfExpr>(&expression.data)) {
            for (const auto& item : node->conditions) each(item);
            for (const auto& item : node->values) each(item);
            each(node->otherwise);
        } else if (const auto* node = std::get_if<CallExpr>(&expression.data)) {
            for (const auto& argument : node->args) each(argument.value);
        } else if (const auto* node = std::get_if<MethodCallExpr>(&expression.data)) {
            each(node->receiver);
            for (const auto& argument : node->args) each(argument.value);
        }
    }

    // R3. `chain` is the outermost call of the postfix chain `expression`
    // is in (receiver positions), `covered` whether a marker written before
    // that chain covers it.
    void expr(const Expr& expression, const Expr* chain = nullptr, bool covered = false) {
        const bool call = std::holds_alternative<CallExpr>(expression.data) ||
                          std::holds_alternative<MethodCallExpr>(expression.data);
        if (call && !chain) {
            chain = &expression;
            covered = marked(expression);
            // '.receiver.extension(...)' is the method form of passing
            // '&receiver' as an explicit reference argument. This dot makes
            // the writable receiver visible; it is not hidden mutation of
            // the ordinary function, whose direct f(&receiver) form remains
            // valid without a leading marker.
            bool extension_receiver_borrow = false;
            if (const auto* direct = std::get_if<CallExpr>(&expression.data);
                direct && !direct->args.empty() && direct->args.front().writable) {
                if (const auto resolved = checked_.call_resolutions.find(&expression);
                    resolved != checked_.call_resolutions.end()) {
                    if (const auto* target = declaration(resolved->second.target);
                        target && !target->parameters.empty()) {
                        const auto& param = target->parameters.front();
                        extension_receiver_borrow = param.is_extension && param.writable;
                    }
                }
            }
            if (covered && !chain_has_marked_callee(expression) &&
                !extension_receiver_borrow) {
                report(Diagnostic{marker_code, "'.' marks a call that changes no existing state.",
                                  expression.span});
            }
        }
        if (call) {
            if (const auto callee = callee_of(expression); callee && callee->marked && !covered) {
                const auto suggestion = "." + spell(*chain);
                report(Diagnostic{marker_code,
                                  callee->constructor
                                      ? "This constructor call may change existing state; write '" +
                                            suggestion + "'."
                                      : "This call may change existing state through '." +
                                            callee->shown + "'; write '" + suggestion + "'.",
                                  expression.span});
            }
        }
        // The receiver chain stays in this chain; everything else starts its
        // own.
        if (const auto* node = std::get_if<MethodCallExpr>(&expression.data)) {
            receiver_chain(*node->receiver, chain, covered);
            for (const auto& argument : node->args) expr(*argument.value);
            return;
        }
        if (const auto* node = std::get_if<CallExpr>(&expression.data)) {
            for (const auto& argument : node->args) expr(*argument.value);
            return;
        }
        each_child(expression, [&](const Expr& child) { expr(child); });
    }

    // A receiver position: calls, members and indexed bases continue the
    // chain; an index operand does not.
    void receiver_chain(const Expr& expression, const Expr* chain, bool covered) {
        if (std::holds_alternative<CallExpr>(expression.data) ||
            std::holds_alternative<MethodCallExpr>(expression.data)) {
            expr(expression, chain, covered);
            return;
        }
        if (const auto* node = std::get_if<MemberExpr>(&expression.data)) {
            receiver_chain(*node->base, chain, covered);
            return;
        }
        if (const auto* node = std::get_if<IndexExpr>(&expression.data)) {
            receiver_chain(*node->base, chain, covered);
            for (const auto& item : node->items) {
                if (item.index) expr(*item.index);
                if (item.start) expr(*item.start);
                if (item.stop) expr(*item.stop);
                if (item.step) expr(*item.step);
            }
            return;
        }
        expr(expression);
    }

    // Whether a call of the chain `expression` (it and the calls in its
    // receiver positions) reaches a `.` callee (Q15).
    bool chain_has_marked_callee(const Expr& expression) const {
        const Expr* current = &expression;
        while (current) {
            if (const auto callee = callee_of(*current); callee && callee->marked) return true;
            if (const auto* node = std::get_if<MethodCallExpr>(&current->data)) {
                current = node->receiver.get();
            } else if (const auto* node = std::get_if<MemberExpr>(&current->data)) {
                current = node->base.get();
            } else if (const auto* node = std::get_if<IndexExpr>(&current->data)) {
                current = node->base.get();
            } else {
                current = nullptr;
            }
        }
        return false;
    }

    void stmt(const Stmt& statement) {
        const auto block = [&](const std::vector<StmtPtr>& body) {
            for (const auto& item : body) stmt(*item);
        };
        const auto& data = statement.data;
        if (const auto* node = std::get_if<BindingStmt>(&data)) {
            if (node->value) expr(*node->value);
        } else if (const auto* node = std::get_if<AssignStmt>(&data)) {
            if(node->pattern)visit_pattern(*node->pattern,[&](const Pattern& p){if(p.place)expr(*p.place);});
            else expr(*node->target_expr());
            expr(*node->value);
        } else if (const auto* node = std::get_if<RebindStmt>(&data)) {
            if(node->place)expr(*node->place);
            expr(*node->target);
        } else if (const auto* node = std::get_if<ReturnStmt>(&data)) {
            if (node->value) expr(*node->value);
        } else if (const auto* node = std::get_if<ExprStmt>(&data)) {
            expr(*node->value);
        } else if (const auto* node = std::get_if<IfStmt>(&data)) {
            expr(*node->condition);
            block(node->then_body);
            block(node->else_body);
        } else if (const auto* node = std::get_if<ParallelStmt>(&data)) {
            block(node->body);
        } else if (const auto* node = std::get_if<MainGuardStmt>(&data)) {
            block(node->body);
        } else if (const auto* node = std::get_if<WhileStmt>(&data)) {
            expr(*node->condition);
            block(node->body);
        } else if (const auto* node = std::get_if<ForStmt>(&data)) {
            visit_for_header(*node,[&](const Expr& operand) { expr(operand); });
            block(node->body);
        } else if (const auto* node = std::get_if<MatchStmt>(&data)) {
            expr(*node->value);
            for (const auto& match_case : node->cases) block(match_case.body);
        }
    }

    const CheckedProgram& checked_;
    std::unordered_map<std::string, const FunctionDecl*> declarations_;
    std::map<Key, Diagnostic> diagnostics_;
};

} // namespace

std::vector<Diagnostic> check_mutation_markers(const CheckedProgram& checked) {
    return MarkerChecker(checked).run();
}

} // namespace quidra::semantics
