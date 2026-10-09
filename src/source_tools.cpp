#include "quidra/source_tools.hpp"
#include "quidra/lexer.hpp"
#include "quidra/member_function_names.hpp"
#include "quidra/parser.hpp"

#include "platform/sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>

namespace quidra {
namespace {

std::string json_escape(std::string_view text) {
    std::ostringstream out;
    for (char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    const char* hex = "0123456789abcdef";
                    out << "\\u00" << hex[(c >> 4) & 0xf] << hex[c & 0xf];
                } else {
                    out << static_cast<char>(c);
                }
        }
    }
    return out.str();
}

std::string json_string_set(const std::unordered_set<std::string>& values) {
    std::vector<std::string> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        if (i) out << ',';
        out << '"' << json_escape(sorted[i]) << '"';
    }
    out << ']';
    return out.str();
}

void write_effect_json(std::ostringstream& out, const StorageEffect& effect) {
    out << "{\"requires\":" << json_string_set(effect.required)
        << ",\"writes\":" << json_string_set(effect.writes)
        << ",\"initializes\":" << json_string_set(effect.initializes)
        << ",\"invalidates\":" << json_string_set(effect.invalidates) << '}';
}

const char* expr_kind(const Expr& expr) {
    return std::visit([](const auto& node) -> const char* {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, IntegerExpr>) return "integer";
        else if constexpr (std::is_same_v<T, RealLiteralExpr>) return "float";
        else if constexpr (std::is_same_v<T, ImaginaryLiteralExpr>) return "imaginary";
        else if constexpr (std::is_same_v<T, StringExpr>) return "string";
        else if constexpr (std::is_same_v<T, StringTemplateExpr>) return "string_template";
        else if constexpr (std::is_same_v<T, BoolExpr>) return "bool";
        else if constexpr (std::is_same_v<T, VoidExpr>) return "void";
        else if constexpr (std::is_same_v<T, NoneExpr>) return "none";
        else if constexpr (std::is_same_v<T, ArrayExpr>) return "array";
        else if constexpr (std::is_same_v<T, IndexExpr>) return "index";
        else if constexpr (std::is_same_v<T, MemberExpr>) return "member";
        else if constexpr (std::is_same_v<T, MethodCallExpr>) return "method_call";
        else if constexpr (std::is_same_v<T, TryExpr>) return "try";
        else if constexpr (std::is_same_v<T, IfExpr>) return "if_expression";
        else if constexpr (std::is_same_v<T, NameExpr>) return "name";
        else if constexpr (std::is_same_v<T, UnaryExpr>) return "unary";
        else if constexpr (std::is_same_v<T, BinaryExpr>) return "binary";
        else if constexpr (std::is_same_v<T, CallExpr>) return "call";
        else return "expression";
    }, expr.data);
}

const char* stmt_kind(const Stmt& stmt) {
    return std::visit([](const auto& node) -> const char* {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, BindingStmt>) {
            return node.declared_type.name.rfind("$cli.", 0) == 0 ? "cli" : "binding";
        } else if constexpr (std::is_same_v<T, AssignStmt>) return "assignment";
        else if constexpr (std::is_same_v<T, ReturnStmt>) return "return";
        else if constexpr (std::is_same_v<T, LoopControlStmt>) return node.is_continue ? "continue" : "break";
        else if constexpr (std::is_same_v<T, ExprStmt>) return "expression_statement";
        else if constexpr (std::is_same_v<T, IfStmt>) return "if";
        else if constexpr (std::is_same_v<T, MainGuardStmt>) return "main_guard";
        else if constexpr (std::is_same_v<T, WhileStmt>) return "while";
        else if constexpr (std::is_same_v<T, ForStmt>) return "for";
        else if constexpr (std::is_same_v<T, MatchStmt>) return "match";
        else return "statement";
    }, stmt.data);
}

struct NodeCollector {
    std::string_view source;
    const CheckedProgram* checked{};
    std::vector<SourceNode> nodes;
    std::vector<std::string> parents;
    std::size_t next_id{};

    NodeCollector(std::string_view source_text, const CheckedProgram* checked_program)
        : source(source_text), checked(checked_program) {}

