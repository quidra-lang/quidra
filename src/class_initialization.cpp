#include "class_initialization.hpp"
#include <unordered_set>

namespace quidra {
namespace {
class ClassNames {
public:
    explicit ClassNames(ClassDecl& declaration) : declaration_(declaration) {
        for (const auto& field : declaration.fields) {
            if (!members_.insert(field.name).second)
                fail("DUPLICATE_NAME", "Duplicate class member '" + field.name + "'.", field.span);
        }
        for (const auto& method : declaration.methods) {
            if (!method.is_constructor && !declaration.self_name.empty() && method.name == declaration.self_name)
                fail("DUPLICATE_NAME", "The instance name conflicts with a method.", method.span);
        }
        if (!declaration.self_name.empty() && members_.contains(declaration.self_name))
            fail("DUPLICATE_NAME", "The instance name conflicts with a class member.", declaration.span);
    }
    void run() {
        for (auto& method : declaration_.methods) {
            for (auto& parameter : method.parameters) {
                if (!method.is_constructor || parameter.is_local) reject_binding(parameter.name, parameter.span);
                // Header defaults are evaluated by the caller, before an instance exists.
                if (!method.is_constructor) type(parameter.type);
            }
            if (!method.is_constructor) type(method.return_type);
            body(method.body);
        }
        auto& initializer = declaration_.methods.front();
        std::vector<StmtPtr> prefix;
        for (const auto& parameter : initializer.parameters) {
            if (parameter.is_local) continue;
            auto target = std::make_unique<Expr>();
            target->span = parameter.span;
            target->data = NameExpr{parameter.name, parameter.span};
            auto value = std::make_unique<Expr>();
            value->span = parameter.span;
            value->data = NameExpr{parameter.name};
            auto statement = std::make_unique<Stmt>();
            statement->span = parameter.span;
            if (parameter.writable)
                statement->data = RebindStmt{parameter.name, std::move(value), std::move(target)};
            else statement->data = AssignStmt{std::move(target), std::move(value), {}, {}, nullptr};
            prefix.push_back(std::move(statement));
        }
        for (auto& statement : initializer.body) prefix.push_back(std::move(statement));
        initializer.body = std::move(prefix);
    }
private:
    ClassDecl& declaration_;
    std::unordered_set<std::string> members_;
    [[noreturn]] static void fail(const char* code, std::string message, SourceSpan span) {
        throw CompileError(Diagnostic{code, std::move(message), span});
    }
    void reject_binding(const std::string& name, SourceSpan span) const {
        if (members_.contains(name) || (!declaration_.self_name.empty() && name == declaration_.self_name))
            fail("SHADOWING", "Binding '" + name + "' conflicts with a class member or the fixed instance name.", span);
    }
    void type(TypeName& value) {
        for (auto& argument : value.arguments) type(argument);
        for (auto& parameter : value.function_parameters) type(parameter);
        for (auto& dimension : value.dimension_expressions) if (dimension) expr(*dimension);
        for (auto& dimension : value.tensor_shape_expressions) if (dimension) expr(*dimension);
        for (auto& axis : value.shape_axes) {
            if (axis.kind == ShapeAxisKind::Declare || axis.kind == ShapeAxisKind::DeclareEqual)
                reject_binding(axis.name, axis.span);
            if (axis.value) expr(*axis.value);
        }
    }
    void pattern(Pattern& value) {
        if (value.kind == Pattern::Kind::Declare) reject_binding(value.name, value.span);
        if (value.type) type(*value.type);
        if (value.place) {
            expr(*value.place);
            if (self(*value.place)) fail("WRITE_CAPABILITY", "The fixed instance name cannot be reassigned.", value.place->span);
        }
        for (auto& child : value.elements) pattern(child);
    }
    bool self(const Expr& value) const {
        const auto* name = std::get_if<NameExpr>(&value.data);
        return name && name->self_reference;
    }
    void callee(const std::string& name, SourceSpan span) const {
        if (members_.contains(name))
            fail("NOT_CALLABLE", "Read a function-valued member into an explicit local function binding before calling it.", span);
        if (!declaration_.self_name.empty() && name == declaration_.self_name)
            fail("NOT_CALLABLE", "The fixed instance name is not callable.", span);
    }
    void expr(Expr& value) {
        if (auto* node = std::get_if<NameExpr>(&value.data)) {
            if (!node->this_qualifier) {
                if (!declaration_.self_name.empty() && node->name == declaration_.self_name) node->self_reference = true;
                else if (members_.contains(node->name)) node->this_qualifier = value.span;
            }
        } else if (auto* node = std::get_if<ArrayExpr>(&value.data)) {
            for (auto& child : node->elements) expr(*child);
        } else if (auto* node = std::get_if<ArrayComprehensionExpr>(&value.data)) {
            expr(*node->source);
            reject_binding(node->name, node->binding_span);
            if (node->type) type(*node->type);
            if (node->condition) expr(*node->condition);
            expr(*node->value);
        } else if (auto* node = std::get_if<TupleExpr>(&value.data)) {
            for (auto& child : node->elements) expr(*child);
        } else if (auto* node = std::get_if<StringTemplateExpr>(&value.data)) {
            for (auto& child : node->expressions) expr(*child);
        } else if (auto* node = std::get_if<IndexExpr>(&value.data)) {
            expr(*node->base);
            for (auto& item : node->items) {
                if (item.index) expr(*item.index);
                if (item.start) expr(*item.start);
                if (item.stop) expr(*item.stop);
                if (item.step) expr(*item.step);
            }
        } else if (auto* node = std::get_if<MemberExpr>(&value.data)) {
            expr(*node->base);
            if (self(*node->base) && members_.contains(node->name)) value.data = NameExpr{node->name, value.span};
        } else if (auto* node = std::get_if<UnaryExpr>(&value.data)) {
            expr(*node->operand);
        } else if (auto* node = std::get_if<BinaryExpr>(&value.data)) {
            expr(*node->left); expr(*node->right);
        } else if (auto* node = std::get_if<ComparisonChainExpr>(&value.data)) {
            for (auto& child : node->operands) expr(*child);
        } else if (auto* node = std::get_if<CallExpr>(&value.data)) {
            callee(node->callee, value.span);
            for (auto& argument : node->args) expr(*argument.value);
            for (auto& argument : node->type_arguments) type(argument);
        } else if (auto* node = std::get_if<MethodCallExpr>(&value.data)) {
            expr(*node->receiver);
            for (auto& argument : node->args) expr(*argument.value);
            for (auto& argument : node->type_arguments) type(argument);
            if (self(*node->receiver)) {
                callee(node->method, value.span);
                CallExpr call;
                call.callee = node->method;
                call.args = std::move(node->args);
                call.type_arguments = std::move(node->type_arguments);
                call.mutation_marker = node->mutation_marker;
                value.data = std::move(call);
            }
        } else if (auto* node = std::get_if<TryExpr>(&value.data)) {
            expr(*node->value);
        } else if (auto* node = std::get_if<LiteralConstantExpr>(&value.data)) {
            expr(*node->value);
        } else if (auto* node = std::get_if<IfExpr>(&value.data)) {
            for (auto& child : node->conditions) expr(*child);
            for (auto& child : node->values) expr(*child);
            expr(*node->otherwise);
        }
    }
    void body(std::vector<StmtPtr>& statements) {
        for (auto& statement : statements) {
            auto& value = *statement;
            if (auto* node = std::get_if<BindingStmt>(&value.data)) {
                if (node->pattern) pattern(*node->pattern);
                else { reject_binding(node->name, value.span); type(node->declared_type); }
                if (node->value) expr(*node->value);
            } else if (auto* node = std::get_if<AssignStmt>(&value.data)) {
                if (node->pattern) pattern(*node->pattern);
                if (node->target) {
                    expr(*node->target);
                    if (self(*node->target)) fail("WRITE_CAPABILITY", "The fixed instance name cannot be reassigned.", node->target->span);
                }
                expr(*node->value);
                const auto* address = node->target ? std::get_if<UnaryExpr>(&node->target->data) : nullptr;
                const auto* source = std::get_if<UnaryExpr>(&node->value->data);
                if (!node->pattern && node->compound_op.empty() && address && address->op == "&" &&
                    source && source->op == "&") {
                    const auto* field = std::get_if<NameExpr>(&address->operand->data);
                    if (field && field->this_qualifier) {
                        const auto name = field->name;
                        auto target = std::move(std::get<UnaryExpr>(node->value->data).operand);
                        auto place = std::move(std::get<UnaryExpr>(node->target->data).operand);
                        value.data = RebindStmt{name, std::move(target), std::move(place)};
                    }
                }
            } else if (auto* node = std::get_if<RebindStmt>(&value.data)) {
                if (node->target) expr(*node->target);
                if (node->place) {
                    expr(*node->place);
                    if (self(*node->place))
                        fail("WRITE_CAPABILITY", "The fixed instance name cannot be rebound.", node->place->span);
                } else if (!declaration_.self_name.empty() && node->name == declaration_.self_name) {
                    fail("WRITE_CAPABILITY", "The fixed instance name cannot be rebound.", value.span);
                } else if (members_.contains(node->name)) {
                    node->place = std::make_unique<Expr>();
                    node->place->span = value.span;
                    node->place->data = NameExpr{node->name, value.span};
                }
            } else if (auto* node = std::get_if<ReturnStmt>(&value.data)) {
                if (node->value) expr(*node->value);
            } else if (auto* node = std::get_if<ExprStmt>(&value.data)) {
                expr(*node->value);
            } else if (auto* node = std::get_if<IfStmt>(&value.data)) {
                expr(*node->condition); body(node->then_body); body(node->else_body);
            } else if (auto* node = std::get_if<WhileStmt>(&value.data)) {
                expr(*node->condition); body(node->body);
            } else if (auto* node = std::get_if<ForStmt>(&value.data)) {
                visit_for_header(*node, [&](Expr& operand) { expr(operand); });
                if (!node->discards()) reject_binding(node->name, value.span);
                if (node->index && node->index->declares) reject_binding(node->index->name, node->index->span);
                if (node->pattern) pattern(*node->pattern);
                if (node->type) type(*node->type);
                for (auto& collection : node->collections) {
                    if (collection.name != "_") reject_binding(collection.name, collection.span);
                    if (collection.index && collection.index->declares) reject_binding(collection.index->name, collection.index->span);
                    if (collection.type) type(*collection.type);
                }
                body(node->body);
            } else if (auto* node = std::get_if<MatchStmt>(&value.data)) {
                expr(*node->value);
                for (auto& item : node->cases) {
                    if (item.binder) reject_binding(*item.binder, item.span);
                    if (item.pattern) pattern(*item.pattern);
                    type(item.type); body(item.body);
                }
            } else if (auto* node = std::get_if<MainGuardStmt>(&value.data)) {
                body(node->body);
            } else if (auto* node = std::get_if<ParallelStmt>(&value.data)) {
                body(node->body);
            }
        }
    }
};
}
void elaborate_class_initialization(ClassDecl& declaration) {
    ClassNames(declaration).run();
}
}
