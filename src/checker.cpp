#include "quidra/checker.hpp"
#include "quidra/abi/symbols.hpp"
#include "quidra/language.hpp"
#include "quidra/member_function_names.hpp"
#include "quidra/standard_classes.hpp"
#include "operator_policy.hpp"
#include "numeric_literal_policy.hpp"
#include "constant_integer_eval.hpp"
#include "constant_numeric_eval.hpp"
#include "nesting_budget.hpp"
#include "semantics/effect_summary.hpp"
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>
#include <tuple>
#include <unordered_set>

namespace quidra {
namespace {

Type simple(TypeKind kind) {
    return Type::simple(kind);
}

bool poisoned(const Type& type) {
    return type.kind == TypeKind::Invalid;
}

// The source spelling of a short expression, for a diagnostic that names
// it: names, members, calls, indexing and literals; anything else is "this
// value".
std::string spelled_expression(const Expr& expression, int depth = 0) {
    if (depth > 4) return "...";
    const auto arguments = [&](const std::vector<CallArg>& args) {
        std::string out;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i) out += ", ";
            if (args[i].name) out += *args[i].name + " = ";
            if (args[i].writable) out += "&";
            out += args[i].value ? spelled_expression(*args[i].value, depth + 1) : "?";
        }
        return out;
    };
    // A receiver or indexed base that is an operator expression keeps its
    // parentheses.
    const auto base = [&](const Expr& inner) {
        const auto text = spelled_expression(inner, depth + 1);
        return std::holds_alternative<BinaryExpr>(inner.data) ? "(" + text + ")" : text;
    };
    if (const auto* name = std::get_if<NameExpr>(&expression.data))
        return name->this_qualifier ? "this." + name->name : name->name;
    if (const auto* literal = std::get_if<IntegerExpr>(&expression.data))
        return literal->spelling.empty() ? std::to_string(literal->value) : literal->spelling;
    if (const auto* member = std::get_if<MemberExpr>(&expression.data))
        return base(*member->base) + "." + member->name;
    if (const auto* call = std::get_if<CallExpr>(&expression.data))
        return call->callee + "(" + arguments(call->args) + ")";
    if (const auto* method = std::get_if<MethodCallExpr>(&expression.data))
        return base(*method->receiver) + "." + method->method + "(" +
               arguments(method->args) + ")";
    if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
        if (index->items.size() == 1 && !index->items.front().slice && index->items.front().index)
            return base(*index->base) + "[" +
                   spelled_expression(*index->items.front().index, depth + 1) + "]";
    }
    if (const auto* binary = std::get_if<BinaryExpr>(&expression.data))
        return spelled_expression(*binary->left, depth + 1) + " " + binary->op + " " +
               spelled_expression(*binary->right, depth + 1);
    return "this value";
}

// The text of a mismatch between nat and another integer kind (lengths,
// counts, shapes and indices are nat; N1 converts nothing implicitly), or
// nothing when the mismatch is another one.
std::optional<std::string> nat_mismatch_message(const Expr& expression, const Type& received,
                                                const Type& expected) {
    const auto element = [](const Type& type) -> const Type& {
        return type.kind == TypeKind::Array && type.first ? *type.first : type;
    };
    const bool arrays = received.kind == TypeKind::Array && expected.kind == TypeKind::Array;
    if (!arrays && (received.kind == TypeKind::Array || expected.kind == TypeKind::Array))
        return std::nullopt;
    const auto& from = element(received);
    const auto& to = element(expected);
    if (!is_integer_family_type(from) || !is_integer_family_type(to) || from == to)
        return std::nullopt;
    const auto text = spelled_expression(expression);
    if (from.kind == TypeKind::Nat) {
        return "'" + text + "' has type " + type_name(received) + "; convert explicitly: " +
               type_name(to) + "(" + text + ").";
    }
    if (to.kind == TypeKind::Nat) {
        return "'" + text + "' has type " + type_name(received) + "; this parameter is " +
               type_name(expected) + ": write nat(" + text + ").";
    }
    return std::nullopt;
}

// The case of an expected type that a literal of the other family met (the
// type itself unless it is a union), for the diagnostic.
Type literal_target(const Type& expected, bool (*family)(const Type&)) {
    if (expected.kind == TypeKind::Union)
        for (const auto& candidate : expected.cases)
            if (family(candidate)) return candidate;
    return expected;
}

// The value of a numeric literal operand (an integer or real literal, or its
// negation), for the compile-time power checks; nothing for any other
// expression.
std::optional<double> literal_number_value(const Expr& expression) {
    if (const auto* integer = std::get_if<IntegerExpr>(&expression.data))
        return static_cast<double>(integer->value);
    if (const auto* real = std::get_if<RealLiteralExpr>(&expression.data))
        return real->value;
    if (const auto* unary = std::get_if<UnaryExpr>(&expression.data)) {
        if (unary->op != "-") return std::nullopt;
        const auto value = literal_number_value(*unary->operand);
        if (!value) return std::nullopt;
        return -*value;
    }
    return std::nullopt;
}

bool printable(const Type& type) {
    return poisoned(type) || is_numeric(type) || type.kind == TypeKind::Bool ||
           type.kind == TypeKind::String || type.kind == TypeKind::Bin ||
           type.kind == TypeKind::Error;
}

using numeric_policy::NumericLiteralContext;
using numeric_policy::NumericLiteralCategory;
using numeric_policy::complex_category;
using numeric_policy::integer_category;
using numeric_policy::single_family_category;
using numeric_policy::direct_numeric_literal_family;
using numeric_policy::numeric_literal_context;
using numeric_policy::numeric_literal_family;

// An if-expression gives a value: `&` cannot take its address, a reference
// cannot be bound to it, and its branches are not addresses (IF_EXPRESSION).
constexpr char if_expression_reference_message[] =
    "An if-expression gives a value; a reference needs storage: use an if statement.";

bool is_if_expression(const Expr& expression) {
    return std::holds_alternative<IfExpr>(expression.data);
}

bool runtime_reserved_c_symbol(std::string_view symbol) {
    namespace ns = abi::symbol_namespace;
    if (symbol == ns::entry || symbol.starts_with(ns::user_prefix) ||
        symbol.starts_with(ns::runtime_prefix) || symbol.starts_with(ns::internal_prefix)) {
        return true;
    }
    static const std::unordered_set<std::string> symbols{
        "printf", "puts", "strlen", "strcmp", "strchr", "free",
        "memcpy", "memmove", "memset", "memcmp", "snprintf", "fflush",
        "exit", "_Exit",
        "sin", "sinf", "cos", "cosf", "tan", "tanf",
        "log", "logf", "exp", "expf", "pow", "powf"};
    return symbols.contains(std::string(symbol));
}

// The C23 keywords. An exported function's name is its C symbol, so it may
// not be one (FFI_SYMBOL).
bool c_keyword(std::string_view symbol) {
    static const std::unordered_set<std::string_view> keywords{
        "alignas", "alignof", "auto", "bool", "break", "case", "char", "const",
        "constexpr", "continue", "default", "do", "double", "else", "enum", "extern",
        "false", "float", "for", "goto", "if", "inline", "int", "long", "nullptr",
        "register", "restrict", "return", "short", "signed", "sizeof", "static",
        "static_assert", "struct", "switch", "thread_local", "true", "typedef", "typeof",
        "typeof_unqual", "union", "unsigned", "void", "volatile", "while", "_Alignas",
        "_Alignof", "_Atomic", "_BitInt", "_Bool", "_Complex", "_Decimal128",
        "_Decimal32", "_Decimal64", "_Generic", "_Imaginary", "_Noreturn",
        "_Static_assert", "_Thread_local"};
    return keywords.contains(symbol);
}

// Identifiers C reserves for the implementation: a leading underscore
// followed by an uppercase letter, or two leading underscores.
bool reserved_c_identifier(std::string_view symbol) {
    return symbol.size() >= 2 && symbol[0] == '_' &&
           (symbol[1] == '_' || std::isupper(static_cast<unsigned char>(symbol[1])));
}

// The spelled types that cross the exported C boundary by value: the
// fixed-width scalars with a defined C mapping (the arbitrary-precision int
// and nat have no fixed C representation).
bool foreign_export_scalar(const TypeName& type) {
    static const std::unordered_set<std::string_view> scalars{
        "int8", "int16", "int32", "int64", "nat8", "nat16", "nat32", "nat64",
        "real32", "real64"};
    return type.arguments.empty() && type.function_parameters.empty() &&
           type.array_depth == 0 && type.tensor_shape_prefix.empty() &&
           scalars.contains(type.name);
}

// Why a spelled parameter or result type cannot cross the exported C
// boundary, completing "Exported C parameter 'x' ...".
std::string foreign_export_type_problem(const TypeName& type) {
    if (type.array_depth != 0) return "is an array, a managed value that cannot cross the C ABI.";
    if (type.name == "union") return "is a union, which cannot cross the C ABI.";
    if (type.name == "fn") return "is a function value, which cannot cross the C ABI.";
    if (type.name == "tensor") return "is a tensor, a managed value that cannot cross the C ABI.";
    if (type.name == "int")
        return "has type int, which has no fixed C representation; write int64.";
    if (type.name == "nat")
        return "has type nat, which has no fixed C representation; write nat64.";
    if (type.name == "real")
        return "has type " + type.name + ", which has no fixed C representation.";
    if (type.name == "bool")
        return "has type bool, which is not part of the exported C subset; use an integer type.";
    if (type.name == "string" || type.name == "bin")
        return "has type " + type.name + ", a managed value that cannot cross the C ABI.";
    return "has type " + type.name + ", which has no C mapping.";
}

bool storage_paths_overlap(
    const std::pair<std::string, std::string>& left,
    const std::pair<std::string, std::string>& right) {
    // A flow-dependent reference target may be one of several storages. It must
    // conservatively overlap every concrete path until an explicit rebind makes
    // the target unique again.
    if (left.first == "$unknown" || right.first == "$unknown") return true;
    if (left.first != right.first) return false;
    if (left.second.empty() || right.second.empty()) return true;
    if (left.second == right.second) return true;

    const auto contains = [](const std::string& parent, const std::string& child) {
        return child.rfind(parent + ".", 0) == 0 ||
               child.rfind(parent + "[]", 0) == 0;
    };
    return contains(left.second, right.second) || contains(right.second, left.second);
}

using ReferenceRoots = std::unordered_map<std::string, std::string>;
using ReferencePaths =
    std::unordered_map<std::string, std::pair<std::string, std::string>>;

struct ReferenceTargetState {
    ReferencePaths paths;
    std::unordered_set<std::string> unknown;
};

struct ReferenceTargetJoin {
    ReferenceRoots roots;
    ReferencePaths paths;
    std::unordered_set<std::string> unknown;
    std::unordered_set<std::string> ambiguous;
};

struct AbstractReferenceTarget {
    bool unknown{};
    std::pair<std::string, std::string> path;
};

AbstractReferenceTarget reference_target(
    const ReferenceTargetState& state, const std::string& name) {
    if (state.unknown.contains(name)) return {true, {name, ""}};
    const auto it = state.paths.find(name);
    if (it == state.paths.end()) return {true, {name, ""}};
    return {false, it->second};
}

bool same_reference_target(
    const AbstractReferenceTarget& left, const AbstractReferenceTarget& right) {
    if (left.unknown || right.unknown) return left.unknown == right.unknown;
    return left.path == right.path;
}

ReferenceTargetJoin join_reference_targets(
    const ReferenceRoots& before_roots,
    const ReferencePaths& before_paths,
    const std::unordered_set<std::string>& before_unknown,
    const std::vector<ReferenceTargetState>& continuing) {
    ReferenceTargetJoin result{before_roots, before_paths, before_unknown, {}};
    if (continuing.empty()) return result;

    for (const auto& [name, _] : before_paths) {
        const auto first = reference_target(continuing.front(), name);
        bool same = true;
        for (std::size_t i = 1; i < continuing.size(); ++i) {
            if (!same_reference_target(first, reference_target(continuing[i], name))) {
                same = false;
                break;
            }
        }

        if (!same) {
            result.roots[name] = name;
            result.paths[name] = {name, ""};
            result.unknown.insert(name);
            result.ambiguous.insert(name);
            continue;
        }

        if (first.unknown) {
            result.roots[name] = name;
            result.paths[name] = {name, ""};
            result.unknown.insert(name);
            if (!before_unknown.contains(name)) result.ambiguous.insert(name);
            continue;
        }

        result.roots[name] = first.path.first;
        result.paths[name] = first.path;
        result.unknown.erase(name);
    }
    return result;
}

Type merge_shaped_flow_facts(
    const Type& base, const std::vector<Type>& continuing) {
    const bool shaped = base.kind == TypeKind::Tensor;
    if (!shaped || continuing.empty()) return base;

    auto merged = base;
    merged.length = continuing.front().kind == base.kind
        ? continuing.front().length
        : -1;
    for (const auto& state : continuing) {
        if (state.kind != base.kind || state.length != merged.length) {
            merged.length = -1;
            break;
        }
    }

    merged.tensor_known_shape_prefix.clear();
    for (std::size_t axis = 0;; ++axis) {
        if (merged.length >= 0 &&
            axis >= static_cast<std::size_t>(merged.length)) {
            break;
        }
        std::optional<long long> extent;
        bool known_on_every_path = true;
        for (const auto& state : continuing) {
            if (state.kind != base.kind) {
                known_on_every_path = false;
                break;
            }
            const auto current = tensor_known_extent(state, axis);
            if (!current) {
                known_on_every_path = false;
                break;
            }
            if (!extent) {
                extent = current;
            } else if (*extent != *current) {
                known_on_every_path = false;
                break;
            }
        }
        if (!known_on_every_path) break;
        merged.tensor_known_shape_prefix.push_back(*extent);
    }
    return merged;
}

// A name expression that can denote a binding: not `this.NAME`, which only
// ever denotes a receiver field. Name-keyed facts about locals, parameters
// and references are looked up through it.
const NameExpr* binding_name(const Expr& expression) {
    const auto* name = std::get_if<NameExpr>(&expression.data);
    return name && !name->this_qualifier ? name : nullptr;
}

void collect_assigned_bindings(
    const std::vector<StmtPtr>& body, std::unordered_set<std::string>& names) {
    for (const auto& statement : body) {
        if (const auto* assignment = std::get_if<AssignStmt>(&statement->data)) {
            if (const auto* name = binding_name(*assignment->target)) {
                names.insert(name->name);
            }
            continue;
        }
        if (const auto* branch = std::get_if<IfStmt>(&statement->data)) {
            collect_assigned_bindings(branch->then_body, names);
            collect_assigned_bindings(branch->else_body, names);
            continue;
        }
        if (const auto* loop = std::get_if<WhileStmt>(&statement->data)) {
            collect_assigned_bindings(loop->body, names);
            continue;
        }
        if (const auto* loop = std::get_if<ForStmt>(&statement->data)) {
            collect_assigned_bindings(loop->body, names);
            continue;
        }
        if (const auto* match = std::get_if<MatchStmt>(&statement->data)) {
            for (const auto& match_case : match->cases) {
                collect_assigned_bindings(match_case.body, names);
            }
        }
    }
}

void weaken_loop_tensor_facts(
    std::unordered_map<std::string, Type>& variables,
    const std::unordered_set<std::string>& assigned) {
    for (const auto& name : assigned) {
        const auto it = variables.find(name);
        if (it == variables.end() ||
            it->second.kind != TypeKind::Tensor) continue;
        it->second.length = it->second.tensor_shape_prefix.empty()
            ? -1
            : static_cast<long long>(it->second.tensor_shape_prefix.size());
        it->second.tensor_known_shape_prefix.clear();
    }
}

void collect_rebound_references(
    const std::vector<StmtPtr>& body, std::unordered_set<std::string>& names) {
    for (const auto& statement : body) {
        if (const auto* rebind = std::get_if<RebindStmt>(&statement->data)) {
            names.insert(rebind->name);
            continue;
        }
        if (const auto* branch = std::get_if<IfStmt>(&statement->data)) {
            collect_rebound_references(branch->then_body, names);
            collect_rebound_references(branch->else_body, names);
            continue;
        }
        if (const auto* loop = std::get_if<WhileStmt>(&statement->data)) {
            collect_rebound_references(loop->body, names);
            continue;
        }
        if (const auto* loop = std::get_if<ForStmt>(&statement->data)) {
            collect_rebound_references(loop->body, names);
            continue;
        }
        if (const auto* match = std::get_if<MatchStmt>(&statement->data)) {
            for (const auto& match_case : match->cases) {
                collect_rebound_references(match_case.body, names);
            }
        }
    }
}

void union_set(std::unordered_set<std::string>& destination,
               const std::unordered_set<std::string>& source) {
    destination.insert(source.begin(), source.end());
}

std::unordered_map<std::string, StorageEffect> merge_reference_branches(
    const std::unordered_map<std::string, StorageEffect>& before,
    const std::unordered_map<std::string, StorageEffect>& yes,
    const std::unordered_map<std::string, StorageEffect>& no,
    bool yes_terminates, bool no_terminates) {
    std::unordered_map<std::string, StorageEffect> result = before;
    std::unordered_set<std::string> names;
    for (const auto& [name, effect] : yes) {
        (void)effect;
        names.insert(name);
    }
    for (const auto& [name, effect] : no) {
        (void)effect;
        names.insert(name);
    }
    for (const auto& name : names) {
        const auto yes_it = yes.find(name);
        const auto no_it = no.find(name);
        const StorageEffect empty;
        const auto& yes_effect = yes_it == yes.end() ? empty : yes_it->second;
        const auto& no_effect = no_it == no.end() ? empty : no_it->second;
        auto& merged = result[name];
        merged.required.clear();
        merged.writes.clear();
        merged.invalidates.clear();
        union_set(merged.required, yes_effect.required);
        union_set(merged.required, no_effect.required);
        union_set(merged.writes, yes_effect.writes);
        union_set(merged.writes, no_effect.writes);
        union_set(merged.invalidates, yes_effect.invalidates);
        union_set(merged.invalidates, no_effect.invalidates);
        if (yes_terminates) {
            merged.initializes = no_effect.initializes;
        } else if (no_terminates) {
            merged.initializes = yes_effect.initializes;
        } else {
            merged.initializes.clear();
            for (const auto& path : yes_effect.initializes) {
                if (no_effect.initializes.contains(path)) merged.initializes.insert(path);
            }
        }
    }
    return result;
}

std::unordered_map<std::string, StorageEffect> merge_reference_loop(
    const std::unordered_map<std::string, StorageEffect>& before,
    const std::unordered_map<std::string, StorageEffect>& body) {
    auto result = before;
    for (const auto& [name, body_effect] : body) {
        auto& merged = result[name];
        union_set(merged.required, body_effect.required);
        union_set(merged.writes, body_effect.writes);
        union_set(merged.invalidates, body_effect.invalidates);
    }
    return result;
}


// Static extent of one axis of a tensor-to-tensor arithmetic result, from
// the operands' static extents (nullopt: unknown). It follows the runtime
// broadcast rule (tensor_broadcast_shape): equal extents stay, and a 1 takes
// the other side's extent, 0 included. A known extent other than 1 is the
// result whatever the unknown side holds (it can only match or be 1);
// incompatible known extents fail at runtime and say nothing here.
std::optional<long long> broadcast_result_extent(
    std::optional<long long> left, std::optional<long long> right) {
    if (left && right) {
        if (*left == *right || *right == 1) return left;
        if (*left == 1) return right;
        return std::nullopt;
    }
    const auto known = left ? left : right;
    if (known && *known != 1) return known;
    return std::nullopt;
}

// The static shape of a tensor-to-tensor arithmetic result. Its source
// pattern stays the left operand's, except that an axis the pattern fixes to
// 1 takes the broadcast extent: the right operand's statically known extent
// on that axis, from its pattern or its flow facts (so a known 1 keeps the
// 1), or a wildcard when that extent is not known. Its flow facts are the
// known prefix of the broadcast shape. Copying the left operand's shape
// unchanged claimed [1, 3] for [1, 3] + [0, 3] (and for [1, 3] + [2, 3]) and
// rejected correct programs.
//
// The wildcard is needed even though keeping the 1 would leave the type
// identical to the left operand's: a fixed pattern extent is a static fact
// (tensor_known_extent falls back to it where the flow facts end), so a 1
// kept for an unknown extent would flow into derived facts, e.g. [3, 1] for
// (one + unknown).transpose(0, 1) when the value is [3, 2] or [3, 0].
void apply_broadcast_shape(const Type& left, const Type& right, Type& result) {
    for (std::size_t axis = 0; axis < result.tensor_shape_prefix.size(); ++axis) {
        if (result.tensor_shape_prefix[axis] != 1) continue;
        const auto extent = broadcast_result_extent(1, tensor_known_extent(right, axis));
        result.tensor_shape_prefix[axis] = extent ? *extent : -1;
    }
    result.tensor_known_shape_prefix.clear();
    for (std::size_t axis = 0;; ++axis) {
        if (result.length >= 0 && axis >= static_cast<std::size_t>(result.length)) break;
        const auto extent = broadcast_result_extent(
            tensor_known_extent(left, axis), tensor_known_extent(right, axis));
        if (!extent) break;
        result.tensor_known_shape_prefix.push_back(*extent);
    }
}

// check_block can now throw NESTING_DEPTH, which makes the previously
// unreachable loop_depth_ leak reachable. A leaked loop_depth_ silently
// legalises break/continue outside a loop for the rest of the check.
struct ScopedCounter {
    explicit ScopedCounter(std::size_t& value) : value_(value) { ++value_; }
    ~ScopedCounter() { --value_; }
    ScopedCounter(const ScopedCounter&) = delete;
    ScopedCounter& operator=(const ScopedCounter&) = delete;

private:
    std::size_t& value_;
};

} // namespace

[[noreturn]] void Checker::error(std::string code, std::string message, SourceSpan span) const {
    throw CompileError(Diagnostic{std::move(code), std::move(message), span});
}

void Checker::record(const CompileError& compile_error) {
    if (suppress_diagnostics_) return;
    if (diagnostics_.size() >= max_errors_) {
        throw CompileErrors(diagnostics_, true);
    }
    diagnostics_.push_back(compile_error.diagnostic());
}

const ClassFieldType* Checker::find_field(const std::string& class_name, const std::string& field) const {
    const auto class_it = classes_.find(class_name);
    if (class_it == classes_.end()) return nullptr;
    const auto& fields = class_it->second.fields;
    const auto it = std::find_if(fields.begin(), fields.end(), [&](const auto& item) { return item.name == field; });
    return it == fields.end() ? nullptr : &*it;
}

bool Checker::standard_library_class(const std::string& class_name) const {
    const auto class_it = classes_.find(class_name);
    return class_it != classes_.end() && class_it->second.standard_library;
}

const std::string* Checker::find_method(const std::string& class_name, const std::string& method) const {
    const auto class_it = classes_.find(class_name);
    if (class_it == classes_.end()) return nullptr;
    const auto it = class_it->second.methods.find(method);
    return it == class_it->second.methods.end() ? nullptr : &it->second;
}

const ClassFieldType* Checker::receiver_field(const Expr& expression) const {
    const auto* name = std::get_if<NameExpr>(&expression.data);
    if (!name || !name->this_qualifier || current_class_.empty()) return nullptr;
    return find_field(current_class_, name->name);
}

// The root of a member or index chain, when it is a bare name of a field
// that no binding shadows: the field must be written `this.NAME`.
void Checker::reject_bare_storage_root(const Expr& expression) const {
    const Expr* root = &expression;
    for (;;) {
        if (const auto* member = std::get_if<MemberExpr>(&root->data)) root = member->base.get();
        else if (const auto* index = std::get_if<IndexExpr>(&root->data)) root = index->base.get();
        else break;
    }
    if (const auto* name = binding_name(*root); name && !variables_.contains(name->name)) {
        reject_bare_field(name->name, root->span);
    }
}

// A bare name that resolves to nothing while the enclosing class has a field
// of that name: the field must be written `this.NAME`.
void Checker::reject_bare_field(const std::string& name, SourceSpan span) const {
    if (current_class_.empty() || !find_field(current_class_, name)) return;
    error("THIS_QUALIFIER",
          "'" + name + "' is a field of '" + current_class_ + "': write 'this." + name + "'.",
          span);
}

// `this.NAME`: valid inside method and constructor bodies and in a method's
// signature shapes, where the receiver exists; NAME must be a field of the
// enclosing class.
const ClassFieldType& Checker::check_this_field(const Expr& expression, const NameExpr& name) {
    const auto span = name.this_qualifier
                          ? SourceSpan{name.this_qualifier->start, expression.span.end}
                          : expression.span;
    if (current_class_.empty() || this_unavailable_) {
        error("THIS_QUALIFIER", "'this' is only valid inside method and constructor bodies.", span);
    }
    const auto* field = find_field(current_class_, name.name);
    if (!field) {
        error("UNKNOWN_MEMBER",
              "Class '" + current_class_ + "' has no field '" + name.name + "'.", expression.span);
    }
    if (field->is_private && current_class_ != field->owner) {
        error("PRIVATE_MEMBER",
              "Private field '" + name.name + "' is only accessible inside class '" +
                  field->owner + "'.",
              expression.span);
    }
    return *field;
}

// Methods are called bare, so a binding cannot share a method's name. Fields
// are written `this.NAME` and never bare, so bindings may share their names.
bool Checker::member_name_visible(const std::string& name) const {
    if (current_class_.empty()) return false;
    return find_method(current_class_, name) != nullptr;
}

namespace {
std::string module_scoped_name(const std::string& module_namespace, const std::string& name) {
    return module_namespace.empty() ? name : module_namespace + "." + name;
}
}  // namespace

// A function, class, or enum name that a local binding would shadow. Imported
// declarations carry their module namespace ("vision.crop"), so a body is
// checked against the declarations of its own module: package code never
// sees the importing program's globals, and the root file never sees a
// package's unqualified names.
bool Checker::declaration_name_visible(const std::string& name) const {
    return functions_.contains(module_scoped_name(current_module_namespace_, name)) ||
           type_name_declared_in(name, current_module_namespace_);
}

// A class or enum `name` declared by the module `module_namespace` ("" for the
// root file). Parameters and fields of an imported declaration are checked
// with its own module_namespace, never against the importing program.
bool Checker::type_name_declared_in(const std::string& name, const std::string& module_namespace) const {
    const auto scoped = module_scoped_name(module_namespace, name);
    return class_names_.contains(scoped) || enum_types_.contains(scoped);
}

// The checker-internal name of the function that `name`, as a function value
// or a callee in the current body, denotes. Root code sees its own functions
// and the qualified members of its imports. Code in an imported module sees
// the same from its own side: a bare name denotes the module's own function
// ("vision.crop") and never the importing program's, which is not in its
// lexical environment. Standard-library bodies keep the merged lookup.
std::optional<std::string> Checker::visible_function_name(const std::string& name) const {
    if (!current_module_namespace_.empty() &&
        current_module_namespace_.rfind("$std.", 0) != 0) {
        if (auto own = module_scoped_name(current_module_namespace_, name);
            functions_.contains(own)) {
            return own;
        }
        if (root_functions_.contains(name)) return std::nullopt;
    }
    if (functions_.contains(name)) return name;
    return std::nullopt;
}

bool Checker::equality_supported(const Type& type) const {
    std::unordered_set<std::string> visiting;
    std::function<bool(const Type&)> supported =
        [&](const Type& current) -> bool {
            if (poisoned(current) || is_numeric(current) || current.kind == TypeKind::Bool ||
                current.kind == TypeKind::String || current.kind == TypeKind::Bin ||
                current.kind == TypeKind::Error) {
                return true;
            }
            if (current.kind == TypeKind::Array) {
                return current.first && supported(*current.first);
            }
            if (current.kind == TypeKind::Class) {
                if (standard_class::is_runtime_handle(current.class_name)) return false;
                if (!visiting.insert(current.class_name).second) return true;
                const auto it = classes_.find(current.class_name);
                if (it == classes_.end()) return false;
                for (const auto& field : it->second.fields) {
                    if (!supported(field.type)) {
                        visiting.erase(current.class_name);
                        return false;
                    }
                }
                visiting.erase(current.class_name);
                return true;
            }
            return false;
        };
    return supported(type);
}

bool Checker::fully_initialized_for_equality(const Expr& expression, const Type& type) const {
    if (type.kind != TypeKind::Class) return true;
    const auto paths = initialized_paths_for_expr(expression);
    std::function<bool(const Type&, const std::string&)> complete =
        [&](const Type& current, const std::string& prefix) -> bool {
            if (current.kind != TypeKind::Class) return true;
            const auto class_it = classes_.find(current.class_name);
            if (class_it == classes_.end()) return false;
            for (const auto& field : class_it->second.fields) {
                const auto path = prefix.empty() ? field.name : prefix + "." + field.name;
                if (!paths.contains(path)) return false;
                if (field.type.kind == TypeKind::Class && !complete(field.type, path)) return false;
            }
            return true;
        };
    return complete(type, "");
}

std::unordered_set<std::string> Checker::complete_class_paths(const Type& type) const {
    std::unordered_set<std::string> result;
    std::unordered_set<std::string> visiting;
    std::function<void(const Type&, const std::string&)> collect =
        [&](const Type& current, const std::string& prefix) {
            if (current.kind != TypeKind::Class) return;
            const auto class_it = classes_.find(current.class_name);
            if (class_it == classes_.end()) return;
            const bool recurse = visiting.insert(current.class_name).second;
            for (const auto& field : class_it->second.fields) {
                const auto path = prefix.empty() ? field.name : prefix + "." + field.name;
                result.insert(path);
                if (recurse && field.type.kind == TypeKind::Class) collect(field.type, path);
            }
            if (recurse) visiting.erase(current.class_name);
        };
    collect(type, "");
    return result;
}

std::string Checker::reference_root(const std::string& name) const {
    if (const auto it = reference_roots_.find(name); it != reference_roots_.end()) return it->second;
    return name;
}

std::unordered_set<std::string> Checker::initialized_paths_for_expr(const Expr& expression) const {
    // Name/member initialization is flow-sensitive. Never let a previous fixed-point
    // iteration's expression memo override the current data-flow state.
    if (!std::holds_alternative<NameExpr>(expression.data) &&
        !std::holds_alternative<MemberExpr>(expression.data)) {
        if (const auto it = class_expr_initialized_paths_.find(&expression);
            it != class_expr_initialized_paths_.end()) {
            return it->second;
        }
    }
    if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        if (!name->this_qualifier && current_reference_parameters_.contains(name->name)) {
            std::unordered_set<std::string> paths;
            if (const auto it = current_reference_effects_.find(name->name);
                it != current_reference_effects_.end()) {
                for (const auto& path : it->second.required) {
                    if (!path.empty()) paths.insert(path);
                }
                for (const auto& path : it->second.initializes) {
                    if (!path.empty()) paths.insert(path);
                }
            }
            return paths;
        }
        if (receiver_field(expression)) {
            if (!current_receiver_effect_.initializes.contains(name->name)) return {};
            std::unordered_set<std::string> nested;
            const auto prefix = name->name + ".";
            for (const auto& path : current_receiver_effect_.initializes) {
                if (path.rfind(prefix, 0) == 0) nested.insert(path.substr(prefix.size()));
            }
            return nested;
        }
        if (name->this_qualifier) return {};

        std::string root = reference_root(name->name);
        std::string base_path;
        if (const auto ref = reference_paths_.find(name->name); ref != reference_paths_.end()) {
            root = ref->second.first;
            base_path = ref->second.second;
        }
        const auto it = class_initialized_paths_.find(root);
        if (it == class_initialized_paths_.end()) return {};
        if (base_path.empty()) return it->second;
        if (!it->second.contains(base_path)) return {};
        std::unordered_set<std::string> nested;
        const auto prefix = base_path + ".";
        for (const auto& path : it->second) {
            if (path.rfind(prefix, 0) == 0) nested.insert(path.substr(prefix.size()));
        }
        return nested;
    }
    if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
        const auto base = initialized_paths_for_expr(*member->base);
        if (!base.contains(member->name)) return {};
        std::unordered_set<std::string> nested;
        const auto prefix = member->name + ".";
        for (const auto& path : base) {
            if (path.rfind(prefix, 0) == 0) nested.insert(path.substr(prefix.size()));
        }
        return nested;
    }
    return {};
}

std::optional<std::pair<std::string, std::string>> Checker::member_storage_path(const Expr& expression) const {
    if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        if (name->this_qualifier || !variables_.contains(name->name)) return std::nullopt;
        if (const auto ref = reference_paths_.find(name->name); ref != reference_paths_.end()) {
            return ref->second;
        }
        return std::pair<std::string, std::string>{reference_root(name->name), ""};
    }
    if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
        auto base = member_storage_path(*member->base);
        if (!base) return std::nullopt;
        if (!base->second.empty()) base->second += ".";
        base->second += member->name;
        return base;
    }
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>>
Checker::writable_storage_path(const Expr& expression) const {
    if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
        auto base = writable_storage_path(*index->base);
        if (!base) return std::nullopt;
        base->second += "[]";
        return base;
    }
    if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
        auto base = writable_storage_path(*member->base);
        if (!base) return std::nullopt;
        if (!base->second.empty()) base->second += ".";
        base->second += member->name;
        return base;
    }
    return member_storage_path(expression);
}

std::optional<std::pair<std::string, std::string>>
Checker::alias_storage_path(const Expr& expression) const {
    if (unknown_reference_access_path(expression)) {
        return std::pair<std::string, std::string>{"$unknown", ""};
    }
    if (const auto local = writable_storage_path(expression)) return local;
    if (const auto receiver = current_receiver_path(expression)) {
        return std::pair<std::string, std::string>{"$receiver", *receiver};
    }
    if (const auto reference = current_reference_parameter_path(expression)) {
        return std::pair<std::string, std::string>{"$reference." + reference->first,
                                                   reference->second};
    }
    return std::nullopt;
}

bool Checker::unknown_reference_access_path(const Expr& expression) const {
    if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        return !name->this_qualifier && unknown_reference_targets_.contains(name->name);
    }
    if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
        return unknown_reference_access_path(*member->base);
    }
    if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
        return unknown_reference_access_path(*index->base);
    }
    return false;
}

std::optional<std::string> Checker::current_receiver_path(const Expr& expression) const {
    if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        if (receiver_field(expression)) return name->name;
        return std::nullopt;
    }
    if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
        auto base = current_receiver_path(*member->base);
        if (!base) return std::nullopt;
        return *base + "." + member->name;
    }
    if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
        auto base = current_receiver_path(*index->base);
        if (!base) return std::nullopt;
        return *base + "[]";
    }
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>>
Checker::current_reference_parameter_path(const Expr& expression) const {
    if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        if (!name->this_qualifier && current_reference_parameters_.contains(name->name)) {
            return std::pair<std::string, std::string>{name->name, ""};
        }
        return std::nullopt;
    }
    if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
        auto base = current_reference_parameter_path(*member->base);
        if (!base) return std::nullopt;
        if (!base->second.empty()) base->second += ".";
        base->second += member->name;
        return base;
    }
    if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
        auto base = current_reference_parameter_path(*index->base);
        if (!base) return std::nullopt;
        base->second += "[]";
        return base;
    }
    return std::nullopt;
}

bool Checker::const_access_path(const Expr& expression) const {
    std::function<std::optional<Type>(const Expr&)> storage_type =
        [&](const Expr& current) -> std::optional<Type> {
            if (std::holds_alternative<NameExpr>(current.data)) {
                if (const auto* field = receiver_field(current)) return field->type;
                if (const auto* name = binding_name(current)) {
                    if (const auto it = variables_.find(name->name); it != variables_.end()) return it->second;
                }
                return std::nullopt;
            }
            if (const auto* member = std::get_if<MemberExpr>(&current.data)) {
                const auto base = storage_type(*member->base);
                if (!base || base->kind != TypeKind::Class) return std::nullopt;
                if (const auto* field = find_field(base->class_name, member->name)) return field->type;
                return std::nullopt;
            }
            if (const auto* index = std::get_if<IndexExpr>(&current.data)) {
                const auto base = storage_type(*index->base);
                if (!base) return std::nullopt;
                if (base->kind == TypeKind::Array) return *base->first;
                if (base->kind == TypeKind::Bin) return Type::simple(TypeKind::Bin);
                if (base->kind == TypeKind::Tensor) return *base->first;
            }
            return std::nullopt;
        };

    if (std::holds_alternative<NameExpr>(expression.data)) {
        if (const auto* field = receiver_field(expression)) return field->is_const;
        if (const auto* name = binding_name(expression)) return const_bindings_.contains(name->name);
        return false;
    }
    if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
        if (const_access_path(*member->base)) return true;
        const auto base = storage_type(*member->base);
        if (base && base->kind == TypeKind::Class) {
            if (const auto* field = find_field(base->class_name, member->name)) return field->is_const;
        }
        return false;
    }
    if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
        return const_access_path(*index->base);
    }
    return false;
}

bool Checker::stable_addressable_storage(const Expr& expression) const {
    return writable_storage_path(expression).has_value() ||
           current_receiver_path(expression).has_value() ||
           current_reference_parameter_path(expression).has_value();
}

bool Checker::stable_writable_storage(const Expr& expression) const {
    return stable_addressable_storage(expression) && !const_access_path(expression);
}

bool Checker::storage_initialized(const Expr& expression) const {
    if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        if (name->this_qualifier) {
            return receiver_field(expression) &&
                   current_receiver_effect_.initializes.contains(name->name);
        }
        if (current_reference_parameters_.contains(name->name)) {
            if (const auto it = current_reference_effects_.find(name->name);
                it != current_reference_effects_.end()) {
                return it->second.required.contains("") || it->second.initializes.contains("");
            }
            return false;
        }
        if (variables_.contains(name->name)) {
            if (const auto ref = reference_paths_.find(name->name); ref != reference_paths_.end()) {
                if (ref->second.second.empty()) {
                    return initialized_.contains(ref->second.first);
                }
                const auto it = class_initialized_paths_.find(ref->second.first);
                return it != class_initialized_paths_.end() && it->second.contains(ref->second.second);
            }
            const auto root = reference_root(name->name);
            return initialized_.contains(root) || initialized_.contains(name->name);
        }
        if (receiver_field(expression)) {
            return current_receiver_effect_.initializes.contains(name->name);
        }
        return false;
    }
    if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
        const auto paths = initialized_paths_for_expr(*member->base);
        return paths.contains(member->name);
    }
    if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
        return storage_initialized(*index->base);
    }
    return false;
}

std::optional<std::string> Checker::initialization_root(const std::string& name) const {
    if (unknown_reference_targets_.contains(name)) return std::nullopt;
    if (const auto ref = reference_paths_.find(name); ref != reference_paths_.end()) {
        if (!ref->second.second.empty()) return std::nullopt;
        return ref->second.first;
    }
    return reference_root(name);
}

void Checker::check_maybe_initialized_read(const Expr& read, const std::string& name) {
    const auto root = initialization_root(name);
    if (!root) {
        if (unknown_reference_targets_.contains(name)) {
            error("UNINITIALIZED",
                  "'" + name + "' may be uninitialized through a reference whose target is not known.",
                  read.span);
        } else {
            error("UNINITIALIZED", "Binding '" + name + "' may be uninitialized.", read.span);
        }
        return;
    }
    if (untracked_initialized_.contains(*root)) {
        // A call may have initialized it, which no flag records.
        error("UNINITIALIZED", "Binding '" + name + "' may be uninitialized.", read.span);
        return;
    }
    if (!maybe_initialized_.contains(*root)) {
        error("UNINITIALIZED", "'" + name + "' is read before it is initialized on every path.",
              read.span);
        return;
    }
    const auto declaration = binding_declarations_.find(*root);
    if (declaration == binding_declarations_.end()) {
        error("UNINITIALIZED", "Binding '" + name + "' may be uninitialized.", read.span);
        return;
    }
    // Checked here at run time; past the check the binding is initialized.
    initialization_checks_[&read] =
        InitializationCheck{InitSubject::binding, declaration->second, name};
    initialization_check_order_.emplace_back(&read, *root);
    initialized_.insert(*root);
    maybe_initialized_.erase(*root);
}

bool Checker::check_maybe_initialized_field(const Expr& read, const std::string& root,
                                            const std::string& path, const std::string& shown) {
    // Only a field of the value itself has a bit the checker can trust: a
    // local declared without an initializer, or the receiver in its
    // constructor, whose field stores are all seen. A field written where
    // no bit records it (through a reference, by a callee) may be
    // initialized untracked; nested fields are never tracked. Those reads
    // stay errors.
    const auto access = field_accesses_.find(&read);
    if (path.empty() || path.find('.') != std::string::npos ||
        untracked_initialized_.contains(root + "." + path) || access == field_accesses_.end() ||
        classes_.at(access->second.owner).fields.size() > 63 ||
        standard_library_class(access->second.owner)) {
        return false;
    }
    if (!maybe_initialized_.contains(root + "." + path)) {
        // The receiver keeps its constructor text.
        if (root == "$this") return false;
        error("UNINITIALIZED", "'" + shown + "' is read before it is initialized on every path.",
              read.span);
        return true;
    }
    initialization_checks_[&read] = InitializationCheck{InitSubject::field, nullptr, shown};
    initialization_masked_classes_.insert(access->second.owner);
    return true;
}

void Checker::note_field_store(const std::string& root, const std::string& path) {
    if (!path.empty() && path.find('.') == std::string::npos)
        maybe_initialized_.insert(root + "." + path);
}

void Checker::note_untracked_field_write(const std::string& root, const std::string& path) {
    if (!path.empty()) {
        untracked_initialized_.insert(root + "." + path.substr(0, path.find_first_of(".[")));
        return;
    }
    const auto variable = variables_.find(root);
    const auto class_name = root == "$this" ? current_class_
                            : variable != variables_.end() && variable->second.kind == TypeKind::Class
                                ? variable->second.class_name
                                : std::string{};
    if (const auto info = classes_.find(class_name); info != classes_.end()) {
        for (const auto& field : info->second.fields) untracked_initialized_.insert(root + "." + field.name);
    }
}

void Checker::note_class_value(const std::string& root, const std::string& path, const Type& type,
                               const std::unordered_set<std::string>& initialized) {
    (void)initialized;
    if (type.kind != TypeKind::Class || !path.empty()) return;
    // A whole value from elsewhere: its bits are not known to match.
    const auto prefix = root + ".";
    for (auto it = maybe_initialized_.begin(); it != maybe_initialized_.end();) {
        if (it->rfind(prefix, 0) == 0) it = maybe_initialized_.erase(it);
        else ++it;
    }
    if (const auto info = classes_.find(type.class_name); info != classes_.end()) {
        for (const auto& field : info->second.fields) untracked_initialized_.insert(prefix + field.name);
    }
}

void Checker::record_initialization(const std::string& root) {
    if (initialized_.contains(root)) return;
    const auto declaration = binding_declarations_.find(root);
    if (declaration == binding_declarations_.end()) return;
    const auto add = [&](std::vector<const Stmt*>& list) {
        if (std::find(list.begin(), list.end(), declaration->second) == list.end())
            list.push_back(declaration->second);
    };
    if (current_simple_statement_) add(statement_initializes_[current_simple_statement_]);
    if (current_call_expression_) add(expression_initializes_[current_call_expression_]);
}

namespace {

// The binding a place's storage belongs to: the name at the base of its
// member and index chain, or null when the chain starts at a value.
const NameExpr* place_root(const Expr& expression) {
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
    const auto* name = std::get_if<NameExpr>(&current->data);
    return name && !name->this_qualifier ? name : nullptr;
}

// The bindings a body may write or initialize, with `unknown` set when a
// write has no place: assignment and rebinding targets, `&` arguments, method
// receivers and the targets of scan formats.
void collect_possible_initializations(const std::vector<StmtPtr>& body,
                                      std::unordered_set<std::string>& names,
                                      bool& unknown) {
    const auto place = [&](const Expr& expression) {
        if (const auto* root = place_root(expression)) {
            names.insert(root->name);
            return;
        }
        const Expr* current = &expression;
        while (const auto* member = std::get_if<MemberExpr>(&current->data)) current = member->base.get();
        while (const auto* index = std::get_if<IndexExpr>(&current->data)) current = index->base.get();
        if (const auto* name = std::get_if<NameExpr>(&current->data); name && name->this_qualifier)
            names.insert("$this");
        else
            unknown = true;
    };
    std::function<void(const Expr&)> expression = [&](const Expr& node) {
        const auto each = [&](const ExprPtr& item) {
            if (item) expression(*item);
        };
        const auto& data = node.data;
        if (const auto* call = std::get_if<CallExpr>(&data)) {
            names.insert("$this");
            for (const auto& argument : call->args) {
                if (argument.writable) place(*argument.value);
                each(argument.value);
                if (call->callee == "scan") {
                    if (const auto* format = std::get_if<StringTemplateExpr>(&argument.value->data)) {
                        for (const auto& target : format->expressions) {
                            if (target) place(*target);
                        }
                    }
                }
            }
        } else if (const auto* call = std::get_if<MethodCallExpr>(&data)) {
            if (const auto* root = place_root(*call->receiver)) names.insert(root->name);
            each(call->receiver);
            for (const auto& argument : call->args) {
                if (argument.writable) place(*argument.value);
                each(argument.value);
            }
        } else if (const auto* node_value = std::get_if<StringTemplateExpr>(&data)) {
            for (const auto& item : node_value->expressions) each(item);
        } else if (const auto* array = std::get_if<ArrayExpr>(&data)) {
            for (const auto& item : array->elements) each(item);
        } else if (const auto* index = std::get_if<IndexExpr>(&data)) {
            each(index->base);
            for (const auto& item : index->items) {
                each(item.index);
                each(item.start);
                each(item.stop);
                each(item.step);
            }
        } else if (const auto* member = std::get_if<MemberExpr>(&data)) {
            each(member->base);
        } else if (const auto* unary = std::get_if<UnaryExpr>(&data)) {
            each(unary->operand);
        } else if (const auto* binary = std::get_if<BinaryExpr>(&data)) {
            each(binary->left);
            each(binary->right);
        } else if (const auto* attempt = std::get_if<TryExpr>(&data)) {
            each(attempt->value);
        } else if (const auto* choice = std::get_if<IfExpr>(&data)) {
            for (const auto& item : choice->conditions) each(item);
            for (const auto& item : choice->values) each(item);
            each(choice->otherwise);
        }
    };
    for (const auto& statement : body) {
        const auto& data = statement->data;
        if (const auto* node = std::get_if<BindingStmt>(&data)) {
            if (node->value) expression(*node->value);
        } else if (const auto* node = std::get_if<AssignStmt>(&data)) {
            place(*node->target);
            expression(*node->target);
            expression(*node->value);
        } else if (const auto* node = std::get_if<RebindStmt>(&data)) {
            names.insert(node->name);
            expression(*node->target);
        } else if (const auto* node = std::get_if<ReturnStmt>(&data)) {
            if (node->value) expression(*node->value);
        } else if (const auto* node = std::get_if<ExprStmt>(&data)) {
            expression(*node->value);
        } else if (const auto* node = std::get_if<IfStmt>(&data)) {
            expression(*node->condition);
            collect_possible_initializations(node->then_body, names, unknown);
            collect_possible_initializations(node->else_body, names, unknown);
        } else if (const auto* node = std::get_if<WhileStmt>(&data)) {
            expression(*node->condition);
            collect_possible_initializations(node->body, names, unknown);
        } else if (const auto* node = std::get_if<ForStmt>(&data)) {
            expression(*node->iterable);
            collect_possible_initializations(node->body, names, unknown);
        } else if (const auto* node = std::get_if<MatchStmt>(&data)) {
            expression(*node->value);
            for (const auto& match_case : node->cases)
                collect_possible_initializations(match_case.body, names, unknown);
        } else if (const auto* node = std::get_if<MainGuardStmt>(&data)) {
            collect_possible_initializations(node->body, names, unknown);
        }
    }
}

// Whether `body` holds a break or continue of the loop it is the body of
// (not of a loop nested in it).
bool block_leaves_loop(const std::vector<StmtPtr>& body) {
    for (const auto& statement : body) {
        const auto& data = statement->data;
        if (std::holds_alternative<LoopControlStmt>(data)) return true;
        if (const auto* node = std::get_if<IfStmt>(&data)) {
            if (block_leaves_loop(node->then_body) || block_leaves_loop(node->else_body)) return true;
        } else if (const auto* node = std::get_if<MatchStmt>(&data)) {
            for (const auto& match_case : node->cases)
                if (block_leaves_loop(match_case.body)) return true;
        } else if (const auto* node = std::get_if<MainGuardStmt>(&data)) {
            if (block_leaves_loop(node->body)) return true;
        }
    }
    return false;
}

// Whether a for loop provably runs its body at least once: over a non-empty
// array literal, a fixed array of length at least one, or a range with
// constant bounds and step that holds at least one value.
bool loop_runs_at_least_once(const ForStmt& loop, const Type& iterable,
                             const std::unordered_map<std::string, long long>* constants) {
    if (const auto* array = std::get_if<ArrayExpr>(&loop.iterable->data))
        return !array->elements.empty();
    if (iterable.kind == TypeKind::Array) return iterable.length >= 1;
    const auto* call = std::get_if<CallExpr>(&loop.iterable->data);
    if (iterable.kind != TypeKind::Range || !call || call->callee != "range") return false;
    std::vector<const Expr*> positional;
    const Expr* named_step = nullptr;
    for (const auto& argument : call->args) {
        if (!argument.name) positional.push_back(argument.value.get());
        else if (*argument.name == "step") named_step = argument.value.get();
        else return false;
    }
    if (positional.empty() || positional.size() > 3 || (named_step && positional.size() == 3))
        return false;
    const auto value = [&](const Expr* expression) {
        return constant_eval::integer(*expression, constants);
    };
    const auto start = positional.size() >= 2 ? value(positional[0]) : std::optional<long long>{0};
    const auto stop = value(positional.size() >= 2 ? positional[1] : positional[0]);
    const auto step = named_step ? value(named_step)
                      : positional.size() == 3 ? value(positional[2])
                                               : std::optional<long long>{1};
    if (!start || !stop || !step || *step == 0) return false;
    return *step > 0 ? *stop > *start : *stop < *start;
}

} // namespace

void Checker::note_loop_initializations(const std::vector<StmtPtr>& body) {
    std::unordered_set<std::string> names;
    bool unknown = false;
    collect_possible_initializations(body, names, unknown);
    std::unordered_set<std::string> roots;
    for (const auto& name : names) {
        if (unknown_reference_targets_.contains(name)) unknown = true;
        if (element_tracked_arrays_.contains(name)) maybe_initialized_.insert(name + "[]");
        roots.insert(name);
        roots.insert(reference_root(name));
        if (const auto ref = reference_paths_.find(name); ref != reference_paths_.end())
            roots.insert(ref->second.first);
    }
    if (unknown) {
        for (const auto& [name, _] : binding_declarations_) roots.insert(name);
        for (const auto& [name, _] : element_tracked_arrays_) maybe_initialized_.insert(name + "[]");
    }
    for (const auto& name : roots) {
        if (!initialized_.contains(name)) maybe_initialized_.insert(name);
        if (name == "$this") {
            if (!current_class_.empty()) {
                for (const auto& path : complete_class_paths(Type::class_type(current_class_)))
                    maybe_initialized_.insert("$this." + path);
            }
        } else if (const auto variable = variables_.find(name);
                   variable != variables_.end() && variable->second.kind == TypeKind::Class) {
            for (const auto& path : complete_class_paths(variable->second))
                maybe_initialized_.insert(name + "." + path);
        }
    }
}

void Checker::reject_untracked_loop_checks(std::size_t first) {
    for (std::size_t i = first; i < initialization_check_order_.size(); ++i) {
        const auto& [read, root] = initialization_check_order_[i];
        if (!untracked_initialized_.contains(root)) continue;
        const auto check = initialization_checks_.find(read);
        if (check == initialization_checks_.end()) continue;
        const auto path = check->second.path;
        initialization_checks_.erase(check);
        try {
            error("UNINITIALIZED", "Binding '" + path + "' may be uninitialized.", read->span);
        } catch (const CompileError& compile_error) {
            record(compile_error);
        }
    }
}

void Checker::declare_array_elements(const std::string& name, const Type& type,
                                     const BindingStmt& node) {
    element_tracked_arrays_.erase(name);
    const auto prefix = name + "[";
    for (auto it = maybe_initialized_.begin(); it != maybe_initialized_.end();) {
        if (it->rfind(prefix, 0) == 0) it = maybe_initialized_.erase(it);
        else ++it;
    }
    if (node.reference || type.kind != TypeKind::Array || !type.first ||
        type.first->kind == TypeKind::Array) {
        return;
    }
    constexpr long long most_tracked = 256;
    const auto count = [&](std::optional<long long> length) {
        return length && *length >= 0 && *length <= most_tracked ? *length : -1;
    };
    if (!node.value) {
        if (type.length >= 0) {
            element_tracked_arrays_[name] = count(type.length);
        } else if (type.length == -2 && !node.declared_type.dimension_expressions.empty() &&
                   node.declared_type.dimension_expressions.front()) {
            element_tracked_arrays_[name] = -1;
        }
        return;
    }
    const auto* call = std::get_if<CallExpr>(&node.value->data);
    if (!call || call->callee != "array" || call->args.size() != 1 || call->args.front().name)
        return;
    const auto resolution = call_resolutions_.find(node.value.get());
    if (resolution == call_resolutions_.end() || resolution->second.kind != CallKind::Builtin)
        return;
    element_tracked_arrays_[name] =
        count(constant_eval::integer(*call->args.front().value, &const_integer_values_));
}

void Checker::note_element_store(const Expr& target) {
    const auto* index = std::get_if<IndexExpr>(&target.data);
    const auto* base = index ? std::get_if<NameExpr>(&index->base->data) : nullptr;
    if (!base || base->this_qualifier) return;
    const auto tracked = element_tracked_arrays_.find(base->name);
    if (tracked == element_tracked_arrays_.end()) return;
    std::optional<long long> position;
    if (index->items.size() == 1 && !index->items.front().slice && index->items.front().index)
        position = constant_eval::integer(*index->items.front().index, &const_integer_values_);
    if (tracked->second >= 0 && position && *position >= 0 && *position < tracked->second) {
        maybe_initialized_.insert(base->name + "[" + std::to_string(*position) + "]");
    } else {
        maybe_initialized_.insert(base->name + "[]");
    }
}

void Checker::note_element_writes(const Expr& place) {
    const auto* root = place_root(place);
    if (root && element_tracked_arrays_.contains(root->name))
        maybe_initialized_.insert(root->name + "[]");
}

void Checker::check_element_read(const Expr& base, const Expr* index, SourceSpan span) {
    const auto* name = std::get_if<NameExpr>(&base.data);
    if (!name || name->this_qualifier) return;
    const auto tracked = element_tracked_arrays_.find(name->name);
    if (tracked == element_tracked_arrays_.end()) return;
    if (maybe_initialized_.contains(name->name + "[]")) return;
    const auto element = [&](long long position) {
        return maybe_initialized_.contains(name->name + "[" + std::to_string(position) + "]");
    };
    const auto reject = [&](const std::string& shown) {
        error("UNINITIALIZED",
              "'" + name->name + "[" + shown + "]' is read before it is initialized on every path.",
              span);
    };
    std::optional<long long> position;
    if (index) position = constant_eval::integer(*index, &const_integer_values_);
    if (tracked->second >= 0 && position) {
        if (*position >= 0 && *position < tracked->second && !element(*position))
            reject(std::to_string(*position));
        return;
    }
    if (tracked->second >= 0 && !index) {
        for (long long i = 0; i < tracked->second; ++i) {
            if (!element(i)) return reject(std::to_string(i));
        }
        return;
    }
    // Another index, or one state for every element: an error only when no
    // element may be initialized.
    const auto prefix = name->name + "[";
    for (const auto& entry : maybe_initialized_) {
        if (entry.rfind(prefix, 0) == 0) return;
    }
    if (tracked->second == 0) return;
    if (index && position) reject(std::to_string(*position));
    else if (const auto* variable = index ? std::get_if<NameExpr>(&index->data) : nullptr)
        reject(variable->name);
    else
        reject(index ? "..." : "0");
}

void Checker::join_maybe_initialized(
    const std::vector<std::pair<const std::unordered_set<std::string>*,
                                const std::unordered_set<std::string>*>>& continuing) {
    if (continuing.empty()) return;
    std::unordered_set<std::string> joined;
    for (const auto& [initialized, maybe] : continuing) {
        joined.insert(initialized->begin(), initialized->end());
        joined.insert(maybe->begin(), maybe->end());
    }
    for (const auto& name : initialized_) joined.erase(name);
    maybe_initialized_ = std::move(joined);
}

void Checker::check_static_index_bounds(const Type& base, const Expr& index) {
    if (base.kind != TypeKind::Array || base.length < 0) return;
    const auto value = constant_eval::integer(index);
    if (!value) return;
    if (*value < 0 || *value >= base.length) {
        error("INDEX_BOUNDS",
              "Index " + std::to_string(*value) + " out of bounds for length " +
                  std::to_string(base.length) + ".",
              index.span);
        return;
    }
    bounds_proven_.insert(&index);
}

Type Checker::check_index_operand(const Expr& index) {
    if (integer_category(numeric_literal_family(index))) {
        const auto int_type = simple(TypeKind::Int);
        return check_expr(index, &int_type);
    }
    const auto type = check_expr(index);
    if (!poisoned(type) && !is_integer_family_type(type)) {
        error("TYPE_MISMATCH", "An index must be an integer, not " + type_name(type) + ".",
              index.span);
    }
    return type;
}

Type Checker::check_address_target(const Expr& expression, bool allow_tensor_element) {
    // Shares expr_depth_ with check_expr: the two interleave, and the budget
    // is about total stack, not about either cycle alone.
    nesting::DepthGuard guard(
        expr_depth_, nesting::max_expression_depth, expression.span, "Address target");
    // Storage given a reference may have any element written (L13); the
    // element an assignment stores into, and its array, are recorded after
    // the value instead.
    const auto* store = element_store_target_
        ? std::get_if<IndexExpr>(&element_store_target_->data) : nullptr;
    if (&expression != element_store_target_ && !(store && &expression == store->base.get()))
        note_element_writes(expression);
    Type type;
    if (is_if_expression(expression)) {
        error("IF_EXPRESSION", if_expression_reference_message, expression.span);
    }
    if (const auto* name = std::get_if<NameExpr>(&expression.data); name && name->this_qualifier) {
        const auto& field = check_this_field(expression, *name);
        type = field.type;
        field_accesses_[&expression] = FieldAccessInfo{field.owner, field.index, field.type};
    } else if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        if (is_discard_name(name->name)) error("DISCARD", discard_read_message, expression.span);
        if (variables_.contains(name->name)) {
            type = variables_.at(name->name);
        } else {
            reject_bare_field(name->name, expression.span);
            error("UNKNOWN_NAME", "Unknown storage name '" + name->name + "'.", expression.span);
        }
    } else if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
        Type base;
        if (current_reference_parameter_path(*member->base)) {
            base = check_address_target(*member->base);
        } else {
            base = check_expr(*member->base);
        }
        if (base.kind != TypeKind::Class) {
            error("TYPE_MISMATCH", "Field address requires a class value.", expression.span);
        }
        const auto* field = find_field(base.class_name, member->name);
        if (!field) error("UNKNOWN_MEMBER", "Unknown class field '" + member->name + "'.", expression.span);
        if (field->is_private && current_class_ != field->owner) {
            error("PRIVATE_MEMBER",
                  "Private field '" + member->name + "' is only accessible inside class '" +
                      field->owner + "'.",
                  expression.span);
        }
        type = field->type;
        field_accesses_[&expression] = FieldAccessInfo{field->owner, field->index, field->type};
    } else if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
        auto base = stable_writable_storage(*index->base)
            ? check_address_target(*index->base)
            : check_expr(*index->base);
        if (base.kind == TypeKind::Tensor) {
            if (!allow_tensor_element) {
                error("WRITE_CAPABILITY",
                      "Tensor elements do not expose writable references; assign through a stored tensor value instead.",
                      expression.span);
            }
            if (std::holds_alternative<IndexExpr>(index->base->data)) {
                error("WRITE_CAPABILITY",
                      "Assignment through a temporary tensor view is not allowed; bind the view first.",
                      expression.span);
            }
            if (index->items.empty()) {
                error("INDEX_ARITY", "Tensor index list cannot be empty.", expression.span);
            }
            if (base.length >= 0 &&
                index->items.size() != static_cast<std::size_t>(base.length)) {
                error("INDEX_ARITY",
                      "Writable tensor element assignment requires one integer index per static rank dimension.",
                      expression.span);
            }
            for (const auto& item : index->items) {
                if (item.slice || !item.index) {
                    error("WRITE_CAPABILITY",
                          "Tensor slice assignment is not supported; assign individual elements.",
                          item.span);
                }
                check_index_operand(*item.index);
            }
            type = *base.first;
        } else {
            if (index->items.size() != 1 || index->items.front().slice ||
                !index->items.front().index) {
                error("INDEX_ARITY", "Array and bin indexing requires exactly one integer index.",
                      expression.span);
            }
            check_index_operand(*index->items.front().index);
            check_static_index_bounds(base, *index->items.front().index);
            if (base.kind == TypeKind::Array) {
                type = *base.first;
            } else if (base.kind == TypeKind::Bin) {
                error("WRITE_CAPABILITY",
                      "bin elements do not expose addressable references; assign through bin[index].",
                      expression.span);
                type = simple(TypeKind::Bin);
            } else {
                error("TYPE_MISMATCH",
                      "Element address requires an array or tensor.", expression.span);
            }
        }
    } else {
        error("REFERENCE_BINDING", "Address requires a binding, field, or array element.", expression.span);
    }
    raw_types_[&expression] = expr_types_[&expression] = type;
    return type;
}

void Checker::mark_member_initialized(const Expr& expression) {
    const auto insert_prefixes = [](std::unordered_set<std::string>& fields,
                                    const std::string& path) {
        if (path.empty()) {
            fields.insert("");
            return;
        }
        std::string prefix;
        std::size_t start = 0;
        while (start < path.size()) {
            const auto dot = path.find('.', start);
            if (!prefix.empty()) prefix += ".";
            prefix += path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            fields.insert(prefix);
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
    };

    if (const auto path = current_receiver_path(expression)) {
        insert_prefixes(current_receiver_effect_.initializes, *path);
        return;
    }
    if (const auto path = current_reference_parameter_path(expression)) {
        insert_prefixes(current_reference_effects_[path->first].initializes, path->second);
        return;
    }
    if (const auto path = member_storage_path(expression); path && !path->second.empty()) {
        insert_prefixes(class_initialized_paths_[path->first], path->second);
    }
}

void Checker::record_storage_assignment(StorageEffect& effect, const std::string& path,
                                        const Type& type, const Expr& value) {
    const auto qualify = [&](const std::string& relative) {
        if (path.empty()) return relative;
        if (relative.empty()) return path;
        return path + "." + relative;
    };
    const auto erase_path = [](std::unordered_set<std::string>& paths,
                               const std::string& target) {
        paths.erase(target);
        if (target.empty()) {
            paths.clear();
            return;
        }
        const auto prefix = target + ".";
        for (auto it = paths.begin(); it != paths.end();) {
            if (it->rfind(prefix, 0) == 0) it = paths.erase(it);
            else ++it;
        }
    };
    const auto insert_prefixes = [](std::unordered_set<std::string>& paths,
                                    const std::string& target) {
        if (target.empty()) {
            paths.insert("");
            return;
        }
        std::string current;
        std::size_t start = 0;
        while (start < target.size()) {
            const auto dot = target.find('.', start);
            if (!current.empty()) current += ".";
            current += target.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            paths.insert(current);
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
    };

    effect.writes.insert(path);
    effect.invalidates.erase(path);
    erase_path(effect.initializes, path);
    insert_prefixes(effect.initializes, path);

    if (type.kind != TypeKind::Class) return;

    const auto initialized = initialized_paths_for_expr(value);
    for (const auto& relative : complete_class_paths(type)) {
        const auto full = qualify(relative);
        if (initialized.contains(relative)) {
            effect.invalidates.erase(full);
            insert_prefixes(effect.initializes, full);
        } else {
            effect.invalidates.insert(full);
            erase_path(effect.initializes, full);
        }
    }
}

void Checker::record_current_receiver_assignment(const std::string& path,
                                                 const Type& type,
                                                 const Expr& value) {
    record_storage_assignment(current_receiver_effect_, path, type, value);
}

namespace {

// The elements of an initialized array of class values are whole values (a
// class value enters an array only fully initialized), so a path below an
// element step (`items[].storage`) is initialized when the array path before
// the step (`items`) is: requirements name that array path.
std::string element_owner_path(const std::string& path) {
    const auto element = path.find("[]");
    return element == std::string::npos ? path : path.substr(0, element);
}

} // namespace

void Checker::compose_storage_effect(StorageEffect& destination, const StorageEffect& source,
                                     const std::string& prefix) {
    const auto qualify = [&](const std::string& path) {
        if (prefix.empty()) return path;
        if (path.empty()) return prefix;
        return prefix + "." + path;
    };
    const auto erase_path = [](std::unordered_set<std::string>& paths,
                               const std::string& target) {
        paths.erase(target);
        if (target.empty()) {
            paths.clear();
            return;
        }
        const auto child_prefix = target + ".";
        for (auto it = paths.begin(); it != paths.end();) {
            if (it->rfind(child_prefix, 0) == 0) it = paths.erase(it);
            else ++it;
        }
    };
    const auto insert_prefixes = [](std::unordered_set<std::string>& paths,
                                    const std::string& target) {
        if (target.empty()) {
            paths.insert("");
            return;
        }
        std::string current;
        std::size_t start = 0;
        while (start < target.size()) {
            const auto dot = target.find('.', start);
            if (!current.empty()) current += ".";
            current += target.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            paths.insert(current);
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
    };

    for (const auto& path : source.required) {
        const auto full = element_owner_path(qualify(path));
        if (!destination.initializes.contains(full)) destination.required.insert(full);
    }
    for (const auto& path : source.writes) destination.writes.insert(qualify(path));
    for (const auto& path : source.invalidates) {
        const auto full = qualify(path);
        destination.invalidates.insert(full);
        erase_path(destination.initializes, full);
    }
    for (const auto& path : source.initializes) {
        const auto full = qualify(path);
        destination.invalidates.erase(full);
        insert_prefixes(destination.initializes, full);
    }
}

void Checker::apply_current_method_summary(const FunctionType& signature,
                                           const std::string& prefix) {
    compose_storage_effect(current_receiver_effect_, signature.receiver_effect, prefix);
}

void Checker::apply_method_effects(const Expr& receiver, const FunctionType& signature,
                                   SourceSpan span) {
    apply_storage_effect_to_target(receiver, signature.receiver_effect, span, true);
}

void Checker::mark_storage_initialized(const Expr& expression) {
    if (const auto path = current_reference_parameter_path(expression)) {
        const auto insert_prefixes = [](std::unordered_set<std::string>& paths,
                                        const std::string& target) {
            if (target.empty()) {
                paths.insert("");
                return;
            }
            std::string current;
            std::size_t start = 0;
            while (start < target.size()) {
                const auto dot = target.find('.', start);
                if (!current.empty()) current += ".";
                current += target.substr(
                    start, dot == std::string::npos ? std::string::npos : dot - start);
                paths.insert(current);
                if (dot == std::string::npos) break;
                start = dot + 1;
            }
        };
        insert_prefixes(current_reference_effects_[path->first].initializes, path->second);
        return;
    }
    if (const auto* name = binding_name(expression)) {
        if (const auto ref = reference_paths_.find(name->name); ref != reference_paths_.end()) {
            if (ref->second.second.empty()) {
                if (!unknown_reference_targets_.contains(name->name)) {
                    record_initialization(ref->second.first);
                } else {
                    // The write may reach any binding.
                    for (const auto& [binding, _] : binding_declarations_) {
                        if (!initialized_.contains(binding)) untracked_initialized_.insert(binding);
                    }
                }
                initialized_.insert(ref->second.first);
            } else {
                class_initialized_paths_[ref->second.first].insert(ref->second.second);
            }
            initialized_.insert(name->name);
            return;
        }
        if (variables_.contains(name->name)) {
            record_initialization(reference_root(name->name));
            initialized_.insert(reference_root(name->name));
            initialized_.insert(name->name);
            return;
        }
    }
    if (std::holds_alternative<MemberExpr>(expression.data)) {
        mark_member_initialized(expression);
    }
}

void Checker::check_storage_effect_requirements(
    const Expr& target, const StorageEffect& effect, SourceSpan span, bool receiver_context) {
    StorageEffect requirements;
    requirements.required = effect.required;

    if (const auto receiver_path = current_receiver_path(target)) {
        compose_storage_effect(current_receiver_effect_, requirements, *receiver_path);
        return;
    }
    if (const auto reference_path = current_reference_parameter_path(target)) {
        compose_storage_effect(
            current_reference_effects_[reference_path->first], requirements, reference_path->second);
        return;
    }

    const auto initialized_paths = initialized_paths_for_expr(target);
    for (const auto& path : effect.required) {
        const bool initialized =
            path.empty() ? storage_initialized(target) : initialized_paths.contains(path);
        if (!initialized && path.empty()) {
            // The callee reads the whole binding: a read of it (L13).
            if (const auto* name = std::get_if<NameExpr>(&target.data);
                name && !name->this_qualifier && variables_.contains(name->name) &&
                !current_reference_parameters_.contains(name->name)) {
                check_maybe_initialized_read(target, name->name);
                continue;
            }
        }
        if (!initialized) {
            const std::string subject = receiver_context ? "Method receiver" : "Reference argument";
            error("UNINITIALIZED",
                  path.empty() ? subject + " requires initialized storage."
                               : subject + " requires initialized field '" + path + "'.",
                  span);
        }
    }
}

void Checker::apply_storage_effect_postconditions(
    const Expr& target, const StorageEffect& effect, bool allow_initializes) {
    StorageEffect postconditions = effect;
    postconditions.required.clear();
    if (!allow_initializes) postconditions.initializes.clear();

    if (const auto receiver_path = current_receiver_path(target)) {
        compose_storage_effect(current_receiver_effect_, postconditions, *receiver_path);
        for (const auto* paths : {&effect.writes, &effect.initializes, &effect.invalidates}) {
            for (const auto& path : *paths)
                note_untracked_field_write("$this", path.empty() ? *receiver_path : *receiver_path + "." + path);
        }
        return;
    }
    if (const auto reference_path = current_reference_parameter_path(target)) {
        compose_storage_effect(
            current_reference_effects_[reference_path->first], postconditions, reference_path->second);
        return;
    }

    const auto storage = member_storage_path(target);
    const auto erase_path = [&](const std::string& relative) {
        if (!storage) return;
        const auto qualify = [&](const std::string& path) {
            if (storage->second.empty()) return path;
            if (path.empty()) return storage->second;
            return storage->second + "." + path;
        };
        const auto full = qualify(relative);
        if (full.empty()) {
            initialized_.erase(storage->first);
            class_initialized_paths_.erase(storage->first);
            return;
        }
        auto& fields = class_initialized_paths_[storage->first];
        fields.erase(full);
        const auto prefix = full + ".";
        for (auto it = fields.begin(); it != fields.end();) {
            if (it->rfind(prefix, 0) == 0) it = fields.erase(it);
            else ++it;
        }
    };
    const auto insert_prefixes = [&](const std::string& relative) {
        if (relative.empty()) {
            mark_storage_initialized(target);
            return;
        }
        if (!storage) return;
        auto& fields = class_initialized_paths_[storage->first];
        std::string current = storage->second;
        std::size_t start = 0;
        while (start < relative.size()) {
            const auto dot = relative.find('.', start);
            if (!current.empty()) current += ".";
            current += relative.substr(
                start, dot == std::string::npos ? std::string::npos : dot - start);
            fields.insert(current);
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        record_initialization(storage->first);
        initialized_.insert(storage->first);
    };

    for (const auto& path : postconditions.invalidates) erase_path(path);
    for (const auto& path : postconditions.initializes) insert_prefixes(path);
    // A callee that may write the storage without initializing it on every
    // path leaves a binding no flag can track (L13), and fields no bit records.
    if (storage && !initialized_.contains(storage->first) &&
        (!effect.writes.empty() || !effect.initializes.empty() || !effect.invalidates.empty())) {
        untracked_initialized_.insert(storage->first);
    }
    if (storage) {
        const auto qualify = [&](const std::string& path) {
            if (storage->second.empty()) return path;
            if (path.empty()) return storage->second;
            return storage->second + "." + path;
        };
        for (const auto* paths : {&effect.writes, &effect.initializes, &effect.invalidates}) {
            for (const auto& path : *paths) note_untracked_field_write(storage->first, qualify(path));
        }
    }
}

void Checker::apply_storage_effect_to_target(const Expr& target, const StorageEffect& effect,
                                             SourceSpan span, bool receiver_context) {
    check_storage_effect_requirements(target, effect, span, receiver_context);
    apply_storage_effect_postconditions(target, effect);
}

void Checker::check_reference_effect_requirements(
    const Expr& target, const FunctionType& signature,
    const FunctionParameterType& parameter, SourceSpan span) {
    const auto effect = signature.reference_effects.find(parameter.name);
    if (effect == signature.reference_effects.end()) {
        if (parameter.is_const && !storage_initialized(target)) {
            error("UNINITIALIZED",
                  "Readonly reference arguments require initialized storage.", span);
        }
        return;
    }
    check_storage_effect_requirements(target, effect->second, span);
}

void Checker::apply_reference_effect_postconditions(
    const Expr& target, const FunctionType& signature,
    const FunctionParameterType& parameter, SourceSpan,
    bool allow_initializes) {
    const auto effect = signature.reference_effects.find(parameter.name);
    if (effect == signature.reference_effects.end()) return;
    apply_storage_effect_postconditions(target, effect->second, allow_initializes);
}

void Checker::finish_call_effects(
    const FunctionType& signature,
    const std::vector<PendingReferenceEffect>& pending,
    const Expr* receiver,
    bool current_receiver,
    SourceSpan receiver_span) {
    std::optional<std::pair<std::string, std::string>> receiver_alias;
    if (current_receiver) {
        StorageEffect requirements;
        requirements.required = signature.receiver_effect.required;
        compose_storage_effect(current_receiver_effect_, requirements);
        receiver_alias = std::pair<std::string, std::string>{"$receiver", ""};
    } else if (receiver) {
        check_storage_effect_requirements(
            *receiver, signature.receiver_effect, receiver_span, true);
        receiver_alias = alias_storage_path(*receiver);
    }

    // Preconditions are evaluated against the state at call entry. No parameter's
    // postcondition may initialize storage early enough to satisfy another aliased
    // parameter's read-before-write requirement, including receiver/argument aliases.
    for (const auto& item : pending) {
        check_reference_effect_requirements(
            *item.target, signature, *item.parameter, item.span);
    }

    for (std::size_t i = 0; i < pending.size(); ++i) {
        bool allow_initializes = true;
        const auto path = alias_storage_path(*pending[i].target);
        if (path) {
            if (receiver_alias && storage_paths_overlap(*path, *receiver_alias) &&
                !signature.receiver_effect.invalidates.empty()) {
                allow_initializes = false;
            }
            for (std::size_t j = 0; allow_initializes && j < pending.size(); ++j) {
                if (i == j) continue;
                const auto other_path = alias_storage_path(*pending[j].target);
                if (!other_path || !storage_paths_overlap(*path, *other_path)) continue;
                const auto effect = signature.reference_effects.find(pending[j].parameter->name);
                if (effect != signature.reference_effects.end() &&
                    !effect->second.invalidates.empty()) {
                    // Independent parameter summaries do not encode cross-alias write
                    // ordering. If an overlapping alias may invalidate storage, do not
                    // claim another parameter's initialization as a post-call guarantee.
                    allow_initializes = false;
                }
            }
        }
        apply_reference_effect_postconditions(
            *pending[i].target, signature, *pending[i].parameter,
            pending[i].span, allow_initializes);
    }

    if (current_receiver || receiver) {
        bool allow_receiver_initializes = true;
        if (receiver_alias) {
            for (const auto& item : pending) {
                const auto path = alias_storage_path(*item.target);
                if (!path || !storage_paths_overlap(*receiver_alias, *path)) continue;
                const auto effect = signature.reference_effects.find(item.parameter->name);
                if (effect != signature.reference_effects.end() &&
                    !effect->second.invalidates.empty()) {
                    allow_receiver_initializes = false;
                    break;
                }
            }
        }

        if (current_receiver) {
            StorageEffect postconditions = signature.receiver_effect;
            postconditions.required.clear();
            if (!allow_receiver_initializes) postconditions.initializes.clear();
            compose_storage_effect(current_receiver_effect_, postconditions);
            // A method called on the receiver writes its fields untracked.
            for (const auto* paths : {&signature.receiver_effect.writes,
                                      &signature.receiver_effect.initializes,
                                      &signature.receiver_effect.invalidates}) {
                for (const auto& path : *paths) note_untracked_field_write("$this", path);
            }
        } else {
            apply_storage_effect_postconditions(
                *receiver, signature.receiver_effect, allow_receiver_initializes);
        }
    }
}

void Checker::record_effect_exit() {
    const auto intersect = [](std::unordered_set<std::string>& destination,
                              const std::unordered_set<std::string>& source) {
        for (auto it = destination.begin(); it != destination.end();) {
            if (!source.contains(*it)) it = destination.erase(it);
            else ++it;
        }
    };

    if (!current_effect_exit_summary_seen_) {
        current_exit_receiver_initialized_ = current_receiver_effect_.initializes;
        current_exit_reference_initialized_.clear();
        for (const auto& name : current_reference_parameters_) {
            const auto effect = current_reference_effects_.find(name);
            current_exit_reference_initialized_[name] =
                effect == current_reference_effects_.end()
                    ? std::unordered_set<std::string>{}
                    : effect->second.initializes;
        }
        current_effect_exit_summary_seen_ = true;
        return;
    }

    intersect(current_exit_receiver_initialized_, current_receiver_effect_.initializes);
    for (const auto& name : current_reference_parameters_) {
        const auto effect = current_reference_effects_.find(name);
        const auto& initialized =
            effect == current_reference_effects_.end()
                ? std::unordered_set<std::string>{}
                : effect->second.initializes;
        intersect(current_exit_reference_initialized_[name], initialized);
    }
}

void Checker::finalize_receiver_effects(FunctionType& signature, bool include_fallthrough) {
    signature.receiver_effect = current_receiver_effect_;
    std::unordered_set<std::string> guaranteed;
    bool seen = false;
    if (current_effect_exit_summary_seen_) {
        guaranteed = current_exit_receiver_initialized_;
        seen = true;
    }
    if (include_fallthrough) {
        if (!seen) {
            guaranteed = current_receiver_effect_.initializes;
            seen = true;
        } else {
            for (auto it = guaranteed.begin(); it != guaranteed.end();) {
                if (!current_receiver_effect_.initializes.contains(*it)) {
                    it = guaranteed.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
    signature.receiver_effect.initializes =
        seen ? std::move(guaranteed) : std::unordered_set<std::string>{};
}

void Checker::finalize_reference_effects(FunctionType& signature, bool include_fallthrough) {
    signature.reference_effects = current_reference_effects_;
    std::unordered_set<std::string> names;
    for (const auto& [name, _] : current_reference_effects_) names.insert(name);
    for (const auto& [name, _] : current_exit_reference_initialized_) names.insert(name);

    for (const auto& name : names) {
        std::unordered_set<std::string> guaranteed;
        bool seen = false;
        if (current_effect_exit_summary_seen_) {
            if (const auto exit = current_exit_reference_initialized_.find(name);
                exit != current_exit_reference_initialized_.end()) {
                guaranteed = exit->second;
            }
            seen = true;
        }
        if (include_fallthrough) {
            const auto current = current_reference_effects_.find(name);
            const auto& initialized =
                current == current_reference_effects_.end()
                    ? std::unordered_set<std::string>{}
                    : current->second.initializes;
            if (!seen) {
                guaranteed = initialized;
                seen = true;
            } else {
                for (auto it = guaranteed.begin(); it != guaranteed.end();) {
                    if (!initialized.contains(*it)) it = guaranteed.erase(it);
                    else ++it;
                }
            }
        }
        if (seen || signature.reference_effects.contains(name)) {
            signature.reference_effects[name].initializes =
                seen ? std::move(guaranteed) : std::unordered_set<std::string>{};
        }
    }
}

void Checker::reset_current_effect_state() {
    maybe_initialized_.clear();
    element_tracked_arrays_.clear();
    untracked_initialized_.clear();
    binding_declarations_.clear();
    current_receiver_effect_ = {};
    current_return_initialized_.clear();
    current_return_summary_seen_ = false;
    current_reference_parameters_.clear();
    current_reference_effects_.clear();
    current_exit_receiver_initialized_.clear();
    current_exit_reference_initialized_.clear();
    current_effect_exit_summary_seen_ = false;
}

Type Checker::resolve_type(const TypeName& source, bool auto_ok) {
    if (source.name != "union" && source.name != "tensor" &&
        source.name != "fn" && !source.arguments.empty()) {
        throw std::logic_error("ConcreteProgram contains unresolved generic type arguments.");
    }
    Type type;
    if (source.name == "union") {
        std::vector<Type> cases;
        for (const auto& argument : source.arguments) {
            auto current = resolve_type(argument);
            if (current.kind == TypeKind::Auto || current.kind == TypeKind::Never) {
                error("INVALID_TYPE", "Union cases cannot be auto or never.", argument.span);
            }
            cases.push_back(current);
        }
        type = Type::union_of(std::move(cases));
    } else if (source.name == "tensor") {
        if (source.arguments.size() != 1) {
            error("GENERIC_ARITY", "tensor requires exactly one element type.", source.span);
        }
        auto element = resolve_type(source.arguments.front());
        if (is_bare_integer(element)) {
            error("INVALID_TYPE", "Tensor element types are fixed-width: use tensor<int64>.",
                  source.arguments.front().span);
        } else if (!is_tensor_numeric(element) && element.kind != TypeKind::Bool) {
            error("INVALID_TYPE", "tensor element type must be a fixed-width native numeric type or bool.", source.arguments.front().span);
        }
        const auto rank = !source.tensor_shape_prefix.empty()
            ? static_cast<long long>(source.tensor_shape_prefix.size())
            : source.tensor_rank.value_or(-1);
        type = Type::tensor(element, rank, source.tensor_shape_prefix,
                            source.tensor_known_shape_prefix);
    } else if (source.name == "fn") {
        if (source.arguments.size() != 1) {
            error("GENERIC_ARITY", "fn requires exactly one result type.", source.span);
        }
        auto result = resolve_type(source.arguments.front());
        if (result.kind == TypeKind::Auto || result.kind == TypeKind::Range ||
            result.kind == TypeKind::Invalid) {
            error("INVALID_TYPE", "fn result type must be explicit.", source.arguments.front().span);
        }
        std::vector<Type> parameters;
        parameters.reserve(source.function_parameters.size());
        for (const auto& parameter : source.function_parameters) {
            auto current = resolve_type(parameter);
            if (!is_storable(current)) {
                error("INVALID_TYPE", "fn parameter types must be storable values.", parameter.span);
            }
            parameters.push_back(std::move(current));
        }
        type = Type::function(std::move(result), std::move(parameters));
    } else if (const auto builtin = builtin_scalar_type(source.name)) {
        type = *builtin;
    } else if (source.name == "void") {
        type = simple(TypeKind::Void);
    } else if (source.name == "none") {
        type = simple(TypeKind::None);
    } else if (source.name == "never") {
        error("INVALID_TYPE",
              "'never' is compiler-internal control-flow state, not a source type.",
              source.span);
    } else if (source.name == "error") {
        type = simple(TypeKind::Error);
    } else if (source.name == "auto") {
        if (!auto_ok || source.array_depth) {
            error("INVALID_AUTO", "auto is only a complete initialized local type.", source.span);
        }
        return simple(TypeKind::Auto);
    } else if (enum_types_.contains(source.name)) {
        type = enum_types_.at(source.name);
    } else if (class_names_.contains(source.name)) {
        type = Type::class_type(source.name);
    } else {
        error("UNKNOWN_TYPE", "Unknown type '" + source.name + "'.", source.span);
    }

    for (auto it = source.dimensions.rbegin(); it != source.dimensions.rend(); ++it) {
        if (!is_storable(type)) {
            error("INVALID_TYPE", "Array elements must be storable.", source.span);
        }
        type = Type::array(type, *it);
    }
    return type;
}


void Checker::check_type_extent_expressions(const TypeName& source) {
    const auto check_extent = [&](const std::shared_ptr<Expr>& expression) {
        if (!expression) return;
        // An extent is an integer of any kind; a literal materializes as int.
        const auto int_type = simple(TypeKind::Int);
        const auto type = numeric_literal_family(*expression) != NumericLiteralCategory::None
            ? check_expr(*expression, &int_type) : check_expr(*expression);
        if (!poisoned(type) && !is_integer_family_type(type)) {
            error("INVALID_TYPE",
                  "Array/tensor extents require integer expressions.",
                  expression->span);
        }
        if (const auto known =
                constant_eval::integer(*expression, &const_integer_values_);
            known && *known < 0) {
            error("INVALID_TYPE",
                  "Array/tensor extents cannot be negative.",
                  expression->span);
        }
    };

    for (const auto& expression : source.dimension_expressions) {
        check_extent(expression);
    }
    for (const auto& expression : source.tensor_shape_expressions) {
        check_extent(expression);
    }
    for (const auto& argument : source.arguments) {
        check_type_extent_expressions(argument);
    }
    for (const auto& parameter : source.function_parameters) {
        check_type_extent_expressions(parameter);
    }
}


namespace {

std::string unknown_name_message(const std::string& name) {
    std::string message = "Unknown name '" + name + "'.";
    const auto renamed = renamed_text_constant(name);
    if (!renamed.empty()) {
        message += " The built-in text constant is spelled '" + std::string(renamed) + "'.";
    }
    return message;
}

} // namespace

Type Checker::check_name_expr(const Expr& expression, const NameExpr& node_value,
                              const Type* expected) {
    Type type = simple(TypeKind::Void);
    const auto* node = &node_value;

        if (!node->this_qualifier && is_discard_name(node->name)) {
            error("DISCARD", discard_read_message, expression.span);
        }
        if (node->this_qualifier) {
            const auto& field = check_this_field(expression, *node);
            field_accesses_[&expression] = FieldAccessInfo{field.owner, field.index, field.type};
            if (!current_receiver_effect_.initializes.contains(node->name)) {
                if (in_constructor_ &&
                    check_maybe_initialized_field(expression, "$this", node->name,
                                                  "this." + node->name)) {
                    current_receiver_effect_.initializes.insert(node->name);
                } else if (in_constructor_) {
                    error("UNINITIALIZED",
                          "Field '" + node->name + "' is read before the constructor initializes it.",
                          expression.span);
                } else {
                    current_receiver_effect_.required.insert(node->name);
                }
            }
            type = field.type;
        } else if (is_builtin_text_constant(node->name)) {
            type = simple(TypeKind::String);
        } else if (variables_.contains(node->name)) {
            if (current_reference_parameters_.contains(node->name)) {
                auto& effect = current_reference_effects_[node->name];
                if (!effect.initializes.contains("")) effect.required.insert("");
            } else if (!storage_initialized(expression)) {
                check_maybe_initialized_read(expression, node->name);
            }
            type = variables_.at(node->name);
            if (type.kind == TypeKind::Class) {
                const auto paths = initialized_paths_for_expr(expression);
                class_expr_initialized_paths_[&expression] = paths;
            }
        } else if (const auto function_name = visible_function_name(node->name)) {
            if (!expected || expected->kind != TypeKind::Function || !expected->first) {
                error("FUNCTION_REFERENCE_CONTEXT",
                      "Function '" + node->name +
                      "' becomes a value only in an explicit fn<...>(...) type context.",
                      expression.span);
            }
            const auto& function = functions_.at(*function_name);
            if (function.external) {
                error("FUNCTION_REFERENCE_EXTERN",
                      "extern functions are not function values; wrap the foreign call in a Quidra function.",
                      expression.span);
            }
            if (std::any_of(function.parameters.begin(), function.parameters.end(),
                            [](const auto& parameter) { return parameter.writable; })) {
                error("FUNCTION_REFERENCE_SIGNATURE",
                      "Function values currently require value parameters; reference parameters are not representable in fn signatures.",
                      expression.span);
            }
            if (*expected->first != function.result ||
                expected->parameters.size() != function.parameters.size()) {
                error("FUNCTION_REFERENCE_SIGNATURE",
                      "Function '" + node->name + "' does not match expected " +
                      type_name(*expected) + ".",
                      expression.span);
            }
            for (std::size_t i = 0; i < expected->parameters.size(); ++i) {
                if (expected->parameters[i] != function.parameters[i].type) {
                    error("FUNCTION_REFERENCE_SIGNATURE",
                          "Function '" + node->name + "' does not match expected " +
                          type_name(*expected) + ".",
                          expression.span);
                }
            }
            type = *expected;
            function_references_[&expression] = *function_name;
        } else if (!current_class_.empty()) {
            if (find_method(current_class_, node->name)) {
                error("FUNCTION_NOT_VALUE",
                      "Method '" + node->name + "' is callable but is not a first-class value.",
                      expression.span);
            }
            reject_bare_field(node->name, expression.span);
            error("UNKNOWN_NAME", unknown_name_message(node->name), expression.span);
        } else {
            error("UNKNOWN_NAME", unknown_name_message(node->name), expression.span);
        }
    
    return type;
}

Type Checker::check_member_expr(const Expr& expression, const MemberExpr& node_value) {
    Type type = simple(TypeKind::Void);
    const auto* node = &node_value;

        if (const auto* enum_name = binding_name(*node->base);
            enum_name && enum_types_.contains(enum_name->name)) {
            const auto enum_type = enum_types_.at(enum_name->name);
            const auto it = std::find(enum_type.case_names.begin(), enum_type.case_names.end(), node->name);
            if (it == enum_type.case_names.end())
                error("UNKNOWN_MEMBER", "Enum '" + enum_name->name + "' has no variant '" + node->name + "'.", expression.span);
            const auto tag = static_cast<int>(it - enum_type.case_names.begin());
            const auto payload = enum_type.cases[static_cast<std::size_t>(tag)];
            if (payload.kind != TypeKind::Void)
                error("ARGUMENT_MISMATCH", "Enum variant '" + enum_name->name + "." + node->name + "' requires one payload value.", expression.span);
            enum_constructions_[&expression] = EnumConstructionInfo{enum_type, tag, payload};
            return enum_type;
        }

        const auto reference_base = current_reference_parameter_path(*node->base);
        Type base;
        if (reference_base) {
            base = check_address_target(*node->base);
        } else {
            base = check_expr(*node->base);
        }
        if (poisoned(base)) {
            type = base;
        } else if (base.kind == TypeKind::Tensor && node->name == "grad") {
            if (!base.first ||
                (base.first->kind != TypeKind::Real32 && base.first->kind != TypeKind::Real64)) {
                error("TYPE_MISMATCH",
                      "tensor.grad is available only on tensor<real32> and tensor<real64>.",
                      expression.span);
            }
            tensor_grad_accesses_.insert(&expression);
            type = base;
        } else {
            if (base.kind != TypeKind::Class) {
                error("TYPE_MISMATCH", "Member access requires a class value.", expression.span);
            }
            const bool standard = standard_library_class(base.class_name);
            const auto standard_map = standard_class::is_map_instance(base.class_name, standard);
            const auto standard_set = standard_class::is_set_instance(base.class_name, standard);
            if ((standard_map || standard_set) && node->name.rfind("__", 0) == 0) {
                error("UNKNOWN_MEMBER", "Standard collection internals are not source-visible.", expression.span);
            }
            const auto* field = find_field(base.class_name, node->name);
            if (!field) {
                error("UNKNOWN_MEMBER", "Class '" + base.class_name + "' has no field '" + node->name + "'.", expression.span);
            }
            if (field->is_private && current_class_ != field->owner) {
                error("PRIVATE_MEMBER",
                      "Private field '" + node->name + "' is only accessible inside class '" +
                          field->owner + "'.",
                      expression.span);
            }
            if (const auto receiver_base = current_receiver_path(*node->base)) {
                const auto full = element_owner_path(*receiver_base + "." + node->name);
                if (!current_receiver_effect_.initializes.contains(full)) {
                    if (in_constructor_) {
                        error("UNINITIALIZED",
                              "Field '" + full + "' is read before the constructor initializes it.",
                              expression.span);
                    }
                    current_receiver_effect_.required.insert(full);
                }
            } else if (reference_base) {
                auto full = reference_base->second;
                if (!full.empty()) full += ".";
                full += node->name;
                full = element_owner_path(full);
                auto& effect = current_reference_effects_[reference_base->first];
                if (!effect.initializes.contains(full)) effect.required.insert(full);
            } else {
                const auto paths = initialized_paths_for_expr(*node->base);
                if (!paths.contains(node->name)) {
                    // L13: a field of a local value initialized on some
                    // paths only is checked at run time.
                    field_accesses_[&expression] = FieldAccessInfo{field->owner, field->index, field->type};
                    const auto storage = member_storage_path(expression);
                    if (storage && !unknown_reference_access_path(expression) &&
                        std::holds_alternative<NameExpr>(node->base->data) &&
                        check_maybe_initialized_field(
                            expression, storage->first, storage->second,
                            std::get<NameExpr>(node->base->data).name + "." + node->name)) {
                        class_initialized_paths_[storage->first].insert(storage->second);
                    } else {
                        error("UNINITIALIZED", "Field '" + node->name + "' may be uninitialized.",
                              expression.span);
                    }
                }
            }
            type = field->type;
            field_accesses_[&expression] = FieldAccessInfo{field->owner, field->index, field->type};
            if (type.kind == TypeKind::Class) {
                class_expr_initialized_paths_[&expression] = initialized_paths_for_expr(expression);
            }
        }
    
    return type;
}

Type Checker::check_index_expr(const Expr& expression, const IndexExpr& node_value) {
    Type type = simple(TypeKind::Void);
    const auto* node = &node_value;

    auto base = check_expr(*node->base);
    if (poisoned(base)) return base;

    // The parser preserves exclusive-start/end markers. Reject them until
    // the corresponding typed slice IR and runtime lowering are wired up;
    // ignoring them would silently select the wrong elements, particularly
    // for descending slices and excluded starts.
    for (const auto& item : node->items) {
        if (!item.start_marker && !item.end_marker) continue;
        if (item.start_marker && item.end_marker &&
            item.start_marker != item.end_marker) {
            error("SLICE_STEP", "Slice boundary markers must agree on direction.", item.span);
        }
        // Diagnose statically impossible directions before the backend gate.
        // Dynamic steps will be checked by the shared slice planner.
        if (item.step) {
            if (const auto step = constant_eval::integer(*item.step)) {
                if (*step == 0) {
                    error("SLICE_STEP", "Slice step cannot be zero.", item.step->span);
                }
                const char direction = item.start_marker ? item.start_marker : item.end_marker;
                if ((direction == '<' && *step < 0) ||
                    (direction == '>' && *step > 0)) {
                    error("SLICE_STEP",
                          "Slice boundary direction conflicts with the step sign.",
                          item.step->span);
                }
            }
        }
        error("SLICE_MARKER",
              "Exclusive slice boundary markers require the directional slice backend.",
              item.span);
    }

    if (base.kind == TypeKind::Tensor) {
        if (node->items.empty()) {
            error("INDEX_ARITY", "Tensor index list cannot be empty.", expression.span);
        }
        if (base.length >= 0 &&
            node->items.size() > static_cast<std::size_t>(base.length)) {
            error("INDEX_ARITY", "Tensor index list exceeds the statically known rank.", expression.span);
        }
        long long result_rank = base.length;
        for (const auto& item : node->items) {
            if (!item.slice) {
                if (!item.index) error("INDEX_SYNTAX", "Tensor index is missing.", item.span);
                check_index_operand(*item.index);
                if (result_rank >= 0) --result_rank;
                continue;
            }
            if (item.start) check_index_operand(*item.start);
            if (item.stop) check_index_operand(*item.stop);
            if (item.step) {
                check_index_operand(*item.step);
                if (const auto step = constant_eval::integer(*item.step); step && *step <= 0) {
                    error("SLICE_STEP",
                          "Tensor slices currently require a positive step.",
                          item.step->span);
                }
            }
        }
        auto shape_prefix = base.tensor_shape_prefix;
        auto known_shape_prefix = base.tensor_known_shape_prefix;
        std::size_t axis = 0;
        for (const auto& item : node->items) {
            if (!item.slice) {
                if (axis < shape_prefix.size()) shape_prefix.erase(shape_prefix.begin() + axis);
                if (axis < known_shape_prefix.size())
                    known_shape_prefix.erase(known_shape_prefix.begin() + axis);
                continue;
            }
            const bool full_slice = !item.start && !item.stop && !item.step;
            if (!full_slice) {
                if (axis < shape_prefix.size()) shape_prefix[axis] = -1;
                if (axis < known_shape_prefix.size()) known_shape_prefix.resize(axis);
            }
            ++axis;
        }
        return Type::tensor(*base.first, result_rank,
                            std::move(shape_prefix), std::move(known_shape_prefix));
    }

    if (base.kind == TypeKind::Bin) {
        if (node->items.size() != 1) {
            error("INDEX_ARITY", "bin indexing requires exactly one index or slice.", expression.span);
        }
        const auto& item = node->items.front();
        if (item.slice) {
            if (item.start) check_index_operand(*item.start);
            if (item.stop) check_index_operand(*item.stop);
            if (item.step) error("SLICE_STEP", "bin slices do not take a step.", item.step->span);
            type = simple(TypeKind::Bin);
        } else {
            if (!item.index) error("INDEX_SYNTAX", "bin index is missing.", item.span);
            check_index_operand(*item.index);
            type = simple(TypeKind::Bin);
        }
        return type;
    }

    if (node->items.size() != 1 || node->items.front().slice ||
        !node->items.front().index) {
        error("INDEX_ARITY", "Array and string indexing requires exactly one integer index.",
              expression.span);
    }
    check_index_operand(*node->items.front().index);
    check_static_index_bounds(base, *node->items.front().index);
    if (base.kind == TypeKind::Array) {
        check_element_read(*node->base, node->items.front().index.get(), expression.span);
        type = *base.first;
        // A class value may enter array storage only after all of its fields
        // are definitely initialized. The array element itself can still be
        // runtime-uninitialized (for array(n)/fixed storage), but a successful
        // indexed read of a stored class value therefore recovers the complete
        // class-field initialization proof.
        if (type.kind == TypeKind::Class) {
            class_expr_initialized_paths_[&expression] = complete_class_paths(type);
        }
    } else if (base.kind == TypeKind::String) {
        type = simple(TypeKind::String);
    } else {
        error("TYPE_MISMATCH", "Indexing requires an array, bin, string, or tensor.", expression.span);
    }

    return type;
}

Type Checker::check_method_call_expr(const Expr& expression,
                                     const MethodCallExpr& node_value,
                                     const Type* /*expected*/) {
    Type type = simple(TypeKind::Void);
    const auto* node = &node_value;
        const auto* receiver_name = binding_name(*node->receiver);
        const auto type_receiver =
            receiver_name ? builtin_scalar_type(receiver_name->name) : std::optional<Type>{};

        if (receiver_name && enum_types_.contains(receiver_name->name)) {
            const auto enum_type = enum_types_.at(receiver_name->name);
            const auto it = std::find(enum_type.case_names.begin(), enum_type.case_names.end(), node->method);
            if (it == enum_type.case_names.end())
                error("UNKNOWN_MEMBER", "Enum '" + receiver_name->name + "' has no variant '" + node->method + "'.", expression.span);
            const auto tag = static_cast<int>(it - enum_type.case_names.begin());
            const auto payload = enum_type.cases[static_cast<std::size_t>(tag)];
            if (!node->type_arguments.empty())
                error("GENERIC_TARGET", "Enum variants do not take type arguments.", expression.span);
            if (payload.kind == TypeKind::Void)
                error("ARGUMENT_MISMATCH", "Payload-free enum variant '" + receiver_name->name + "." + node->method + "' is a value; write it without ().", expression.span);
            if (node->args.size() != 1 || node->args[0].writable || node->args[0].name)
                error("ARGUMENT_MISMATCH", "Enum payload variant '" + receiver_name->name + "." + node->method + "' requires exactly one positional value.", expression.span);
            const auto actual = check_expr(*node->args[0].value, &payload);
            if (!poisoned(actual) && !assignable(actual, payload))
                error("TYPE_MISMATCH", "Enum payload expected " + type_name(payload) + ", got " + type_name(actual) + ".", node->args[0].span);
            enum_constructions_[&expression] = EnumConstructionInfo{enum_type, tag, payload};
            type = poisoned(actual) ? simple(TypeKind::Invalid) : enum_type;
        } else if (type_receiver && type_receiver->kind == TypeKind::Bin &&
            node->method == "fill") {
            if (!node->type_arguments.empty() || node->args.size() != 2 ||
                node->args[0].writable || node->args[0].name ||
                node->args[1].writable || node->args[1].name) {
                error("ARGUMENT_MISMATCH",
                      "bin.fill(n, bit) requires exactly two positional integer arguments.",
                      expression.span);
            }
            auto int_type = simple(TypeKind::Int);
            auto nat_type = simple(TypeKind::Nat);
            auto count = check_expr(*node->args[0].value, &nat_type);
            auto fill = check_expr(*node->args[1].value, &int_type);
            if (const auto value = constant_eval::integer(*node->args[0].value);
                value && *value < 0) {
                error("ARGUMENT_MISMATCH", "bin.fill length cannot be negative.",
                      node->args[0].span);
            }
            if (const auto value = constant_eval::integer(*node->args[1].value);
                value && *value != 0 && *value != 1) {
                error("ARGUMENT_MISMATCH", "bin.fill bit must be 0 or 1.",
                      node->args[1].span);
            }
            expr_types_[node->receiver.get()] = raw_types_[node->receiver.get()] = *type_receiver;
            type = poisoned(count) || poisoned(fill)
                ? simple(TypeKind::Invalid)
                : simple(TypeKind::Bin);
        } else if (type_receiver && type_receiver->kind == TypeKind::String &&
                   node->method == "from_utf8") {
            if (!node->type_arguments.empty() || node->args.size() != 1 ||
                node->args[0].writable ||
                (node->args[0].name && *node->args[0].name != "data")) {
                error("ARGUMENT_MISMATCH",
                      "string.from_utf8(data) requires one bin value.",
                      expression.span);
            }
            auto bin_type = simple(TypeKind::Bin);
            auto data = check_expr(*node->args[0].value, &bin_type);
            expr_types_[node->receiver.get()] = raw_types_[node->receiver.get()] = *type_receiver;
            type = poisoned(data)
                ? simple(TypeKind::Invalid)
                : Type::union_of({simple(TypeKind::String), simple(TypeKind::Error)});
        } else if (type_receiver && type_receiver->kind == TypeKind::String &&
                   node->method == "repeat") {
            if (!node->type_arguments.empty() || node->args.size() != 2 ||
                node->args[0].writable || node->args[0].name ||
                node->args[1].writable || node->args[1].name) {
                error("ARGUMENT_MISMATCH",
                      "string.repeat(value, n) requires a string and an integer count.",
                      expression.span);
            }
            auto string_type = simple(TypeKind::String);
            auto nat_type = simple(TypeKind::Nat);
            auto value = check_expr(*node->args[0].value, &string_type);
            auto count = check_expr(*node->args[1].value, &nat_type);
            if (const auto amount = constant_eval::integer(*node->args[1].value);
                amount && *amount < 0) {
                error("ARGUMENT_MISMATCH", "string.repeat count cannot be negative.",
                      node->args[1].span);
            }
            expr_types_[node->receiver.get()] = raw_types_[node->receiver.get()] = *type_receiver;
            type = poisoned(value) || poisoned(count)
                ? simple(TypeKind::Invalid)
                : simple(TypeKind::String);
        } else if (type_receiver &&
            (is_numeric(*type_receiver) || type_receiver->kind == TypeKind::Bin) &&
            node->method == "parse") {
            if (!node->type_arguments.empty()) {
                error("ARGUMENT_MISMATCH", "parse does not take type arguments.", expression.span);
            }
            if (node->args.size() != 1 || node->args[0].writable ||
                (node->args[0].name && *node->args[0].name != "text")) {
                error("ARGUMENT_MISMATCH", "Type.parse requires one string argument.", expression.span);
            }
            auto string_type = simple(TypeKind::String);
            auto argument = check_expr(*node->args[0].value, &string_type);
            expr_types_[node->receiver.get()] = raw_types_[node->receiver.get()] = *type_receiver;
            if (poisoned(argument)) {
                type = simple(TypeKind::Invalid);
            } else if (type_receiver->kind == TypeKind::Bin) {
                if (const auto* literal = std::get_if<StringExpr>(&node->args[0].value->data)) {
                    for (const char ch : literal->value) {
                        if (ch != '0' && ch != '1') {
                            error("BIN_PARSE", "bin.parse accepts only '0' and '1'.",
                                  node->args[0].span);
                        }
                    }
                }
                type = Type::union_of({simple(TypeKind::Bin), simple(TypeKind::Error)});
            } else {
                type = Type::union_of({*type_receiver, simple(TypeKind::Error)});
            }
        } else {
            Type receiver;
            const std::string* internal = nullptr;
            if (receiver_name &&
                       current_reference_parameters_.contains(receiver_name->name) &&
                       variables_.at(receiver_name->name).kind == TypeKind::Class) {
                receiver = variables_.at(receiver_name->name);
                expr_types_[node->receiver.get()] = raw_types_[node->receiver.get()] = receiver;
            } else {
                receiver = check_expr(*node->receiver);
            }

            if (poisoned(receiver)) {
                type = receiver;
            } else if (receiver.kind == TypeKind::Class &&
                       receiver.class_name == standard_class::atomic_counter) {
                if (!node->type_arguments.empty()) {
                    error("GENERIC_TARGET",
                          "atomic.Counter methods do not take type arguments.",
                          expression.span);
                }
                if (node->method == "add") {
                    if (const_access_path(*node->receiver)) {
                        error("WRITE_CAPABILITY",
                              "atomic.Counter.add cannot mutate through a const access path.",
                              expression.span);
                    }
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH",
                              "atomic.Counter.add requires one int delta.",
                              expression.span);
                    }
                    auto int_type = simple(TypeKind::Int);
                    auto delta = check_expr(*node->args[0].value, &int_type);
                    if (!poisoned(delta) && delta != int_type) {
                        error("TYPE_MISMATCH",
                              "atomic.Counter.add requires an int delta.",
                              node->args[0].span);
                    }
                    type = poisoned(delta) ? simple(TypeKind::Invalid) : int_type;
                } else if (node->method == "load") {
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH",
                              "atomic.Counter.load takes no arguments.",
                              expression.span);
                    }
                    type = simple(TypeKind::Int);
                } else {
                    error("UNKNOWN_METHOD",
                          "atomic.Counter has no method '" + node->method + "'.",
                          expression.span);
                    type = simple(TypeKind::Invalid);
                }
            } else if (receiver.kind == TypeKind::Class &&
                       receiver.class_name == standard_class::autograd_target) {
                if (node->method == "has_grad") {
                    if (!node->type_arguments.empty() || !node->args.empty()) {
                        error("ARGUMENT_MISMATCH",
                              "autograd.Target.has_grad() takes no arguments.",
                              expression.span);
                    }
                    type = simple(TypeKind::Bool);
                } else if (node->method == "clear_grad") {
                    if (!node->type_arguments.empty() || !node->args.empty()) {
                        error("ARGUMENT_MISMATCH",
                              "autograd.Target.clear_grad() takes no arguments.",
                              expression.span);
                    }
                    if (const_access_path(*node->receiver)) {
                        error("WRITE_CAPABILITY",
                              "autograd.Target.clear_grad cannot mutate through a const access path.",
                              expression.span);
                    }
                    type = simple(TypeKind::Void);
                } else if (node->method == "gradient") {
                    if (node->type_arguments.size() != 1 || !node->args.empty()) {
                        error("ARGUMENT_MISMATCH",
                              "autograd.Target.gradient<T>() requires one floating type argument and no value arguments.",
                              expression.span);
                    }
                    Type element = node->type_arguments.size() == 1
                        ? resolve_type(node->type_arguments.front())
                        : simple(TypeKind::Invalid);
                    if (!poisoned(element) &&
                        element.kind != TypeKind::Real32 &&
                        element.kind != TypeKind::Real64) {
                        error("TYPE_MISMATCH",
                              "autograd.Target.gradient<T>() requires T to be real32 or real64.",
                              expression.span);
                        element = simple(TypeKind::Invalid);
                    }
                    type = poisoned(element)
                        ? simple(TypeKind::Invalid)
                        : Type::tensor(element);
                } else {
                    error("UNKNOWN_METHOD",
                          "autograd.Target has no method '" + node->method + "'.",
                          expression.span);
                    type = simple(TypeKind::Invalid);
                }
            } else if (receiver.kind == TypeKind::Class &&
                       receiver.class_name == standard_class::file_handle) {
                if (!node->type_arguments.empty()) {
                    error("GENERIC_TARGET",
                          "file.Handle methods do not take type arguments.",
                          expression.span);
                }
                const bool one_string_argument =
                    node->method == "write" || node->method == "write_line";
                const bool one_int_argument = node->method == "seek";
                if (one_string_argument) {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH",
                              "file.Handle." + node->method + " requires one string argument.",
                              expression.span);
                    }
                    auto string_type = simple(TypeKind::String);
                    auto argument = check_expr(*node->args[0].value, &string_type);
                    if (!poisoned(argument) && argument != string_type) {
                        error("TYPE_MISMATCH",
                              "file.Handle." + node->method + " requires a string.",
                              node->args[0].span);
                    }
                } else if (one_int_argument) {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH",
                              "file.Handle.seek requires one nat position.",
                              expression.span);
                    }
                    auto nat_type = simple(TypeKind::Nat);
                    auto argument = check_expr(*node->args[0].value, &nat_type);
                    if (!poisoned(argument) && argument != nat_type) {
                        error("TYPE_MISMATCH", "file.Handle.seek requires a nat.", node->args[0].span);
                    }
                } else if (!node->args.empty()) {
                    error("ARGUMENT_MISMATCH",
                          "file.Handle." + node->method + " takes no arguments.",
                          expression.span);
                }
                if (const_access_path(*node->receiver)) {
                    error("WRITE_CAPABILITY",
                          "file.Handle operations cannot mutate through a const access path.",
                          expression.span);
                }
                if (const auto path = current_receiver_path(*node->receiver)) {
                    current_receiver_effect_.writes.insert(*path);
                }
                if (const auto path =
                        current_reference_parameter_path(*node->receiver)) {
                    current_reference_effects_[path->first].writes.insert(path->second);
                }

                if (node->method == "read") {
                    type = Type::union_of({
                        simple(TypeKind::String), simple(TypeKind::Error)});
                } else if (node->method == "read_line") {
                    type = Type::union_of({
                        simple(TypeKind::String), simple(TypeKind::None),
                        simple(TypeKind::Error)});
                } else if (node->method == "read_bin") {
                    type = Type::union_of({
                        simple(TypeKind::Bin), simple(TypeKind::Error)});
                } else if (node->method == "write" || node->method == "write_line" ||
                           node->method == "flush" || node->method == "seek") {
                    type = Type::union_of({simple(TypeKind::Void), simple(TypeKind::Error)});
                } else if (node->method == "close") {
                    type = simple(TypeKind::Void);
                } else {
                    error("UNKNOWN_METHOD",
                          "file.Handle has no method '" + node->method + "'.",
                          expression.span);
                    type = simple(TypeKind::Invalid);
                }
            } else if (receiver.kind != TypeKind::Class) {
                if (receiver.kind == TypeKind::Tensor) {
                    const auto shape_type = Type::array(simple(TypeKind::Int));
                    const auto nat_shape_type = Type::array(simple(TypeKind::Nat));
                    const bool floating_tensor = receiver.first &&
                        (receiver.first->kind == TypeKind::Real32 ||
                         receiver.first->kind == TypeKind::Real64);
                    if (node->method == "track" || node->method == "retrack") {
                        const bool targeted_track =
                            node->method == "track" && node->args.size() == 1;
                        if (!node->type_arguments.empty() ||
                            (node->method == "retrack" && !node->args.empty()) ||
                            (node->method == "track" && node->args.size() > 1)) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor." + node->method +
                                      (node->method == "track"
                                           ? "() accepts at most one &autograd.Target."
                                           : "() takes no arguments."),
                                  expression.span);
                        }
                        if (targeted_track) {
                            const auto& argument = node->args[0];
                            if (!argument.writable || argument.name) {
                                error("ARGUMENT_MISMATCH",
                                      "tensor.track target must be written as &target.",
                                      argument.span);
                            }
                            const auto target_type =
                                Type::class_type(standard_class::autograd_target);
                            auto actual = check_expr(*argument.value, &target_type);
                            if (!poisoned(actual) && actual != target_type) {
                                error("TYPE_MISMATCH",
                                      "tensor.track target must be autograd.Target.",
                                      argument.span);
                            }
                            if (const_access_path(*argument.value)) {
                                error("WRITE_CAPABILITY",
                                      "tensor.track target cannot be const.",
                                      argument.span);
                            }
                            if (!stable_writable_storage(*argument.value)) {
                                error("WRITE_CAPABILITY",
                                      "tensor.track target must name writable stable storage.",
                                      argument.span);
                            }
                        }
                        if (!floating_tensor) {
                            error("TYPE_MISMATCH",
                                  "tensor." + node->method +
                                      "() requires tensor<real32> or tensor<real64>.",
                                  expression.span);
                        }
                        type = receiver;
                    } else if (node->method == "untrack") {
                        if (!node->type_arguments.empty() || !node->args.empty()) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.untrack() takes no arguments.", expression.span);
                        }
                        type = receiver;
                    } else if (node->method == "clear_grad") {
                        if (!node->type_arguments.empty() || !node->args.empty()) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.clear_grad() takes no arguments.",
                                  expression.span);
                        }
                        if (!floating_tensor) {
                            error("TYPE_MISMATCH",
                                  "tensor.clear_grad() requires tensor<real32> or tensor<real64>.",
                                  expression.span);
                        }
                        if (const_access_path(*node->receiver)) {
                            error("WRITE_CAPABILITY",
                                  "tensor.clear_grad cannot mutate through a const access path.",
                                  expression.span);
                        }
                        type = simple(TypeKind::Void);
                    } else if (node->method == "backward") {
                        if (!floating_tensor) {
                            error("TYPE_MISMATCH",
                                  "tensor.backward requires tensor<real32> or tensor<real64>.",
                                  expression.span);
                        }
                        if (!node->type_arguments.empty()) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.backward does not take type arguments.",
                                  expression.span);
                        }
                        std::function<bool(const Type&,std::unordered_set<std::string>&)>
                            contains_autograd_target =
                                [&](const Type& candidate,
                                    std::unordered_set<std::string>& active) -> bool {
                            if (candidate.kind == TypeKind::Class &&
                                candidate.class_name == standard_class::autograd_target)
                                return true;
                            if (candidate.kind == TypeKind::Array) {
                                if (!candidate.first) return false;
                                return contains_autograd_target(*candidate.first, active);
                            }
                            if (candidate.kind != TypeKind::Class) return false;
                            if (!active.insert(candidate.class_name).second) return false;
                            const auto found = classes_.find(candidate.class_name);
                            if (found == classes_.end()) {
                                active.erase(candidate.class_name);
                                return false;
                            }
                            for (const auto& field : found->second.fields) {
                                if (contains_autograd_target(field.type, active)) {
                                    active.erase(candidate.class_name);
                                    return true;
                                }
                            }
                            active.erase(candidate.class_name);
                            return false;
                        };
                        bool saw_named_track = false;
                        std::size_t gradient_targets = 0;
                        for (std::size_t index = 0; index < node->args.size(); ++index) {
                            const auto& argument = node->args[index];
                            if (argument.name) {
                                if (*argument.name != "track" || argument.writable ||
                                    saw_named_track || index + 1 != node->args.size()) {
                                    error("ARGUMENT_MISMATCH",
                                          "tensor.backward accepts only a final named track = bool option after writable targets.",
                                          argument.span);
                                }
                                const auto bool_type = simple(TypeKind::Bool);
                                check_expr(*argument.value, &bool_type);
                                saw_named_track = true;
                                continue;
                            }
                            if (!argument.writable) {
                                error("WRITE_CAPABILITY",
                                      "tensor.backward gradient targets must be written with &.",
                                      argument.span);
                            }
                            ++gradient_targets;
                            if (const_access_path(*argument.value)) {
                                error("WRITE_CAPABILITY",
                                      "tensor.backward cannot write a gradient through const storage.",
                                      argument.span);
                            }
                            if (!stable_writable_storage(*argument.value)) {
                                error("WRITE_CAPABILITY",
                                      "tensor.backward target must name writable stable storage.",
                                      argument.span);
                            }
                            Type target = check_expr(*argument.value);
                            bool valid = false;
                            if (target.kind == TypeKind::Tensor && target.first &&
                                receiver.first && *target.first == *receiver.first) {
                                valid = true;
                            } else if (target.kind == TypeKind::Class) {
                                std::unordered_set<std::string> active;
                                valid = contains_autograd_target(target, active);
                            }
                            if (!poisoned(target) && !valid) {
                                error("TYPE_MISMATCH",
                                      "tensor.backward target must be a matching floating tensor, autograd.Target, or class containing an autograd.Target.",
                                      argument.span);
                            }
                        }
                        if (gradient_targets == 0) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.backward requires at least one writable gradient target.",
                                  expression.span);
                        }
                        type = simple(TypeKind::Void);
                    } else if (node->method == "gpu") {
                        const auto int_type = simple(TypeKind::Nat);
                        if (!node->type_arguments.empty() || node->args.size() != 1 ||
                            node->args[0].writable || node->args[0].name) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.gpu(index) requires exactly one positional integer GPU index.",
                                  expression.span);
                        }
                        auto gpu = node->args.empty()
                            ? simple(TypeKind::Invalid)
                            : check_expr(*node->args[0].value, &int_type);
                        if (!node->args.empty()) {
                            if (const auto index =
                                    constant_eval::integer(*node->args[0].value,
                                                           &const_integer_values_);
                                index && *index < 0) {
                                error("ARGUMENT_MISMATCH",
                                      "tensor.gpu(index) requires a non-negative GPU index.",
                                      node->args[0].span);
                                gpu = simple(TypeKind::Invalid);
                            }
                        }
                        type = poisoned(gpu) ? simple(TypeKind::Invalid) : receiver;
                    } else if (node->method == "cpu") {
                        if (!node->type_arguments.empty() || !node->args.empty()) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.cpu() takes no arguments.", expression.span);
                        }
                        type = receiver;
                    } else if (node->method == "transpose") {
                        const auto int_type = simple(TypeKind::Nat);
                        if (!node->type_arguments.empty() || node->args.size() != 2 ||
                            node->args[0].writable || node->args[1].writable ||
                            node->args[0].name || node->args[1].name) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.transpose(axis0, axis1) requires exactly two positional integer axes.",
                                  expression.span);
                        }
                        auto axis0 = node->args.size() > 0
                            ? check_expr(*node->args[0].value, &int_type)
                            : simple(TypeKind::Invalid);
                        auto axis1 = node->args.size() > 1
                            ? check_expr(*node->args[1].value, &int_type)
                            : simple(TypeKind::Invalid);
                        type = poisoned(axis0) || poisoned(axis1)
                            ? simple(TypeKind::Invalid)
                            : Type::tensor(*receiver.first, receiver.length);
                        if (!poisoned(type)) {
                            const auto constant_axis = [&](std::size_t index)
                                -> std::optional<long long> {
                                if (index >= node->args.size()) return std::nullopt;
                                return constant_eval::integer(
                                    *node->args[index].value, &const_integer_values_);
                            };
                            const auto a0 = constant_axis(0);
                            const auto a1 = constant_axis(1);
                            const auto check_axis = [&](const std::optional<long long>& axis,
                                                        const SourceSpan& span) {
                                if (!axis) return;
                                if (*axis < 0 ||
                                    (receiver.length >= 0 && *axis >= receiver.length)) {
                                    error("ARGUMENT_MISMATCH",
                                          "tensor.transpose axis is outside the tensor rank.",
                                          span);
                                }
                            };
                            check_axis(a0, node->args[0].span);
                            check_axis(a1, node->args[1].span);
                            if (a0 && a1 && receiver.length >= 0) {
                                for (long long output_axis = 0;
                                     output_axis < receiver.length; ++output_axis) {
                                    long long source_axis = output_axis;
                                    if (output_axis == *a0) source_axis = *a1;
                                    else if (output_axis == *a1) source_axis = *a0;
                                    const auto extent = tensor_known_extent(
                                        receiver, static_cast<std::size_t>(source_axis));
                                    if (!extent) break;
                                    type.tensor_known_shape_prefix.push_back(*extent);
                                }
                            }
                        }
                    } else if (node->method == "contiguous") {
                        if (!node->type_arguments.empty() || !node->args.empty()) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.contiguous() takes no arguments.",
                                  expression.span);
                        }
                        type = receiver;
                    } else if (node->method == "reshape") {
                        if (!node->type_arguments.empty() || node->args.size() != 1 ||
                            node->args[0].writable ||
                            (node->args[0].name && *node->args[0].name != "shape")) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.reshape requires one shape array.", expression.span);
                        }
                        auto shape = check_expr(*node->args[0].value, &nat_shape_type);
                        long long rank = -1;
                        std::vector<long long> known_shape_prefix;
                        if (!poisoned(shape)) {
                            if (const auto* literal =
                                    std::get_if<ArrayExpr>(&node->args[0].value->data)) {
                                // Literal arity is static rank information. Constant extent
                                // values are flow facts only: keep them out of the public
                                // source type while retaining them for diagnostics/optimization.
                                rank = static_cast<long long>(literal->elements.size());
                                for (const auto& extent_expression : literal->elements) {
                                    const auto extent = constant_eval::integer(
                                        *extent_expression, &const_integer_values_);
                                    if (!extent || *extent < 0) break;
                                    known_shape_prefix.push_back(*extent);
                                }
                            } else {
                                const auto raw = raw_types_.find(node->args[0].value.get());
                                if (raw != raw_types_.end() &&
                                    raw->second.kind == TypeKind::Array &&
                                    raw->second.length >= 0) {
                                    rank = raw->second.length;
                                }
                            }
                        }
                        type = poisoned(shape)
                            ? simple(TypeKind::Invalid)
                            : Type::tensor(*receiver.first, rank, {}, known_shape_prefix);
                    } else if (node->method == "gather") {
                        if (!node->type_arguments.empty() || node->args.size() != 2 ||
                            (node->args.size() > 0 && (node->args[0].writable ||
                             (node->args[0].name && *node->args[0].name != "indices"))) ||
                            (node->args.size() > 1 && (node->args[1].writable ||
                             (node->args[1].name && *node->args[1].name != "shape")))) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.gather(indices, shape) requires integer index and shape arrays.",
                                  expression.span);
                        }
                        if (node->args.size() != 2) {
                            type = simple(TypeKind::Invalid);
                        } else {
                            auto indices = check_expr(*node->args[0].value, &shape_type);
                            auto shape = check_expr(*node->args[1].value, &shape_type);
                            long long rank = -1;
                            std::vector<long long> known_shape_prefix;
                            if (!poisoned(shape)) {
                                if (const auto* literal =
                                        std::get_if<ArrayExpr>(&node->args[1].value->data)) {
                                    rank = static_cast<long long>(literal->elements.size());
                                    for (const auto& extent_expression : literal->elements) {
                                        const auto extent = constant_eval::integer(
                                            *extent_expression, &const_integer_values_);
                                        if (!extent || *extent < 0) break;
                                        known_shape_prefix.push_back(*extent);
                                    }
                                } else {
                                    const auto raw =
                                        raw_types_.find(node->args[1].value.get());
                                    if (raw != raw_types_.end() &&
                                        raw->second.kind == TypeKind::Array &&
                                        raw->second.length >= 0) {
                                        rank = raw->second.length;
                                    }
                                }
                            }
                            type = poisoned(indices) || poisoned(shape)
                                ? simple(TypeKind::Invalid)
                                : Type::tensor(*receiver.first, rank, {}, std::move(known_shape_prefix));
                        }
                    } else if (node->method == "scatter") {
                        if (!node->type_arguments.empty() || node->args.size() != 2 ||
                            (node->args.size() > 0 && (node->args[0].writable ||
                             (node->args[0].name && *node->args[0].name != "indices"))) ||
                            (node->args.size() > 1 && (node->args[1].writable ||
                             (node->args[1].name && *node->args[1].name != "shape")))) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.scatter(indices, shape) requires integer index and shape arrays.",
                                  expression.span);
                        }
                        if (receiver.first && !is_tensor_numeric(*receiver.first)) {
                            error("TYPE_MISMATCH",
                                  "tensor.scatter requires a numeric tensor element type.",
                                  expression.span);
                        }
                        if (node->args.size() != 2) {
                            type = simple(TypeKind::Invalid);
                        } else {
                            auto indices = check_expr(*node->args[0].value, &shape_type);
                            auto shape = check_expr(*node->args[1].value, &shape_type);
                            long long rank = -1;
                            std::vector<long long> known_shape_prefix;
                            if (!poisoned(shape)) {
                                if (const auto* literal =
                                        std::get_if<ArrayExpr>(&node->args[1].value->data)) {
                                    rank = static_cast<long long>(literal->elements.size());
                                    for (const auto& extent_expression : literal->elements) {
                                        const auto extent = constant_eval::integer(
                                            *extent_expression, &const_integer_values_);
                                        if (!extent || *extent < 0) break;
                                        known_shape_prefix.push_back(*extent);
                                    }
                                } else {
                                    const auto raw =
                                        raw_types_.find(node->args[1].value.get());
                                    if (raw != raw_types_.end() &&
                                        raw->second.kind == TypeKind::Array &&
                                        raw->second.length >= 0) {
                                        rank = raw->second.length;
                                    }
                                }
                            }
                            type = poisoned(indices) || poisoned(shape)
                                ? simple(TypeKind::Invalid)
                                : Type::tensor(*receiver.first, rank, {},
                                               std::move(known_shape_prefix));
                        }
                    } else if (node->method == "shape") {
                        if (!node->type_arguments.empty() || !node->args.empty()) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.shape() takes no arguments.", expression.span);
                        }
                        type = Type::array(simple(TypeKind::Nat), receiver.length);
                    } else if (node->method == "device") {
                        if (!node->type_arguments.empty() || !node->args.empty()) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.device() takes no arguments.", expression.span);
                        }
                        type = simple(TypeKind::Int);
                    } else if (node->method == "is_contiguous") {
                        if (!node->type_arguments.empty() || !node->args.empty()) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.is_contiguous() takes no arguments.", expression.span);
                        }
                        type = simple(TypeKind::Bool);
                    } else if (node->method == "is_tracked") {
                        if (!node->type_arguments.empty() || !node->args.empty()) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.is_tracked() takes no arguments.", expression.span);
                        }
                        type = simple(TypeKind::Bool);
                    } else if (node->method == "has_grad") {
                        if (!node->type_arguments.empty() || !node->args.empty()) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.has_grad() takes no arguments.", expression.span);
                        }
                        if (!floating_tensor) {
                            error("TYPE_MISMATCH",
                                  "tensor.has_grad() requires tensor<real32> or tensor<real64>.",
                                  expression.span);
                        }
                        type = simple(TypeKind::Bool);
                    } else if (node->method == "item") {
                        if (!node->type_arguments.empty() || !node->args.empty()) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.item() takes no arguments.", expression.span);
                        }
                        if (receiver.length > 0) {
                            error("TYPE_MISMATCH",
                                  "tensor.item() requires a rank-0 tensor; index or reshape the tensor explicitly.",
                                  expression.span);
                        }
                        type = *receiver.first;
                    } else if (node->method == "all" || node->method == "any") {
                        if (!node->type_arguments.empty() || !node->args.empty()) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor<bool>." + node->method + "() takes no arguments.",
                                  expression.span);
                        }
                        if (!receiver.first || receiver.first->kind != TypeKind::Bool) {
                            error("UNKNOWN_MEMBER",
                                  "Boolean reductions all()/any() are available only on tensor<bool>.",
                                  expression.span);
                        }
                        type = simple(TypeKind::Bool);
                    } else {
                        error("UNKNOWN_MEMBER",
                              "Type '" + type_name(receiver) + "' has no method '" +
                                  node->method + "'.",
                              expression.span);
                    }
                } else {
                auto require_no_type_arguments = [&] {
                    if (!node->type_arguments.empty()) {
                        error("ARGUMENT_MISMATCH", "Primitive value methods do not take type arguments.", expression.span);
                    }
                };
                auto require_count = [&](std::size_t count, const std::string& signature) {
                    if (node->args.size() != count) error("ARGUMENT_MISMATCH", signature, expression.span);
                };
                auto check_plain_argument = [&](std::size_t index, const char* label, const Type& required) {
                    const auto& argument = node->args[index];
                    if (argument.writable || (argument.name && *argument.name != label)) {
                        error("ARGUMENT_MISMATCH", "Invalid primitive method argument.", argument.span);
                    }
                    return check_expr(*argument.value, &required);
                };
                require_no_type_arguments();
                if (receiver.kind == TypeKind::String) {
                    const auto string_type = simple(TypeKind::String);
                    const auto int_type = simple(TypeKind::Int);
                    if (node->method == "string") {
                        require_count(0, "string.string() takes no arguments."); type = string_type;
                    } else if (node->method == "contains") {
                        require_count(1, "string.contains requires needle.");
                        auto a=check_plain_argument(0,"needle",string_type);
                        type=poisoned(a)?simple(TypeKind::Invalid):simple(TypeKind::Bool);
                    } else if (node->method == "starts_with") {
                        require_count(1, "string.starts_with requires prefix.");
                        auto a=check_plain_argument(0,"prefix",string_type);
                        type=poisoned(a)?simple(TypeKind::Invalid):simple(TypeKind::Bool);
                    } else if (node->method == "ends_with") {
                        require_count(1, "string.ends_with requires suffix.");
                        auto a=check_plain_argument(0,"suffix",string_type);
                        type=poisoned(a)?simple(TypeKind::Invalid):simple(TypeKind::Bool);
                    } else if (node->method == "find") {
                        require_count(1, "string.find requires needle.");
                        auto a=check_plain_argument(0,"needle",string_type);
                        type=poisoned(a)?simple(TypeKind::Invalid):Type::union_of({simple(TypeKind::Nat),simple(TypeKind::None)});
                    } else if (node->method == "slice") {
                        require_count(2, "string.slice requires start and end.");
                        auto a=check_plain_argument(0,"start",int_type);
                        auto b=check_plain_argument(1,"end",int_type);
                        type=poisoned(a)||poisoned(b)?simple(TypeKind::Invalid):string_type;
                    } else if (node->method == "trim") {
                        require_count(0, "string.trim() takes no arguments."); type=string_type;
                    } else if (node->method == "split") {
                        require_count(1, "string.split requires separator.");
                        auto a=check_plain_argument(0,"separator",string_type);
                        type=poisoned(a)?simple(TypeKind::Invalid):Type::array(string_type);
                    } else if (node->method == "utf8") {
                        require_count(0, "string.utf8() takes no arguments.");
                        type=simple(TypeKind::Bin);
                    } else if (node->method == "codepoints") {
                        require_count(0, "string.codepoints() takes no arguments.");
                        type=Type::array(int_type);
                    } else {
                        error("UNKNOWN_MEMBER","Type 'string' has no method '" + node->method + "'.",expression.span);
                    }
                } else if (receiver.kind == TypeKind::Array &&
                           (node->method == "append" || node->method == "concat" ||
                            node->method == "join" || node->method == "sorted")) {
                    if (node->method == "append") {
                        require_count(1, "array.append requires value.");
                        auto a=check_plain_argument(0,"value",*receiver.first);
                        if (!poisoned(a) && receiver.first->kind == TypeKind::Class &&
                            !fully_initialized_for_equality(*node->args[0].value, *receiver.first)) {
                            error("UNINITIALIZED_ARGUMENT",
                                  "Class values stored in arrays must have every field definitely initialized.",
                                  node->args[0].span);
                        }
                        type=poisoned(a)?simple(TypeKind::Invalid):Type::array(*receiver.first);
                    } else if (node->method == "concat") {
                        require_count(1, "array.concat requires other.");
                        const auto other_type=Type::array(*receiver.first);
                        auto a=check_plain_argument(0,"other",other_type);
                        type=poisoned(a)?simple(TypeKind::Invalid):Type::array(*receiver.first);
                    } else if (node->method == "join") {
                        if (receiver.first->kind != TypeKind::String) {
                            error("UNKNOWN_MEMBER","join is available on string arrays.",expression.span);
                        }
                        require_count(1, "string[].join requires separator.");
                        const auto text_type=simple(TypeKind::String);
                        auto a=check_plain_argument(0,"separator",text_type);
                        type=poisoned(a)?simple(TypeKind::Invalid):text_type;
                    } else {
                        if (!is_tensor_numeric(*receiver.first) &&
                            !is_bare_integer(*receiver.first) &&
                            receiver.first->kind != TypeKind::Bool &&
                            receiver.first->kind != TypeKind::String) {
                            error("UNKNOWN_MEMBER",
                                  "sorted is available on fixed-width numeric, bool, and string arrays.",
                                  expression.span);
                        }
                        require_count(0, "array.sorted() takes no arguments.");
                        type=Type::array(*receiver.first);
                    }
                } else {
                    if (node->method != "string" || !printable(receiver)) {
                        error("UNKNOWN_MEMBER","Type '" + type_name(receiver) + "' has no method '" + node->method + "'.",expression.span);
                    }
                    require_count(0, "value.string() takes no arguments.");
                    type = simple(TypeKind::String);
                }
                }
            } else {
                internal = find_method(receiver.class_name, node->method);
                if (!internal) {
                    error("UNKNOWN_MEMBER", "Class '" + receiver.class_name + "' has no method '" + node->method + "'.", expression.span);
                }
                const auto owner_it = classes_.find(receiver.class_name);
                if (owner_it != classes_.end()) {
                    if (const auto private_it = owner_it->second.private_methods.find(node->method);
                        private_it != owner_it->second.private_methods.end() &&
                        current_class_ != private_it->second) {
                        error("PRIVATE_MEMBER",
                              "Private method '" + node->method + "' is only accessible inside class '" +
                                  private_it->second + "'.",
                              expression.span);
                    }
                }
                if (!node->type_arguments.empty()) {
                    throw std::logic_error("ConcreteProgram contains unresolved generic method arguments.");
                }
                method_calls_[&expression] = MethodCallInfo{*internal};
                const auto& function = functions_.at(*internal);
                const bool mutates_receiver =
                    !function.receiver_effect.writes.empty() ||
                    !function.receiver_effect.initializes.empty() ||
                    !function.receiver_effect.invalidates.empty();
                if (const_access_path(*node->receiver) && mutates_receiver) {
                    error("WRITE_CAPABILITY",
                          "Mutating methods cannot be called through a const access path.",
                          expression.span);
                }
                const std::size_t offset = 1;
                const std::size_t count = function.parameters.size() - offset;
                std::vector<bool> filled(count);
                std::size_t positional = 0;
                std::unordered_set<std::string> labels;
                bool named = false;
                bool any_poison = false;
                std::vector<PendingReferenceEffect> pending_reference_effects;

                for (const auto& argument : node->args) {
                    if (argument.name) {
                        named = true;
                        if (!labels.insert(*argument.name).second) {
                            error("ARGUMENT_MISMATCH",
                                  "Argument '" + *argument.name + "' is supplied more than once.",
                                  argument.span);
                        }
                    } else if (named) {
                        error("ARGUMENT_MISMATCH",
                              "Positional argument cannot follow named arguments; name it explicitly.",
                              argument.span);
                    }

                    std::size_t target = 0;
                    if (argument.name) {
                        const auto begin =
                            function.parameters.begin() + static_cast<std::ptrdiff_t>(offset);
                        const auto it = std::find_if(
                            begin, function.parameters.end(),
                            [&](const auto& parameter) { return parameter.name == *argument.name; });
                        if (it == function.parameters.end()) {
                            error("ARGUMENT_MISMATCH",
                                  "Unknown method argument '" + *argument.name + "'.",
                                  argument.span);
                        }
                        target = static_cast<std::size_t>(it - begin);
                    } else {
                        if (positional >= count) {
                            error("ARGUMENT_MISMATCH",
                                  "Too many positional method arguments.", argument.span);
                        }
                        target = positional++;
                    }
                    if (filled[target]) {
                        error("ARGUMENT_MISMATCH",
                              "Argument '" + function.parameters[target + offset].name +
                                  "' is supplied more than once.",
                              argument.span);
                    }
                    filled[target] = true;
                    const auto& parameter = function.parameters[target + offset];
                    if (argument.writable != parameter.writable) {
                        error("WRITE_CAPABILITY", "Argument reference form must match parameter.", argument.span);
                    }
                    if (argument.writable) {
                        const bool readonly_reference = parameter.is_const;
                        const bool valid_storage = readonly_reference
                            ? stable_addressable_storage(*argument.value)
                            : stable_writable_storage(*argument.value);
                        if (!valid_storage) {
                            reject_bare_storage_root(*argument.value);
                            error("WRITE_CAPABILITY",
                                  readonly_reference
                                      ? "Readonly reference arguments require stable addressable storage."
                                      : "Writable reference arguments require storage with write authority.",
                                  argument.span);
                        }
                        auto actual = check_address_target(*argument.value);
                        any_poison |= poisoned(actual);
                        if (!readonly_reference) {
                            if (const auto path = writable_storage_path(*argument.value)) {
                            if (narrowed_.contains(path->first) || borrowed_.contains(path->first)) {
                                error("WRITE_CAPABILITY",
                                      "Writable reference arguments require unnarrowed, unborrowed storage.",
                                      argument.span);
                            }
                            }
                        }
                        if (!poisoned(actual) && actual != parameter.type) {
                            error("TYPE_MISMATCH", "Reference arguments require identical declared types.", argument.span);
                        }
                        if (!poisoned(actual)) {
                            pending_reference_effects.push_back(
                                PendingReferenceEffect{argument.value.get(), &parameter, argument.span});
                        }
                    } else {
                        any_poison |= poisoned(check_expr(*argument.value, &parameter.type));
                    }
                }
                for (std::size_t i = 0; i < count; ++i) {
                    if (!filled[i] && !function.parameters[i + offset].default_value) {
                        error("ARGUMENT_MISMATCH",
                              "Missing required argument '" +
                                  function.parameters[i + offset].name + "'.",
                              expression.span);
                    }
                }

                if (!any_poison) {
                    finish_call_effects(
                        function, pending_reference_effects,
                        node->receiver.get(), false, expression.span);
                }
                type = any_poison ? simple(TypeKind::Invalid) : function.result;
            if (!any_poison && type.kind == TypeKind::Class) {
                class_expr_initialized_paths_[&expression] = function.return_initialized_fields;
            }
            }
        }

    return type;
}

Type Checker::check_builtin_call_expr(const Expr& expression,
                                      const CallExpr& node_value,
                                      BuiltinCallable builtin,
                                      const Type* expected) {
    Type type = simple(TypeKind::Void);
    const auto* node = &node_value;
    const auto& name = node->callee;
    auto builtin_arg = [&](std::size_t i, const std::string& label,
                           const Type* required = nullptr) {
        const auto& argument = node->args[i];
        if (argument.writable || (argument.name && *argument.name != label)) {
            error("ARGUMENT_MISMATCH", "Invalid builtin argument.", argument.span);
        }
        return check_expr(*argument.value, required);
    };
            call_resolutions_[&expression] =
                CallResolution{CallKind::Builtin, name, builtin, simple(TypeKind::Void)};
            switch (builtin) {
                case BuiltinCallable::Scan: {
                    // scan(&n) reads one value; scan("{&n} {&m}") reads a line
                    // against a format whose literal text must match the input.
                    if (node->args.size() != 1 || node->args[0].name) {
                        error("ARGUMENT_MISMATCH",
                              "scan takes one argument: an input target such as scan(&n), or a format string such as scan(\"{&n} {&m}\").",
                              expression.span);
                    }
                    const auto& argument = node->args[0];
                    ScanFormat format;
                    if (argument.writable) {
                        format.literals = {"", ""};
                        format.targets.push_back(argument.value.get());
                    } else if (const auto* text = std::get_if<StringTemplateExpr>(&argument.value->data)) {
                        format.literals = text->literals;
                        for (std::size_t i = 0; i < text->expressions.size(); ++i) {
                            const auto* placeholder = std::get_if<UnaryExpr>(&text->expressions[i]->data);
                            if (!placeholder || placeholder->op != "scan&") {
                                error("ARGUMENT_MISMATCH",
                                      "scan format placeholders are input targets written {&name}.",
                                      text->expressions[i]->span);
                            }
                            if (i > 0 && text->literals[i].empty()) {
                                error("ARGUMENT_MISMATCH",
                                      "Two input targets need literal text between them so scan can split the input.",
                                      text->expressions[i]->span);
                            }
                            format.targets.push_back(placeholder->operand.get());
                        }
                    } else if (std::holds_alternative<StringExpr>(argument.value->data)) {
                        error("ARGUMENT_MISMATCH",
                              "scan format names no input target; write {&name} where a value is read.",
                              argument.span);
                    } else {
                        error("ARGUMENT_MISMATCH",
                              "scan takes an input target such as scan(&n) or a format string literal such as scan(\"{&n} {&m}\").",
                              argument.span);
                    }
                    const bool terminating = error_terminating_expr_ == &expression;
                    bool any_poison = false;
                    for (const auto* target : format.targets) {
                        if (!stable_writable_storage(*target)) {
                            error("WRITE_CAPABILITY", "scan targets require storage with write authority.",
                                  target->span);
                        }
                        if (const auto path = writable_storage_path(*target)) {
                            if (narrowed_.contains(path->first) || borrowed_.contains(path->first)) {
                                error("WRITE_CAPABILITY",
                                      "scan targets require unnarrowed, unborrowed storage.",
                                      target->span);
                            }
                        }
                        auto actual = check_address_target(*target);
                        any_poison |= poisoned(actual);
                        if (!poisoned(actual) && !is_numeric(actual) && actual.kind != TypeKind::String) {
                            error("TYPE_MISMATCH",
                                  "scan reads numeric and string targets; this target has type '" +
                                      type_name(actual) + "'.",
                                  target->span);
                        }
                        format.target_types.push_back(actual);
                        if (terminating) {
                            mark_storage_initialized(*target);
                        } else if (!storage_initialized(*target)) {
                            error("UNINITIALIZED",
                                  "A scan whose error is handled leaves its targets unchanged on failure, so this target must be initialized first; as a statement or under try, scan initializes it.",
                                  target->span);
                        }
                    }
                    type = any_poison
                        ? simple(TypeKind::Invalid)
                        : Type::union_of({simple(TypeKind::Void), simple(TypeKind::Error)});
                    if (!any_poison) scan_formats_[&expression] = std::move(format);
                    break;
                }
                case BuiltinCallable::Print: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "print requires one argument.", expression.span);
                    }
                    auto argument = builtin_arg(0, "value");
                    // print is a success-only consumer. When its argument
                    // is an unnamed T|error with a printable T, consume the error
                    // through the same contextual fail-fast rule as a typed
                    // binding or function parameter.
                    if (!poisoned(argument) && argument.kind == TypeKind::Union &&
                        argument.union_name.empty() &&
                        case_index(argument, simple(TypeKind::Error)) >= 0) {
                        std::vector<Type> non_error;
                        for (const auto& current : argument.cases) {
                            if (current.kind != TypeKind::Error) non_error.push_back(current);
                        }
                        if (!non_error.empty()) {
                            const auto residual = Type::union_of(std::move(non_error));
                            if (printable(residual) || residual.kind == TypeKind::Address) {
                                fail_fast_expressions_.insert(node->args[0].value.get());
                                expr_types_[node->args[0].value.get()] = residual;
                                argument = residual;
                            }
                        }
                    }
                    if (!poisoned(argument) && !printable(argument) &&
                        argument.kind != TypeKind::Address) {
                        error("TYPE_MISMATCH", "print requires a scalar or address.", expression.span);
                    }
                    type = poisoned(argument)
                        ? simple(TypeKind::Invalid)
                        : Type::union_of({simple(TypeKind::Void), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::Exit: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "process.exit requires one status argument.", expression.span);
                    }
                    auto required = simple(TypeKind::Int);
                    auto argument = builtin_arg(0, "status", &required);
                    type = poisoned(argument) ? simple(TypeKind::Invalid) : simple(TypeKind::Never);
                    break;
                }
                case BuiltinCallable::Array: {
                    if (node->args.empty() || node->args.size() > 2 ||
                        (node->args[0].name && *node->args[0].name != "n") ||
                        (node->args.size() == 2 &&
                         (!node->args[1].name || *node->args[1].name != "fill"))) {
                        error("ARGUMENT_MISMATCH",
                              "array requires n and optionally fill = value.", expression.span);
                    }
                    auto nat_type = simple(TypeKind::Nat);
                    auto count = builtin_arg(0, "n", &nat_type);
                    if (node->args.size() == 1) {
                        if (!expected || expected->kind != TypeKind::Array) {
                            error("AMBIGUOUS_TYPE",
                                  "array(n) requires an explicit array type context because no element value is provided.",
                                  expression.span);
                        }
                        if (expected->length >= 0) {
                            const auto literal = constant_eval::integer(*node->args[0].value);
                            if (!literal || *literal != expected->length) {
                                error("ARRAY_SHAPE",
                                      "array(n) used for a fixed array requires a matching constant length.",
                                      expression.span);
                            }
                        }
                        type = poisoned(count) ? simple(TypeKind::Invalid) : *expected;
                    } else {
                        auto element = builtin_arg(
                            1, "fill",
                            expected && expected->kind == TypeKind::Array ? expected->first.get() : nullptr);
                        if (poisoned(count) || poisoned(element)) {
                            type = simple(TypeKind::Invalid);
                        } else {
                            if (!is_storable(element)) {
                                error("TYPE_MISMATCH", "Array fill must be storable.", expression.span);
                            }
                            if (element.kind == TypeKind::Class &&
                                !fully_initialized_for_equality(*node->args[1].value, element)) {
                                error("UNINITIALIZED_ARGUMENT",
                                      "Class values stored in arrays must have every field definitely initialized.",
                                      node->args[1].span);
                            }
                            if (expected && expected->kind == TypeKind::Array &&
                                expected->length >= 0) {
                                const auto literal = constant_eval::integer(*node->args[0].value);
                                if (!literal || *literal != expected->length) {
                                    error("ARRAY_SHAPE",
                                          "array(n, fill = value) used for a fixed array requires a matching constant length.",
                                          expression.span);
                                }
                                type = *expected;
                            } else {
                                type = Type::array(element);
                            }
                        }
                    }
                    break;
                }
                case BuiltinCallable::Range: {
                    if (node->args.empty() || node->args.size() > 3) {
                        error("ARGUMENT_MISMATCH", "range takes one to three arguments.", expression.span);
                    }
                    // The bounds and the step are integers of any kind; a
                    // literal materializes as int.
                    auto int_type = simple(TypeKind::Int);
                    bool any_poison = false;
                    for (std::size_t i = 0; i < node->args.size(); ++i) {
                        const std::string label =
                            node->args.size() == 1 ? "end" :
                            i == 0 ? "start" : i == 1 ? "end" : "step";
                        const auto& argument = node->args[i];
                        if (argument.writable || (argument.name && *argument.name != label)) {
                            error("ARGUMENT_MISMATCH", "Invalid builtin argument.", argument.span);
                        }
                        const auto bound =
                            numeric_literal_family(*argument.value) != NumericLiteralCategory::None
                                ? check_expr(*argument.value, &int_type)
                                : check_expr(*argument.value);
                        if (!poisoned(bound) && !is_integer_family_type(bound)) {
                            error("TYPE_MISMATCH", "range bounds and step must be integers.",
                                  argument.span);
                        }
                        any_poison |= poisoned(bound);
                    }
                    type = any_poison ? simple(TypeKind::Invalid) : simple(TypeKind::Range);
                    break;
                }
                case BuiltinCallable::Len: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "len requires one string, array, or bin argument.", expression.span);
                    }
                    auto argument = builtin_arg(0, "value");
                    if (poisoned(argument)) {
                        type = argument;
                    } else {
                        if (argument.kind != TypeKind::String &&
                            argument.kind != TypeKind::Array &&
                            argument.kind != TypeKind::Bin) {
                            error("TYPE_MISMATCH", "len requires a string, array, or bin.", expression.span);
                        }
                        type = simple(TypeKind::Nat);
                    }
                    break;
                }
                case BuiltinCallable::ExactAtom: {
                    if (node->args.size() != 2) {
                        error("ARGUMENT_MISMATCH",
                              "real.atom requires a provider string and an integer opcode.",
                              expression.span);
                    }
                    auto string_type = simple(TypeKind::String);
                    auto int_type = simple(TypeKind::Int);
                    auto provider_type = builtin_arg(0, "provider", &string_type);
                    auto opcode_type = builtin_arg(1, "opcode", &int_type);
                    if (!poisoned(provider_type) &&
                        !std::holds_alternative<StringExpr>(
                            node->args[0].value->data)) {
                        error("EXACT_PROVIDER",
                              "real.atom provider must be a string literal.",
                              node->args[0].span);
                    }
                    const auto* opcode =
                        std::get_if<IntegerExpr>(&node->args[1].value->data);
                    if (!poisoned(opcode_type) &&
                        (!opcode || !opcode->fits_u64 ||
                         opcode->value >
                             std::numeric_limits<std::uint32_t>::max())) {
                        error("EXACT_PROVIDER",
                              "real.atom opcode must be a nat32-range integer literal.",
                              node->args[1].span);
                    }
                    if (!expected || !is_real(*expected)) {
                        error("AMBIGUOUS_NUMERIC_LITERAL",
                              "real.atom requires a unique real-family type context.",
                              expression.span);
                        type = simple(TypeKind::Invalid);
                    } else {
                        type = *expected;
                    }
                    break;
                }
                case BuiltinCallable::ExactUnary: {
                    if (node->args.size() != 3) {
                        error("ARGUMENT_MISMATCH",
                              "real.unary requires a provider string, an integer opcode, and a real value.",
                              expression.span);
                    }
                    auto string_type = simple(TypeKind::String);
                    auto int_type = simple(TypeKind::Int);
                    auto real_type = simple(TypeKind::Real);
                    auto provider_type = builtin_arg(0, "provider", &string_type);
                    auto opcode_type = builtin_arg(1, "opcode", &int_type);
                    auto value_type = builtin_arg(2, "value", &real_type);
                    if (!poisoned(provider_type) &&
                        !std::holds_alternative<StringExpr>(
                            node->args[0].value->data)) {
                        error("EXACT_PROVIDER",
                              "real.unary provider must be a string literal.",
                              node->args[0].span);
                    }
                    const auto* opcode =
                        std::get_if<IntegerExpr>(&node->args[1].value->data);
                    if (!poisoned(opcode_type) &&
                        (!opcode || !opcode->fits_u64 ||
                         opcode->value >
                             std::numeric_limits<std::uint32_t>::max())) {
                        error("EXACT_PROVIDER",
                              "real.unary opcode must be a nat32-range integer literal.",
                              node->args[1].span);
                    }
                    type = poisoned(value_type)
                        ? simple(TypeKind::Invalid)
                        : real_type;
                    break;
                }
                case BuiltinCallable::Flush: {
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "flush takes no arguments.", expression.span);
                    }
                    type = Type::union_of({simple(TypeKind::Void), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::CliArgument: {
                    if (node->args.size() != 2 || !expected) {
                        error("ARGUMENT_MISMATCH", "argument() is only valid inside a typed cli declaration.", expression.span);
                    }
                    auto string_type = simple(TypeKind::String);
                    auto int_type = simple(TypeKind::Int);
                    (void)builtin_arg(0, "name", &string_type);
                    (void)builtin_arg(1, "index", &int_type);
                    if (expected->kind != TypeKind::String && expected->kind != TypeKind::Int64 &&
                        expected->kind != TypeKind::Real64 && expected->kind != TypeKind::Int &&
                        expected->kind != TypeKind::Real && expected->kind != TypeKind::Bool) {
                        error("TYPE_MISMATCH",
                              "CLI arguments support string, int64, int, real64, real, and bool.",
                              expression.span);
                    }
                    type = *expected;
                    break;
                }
                case BuiltinCallable::CliArgumentOptional: {
                    if (node->args.size() != 3 || !expected) {
                        error("ARGUMENT_MISMATCH", "argument(default = ...) is only valid inside a typed cli declaration.", expression.span);
                    }
                    auto string_type = simple(TypeKind::String);
                    auto int_type = simple(TypeKind::Int);
                    (void)builtin_arg(0, "name", &string_type);
                    (void)builtin_arg(1, "index", &int_type);
                    (void)builtin_arg(2, "default", expected);
                    if (expected->kind != TypeKind::String && expected->kind != TypeKind::Int64 &&
                        expected->kind != TypeKind::Real64 && expected->kind != TypeKind::Int &&
                        expected->kind != TypeKind::Real && expected->kind != TypeKind::Bool) {
                        error("TYPE_MISMATCH",
                              "Optional CLI arguments support string, int64, int, real64, real, and bool.",
                              expression.span);
                    }
                    type = *expected;
                    break;
                }
                case BuiltinCallable::CliOption: {
                    if (node->args.size() != 2 || !expected) {
                        error("ARGUMENT_MISMATCH", "option() is only valid inside a typed cli declaration.", expression.span);
                    }
                    auto string_type = simple(TypeKind::String);
                    (void)builtin_arg(0, "name", &string_type);
                    (void)builtin_arg(1, "default", expected);
                    if (expected->kind != TypeKind::String && expected->kind != TypeKind::Int64 &&
                        expected->kind != TypeKind::Real64 && expected->kind != TypeKind::Int &&
                        expected->kind != TypeKind::Real && expected->kind != TypeKind::Bool) {
                        error("TYPE_MISMATCH",
                              "CLI options support string, int64, int, real64, real, and bool.",
                              expression.span);
                    }
                    type = *expected;
                    break;
                }
                case BuiltinCallable::CliFlag: {
                    if (node->args.size() != 1 || !expected) {
                        error("ARGUMENT_MISMATCH", "flag() is only valid inside a typed cli declaration.", expression.span);
                    }
                    auto string_type = simple(TypeKind::String);
                    (void)builtin_arg(0, "name", &string_type);
                    if (expected->kind != TypeKind::Bool) {
                        error("TYPE_MISMATCH", "flag() requires a bool field.", expression.span);
                    }
                    type = simple(TypeKind::Bool);
                    break;
                }
                case BuiltinCallable::CliFinish: {
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "internal cli finalization takes no arguments.", expression.span);
                    }
                    type = simple(TypeKind::Void);
                    break;
                }
                case BuiltinCallable::FileOpen:
                case BuiltinCallable::FileCreate:
                case BuiltinCallable::FileAppend: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", name + " requires one path string.", expression.span);
                    }
                    auto string_type = simple(TypeKind::String);
                    auto path = builtin_arg(0, "path", &string_type);
                    type = poisoned(path) ? simple(TypeKind::Invalid)
                        : Type::union_of({
                            Type::class_type(standard_class::file_handle),
                            simple(TypeKind::Error)});
                    if (!poisoned(path)) {
                        class_expr_initialized_paths_[&expression] = {"$handle"};
                    }
                    break;
                }
                case BuiltinCallable::FileRead: {
                    if (node->args.size() != 1) error("ARGUMENT_MISMATCH", "file.read requires one path.", expression.span);
                    auto string_type = simple(TypeKind::String);
                    auto path = builtin_arg(0, "path", &string_type);
                    type = poisoned(path) ? simple(TypeKind::Invalid)
                        : Type::union_of({string_type, simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::FileReadBin: {
                    if (node->args.size() != 1) error("ARGUMENT_MISMATCH", "file.read_bin requires one path.", expression.span);
                    auto string_type = simple(TypeKind::String);
                    auto path = builtin_arg(0, "path", &string_type);
                    type = poisoned(path) ? simple(TypeKind::Invalid)
                        : Type::union_of({simple(TypeKind::Bin), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::FileWrite: {
                    if (node->args.size() != 2) error("ARGUMENT_MISMATCH", "file.write requires path and text.", expression.span);
                    auto string_type = simple(TypeKind::String);
                    auto path = builtin_arg(0, "path", &string_type);
                    auto text = builtin_arg(1, "text", &string_type);
                    type = poisoned(path) || poisoned(text) ? simple(TypeKind::Invalid)
                        : Type::union_of({simple(TypeKind::Void), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::FileWriteBin: {
                    if (node->args.size() != 2) error("ARGUMENT_MISMATCH", "file.write_bin requires path and bin.", expression.span);
                    auto string_type = simple(TypeKind::String);
                    auto bin_type = simple(TypeKind::Bin);
                    auto path = builtin_arg(0, "path", &string_type);
                    auto bin = builtin_arg(1, "bin", &bin_type);
                    type = poisoned(path) || poisoned(bin) ? simple(TypeKind::Invalid)
                        : Type::union_of({simple(TypeKind::Void), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::FileList: {
                    if (node->args.empty() || node->args.size() > 2) {
                        error("ARGUMENT_MISMATCH",
                              "file.list requires path and optional recursive = bool.",
                              expression.span);
                    }
                    auto string_type = simple(TypeKind::String);
                    auto bool_type = simple(TypeKind::Bool);
                    auto path = builtin_arg(0, "path", &string_type);
                    bool any_poison = poisoned(path);
                    if (node->args.size() == 2) {
                        any_poison |= poisoned(builtin_arg(1, "recursive", &bool_type));
                    }
                    type = any_poison ? simple(TypeKind::Invalid)
                        : Type::union_of({Type::array(string_type), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::FileExists:
                case BuiltinCallable::FileIsDirectory: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", name + " requires one path.", expression.span);
                    }
                    auto string_type = simple(TypeKind::String);
                    auto path = builtin_arg(0, "path", &string_type);
                    type = poisoned(path) ? simple(TypeKind::Invalid)
                        : Type::union_of({simple(TypeKind::Bool), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::FileRemove:
                case BuiltinCallable::FileMkdir: {
                    if (node->args.size() != 1) error("ARGUMENT_MISMATCH", name + " requires one path.", expression.span);
                    auto string_type = simple(TypeKind::String);
                    auto path = builtin_arg(0, "path", &string_type);
                    type = poisoned(path) ? simple(TypeKind::Invalid)
                        : Type::union_of({simple(TypeKind::Void), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::FileCopy:
                case BuiltinCallable::FileMove: {
                    if (node->args.size() != 2) error("ARGUMENT_MISMATCH", name + " requires source and destination.", expression.span);
                    auto string_type = simple(TypeKind::String);
                    auto source = builtin_arg(0, "source", &string_type);
                    auto destination = builtin_arg(1, "destination", &string_type);
                    type = poisoned(source) || poisoned(destination) ? simple(TypeKind::Invalid)
                        : Type::union_of({simple(TypeKind::Void), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::EnvironmentGet: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "environment.get requires one name.", expression.span);
                    }
                    auto string_type = simple(TypeKind::String);
                    auto key = builtin_arg(0, "name", &string_type);
                    type = poisoned(key) ? simple(TypeKind::Invalid)
                        : Type::union_of({string_type, simple(TypeKind::None)});
                    break;
                }
                case BuiltinCallable::EnvironmentHas: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "environment.has requires one name.", expression.span);
                    }
                    auto string_type = simple(TypeKind::String);
                    auto key = builtin_arg(0, "name", &string_type);
                    type = poisoned(key) ? simple(TypeKind::Invalid) : simple(TypeKind::Bool);
                    break;
                }
                case BuiltinCallable::TestCheck: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "test.check requires one bool condition.", expression.span);
                    }
                    auto bool_type = simple(TypeKind::Bool);
                    auto condition = builtin_arg(0, "condition", &bool_type);
                    type = poisoned(condition) ? simple(TypeKind::Invalid) : simple(TypeKind::Void);
                    break;
                }
                case BuiltinCallable::TestEqual: {
                    if (node->args.size() != 2) {
                        error("ARGUMENT_MISMATCH", "test.equal requires actual and expected values.", expression.span);
                    }
                    const auto actual_family =
                        numeric_literal_family(*node->args[0].value);
                    const auto expected_family =
                        numeric_literal_family(*node->args[1].value);
                    const bool actual_family_only = single_family_category(actual_family);
                    const bool expected_family_only = single_family_category(expected_family);

                    Type actual;
                    Type expected_value;
                    if (actual_family_only && !expected_family_only) {
                        expected_value = builtin_arg(1, "expected");
                        actual = poisoned(expected_value)
                            ? simple(TypeKind::Invalid)
                            : builtin_arg(0, "actual", &expected_value);
                    } else {
                        actual = builtin_arg(0, "actual");
                        expected_value = poisoned(actual)
                            ? simple(TypeKind::Invalid)
                            : builtin_arg(1, "expected", &actual);
                    }
                    if (poisoned(actual) || poisoned(expected_value)) {
                        type = simple(TypeKind::Invalid);
                    } else {
                        if (actual != expected_value) {
                            error("TYPE_MISMATCH", "test.equal values must have identical types.", expression.span);
                        }
                        if (!equality_supported(actual)) {
                            error("TYPE_MISMATCH", "test.equal is not defined for this type.", expression.span);
                        }
                        if (actual.kind == TypeKind::Class &&
                            (!fully_initialized_for_equality(*node->args[0].value, actual) ||
                             !fully_initialized_for_equality(*node->args[1].value, expected_value))) {
                            error("UNINITIALIZED_FIELD_EQUALITY",
                                  "test.equal requires every compared class field to be definitely initialized.",
                                  expression.span);
                        }
                        type = simple(TypeKind::Void);
                    }
                    break;
                }
                case BuiltinCallable::TimeNow: {
                    if (node->args.size() > 1 ||
                        (node->args.size() == 1 &&
                         (!node->args[0].name || *node->args[0].name != "sync"))) {
                        error("ARGUMENT_MISMATCH",
                              "time.now accepts only optional sync = bool.",
                              expression.span);
                    }
                    bool any_poison = false;
                    if (node->args.size() == 1) {
                        auto bool_type = simple(TypeKind::Bool);
                        any_poison = poisoned(builtin_arg(0, "sync", &bool_type));
                    }
                    type = any_poison ? simple(TypeKind::Invalid)
                                      : Type::class_type(standard_class::time_instant);
                    if (!any_poison) class_expr_initialized_paths_[&expression] = {"$seconds"};
                    break;
                }
                case BuiltinCallable::TimeSince: {
                    if (node->args.empty() || node->args.size() > 2 ||
                        (node->args.size() == 2 &&
                         (!node->args[1].name || *node->args[1].name != "sync"))) {
                        error("ARGUMENT_MISMATCH",
                              "time.since requires start and optional sync = bool.",
                              expression.span);
                    }
                    bool any_poison = false;
                    if (!node->args.empty()) {
                        auto instant_type = Type::class_type(standard_class::time_instant);
                        auto start = builtin_arg(0, "start", &instant_type);
                        any_poison |= poisoned(start);
                        if (!poisoned(start) &&
                            !initialized_paths_for_expr(*node->args[0].value).contains("$seconds")) {
                            error("UNINITIALIZED",
                                  "time.since requires an initialized Instant.",
                                  expression.span);
                        }
                    }
                    if (node->args.size() == 2) {
                        auto bool_type = simple(TypeKind::Bool);
                        any_poison |= poisoned(builtin_arg(1, "sync", &bool_type));
                    }
                    type = any_poison ? simple(TypeKind::Invalid)
                                      : Type::class_type(standard_class::time_duration);
                    if (!any_poison) class_expr_initialized_paths_[&expression] = {"$seconds"};
                    break;
                }
                case BuiltinCallable::TimeSeconds: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "time.seconds requires one real64 value.", expression.span);
                    }
                    auto float_type = simple(TypeKind::Real64);
                    auto seconds = builtin_arg(0, "value", &float_type);
                    type = poisoned(seconds) ? simple(TypeKind::Invalid)
                                             : Type::class_type(standard_class::time_duration);
                    if (!poisoned(seconds)) class_expr_initialized_paths_[&expression] = {"$seconds"};
                    break;
                }
                case BuiltinCallable::TimeSleep: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "time.sleep requires one Duration.", expression.span);
                    }
                    auto duration_type = Type::class_type(standard_class::time_duration);
                    auto duration = builtin_arg(0, "duration", &duration_type);
                    if (!poisoned(duration) &&
                        !initialized_paths_for_expr(*node->args[0].value).contains("$seconds")) {
                        error("UNINITIALIZED", "time.sleep requires an initialized Duration.", expression.span);
                    }
                    type = poisoned(duration) ? simple(TypeKind::Invalid) : simple(TypeKind::Void);
                    break;
                }
                case BuiltinCallable::GpuSync: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "gpu.sync requires one GPU index.", expression.span);
                    }
                    auto nat_type = simple(TypeKind::Nat);
                    auto index = builtin_arg(0, "index", &nat_type);
                    if (!node->args.empty()) {
                        if (const auto value =
                                constant_eval::integer(*node->args[0].value,
                                                       &const_integer_values_);
                            value && *value < 0) {
                            error("ARGUMENT_MISMATCH",
                                  "gpu.sync(index) requires a non-negative GPU index.",
                                  node->args[0].span);
                            index = simple(TypeKind::Invalid);
                        }
                    }
                    type = poisoned(index) ? simple(TypeKind::Invalid)
                                           : simple(TypeKind::Void);
                    break;
                }
                case BuiltinCallable::AutogradTarget: {
                    if (!node->type_arguments.empty() || !node->args.empty()) {
                        error("ARGUMENT_MISMATCH",
                              "autograd.target() takes no arguments.",
                              expression.span);
                    }
                    type = Type::class_type(standard_class::autograd_target);
                    class_expr_initialized_paths_[&expression] = {"$handle"};
                    break;
                }
                case BuiltinCallable::AtomicCounter: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH",
                              "atomic.counter requires one int initial value.",
                              expression.span);
                    }
                    auto int_type = simple(TypeKind::Int);
                    auto initial = builtin_arg(0, "initial", &int_type);
                    type = poisoned(initial)
                        ? simple(TypeKind::Invalid)
                        : Type::class_type(standard_class::atomic_counter);
                    if (!poisoned(initial)) class_expr_initialized_paths_[&expression] = {"$handle"};
                    break;
                }
                case BuiltinCallable::TaskAll: {
                    if (node->args.size() != 1 && node->args.size() != 2) {
                        error("ARGUMENT_MISMATCH",
                              "task.all requires an operation array and optional atomic.Counter shared value.",
                              expression.span);
                    }
                    const auto& argument = node->args[0];
                    if (argument.writable ||
                        (argument.name && *argument.name != "operations")) {
                        error("ARGUMENT_MISMATCH", "Invalid builtin argument.", argument.span);
                    }

                    if (node->args.size() == 2) {
                        const auto counter_type = Type::class_type(standard_class::atomic_counter);
                        const auto& shared_argument = node->args[1];
                        if (shared_argument.writable ||
                            (shared_argument.name && *shared_argument.name != "shared")) {
                            error("ARGUMENT_MISMATCH", "Invalid shared task argument.", shared_argument.span);
                        }
                        auto shared = check_expr(*shared_argument.value, &counter_type);
                        const auto operation_type =
                            Type::function(simple(TypeKind::Void), {counter_type});
                        const auto operations_type = Type::array(operation_type);
                        Type operations;
                        if (const auto* literal =
                                std::get_if<ArrayExpr>(&argument.value->data)) {
                            bool any_poison = false;
                            for (const auto& item : literal->elements) {
                                any_poison |= poisoned(check_expr(*item, &operation_type));
                            }
                            operations = any_poison ? simple(TypeKind::Invalid) : operations_type;
                            if (!any_poison) {
                                expr_types_[argument.value.get()] = operations_type;
                                raw_types_[argument.value.get()] = operations_type;
                            }
                        } else {
                            operations = check_expr(*argument.value, &operations_type);
                        }
                        type = poisoned(shared) || poisoned(operations)
                            ? simple(TypeKind::Invalid)
                            : simple(TypeKind::Void);
                        break;
                    }

                    Type result_type = simple(TypeKind::Void);
                    if (expected && expected->kind == TypeKind::Array && expected->first &&
                        (expected->first->kind == TypeKind::Int ||
                         expected->first->kind == TypeKind::Int64 ||
                         expected->first->kind == TypeKind::Real64)) {
                        result_type = *expected->first;
                    }

                    auto operation_type = Type::function(result_type, {});
                    auto operations_type = Type::array(operation_type);
                    Type operations;
                    if (const auto* literal =
                            std::get_if<ArrayExpr>(&argument.value->data)) {
                        bool any_poison = false;
                        for (const auto& item : literal->elements) {
                            any_poison |= poisoned(check_expr(*item, &operation_type));
                        }
                        operations = any_poison ? simple(TypeKind::Invalid) : operations_type;
                        if (!any_poison) {
                            expr_types_[argument.value.get()] = operations_type;
                            raw_types_[argument.value.get()] = operations_type;
                        }
                    } else if (result_type.kind != TypeKind::Void) {
                        operations = check_expr(*argument.value, &operations_type);
                    } else {
                        operations = check_expr(*argument.value);
                        if (!poisoned(operations) &&
                            operations.kind == TypeKind::Array && operations.first &&
                            operations.first->kind == TypeKind::Function &&
                            operations.first->first &&
                            operations.first->parameters.empty()) {
                            const auto candidate = *operations.first->first;
                            if (candidate.kind == TypeKind::Void ||
                                candidate.kind == TypeKind::Int ||
                                candidate.kind == TypeKind::Real64) {
                                result_type = candidate;
                                operation_type = Type::function(result_type, {});
                                operations_type = Type::array(operation_type);
                            }
                        }
                    }

                    if (!poisoned(operations) && operations != operations_type) {
                        error("TYPE_MISMATCH",
                              "task.all requires fn<void>()[], fn<int>()[], or fn<real64>()[].",
                              argument.span);
                    }
                    type = poisoned(operations)
                        ? simple(TypeKind::Invalid)
                        : (result_type.kind == TypeKind::Void
                               ? simple(TypeKind::Void)
                               : Type::array(result_type));
                    break;
                }
                case BuiltinCallable::RandomGenerator: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "random.generator requires one int seed.", expression.span);
                    }
                    auto int_type = simple(TypeKind::Int);
                    auto seed = builtin_arg(0, "seed", &int_type);
                    type = poisoned(seed) ? simple(TypeKind::Invalid)
                                          : Type::class_type(standard_class::random_generator);
                    if (!poisoned(seed)) class_expr_initialized_paths_[&expression] = {"$state"};
                    break;
                }
                case BuiltinCallable::RandomInt: {
                    if (current_class_ != standard_class::random_generator) {
                        error("INVALID_CONTEXT", "random generator operation requires a Generator receiver.", expression.span);
                    }
                    if (node->args.size() != 2) {
                        error("ARGUMENT_MISMATCH", "Generator.int requires start and end.", expression.span);
                    }
                    auto int_type = simple(TypeKind::Int);
                    auto start = builtin_arg(0, "start", &int_type);
                    auto end = builtin_arg(1, "end", &int_type);
                    current_receiver_effect_.required.insert("$state");
                    current_receiver_effect_.writes.insert("$state");
                    current_receiver_effect_.initializes.insert("$state");
                    type = poisoned(start) || poisoned(end) ? simple(TypeKind::Invalid) : int_type;
                    break;
                }
                case BuiltinCallable::RandomFloat: {
                    if (current_class_ != standard_class::random_generator) {
                        error("INVALID_CONTEXT", "random generator operation requires a Generator receiver.", expression.span);
                    }
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "Generator.real64 takes no arguments.", expression.span);
                    }
                    current_receiver_effect_.required.insert("$state");
                    current_receiver_effect_.writes.insert("$state");
                    current_receiver_effect_.initializes.insert("$state");
                    type = simple(TypeKind::Real64);
                    break;
                }
                case BuiltinCallable::RandomBool: {
                    if (current_class_ != standard_class::random_generator) {
                        error("INVALID_CONTEXT", "random generator operation requires a Generator receiver.", expression.span);
                    }
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "Generator.bool takes no arguments.", expression.span);
                    }
                    current_receiver_effect_.required.insert("$state");
                    current_receiver_effect_.writes.insert("$state");
                    current_receiver_effect_.initializes.insert("$state");
                    type = simple(TypeKind::Bool);
                    break;
                }
                case BuiltinCallable::ProcessRun: {
                    if (node->args.size() != 2) {
                        error("ARGUMENT_MISMATCH", "process.run requires program and args.", expression.span);
                    }
                    auto string_type = simple(TypeKind::String);
                    auto args_type = Type::array(string_type);
                    auto program = builtin_arg(0, "program", &string_type);
                    auto args = builtin_arg(1, "args", &args_type);
                    type = poisoned(program) || poisoned(args)
                        ? simple(TypeKind::Invalid)
                        : Type::class_type(standard_class::process_result);
                    if (!poisoned(program) && !poisoned(args)) {
                        class_expr_initialized_paths_[&expression] = {
                            "status", "output", "error", "started"
                        };
                    }
                    break;
                }
                case BuiltinCallable::ProcessShell: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "process.shell requires one command string.", expression.span);
                    }
                    auto string_type = simple(TypeKind::String);
                    auto command = builtin_arg(0, "command", &string_type);
                    type = poisoned(command)
                        ? simple(TypeKind::Invalid)
                        : Type::class_type(standard_class::process_result);
                    if (!poisoned(command)) {
                        class_expr_initialized_paths_[&expression] = {
                            "status", "output", "error", "started"
                        };
                    }
                    break;
                }
                case BuiltinCallable::JsonParse: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "json.parse requires one string.", expression.span);
                    }
                    auto string_type = simple(TypeKind::String);
                    auto text = builtin_arg(0, "text", &string_type);
                    type = poisoned(text) ? simple(TypeKind::Invalid)
                        : Type::union_of({
                            Type::class_type(standard_class::json_value), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::JsonKind: {
                    if (current_class_ != standard_class::json_value) {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "Value.kind takes no arguments.", expression.span);
                    }
                    type = simple(TypeKind::String);
                    break;
                }
                case BuiltinCallable::JsonSize: {
                    if (current_class_ != standard_class::json_value) {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "Value.size takes no arguments.", expression.span);
                    }
                    type = Type::union_of({simple(TypeKind::Nat), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::JsonGet: {
                    if (current_class_ != standard_class::json_value) {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "Value.get requires one string key.", expression.span);
                    }
                    auto string_type = simple(TypeKind::String);
                    auto key = builtin_arg(0, "key", &string_type);
                    type = poisoned(key) ? simple(TypeKind::Invalid)
                        : Type::union_of({
                            Type::class_type(standard_class::json_value),
                            simple(TypeKind::None),
                            simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::JsonAt: {
                    if (current_class_ != standard_class::json_value) {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "Value.at requires one nat index.", expression.span);
                    }
                    auto nat_type = simple(TypeKind::Nat);
                    auto index = builtin_arg(0, "index", &nat_type);
                    type = poisoned(index) ? simple(TypeKind::Invalid)
                        : Type::union_of({
                            Type::class_type(standard_class::json_value),
                            simple(TypeKind::None),
                            simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::JsonText: {
                    if (current_class_ != standard_class::json_value) {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "Value.text takes no arguments.", expression.span);
                    }
                    type = Type::union_of({simple(TypeKind::String), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::JsonInteger: {
                    if (current_class_ != standard_class::json_value) {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "Value.integer takes no arguments.", expression.span);
                    }
                    type = Type::union_of({simple(TypeKind::Int), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::JsonNumber: {
                    if (current_class_ != standard_class::json_value) {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "Value.number takes no arguments.", expression.span);
                    }
                    type = Type::union_of({simple(TypeKind::Real64), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::JsonBigReal: {
                    if (current_class_ != standard_class::json_value) {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "Value.real takes no arguments.", expression.span);
                    }
                    type = Type::union_of({simple(TypeKind::Real), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::JsonBoolean: {
                    if (current_class_ != standard_class::json_value) {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "Value.boolean takes no arguments.", expression.span);
                    }
                    type = Type::union_of({simple(TypeKind::Bool), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::JsonEncode: {
                    if (current_class_ != standard_class::json_value) {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "Value.encode takes no arguments.", expression.span);
                    }
                    type = simple(TypeKind::String);
                    break;
                }
                case BuiltinCallable::JsonEqual: {
                    if (current_class_ != standard_class::json_value) {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "Value.equal requires one Value.", expression.span);
                    }
                    auto value_type = Type::class_type(standard_class::json_value);
                    auto other = builtin_arg(0, "other", &value_type);
                    type = poisoned(other) ? simple(TypeKind::Invalid) : simple(TypeKind::Bool);
                    break;
                }
                case BuiltinCallable::HttpGet: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "http.get requires one URL string.", expression.span);
                    }
                    auto string_type = simple(TypeKind::String);
                    auto url = builtin_arg(0, "url", &string_type);
                    type = poisoned(url) ? simple(TypeKind::Invalid)
                        : Type::union_of({
                            Type::class_type(standard_class::http_response), simple(TypeKind::Error)});
                    if (!poisoned(url)) {
                        class_expr_initialized_paths_[&expression] = {
                            "status", "body", "$headers"
                        };
                    }
                    break;
                }
                case BuiltinCallable::HttpHeader: {
                    if (current_class_ != standard_class::http_response) {
                        error("INVALID_CONTEXT", "HTTP header lookup requires a Response receiver.", expression.span);
                    }
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "Response.header requires one header name.", expression.span);
                    }
                    auto string_type = simple(TypeKind::String);
                    auto name_value = builtin_arg(0, "name", &string_type);
                    type = poisoned(name_value) ? simple(TypeKind::Invalid)
                        : Type::union_of({string_type, simple(TypeKind::None)});
                    break;
                }
                case BuiltinCallable::ReflectTypeName: {
                    if (!node->type_arguments.empty()) {
                        error("GENERIC_ARITY",
                              "reflect.type_name(value) does not take type arguments.",
                              expression.span);
                    }
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH",
                              "reflect.type_name(value) requires exactly one class value.",
                              expression.span);
                    }
                    Type source = node->args.size() == 1
                        ? builtin_arg(0, "value")
                        : simple(TypeKind::Invalid);
                    if (!poisoned(source) && source.kind != TypeKind::Class) {
                        error("TYPE_MISMATCH",
                              "reflect.type_name(value) requires a class value.",
                              expression.span);
                    }
                    type = poisoned(source)
                        ? simple(TypeKind::Invalid)
                        : simple(TypeKind::String);
                    break;
                }
                case BuiltinCallable::ReflectCollect:
                case BuiltinCallable::ReflectPaths: {
                    const bool path_query = builtin == BuiltinCallable::ReflectPaths;
                    const std::string reflect_name =
                        path_query ? "reflect.paths" : "reflect.collect";
                    if (node->type_arguments.size() != 1) {
                        error("GENERIC_ARITY",
                              reflect_name + "<T>(value) requires exactly one target type.",
                              expression.span);
                    }
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH",
                              reflect_name + "<T>(value) requires exactly one class value.",
                              expression.span);
                    }
                    Type target = node->type_arguments.size() == 1
                        ? resolve_type(node->type_arguments.front())
                        : simple(TypeKind::Invalid);
                    Type source = node->args.size() == 1
                        ? builtin_arg(0, "value")
                        : simple(TypeKind::Invalid);
                    if (!poisoned(source) && source.kind != TypeKind::Class) {
                        error("TYPE_MISMATCH",
                              reflect_name + "<T>(value) requires a class value.",
                              expression.span);
                    }
                    if (!poisoned(source) && source.kind == TypeKind::Class &&
                        !poisoned(target)) {
                        StorageEffect read_effect;
                        std::unordered_set<std::string> active;
                        const auto require_value = [&](const Type& value_type,
                                                       const std::string& prefix) {
                            if (prefix.empty()) {
                                if (value_type.kind == TypeKind::Class) {
                                    const auto complete = complete_class_paths(value_type);
                                    if (complete.empty()) read_effect.required.insert("");
                                    for (const auto& path : complete)
                                        read_effect.required.insert(path);
                                } else {
                                    read_effect.required.insert("");
                                }
                                return;
                            }
                            read_effect.required.insert(prefix);
                            if (value_type.kind == TypeKind::Class) {
                                for (const auto& path : complete_class_paths(value_type))
                                    read_effect.required.insert(prefix + "." + path);
                            }
                        };
                        const auto contains_target = [&](const Type& root) {
                            std::unordered_set<std::string> seen;
                            std::function<bool(const Type&)> visit;
                            visit = [&](const Type& current) -> bool {
                                if (current == target) return true;
                                if (current.kind == TypeKind::Array) {
                                    return current.first &&
                                           visit(*current.first);
                                }
                                if (current.kind != TypeKind::Class) return false;
                                if (!seen.insert(current.class_name).second) return false;
                                const auto class_it = classes_.find(current.class_name);
                                if (class_it == classes_.end()) {
                                    seen.erase(current.class_name);
                                    return false;
                                }
                                for (const auto& field : class_it->second.fields) {
                                    if (field.is_private) continue;
                                    if (visit(field.type)) {
                                        seen.erase(current.class_name);
                                        return true;
                                    }
                                }
                                seen.erase(current.class_name);
                                return false;
                            };
                            return visit(root);
                        };
                        std::function<void(const Type&, const std::string&)> collect_requirements;
                        collect_requirements =
                            [&](const Type& current, const std::string& prefix) {
                                if (current == target) {
                                    require_value(current, prefix);
                                    return;
                                }
                                if (current.kind == TypeKind::Array) {
                                    if (current.first &&
                                        contains_target(*current.first)) {
                                        // Array indices are structural rather than source
                                        // storage-path names. Requiring the array field
                                        // proves the container exists; runtime traversal
                                        // checks dynamic element initialization.
                                        require_value(current, prefix);
                                    }
                                    return;
                                }
                                if (current.kind != TypeKind::Class) return;
                                if (!active.insert(current.class_name).second) return;
                                const auto class_it = classes_.find(current.class_name);
                                if (class_it != classes_.end()) {
                                    for (const auto& field : class_it->second.fields) {
                                        if (field.is_private ||
                                            !contains_target(field.type))
                                            continue;
                                        const auto path = prefix.empty()
                                            ? field.name
                                            : prefix + "." + field.name;
                                        collect_requirements(field.type, path);
                                    }
                                }
                                active.erase(current.class_name);
                            };
                        collect_requirements(source, "");
                        check_storage_effect_requirements(
                            *node->args[0].value, read_effect, node->args[0].span, false);
                    }
                    call_resolutions_[&expression].reflected_target = target;
                    if (!poisoned(target) &&
                        (target.kind == TypeKind::Void ||
                         target.kind == TypeKind::Error ||
                         target.kind == TypeKind::Invalid)) {
                        error("INVALID_TYPE",
                              reflect_name + " target type must be a storable value type.",
                              expression.span);
                        target = simple(TypeKind::Invalid);
                    }
                    type = poisoned(source) || poisoned(target)
                        ? simple(TypeKind::Invalid)
                        : Type::array(path_query
                            ? simple(TypeKind::String)
                            : target);
                    break;
                }
                case BuiltinCallable::TensorCreate:
                case BuiltinCallable::TensorZeros:
                case BuiltinCallable::TensorOnes: {
                    Type element = simple(TypeKind::Invalid);
                    if (node->type_arguments.size() == 1) {
                        element = resolve_type(node->type_arguments.front());
                    } else if (node->type_arguments.empty() && expected &&
                               expected->kind == TypeKind::Tensor && expected->first) {
                        element = *expected->first;
                    } else {
                        error("GENERIC_ARITY",
                              "tensor construction requires one numeric element type unless the expected tensor type supplies it.",
                              expression.span);
                    }
                    if (!poisoned(element) && is_bare_integer(element)) {
                        error("INVALID_TYPE", "Tensor element types are fixed-width: use tensor<int64>.",
                              expression.span);
                    } else if (!poisoned(element) && !is_tensor_numeric(element) &&
                        element.kind != TypeKind::Bool) {
                        error("INVALID_TYPE", "tensor element type must be a fixed-width native numeric type or bool.",
                              expression.span);
                    }

                    std::optional<std::size_t> shape_index;
                    std::optional<std::size_t> gpu_index;
                    for (std::size_t i = 0; i < node->args.size(); ++i) {
                        const auto& argument = node->args[i];
                        if (argument.writable) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor construction arguments are values, not writable references.",
                                  argument.span);
                            continue;
                        }
                        if (!argument.name) {
                            if (shape_index) {
                                error("ARGUMENT_MISMATCH",
                                      "tensor construction accepts at most one positional shape argument; gpu must be named.",
                                      argument.span);
                            } else {
                                shape_index = i;
                            }
                            continue;
                        }
                        if (*argument.name == "shape") {
                            if (shape_index) {
                                error("ARGUMENT_MISMATCH",
                                      "tensor shape is supplied more than once.",
                                      argument.span);
                            } else {
                                shape_index = i;
                            }
                        } else if (*argument.name == "gpu") {
                            if (gpu_index) {
                                error("ARGUMENT_MISMATCH",
                                      "tensor gpu is supplied more than once.",
                                      argument.span);
                            } else {
                                gpu_index = i;
                            }
                        } else {
                            error("ARGUMENT_MISMATCH",
                                  "tensor construction supports only shape and gpu named arguments.",
                                  argument.span);
                        }
                    }

                    bool bad_gpu = false;
                    if (gpu_index) {
                        const auto int_type = simple(TypeKind::Nat);
                        auto gpu = check_expr(*node->args[*gpu_index].value, &int_type);
                        bad_gpu = poisoned(gpu);
                        if (!bad_gpu && gpu != int_type) {
                            error("TYPE_MISMATCH",
                                  "tensor gpu index expected nat.",
                                  node->args[*gpu_index].span);
                            bad_gpu = true;
                        }
                        if (const auto index =
                                constant_eval::integer(*node->args[*gpu_index].value,
                                                       &const_integer_values_);
                            index && *index < 0) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor gpu index must be non-negative.",
                                  node->args[*gpu_index].span);
                            bad_gpu = true;
                        }
                    }

                    const bool contextual_shape = !shape_index;
                    if (contextual_shape) {
                        if (builtin == BuiltinCallable::TensorCreate) {
                            error("ARGUMENT_MISMATCH",
                                  "Uninitialized tensor construction requires an explicit shape array.",
                                  expression.span);
                        }
                        if (!expected || expected->kind != TypeKind::Tensor ||
                            expected->tensor_shape_prefix.empty()) {
                            error("ARGUMENT_MISMATCH",
                                  "Contextual tensor.zeros()/tensor.ones() requires an exact-rank expected tensor shape.",
                                  expression.span);
                        }
                        if (expected &&
                            std::any_of(expected->tensor_shape_prefix.begin(),
                                        expected->tensor_shape_prefix.end(),
                                        [](long long extent) { return extent == -1; })) {
                            error("ARGUMENT_MISMATCH",
                                  "Contextual tensor allocation cannot infer '_' extents.",
                                  expression.span);
                        }
                        type = expected && !bad_gpu
                            ? Type::tensor(element, expected->length)
                            : simple(TypeKind::Invalid);
                        if (expected && !bad_gpu) {
                            bool prefix_known=true;
                            for (const auto extent : expected->tensor_shape_prefix) {
                                if (extent < 0) {
                                    prefix_known=false;
                                    break;
                                }
                                type.tensor_known_shape_prefix.push_back(extent);
                            }
                            if (!prefix_known) type.tensor_known_shape_prefix.clear();
                        }
                        break;
                    }

                    const auto shape_type = Type::array(simple(TypeKind::Nat));
                    auto shape = check_expr(*node->args[*shape_index].value, &shape_type);
                    long long rank = -1;
                    std::vector<long long> known_shape_prefix;
                    if (!poisoned(shape)) {
                        if (const auto* literal =
                                std::get_if<ArrayExpr>(&node->args[*shape_index].value->data)) {
                            // Literal arity determines rank. Constant extent values remain
                            // internal flow facts and never specialize the source-visible type.
                            rank = static_cast<long long>(literal->elements.size());
                            for (const auto& extent_expression : literal->elements) {
                                const auto extent = constant_eval::integer(
                                    *extent_expression, &const_integer_values_);
                                if (!extent || *extent < 0) break;
                                known_shape_prefix.push_back(*extent);
                            }
                        } else {
                            const auto raw =
                                raw_types_.find(node->args[*shape_index].value.get());
                            if (raw != raw_types_.end() &&
                                raw->second.kind == TypeKind::Array &&
                                raw->second.length >= 0) {
                                rank = raw->second.length;
                            }
                        }
                    }
                    if (!poisoned(shape) && expected &&
                        expected->kind == TypeKind::Tensor &&
                        !expected->tensor_shape_prefix.empty()) {
                        if (const auto* literal =
                                std::get_if<ArrayExpr>(&node->args[*shape_index].value->data)) {
                            if (literal->elements.size() !=
                                expected->tensor_shape_prefix.size()) {
                                error("TYPE_MISMATCH",
                                      "Tensor shape argument contradicts the expected tensor rank.",
                                      node->args[*shape_index].span);
                            } else {
                                for (std::size_t axis = 0;
                                     axis < literal->elements.size(); ++axis) {
                                    const auto required =
                                        expected->tensor_shape_prefix[axis];
                                    if (required < 0) continue;
                                    const auto extent = constant_eval::integer(
                                        *literal->elements[axis], &const_integer_values_);
                                    if (extent && *extent != required) {
                                        error("TYPE_MISMATCH",
                                              "Tensor shape argument contradicts the expected tensor shape.",
                                              literal->elements[axis]->span);
                                    }
                                }
                            }
                        }
                    }
                    type = poisoned(shape) || bad_gpu
                        ? simple(TypeKind::Invalid)
                        : Type::tensor(element, rank, {}, known_shape_prefix);
                    break;
                }
            }
            call_resolutions_[&expression].type = type;

    return type;
}

Type Checker::check_call_expr(const Expr& expression,
                              const CallExpr& node_value,
                              const Type* expected) {
    Type type = simple(TypeKind::Void);
    const auto* node = &node_value;
        const auto& name = node->callee;
        const bool tensor_generic_call =
            name == "tensor" || name == "$std.tensor.zeros" ||
            name == "$std.tensor.ones";
        const bool reflect_generic_call =
            name == "$std.reflect.collect" || name == "$std.reflect.paths";
        if (!node->type_arguments.empty() &&
            !tensor_generic_call && !reflect_generic_call) {
            throw std::logic_error("ConcreteProgram contains unresolved generic call arguments.");
        }
        auto builtin_arg = [&](std::size_t i, const std::string& label, const Type* required = nullptr) {
            const auto& argument = node->args[i];
            if (argument.writable || (argument.name && *argument.name != label)) {
                error("ARGUMENT_MISMATCH", "Invalid builtin argument.", argument.span);
            }
            return check_expr(*argument.value, required);
        };

        bool named = false;
        std::unordered_set<std::string> labels;
        for (const auto& argument : node->args) {
            if (argument.name) {
                named = true;
                if (!labels.insert(*argument.name).second) {
                    error("ARGUMENT_MISMATCH",
                          "Argument '" + *argument.name + "' is supplied more than once.",
                          argument.span);
                }
            } else if (named) {
                error("ARGUMENT_MISMATCH",
                      "Positional argument cannot follow named arguments; name it explicitly.",
                      argument.span);
            }
        }

        if (const auto variable = variables_.find(name); variable != variables_.end()) {
            const auto& callable = variable->second;
            if (callable.kind != TypeKind::Function || !callable.first) {
                error("NOT_CALLABLE", "Binding '" + name + "' is not callable.", expression.span);
            }
            if (current_reference_parameters_.contains(name)) {
                auto& effect = current_reference_effects_[name];
                if (!effect.initializes.contains("")) effect.required.insert("");
            } else if (!initialized_.contains(name)) {
                error("UNINITIALIZED",
                      "Function binding '" + name + "' may be uninitialized.",
                      expression.span);
            }
            if (node->args.size() != callable.parameters.size()) {
                error("ARGUMENT_MISMATCH",
                      "Function value requires exactly " +
                      std::to_string(callable.parameters.size()) + " positional argument(s).",
                      expression.span);
            }
            bool any_poison = false;
            for (std::size_t i = 0; i < node->args.size(); ++i) {
                const auto& argument = node->args[i];
                if (argument.name || argument.writable) {
                    error("ARGUMENT_MISMATCH",
                          "Function-value calls use positional value arguments only.",
                          argument.span);
                }
                const auto actual = check_expr(*argument.value, &callable.parameters[i]);
                any_poison |= poisoned(actual);
            }
            type = any_poison ? simple(TypeKind::Invalid) : *callable.first;
            call_resolutions_[&expression] =
                CallResolution{CallKind::FunctionValue, name, std::nullopt, type};
        } else if (!current_class_.empty() && find_method(current_class_, name)) {
            const auto private_it = classes_.at(current_class_).private_methods.find(name);
            if (private_it != classes_.at(current_class_).private_methods.end() &&
                current_class_ != private_it->second) {
                error("PRIVATE_MEMBER",
                      "Private method '" + name + "' is only accessible inside class '" +
                          private_it->second + "'.",
                      expression.span);
            }
            const auto internal = *find_method(current_class_, name);
            method_calls_[&expression] = MethodCallInfo{internal};
            call_resolutions_[&expression] =
                CallResolution{CallKind::Function, internal, std::nullopt, functions_.at(internal).result};
            const auto& function = functions_.at(internal);
            const std::size_t offset = 1;
            const std::size_t count = function.parameters.size() - offset;
            std::vector<bool> filled(count);
            std::size_t positional = 0;
            bool any_poison = false;
            std::vector<PendingReferenceEffect> pending_reference_effects;

            for (const auto& argument : node->args) {
                std::size_t target = 0;
                if (argument.name) {
                    const auto begin =
                        function.parameters.begin() + static_cast<std::ptrdiff_t>(offset);
                    const auto it = std::find_if(
                        begin, function.parameters.end(),
                        [&](const auto& parameter) { return parameter.name == *argument.name; });
                    if (it == function.parameters.end()) {
                        error("ARGUMENT_MISMATCH",
                              "Unknown method argument '" + *argument.name + "'.",
                              argument.span);
                    }
                    target = static_cast<std::size_t>(it - begin);
                } else {
                    if (positional >= count) {
                        error("ARGUMENT_MISMATCH",
                              "Too many positional method arguments.", argument.span);
                    }
                    target = positional++;
                }
                if (filled[target]) {
                    error("ARGUMENT_MISMATCH",
                          "Argument '" + function.parameters[target + offset].name +
                              "' is supplied more than once.",
                          argument.span);
                }
                filled[target] = true;
                const auto& parameter = function.parameters[target + offset];
                if (argument.writable != parameter.writable) {
                    error("WRITE_CAPABILITY", "Argument reference form must match parameter.", argument.span);
                }
                if (argument.writable) {
                        const bool readonly_reference = parameter.is_const;
                        const bool valid_storage = readonly_reference
                            ? stable_addressable_storage(*argument.value)
                            : stable_writable_storage(*argument.value);
                        if (!valid_storage) {
                            reject_bare_storage_root(*argument.value);
                            error("WRITE_CAPABILITY",
                                  readonly_reference
                                      ? "Readonly reference arguments require stable addressable storage."
                                      : "Writable reference arguments require storage with write authority.",
                                  argument.span);
                        }
                        auto actual = check_address_target(*argument.value);
                        any_poison |= poisoned(actual);
                        if (!readonly_reference) {
                            if (const auto path = writable_storage_path(*argument.value)) {
                            if (narrowed_.contains(path->first) || borrowed_.contains(path->first)) {
                                error("WRITE_CAPABILITY",
                                      "Writable reference arguments require unnarrowed, unborrowed storage.",
                                      argument.span);
                            }
                            }
                        }
                        if (!poisoned(actual) && actual != parameter.type) {
                            error("TYPE_MISMATCH", "Reference arguments require identical declared types.", argument.span);
                        }
                        if (!poisoned(actual)) {
                            pending_reference_effects.push_back(
                                PendingReferenceEffect{argument.value.get(), &parameter, argument.span});
                        }
                    } else {
                    const auto actual = check_expr(*argument.value, &parameter.type);
                    any_poison |= poisoned(actual);
                    if (!poisoned(actual) && parameter.type.kind == TypeKind::Class &&
                        !fully_initialized_for_equality(*argument.value, parameter.type)) {
                        error("UNINITIALIZED_ARGUMENT",
                              "Class value arguments must have every field definitely initialized.",
                              argument.span);
                    }
                }
            }

            for (std::size_t i = 0; i < count; ++i) {
                if (!filled[i] && !function.parameters[i + offset].default_value) {
                    error("ARGUMENT_MISMATCH",
                          "Missing required argument '" +
                              function.parameters[i + offset].name + "'.",
                          expression.span);
                }
            }
            if (!any_poison && in_constructor_) {
                for (const auto& path : function.receiver_effect.required) {
                    if (path.empty() || current_receiver_effect_.initializes.contains(path)) continue;
                    error("UNINITIALIZED",
                          "Method '" + name + "' reads field '" + path +
                              "', which the constructor has not initialized yet.",
                          expression.span);
                }
            }
            if (!any_poison) {
                finish_call_effects(
                    function, pending_reference_effects,
                    nullptr, true, expression.span);
            }
            type = any_poison ? simple(TypeKind::Invalid) : function.result;
            if (!any_poison && type.kind == TypeKind::Class) {
                class_expr_initialized_paths_[&expression] = function.return_initialized_fields;
            }
        } else if (const auto target = builtin_scalar_type(name);
                   target && is_numeric(*target)) {
            if (node->args.size() != 1 || node->args[0].writable ||
                (node->args[0].name && *node->args[0].name != "value")) {
                error("ARGUMENT_MISMATCH", "Numeric cast requires one value argument.", expression.span);
            }
            Type source;
            const auto family = numeric_literal_family(*node->args[0].value);
            const auto direct_family =
                direct_numeric_literal_family(*node->args[0].value);
            const bool target_integer = is_integer_family_type(*target);
            const bool target_floating = is_real(*target);
            const bool same_family =
                (integer_category(family) && target_integer) ||
                (family == NumericLiteralCategory::Real && target_floating);
            const bool direct_cross_family =
                (integer_category(direct_family) && target_floating) ||
                (direct_family == NumericLiteralCategory::Real && target_integer);

            if (family == NumericLiteralCategory::Mixed) {
                error("NUMERIC_FAMILY",
                      "Explicit numeric cast source cannot implicitly mix integer and floating literal families.",
                      node->args[0].span);
            } else if (same_family || direct_cross_family) {
                const bool previous = explicit_numeric_literal_context_;
                explicit_numeric_literal_context_ = true;
                try {
                    source = check_expr(*node->args[0].value, &*target);
                } catch (...) {
                    explicit_numeric_literal_context_ = previous;
                    throw;
                }
                explicit_numeric_literal_context_ = previous;
            } else if (single_family_category(family)) {
                error("NUMERIC_CAST",
                      "Cross-family conversion of a numeric-family expression requires a concrete source type.",
                      node->args[0].span);
            } else {
                source = check_expr(*node->args[0].value);
            }

            std::function<std::optional<Type>(const Type&)> cast_result =
                [&](const Type& current) -> std::optional<Type> {
                    const auto error_type = simple(TypeKind::Error);
                    if (current.kind == TypeKind::Bin) {
                        return is_fixed_integer(*target) ? std::optional<Type>{*target} : std::nullopt;
                    }
                    if (is_numeric(current)) {
                        if (!explicit_numeric_cast_supported(current, *target)) return std::nullopt;
                        if (numeric_conversion_policy(current, *target) ==
                            NumericConversionPolicy::ExplicitRangeCheck) {
                            return Type::union_of({*target, error_type});
                        }
                        return *target;
                    }
                    if (current.kind == TypeKind::Array && current.first) {
                        const auto child = cast_result(*current.first);
                        if (!child || current.first->kind == TypeKind::Bin) return std::nullopt;
                        if (child->kind == TypeKind::Union && child->union_name.empty() &&
                            case_index(*child, error_type) >= 0) {
                            std::optional<Type> success_child;
                            for (const auto& candidate : child->cases) {
                                if (candidate.kind == TypeKind::Error) continue;
                                if (success_child) return std::nullopt;
                                success_child = candidate;
                            }
                            if (!success_child) return std::nullopt;
                            return Type::union_of(
                                {Type::array(*success_child, current.length), error_type});
                        }
                        return Type::array(*child, current.length);
                    }
                    if (current.kind == TypeKind::Tensor && current.first &&
                        is_tensor_numeric(*current.first) && is_tensor_numeric(*target) &&
                        explicit_numeric_cast_supported(*current.first, *target)) {
                        const auto converted =
                            Type::tensor(*target, current.length,
                                         current.tensor_shape_prefix,
                                         current.tensor_known_shape_prefix);
                        if (numeric_conversion_policy(*current.first, *target) ==
                            NumericConversionPolicy::ExplicitRangeCheck) {
                            return Type::union_of({converted, error_type});
                        }
                        return converted;
                    }
                    return std::nullopt;
                };

            if (poisoned(source)) {
                type = source;
                call_resolutions_[&expression] =
                    CallResolution{CallKind::NumericCast, name, std::nullopt, source};
            } else {
                const auto result = cast_result(source);
                if (!result) {
                    Type leaf = source;
                    while (leaf.kind == TypeKind::Array && leaf.first) leaf = *leaf.first;
                    if (leaf.kind == TypeKind::Tensor &&
                        leaf.first) leaf = *leaf.first;
                    const std::string detail = is_fixed_real(leaf) && is_integer_family_type(*target)
                        ? " Floating-point to integer conversion requires math.trunc, math.round, math.floor, or math.ceil."
                        : "";
                    error("NUMERIC_CAST",
                          "Explicit cast from " + type_name(source) +
                          " to element type " + type_name(*target) +
                          " is not supported." + detail,
                          expression.span);
                    type = simple(TypeKind::Invalid);
                } else {
                    type = *result;
                }
                call_resolutions_[&expression] =
                    CallResolution{CallKind::NumericCast, name, std::nullopt, type};
            }
        } else if (name == "bool") {
            if (node->args.size() != 1 || node->args[0].writable || node->args[0].name) {
                error("ARGUMENT_MISMATCH", "bool conversion requires one positional bin value.", expression.span);
            }
            auto source = check_expr(*node->args[0].value);
            if (!poisoned(source) && source.kind != TypeKind::Bin) {
                error("TYPE_MISMATCH", "bool(value) accepts bin only.", expression.span);
            }
            type = poisoned(source) ? source : simple(TypeKind::Bool);
            call_resolutions_[&expression] =
                CallResolution{CallKind::NumericCast, name, std::nullopt, type};
        } else if (name == "bin") {
            if (node->args.size() != 1 || node->args[0].writable || node->args[0].name) {
                error("ARGUMENT_MISMATCH",
                      "bin(value) is an explicit conversion; use bin.fill(n, bit) to allocate raw bits.",
                      expression.span);
            }
            const auto source = check_expr(*node->args[0].value);
            bool valid = is_fixed_integer(source) || source.kind == TypeKind::Bool;
            if (source.kind == TypeKind::Array && source.first)
                valid = is_fixed_integer(*source.first) || source.first->kind == TypeKind::Bool;
            if (!poisoned(source) && !valid) {
                error("TYPE_MISMATCH",
                      "bin(value) accepts an integer, bool, or a flat integer/bool array.",
                      node->args[0].span);
            }
            type = poisoned(source) ? simple(TypeKind::Invalid) : simple(TypeKind::Bin);
            call_resolutions_[&expression] =
                CallResolution{CallKind::Constructor, "bin.cast", std::nullopt, type};
        } else if (name == "string") {
            error("ARGUMENT_MISMATCH",
                  "string is not a constructor; use string.repeat(value, n) for repetition.",
                  expression.span);
        } else if (name.size() > 2 && name.ends_with("[]")) {
            const auto element_name = name.substr(0, name.size() - 2);
            const auto element = builtin_scalar_type(element_name);
            if (!element || (!is_fixed_integer(*element) && element->kind != TypeKind::Bool)) {
                error("TYPE_MISMATCH",
                      "Only integer[] and bool[] explicit conversions are supported here.",
                      expression.span);
            }
            if (node->args.size() != 1 || node->args[0].writable || node->args[0].name) {
                error("ARGUMENT_MISMATCH",
                      "Array conversion requires one positional bin value.", expression.span);
            }
            const auto source = check_expr(*node->args[0].value);
            if (!poisoned(source) && source.kind != TypeKind::Bin) {
                error("TYPE_MISMATCH",
                      "T[](value) binary conversion accepts bin only.", expression.span);
            }
            type = poisoned(source) || !element
                ? simple(TypeKind::Invalid)
                : Type::array(*element);
            call_resolutions_[&expression] =
                CallResolution{CallKind::NumericCast, name, std::nullopt, type};
        } else if (classes_.contains(name) && name.front() != '$' &&
                   !standard_library_class(name)) {
            type = check_class_construction(expression, *node, name);
        } else if (classes_.contains(name)) {
            // Standard-library value types keep their language-provided
            // construction forms, such as map.Map<K, V>(); compiler-generated
            // records such as the cli argument class are built the same way.
            call_resolutions_[&expression] =
                CallResolution{CallKind::Constructor, name, std::nullopt, Type::class_type(name)};
            if (standard_library_class(name) && name.front() == '$') {
                    error("ARGUMENT_MISMATCH",
                          "Standard library value types cannot be constructed directly.",
                          expression.span);
                }
                std::unordered_set<std::string> supplied;
                std::unordered_set<std::string> initialized_paths;
                bool any_poison = false;
                for (const auto& argument : node->args) {
                    if (!argument.name || argument.writable) {
                        error("ARGUMENT_MISMATCH", "Class fields must use named initialization.", argument.span);
                    }
                    const auto* field = find_field(name, *argument.name);
                    if (!field || !supplied.insert(*argument.name).second) {
                        error("ARGUMENT_MISMATCH", "Unknown or duplicate class field.", argument.span);
                    }
                    any_poison |= poisoned(check_expr(*argument.value, &field->type));
                    initialized_paths.insert(field->name);
                    if (field->type.kind == TypeKind::Class) {
                        for (const auto& nested : initialized_paths_for_expr(*argument.value)) {
                            initialized_paths.insert(field->name + "." + nested);
                        }
                    }
                }
                for (const auto& field : classes_.at(name).fields) {
                    if (supplied.contains(field.name) || !field.default_value) continue;
                    initialized_paths.insert(field.name);
                    if (field.type.kind == TypeKind::Class) {
                        for (const auto& nested : initialized_paths_for_expr(*field.default_value)) {
                            initialized_paths.insert(field.name + "." + nested);
                        }
                    }
                }
                for (const auto& field : classes_.at(name).fields) {
                    if (field.is_const && !initialized_paths.contains(field.name)) {
                        error("CONST_INITIALIZATION",
                              "const field '" + field.name + "' must be initialized during construction.",
                              expression.span);
                    }
                }
                type = any_poison ? simple(TypeKind::Invalid) : Type::class_type(name);
                if (!any_poison) class_expr_initialized_paths_[&expression] = std::move(initialized_paths);
        } else if (name == "error") {
            call_resolutions_[&expression] =
                CallResolution{CallKind::Constructor, "error", std::nullopt, simple(TypeKind::Error)};
            if (node->args.size() != 1) {
                error("ARGUMENT_MISMATCH", "error requires one message argument.", expression.span);
            }
            auto required = simple(TypeKind::String);
            auto argument = builtin_arg(0, "message", &required);
            type = poisoned(argument) ? simple(TypeKind::Invalid) : simple(TypeKind::Error);
        } else if (const auto builtin = builtin_callable(name)) {
            type = check_builtin_call_expr(expression, *node, *builtin, expected);
        } else {
            const auto function_name = visible_function_name(name);
            if (!function_name || (!name.empty() && name.front() == '$')) {
                error("UNKNOWN_NAME", "Unknown function '" + name + "'.", expression.span);
            }
            const auto& function = functions_.at(*function_name);
            call_resolutions_[&expression] =
                CallResolution{CallKind::Function, *function_name, std::nullopt, function.result};
            std::vector<PendingReferenceEffect> pending_reference_effects;
            const bool any_poison =
                check_call_arguments(expression, node->args, function, pending_reference_effects);
            if (!any_poison) {
                finish_call_effects(function, pending_reference_effects);
            }
            type = any_poison ? simple(TypeKind::Invalid) : function.result;
            if (!any_poison && type.kind == TypeKind::Class) {
                class_expr_initialized_paths_[&expression] = function.return_initialized_fields;
            }
        }

    return type;
}

std::unordered_set<std::string> Checker::default_initialized_paths(const std::string& class_name) const {
    std::unordered_set<std::string> paths;
    const auto it = classes_.find(class_name);
    if (it == classes_.end()) return paths;
    for (const auto& field : it->second.fields) {
        if (!field.default_value) continue;
        paths.insert(field.name);
        if (field.type.kind == TypeKind::Class) {
            // A class-typed default must be fully initialized (checked where
            // defaults are checked), so every nested path is established.
            for (const auto& nested : complete_class_paths(field.type)) {
                paths.insert(field.name + "." + nested);
            }
        }
    }
    return paths;
}

// Prepares the flow state for checking one class member body. Returns the
// member's signature, or nullptr when the member was not registered (an
// earlier error). Constructors start with the receiver's default-initialized
// fields and no receiver parameter.
FunctionType* Checker::begin_member_body(const ClassDecl& class_decl, const FunctionDecl& method) {
    const auto internal_it = method_internal_names_.find(&method);
    if (internal_it == method_internal_names_.end() ||
        !functions_.contains(internal_it->second)) {
        return nullptr;
    }
    variables_.clear();
    reference_roots_.clear();
    reference_paths_.clear();
    unknown_reference_targets_.clear();
    initialized_.clear();
    const_bindings_.clear();
    const_integer_values_.clear();
    class_initialized_paths_.clear();
    reset_current_effect_state();
    current_module_namespace_ = class_decl.module_namespace;

    auto& signature = functions_.at(internal_it->second);
    for (std::size_t i = method.is_constructor ? 0 : 1; i < signature.parameters.size(); ++i) {
        const auto& parameter = signature.parameters[i];
        variables_[parameter.name] = parameter.type;
        if (parameter.is_const) const_bindings_.insert(parameter.name);
        if (parameter.writable) {
            current_reference_parameters_.insert(parameter.name);
            if (parameter.is_const) current_reference_effects_[parameter.name].required.insert("");
        } else {
            initialized_.insert(parameter.name);
            if (parameter.type.kind == TypeKind::Class) {
                class_initialized_paths_[parameter.name] = complete_class_paths(parameter.type);
            }
        }
    }
    current_return_ = signature.result;
    current_class_ = class_decl.name;
    in_function_ = true;
    in_constructor_ = method.is_constructor;
    constructor_block_depth_ = 0;
    if (method.is_constructor) {
        current_receiver_effect_.initializes = default_initialized_paths(class_decl.name);
    }
    return &signature;
}

void Checker::finish_member_body(const ClassDecl& class_decl, const FunctionDecl& method,
                                 FunctionType& signature, bool report) {
    const bool falls_through = !block_always_terminates(method.body);
    signature.no_normal_return = !block_contains_return(method.body) && !falls_through;
    finalize_receiver_effects(signature, falls_through);
    finalize_reference_effects(signature, falls_through);
    if (!method.is_constructor) {
        signature.return_initialized_fields =
            signature.result.kind == TypeKind::Class && current_return_summary_seen_
                ? current_return_initialized_
                : std::unordered_set<std::string>{};
        return;
    }

    // A constructor's result is its receiver, so the fields it definitely
    // initializes on every normal exit are the fields a caller may read.
    const auto required = signature.receiver_effect.required;
    signature.receiver_effect.required.clear();
    signature.return_initialized_fields = signature.receiver_effect.initializes;
    in_constructor_ = false;
    if (!report) return;

    const auto& guaranteed = signature.return_initialized_fields;
    for (const auto& path : required) {
        if (path.empty()) continue;
        try {
            error("UNINITIALIZED",
                  "Constructor of '" + class_decl.name + "' reads field '" + path +
                      "' before initializing it.",
                  method.span);
        } catch (const CompileError& compile_error) {
            record(compile_error);
        }
    }
    for (const auto& field : classes_.at(class_decl.name).fields) {
        if (guaranteed.contains(field.name)) continue;
        if (field.is_const) {
            try {
                error("CONST_INITIALIZATION",
                      "const field '" + field.name + "' must be initialized by the constructor of '" +
                          class_decl.name + "'.",
                      method.span);
            } catch (const CompileError& compile_error) {
                record(compile_error);
            }
        } else if (signature.result.kind == TypeKind::Union) {
            try {
                error("UNINITIALIZED_UNION_PAYLOAD",
                      "A constructor that can fail completes with every field initialized; '" +
                          field.name + "' may be uninitialized when this one completes.",
                      method.span);
            } catch (const CompileError& compile_error) {
                record(compile_error);
            }
        }
    }
    if (signature.result.kind == TypeKind::Union) {
        // Nested class fields must be complete too: the value enters a union.
        const auto complete = complete_class_paths(Type::class_type(class_decl.name));
        for (const auto& path : complete) {
            if (guaranteed.contains(path)) continue;
            const auto dot = path.find('.');
            if (dot == std::string::npos) continue;  // reported above
            if (!guaranteed.contains(path.substr(0, dot))) continue;  // reported above
            try {
                error("UNINITIALIZED_UNION_PAYLOAD",
                      "A constructor that can fail completes with every field initialized; '" +
                          path + "' may be uninitialized when this one completes.",
                      method.span);
            } catch (const CompileError& compile_error) {
                record(compile_error);
            }
            break;
        }
    }
}

// `T(...)` for a user class always calls the class's sole construct(...)
// member. Argument count, names, reference form, and defaults are checked
// against that one signature; alternative call forms belong in default
// parameters rather than constructor overloads.
Type Checker::check_class_construction(const Expr& expression, const CallExpr& node,
                                       const std::string& class_name) {
    const auto& info = classes_.at(class_name);
    if (info.constructors.empty()) {
        error("NO_CONSTRUCTOR",
              "Class '" + class_name + "' declares no constructor. Declare '" + class_name +
                  " value' and assign its fields, or add construct(...) to the class.",
              expression.span);
    }

    auto chosen = info.constructors.front();
    if (!node.constructor.empty()) {
        const auto instance = info.constructor_instances.find(node.constructor);
        if (instance == info.constructor_instances.end()) {
            error("NO_CONSTRUCTOR",
                  "Class '" + class_name + "' has no constructor instance for this call.",
                  expression.span);
        }
        chosen = instance->second;
    }
    const auto describe = [&](const std::string& internal) {
        std::string text = "construct(";
        const auto& candidate = functions_.at(internal);
        for (std::size_t i = 0; i < candidate.parameters.size(); ++i) {
            const auto& parameter = candidate.parameters[i];
            if (i) text += ", ";
            if (parameter.is_const) text += "const ";
            text += type_name(parameter.type);
            text += parameter.writable ? " &" : " ";
            text += parameter.name;
            if (parameter.default_value) text += " = ...";
        }
        return text + ")";
    };

    if (info.private_constructors.contains(chosen) && current_class_ != class_name) {
        error("PRIVATE_MEMBER",
              "Constructor " + describe(chosen) + " of '" + class_name +
                  "' is private and only callable inside the class.",
              expression.span);
    }

    const auto& function = functions_.at(chosen);
    call_resolutions_[&expression] =
        CallResolution{CallKind::Function, chosen, std::nullopt, function.result};
    std::vector<PendingReferenceEffect> pending;
    const bool any_poison = check_call_arguments(expression, node.args, function, pending);
    if (!any_poison) finish_call_effects(function, pending);
    if (any_poison) return simple(TypeKind::Invalid);
    class_expr_initialized_paths_[&expression] = function.return_initialized_fields;
    return function.result;
}

// Binds a call's arguments to a signature's parameters by position and name,
// checks each argument against its parameter, and collects the reference
// effects to apply once the call is accepted. Shared by function calls and
// constructor calls, whose signatures carry no receiver parameter.
bool Checker::check_call_arguments(const Expr& expression, const std::vector<CallArg>& args,
                                   const FunctionType& function,
                                   std::vector<PendingReferenceEffect>& pending) {
            std::vector<bool> filled(function.parameters.size());
            std::size_t positional = 0;
            bool any_poison = false;

            for (const auto& argument : args) {
                std::size_t target = 0;
                if (argument.name) {
                    const auto it = std::find_if(
                        function.parameters.begin(), function.parameters.end(),
                        [&](const auto& parameter) { return parameter.name == *argument.name; });
                    if (it == function.parameters.end()) {
                        error("ARGUMENT_MISMATCH",
                              "Unknown argument '" + *argument.name + "'.",
                              argument.span);
                    }
                    target = static_cast<std::size_t>(it - function.parameters.begin());
                } else {
                    if (positional >= filled.size()) {
                        error("ARGUMENT_MISMATCH", "Too many positional arguments.", argument.span);
                    }
                    target = positional++;
                }
                if (filled[target]) {
                    error("ARGUMENT_MISMATCH",
                          "Argument '" + function.parameters[target].name +
                              "' is supplied more than once.",
                          argument.span);
                }
                filled[target] = true;
                const auto& parameter = function.parameters[target];
                if (argument.writable != parameter.writable) {
                    error("WRITE_CAPABILITY", "Argument reference form must match parameter.", argument.span);
                }
                if (argument.writable) {
                        const bool readonly_reference = parameter.is_const;
                        const bool valid_storage = readonly_reference
                            ? stable_addressable_storage(*argument.value)
                            : stable_writable_storage(*argument.value);
                        if (!valid_storage) {
                            reject_bare_storage_root(*argument.value);
                            error("WRITE_CAPABILITY",
                                  readonly_reference
                                      ? "Readonly reference arguments require stable addressable storage."
                                      : "Writable reference arguments require storage with write authority.",
                                  argument.span);
                        }
                        auto actual = check_address_target(*argument.value);
                        any_poison |= poisoned(actual);
                        if (!readonly_reference) {
                            if (const auto path = writable_storage_path(*argument.value)) {
                            if (narrowed_.contains(path->first) || borrowed_.contains(path->first)) {
                                error("WRITE_CAPABILITY",
                                      "Writable reference arguments require unnarrowed, unborrowed storage.",
                                      argument.span);
                            }
                            }
                        }
                        if (!poisoned(actual) && actual != parameter.type) {
                            error("TYPE_MISMATCH", "Reference arguments require identical declared types.", argument.span);
                        }
                        if (!poisoned(actual)) {
                            pending.push_back(
                                PendingReferenceEffect{argument.value.get(), &parameter, argument.span});
                        }
                    } else {
                    const auto actual = check_expr(*argument.value, &parameter.type);
                    any_poison |= poisoned(actual);
                    if (!poisoned(actual) && parameter.type.kind == TypeKind::Class &&
                        !fully_initialized_for_equality(*argument.value, parameter.type)) {
                        error("UNINITIALIZED_ARGUMENT",
                              "Class value arguments must have every field definitely initialized.",
                              argument.span);
                    }
                }
            }

            for (std::size_t i = 0; i < filled.size(); ++i) {
                if (!filled[i] && !function.parameters[i].default_value) {
                    error("ARGUMENT_MISMATCH",
                          "Missing required argument '" + function.parameters[i].name + "'.",
                          expression.span);
                }
            }
    return any_poison;
}

namespace {

// The flow state a path through an if-expression can change, saved before
// a branch and merged after the branches exactly as check_if_stmt merges its
// blocks. The receiver's requirements are not saved: they accumulate over
// every path, as they do for an if statement.
struct BranchFlow {
    std::unordered_map<std::string, Type> variables;
    ReferenceRoots reference_roots;
    ReferencePaths reference_paths;
    std::unordered_set<std::string> unknown_reference_targets;
    std::unordered_set<std::string> const_bindings;
    std::unordered_set<std::string> initialized;
    std::unordered_map<std::string, std::unordered_set<std::string>> class_initialized_paths;
    std::unordered_set<std::string> receiver_initializes;
    std::unordered_set<std::string> receiver_writes;
    std::unordered_set<std::string> receiver_invalidates;
    std::unordered_map<std::string, StorageEffect> reference_effects;
    std::unordered_set<std::string> maybe_initialized{};

    const std::unordered_set<std::string>& paths_of(const std::string& name) const {
        static const std::unordered_set<std::string> none;
        const auto found = class_initialized_paths.find(name);
        return found == class_initialized_paths.end() ? none : found->second;
    }
};

} // namespace

// An if-expression (language.md, "If expressions"). Every condition is bool.
// With a contextual type each branch is checked against it as an initializer
// is; without one the typed branches have one identical type, into which
// the numeric literal branches materialize. Initialization and effects flow
// as through an if statement: each condition on the path that reaches it,
// each branch on its own path, merged after the expression.
Type Checker::check_if_expr(const Expr& expression, const IfExpr& node, const Type* expected) {
    const auto bool_type = simple(TypeKind::Bool);
    const bool contextual = expected != nullptr;

    const auto save = [&]() {
        return BranchFlow{variables_, reference_roots_, reference_paths_,
                          unknown_reference_targets_, const_bindings_, initialized_,
                          class_initialized_paths_, current_receiver_effect_.initializes,
                          current_receiver_effect_.writes,
                          current_receiver_effect_.invalidates, current_reference_effects_,
                          maybe_initialized_};
    };
    const auto restore = [&](const BranchFlow& state) {
        variables_ = state.variables;
        reference_roots_ = state.reference_roots;
        reference_paths_ = state.reference_paths;
        unknown_reference_targets_ = state.unknown_reference_targets;
        const_bindings_ = state.const_bindings;
        initialized_ = state.initialized;
        maybe_initialized_ = state.maybe_initialized;
        class_initialized_paths_ = state.class_initialized_paths;
        current_receiver_effect_.initializes = state.receiver_initializes;
        current_receiver_effect_.writes = state.receiver_writes;
        current_receiver_effect_.invalidates = state.receiver_invalidates;
        current_reference_effects_ = state.reference_effects;
    };
    // The state after a two-way choice from `before`, as check_if_stmt
    // computes it after its two blocks.
    const auto merge = [&](const BranchFlow& before, const BranchFlow& yes, bool yes_terminates,
                           const BranchFlow& no, bool no_terminates) {
        restore(before);
        for (const auto& [name, base_type] : before.variables) {
            if (base_type.kind == TypeKind::Tensor) {
                std::vector<Type> continuing_types;
                if (!yes_terminates) {
                    if (const auto it = yes.variables.find(name); it != yes.variables.end()) {
                        continuing_types.push_back(it->second);
                    }
                }
                if (!no_terminates) {
                    if (const auto it = no.variables.find(name); it != no.variables.end()) {
                        continuing_types.push_back(it->second);
                    }
                }
                if (!continuing_types.empty()) {
                    variables_[name] = merge_shaped_flow_facts(base_type, continuing_types);
                }
            }
            if ((yes_terminates || yes.initialized.contains(name)) &&
                (no_terminates || no.initialized.contains(name))) {
                initialized_.insert(name);
            } else if (before.reference_paths.contains(name)) {
                initialized_.erase(name);
            }
            if (base_type.kind == TypeKind::Class) {
                std::unordered_set<std::string> merged;
                const auto& yp = yes.paths_of(name);
                const auto& np = no.paths_of(name);
                const bool yes_unset = !yes.initialized.contains(name) &&
                                       !yes.maybe_initialized.contains(name);
                const bool no_unset = !no.initialized.contains(name) &&
                                      !no.maybe_initialized.contains(name);
                if (yes_terminates || (yes_unset && !no_terminates && !no_unset)) merged = np;
                else if (no_terminates || (no_unset && !yes_unset)) merged = yp;
                else {
                    for (const auto& path : yp) {
                        if (np.contains(path)) merged.insert(path);
                    }
                }
                class_initialized_paths_[name] = std::move(merged);
            }
        }
        if (yes_terminates) current_receiver_effect_.initializes = no.receiver_initializes;
        else if (no_terminates) current_receiver_effect_.initializes = yes.receiver_initializes;
        else {
            current_receiver_effect_.initializes.clear();
            for (const auto& field : yes.receiver_initializes) {
                if (no.receiver_initializes.contains(field)) {
                    current_receiver_effect_.initializes.insert(field);
                }
            }
        }
        current_receiver_effect_.writes = before.receiver_writes;
        current_receiver_effect_.writes.insert(yes.receiver_writes.begin(), yes.receiver_writes.end());
        current_receiver_effect_.writes.insert(no.receiver_writes.begin(), no.receiver_writes.end());
        current_receiver_effect_.invalidates = yes.receiver_invalidates;
        current_receiver_effect_.invalidates.insert(no.receiver_invalidates.begin(),
                                                    no.receiver_invalidates.end());
        current_reference_effects_ =
            merge_reference_branches(before.reference_effects, yes.reference_effects,
                                     no.reference_effects, yes_terminates, no_terminates);
        std::vector<ReferenceTargetState> continuing_targets;
        if (!yes_terminates) {
            continuing_targets.push_back({yes.reference_paths, yes.unknown_reference_targets});
        }
        if (!no_terminates) {
            continuing_targets.push_back({no.reference_paths, no.unknown_reference_targets});
        }
        auto joined = join_reference_targets(before.reference_roots, before.reference_paths,
                                             before.unknown_reference_targets, continuing_targets);
        reference_roots_ = std::move(joined.roots);
        reference_paths_ = std::move(joined.paths);
        unknown_reference_targets_ = std::move(joined.unknown);
        for (const auto& name : joined.ambiguous) {
            initialized_.erase(name);
            class_initialized_paths_.erase(name);
        }
        std::vector<std::pair<const std::unordered_set<std::string>*,
                              const std::unordered_set<std::string>*>> continuing;
        if (!yes_terminates) continuing.emplace_back(&yes.initialized, &yes.maybe_initialized);
        if (!no_terminates) continuing.emplace_back(&no.initialized, &no.maybe_initialized);
        join_maybe_initialized(continuing);
    };

    const auto check_condition = [&](const Expr& condition) {
        try {
            (void)check_expr(condition, &bool_type);
        } catch (const CompileError& failure) {
            const auto& diagnostic = failure.diagnostic();
            const auto raw = raw_types_.find(&condition);
            if (diagnostic.code == "TYPE_MISMATCH" &&
                diagnostic.span.start.offset == condition.span.start.offset &&
                diagnostic.span.end.offset == condition.span.end.offset &&
                raw != raw_types_.end() && raw->second.kind != TypeKind::Bool) {
                error("TYPE_MISMATCH", "An if-expression condition must be bool.", condition.span);
            }
            throw;
        }
    };

    std::optional<Type> branch_type;
    std::vector<const Expr*> literal_branches;
    std::optional<std::unordered_set<std::string>> class_paths;
    bool poisoned_branch = false;
    // Checks one branch on the current path; true when it never continues.
    const auto check_branch = [&](const Expr& value) -> bool {
        if (const auto* unary = std::get_if<UnaryExpr>(&value.data); unary && unary->op == "&") {
            error("IF_EXPRESSION", if_expression_reference_message, value.span);
        }
        // Without a contextual type a numeric literal branch takes the type
        // of the typed branches, so it is checked after them; it has no
        // effect on the flow. A nested if-expression is always typed here.
        if (!contextual && !is_if_expression(value) &&
            numeric_literal_family(value) != NumericLiteralCategory::None) {
            literal_branches.push_back(&value);
            return false;
        }
        const auto type = contextual ? check_expr(value, expected) : check_expr(value);
        const bool terminates = expr_has_no_normal_return(value);
        if (poisoned(type)) {
            poisoned_branch = true;
            return terminates;
        }
        if (terminates || type.kind == TypeKind::Never) return true;
        if (!contextual) {
            if (!branch_type) {
                branch_type = type;
            } else if (*branch_type != type) {
                error("TYPE_MISMATCH",
                      "The branches of an if-expression have different types: '" +
                          type_name(*branch_type) + "' and '" + type_name(type) + "'.",
                      value.span);
            }
        }
        if (type.kind == TypeKind::Class) {
            const auto paths = initialized_paths_for_expr(value);
            if (!class_paths) {
                class_paths = paths;
            } else {
                std::unordered_set<std::string> common;
                for (const auto& path : *class_paths) {
                    if (paths.contains(path)) common.insert(path);
                }
                class_paths = std::move(common);
            }
        }
        return false;
    };

    // The choices nest to the right: condition i leads to value i or to the
    // rest of the chain. Each branch is checked on its own path, then the
    // paths are merged from the last choice back to the first.
    std::vector<BranchFlow> before_states;
    std::vector<BranchFlow> yes_states;
    std::vector<bool> yes_terminates;
    for (std::size_t index = 0; index < node.conditions.size(); ++index) {
        check_condition(*node.conditions[index]);
        before_states.push_back(save());
        yes_terminates.push_back(check_branch(*node.values[index]));
        yes_states.push_back(save());
        restore(before_states.back());
    }
    bool rest_terminates = check_branch(*node.otherwise);
    for (std::size_t index = node.conditions.size(); index-- > 0;) {
        const auto rest = save();
        merge(before_states[index], yes_states[index], yes_terminates[index], rest,
              rest_terminates);
        rest_terminates = yes_terminates[index] && rest_terminates;
    }

    if (poisoned_branch) return simple(TypeKind::Invalid);
    Type type = simple(TypeKind::Never);
    if (contextual) {
        type = *expected;
    } else if (branch_type) {
        type = *branch_type;
    } else if (!literal_branches.empty()) {
        error("AMBIGUOUS_NUMERIC_LITERAL",
              "An if-expression whose branches are numeric literals needs a type from its context.",
              expression.span);
    }
    for (const auto* value : literal_branches) (void)check_expr(*value, &type);
    if (type.kind == TypeKind::Void) {
        error("TYPE_MISMATCH", "An if-expression gives a value; its branches cannot be void.",
              expression.span);
    }
    if (type.kind == TypeKind::Range) {
        error("RANGE_CONTEXT", "range is only a for iterable.", expression.span);
    }
    if (type.kind == TypeKind::Class && class_paths) {
        class_expr_initialized_paths_[&expression] = std::move(*class_paths);
    }
    return type;
}

Type Checker::check_expr(const Expr& expression, const Type* expected) {
    nesting::DepthGuard guard(
        expr_depth_, nesting::max_expression_depth, expression.span, "Expression");
    std::optional<Type> contextual_default;
    if (!expected && expression.contextual_default_type &&
        expression.contextual_default_type->name != "auto") {
        contextual_default = resolve_type(*expression.contextual_default_type);
        expected = &*contextual_default;
    }
    Type type = simple(TypeKind::Void);
    // A call is where the bindings its effects initialize become initialized
    // (L13): its `&` arguments and receiver, and scan targets.
    struct CallScope {
        const Expr*& current;
        const Expr* saved;
        ~CallScope() { current = saved; }
    } call_scope{current_call_expression_, current_call_expression_};
    if (std::holds_alternative<CallExpr>(expression.data) ||
        std::holds_alternative<MethodCallExpr>(expression.data)) {
        current_call_expression_ = &expression;
    }
    // A writable argument names storage, which `_` never names and an
    // if-expression, a value, does not give.
    const auto check_writable_arguments = [&](const std::vector<CallArg>& args) {
        for (const auto& argument : args) {
            if (!argument.writable) continue;
            if (const auto* name = std::get_if<NameExpr>(&argument.value->data);
                name && is_discard_name(name->name)) {
                error("DISCARD", discard_read_message, argument.value->span);
            }
            if (is_if_expression(*argument.value)) {
                error("IF_EXPRESSION", if_expression_reference_message, argument.value->span);
            }
        }
    };

    if (const auto* node = std::get_if<IntegerExpr>(&expression.data)) {
        const auto context =
            numeric_literal_context(expected, NumericLiteralCategory::NonNegativeInteger);
        if (context.ambiguous) {
            error("AMBIGUOUS_NUMERIC_LITERAL",
                  "Integer-family literal matches multiple concrete integer types.",
                  expression.span);
        }
        if (context.type) {
            if (!is_bare_integer(*context.type) &&
                (!node->fits_u64 || !integer_literal_value_fits(
                    static_cast<unsigned long long>(node->value), *context.type))) {
                error(explicit_numeric_literal_context_ ? "NUMERIC_CAST" : "INTEGER_RANGE",
                      "Integer-family literal is outside the range of " +
                          type_name(*context.type) + ".",
                      expression.span);
            }
            type = *context.type;
        } else if (explicit_numeric_literal_context_ && expected &&
                   is_real(*expected)) {
            const auto spelling =
                node->spelling.empty() ? std::to_string(node->value) : node->spelling;
            if (is_fixed_real(*expected) &&
                !constant_eval::real_literal_value(spelling, *expected)) {
                error("NUMERIC_CAST",
                      "Integer-family literal is outside the finite range of " +
                          type_name(*expected) + ".",
                      expression.span);
            }
            type = *expected;
        } else if (context.opposite_family) {
            const auto spelling =
                node->spelling.empty() ? std::to_string(node->value) : node->spelling;
            const auto target = type_name(literal_target(*expected, is_real));
            error("NUMERIC_FAMILY",
                  "An integer literal cannot materialize as " + target + ": write " + spelling +
                      ".0 or " + target + "(" + spelling + ").",
                  expression.span);
        } else if (expected) {
            error("TYPE_MISMATCH",
                  "Integer-family literal requires a concrete integer type context.",
                  expression.span);
        } else {
            error("AMBIGUOUS_NUMERIC_LITERAL",
                  "Integer-family literal requires a unique concrete integer type context.",
                  expression.span);
        }
    } else if (const auto* node = std::get_if<RealLiteralExpr>(&expression.data)) {
        const auto context =
            numeric_literal_context(expected, NumericLiteralCategory::Real);
        if (context.ambiguous) {
            error("AMBIGUOUS_NUMERIC_LITERAL",
                  "Real-family literal matches multiple concrete real types.",
                  expression.span);
        }
        if (context.type) {
            if (context.type->kind != TypeKind::Real &&
                !constant_eval::real_literal_value(node->spelling, *context.type)) {
                error(explicit_numeric_literal_context_ ? "NUMERIC_CAST" : "REAL_RANGE",
                      "Real-family literal is outside the finite range of " +
                          type_name(*context.type) + ".",
                      expression.span);
            }
            type = *context.type;
        } else if (explicit_numeric_literal_context_ && expected &&
                   is_integer_family_type(*expected)) {
            error("NUMERIC_CAST",
                  "Floating-point to integer conversion requires math.trunc, math.round, math.floor, or math.ceil.",
                  expression.span);
        } else if (context.opposite_family) {
            error("NUMERIC_FAMILY",
                  "A real literal cannot materialize as " +
                      type_name(literal_target(*expected, is_integer_family_type)) + ".",
                  expression.span);
        } else if (expected) {
            error("TYPE_MISMATCH",
                  "Real-family literal requires a concrete real type context.",
                  expression.span);
        } else {
            error("AMBIGUOUS_NUMERIC_LITERAL",
                  "Real-family literal requires a unique concrete real type context.",
                  expression.span);
        }
    } else if (const auto* node = std::get_if<ImaginaryLiteralExpr>(&expression.data)) {
        // Imaginary literals materialize only as complex types, which do not
        // exist yet; the integer form names the real form to write.
        if (node->integer_form) {
            error("NUMERIC_FAMILY",
                  "An imaginary literal requires real form: write " + node->spelling + ".0i.",
                  expression.span);
        } else if (expected) {
            error("NUMERIC_FAMILY",
                  "An imaginary literal cannot materialize as " + type_name(*expected) + ".",
                  expression.span);
        } else {
            error("AMBIGUOUS_NUMERIC_LITERAL",
                  "An imaginary literal requires a complex type context.", expression.span);
        }
    } else if (std::holds_alternative<StringExpr>(expression.data)) {
        type = simple(TypeKind::String);
    } else if (std::holds_alternative<BoolExpr>(expression.data)) {
        type = simple(TypeKind::Bool);
    } else if (std::holds_alternative<VoidExpr>(expression.data)) {
        type = simple(TypeKind::Void);
    } else if (std::holds_alternative<NoneExpr>(expression.data)) {
        type = simple(TypeKind::None);
    } else if (const auto* node = std::get_if<StringTemplateExpr>(&expression.data)) {
        for (std::size_t index = 0; index < node->expressions.size(); ++index) {
            const auto& item = node->expressions[index];
            const auto item_type = check_expr(*item);
            if (!printable(item_type))
                error("TYPE_MISMATCH", "Interpolation requires scalar values.", item->span);
            if (node->formats[index].active() && !poisoned(item_type) && !is_numeric(item_type))
                error("TYPE_MISMATCH", "Interpolation formatting requires a numeric value.", item->span);
        }
        type = simple(TypeKind::String);
    } else if (const auto* node = std::get_if<NameExpr>(&expression.data)) {
        type = check_name_expr(expression, *node, expected);
    } else if (const auto* node = std::get_if<MemberExpr>(&expression.data)) {
        type = check_member_expr(expression, *node);
    } else if (const auto* node = std::get_if<ArrayExpr>(&expression.data)) {
        const Type* context = expected && expected->kind == TypeKind::Array ? expected : nullptr;
        if (!context && expected && expected->kind == TypeKind::Union) {
            for (const auto& candidate : expected->cases) {
                if (candidate.kind == TypeKind::Array) {
                    if (context) {
                        error("AMBIGUOUS_TYPE", "Array literal matches multiple union array cases.", expression.span);
                    }
                    context = &candidate;
                }
            }
        }
        if (node->elements.empty()) {
            if (!context) {
                error("AMBIGUOUS_TYPE", "Empty array needs an explicit array type.", expression.span);
            }
            type = Type::array(*context->first, 0);
        } else {
            Type element;
            if (context) {
                element = check_expr(*node->elements[0], context->first.get());
                if (!poisoned(element)) {
                    for (std::size_t i = 1; i < node->elements.size(); ++i) {
                        check_expr(*node->elements[i], &element);
                    }
                }
            } else {
                std::optional<std::size_t> seed;
                for (std::size_t i = 0; i < node->elements.size(); ++i) {
                    const auto family = numeric_literal_family(*node->elements[i]);
                    if (family == NumericLiteralCategory::Mixed) {
                        error("NUMERIC_FAMILY",
                              "Numeric-family expression cannot mix integer- and real-family literals implicitly.",
                              node->elements[i]->span);
                    }
                    if (family == NumericLiteralCategory::None) {
                        seed = i;
                        break;
                    }
                }
                if (!seed) {
                    error("AMBIGUOUS_NUMERIC_LITERAL",
                          "An array made only of numeric-family expressions needs an explicit element type.",
                          expression.span);
                }
                element = check_expr(*node->elements[*seed]);
                if (!poisoned(element)) {
                    for (std::size_t i = 0; i < node->elements.size(); ++i) {
                        if (i == *seed) continue;
                        check_expr(*node->elements[i], &element);
                    }
                }
            }
            if (poisoned(element)) {
                type = element;
            } else {
                if (!is_storable(element)) {
                    error("TYPE_MISMATCH", "Array elements must be storable.", expression.span);
                }
                if (element.kind == TypeKind::Class) {
                    for (const auto& item : node->elements) {
                        if (!fully_initialized_for_equality(*item, element)) {
                            error("UNINITIALIZED_ARGUMENT",
                                  "Class values stored in arrays must have every field definitely initialized.",
                                  item->span);
                        }
                    }
                }
                type = Type::array(element, static_cast<long long>(node->elements.size()));
            }
        }
        if (context && !poisoned(type)) {
            if (!assignable(type, *context)) {
                error("ARRAY_SHAPE", "Array literal does not match declared shape.", expression.span);
            }
            type = *context;
        }
    } else if (const auto* node = std::get_if<IndexExpr>(&expression.data)) {
        type = check_index_expr(expression, *node);
    } else if (const auto* node = std::get_if<UnaryExpr>(&expression.data)) {
        if (node->op == "scan&") {
            error("SCAN_TARGET",
                  "'{&name}' names an input target and is only valid inside a scan format.",
                  expression.span);
        }
        if (node->op == "&") {
            (void)check_address_target(*node->operand, false);
            type = simple(TypeKind::Address);
        } else {
        std::optional<Type> literal_expected;
        const Type* operand_expected = nullptr;
        bool materialized_signed_minimum = false;
        if (node->op == "NOT") {
            const auto family = numeric_literal_family(*node->operand);
            if (family == NumericLiteralCategory::Real || family == NumericLiteralCategory::Mixed ||
                complex_category(family)) {
                error("TYPE_MISMATCH", "NOT requires a fixed-width integer.", expression.span);
            }
            const auto context = numeric_literal_context(expected, family);
            if (context.ambiguous) {
                error("AMBIGUOUS_NUMERIC_LITERAL",
                      "Numeric-family literal matches multiple concrete types.",
                      expression.span);
            }
            if (context.type) {
                literal_expected = *context.type;
                operand_expected = &*literal_expected;
            }
        } else if (node->op == "-" && expected &&
            (is_numeric(*expected) || expected->kind == TypeKind::Tensor)) {
            operand_expected = expected;
            if (const auto* literal = std::get_if<IntegerExpr>(&node->operand->data);
                literal && is_fixed_integer(*expected) && is_signed_integer(*expected) &&
                !integer_literal_value_fits(
                    static_cast<unsigned long long>(literal->value), *expected) &&
                negative_integer_literal_value_fits(
                    static_cast<unsigned long long>(literal->value), *expected)) {
                // The sign is part of the source expression's value. Signed minima
                // such as int8(-128) must not reject the magnitude 128 before '-'
                // is applied.
                type = *expected;
                raw_types_[node->operand.get()] = *expected;
                expr_types_[node->operand.get()] = *expected;
                materialized_signed_minimum = true;
            }
        } else if (node->op == "-" || node->op == "NOT") {
            const auto family = numeric_literal_family(*node->operand);
            if (family == NumericLiteralCategory::Mixed) {
                error("NUMERIC_FAMILY",
                      "Numeric-family expression cannot mix integer- and real-family literals implicitly.",
                      expression.span);
            }
            const auto context = numeric_literal_context(expected, family);
            if (context.ambiguous) {
                error("AMBIGUOUS_NUMERIC_LITERAL",
                      "Numeric-family literal matches multiple concrete types.",
                      expression.span);
            }
            if (context.type) {
                literal_expected = *context.type;
                operand_expected = &*literal_expected;
            }
        }
        if (!materialized_signed_minimum) {
            type = check_expr(*node->operand, operand_expected);
        }
        if (!poisoned(type)) {
            if (node->op == "not") {
                if (type.kind != TypeKind::Bool) {
                    error("TYPE_MISMATCH", "not requires bool.", expression.span);
                }
            } else if (node->op == "NOT") {
                if (!is_fixed_integer(type)) {
                    error("TYPE_MISMATCH", "NOT requires a fixed-width integer.", expression.span);
                }
            } else if (type.kind == TypeKind::Tensor) {
                if (!type.first || !is_numeric(*type.first) ||
                    (is_fixed_integer(*type.first) && !is_signed_integer(*type.first))) {
                    error("TYPE_MISMATCH",
                          "Tensor negation requires a signed numeric tensor.",
                          expression.span);
                }
            } else if (!is_numeric(type)) {
                error("TYPE_MISMATCH", "Negation requires a number.", expression.span);
            } else if ((type.kind == TypeKind::Nat ||
                        (is_fixed_integer(type) && !is_signed_integer(type))) &&
                       integer_category(direct_numeric_literal_family(*node->operand))) {
                // A negative integer literal (`-0` included) materializes
                // only as a signed integer type.
                error("NUMERIC_FAMILY",
                      "A negative integer literal cannot materialize as " + type_name(type) + ".",
                      expression.span);
            } else if ((is_fixed_integer(type) && !is_signed_integer(type)) ||
                       type.kind == TypeKind::Nat) {
                error("TYPE_MISMATCH", "Negation requires a signed numeric type.", expression.span);
            }
        }
        }
    } else if (const auto* node = std::get_if<BinaryExpr>(&expression.data)) {
        const auto left_family = numeric_literal_family(*node->left);
        const auto right_family = numeric_literal_family(*node->right);
        const auto joined_family =
            numeric_policy::join_numeric_literal_categories(left_family, right_family);
        if (left_family == NumericLiteralCategory::Mixed ||
            right_family == NumericLiteralCategory::Mixed) {
            error("NUMERIC_FAMILY",
                  "Numeric-family expression cannot mix integer- and real-family literals implicitly.",
                  expression.span);
        } else if ((integer_category(left_family) && complex_category(right_family)) ||
                   (complex_category(left_family) && integer_category(right_family))) {
            error("NUMERIC_FAMILY",
                  "Complex literal parts require real form: write 1.0 + 2.0i.",
                  expression.span);
        }

        Type left;
        Type right;
        const auto scalar_context = [](const Type& other) -> const Type* {
            if (is_numeric(other)) return &other;
            if (other.kind == TypeKind::Tensor &&
                other.first && is_numeric(*other.first)) {
                return other.first.get();
            }
            return nullptr;
        };

        const bool left_literal = single_family_category(left_family);
        const bool right_literal = single_family_category(right_family);

        if (joined_family == NumericLiteralCategory::Complex &&
            operator_policy::participates_in_numeric_literal_family(node->op)) {
            // A real and an imaginary literal joined: a complex literal,
            // which materializes only as a complex type (none exists yet).
            if (expected) {
                error("NUMERIC_FAMILY",
                      "A complex literal cannot materialize as " + type_name(*expected) + ".",
                      expression.span);
            } else {
                error("AMBIGUOUS_NUMERIC_LITERAL",
                      "A complex literal requires a complex type context.", expression.span);
            }
        } else if (left_literal && right_literal) {
            if (integer_category(left_family) != integer_category(right_family)) {
                error("NUMERIC_FAMILY",
                      "Integer- and real-family expressions cannot mix implicitly.",
                      expression.span);
            }
            const auto context = numeric_literal_context(expected, joined_family);
            if (context.ambiguous) {
                error("AMBIGUOUS_NUMERIC_LITERAL",
                      "Numeric-family expression matches multiple concrete numeric types.",
                      expression.span);
            }
            if (!context.type && context.natural_target) {
                error("NUMERIC_FAMILY",
                      "A negative integer literal cannot materialize as " +
                          type_name(*context.natural_target) + ".",
                      expression.span);
            } else if (!context.type) {
                if (context.opposite_family) {
                    error("NUMERIC_FAMILY",
                          "Numeric-family expression cannot cross families without an explicit cast.",
                          expression.span);
                }
                error("AMBIGUOUS_NUMERIC_LITERAL",
                      "Numeric-family expression requires a unique concrete numeric type context.",
                      expression.span);
            }
            left = check_expr(*node->left, &*context.type);
            right = check_expr(*node->right, &*context.type);
        } else if (left_literal) {
            right = check_expr(*node->right);
            left = check_expr(*node->left, scalar_context(right));
        } else if (right_literal) {
            left = check_expr(*node->left);
            right = check_expr(*node->right, scalar_context(left));
        } else {
            left = check_expr(*node->left);
            right = check_expr(*node->right);
        }

        if (poisoned(left) || poisoned(right)) {
            type = simple(TypeKind::Invalid);
        } else if (left.kind == TypeKind::Tensor || right.kind == TypeKind::Tensor) {
            const bool left_tensor = left.kind == TypeKind::Tensor;
            const bool right_tensor = right.kind == TypeKind::Tensor;
            const bool comparison =
                node->op == "==" || node->op == "!=" || node->op == "<" ||
                node->op == "<=" || node->op == ">" || node->op == ">=";
            const Type& tensor_type = left_tensor ? left : right;
            const auto element = *tensor_type.first;
            Type tensor_result = tensor_type;
            if (comparison) {
                if (left_tensor && right_tensor) {
                    if (*left.first != *right.first) {
                        error("TYPE_MISMATCH",
                              "Tensor comparison requires identical element types.", expression.span);
                    }
                    if (left.length >= 0 && right.length >= 0 && left.length != right.length) {
                        error("TENSOR_SHAPE",
                              "Tensor comparison requires identical rank.", expression.span);
                    }
                    const auto known = std::min(
                        left.tensor_known_shape_prefix.size(),
                        right.tensor_known_shape_prefix.size());
                    for (std::size_t axis = 0; axis < known; ++axis) {
                        if (left.tensor_known_shape_prefix[axis] !=
                            right.tensor_known_shape_prefix[axis]) {
                            error("TENSOR_SHAPE",
                                  "Tensor comparison requires identical shape.",
                                  expression.span);
                        }
                    }
                } else {
                    const Type& scalar = left_tensor ? right : left;
                    if (scalar != element) {
                        error("TYPE_MISMATCH",
                              "Tensor-scalar comparison requires the exact tensor element type.",
                              expression.span);
                    }
                }
                type = Type::tensor(simple(TypeKind::Bool), tensor_type.length,
                                    tensor_type.tensor_shape_prefix,
                                    tensor_type.tensor_known_shape_prefix);
            } else {
                if (!is_tensor_numeric(element)) {
                    error("TYPE_MISMATCH",
                          "Tensor arithmetic requires a numeric element type; tensor<bool> is a mask type.",
                          expression.span);
                }
                if (left_tensor && right_tensor) {
                    if (*left.first != *right.first) {
                        error("TYPE_MISMATCH",
                              "Tensor arithmetic requires identical element types.", expression.span);
                    }
                    if (left.length >= 0 && right.length >= 0 && left.length != right.length) {
                        error("TYPE_MISMATCH",
                              "Tensor arithmetic requires identical rank when both ranks are statically known.",
                              expression.span);
                    }
                    if (tensor_result.length < 0) {
                        tensor_result.length = left.length >= 0 ? left.length : right.length;
                    }
                    apply_broadcast_shape(left, right, tensor_result);
                } else {
                    const auto& scalar_type = left_tensor ? right : left;
                    if (!is_tensor_numeric(scalar_type) || scalar_type != element) {
                        error("TYPE_MISMATCH",
                              "Tensor scalar arithmetic requires the exact element type.",
                              expression.span);
                    }
                }
                if (node->op != "+" && node->op != "-" && node->op != "*" &&
                    node->op != "/" && node->op != "%" && node->op != "^") {
                    error("TYPE_MISMATCH",
                          "Tensor operators are +, -, *, /, %, ^, and all-element comparisons.",
                          expression.span);
                }
                if (node->op == "%" && !is_fixed_integer(element)) {
                    error("TYPE_MISMATCH", "Tensor remainder requires an integer element type.",
                          expression.span);
                }
                if (node->op == "^") {
                    if (!left_tensor || right_tensor) {
                        error("TYPE_MISMATCH",
                              "Tensor exponentiation is defined as tensor ^ scalar.",
                              expression.span);
                    } else if (is_fixed_integer(element)) {
                        const auto exponent = constant_eval::integer(
                            *node->right, &const_integer_values_);
                        if (exponent && *exponent < 0) {
                            error("POWER_DOMAIN",
                                  "Integer tensor exponentiation requires a non-negative exponent.",
                                  node->right->span);
                        }
                    }
                }
                type = tensor_result;
            }
        } else {
            if (left != right) {
                // A nat operand with another integer kind names the
                // conversion to write.
                if (left.kind == TypeKind::Nat)
                    if (const auto message = nat_mismatch_message(*node->left, left, right))
                        error("TYPE_MISMATCH", *message, node->left->span);
                if (right.kind == TypeKind::Nat)
                    if (const auto message = nat_mismatch_message(*node->right, right, left))
                        error("TYPE_MISMATCH", *message, node->right->span);
                error("TYPE_MISMATCH", "Operands must have identical types.", expression.span);
            }
            if (node->op == "and" || node->op == "or") {
                if (left.kind != TypeKind::Bool) {
                    error("TYPE_MISMATCH", "Logical operands must be bool.", expression.span);
                }
                type = left;
            } else if (operator_policy::is_fixed_width_bitwise_binary(node->op)) {
                if (!is_fixed_integer(left)) {
                    error("TYPE_MISMATCH",
                          "Bitwise operations require nat8..nat64 or int8..int64.",
                          expression.span);
                }
                if (operator_policy::is_shift(node->op)) {
                    const auto count = constant_eval::integer(*node->right);
                    if (count && (*count < 0 ||
                                  *count >= static_cast<long long>(integer_width(left)))) {
                        error("SHIFT_COUNT",
                              "Shift count must be between zero and one less than the integer width.",
                              node->right->span);
                    }
                }
                type = left;
            } else if (node->op == "==" || node->op == "!=") {
                if (left.kind != TypeKind::Address && !equality_supported(left)) {
                    error("TYPE_MISMATCH", "Equality is not defined for this type.", expression.span);
                }
                if (left.kind == TypeKind::Class &&
                    (!fully_initialized_for_equality(*node->left, left) ||
                     !fully_initialized_for_equality(*node->right, right))) {
                    error("UNINITIALIZED_FIELD_EQUALITY",
                          "Class equality requires every compared field to be definitely initialized.",
                          expression.span);
                }
                if (left.kind == TypeKind::Array) {
                    // Equality reads every element.
                    check_element_read(*node->left, nullptr, expression.span);
                    check_element_read(*node->right, nullptr, expression.span);
                }
                type = simple(TypeKind::Bool);
            } else if (node->op == "<" || node->op == "<=" || node->op == ">" || node->op == ">=") {
                if (!is_numeric(left)) {
                    error("TYPE_MISMATCH", "Ordering requires numbers.", expression.span);
                }
                type = simple(TypeKind::Bool);
            } else if (node->op == "+" && left.kind == TypeKind::String) {
                type = left;
            } else {
                if (!is_numeric(left) ||
                    (node->op == "%" && !is_integer_family_type(left))) {
                    error("TYPE_MISMATCH", "Arithmetic requires compatible numbers.", expression.span);
                }
                if (node->op == "^" && is_integer_family_type(left)) {
                    const auto exponent = constant_eval::integer(*node->right);
                    if (exponent && *exponent < 0) {
                        error("POWER_DOMAIN",
                              "Integer exponent must be non-negative.",
                              node->right->span);
                    } else if (exponent && *exponent == 0) {
                        const auto base = constant_eval::integer(*node->left);
                        if (base && *base == 0)
                            error("POWER_DOMAIN", "0 ^ 0 is undefined.", expression.span);
                    }
                }
                if (node->op == "^" && is_real(left)) {
                    // Literal operands decide the undefined powers at
                    // compile time: a zero base with a zero or negative
                    // exponent, and a negative base with an exponent that is
                    // not an integer.
                    const auto base = literal_number_value(*node->left);
                    const auto exponent = literal_number_value(*node->right);
                    if (base && exponent) {
                        if (*base == 0.0 && *exponent == 0.0)
                            error("POWER_DOMAIN", "0 ^ 0 is undefined.", expression.span);
                        else if (*base == 0.0 && *exponent < 0.0)
                            error("POWER_DOMAIN", "0 ^ a negative exponent is undefined.",
                                  expression.span);
                        else if (*base < 0.0 && std::trunc(*exponent) != *exponent)
                            error("POWER_DOMAIN",
                                  "A negative base requires an integer exponent.",
                                  expression.span);
                    }
                }
                if (is_integer_family_type(left) && (node->op == "/" || node->op == "%")) {
                    const auto divisor = constant_eval::integer(*node->right);
                    if (divisor && *divisor == 0) {
                        error("DIVIDE_BY_ZERO",
                              node->op == "/" ? "Integer division by zero is known at compile time."
                                              : "Integer remainder by zero is known at compile time.",
                              node->right->span);
                    }
                }
                if (left_literal && right_literal && integer_category(joined_family) &&
                    (is_fixed_integer(left) || left.kind == TypeKind::Nat)) {
                    // A compound literal expression computes in the type its
                    // literals materialize as, with checked arithmetic: an
                    // operation whose known operands leave that type's range
                    // is a compile-time error (nat8 x = 3 - 5).
                    using State = constant_eval::IntegerInType::State;
                    if (constant_eval::integer_in_type(*node->left, left).state == State::Known &&
                        constant_eval::integer_in_type(*node->right, left).state == State::Known &&
                        constant_eval::integer_in_type(expression, left).state == State::OutOfRange) {
                        error("INTEGER_RANGE",
                              "This integer literal expression is outside the range of " +
                                  type_name(left) + ".",
                              expression.span);
                    }
                }
                type = left;
            }
        }
    } else if (const auto* node = std::get_if<TryExpr>(&expression.data)) {
        const auto error_type = simple(TypeKind::Error);
        Type source;
        error_terminating_expr_ = node->value.get();
        if (expected) {
            const auto operand_expected = Type::union_of({*expected, error_type});
            source = check_expr(*node->value, &operand_expected);
        } else {
            source = check_expr(*node->value);
        }
        if (poisoned(source)) {
            type = source;
        } else {
            if (source.kind != TypeKind::Union || case_index(source, error_type) < 0) {
                error("TYPE_MISMATCH", "try requires a union containing error.", expression.span);
            }
            if (!in_function_ || !assignable(error_type, current_return_)) {
                error("TRY_CONTEXT", "Enclosing return type must accept error.", expression.span);
            }
            auto cases = source.cases;
            cases.erase(std::remove(cases.begin(), cases.end(), error_type), cases.end());
            type = Type::union_of(std::move(cases));
            if (type.kind == TypeKind::Class) {
                // A class can enter a union only after the checker has proved every
                // field initialized. Removing error with try preserves that fact.
                class_expr_initialized_paths_[&expression] = complete_class_paths(type);
            }
        }
    } else if (const auto* node = std::get_if<MethodCallExpr>(&expression.data)) {
        check_writable_arguments(node->args);
        type = check_method_call_expr(expression, *node, expected);
    } else if (const auto* node = std::get_if<CallExpr>(&expression.data)) {
        check_writable_arguments(node->args);
        type = check_call_expr(expression, *node, expected);
    } else if (const auto* node = std::get_if<IfExpr>(&expression.data)) {
        type = check_if_expr(expression, *node, expected);
    }

    raw_types_[&expression] = type;
    if (expected && type.kind != TypeKind::Never && type.kind != TypeKind::Invalid) {
        if (!assignable(type, *expected) &&
            type.kind == TypeKind::Union && type.union_name.empty()) {
            const auto error_type = simple(TypeKind::Error);
            if (case_index(type, error_type) >= 0) {
                std::vector<Type> non_error;
                non_error.reserve(type.cases.size());
                for (const auto& current : type.cases) {
                    if (current.kind != TypeKind::Error) non_error.push_back(current);
                }
                if (!non_error.empty()) {
                    const auto residual = Type::union_of(std::move(non_error));
                    if (assignable(residual, *expected)) {
                        fail_fast_expressions_.insert(&expression);
                        type = residual;
                        if (type.kind == TypeKind::Class) {
                            class_expr_initialized_paths_[&expression] =
                                complete_class_paths(type);
                        }
                    }
                }
            }
        }
        if (!assignable(type, *expected)) {
            if (const auto message =
                    nat_mismatch_message(expression, raw_types_.at(&expression), *expected)) {
                error("TYPE_MISMATCH", *message, expression.span);
            } else {
                error("TYPE_MISMATCH",
                      "Expected " + type_name(*expected) + " but received " +
                          type_name(raw_types_.at(&expression)) + ".",
                      expression.span);
            }
        }
        if (expected->kind == TypeKind::Union && type.kind == TypeKind::Class &&
            !fully_initialized_for_equality(expression, type)) {
            error("UNINITIALIZED_UNION_PAYLOAD",
                  "Class values must be fully initialized before conversion into a union.",
                  expression.span);
        }
        type = *expected;
    }
    expr_types_[&expression] = type;
    return type;
}
void Checker::check_binding_stmt(const Stmt& statement, const BindingStmt& node) {
        if (is_discard_name(node.name)) error("DISCARD", discard_typed_message, statement.span);
        if (node.name.starts_with(abi::symbol_namespace::internal_prefix))
            error("SHADOWING", compiler_name_message(node.name), statement.span);
        if (variables_.contains(node.name) || declaration_name_visible(node.name) ||
            is_reserved_value_name(node.name) || member_name_visible(node.name)) {
            error("SHADOWING", "Name is already visible or reserved.", statement.span);
        }

        const auto exact_type_atom = [](const TypeName& source,
                                              const char* name) {
            return source.name == name && source.arguments.empty() &&
                   source.function_parameters.empty() && source.array_depth == 0 &&
                   source.tensor_shape_prefix.empty() &&
                   source.tensor_shape_expressions.empty();
        };
        const bool infer_preserving_error =
            node.declared_type.name == "union" &&
            node.declared_type.arguments.size() == 2 &&
            exact_type_atom(node.declared_type.arguments[0], "auto") &&
            exact_type_atom(node.declared_type.arguments[1], "error");
        if (infer_preserving_error && node.reference) {
            error("INVALID_AUTO",
                  "auto | error is only valid for initialized value bindings.",
                  statement.span);
        }
        auto type = infer_preserving_error
            ? simple(TypeKind::Auto)
            : resolve_type(node.declared_type, true);

        const auto resolve_extent = [&](const std::shared_ptr<Expr>& expression,
                                        SourceSpan span) -> long long {
            if (!expression) return -1;
            const auto int_type = simple(TypeKind::Int);
            const auto extent_type =
                numeric_literal_family(*expression) != NumericLiteralCategory::None
                    ? check_expr(*expression, &int_type) : check_expr(*expression);
            if (!poisoned(extent_type) && !is_integer_family_type(extent_type)) {
                error("INVALID_TYPE", "Array/tensor extents require integer expressions.", span);
            }
            if (const auto known =
                    constant_eval::integer(*expression, &const_integer_values_)) {
                if (*known < 0) {
                    error("INVALID_TYPE", "Array/tensor extents cannot be negative.", span);
                }
                return *known;
            }
            return -2;
        };

        if (type.kind == TypeKind::Tensor) {
            for (std::size_t axis = 0;
                 axis < node.declared_type.tensor_shape_expressions.size(); ++axis) {
                if (axis >= type.tensor_shape_prefix.size()) break;
                const auto& expression =
                    node.declared_type.tensor_shape_expressions[axis];
                if (!expression) {
                    type.tensor_shape_prefix[axis] = -1;
                    continue;
                }
                type.tensor_shape_prefix[axis] =
                    resolve_extent(expression, expression->span);
            }
        }

        Type* array_axis = &type;
        for (std::size_t axis = 0;
             axis < node.declared_type.dimension_expressions.size() &&
             array_axis && array_axis->kind == TypeKind::Array;
             ++axis) {
            const auto& expression =
                node.declared_type.dimension_expressions[axis];
            array_axis->length = expression
                ? resolve_extent(expression, expression->span)
                : -1;
            array_axis = array_axis->first.get();
        }

        if (node.is_const && !node.value) {
            error("CONST_INITIALIZATION", "const bindings require an initializer.", statement.span);
        }

        if (node.reference) {
            if (node.value && is_if_expression(*node.value)) {
                error("IF_EXPRESSION", if_expression_reference_message, node.value->span);
            }
            if (!node.value || !node.reference_initializer) {
                error("REFERENCE_BINDING", "Reference bindings require '= &storage'.", statement.span);
            }
            const bool addressable = node.is_const
                ? stable_addressable_storage(*node.value)
                : stable_writable_storage(*node.value);
            if (!addressable) {
                error("REFERENCE_BINDING",
                      node.is_const
                          ? "A readonly reference requires stable addressable storage."
                          : "A writable reference requires storage with write authority.",
                      node.value->span);
            }
            auto actual = check_address_target(*node.value);
            if (type.kind == TypeKind::Auto) type = actual;
            else if (!poisoned(actual) && actual != type) {
                error("TYPE_MISMATCH", "Reference bindings require identical types.", statement.span);
            }
            if (!is_storable(type)) {
                error("INVALID_TYPE", "Reference binding requires a storable type.", statement.span);
            }
            variables_[node.name] = type;
            binding_declarations_.erase(node.name);
            maybe_initialized_.erase(node.name);
            untracked_initialized_.erase(node.name);
            declare_array_elements(node.name, type, node);
            if (node.is_const) {
                if (!storage_initialized(*node.value)) {
                    error("UNINITIALIZED", "Readonly references require initialized storage.", node.value->span);
                }
                const_bindings_.insert(node.name);
            }
            if (unknown_reference_access_path(*node.value)) {
                reference_roots_[node.name] = node.name;
                reference_paths_[node.name] = {node.name, ""};
                unknown_reference_targets_.insert(node.name);
            } else if (const auto* source = binding_name(*node.value);
                       source && variables_.contains(source->name)) {
                reference_roots_[node.name] = reference_root(source->name);
                if (const auto existing = reference_paths_.find(source->name); existing != reference_paths_.end()) {
                    reference_paths_[node.name] = existing->second;
                } else {
                    reference_paths_[node.name] = {reference_root(source->name), ""};
                }
                unknown_reference_targets_.erase(node.name);
            } else if (const auto path = member_storage_path(*node.value)) {
                reference_roots_[node.name] = path->first;
                reference_paths_[node.name] = *path;
                unknown_reference_targets_.erase(node.name);
            } else {
                reference_roots_[node.name] = node.name;
                reference_paths_[node.name] = {node.name, ""};
                unknown_reference_targets_.insert(node.name);
            }
            binding_types_[&statement] = type;
            if (storage_initialized(*node.value)) initialized_.insert(node.name);
            if (type.kind == TypeKind::Class) {
                class_initialized_paths_[node.name] = initialized_paths_for_expr(*node.value);
            }
            return;
        }

        if (node.reference_initializer) {
            error("REFERENCE_BINDING", "'&' on an initializer requires a reference binding.", statement.span);
        }

        if (type.kind == TypeKind::Auto) {
            if (!node.value) {
                error("INVALID_AUTO",
                      infer_preserving_error
                          ? "auto | error requires an initializer."
                          : "auto requires an initializer.",
                      statement.span);
            }
            type = check_expr(*node.value);

            const auto error_type = simple(TypeKind::Error);
            const bool has_error =
                type.kind == TypeKind::Union && type.union_name.empty() &&
                case_index(type, error_type) >= 0;

            if (infer_preserving_error) {
                if (!has_error) {
                    error("INVALID_AUTO",
                          "auto | error requires an initializer whose static type contains error.",
                          statement.span);
                }
            } else if (has_error) {
                std::vector<Type> success_cases;
                success_cases.reserve(type.cases.size());
                for (const auto& current : type.cases) {
                    if (current.kind != TypeKind::Error) {
                        success_cases.push_back(current);
                    }
                }
                if (success_cases.empty()) {
                    error("INVALID_AUTO",
                          "auto cannot infer a success type from an error-only initializer.",
                          statement.span);
                }
                type = Type::union_of(std::move(success_cases));
                fail_fast_expressions_.insert(node.value.get());
                expr_types_[node.value.get()] = type;
                if (type.kind == TypeKind::Class) {
                    class_expr_initialized_paths_[node.value.get()] =
                        complete_class_paths(type);
                }
            }

            if (type.kind == TypeKind::Array &&
                std::holds_alternative<ArrayExpr>(node.value->data)) {
                // Array literals infer a runtime-sized array so common auto bindings remain
                // appendable. Other expressions preserve their declared static shape contract.
                type.length = -1;
                expr_types_[node.value.get()] = type;
            }
        } else if (node.value) {
            check_expr(*node.value, &type);
            if (type.kind == TypeKind::Tensor) {
                const auto raw = raw_types_.find(node.value.get());
                if (raw != raw_types_.end() && raw->second.kind == type.kind) {
                    type.length = raw->second.length;
                    type.tensor_known_shape_prefix =
                        raw->second.tensor_known_shape_prefix;
                }
            }
        }
        if (!is_storable(type)) {
            error("INVALID_TYPE", "Binding requires a storable type.", statement.span);
        }
        variables_[node.name] = type;
        binding_declarations_[node.name] = &statement;
        maybe_initialized_.erase(node.name);
        untracked_initialized_.erase(node.name);
        declare_array_elements(node.name, type, node);
        if (node.is_const) {
            const_bindings_.insert(node.name);
            if (node.value && is_fixed_integer(type)) {
                if (const auto known =
                        constant_eval::integer(*node.value, &const_integer_values_)) {
                    const_integer_values_[node.name] = *known;
                }
            }
        }
        binding_types_[&statement] = type;
        if (node.value) {
            initialized_.insert(node.name);
            const auto paths = initialized_paths_for_expr(*node.value);
            if (!paths.empty()) {
                class_initialized_paths_[node.name] = paths;
            }
            note_class_value(node.name, "", type, paths);
        } else if (type.kind == TypeKind::Class &&
                   class_storage_established_at_declaration(
                       type.class_name, standard_library_class(type.class_name))) {
            // `Point point` creates the value with its declared defaults. Each
            // other field stays uninitialized until assigned, and reads are
            // checked per field like any other class value.
            for (const auto& field : classes_.at(type.class_name).fields) {
                if (field.is_const && !field.default_value) {
                    error("CONST_INITIALIZATION",
                          "const field '" + field.name + "' of '" + type_name(type) +
                              "' has no default, so the value must come from a constructor.",
                          statement.span);
                }
            }
            initialized_.insert(node.name);
            class_initialized_paths_[node.name] = default_initialized_paths(type.class_name);
            // A fresh value whose field stores are all seen from here (L13).
            const auto prefix = node.name + ".";
            for (auto it = maybe_initialized_.begin(); it != maybe_initialized_.end();) {
                if (it->rfind(prefix, 0) == 0) it = maybe_initialized_.erase(it);
                else ++it;
            }
            for (auto it = untracked_initialized_.begin(); it != untracked_initialized_.end();) {
                if (it->rfind(prefix, 0) == 0) it = untracked_initialized_.erase(it);
                else ++it;
            }
        } else if (type.kind == TypeKind::Array &&
                   (type.length >= 0 ||
                    (type.length == -2 &&
                     !node.declared_type.dimension_expressions.empty() &&
                     node.declared_type.dimension_expressions.front()))) {
            // A fixed or captured-extent outer array declaration creates storage
            // immediately. Its elements are tracked independently and may still
            // be uninitialized.
            initialized_.insert(node.name);
        }
        return;
}

void Checker::check_rebind_stmt(const Stmt& statement, const RebindStmt& node) {
        if (is_discard_name(node.name)) error("DISCARD", discard_read_message, statement.span);
        if (is_if_expression(*node.target)) {
            error("IF_EXPRESSION", if_expression_reference_message, node.target->span);
        }
        if (const_bindings_.contains(node.name)) {
            error("WRITE_CAPABILITY", "const references cannot be rebound.", statement.span);
        }
        if (!variables_.contains(node.name) || !reference_roots_.contains(node.name)) {
            error("REFERENCE_BINDING", "Address assignment requires a reference binding on the left.", statement.span);
        }
        if (!stable_writable_storage(*node.target)) {
            error("REFERENCE_BINDING",
                  "A reference address must be rooted in an existing binding or receiver storage.",
                  node.target->span);
        }
        auto actual = check_address_target(*node.target);
        const auto expected = variables_.at(node.name);
        if (!poisoned(actual) && actual != expected) {
            error("TYPE_MISMATCH", "Address assignment requires identical referenced types.", statement.span);
        }
        if (unknown_reference_access_path(*node.target)) {
            reference_roots_[node.name] = node.name;
            reference_paths_[node.name] = {node.name, ""};
            unknown_reference_targets_.insert(node.name);
        } else if (const auto* source = binding_name(*node.target);
                   source && variables_.contains(source->name)) {
            reference_roots_[node.name] = reference_root(source->name);
            if (const auto existing = reference_paths_.find(source->name); existing != reference_paths_.end()) {
                reference_paths_[node.name] = existing->second;
            } else {
                reference_paths_[node.name] = {reference_root(source->name), ""};
            }
            unknown_reference_targets_.erase(node.name);
        } else if (const auto path = member_storage_path(*node.target)) {
            reference_roots_[node.name] = path->first;
            reference_paths_[node.name] = *path;
            unknown_reference_targets_.erase(node.name);
        } else {
            reference_roots_[node.name] = node.name;
            reference_paths_[node.name] = {node.name, ""};
            unknown_reference_targets_.insert(node.name);
        }
        if (storage_initialized(*node.target)) initialized_.insert(node.name);
        else initialized_.erase(node.name);
        if (expected.kind == TypeKind::Class) {
            class_initialized_paths_[node.name] = initialized_paths_for_expr(*node.target);
        }
        return;
}

void Checker::check_assign_stmt(const Stmt& statement, const AssignStmt& node) {
        if (const auto* name = std::get_if<NameExpr>(&node.target->data);
            name && is_discard_name(name->name)) {
            error("DISCARD", discard_read_message, node.target->span);
        }
        if (const_access_path(*node.target)) {
            // A constructor assigns each const field of its own class once, by a
            // statement directly in its body, so the value is fixed by the time
            // the constructor completes.
            bool constructor_const_initialization = false;
            if (in_constructor_) {
                if (const auto* name = std::get_if<NameExpr>(&node.target->data)) {
                    if (const auto* field = receiver_field(*node.target);
                        field && field->is_const) {
                        if (constructor_block_depth_ != 1) {
                            error("CONST_INITIALIZATION",
                                  "const field '" + name->name +
                                      "' must be assigned by a statement directly in the constructor body, not inside a branch or loop.",
                                  statement.span);
                        }
                        if (current_receiver_effect_.initializes.contains(name->name)) {
                            error("CONST_INITIALIZATION",
                                  "const field '" + name->name +
                                      "' is already initialized; a const field is assigned once.",
                                  statement.span);
                        }
                        if (!node.compound_op.empty()) {
                            error("CONST_INITIALIZATION",
                                  "const field '" + name->name + "' is initialized by plain assignment.",
                                  statement.span);
                        }
                        constructor_const_initialization = true;
                    }
                }
            }
            if (!constructor_const_initialization) {
                error("WRITE_CAPABILITY", "Cannot write through a const access path.", statement.span);
            }
        }
        if (const auto* indexed = std::get_if<IndexExpr>(&node.target->data)) {
            const auto base_type = check_expr(*indexed->base);
        }
        if (!node.compound_op.empty()) {
            const auto read_type = check_expr(*node.target);
            if (read_type.kind == TypeKind::Tensor &&
                std::holds_alternative<IndexExpr>(node.target->data)) {
                error("WRITE_CAPABILITY",
                      "Tensor element compound assignment is not supported; assign an explicit scalar result.",
                      statement.span);
            }
            if (!poisoned(read_type)) {
                const bool valid =
                    (node.compound_op == "+" &&
                     (is_numeric(read_type) || read_type.kind == TypeKind::String)) ||
                    ((node.compound_op == "-" || node.compound_op == "*" ||
                      node.compound_op == "/") && is_numeric(read_type)) ||
                    (node.compound_op == "%" && is_integer_family_type(read_type));
                if (!valid) {
                    error("TYPE_MISMATCH",
                          "Operator '" + node.compound_op +
                              "=' is not defined for type '" + type_name(read_type) + "'.",
                          statement.span);
                }
            }
        }
        Type type;
        if (auto* name = std::get_if<NameExpr>(&node.target->data)) {
            if (name->this_qualifier) check_this_field(*node.target, *name);
            if (!name->this_qualifier && is_builtin_text_constant(name->name)) {
                error("WRITE_CAPABILITY", "Built-in text constants are immutable.", statement.span);
            }
            if (!name->this_qualifier && variables_.contains(name->name)) {
                const auto root = reference_root(name->name);
                if (narrowed_.contains(root) || borrowed_.contains(root)) {
                    error("WRITE_CAPABILITY",
                          "Cannot replace a binding while narrowed or borrowed by a writable loop.",
                          statement.span);
                }
                type = variables_.at(name->name);
                expr_types_[node.target.get()] = raw_types_[node.target.get()] = type;
                auto expected = type;
                if (expected.kind == TypeKind::Tensor) {
                    if (expected.tensor_shape_prefix.empty()) expected.length = -1;
                    expected.tensor_known_shape_prefix.clear();
                }
                check_expr(*node.value, &expected);
                if (type.kind == TypeKind::Tensor) {
                    const auto raw = raw_types_.find(node.value.get());
                    if (raw != raw_types_.end() && raw->second.kind == type.kind) {
                        auto refined = type;
                        refined.length = raw->second.length;
                        refined.tensor_known_shape_prefix =
                            raw->second.tensor_known_shape_prefix;
                        variables_[name->name] = std::move(refined);
                    }
                }
                if (current_reference_parameters_.contains(name->name)) {
                    record_storage_assignment(
                        current_reference_effects_[name->name], "", type, *node.value);
                } else if (const auto ref = reference_paths_.find(name->name);
                           ref != reference_paths_.end() && !ref->second.second.empty()) {
                    auto& fields = class_initialized_paths_[ref->second.first];
                    const auto path = ref->second.second;
                    fields.insert(path);
                    const auto prefix = path + ".";
                    for (auto it = fields.begin(); it != fields.end();) {
                        if (it->rfind(prefix, 0) == 0) it = fields.erase(it);
                        else ++it;
                    }
                    if (type.kind == TypeKind::Class) {
                        for (const auto& nested : initialized_paths_for_expr(*node.value)) {
                            fields.insert(prefix + nested);
                        }
                    }
                    initialized_.insert(name->name);
                } else {
                    element_tracked_arrays_.erase(root);
                    if (!unknown_reference_targets_.contains(name->name)) {
                        record_initialization(root);
                    } else {
                        // The write may reach any binding (L13).
                        for (const auto& [binding, _] : binding_declarations_) {
                            if (!initialized_.contains(binding)) untracked_initialized_.insert(binding);
                        }
                    }
                    initialized_.insert(root);
                    initialized_.insert(name->name);
                    if (type.kind == TypeKind::Class) {
                        class_initialized_paths_[root] = initialized_paths_for_expr(*node.value);
                        if (root == name->name) class_initialized_paths_[name->name] = class_initialized_paths_[root];
                        note_class_value(root, "", type, class_initialized_paths_[root]);
                    }
                }
            } else if (const auto* field = receiver_field(*node.target)) {
                if (field->is_private && current_class_ != field->owner) {
                    error("PRIVATE_MEMBER",
                          "Private field '" + name->name + "' is only accessible inside class '" +
                              field->owner + "'.",
                          node.target->span);
                }
                type = field->type;
                field_accesses_[node.target.get()] = FieldAccessInfo{field->owner, field->index, field->type};
                expr_types_[node.target.get()] = raw_types_[node.target.get()] = type;
                check_expr(*node.value, &type);
                record_current_receiver_assignment(name->name, type, *node.value);
                note_field_store("$this", name->name);
                note_class_value("$this", name->name, type, initialized_paths_for_expr(*node.value));
                current_receiver_effect_.initializes.insert(name->name);
                const auto prefix = name->name + ".";
                for (auto it = current_receiver_effect_.initializes.begin();
                     it != current_receiver_effect_.initializes.end();) {
                    if (it->rfind(prefix, 0) == 0) it = current_receiver_effect_.initializes.erase(it);
                    else ++it;
                }
                if (type.kind == TypeKind::Class) {
                    for (const auto& nested : initialized_paths_for_expr(*node.value)) {
                        current_receiver_effect_.initializes.insert(prefix + nested);
                    }
                }
            } else {
                reject_bare_field(name->name, node.target->span);
                error("UNKNOWN_NAME", "Undefined assignment target.", statement.span);
            }
        } else if (std::holds_alternative<IndexExpr>(node.target->data) ||
                   std::holds_alternative<MemberExpr>(node.target->data)) {
            if (!stable_writable_storage(*node.target)) {
                error("WRITE_CAPABILITY",
                      "Assignment storage must be rooted in an existing binding or receiver storage.",
                      node.target->span);
            }
            if (const auto* indexed = std::get_if<IndexExpr>(&node.target->data)) {
                const auto base_type = raw_types_.contains(indexed->base.get())
                    ? raw_types_.at(indexed->base.get())
                    : check_expr(*indexed->base);
                if (base_type.kind == TypeKind::Bin) {
                    if (indexed->items.size() != 1 || indexed->items.front().slice ||
                        !indexed->items.front().index) {
                        error("INDEX_ARITY",
                              "bin assignment requires exactly one integer index.",
                              node.target->span);
                    }
                    check_index_operand(*indexed->items.front().index);
                    type = simple(TypeKind::Bin);
                    raw_types_[node.target.get()] = expr_types_[node.target.get()] = type;
                    check_expr(*node.value, &type);
                } else {
                    element_store_target_ = node.target.get();
                    type = check_address_target(*node.target, true);
                    element_store_target_ = nullptr;
                    const auto value_type = check_expr(*node.value, &type);
                    if (!poisoned(value_type) && type.kind == TypeKind::Class &&
                        !fully_initialized_for_equality(*node.value, type)) {
                        error("UNINITIALIZED_ARGUMENT",
                              "Class values stored in arrays must have every field definitely initialized.",
                              node.value->span);
                    }
                    note_element_store(*node.target);
                }
            } else {
                type = check_address_target(*node.target, false);
                check_expr(*node.value, &type);
            }
            if (const auto receiver_path = current_receiver_path(*node.target)) {
                if (std::holds_alternative<IndexExpr>(node.target->data)) {
                    current_receiver_effect_.writes.insert(*receiver_path);
                } else {
                    record_current_receiver_assignment(*receiver_path, type, *node.value);
                    note_field_store("$this", *receiver_path);
                    note_class_value("$this", *receiver_path, type,
                                     initialized_paths_for_expr(*node.value));
                }
            } else if (const auto reference_path =
                           current_reference_parameter_path(*node.target)) {
                auto& effect = current_reference_effects_[reference_path->first];
                if (std::holds_alternative<IndexExpr>(node.target->data)) {
                    effect.writes.insert(reference_path->second);
                } else {
                    record_storage_assignment(effect, reference_path->second, type, *node.value);
                }
            } else if (std::holds_alternative<MemberExpr>(node.target->data)) {
                const auto clear_descendants = [](std::unordered_set<std::string>& paths,
                                                  const std::string& path) {
                    const auto prefix = path + ".";
                    for (auto it = paths.begin(); it != paths.end();) {
                        if (it->rfind(prefix, 0) == 0) it = paths.erase(it);
                        else ++it;
                    }
                };
                if (const auto path = member_storage_path(*node.target);
                    path && !path->second.empty()) {
                    auto& initialized_paths = class_initialized_paths_[path->first];
                    clear_descendants(initialized_paths, path->second);
                    mark_member_initialized(*node.target);
                    std::unordered_set<std::string> nested_paths;
                    if (type.kind == TypeKind::Class) {
                        nested_paths = initialized_paths_for_expr(*node.value);
                        for (const auto& nested : nested_paths) {
                            initialized_paths.insert(path->second + "." + nested);
                        }
                    }
                    // A store into a field of a value sets its bit (L13); one
                    // through a reference to a field does not.
                    const auto* target_root = place_root(*node.target);
                    if (target_root && reference_paths_.contains(target_root->name) &&
                        !reference_paths_.at(target_root->name).second.empty()) {
                        note_untracked_field_write(path->first, path->second);
                    } else {
                        note_field_store(path->first, path->second);
                    }
                    note_class_value(path->first, path->second, type, nested_paths);
                } else {
                    mark_member_initialized(*node.target);
                }
            }
        } else {
            error("INVALID_ASSIGNMENT", "Expected a binding, field, or array element.", statement.span);
        }
        return;
}

void Checker::check_loop_control_stmt(const Stmt& statement, const LoopControlStmt& node) {
        if (loop_depth_ == 0) {
            error("LOOP_CONTROL_CONTEXT",
                  node.is_continue ? "continue requires an enclosing loop."
                                    : "break requires an enclosing loop.",
                  statement.span);
        }
        return;
}

void Checker::check_return_stmt(const Stmt& statement, const ReturnStmt& node) {
        if (!in_function_) error("RETURN_OUTSIDE_FUNCTION", "return requires a function.", statement.span);
        if (current_return_.kind == TypeKind::Never) {
            error("TYPE_MISMATCH", "never functions cannot return.", statement.span);
        }
        if (in_constructor_) {
            // A constructor completes by returning without a value: the receiver
            // is the result. The only value it may return is an error, and only
            // when it is declared `T | error construct`. An error return produces
            // no value, so it does not count toward the fields the receiver has
            // when construction completes.
            if (std::holds_alternative<VoidExpr>(node.value->data)) {
                record_effect_exit();
                return;
            }
            if (current_return_.kind != TypeKind::Union) {
                error("CONSTRUCTOR_RETURN",
                      "A constructor completes with a bare return; declare it '" +
                          type_name(current_return_) +
                          " | error construct(...)' if it must be able to return an error.",
                      statement.span);
            }
            const auto error_type = simple(TypeKind::Error);
            const auto value = check_expr(*node.value, &error_type);
            if (!poisoned(value) && value.kind != TypeKind::Error) {
                error("CONSTRUCTOR_RETURN",
                      "A constructor completes with a bare return; the receiver is its result, so the only value it can return is an error.",
                      statement.span);
            }
            return;
        }
        check_expr(*node.value, &current_return_);
        if (current_return_.kind == TypeKind::Class) {
            const auto paths = initialized_paths_for_expr(*node.value);
            if (!current_return_summary_seen_) {
                current_return_initialized_ = paths;
                current_return_summary_seen_ = true;
            } else {
                for (auto it = current_return_initialized_.begin();
                     it != current_return_initialized_.end();) {
                    if (!paths.contains(*it)) it = current_return_initialized_.erase(it);
                    else ++it;
                }
            }
        }
        record_effect_exit();
        return;
}

void Checker::check_expression_stmt(const Stmt& statement, const ExprStmt& node) {
        error_terminating_expr_ = node.value.get();
        auto type = check_expr(*node.value);
        error_terminating_expr_ = nullptr;
        if (type.kind == TypeKind::Range) {
            error("RANGE_CONTEXT", "range is only a for iterable.", statement.span);
        }
        // A statement discards its value. An error is not a value to discard:
        // the consumption-site rule applies, and the program fails here.
        if (type.kind == TypeKind::Union && type.union_name.empty() &&
            case_index(type, simple(TypeKind::Error)) >= 0) {
            fail_fast_expressions_.insert(node.value.get());
        }
        return;
}

void Checker::check_if_stmt(const Stmt&, const IfStmt& node) {
        auto bool_type = simple(TypeKind::Bool);
        check_expr(*node.condition, &bool_type);
        auto variables = variables_;
        auto references = reference_roots_;
        auto reference_paths = reference_paths_;
        auto unknown_references = unknown_reference_targets_;
        auto const_bindings = const_bindings_;
        auto before = initialized_;
        auto before_maybe = maybe_initialized_;
        auto before_paths = class_initialized_paths_;
        auto before_method = current_receiver_effect_.initializes;
        auto before_method_written = current_receiver_effect_.writes;
        auto before_method_invalidated = current_receiver_effect_.invalidates;
        auto before_reference_effects = current_reference_effects_;

        check_block(node.then_body);
        auto yes_variables = variables_;
        auto yes = initialized_;
        auto yes_maybe = maybe_initialized_;
        auto yes_paths = class_initialized_paths_;
        auto yes_method = current_receiver_effect_.initializes;
        auto yes_method_written = current_receiver_effect_.writes;
        auto yes_method_invalidated = current_receiver_effect_.invalidates;
        auto yes_reference_effects = current_reference_effects_;
        ReferenceTargetState yes_targets{reference_paths_, unknown_reference_targets_};
        const bool yes_terminates = block_always_terminates(node.then_body);

        variables_ = variables;
        reference_roots_ = references;
        reference_paths_ = reference_paths;
        unknown_reference_targets_ = unknown_references;
        const_bindings_ = const_bindings;
        initialized_ = before;
        maybe_initialized_ = before_maybe;
        class_initialized_paths_ = before_paths;
        current_receiver_effect_.initializes = before_method;
        current_receiver_effect_.writes = before_method_written;
        current_receiver_effect_.invalidates = before_method_invalidated;
        current_reference_effects_ = before_reference_effects;
        check_block(node.else_body);
        auto no_variables = variables_;
        auto no = initialized_;
        auto no_maybe = maybe_initialized_;
        auto no_paths = class_initialized_paths_;
        auto no_method = current_receiver_effect_.initializes;
        auto no_method_written = current_receiver_effect_.writes;
        auto no_method_invalidated = current_receiver_effect_.invalidates;
        auto no_reference_effects = current_reference_effects_;
        ReferenceTargetState no_targets{reference_paths_, unknown_reference_targets_};
        const bool no_terminates = block_always_terminates(node.else_body);

        variables_ = variables;
        reference_roots_ = references;
        reference_paths_ = reference_paths;
        unknown_reference_targets_ = unknown_references;
        const_bindings_ = const_bindings;
        initialized_ = before;
        class_initialized_paths_ = before_paths;
        current_receiver_effect_.initializes = before_method;
        current_receiver_effect_.writes = before_method_written;
        current_receiver_effect_.invalidates = before_method_invalidated;
        current_reference_effects_ = before_reference_effects;
        for (const auto& [name, base_type] : variables) {
            if (base_type.kind == TypeKind::Tensor) {
                std::vector<Type> continuing_types;
                if (!yes_terminates) {
                    if (const auto it = yes_variables.find(name); it != yes_variables.end()) {
                        continuing_types.push_back(it->second);
                    }
                }
                if (!no_terminates) {
                    if (const auto it = no_variables.find(name); it != no_variables.end()) {
                        continuing_types.push_back(it->second);
                    }
                }
                if (!continuing_types.empty()) {
                    variables_[name] =
                        merge_shaped_flow_facts(base_type, continuing_types);
                }
            }
            if ((yes_terminates || yes.contains(name)) && (no_terminates || no.contains(name))) {
                initialized_.insert(name);
            } else if (reference_paths.contains(name)) {
                // A reference can become uninitialized by rebinding to uninitialized
                // storage, so its pre-branch state is not monotonic.
                initialized_.erase(name);
            }
            if (variables.at(name).kind == TypeKind::Class) {
                std::unordered_set<std::string> merged;
                const auto& yp = yes_paths[name];
                const auto& np = no_paths[name];
                // A branch that leaves the binding uninitialized does not
                // constrain its fields: a read after the join checks the
                // binding first (L13).
                const bool yes_unset = !yes.contains(name) && !yes_maybe.contains(name);
                const bool no_unset = !no.contains(name) && !no_maybe.contains(name);
                if (yes_terminates || (yes_unset && !no_terminates && !no_unset)) merged = np;
                else if (no_terminates || (no_unset && !yes_unset)) merged = yp;
                else {
                    for (const auto& p : yp) {
                        if (np.contains(p)) merged.insert(p);
                    }
                }
                class_initialized_paths_[name] = std::move(merged);
            }
        }
        if (yes_terminates) current_receiver_effect_.initializes = no_method;
        else if (no_terminates) current_receiver_effect_.initializes = yes_method;
        else {
            current_receiver_effect_.initializes.clear();
            for (const auto& field : yes_method) {
                if (no_method.contains(field)) current_receiver_effect_.initializes.insert(field);
            }
        }
        current_receiver_effect_.writes = before_method_written;
        current_receiver_effect_.writes.insert(yes_method_written.begin(), yes_method_written.end());
        current_receiver_effect_.writes.insert(no_method_written.begin(), no_method_written.end());
        current_receiver_effect_.invalidates = yes_method_invalidated;
        current_receiver_effect_.invalidates.insert(no_method_invalidated.begin(), no_method_invalidated.end());
        current_reference_effects_ =
            merge_reference_branches(before_reference_effects, yes_reference_effects,
                                     no_reference_effects, yes_terminates, no_terminates);

        std::vector<ReferenceTargetState> continuing_targets;
        if (!yes_terminates) continuing_targets.push_back(std::move(yes_targets));
        if (!no_terminates) continuing_targets.push_back(std::move(no_targets));
        auto joined = join_reference_targets(
            references, reference_paths, unknown_references, continuing_targets);
        reference_roots_ = std::move(joined.roots);
        reference_paths_ = std::move(joined.paths);
        unknown_reference_targets_ = std::move(joined.unknown);
        std::unordered_set<std::string> rebound_in_branch;
        collect_rebound_references(node.then_body, rebound_in_branch);
        collect_rebound_references(node.else_body, rebound_in_branch);
        for (const auto& name : rebound_in_branch) {
            if (unknown_reference_targets_.contains(name)) joined.ambiguous.insert(name);
        }
        for (const auto& name : joined.ambiguous) {
            initialized_.erase(name);
            class_initialized_paths_.erase(name);
        }
        maybe_initialized_ = before_maybe;
        std::vector<std::pair<const std::unordered_set<std::string>*,
                              const std::unordered_set<std::string>*>> continuing;
        if (!yes_terminates) continuing.emplace_back(&yes, &yes_maybe);
        if (!no_terminates) continuing.emplace_back(&no, &no_maybe);
        join_maybe_initialized(continuing);
        return;
}

void Checker::check_while_stmt(const Stmt&, const WhileStmt& node) {
        auto bool_type = simple(TypeKind::Bool);
        check_expr(*node.condition, &bool_type);
        auto variables = variables_;
        auto references = reference_roots_;
        auto reference_paths = reference_paths_;
        auto unknown_references = unknown_reference_targets_;
        auto const_bindings = const_bindings_;
        auto initialized = initialized_;
        auto paths = class_initialized_paths_;
        auto method_initialized = current_receiver_effect_.initializes;
        auto method_written = current_receiver_effect_.writes;
        auto method_invalidated = current_receiver_effect_.invalidates;
        auto reference_effects = current_reference_effects_;
        std::unordered_set<std::string> loop_assigned;
        collect_assigned_bindings(node.body, loop_assigned);
        weaken_loop_tensor_facts(variables_, loop_assigned);
        variables = variables_;
        // The body may run after itself: what it may initialize may be
        // initialized at its start (L13).
        note_loop_initializations(node.body);
        const auto first_check = initialization_check_order_.size();
        {
            ScopedCounter loop(loop_depth_);
            check_block(node.body);
        }
        reject_untracked_loop_checks(first_check);
        auto body_variables = variables_;
        auto body_written = current_receiver_effect_.writes;
        auto body_invalidated = current_receiver_effect_.invalidates;
        auto body_initialized = initialized_;
        auto body_maybe = maybe_initialized_;
        variables_ = variables;
        reference_roots_ = references;
        reference_paths_ = reference_paths;
        unknown_reference_targets_ = unknown_references;
        const_bindings_ = const_bindings;
        initialized_ = initialized;
        class_initialized_paths_ = paths;
        // The loop may run zero times.
        join_maybe_initialized({{&body_initialized, &body_maybe}});
        for (const auto& [name, base_type] : variables) {
            if (base_type.kind != TypeKind::Tensor) continue;
            const auto it = body_variables.find(name);
            if (it == body_variables.end()) continue;
            variables_[name] =
                merge_shaped_flow_facts(base_type, {base_type, it->second});
        }
        current_receiver_effect_.initializes = method_initialized;
        current_receiver_effect_.writes = method_written;
        current_receiver_effect_.writes.insert(body_written.begin(), body_written.end());
        current_receiver_effect_.invalidates = method_invalidated;
        current_receiver_effect_.invalidates.insert(body_invalidated.begin(), body_invalidated.end());
        current_reference_effects_ = merge_reference_loop(reference_effects, current_reference_effects_);
        std::unordered_set<std::string> rebound;
        collect_rebound_references(node.body, rebound);
        for (const auto& name : rebound) {
            if (!reference_paths.contains(name)) continue;
            reference_roots_[name] = name;
            reference_paths_[name] = {name, ""};
            unknown_reference_targets_.insert(name);
            initialized_.erase(name);
            class_initialized_paths_.erase(name);
        }
        return;
}

void Checker::check_for_stmt(const Stmt& statement, const ForStmt& node) {
        auto type = check_expr(*node.iterable);
        if (type.kind == TypeKind::Invalid) return;
        if (type.kind != TypeKind::Array && type.kind != TypeKind::Bin && type.kind != TypeKind::Range) {
            error("TYPE_MISMATCH", "for requires an array, bin, or range.", statement.span);
        }
        if (is_discard_name(node.name)) error("DISCARD", discard_declaration_message, statement.span);
        if (node.name.starts_with(abi::symbol_namespace::internal_prefix))
            error("SHADOWING", compiler_name_message(node.name), statement.span);
        if (variables_.contains(node.name) || declaration_name_visible(node.name) ||
            is_reserved_value_name(node.name) || member_name_visible(node.name)) {
            error("SHADOWING", "Iteration name is already visible or reserved.", statement.span);
        }

        auto variables = variables_;
        auto references = reference_roots_;
        auto reference_paths = reference_paths_;
        auto unknown_references = unknown_reference_targets_;
        auto const_bindings = const_bindings_;
        auto initialized = initialized_;
        auto class_paths = class_initialized_paths_;
        auto method_initialized = current_receiver_effect_.initializes;
        auto method_written = current_receiver_effect_.writes;
        auto method_invalidated = current_receiver_effect_.invalidates;
        auto reference_effects = current_reference_effects_;
        auto borrowed = borrowed_;
        std::unordered_set<std::string> loop_assigned;
        collect_assigned_bindings(node.body, loop_assigned);
        weaken_loop_tensor_facts(variables_, loop_assigned);
        variables = variables_;

        if (node.writable) {
            const auto* name = binding_name(*node.iterable);
            if ((type.kind != TypeKind::Array && type.kind != TypeKind::Bin) ||
                !name || !variables_.contains(name->name)) {
                error("WRITE_CAPABILITY", "Writable iteration requires an array or bin binding.", statement.span);
            }
            const auto root = reference_root(name->name);
            if (borrowed_.contains(root) || narrowed_.contains(root)) {
                error("WRITE_CAPABILITY", "Writable iteration requires an unaliased array or bin binding.", statement.span);
            }
            borrowed_.insert(root);
        }

        Type item_type = simple(TypeKind::Int);
        if (type.kind == TypeKind::Array) {
            item_type = *type.first;
        } else if (type.kind == TypeKind::Bin) {
            item_type = simple(TypeKind::Bin);
        }
        variables_[node.name] = item_type;
        initialized_.insert(node.name);
        if (item_type.kind == TypeKind::Class) {
            class_initialized_paths_[node.name] = complete_class_paths(item_type);
        }
        // The body may run after itself: what it may initialize may be
        // initialized at its start (L13).
        note_loop_initializations(node.body);
        const auto first_check = initialization_check_order_.size();
        {
            ScopedCounter loop(loop_depth_);
            check_block(node.body);
        }
        reject_untracked_loop_checks(first_check);
        auto body_variables = variables_;
        auto body_written = current_receiver_effect_.writes;
        auto body_invalidated = current_receiver_effect_.invalidates;
        auto body_initialized = initialized_;
        auto body_maybe = maybe_initialized_;
        variables_ = variables;
        reference_roots_ = references;
        reference_paths_ = reference_paths;
        unknown_reference_targets_ = unknown_references;
        const_bindings_ = const_bindings;
        initialized_ = initialized;
        class_initialized_paths_ = class_paths;
        // A loop that runs at least once and is left only at the end of its
        // body (no break or continue of its own) ends in the state its body
        // ends in; any other loop may run zero times (L13).
        if (loop_runs_at_least_once(node, type, &const_integer_values_) &&
            !block_leaves_loop(node.body)) {
            for (const auto& name : body_initialized) {
                if (variables.contains(name)) initialized_.insert(name);
            }
        }
        join_maybe_initialized({{&body_initialized, &body_maybe}});
        for (const auto& [name, base_type] : variables) {
            if (base_type.kind != TypeKind::Tensor) continue;
            const auto it = body_variables.find(name);
            if (it == body_variables.end()) continue;
            variables_[name] =
                merge_shaped_flow_facts(base_type, {base_type, it->second});
        }
        current_receiver_effect_.initializes = method_initialized;
        current_receiver_effect_.writes = method_written;
        current_receiver_effect_.writes.insert(body_written.begin(), body_written.end());
        current_receiver_effect_.invalidates = method_invalidated;
        current_receiver_effect_.invalidates.insert(body_invalidated.begin(), body_invalidated.end());
        current_reference_effects_ = merge_reference_loop(reference_effects, current_reference_effects_);
        std::unordered_set<std::string> rebound;
        collect_rebound_references(node.body, rebound);
        for (const auto& name : rebound) {
            if (!reference_paths.contains(name)) continue;
            reference_roots_[name] = name;
            reference_paths_[name] = {name, ""};
            unknown_reference_targets_.insert(name);
            initialized_.erase(name);
            class_initialized_paths_.erase(name);
        }
        borrowed_ = borrowed;
        return;
}

void Checker::check_match_stmt(const Stmt& statement, const MatchStmt& node_value) {
    const auto& node = node_value;
    auto type = check_expr(*node.value);
    if (type.kind == TypeKind::Invalid) return;
    if (type.kind != TypeKind::Union) {
        error("TYPE_MISMATCH", "match requires a union or enum.", statement.span);
    }
    const bool named_enum = !type.union_name.empty();

    auto variables = variables_;
    auto references = reference_roots_;
    auto reference_paths = reference_paths_;
    auto unknown_references = unknown_reference_targets_;
    auto const_bindings = const_bindings_;
    auto initialized = initialized_;
    auto maybe_before = maybe_initialized_;
    auto class_paths = class_initialized_paths_;
    auto receiver_before = current_receiver_effect_;
    auto reference_before = current_reference_effects_;
    auto narrowed = narrowed_;
    std::unordered_set<int> seen;
    std::vector<std::unordered_map<std::string, Type>> continuing_variables;
    std::vector<std::unordered_set<std::string>> continuing_initialized;
    std::vector<std::unordered_set<std::string>> continuing_maybe;
    std::vector<std::unordered_map<std::string, std::unordered_set<std::string>>>
        continuing_class_paths;
    std::vector<std::unordered_set<std::string>> continuing_receiver_initialized;
    std::vector<std::unordered_map<std::string, StorageEffect>> continuing_reference_effects;
    std::vector<ReferenceTargetState> continuing_reference_targets;
    StorageEffect matched_receiver = receiver_before;
    auto matched_reference_effects = reference_before;

    for (auto& match_case : node.cases) {
        Type requested_case_type = simple(TypeKind::Invalid);
        int tag = -1;
        if (named_enum) {
            const auto prefix = type.union_name + ".";
            if (match_case.type.name.rfind(prefix, 0) != 0 ||
                !match_case.type.arguments.empty() || !match_case.type.dimensions.empty())
                error("MATCH_CASE", "Enum match cases must name a variant of '" + type.union_name + "'.", match_case.span);
            const auto variant = match_case.type.name.substr(prefix.size());
            const auto found = std::find(type.case_names.begin(), type.case_names.end(), variant);
            if (found == type.case_names.end())
                error("MATCH_CASE", "Enum '" + type.union_name + "' has no variant '" + variant + "'.", match_case.span);
            tag = static_cast<int>(found - type.case_names.begin());
            requested_case_type = type.cases[static_cast<std::size_t>(tag)];
            if (requested_case_type.kind == TypeKind::Void && match_case.binder)
                error("MATCH_CASE", "Payload-free enum variants cannot bind a value.", match_case.span);
            if (!match_case.tag.empty() && requested_case_type.kind == TypeKind::Void)
                error("MATCH_CASE", "Payload-free enum variants do not use (...).", match_case.span);
            if (match_case.tag.empty() && match_case.binder)
                error("MATCH_CASE", "Enum payload binders use Variant(name) syntax.", match_case.span);
        } else {
            if (!match_case.tag.empty())
                error("MATCH_CASE", "Variant(name) match syntax is only valid for enums.", match_case.span);
            requested_case_type = resolve_type(match_case.type);
            tag = case_index(type, requested_case_type);
        }
        if (!named_enum && tag < 0 &&
            requested_case_type.kind == TypeKind::Tensor &&
            requested_case_type.first) {
            int compatible_tag = -1;
            for (std::size_t i = 0; i < type.cases.size(); ++i) {
                const auto& candidate = type.cases[i];
                if (candidate.kind != requested_case_type.kind || !candidate.first ||
                    *candidate.first != *requested_case_type.first ||
                    !tensor_satisfies_shape_prefix(candidate, requested_case_type)) {
                    continue;
                }
                if (compatible_tag >= 0) {
                    compatible_tag = -2;
                    break;
                }
                compatible_tag = static_cast<int>(i);
            }
            if (compatible_tag >= 0) tag = compatible_tag;
        }
        if (tag < 0 || !seen.insert(tag).second) {
            error("MATCH_CASE", "Unreachable or duplicate typed case.", match_case.span);
        }
        const auto case_type = tag >= 0 ? type.cases[static_cast<std::size_t>(tag)]
                                        : requested_case_type;
        if (case_type.kind == TypeKind::None && match_case.binder) {
            error("MATCH_CASE", "none has no payload binder.", match_case.span);
        }

        case_types_[&match_case] = case_type;
        case_tags_[&match_case] = tag;
        variables_ = variables;
        reference_roots_ = references;
        reference_paths_ = reference_paths;
        unknown_reference_targets_ = unknown_references;
        const_bindings_ = const_bindings;
        initialized_ = initialized;
        maybe_initialized_ = maybe_before;
        class_initialized_paths_ = class_paths;
        current_receiver_effect_ = receiver_before;
        current_reference_effects_ = reference_before;
        narrowed_ = narrowed;

        auto matched_paths = initialized_paths_for_expr(*node.value);
        if (case_type.kind == TypeKind::Class) {
            matched_paths = complete_class_paths(case_type);
        }
        if (!named_enum) {
            if (const auto* name = binding_name(*node.value)) {
                variables_[name->name] = case_type;
                narrowed_.insert(reference_root(name->name));
                if (case_type.kind == TypeKind::Class)
                    class_initialized_paths_[reference_root(name->name)] = matched_paths;
            }
        }

        if (match_case.binder) {
            if (is_discard_name(*match_case.binder))
                error("DISCARD", discard_declaration_message, match_case.span);
            if (match_case.binder->starts_with(abi::symbol_namespace::internal_prefix))
                error("SHADOWING", compiler_name_message(*match_case.binder), match_case.span);
            if (variables_.contains(*match_case.binder) ||
                declaration_name_visible(*match_case.binder) ||
                is_reserved_value_name(*match_case.binder) ||
                member_name_visible(*match_case.binder)) {
                error("SHADOWING", "Case binder is already visible or reserved.", match_case.span);
            }
            variables_[*match_case.binder] = case_type;
            initialized_.insert(*match_case.binder);
            if (case_type.kind == TypeKind::Class) {
                class_initialized_paths_[*match_case.binder] = matched_paths;
            }
        }

        check_block(match_case.body);

        union_set(matched_receiver.required, current_receiver_effect_.required);
        union_set(matched_receiver.writes, current_receiver_effect_.writes);
        union_set(matched_receiver.invalidates, current_receiver_effect_.invalidates);
        for (const auto& [name, effect] : current_reference_effects_) {
            auto& matched = matched_reference_effects[name];
            union_set(matched.required, effect.required);
            union_set(matched.writes, effect.writes);
            union_set(matched.invalidates, effect.invalidates);
        }

        if (!block_always_terminates(match_case.body)) {
            continuing_variables.push_back(variables_);
            continuing_initialized.push_back(initialized_);
            continuing_maybe.push_back(maybe_initialized_);
            continuing_class_paths.push_back(class_initialized_paths_);
            continuing_receiver_initialized.push_back(current_receiver_effect_.initializes);
            continuing_reference_effects.push_back(current_reference_effects_);
            continuing_reference_targets.push_back(
                ReferenceTargetState{reference_paths_, unknown_reference_targets_});
        }
    }

    if (seen.size() != type.cases.size()) {
        error("MATCH_EXHAUSTIVE",
              named_enum ? "match must cover every enum variant exactly once."
                         : "match must cover every union case exactly once.",
              statement.span);
    }

    variables_ = variables;
    reference_roots_ = references;
    reference_paths_ = reference_paths;
    unknown_reference_targets_ = unknown_references;
    const_bindings_ = const_bindings;
    initialized_ = initialized;
    maybe_initialized_ = maybe_before;
    class_initialized_paths_ = class_paths;
    narrowed_ = narrowed;

    if (!continuing_initialized.empty()) {
        for (const auto& [name, variable_type] : variables) {
            if (variable_type.kind == TypeKind::Tensor) {
                std::vector<Type> continuing_types;
                continuing_types.reserve(continuing_variables.size());
                for (const auto& state : continuing_variables) {
                    if (const auto it = state.find(name); it != state.end()) {
                        continuing_types.push_back(it->second);
                    }
                }
                if (!continuing_types.empty()) {
                    variables_[name] =
                        merge_shaped_flow_facts(variable_type, continuing_types);
                }
            }
            if (std::all_of(continuing_initialized.begin(), continuing_initialized.end(),
                            [&](const auto& state) { return state.contains(name); })) {
                initialized_.insert(name);
            } else if (reference_paths.contains(name)) {
                initialized_.erase(name);
            }
            if (variable_type.kind == TypeKind::Class) {
                // Cases that leave the binding uninitialized do not constrain
                // its fields, unless every case does (L13).
                std::vector<std::size_t> setting;
                for (std::size_t i = 0; i < continuing_class_paths.size(); ++i) {
                    if (continuing_initialized[i].contains(name) || continuing_maybe[i].contains(name))
                        setting.push_back(i);
                }
                if (setting.empty()) {
                    for (std::size_t i = 0; i < continuing_class_paths.size(); ++i) setting.push_back(i);
                }
                std::unordered_set<std::string> merged;
                const auto first = continuing_class_paths[setting.front()].find(name);
                if (first != continuing_class_paths[setting.front()].end()) merged = first->second;
                for (std::size_t k = 1; k < setting.size(); ++k) {
                    const auto i = setting[k];
                    const auto current = continuing_class_paths[i].find(name);
                    for (auto it = merged.begin(); it != merged.end();) {
                        if (current == continuing_class_paths[i].end() ||
                            !current->second.contains(*it)) {
                            it = merged.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
                class_initialized_paths_[name] = std::move(merged);
            }
        }

        matched_receiver.initializes = continuing_receiver_initialized.front();
        for (std::size_t i = 1; i < continuing_receiver_initialized.size(); ++i) {
            for (auto it = matched_receiver.initializes.begin();
                 it != matched_receiver.initializes.end();) {
                if (!continuing_receiver_initialized[i].contains(*it)) {
                    it = matched_receiver.initializes.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (auto& [name, effect] : matched_reference_effects) {
            const auto first = continuing_reference_effects.front().find(name);
            effect.initializes =
                first == continuing_reference_effects.front().end()
                    ? std::unordered_set<std::string>{}
                    : first->second.initializes;
            for (std::size_t i = 1; i < continuing_reference_effects.size(); ++i) {
                const auto current = continuing_reference_effects[i].find(name);
                for (auto it = effect.initializes.begin(); it != effect.initializes.end();) {
                    if (current == continuing_reference_effects[i].end() ||
                        !current->second.initializes.contains(*it)) {
                        it = effect.initializes.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }
    } else {
        matched_receiver.initializes = receiver_before.initializes;
        for (auto& [name, effect] : matched_reference_effects) {
            const auto before = reference_before.find(name);
            effect.initializes =
                before == reference_before.end() ? std::unordered_set<std::string>{}
                                                 : before->second.initializes;
        }
    }

    current_receiver_effect_ = std::move(matched_receiver);
    current_reference_effects_ = std::move(matched_reference_effects);

    auto joined = join_reference_targets(
        references, reference_paths, unknown_references, continuing_reference_targets);
    reference_roots_ = std::move(joined.roots);
    reference_paths_ = std::move(joined.paths);
    unknown_reference_targets_ = std::move(joined.unknown);
    std::unordered_set<std::string> rebound_in_match;
    for (const auto& match_case : node.cases) {
        collect_rebound_references(match_case.body, rebound_in_match);
    }
    for (const auto& name : rebound_in_match) {
        if (unknown_reference_targets_.contains(name)) joined.ambiguous.insert(name);
    }
    for (const auto& name : joined.ambiguous) {
        initialized_.erase(name);
        class_initialized_paths_.erase(name);
    }
    std::vector<std::pair<const std::unordered_set<std::string>*,
                          const std::unordered_set<std::string>*>> continuing;
    for (std::size_t i = 0; i < continuing_initialized.size(); ++i)
        continuing.emplace_back(&continuing_initialized[i], &continuing_maybe[i]);
    join_maybe_initialized(continuing);
}

void Checker::check_stmt(const Stmt& statement) {
    // The simple statement after which the bindings it initializes are
    // initialized (L13); none while a compound statement's own expressions
    // are checked, whose calls are those places.
    struct StatementScope {
        const Stmt*& current;
        const Stmt* saved;
        ~StatementScope() { current = saved; }
    } statement_scope{current_simple_statement_, current_simple_statement_};
    current_simple_statement_ =
        std::holds_alternative<BindingStmt>(statement.data) ||
                std::holds_alternative<RebindStmt>(statement.data) ||
                std::holds_alternative<AssignStmt>(statement.data) ||
                std::holds_alternative<ReturnStmt>(statement.data) ||
                std::holds_alternative<ExprStmt>(statement.data)
            ? &statement
            : nullptr;
    if (const auto* node = std::get_if<BindingStmt>(&statement.data)) {
        check_binding_stmt(statement, *node);
        return;
    }
    if (const auto* node = std::get_if<RebindStmt>(&statement.data)) {
        check_rebind_stmt(statement, *node);
        return;
    }
    if (const auto* node = std::get_if<AssignStmt>(&statement.data)) {
        check_assign_stmt(statement, *node);
        return;
    }
    if (const auto* node = std::get_if<LoopControlStmt>(&statement.data)) {
        check_loop_control_stmt(statement, *node);
        return;
    }
    if (const auto* node = std::get_if<ReturnStmt>(&statement.data)) {
        check_return_stmt(statement, *node);
        return;
    }
    if (const auto* node = std::get_if<ExprStmt>(&statement.data)) {
        check_expression_stmt(statement, *node);
        return;
    }
    if (const auto* node = std::get_if<IfStmt>(&statement.data)) {
        check_if_stmt(statement, *node);
        return;
    }
    if (const auto* node = std::get_if<MainGuardStmt>(&statement.data)) {
        const auto variables_before = variables_;
        const auto references_before = reference_roots_;
        const auto reference_paths_before = reference_paths_;
        const auto unknown_before = unknown_reference_targets_;
        const auto initialized_before = initialized_;
        const auto const_before = const_bindings_;
        const auto integer_before = const_integer_values_;
        const auto class_paths_before = class_initialized_paths_;
        const auto receiver_before = current_receiver_effect_;
        const auto reference_effects_before = current_reference_effects_;
        const auto narrowed_before = narrowed_;
        const auto borrowed_before = borrowed_;
        {
            // An imported module's guard is merged into the root statements,
            // but its names are checked against that module, as when the
            // module is the root. Restored on unwinding too: an over-deep
            // body is recovered by the enclosing block's handler.
            struct ModuleNamespaceScope {
                std::string& current;
                std::string saved;
                ~ModuleNamespaceScope() { current = std::move(saved); }
            } module_scope{current_module_namespace_, current_module_namespace_};
            current_module_namespace_ = node->module_namespace;
            check_block(node->body);
        }

        if (!node->active) {
            variables_ = variables_before;
            reference_roots_ = references_before;
            reference_paths_ = reference_paths_before;
            unknown_reference_targets_ = unknown_before;
            initialized_ = initialized_before;
            const_bindings_ = const_before;
            const_integer_values_ = integer_before;
            class_initialized_paths_ = class_paths_before;
            current_receiver_effect_ = receiver_before;
            current_reference_effects_ = reference_effects_before;
            narrowed_ = narrowed_before;
            borrowed_ = borrowed_before;
        } else {
            for (auto it = variables_.begin(); it != variables_.end();)
                if (!variables_before.contains(it->first)) it = variables_.erase(it); else ++it;
            for (auto it = reference_roots_.begin(); it != reference_roots_.end();)
                if (!references_before.contains(it->first)) it = reference_roots_.erase(it); else ++it;
            for (auto it = reference_paths_.begin(); it != reference_paths_.end();)
                if (!reference_paths_before.contains(it->first)) it = reference_paths_.erase(it); else ++it;
            for (auto it = unknown_reference_targets_.begin(); it != unknown_reference_targets_.end();)
                if (!unknown_before.contains(*it)) it = unknown_reference_targets_.erase(it); else ++it;
            for (auto it = initialized_.begin(); it != initialized_.end();)
                if (!variables_before.contains(*it) && !references_before.contains(*it)) it = initialized_.erase(it); else ++it;
            for (auto it = const_bindings_.begin(); it != const_bindings_.end();)
                if (!const_before.contains(*it)) it = const_bindings_.erase(it); else ++it;
            for (auto it = const_integer_values_.begin(); it != const_integer_values_.end();)
                if (!integer_before.contains(it->first)) it = const_integer_values_.erase(it); else ++it;
            for (auto it = class_initialized_paths_.begin(); it != class_initialized_paths_.end();)
                if (!variables_before.contains(it->first)) it = class_initialized_paths_.erase(it); else ++it;
        }
        return;
    }
    if (const auto* node = std::get_if<WhileStmt>(&statement.data)) {
        check_while_stmt(statement, *node);
        return;
    }
    if (const auto* node = std::get_if<ForStmt>(&statement.data)) {
        check_for_stmt(statement, *node);
        return;
    }
    check_match_stmt(statement, std::get<MatchStmt>(statement.data));
}

void Checker::check_block(const std::vector<StmtPtr>& body) {
    // Before this block establishes its own checkpoint machinery, so an
    // over-deep block is recovered by the PARENT block's per-statement handler.
    nesting::DepthGuard guard(
        stmt_depth_, nesting::max_statement_depth,
        body.empty() ? SourceSpan{} : body.front()->span, "Block");
    struct ConstructorDepth {
        std::size_t& depth;
        explicit ConstructorDepth(std::size_t& value) : depth(value) { ++depth; }
        ~ConstructorDepth() { --depth; }
    } constructor_depth(constructor_block_depth_);
    // Valid code is the hot path. Snapshot flow state once per block and only
    // create a new checkpoint after a recovered diagnostic. If a statement
    // fails, restore the latest checkpoint and replay only the statements since
    // it with diagnostics suppressed. This preserves multi-error recovery while
    // avoiding O(statement_count^2) container copies for large valid blocks.
    auto capture_flow = [&]() {
        return std::make_tuple(
            variables_,
            reference_roots_,
            reference_paths_,
            unknown_reference_targets_,
            initialized_,
            class_initialized_paths_,
            current_receiver_effect_.initializes,
            current_receiver_effect_.required,
            current_receiver_effect_.writes,
            current_receiver_effect_.invalidates,
            current_return_initialized_,
            current_return_summary_seen_,
            current_reference_effects_,
            current_exit_receiver_initialized_,
            current_exit_reference_initialized_,
            current_effect_exit_summary_seen_,
            narrowed_,
            borrowed_,
            const_bindings_);
    };

    auto restore_flow = [&](auto&& snapshot) {
        variables_ = std::move(std::get<0>(snapshot));
        reference_roots_ = std::move(std::get<1>(snapshot));
        reference_paths_ = std::move(std::get<2>(snapshot));
        unknown_reference_targets_ = std::move(std::get<3>(snapshot));
        initialized_ = std::move(std::get<4>(snapshot));
        class_initialized_paths_ = std::move(std::get<5>(snapshot));
        current_receiver_effect_.initializes = std::move(std::get<6>(snapshot));
        current_receiver_effect_.required = std::move(std::get<7>(snapshot));
        current_receiver_effect_.writes = std::move(std::get<8>(snapshot));
        current_receiver_effect_.invalidates = std::move(std::get<9>(snapshot));
        current_return_initialized_ = std::move(std::get<10>(snapshot));
        current_return_summary_seen_ = std::get<11>(snapshot);
        current_reference_effects_ = std::move(std::get<12>(snapshot));
        current_exit_receiver_initialized_ = std::move(std::get<13>(snapshot));
        current_exit_reference_initialized_ = std::move(std::get<14>(snapshot));
        current_effect_exit_summary_seen_ = std::get<15>(snapshot);
        narrowed_ = std::move(std::get<16>(snapshot));
        borrowed_ = std::move(std::get<17>(snapshot));
        const_bindings_ = std::move(std::get<18>(snapshot));
    };

    auto recover_binding = [&](const Stmt& statement) {
        if (const auto* binding = std::get_if<BindingStmt>(&statement.data);
            binding && !variables_.contains(binding->name)) {
            const auto invalid = simple(TypeKind::Invalid);
            variables_[binding->name] = invalid;
            initialized_.insert(binding->name);
            binding_types_[&statement] = invalid;
        }
    };

    auto checkpoint = capture_flow();
    std::size_t checkpoint_index = 0;

    for (std::size_t index = 0; index < body.size(); ++index) {
        try {
            check_stmt(*body[index]);
        } catch (const CompileError& compile_error) {
            restore_flow(std::move(checkpoint));

            const bool previous_suppression = suppress_diagnostics_;
            suppress_diagnostics_ = true;
            try {
                for (std::size_t replay = checkpoint_index; replay < index; ++replay) {
                    check_stmt(*body[replay]);
                }
            } catch (...) {
                suppress_diagnostics_ = previous_suppression;
                throw;
            }
            suppress_diagnostics_ = previous_suppression;

            record(compile_error);
            recover_binding(*body[index]);

            checkpoint = capture_flow();
            checkpoint_index = index + 1;
        }
    }
}

bool Checker::expr_has_no_normal_return(const Expr& expression) const {
    if (const auto found = expr_types_.find(&expression);
        found != expr_types_.end() && found->second.kind == TypeKind::Never) {
        return true;
    }
    const auto resolution = call_resolutions_.find(&expression);
    if (resolution != call_resolutions_.end() &&
        resolution->second.kind == CallKind::Function) {
        const auto function = functions_.find(resolution->second.target);
        if (function != functions_.end() && function->second.no_normal_return) {
            return true;
        }
    }
    return false;
}

bool Checker::stmt_always_terminates(const Stmt& statement) const {
    if (std::holds_alternative<ReturnStmt>(statement.data)) return true;
    if (const auto* node = std::get_if<ExprStmt>(&statement.data)) {
        return expr_has_no_normal_return(*node->value);
    }
    if (const auto* node = std::get_if<IfStmt>(&statement.data)) {
        return !node->else_body.empty() && block_always_terminates(node->then_body) &&
               block_always_terminates(node->else_body);
    }
    if (const auto* node = std::get_if<MatchStmt>(&statement.data)) {
        return std::all_of(node->cases.begin(), node->cases.end(),
                           [&](const auto& match_case) {
                               return block_always_terminates(match_case.body);
                           });
    }
    // Loops are not assumed to be infinite. Only proven non-continuing
    // operations and exhaustive terminating branches establish this fact.
    return false;
}

bool Checker::block_always_terminates(const std::vector<StmtPtr>& body) const {
    for (const auto& statement : body) {
        if (stmt_always_terminates(*statement)) return true;
    }
    return false;
}

bool Checker::block_contains_return(const std::vector<StmtPtr>& body) const {
    for (const auto& statement : body) {
        if (std::holds_alternative<ReturnStmt>(statement->data)) return true;
        if (const auto* branch = std::get_if<IfStmt>(&statement->data)) {
            if (block_contains_return(branch->then_body) ||
                block_contains_return(branch->else_body)) return true;
        } else if (const auto* loop = std::get_if<WhileStmt>(&statement->data)) {
            if (block_contains_return(loop->body)) return true;
        } else if (const auto* loop = std::get_if<ForStmt>(&statement->data)) {
            if (block_contains_return(loop->body)) return true;
        } else if (const auto* match = std::get_if<MatchStmt>(&statement->data)) {
            for (const auto& match_case : match->cases) {
                if (block_contains_return(match_case.body)) return true;
            }
        }
    }
    return false;
}

// The root of a library build is checked like an imported module: it holds
// declarations, immutable const bindings and `if main` guards (which never
// run: a library has no entry point), no other top-level statement and no
// cli declaration (LIBRARY_TOP_LEVEL), and it exports at least one function
// to its C host (FFI_EXPORT).
void Checker::check_library_root(const Program& program) {
    try {
        for (const auto& class_decl : program.classes) {
            if (class_decl.name.rfind("$cli.", 0) == 0)
                error("LIBRARY_TOP_LEVEL", "A library build cannot declare cli.", class_decl.span);
        }
    } catch (const CompileError& compile_error) {
        record(compile_error);
    }
    for (const auto& statement : program.statements) {
        if (std::holds_alternative<MainGuardStmt>(statement->data)) continue;
        if (const auto* binding = std::get_if<BindingStmt>(&statement->data);
            binding && binding->is_const && !binding->reference && binding->value)
            continue;
        // A cli declaration's synthesized statements are reported with it.
        if (const auto* binding = std::get_if<BindingStmt>(&statement->data);
            binding && binding->declared_type.name.rfind("$cli.", 0) == 0)
            continue;
        if (statement->span.start.offset == std::numeric_limits<std::size_t>::max()) continue;
        try {
            error("LIBRARY_TOP_LEVEL",
                  "A library build cannot contain executable top-level statements; move them into a function or an if main guard.",
                  statement->span);
        } catch (const CompileError& compile_error) {
            record(compile_error);
        }
    }
    const bool exports = std::any_of(program.functions.begin(), program.functions.end(),
                                     [](const FunctionDecl& f) { return f.foreign_export.has_value(); });
    if (!exports) {
        try {
            error("FFI_EXPORT", "A library build must export at least one function with export \"C\".",
                  SourceSpan{});
        } catch (const CompileError& compile_error) {
            record(compile_error);
        }
    }
}

CheckedProgram Checker::check(ConcreteProgram concrete) {
    Program program = std::move(concrete.program);
    auto compiler_extensions = std::move(concrete.compiler_extensions);
    if (!program.imports.empty()) {
        throw std::logic_error("ConcreteProgram contains unresolved imports.");
    }
    for (const auto& declaration : program.classes) {
        if (!declaration.type_parameters.empty()) {
            throw std::logic_error("ConcreteProgram contains an unresolved generic class.");
        }
        for (const auto& method : declaration.methods) {
            if (!method.type_parameters.empty()) {
                throw std::logic_error("ConcreteProgram contains an unresolved generic method.");
            }
        }
    }
    for (const auto& function : program.functions) {
        if (!function.type_parameters.empty()) {
            throw std::logic_error("ConcreteProgram contains an unresolved generic function.");
        }
    }
    functions_.clear();
    root_functions_.clear();
    classes_.clear();
    class_names_.clear();
    enum_types_.clear();
    variables_.clear();
    reference_roots_.clear();
    reference_paths_.clear();
    unknown_reference_targets_.clear();
    expr_types_.clear();
    raw_types_.clear();
    field_accesses_.clear();
    method_calls_.clear();
    call_resolutions_.clear();
    binding_types_.clear();
    case_types_.clear();
    case_tags_.clear();
    enum_constructions_.clear();
    fail_fast_expressions_.clear();
    initialized_.clear();
    const_bindings_.clear();
    const_integer_values_.clear();
    class_initialized_paths_.clear();
    class_expr_initialized_paths_.clear();
    reset_current_effect_state();

    narrowed_.clear();
    borrowed_.clear();
    invalid_functions_.clear();
    diagnostics_.clear();
    current_class_.clear();
    loop_depth_ = 0;
    expr_depth_ = 0;
    stmt_depth_ = 0;
    explicit_numeric_literal_context_ = false;

    std::unordered_map<std::string, ClassDecl*> class_decls;
    for (auto& class_decl : program.classes) {
        try {
            if (is_reserved_value_name(class_decl.name) ||
                class_decl.name == "main") {
                error("DUPLICATE_NAME", "Class name is reserved.", class_decl.span);
            }
            if (!class_names_.insert(class_decl.name).second) {
                error("DUPLICATE_NAME", "Duplicate class name.", class_decl.span);
            }
            class_decls[class_decl.name] = &class_decl;
        } catch (const CompileError& compile_error) {
            record(compile_error);
        }
    }

    std::unordered_map<std::string, EnumDecl*> enum_decls;
    for (auto& enum_decl : program.enums) {
        try {
            if (is_reserved_value_name(enum_decl.name) || enum_decl.name == "main" ||
                class_names_.contains(enum_decl.name) || enum_decls.contains(enum_decl.name))
                error("DUPLICATE_NAME", "Enum name is reserved or duplicated.", enum_decl.span);
            enum_decls[enum_decl.name] = &enum_decl;
        } catch (const CompileError& compile_error) { record(compile_error); }
    }

    std::unordered_set<std::string> building_enums;
    std::unordered_set<std::string> invalid_enums;
    std::function<void(EnumDecl&)> build_enum;
    std::function<void(const TypeName&)> ensure_enum_dependencies;
    ensure_enum_dependencies = [&](const TypeName& source) {
        if (const auto it = enum_decls.find(source.name); it != enum_decls.end()) build_enum(*it->second);
        for (const auto& argument : source.arguments) ensure_enum_dependencies(argument);
        for (const auto& parameter : source.function_parameters) ensure_enum_dependencies(parameter);
    };
    build_enum = [&](EnumDecl& declaration) {
        if (enum_types_.contains(declaration.name)) return;
        if (invalid_enums.contains(declaration.name))
            error("INVALID_ENUM", "Referenced enum is invalid.", declaration.span);
        if (!building_enums.insert(declaration.name).second)
            error("ENUM_CYCLE", "Enum payload types cannot contain a recursive enum cycle.", declaration.span);
        try {
            std::vector<std::string> names;
            std::vector<Type> payloads;
            std::unordered_set<std::string> seen;
            for (const auto& variant : declaration.variants) {
                if (is_reserved_value_name(variant.name) || !seen.insert(variant.name).second)
                    error("DUPLICATE_NAME", "Enum variant '" + variant.name + "' is reserved or duplicated.", variant.span);
                names.push_back(variant.name);
                if (variant.payload) {
                    ensure_enum_dependencies(*variant.payload);
                    auto payload = resolve_type(*variant.payload);
                    if (!is_storable(payload))
                        error("INVALID_TYPE", "Enum payload types must be storable values.", variant.payload->span);
                    payloads.push_back(std::move(payload));
                } else payloads.push_back(simple(TypeKind::Void));
            }
            enum_types_[declaration.name] = Type::enum_type(declaration.name, std::move(names), std::move(payloads));
            building_enums.erase(declaration.name);
        } catch (...) { building_enums.erase(declaration.name); throw; }
    };
    for (auto& enum_decl : program.enums) {
        if (!enum_decls.contains(enum_decl.name)) continue;
        try { build_enum(enum_decl); }
        catch (const CompileError& compile_error) { record(compile_error); invalid_enums.insert(enum_decl.name); }
    }

    auto build_class = [&](ClassDecl& declaration) {
        if (classes_.contains(declaration.name)) return;

        ClassTypeInfo info;
        info.name = declaration.name;
        info.standard_library = declaration.standard_library;

            std::unordered_set<std::string> own_fields;
            const bool namespace_scoped =
                declaration.standard_library ||
                declaration.name.find('.') != std::string::npos;
            for (auto& field : declaration.fields) {
                if ((!namespace_scoped && is_reserved_value_name(field.name)) ||
                    type_name_declared_in(field.name, declaration.module_namespace)) {
                    error("SHADOWING", "Class field name is reserved or conflicts with a class name.", field.span);
                }
                if (!own_fields.insert(field.name).second) {
                    error("DUPLICATE_NAME", "Duplicate class field.", field.span);
                }
                if (info.methods.contains(field.name)) {
                    error("SHADOWING", "Class field name conflicts with a method.", field.span);
                }
                auto field_type = resolve_type(field.type);
                if (!is_storable(field_type)) {
                    error("INVALID_TYPE", "Class fields require storable explicit types.", field.span);
                }
                info.fields.push_back(ClassFieldType{field.name, field_type, info.fields.size(), field.default_value.get(), field.is_const, field.is_private, declaration.name});
            }

            std::unordered_set<std::string> own_methods;
            for (auto& method : declaration.methods) {
                if (!method.is_constructor) {
                    if ((!namespace_scoped && is_reserved_value_name(method.name)) ||
                        type_name_declared_in(method.name, declaration.module_namespace) || method.name == "main") {
                        error("DUPLICATE_NAME", "Method name is reserved or conflicts with a class/entry point.", method.span);
                    }
                    if (!own_methods.insert(method.name).second) {
                        error("DUPLICATE_NAME", "Duplicate method name.", method.span);
                    }
                    if (std::any_of(info.fields.begin(), info.fields.end(), [&](const auto& field) { return field.name == method.name; })) {
                        error("SHADOWING", "Method name conflicts with a field.", method.span);
                    }
                }

                FunctionType public_signature;
                public_signature.result = resolve_type(method.return_type);
                if (public_signature.result.kind == TypeKind::None) {
                    error("INVALID_TYPE", "none is only a union case.", method.span);
                }

                std::unordered_set<std::string> parameter_names;
                bool defaults = false;
                for (auto& parameter : method.parameters) {
                    if (is_reserved_value_name(parameter.name) ||
                        type_name_declared_in(parameter.name, declaration.module_namespace) ||
                        info.methods.contains(parameter.name) || parameter.name == method.name) {
                        error("SHADOWING", "Method parameter shadows a class method or reserved name.", parameter.span);
                    }
                    if (!parameter_names.insert(parameter.name).second) {
                        error("DUPLICATE_NAME", "Duplicate parameter.", parameter.span);
                    }
                    auto parameter_type = resolve_type(parameter.type);
                    if (!is_storable(parameter_type)) {
                        error("INVALID_TYPE", "Parameters require storable explicit types.", parameter.span);
                    }
                    if (parameter.default_value) {
                        defaults = true;
                        if (parameter.writable) {
                            error("WRITE_CAPABILITY", "Reference parameters cannot have defaults.", parameter.span);
                        }
                    } else if (defaults) {
                        error("ARGUMENT_MISMATCH", "Required parameters must precede defaults.", parameter.span);
                    }
                    public_signature.parameters.push_back(
                        {parameter.name, parameter_type, parameter.writable, parameter.default_value.get(), parameter.is_const});
                }

                if (method.is_constructor) {
                    // The receiver of a constructor is a local of its body, so
                    // the signature carries only the written parameters.
                    const auto internal_name =
                        member_function_name::constructor(declaration.name, info.constructors.size());
                    functions_[internal_name] = std::move(public_signature);
                    info.constructors.push_back(internal_name);
                    info.constructor_instances[method.name] = internal_name;
                    if (method.is_private) info.private_constructors.insert(internal_name);
                    method_internal_names_[&method] = internal_name;
                    continue;
                }

                const auto internal_name = member_function_name::method(declaration.name, method.name);
                FunctionType internal_signature;
                internal_signature.result = public_signature.result;
                internal_signature.parameters.push_back(
                    {"$receiver", Type::class_type(declaration.name), false, nullptr, false});
                internal_signature.parameters.insert(internal_signature.parameters.end(),
                                                     public_signature.parameters.begin(),
                                                     public_signature.parameters.end());
                functions_[internal_name] = std::move(internal_signature);
                info.methods[method.name] = internal_name;
                method_internal_names_[&method] = internal_name;
                if (method.is_private) info.private_methods[method.name] = declaration.name;
                else info.private_methods.erase(method.name);
            }

        classes_[declaration.name] = std::move(info);
    };

    for (auto& class_decl : program.classes) {
        if (!class_names_.contains(class_decl.name)) continue;
        try {
            build_class(class_decl);
        } catch (const CompileError& compile_error) {
            record(compile_error);
        }
    }

    struct ExternalCSymbolBinding {
        std::string origin;
        std::string abi;
    };
    std::unordered_map<std::string, ExternalCSymbolBinding> external_c_symbols;
    const auto ffi_abi_type_key = [](const Type& type, const auto& self) -> std::string {
        if (type.kind == TypeKind::Tensor) return "tensor";
        if (type.kind == TypeKind::Function) {
            std::string key = "fn<" + self(*type.first, self) + ">(";
            for (std::size_t i = 0; i < type.parameters.size(); ++i) {
                if (i) key += ",";
                key += self(type.parameters[i], self);
            }
            return key + ")";
        }
        return std::to_string(static_cast<int>(type.kind));
    };
    // The C symbols that extern declarations bind; an exported function may
    // not define one of them (FFI_SYMBOL_CONFLICT).
    std::unordered_set<std::string> extern_c_symbols;
    for (const auto& function : program.functions)
        if (function.external_symbol) extern_c_symbols.insert(*function.external_symbol);
    std::unordered_set<std::string> exported_c_symbols;
    for (auto& function : program.functions) {
        try {
            if (!function.type_parameters.empty()) {
                throw std::logic_error("ConcreteProgram contains an unresolved generic function.");
            }
            if (function.name == "main") {
                error("RESERVED_MAIN", "Top-level code is the entrypoint.", function.span);
            }
            if (functions_.contains(function.name) || class_names_.contains(function.name) ||
                enum_types_.contains(function.name) || is_reserved_value_name(function.name)) {
                error("DUPLICATE_NAME", "Reserved or duplicate function name.", function.span);
            }
            if (function.module_namespace.empty()) root_functions_.insert(function.name);

            FunctionType signature;
            signature.result = resolve_type(function.return_type);
            signature.external = function.external_symbol.has_value();
            const auto ffi_scalar=[](const Type& type) {
                switch(type.kind) {
                    case TypeKind::Int64:
                    case TypeKind::Int8:
                    case TypeKind::Int16:
                    case TypeKind::Int32:
                    case TypeKind::Nat8:
                    case TypeKind::Nat16:
                    case TypeKind::Nat32:
                    case TypeKind::Nat64:
                    case TypeKind::Real64:
                    case TypeKind::Real32:
                    case TypeKind::Bool:
                        return true;
                    default:
                        return false;
                }
            };
            const auto ffi_callback_scalar=[](const Type& type) {
                switch(type.kind) {
                    case TypeKind::Int64:
                    case TypeKind::Int32:
                    case TypeKind::Nat32:
                    case TypeKind::Nat64:
                    case TypeKind::Real64:
                    case TypeKind::Real32:
                        return true;
                    default:
                        return false;
                }
            };
            const auto ffi_callback=[&](const Type& type) {
                if(type.kind!=TypeKind::Function || !type.first) return false;
                if(type.first->kind!=TypeKind::Void && !ffi_callback_scalar(*type.first))
                    return false;
                return std::all_of(
                    type.parameters.begin(), type.parameters.end(),
                    [&](const Type& parameter) { return ffi_callback_scalar(parameter); });
            };
            if(function.external_symbol) {
                const auto& symbol=*function.external_symbol;
                const auto valid_symbol=!symbol.empty() &&
                    (std::isalpha(static_cast<unsigned char>(symbol.front())) || symbol.front()=='_') &&
                    std::all_of(symbol.begin()+1,symbol.end(),[](char c) {
                        return std::isalnum(static_cast<unsigned char>(c)) || c=='_';
                    });
                if(!valid_symbol)
                    error("FFI_SYMBOL","External C symbol must be an ASCII C identifier.",function.span);
                if(runtime_reserved_c_symbol(symbol))
                    error("FFI_SYMBOL_CONFLICT","External C symbol is reserved by the compiler/runtime implementation; use a distinct C wrapper symbol.",function.span);
                if(signature.result.kind!=TypeKind::Void&&!ffi_scalar(signature.result))
                    error("FFI_TYPE","External C result must be void or an explicit numeric/bool scalar type.",function.span);
            }
            if (function.foreign_export) {
                if (artifact_ == CompileArtifact::Interactive) {
                    error("FFI_EXPORT", "export \"C\" is not available in an interactive session.",
                          function.foreign_export->span);
                }
                const auto& result = function.return_type;
                if (result.name == "int" && result.arguments.empty() && result.array_depth == 0) {
                    error("FFI_TYPE",
                          "Exported C result has type int, which has no fixed C representation; write int64.",
                          result.span);
                }
                if (!(result.name == "void" && result.arguments.empty() && result.array_depth == 0) &&
                    !foreign_export_scalar(result)) {
                    error("FFI_TYPE",
                          "Exported C result must be void or a fixed-width scalar with a defined C mapping: int8..int64, nat8..nat64, real32, real64.",
                          result.span);
                }
                for (const auto& parameter : function.parameters) {
                    if (parameter.default_value) {
                        error("FFI_DEFAULT", "Exported C parameters cannot have default arguments.",
                              parameter.span);
                    }
                    if (parameter.writable) {
                        error("FFI_REFERENCE",
                              "Exported C parameters are passed by value; reference parameters are not part of the C ABI subset.",
                              parameter.span);
                    }
                    if (!foreign_export_scalar(parameter.type)) {
                        error("FFI_TYPE",
                              "Exported C parameter '" + parameter.name + "' " +
                                  foreign_export_type_problem(parameter.type),
                              parameter.span);
                    }
                }
                const auto& symbol = function.foreign_export->symbol;
                if (c_keyword(symbol)) {
                    error("FFI_SYMBOL", "Exported C symbol '" + symbol + "' is a C keyword.",
                          function.span);
                }
                if (reserved_c_identifier(symbol)) {
                    error("FFI_SYMBOL",
                          "Exported C symbol '" + symbol + "' is a reserved C identifier.",
                          function.span);
                }
                if (runtime_reserved_c_symbol(symbol)) {
                    error("FFI_SYMBOL_CONFLICT",
                          "C symbol '" + symbol + "' is reserved by the compiler/runtime implementation.",
                          function.span);
                }
                if (!exported_c_symbols.insert(symbol).second) {
                    error("FFI_SYMBOL_CONFLICT", "C symbol '" + symbol + "' is exported more than once.",
                          function.span);
                }
                if (extern_c_symbols.contains(symbol)) {
                    error("FFI_SYMBOL_CONFLICT",
                          "C symbol '" + symbol + "' is both exported and bound by an extern declaration.",
                          function.span);
                }
            }
            if (signature.result.kind == TypeKind::None) {
                error("INVALID_TYPE", "none is only a union case.", function.span);
            }

            std::unordered_set<std::string> names;
            bool defaults = false;
            for (auto& parameter : function.parameters) {
                if (is_reserved_value_name(parameter.name) ||
                    type_name_declared_in(parameter.name, function.module_namespace)) {
                    error("SHADOWING", "Parameter name is reserved.", parameter.span);
                }
                if (!names.insert(parameter.name).second) {
                    error("DUPLICATE_NAME", "Duplicate parameter.", parameter.span);
                }
                auto type = resolve_type(parameter.type);
                if (!is_storable(type)) {
                    error("INVALID_TYPE", "Parameters require storable explicit types.", parameter.span);
                }
                if(function.external_symbol) {
                    if(parameter.default_value)
                        error("FFI_DEFAULT","External C parameters cannot have default arguments.",parameter.span);
                    if(ffi_scalar(type)) {
                        if(parameter.writable || parameter.is_const)
                            error("FFI_REFERENCE","External C scalar parameters are explicit by-value inputs and cannot use const/reference parameter forms.",parameter.span);
                    } else if(type.kind==TypeKind::Function) {
                        if(parameter.writable || parameter.is_const)
                            error("FFI_REFERENCE","External C callbacks are explicit by-value function pointers and cannot use const/reference parameter forms.",parameter.span);
                        if(!ffi_callback(type))
                            error("FFI_CALLBACK_TYPE","External C callbacks require fn signatures containing only int32/nat32/int64/nat64/real32/real64 value parameters and the same scalar set or void as the result.",parameter.span);
                    } else if(type.kind==TypeKind::String) {
                        if(!parameter.writable || !parameter.is_const)
                            error("FFI_REFERENCE","External C string inputs must be explicit call-scoped read-only borrows written as const string &.",parameter.span);
                    } else if(type.kind==TypeKind::Bin) {
                        if(!parameter.writable)
                            error("FFI_REFERENCE","External C bin parameters must be explicit call-scoped borrows written as const bin & or bin &.",parameter.span);
                    } else if(type.kind==TypeKind::Tensor) {
                        if(!parameter.writable)
                            error("FFI_REFERENCE","External C tensor parameters are opaque call-scoped borrows and must be written as const tensor<T> & or tensor<T> &.",parameter.span);
                    } else {
                        error("FFI_TYPE","External C parameters must use explicit numeric/bool scalar values, ABI-safe fn callbacks, const string &, const bin &, bin &, const tensor<T> &, or tensor<T> &.",parameter.span);
                    }
                }
                if (parameter.default_value) {
                    defaults = true;
                    if (parameter.writable) {
                        error("WRITE_CAPABILITY", "Reference parameters cannot have defaults.", parameter.span);
                    }
                } else if (defaults) {
                    error("ARGUMENT_MISMATCH", "Required parameters must precede defaults.", parameter.span);
                }
                signature.parameters.push_back(
                    {parameter.name, type, parameter.writable, parameter.default_value.get(), parameter.is_const});
            }
            if (function.external_symbol) {
                std::string abi = ffi_abi_type_key(signature.result, ffi_abi_type_key);
                for (const auto& parameter : signature.parameters) {
                    abi += "|" + ffi_abi_type_key(parameter.type, ffi_abi_type_key);
                    abi += parameter.writable ? "&" : "=";
                    abi += parameter.is_const ? "c" : "m";
                }
                const std::string origin =
                    function.source_file + ":" +
                    std::to_string(function.span.start.line) + ":" +
                    std::to_string(function.span.start.column) + ":" +
                    std::to_string(function.span.end.line) + ":" +
                    std::to_string(function.span.end.column);
                const auto [existing, inserted] = external_c_symbols.emplace(
                    *function.external_symbol,
                    ExternalCSymbolBinding{origin, abi});
                if (!inserted &&
                    (existing->second.origin != origin || existing->second.abi != abi)) {
                    error("FFI_SYMBOL_CONFLICT",
                          "External C symbol is already bound by another extern declaration; only ABI-identical specializations of the same generic extern may share it.",
                          function.span);
                }
            }
            functions_[function.name] = std::move(signature);
        } catch (const CompileError& compile_error) {
            record(compile_error);
            invalid_functions_.insert(&function);
        }
    }

    current_class_.clear();
    variables_.clear();
    reference_roots_.clear();
    reference_paths_.clear();
    unknown_reference_targets_.clear();
    initialized_.clear();
    const_bindings_.clear();
    const_integer_values_.clear();
    class_initialized_paths_.clear();
    in_function_ = false;
    for (auto& class_decl : program.classes) {
        if (!classes_.contains(class_decl.name)) continue;
        for (auto& field_decl : class_decl.fields) {
            if (!field_decl.default_value) continue;
            const auto* field = find_field(class_decl.name, field_decl.name);
            if (!field) continue;
            try {
                check_expr(*field_decl.default_value, &field->type);
            } catch (const CompileError& compile_error) {
                record(compile_error);
            }
        }
    }

    current_class_.clear();
    for (auto& function : program.functions) {
        if (invalid_functions_.contains(&function) || function.external_symbol) continue;
        variables_.clear();
        reference_roots_.clear();
        reference_paths_.clear();
        unknown_reference_targets_.clear();
        initialized_.clear();
        const_bindings_.clear();
    const_integer_values_.clear();
        in_function_ = false;
        for (auto& parameter : functions_.at(function.name).parameters) {
            if (!parameter.default_value) continue;
            try {
                check_expr(*parameter.default_value, &parameter.type);
                if (parameter.type.kind == TypeKind::Class &&
                    !fully_initialized_for_equality(*parameter.default_value, parameter.type)) {
                    error("UNINITIALIZED_ARGUMENT",
                          "Class value defaults must initialize every field.",
                          parameter.default_value->span);
                }
            } catch (const CompileError& compile_error) {
                record(compile_error);
            }
        }
    }

    for (auto& class_decl : program.classes) {
        if (!classes_.contains(class_decl.name)) continue;
        for (auto& method : class_decl.methods) {
            const auto internal_it = method_internal_names_.find(&method);
            if (internal_it == method_internal_names_.end() ||
                !functions_.contains(internal_it->second)) continue;
            auto& signature = functions_.at(internal_it->second);
            variables_.clear();
            reference_roots_.clear();
            reference_paths_.clear();
            unknown_reference_targets_.clear();
            initialized_.clear();
            const_bindings_.clear();
    const_integer_values_.clear();
            current_class_.clear();
            in_function_ = false;
            for (std::size_t i = method.is_constructor ? 0 : 1; i < signature.parameters.size(); ++i) {
                auto& parameter = signature.parameters[i];
                if (!parameter.default_value) continue;
                try {
                    check_expr(*parameter.default_value, &parameter.type);
                    if (parameter.type.kind == TypeKind::Class &&
                        !fully_initialized_for_equality(*parameter.default_value, parameter.type)) {
                        error("UNINITIALIZED_ARGUMENT",
                              "Class value defaults must initialize every field.",
                              parameter.default_value->span);
                    }
                } catch (const CompileError& compile_error) {
                    record(compile_error);
                }
            }
        }
    }

    // Effects and class-return initialization are interprocedural must-properties.
    // Seed class returns optimistically and iterate function/method bodies to a fixed point
    // before the diagnostic-producing pass. This removes declaration-order dependence.
    for (auto& [_, signature] : functions_) {
        signature.receiver_effect.required.clear();
        signature.receiver_effect.initializes.clear();
        signature.receiver_effect.writes.clear();
        signature.receiver_effect.invalidates.clear();
        signature.reference_effects.clear();
        signature.no_normal_return = false;
        signature.return_initialized_fields =
            signature.result.kind == TypeKind::Class
                ? complete_class_paths(signature.result)
                : std::unordered_set<std::string>{};
    }

    const auto summaries_equal = [](const FunctionType& left, const FunctionType& right) {
        return left.no_normal_return == right.no_normal_return &&
               left.receiver_effect.required == right.receiver_effect.required &&
               left.receiver_effect.initializes == right.receiver_effect.initializes &&
               left.receiver_effect.writes == right.receiver_effect.writes &&
               left.receiver_effect.invalidates == right.receiver_effect.invalidates &&
               left.return_initialized_fields == right.return_initialized_fields &&
               left.reference_effects == right.reference_effects;
    };

    std::size_t summary_budget = functions_.size() * 8 + 16;
    for (const auto& [_, signature] : functions_) {
        if (signature.result.kind == TypeKind::Class) {
            summary_budget += complete_class_paths(signature.result).size();
        }
    }

    bool summaries_converged = false;
    suppress_diagnostics_ = true;
    try {
        for (std::size_t iteration = 0; iteration < summary_budget; ++iteration) {
            const auto before_summaries = functions_;

            for (auto& function : program.functions) {
                if (invalid_functions_.contains(&function) || function.external_symbol) continue;

                variables_.clear();
                reference_roots_.clear();
                reference_paths_.clear();
                unknown_reference_targets_.clear();
                initialized_.clear();
                const_bindings_.clear();
    const_integer_values_.clear();
                class_initialized_paths_.clear();
                reset_current_effect_state();

                auto& signature = functions_.at(function.name);
                for (const auto& parameter : signature.parameters) {
                    variables_[parameter.name] = parameter.type;
                    if (parameter.is_const) const_bindings_.insert(parameter.name);
                    if (parameter.writable) {
                        current_reference_parameters_.insert(parameter.name);
                        if (parameter.is_const) current_reference_effects_[parameter.name].required.insert("");
                    } else {
                        initialized_.insert(parameter.name);
                        if (parameter.type.kind == TypeKind::Class) {
                            class_initialized_paths_[parameter.name] =
                                complete_class_paths(parameter.type);
                        }
                    }
                }

                current_return_ = signature.result;
                current_class_.clear();
                current_module_namespace_ = function.module_namespace;
                in_function_ = true;
                check_block(function.body);
                signature.no_normal_return =
                    !block_contains_return(function.body) &&
                    block_always_terminates(function.body);
                finalize_reference_effects(signature, !block_always_terminates(function.body));
                signature.return_initialized_fields =
                    signature.result.kind == TypeKind::Class && current_return_summary_seen_
                        ? current_return_initialized_
                        : std::unordered_set<std::string>{};
            }

            for (auto& class_decl : program.classes) {
                if (!classes_.contains(class_decl.name)) continue;
                for (auto& method : class_decl.methods) {
                    auto* signature = begin_member_body(class_decl, method);
                    if (!signature) continue;
                    check_block(method.body);
                    finish_member_body(class_decl, method, *signature, false);
                }
            }

            summaries_converged = true;
            for (const auto& [name, signature] : functions_) {
                const auto previous = before_summaries.find(name);
                if (previous == before_summaries.end() ||
                    !summaries_equal(signature, previous->second)) {
                    summaries_converged = false;
                    break;
                }
            }
            if (summaries_converged) break;
        }
    } catch (...) {
        suppress_diagnostics_ = false;
        throw;
    }
    suppress_diagnostics_ = false;
    // The final pass below records the run-time initialization checks
    // against the converged summaries.
    initialization_checks_.clear();
    initialization_masked_classes_.clear();
    initialization_check_order_.clear();
    statement_initializes_.clear();
    expression_initializes_.clear();

    if (!summaries_converged) {
        try {
            error("SUMMARY_ANALYSIS",
                  "Initialization summary analysis did not converge.",
                  {});
        } catch (const CompileError& compile_error) {
            record(compile_error);
        }
    }

    for (auto& function : program.functions) {
        if (invalid_functions_.contains(&function) || function.external_symbol) continue;
        variables_.clear();
        reference_roots_.clear();
        reference_paths_.clear();
        unknown_reference_targets_.clear();
        initialized_.clear();
        const_bindings_.clear();
    const_integer_values_.clear();
        class_initialized_paths_.clear();
        reset_current_effect_state();

        auto& signature = functions_.at(function.name);
        for (const auto& parameter : signature.parameters) {
            variables_[parameter.name] = parameter.type;
            if (parameter.is_const) const_bindings_.insert(parameter.name);
            if (parameter.writable) {
                current_reference_parameters_.insert(parameter.name);
                if (parameter.is_const) current_reference_effects_[parameter.name].required.insert("");
            } else {
                initialized_.insert(parameter.name);
                if (parameter.type.kind == TypeKind::Class) {
                    class_initialized_paths_[parameter.name] = complete_class_paths(parameter.type);
                }
            }
        }
        current_return_ = signature.result;
        current_class_.clear();
        current_module_namespace_ = function.module_namespace;
        in_function_ = true;
        try {
            for (const auto& parameter : function.parameters) {
                check_type_extent_expressions(parameter.type);
            }
            check_type_extent_expressions(function.return_type);
        } catch (const CompileError& compile_error) {
            record(compile_error);
        }
        check_block(function.body);
        finalize_reference_effects(signature, !block_always_terminates(function.body));
        signature.return_initialized_fields =
            signature.result.kind == TypeKind::Class && current_return_summary_seen_
                ? current_return_initialized_
                : std::unordered_set<std::string>{};
        if (signature.result.kind != TypeKind::Void && !block_always_terminates(function.body)) {
            try {
                error("MISSING_RETURN", "Function can reach its end without a return.", function.span);
            } catch (const CompileError& compile_error) {
                record(compile_error);
            }
        }
    }

    for (auto& class_decl : program.classes) {
        if (!classes_.contains(class_decl.name)) continue;
        for (auto& method : class_decl.methods) {
            auto* signature = begin_member_body(class_decl, method);
            if (!signature) continue;
            try {
                this_unavailable_ = method.is_constructor;
                for (const auto& parameter : method.parameters) {
                    check_type_extent_expressions(parameter.type);
                }
                this_unavailable_ = false;
                check_type_extent_expressions(method.return_type);
            } catch (const CompileError& compile_error) {
                this_unavailable_ = false;
                record(compile_error);
            }
            check_block(method.body);
            finish_member_body(class_decl, method, *signature, true);
            if (!method.is_constructor && signature->result.kind != TypeKind::Void &&
                !block_always_terminates(method.body)) {
                try {
                    error("MISSING_RETURN", "Method can reach its end without a return.", method.span);
                } catch (const CompileError& compile_error) {
                    record(compile_error);
                }
            }
        }
    }

    variables_.clear();
    reference_roots_.clear();
    reference_paths_.clear();
    unknown_reference_targets_.clear();
    initialized_.clear();
    const_bindings_.clear();
    const_integer_values_.clear();
    class_initialized_paths_.clear();
    reset_current_effect_state();

    current_return_ = simple(TypeKind::Void);
    current_class_.clear();
    current_module_namespace_.clear();
    in_function_ = false;
    if (artifact_ == CompileArtifact::Library) check_library_root(program);
    check_block(program.statements);

    if (!diagnostics_.empty()) {
        throw CompileErrors(std::move(diagnostics_));
    }

    CheckedProgram checked{std::move(program), std::move(compiler_extensions),
                           functions_, classes_, expr_types_, raw_types_,
                           field_accesses_, tensor_grad_accesses_, method_calls_, call_resolutions_, function_references_,
                           binding_types_, case_types_, case_tags_, enum_constructions_,
                           bounds_proven_, fail_fast_expressions_,
                           class_expr_initialized_paths_, scan_formats_, nullptr,
                           {}, {}, {}, {}, {}};
    for (const auto& [read, check] : initialization_checks_) {
        if (check.binding) checked.initialization_flags.insert(check.binding);
    }
    checked.initialization_masked_classes = std::move(initialization_masked_classes_);
    checked.initialization_checks = std::move(initialization_checks_);
    checked.statement_initializes = std::move(statement_initializes_);
    checked.expression_initializes = std::move(expression_initializes_);
    checked.effects = std::make_shared<const semantics::EffectSummaries>(semantics::summarize_effects(checked));
    return checked;
}

} // namespace quidra