    std::string emit(std::string_view kind, SourceSpan span,
                     std::optional<Type> inferred = std::nullopt,
                     std::optional<std::string> authority = std::nullopt) {
        std::ostringstream id;
        id << 'n' << std::setw(6) << std::setfill('0') << next_id++;
        const auto node_id = id.str();
        const auto start = std::min(span.start.offset, source.size());
        const auto end = std::min(span.end.offset, source.size());
        const auto fragment = source.substr(start, end >= start ? end - start : 0);
        nodes.push_back(SourceNode{
            node_id,
            std::string(kind),
            span,
            sha256_hex(fragment),
            inferred,
            std::move(authority),
            parents.empty() ? std::optional<std::string>{}
                            : std::optional<std::string>{parents.back()},
            parents.size()});
        return node_id;
    }

    void expr(const Expr& expression) {
        std::optional<Type> type;
        if (checked) {
            if (const auto found = checked->expr_types.find(&expression);
                found != checked->expr_types.end()) {
                type = found->second;
            }
        }
        const auto id = emit(expr_kind(expression), expression.span, type);
        if (const auto* name = std::get_if<NameExpr>(&expression.data); name && name->this_qualifier) {
            nodes.back().this_qualified = true;
        }
        parents.push_back(id);
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, UnaryExpr>) {
                expr(*node.operand);
            } else if constexpr (std::is_same_v<T, TryExpr>) {
                if(node.value) expr(*node.value);
            } else if constexpr (std::is_same_v<T, IfExpr>) {
                for (std::size_t i = 0; i < node.conditions.size(); ++i) {
                    expr(*node.conditions[i]);
                    expr(*node.values[i]);
                }
                expr(*node.otherwise);
            } else if constexpr (std::is_same_v<T, ArrayExpr>) {
                for (const auto& element : node.elements) expr(*element);
            } else if constexpr (std::is_same_v<T, IndexExpr>) {
                expr(*node.base);
                for (const auto& item : node.items) {
                    if (item.index) expr(*item.index);
                    if (item.start) expr(*item.start);
                    if (item.stop) expr(*item.stop);
                    if (item.step) expr(*item.step);
                }
            } else if constexpr (std::is_same_v<T, MemberExpr>) {
                expr(*node.base);
            } else if constexpr (std::is_same_v<T, MethodCallExpr>) {
                expr(*node.receiver);
                for (const auto& arg : node.args) expr(*arg.value);
            } else if constexpr (std::is_same_v<T, StringTemplateExpr>) {
                for (const auto& part : node.expressions) expr(*part);
            } else if constexpr (std::is_same_v<T, BinaryExpr>) {
                expr(*node.left); expr(*node.right);
            } else if constexpr (std::is_same_v<T, CallExpr>) {
                for (const auto& arg : node.args) expr(*arg.value);
            }
        }, expression.data);
        parents.pop_back();
    }

    void stmt(const Stmt& statement) {
        std::optional<Type> inferred;
        std::optional<std::string> authority;
        if (const auto* binding = std::get_if<BindingStmt>(&statement.data)) {
            if (checked) {
                if (const auto it = checked->binding_types.find(&statement);
                    it != checked->binding_types.end()) {
                    inferred = it->second;
                }
            }
            if (binding->reference) {
                authority = binding->is_const ? "read_only_reference" : "read_write_reference";
            } else if (binding->is_const) {
                authority = "const_value";
            }
        }
        const auto id = emit(stmt_kind(statement), statement.span, inferred, authority);
        parents.push_back(id);
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, BindingStmt>) {
                if (node.declared_type.name.rfind("$cli.", 0) != 0 && node.value) expr(*node.value);
            } else if constexpr (std::is_same_v<T, AssignStmt>) {
                expr(*node.target); expr(*node.value);
            } else if constexpr (std::is_same_v<T, ReturnStmt>) {
                if(node.value) expr(*node.value);
            } else if constexpr (std::is_same_v<T, ExprStmt>) {
                if(node.value) expr(*node.value);
            } else if constexpr (std::is_same_v<T, IfStmt>) {
                expr(*node.condition);
                for (const auto& child : node.then_body) stmt(*child);
                for (const auto& child : node.else_body) stmt(*child);
            } else if constexpr (std::is_same_v<T, MainGuardStmt>) {
                for (const auto& child : node.body) stmt(*child);
            } else if constexpr (std::is_same_v<T, WhileStmt>) {
                expr(*node.condition);
                for (const auto& child : node.body) stmt(*child);
            } else if constexpr (std::is_same_v<T, ForStmt>) {
                expr(*node.iterable);
                for (const auto& child : node.body) stmt(*child);
            } else if constexpr (std::is_same_v<T, MatchStmt>) {
                if(node.value) expr(*node.value);
                for (const auto& c : node.cases) for (const auto& child : c.body) stmt(*child);
            }
        }, statement.data);
        parents.pop_back();
    }

    void function(const FunctionDecl& fn, const std::string& signature_name) {
        std::optional<Type> result;
        if (checked) {
            if (const auto found = checked->functions.find(signature_name);
                found != checked->functions.end()) {
                result = found->second.result;
            }
        }
        const auto id = emit("function", fn.span, result);
        parents.push_back(id);
        for (const auto& p : fn.parameters) if(p.default_value) expr(*p.default_value);
        for (const auto& statement : fn.body) stmt(*statement);
        parents.pop_back();
    }

    void class_decl(const ClassDecl& declaration) {
        const auto id = emit("class", declaration.span, Type::class_type(declaration.name));
        parents.push_back(id);
        const ClassTypeInfo* info = nullptr;
        if (checked) {
            if (const auto found = checked->classes.find(declaration.name);
                found != checked->classes.end()) {
                info = &found->second;
            }
        }
        for (const auto& field : declaration.fields) {
            std::optional<Type> field_type;
            if (info) {
                const auto it = std::find_if(
                    info->fields.begin(), info->fields.end(),
                    [&](const auto& item) { return item.name == field.name; });
                if (it != info->fields.end()) field_type = it->type;
            }
            const auto field_id = emit(
                "field", field.span, field_type,
                field.is_const ? std::optional<std::string>{"const_value"}
                               : std::optional<std::string>{});
            if (field.default_value) {
                parents.push_back(field_id);
                expr(*field.default_value);
                parents.pop_back();
            }
        }
        std::size_t constructors = 0;
        for (const auto& method : declaration.methods) {
            if (method.is_constructor) {
                function(method, member_function_name::constructor(declaration.name, constructors++));
                continue;
            }
            function(method, member_function_name::method(declaration.name, method.name));
        }
        parents.pop_back();
    }
};

} // namespace

std::string sha256_hex(std::string_view text) {
    return platform::sha256_hex(text);
}

SourceInspection inspect_syntax_source(std::string_view source) {
    Parser parser(Lexer(source).scan(), 20);
    const auto program = parser.parse();
    NodeCollector collector{source, nullptr};
    const auto belongs_to_root = [&](SourceSpan span) {
        return span.start.offset <= source.size() && span.end.offset <= source.size();
    };
    for (const auto& declaration : program.classes) {
        if (belongs_to_root(declaration.span)) collector.class_decl(declaration);
    }
    for (const auto& fn : program.functions) {
        if (belongs_to_root(fn.span)) collector.function(fn, fn.name);
    }
    for (const auto& statement : program.statements) {
        if (belongs_to_root(statement->span)) collector.stmt(*statement);
    }
    return SourceInspection{sha256_hex(source), std::move(collector.nodes)};
}

SourceInspection inspect_source(std::string_view source, const CheckedProgram& checked) {
    NodeCollector collector{source, &checked};
    const auto belongs_to_root = [&](SourceSpan span) {
        return span.start.offset <= source.size() && span.end.offset <= source.size();
    };
    for (const auto& declaration : checked.program.classes) {
        if (belongs_to_root(declaration.span)) collector.class_decl(declaration);
    }
    for (const auto& fn : checked.program.functions) {
        if (belongs_to_root(fn.span)) collector.function(fn, fn.name);
    }
    for (const auto& statement : checked.program.statements) {
        if (const auto* guard = std::get_if<MainGuardStmt>(&statement->data);
            guard && !guard->active) continue;
        if (belongs_to_root(statement->span)) collector.stmt(*statement);
    }
    return SourceInspection{sha256_hex(source), std::move(collector.nodes)};
}

std::string inspect_source_json(std::string_view source,
                                const CheckedProgram& checked,
                                std::string_view filename,
                                InspectOptions options) {
    const auto inspection = inspect_source(source, checked);
    std::ostringstream out;
    out << "{\"ok\":true,\"schema_version\":1,\"revision\":\"" << inspection.revision
        << "\",\"file\":\"" << json_escape(filename) << "\",\"nodes\":[";
    bool first = true;
    for (const auto& node : inspection.nodes) {
        if (options.kind && node.kind != *options.kind) continue;
        if (options.max_depth && node.depth > *options.max_depth) continue;
        if (!first) out << ',';
        first = false;
        const auto start = std::min(node.span.start.offset, source.size());
        const auto end = std::min(node.span.end.offset, source.size());
        const auto fragment = source.substr(start, end >= start ? end - start : 0);
        out << "{\"node_id\":\"" << node.node_id << "\",\"kind\":\"" << json_escape(node.kind)
            << "\",\"parent_id\":";
        if (node.parent_id) out << "\"" << json_escape(*node.parent_id) << "\"";
        else out << "null";
        out << ",\"depth\":" << node.depth
            << ",\"span\":{\"start\":{\"offset\":" << node.span.start.offset
            << ",\"line\":" << node.span.start.line << ",\"column\":" << node.span.start.column
            << "},\"end\":{\"offset\":" << node.span.end.offset << ",\"line\":" << node.span.end.line
            << ",\"column\":" << node.span.end.column << "}}";
        if (options.include_source) {
            out << ",\"source\":\"" << json_escape(fragment) << "\"";
        }
        out << ",\"source_hash\":\"" << node.source_hash << "\",\"inferred_type\":";
        if (node.inferred_type) out << '"' << type_name(*node.inferred_type) << '"';
        else out << "null";
        if (node.authority) {
            out << ",\"authority\":\"" << json_escape(*node.authority) << "\"";
        }
        if (node.this_qualified) out << ",\"this\":true";
        out << '}';
    }
    out << "],\"effects\":[";
    if (options.include_effects) {
        std::vector<std::string> function_names;
        function_names.reserve(checked.functions.size());
        for (const auto& [name, signature] : checked.functions) {
            (void)signature;
            function_names.push_back(name);
        }
        std::sort(function_names.begin(), function_names.end());
        for (std::size_t function_index = 0; function_index < function_names.size(); ++function_index) {
            if (function_index) out << ',';
            const auto& name = function_names[function_index];
            const auto& signature = checked.functions.at(name);
            out << "{\"function\":\"" << json_escape(name) << "\",\"receiver\":";
            write_effect_json(out, signature.receiver_effect);
            out << ",\"parameter_authority\":{";
            bool first_authority = true;
            for (const auto& parameter : signature.parameters) {
                if (parameter.name == "$receiver" ||
                    (!parameter.writable && !parameter.is_const)) {
                    continue;
                }
                if (!first_authority) out << ',';
                first_authority = false;
                const char* authority = parameter.writable
                    ? (parameter.is_const ? "read_only_reference" : "read_write_reference")
                    : "const_value";
                out << '"' << json_escape(parameter.name) << "\":\"" << authority << '"';
            }
            out << "},\"references\":{";
            std::vector<std::string> reference_names;
            reference_names.reserve(signature.reference_effects.size());
            for (const auto& [reference_name, effect] : signature.reference_effects) {
                (void)effect;
                reference_names.push_back(reference_name);
            }
            std::sort(reference_names.begin(), reference_names.end());
            for (std::size_t reference_index = 0; reference_index < reference_names.size(); ++reference_index) {
                if (reference_index) out << ',';
                const auto& reference_name = reference_names[reference_index];
                out << '"' << json_escape(reference_name) << "\":";
                write_effect_json(out, signature.reference_effects.at(reference_name));
            }
            out << "},\"return_initialized_fields\":"
                << json_string_set(signature.return_initialized_fields) << '}';
        }
    }
    out << "]}";
    return out.str();
}

} // namespace quidra
