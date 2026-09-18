#include "quidra/checker.hpp"
#include "quidra/language.hpp"
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

bool printable(const Type& type) {
    return poisoned(type) || is_numeric(type) || type.kind == TypeKind::Bool ||
           type.kind == TypeKind::String || type.kind == TypeKind::Error;
}

bool runtime_reserved_c_symbol(std::string_view symbol) {
    if (symbol == "main" || symbol.starts_with("n_") || symbol.starts_with("quidra_") || symbol.starts_with("__quidra_")) {
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

bool same_public_signature(const FunctionType& a, const FunctionType& b) {
    if (a.result != b.result || a.parameters.size() != b.parameters.size()) return false;
    for (std::size_t i = 0; i < a.parameters.size(); ++i) {
        if (a.parameters[i].name != b.parameters[i].name ||
            a.parameters[i].type != b.parameters[i].type ||
            a.parameters[i].writable != b.parameters[i].writable ||
            (a.parameters[i].writable &&
             a.parameters[i].is_const != b.parameters[i].is_const)) {
            return false;
        }
    }
    return true;
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
    const bool shaped = base.kind == TypeKind::Tensor || base.kind == TypeKind::Neural;
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

void collect_assigned_bindings(
    const std::vector<StmtPtr>& body, std::unordered_set<std::string>& names) {
    for (const auto& statement : body) {
        if (const auto* assignment = std::get_if<AssignStmt>(&statement->data)) {
            if (const auto* name = std::get_if<NameExpr>(&assignment->target->data)) {
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
            (it->second.kind != TypeKind::Tensor && it->second.kind != TypeKind::Neural)) continue;
        it->second.length = -1;
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

std::optional<long long> constant_integer_value(const Expr& expression) {
    if (const auto* literal = std::get_if<IntegerExpr>(&expression.data)) {
        if (literal->value > static_cast<unsigned long long>(std::numeric_limits<long long>::max())) {
            return std::nullopt;
        }
        return static_cast<long long>(literal->value);
    }

    if (const auto* unary = std::get_if<UnaryExpr>(&expression.data)) {
        if (unary->op != "-") return std::nullopt;
        const auto value = constant_integer_value(*unary->operand);
        if (!value || *value == std::numeric_limits<long long>::min()) return std::nullopt;
        return -*value;
    }

    if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
        if (call->args.size() != 1 || call->args[0].writable || call->args[0].name) return std::nullopt;
        const auto target = builtin_scalar_type(call->callee);
        if (!target || !is_integer(*target)) return std::nullopt;
        const auto value = constant_integer_value(*call->args[0].value);
        if (!value || !integer_value_fits(*value, *target)) return std::nullopt;
        return value;
    }

    const auto* binary = std::get_if<BinaryExpr>(&expression.data);
    if (!binary) return std::nullopt;
    const auto left = constant_integer_value(*binary->left);
    const auto right = constant_integer_value(*binary->right);
    if (!left || !right) return std::nullopt;

    if ((binary->op == "/" || binary->op == "%") && *right == 0) return std::nullopt;
    if ((binary->op == "/" || binary->op == "%") &&
        *left == std::numeric_limits<long long>::min() && *right == -1) {
        return std::nullopt;
    }
    if (binary->op == "/") return *left / *right;
    if (binary->op == "%") return *left % *right;

    const auto checked_add = [](long long a, long long b, long long& out) {
        constexpr auto min = std::numeric_limits<long long>::min();
        constexpr auto max = std::numeric_limits<long long>::max();
        if ((b > 0 && a > max - b) || (b < 0 && a < min - b)) return false;
        out = a + b;
        return true;
    };
    const auto checked_sub = [](long long a, long long b, long long& out) {
        constexpr auto min = std::numeric_limits<long long>::min();
        constexpr auto max = std::numeric_limits<long long>::max();
        if ((b > 0 && a < min + b) || (b < 0 && a > max + b)) return false;
        out = a - b;
        return true;
    };
    const auto checked_mul = [](long long a, long long b, long long& out) {
        constexpr auto min = std::numeric_limits<long long>::min();
        constexpr auto max = std::numeric_limits<long long>::max();
        if (a == 0 || b == 0) {
            out = 0;
            return true;
        }
        if (a > 0) {
            if ((b > 0 && a > max / b) || (b < 0 && b < min / a)) return false;
        } else {
            if ((b > 0 && a < min / b) || (b < 0 && a < max / b)) return false;
        }
        out = a * b;
        return true;
    };

    long long result{};
    if (binary->op == "+") {
        if (!checked_add(*left, *right, result)) return std::nullopt;
    } else if (binary->op == "-") {
        if (!checked_sub(*left, *right, result)) return std::nullopt;
    } else if (binary->op == "*") {
        if (!checked_mul(*left, *right, result)) return std::nullopt;
    } else {
        return std::nullopt;
    }
    return result;
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

const std::string* Checker::find_method(const std::string& class_name, const std::string& method) const {
    const auto class_it = classes_.find(class_name);
    if (class_it == classes_.end()) return nullptr;
    const auto it = class_it->second.methods.find(method);
    return it == class_it->second.methods.end() ? nullptr : &it->second;
}

bool Checker::member_name_visible(const std::string& name) const {
    if (current_class_.empty()) return false;
    return find_field(current_class_, name) || find_method(current_class_, name);
}

bool Checker::equality_supported(const Type& type) const {
    std::unordered_set<std::string> visiting;
    std::function<bool(const Type&, bool)> supported =
        [&](const Type& current, bool inside_array) -> bool {
            if (poisoned(current) || is_numeric(current) || current.kind == TypeKind::Bool ||
                current.kind == TypeKind::String || current.kind == TypeKind::Bytes ||
                current.kind == TypeKind::Error) {
                return true;
            }
            if (current.kind == TypeKind::Array) {
                return current.first && supported(*current.first, true);
            }
            if (current.kind == TypeKind::Class) {
                if (current.class_name == "$std.json.Value" ||
                    current.class_name == "$std.http.Response") return false;
                // Array elements do not yet carry per-element class initialization metadata.
                // Reject arrays containing classes rather than reading conceptual uninitialized fields.
                if (inside_array) return false;
                if (!visiting.insert(current.class_name).second) return true;
                const auto it = classes_.find(current.class_name);
                if (it == classes_.end()) return false;
                for (const auto& field : it->second.fields) {
                    if (!supported(field.type, false)) {
                        visiting.erase(current.class_name);
                        return false;
                    }
                }
                visiting.erase(current.class_name);
                return true;
            }
            return false;
        };
    return supported(type, false);
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
        if (current_reference_parameters_.contains(name->name)) {
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
        if (!variables_.contains(name->name) && !current_class_.empty() &&
            find_field(current_class_, name->name)) {
            if (!current_receiver_effect_.initializes.contains(name->name)) return {};
            std::unordered_set<std::string> nested;
            const auto prefix = name->name + ".";
            for (const auto& path : current_receiver_effect_.initializes) {
                if (path.rfind(prefix, 0) == 0) nested.insert(path.substr(prefix.size()));
            }
            return nested;
        }

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
        if (!variables_.contains(name->name)) return std::nullopt;
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
        return unknown_reference_targets_.contains(name->name);
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
        if (!current_class_.empty() && !variables_.contains(name->name) &&
            find_field(current_class_, name->name)) {
            return name->name;
        }
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
        if (current_reference_parameters_.contains(name->name)) {
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
            if (const auto* name = std::get_if<NameExpr>(&current.data)) {
                if (const auto it = variables_.find(name->name); it != variables_.end()) return it->second;
                if (!current_class_.empty()) {
                    if (const auto* field = find_field(current_class_, name->name)) return field->type;
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
                if (base->kind == TypeKind::Bytes) return Type::simple(TypeKind::UInt8);
                if (base->kind == TypeKind::Tensor) return *base->first;
            }
            return std::nullopt;
        };

    if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        if (variables_.contains(name->name)) return const_bindings_.contains(name->name);
        if (!current_class_.empty()) {
            if (const auto* field = find_field(current_class_, name->name)) return field->is_const;
        }
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
        if (!current_class_.empty() && find_field(current_class_, name->name)) {
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

void Checker::check_static_index_bounds(const Type& base, const Expr& index) {
    if (base.kind != TypeKind::Array || base.length < 0) return;
    const auto value = constant_integer_value(index);
    if (!value) return;
    if (*value < 0 || *value >= base.length) {
        error("INDEX_BOUNDS",
              "Constant array index " + std::to_string(*value) +
                  " is outside [0, " + std::to_string(base.length) + ").",
              index.span);
        return;
    }
    bounds_proven_.insert(&index);
}

Type Checker::check_address_target(const Expr& expression, bool allow_tensor_element) {
    Type type;
    if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        if (variables_.contains(name->name)) {
            type = variables_.at(name->name);
        } else if (!current_class_.empty()) {
            const auto* field = find_field(current_class_, name->name);
            if (!field) error("UNKNOWN_NAME", "Unknown storage name '" + name->name + "'.", expression.span);
            type = field->type;
            field_accesses_[&expression] = FieldAccessInfo{current_class_, field->index, field->type};
        } else {
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
        if (base.kind == TypeKind::Class &&
            base.class_name.rfind("__quidra_gc__std_neural_Parameter_", 0) == 0 &&
            member->name == "value") {
            error("WRITE_CAPABILITY",
                  "neural.Parameter value storage is persistent identity; update it only through neural.update, neural.moment_update, or neural.load.",
                  expression.span);
        }
        const auto* field = find_field(base.class_name, member->name);
        if (!field) error("UNKNOWN_MEMBER", "Unknown class field '" + member->name + "'.", expression.span);
        type = field->type;
        field_accesses_[&expression] = FieldAccessInfo{base.class_name, field->index, field->type};
    } else if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
        auto base = stable_writable_storage(*index->base)
            ? check_address_target(*index->base)
            : check_expr(*index->base);
        auto int_type = simple(TypeKind::Int);
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
                check_expr(*item.index, &int_type);
            }
            type = *base.first;
        } else {
            if (index->items.size() != 1 || index->items.front().slice ||
                !index->items.front().index) {
                error("INDEX_ARITY", "Array and bytes indexing requires exactly one integer index.",
                      expression.span);
            }
            check_expr(*index->items.front().index, &int_type);
            check_static_index_bounds(base, *index->items.front().index);
            if (base.kind == TypeKind::Array) {
                type = *base.first;
            } else if (base.kind == TypeKind::Bytes) {
                type = simple(TypeKind::UInt8);
            } else {
                error("TYPE_MISMATCH",
                      "Element address requires an array, bytes, or tensor.", expression.span);
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
        const auto full = qualify(path);
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
    if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        if (const auto ref = reference_paths_.find(name->name); ref != reference_paths_.end()) {
            if (ref->second.second.empty()) {
                initialized_.insert(ref->second.first);
            } else {
                class_initialized_paths_[ref->second.first].insert(ref->second.second);
            }
            initialized_.insert(name->name);
            return;
        }
        if (variables_.contains(name->name)) {
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
        initialized_.insert(storage->first);
    };

    for (const auto& path : postconditions.invalidates) erase_path(path);
    for (const auto& path : postconditions.initializes) insert_prefixes(path);
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
    if (source.name != "union" && source.name != "tensor" && source.name != "neural" &&
        !source.arguments.empty()) {
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
        if (!is_numeric(element)) {
            error("INVALID_TYPE", "tensor element type must be numeric.", source.arguments.front().span);
        }
        const auto rank = !source.tensor_shape_prefix.empty()
            ? static_cast<long long>(source.tensor_shape_prefix.size())
            : source.tensor_rank.value_or(-1);
        type = Type::tensor(element, rank, source.tensor_shape_prefix,
                            source.tensor_known_shape_prefix);
    } else if (source.name == "neural") {
        Type element = simple(TypeKind::Float32);
        if (source.arguments.size() > 1) {
            error("GENERIC_ARITY", "neural accepts zero or one element type.", source.span);
        } else if (!source.arguments.empty()) {
            element = resolve_type(source.arguments.front());
        }
        if (element.kind != TypeKind::Float32 && element.kind != TypeKind::Float) {
            error("INVALID_TYPE", "neural element type must be float32 or float.", source.span);
        }
        const auto rank = !source.tensor_shape_prefix.empty()
            ? static_cast<long long>(source.tensor_shape_prefix.size())
            : source.tensor_rank.value_or(-1);
        type = Type::neural(element, rank, source.tensor_shape_prefix,
                            source.tensor_known_shape_prefix);
    } else if (source.name == "$std.neural.Gradients") {
        type = simple(TypeKind::Gradients);
    } else if (const auto builtin = builtin_scalar_type(source.name)) {
        type = *builtin;
    } else if (source.name == "void") {
        type = simple(TypeKind::Void);
    } else if (source.name == "none") {
        type = simple(TypeKind::None);
    } else if (source.name == "never") {
        type = simple(TypeKind::Never);
    } else if (source.name == "error") {
        type = simple(TypeKind::Error);
    } else if (source.name == "auto") {
        if (!auto_ok || source.array_depth) {
            error("INVALID_AUTO", "auto is only a complete initialized local type.", source.span);
        }
        return simple(TypeKind::Auto);
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


Type Checker::check_name_expr(const Expr& expression, const NameExpr& node_value) {
    Type type = simple(TypeKind::Void);
    const auto* node = &node_value;

        if (is_builtin_text_constant(node->name)) {
            type = simple(TypeKind::String);
        } else if (standard_float_constant(node->name)) {
            type = simple(TypeKind::Float);
        } else if (variables_.contains(node->name)) {
            if (current_reference_parameters_.contains(node->name)) {
                auto& effect = current_reference_effects_[node->name];
                if (!effect.initializes.contains("")) effect.required.insert("");
            } else if (!storage_initialized(expression)) {
                error("UNINITIALIZED", "Binding '" + node->name + "' may be uninitialized.", expression.span);
            }
            type = variables_.at(node->name);
            if (type.kind == TypeKind::Class) {
                const auto paths = initialized_paths_for_expr(expression);
                class_expr_initialized_paths_[&expression] = paths;
            }
        } else if (functions_.contains(node->name)) {
            error("FUNCTION_NOT_VALUE",
                  "Function '" + node->name + "' is callable but is not a first-class value.",
                  expression.span);
        } else if (!current_class_.empty()) {
            if (find_method(current_class_, node->name)) {
                error("FUNCTION_NOT_VALUE",
                      "Method '" + node->name + "' is callable but is not a first-class value.",
                      expression.span);
            }
            const auto* field = find_field(current_class_, node->name);
            if (!field) {
                error("UNKNOWN_NAME", "Unknown name '" + node->name + "'.", expression.span);
            }
            if (!current_receiver_effect_.initializes.contains(node->name)) {
                current_receiver_effect_.required.insert(node->name);
            }
            type = field->type;
            field_accesses_[&expression] = FieldAccessInfo{current_class_, field->index, field->type};
        } else {
            error("UNKNOWN_NAME", "Unknown name '" + node->name + "'.", expression.span);
        }
    
    return type;
}

Type Checker::check_member_expr(const Expr& expression, const MemberExpr& node_value) {
    Type type = simple(TypeKind::Void);
    const auto* node = &node_value;

        const auto reference_base = current_reference_parameter_path(*node->base);
        Type base;
        if (reference_base) {
            base = check_address_target(*node->base);
        } else {
            base = check_expr(*node->base);
        }
        if (poisoned(base)) {
            type = base;
        } else {
            if (base.kind != TypeKind::Class) {
                error("TYPE_MISMATCH", "Member access requires a class value.", expression.span);
            }
            const auto standard_map =
                base.class_name.rfind("__quidra_gc__std_map_Map_", 0) == 0;
            const auto standard_set =
                base.class_name.rfind("__quidra_gc__std_set_Set_", 0) == 0;
            if ((standard_map || standard_set) && node->name.rfind("__", 0) == 0) {
                error("UNKNOWN_MEMBER", "Standard collection internals are not source-visible.", expression.span);
            }
            const auto* field = find_field(base.class_name, node->name);
            if (!field) {
                error("UNKNOWN_MEMBER", "Class '" + base.class_name + "' has no field '" + node->name + "'.", expression.span);
            }
            if (const auto receiver_base = current_receiver_path(*node->base)) {
                const auto full = *receiver_base + "." + node->name;
                if (!current_receiver_effect_.initializes.contains(full)) {
                    current_receiver_effect_.required.insert(full);
                }
            } else if (reference_base) {
                auto full = reference_base->second;
                if (!full.empty()) full += ".";
                full += node->name;
                auto& effect = current_reference_effects_[reference_base->first];
                if (!effect.initializes.contains(full)) effect.required.insert(full);
            } else {
                const auto paths = initialized_paths_for_expr(*node->base);
                if (!paths.contains(node->name)) {
                    error("UNINITIALIZED", "Field '" + node->name + "' may be uninitialized.", expression.span);
                }
            }
            type = field->type;
            field_accesses_[&expression] = FieldAccessInfo{base.class_name, field->index, field->type};
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
    auto index_type = simple(TypeKind::Int);

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
                check_expr(*item.index, &index_type);
                if (result_rank >= 0) --result_rank;
                continue;
            }
            if (item.start) check_expr(*item.start, &index_type);
            if (item.stop) check_expr(*item.stop, &index_type);
            if (item.step) {
                check_expr(*item.step, &index_type);
                if (const auto step = constant_integer_value(*item.step); step && *step <= 0) {
                    error("SLICE_STEP",
                          "Tensor slices currently require a positive step.",
                          item.step->span);
                }
            }
        }
        auto shape_prefix = base.tensor_shape_prefix;
        auto known_shape_prefix = base.tensor_known_shape_prefix;
        const auto project_prefix = [&](std::vector<long long>& prefix) {
            std::size_t axis = 0;
            for (const auto& item : node->items) {
                if (!item.slice) {
                    if (axis < prefix.size()) prefix.erase(prefix.begin() + axis);
                } else {
                    ++axis;
                }
            }
        };
        project_prefix(shape_prefix);
        project_prefix(known_shape_prefix);
        return Type::tensor(*base.first, result_rank,
                            std::move(shape_prefix), std::move(known_shape_prefix));
    }

    if (node->items.size() != 1 || node->items.front().slice ||
        !node->items.front().index) {
        error("INDEX_ARITY", "Array, bytes, and string indexing requires exactly one integer index.",
              expression.span);
    }
    check_expr(*node->items.front().index, &index_type);
    check_static_index_bounds(base, *node->items.front().index);
    if (base.kind == TypeKind::Array) {
        type = *base.first;
    } else if (base.kind == TypeKind::Bytes) {
        type = simple(TypeKind::UInt8);
    } else if (base.kind == TypeKind::String) {
        type = simple(TypeKind::String);
    } else {
        error("TYPE_MISMATCH", "Indexing requires an array, bytes, string, or tensor.", expression.span);
    }

    return type;
}

Type Checker::check_method_call_expr(const Expr& expression,
                                     const MethodCallExpr& node_value) {
    Type type = simple(TypeKind::Void);
    const auto* node = &node_value;
        const auto* receiver_name = std::get_if<NameExpr>(&node->receiver->data);
        const bool super_receiver = receiver_name && receiver_name->name == "super";
        const auto type_receiver =
            receiver_name ? builtin_scalar_type(receiver_name->name) : std::optional<Type>{};

        if (type_receiver && is_numeric(*type_receiver) && node->method == "parse") {
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
            type = poisoned(argument)
                       ? simple(TypeKind::Invalid)
                       : Type::union_of({*type_receiver, simple(TypeKind::Error)});
        } else {
            Type receiver;
            const std::string* internal = nullptr;
            if (super_receiver) {
                if (current_class_.empty()) {
                    error("SUPER_CONTEXT", "super is only valid inside a class method.", expression.span);
                }
                const auto class_it = classes_.find(current_class_);
                if (class_it == classes_.end() || !class_it->second.parent) {
                    error("SUPER_CONTEXT", "super requires a parent class.", expression.span);
                }
                const auto& parent = *class_it->second.parent;
                internal = find_method(parent, node->method);
                if (!internal) {
                    error("SUPER_METHOD", "Parent class '" + parent + "' has no method '" + node->method + "'.", expression.span);
                }
                receiver = Type::class_type(current_class_);
                expr_types_[node->receiver.get()] = raw_types_[node->receiver.get()] = receiver;
            } else if (receiver_name &&
                       current_reference_parameters_.contains(receiver_name->name) &&
                       variables_.at(receiver_name->name).kind == TypeKind::Class) {
                receiver = variables_.at(receiver_name->name);
                expr_types_[node->receiver.get()] = raw_types_[node->receiver.get()] = receiver;
            } else {
                receiver = check_expr(*node->receiver);
            }

            if (poisoned(receiver)) {
                type = receiver;
            } else if (!super_receiver && receiver.kind != TypeKind::Class) {
                if (receiver.kind == TypeKind::Neural) {
                    if (node->method == "untrack") {
                        if (!node->type_arguments.empty() || !node->args.empty())
                            error("ARGUMENT_MISMATCH", "neural.untrack() takes no arguments.", expression.span);
                        type = Type::tensor(*receiver.first, receiver.length,
                                            receiver.tensor_shape_prefix,
                                            receiver.tensor_known_shape_prefix);
                    } else {
                        error("UNKNOWN_MEMBER", "Type '" + type_name(receiver) +
                              "' has no method '" + node->method + "'.", expression.span);
                    }
                } else if (receiver.kind == TypeKind::Tensor) {
                    const auto shape_type = Type::array(simple(TypeKind::Int));
                    if (node->method == "reshape") {
                        if (!node->type_arguments.empty() || node->args.size() != 1 ||
                            node->args[0].writable ||
                            (node->args[0].name && *node->args[0].name != "shape")) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.reshape requires one shape array.", expression.span);
                        }
                        auto shape = check_expr(*node->args[0].value, &shape_type);
                        long long rank = -1;
                        if (!poisoned(shape)) {
                            if (const auto* literal =
                                    std::get_if<ArrayExpr>(&node->args[0].value->data)) {
                                rank = static_cast<long long>(literal->elements.size());
                            } else {
                                const auto raw = raw_types_.find(node->args[0].value.get());
                                if (raw != raw_types_.end() &&
                                    raw->second.kind == TypeKind::Array &&
                                    raw->second.length >= 0) {
                                    rank = raw->second.length;
                                }
                            }
                        }
                        std::vector<long long> known_shape;
                        if (!poisoned(shape)) {
                            if (const auto* literal =
                                    std::get_if<ArrayExpr>(&node->args[0].value->data)) {
                                bool known = true;
                                for (const auto& item : literal->elements) {
                                    const auto extent = constant_integer_value(*item);
                                    if (!extent || *extent < 0) {
                                        known = false;
                                        break;
                                    }
                                    known_shape.push_back(*extent);
                                }
                                if (!known) known_shape.clear();
                            }
                        }
                        type = poisoned(shape)
                            ? simple(TypeKind::Invalid)
                            : Type::tensor(*receiver.first, rank, {}, std::move(known_shape));
                    } else if (node->method == "contiguous") {
                        if (!node->type_arguments.empty() || !node->args.empty()) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.contiguous() takes no arguments.", expression.span);
                        }
                        type = receiver;
                    } else if (node->method == "shape") {
                        if (!node->type_arguments.empty() || !node->args.empty()) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.shape() takes no arguments.", expression.span);
                        }
                        type = Type::array(simple(TypeKind::Int), receiver.length);
                    } else if (node->method == "is_contiguous") {
                        if (!node->type_arguments.empty() || !node->args.empty()) {
                            error("ARGUMENT_MISMATCH",
                                  "tensor.is_contiguous() takes no arguments.", expression.span);
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
                        type=poisoned(a)?simple(TypeKind::Invalid):Type::union_of({int_type,simple(TypeKind::None)});
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
                        type=simple(TypeKind::Bytes);
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
                        if (!is_numeric(*receiver.first) &&
                            receiver.first->kind != TypeKind::Bool &&
                            receiver.first->kind != TypeKind::String) {
                            error("UNKNOWN_MEMBER",
                                  "sorted is available on numeric, bool, and string arrays.",
                                  expression.span);
                        }
                        require_count(0, "array.sorted() takes no arguments.");
                        type=Type::array(*receiver.first);
                    }
                } else {
                    if (node->method != "string" || !printable(receiver) || receiver.kind == TypeKind::Bytes) {
                        error("UNKNOWN_MEMBER","Type '" + type_name(receiver) + "' has no method '" + node->method + "'.",expression.span);
                    }
                    require_count(0, "value.string() takes no arguments.");
                    type = simple(TypeKind::String);
                }
                }
            } else {
                if (!super_receiver) {
                    internal = find_method(receiver.class_name, node->method);
                    if (!internal) {
                        error("UNKNOWN_MEMBER", "Class '" + receiver.class_name + "' has no method '" + node->method + "'.", expression.span);
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
                if (!super_receiver && const_access_path(*node->receiver) && mutates_receiver) {
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
                        super_receiver ? nullptr : node->receiver.get(),
                        super_receiver, expression.span);
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
                case BuiltinCallable::Input: {
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "input takes no arguments.", expression.span);
                    }
                    type = Type::union_of({
                        simple(TypeKind::String), simple(TypeKind::None), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::Print:
                case BuiltinCallable::Write: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", name + " requires one argument.", expression.span);
                    }
                    auto argument = builtin_arg(0, "value");
                    if (!poisoned(argument) && !printable(argument)) {
                        error("TYPE_MISMATCH", name + " requires a scalar.", expression.span);
                    }
                    type = poisoned(argument) ? simple(TypeKind::Invalid) : simple(TypeKind::Void);
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
                    auto int_type = simple(TypeKind::Int);
                    auto count = builtin_arg(0, "n", &int_type);
                    if (node->args.size() == 1) {
                        if (!expected || expected->kind != TypeKind::Array) {
                            error("AMBIGUOUS_TYPE",
                                  "array(n) requires an explicit array type context because no element value is provided.",
                                  expression.span);
                        }
                        if (expected->length >= 0) {
                            const auto literal = constant_integer_value(*node->args[0].value);
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
                            if (expected && expected->kind == TypeKind::Array &&
                                expected->length >= 0) {
                                const auto literal = constant_integer_value(*node->args[0].value);
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
                    auto int_type = simple(TypeKind::Int);
                    bool any_poison = false;
                    for (std::size_t i = 0; i < node->args.size(); ++i) {
                        const std::string label =
                            node->args.size() == 1 ? "end" :
                            i == 0 ? "start" : i == 1 ? "end" : "step";
                        any_poison |= poisoned(builtin_arg(i, label, &int_type));
                    }
                    type = any_poison ? simple(TypeKind::Invalid) : simple(TypeKind::Range);
                    break;
                }
                case BuiltinCallable::Len: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "len requires one string, array, or bytes argument.", expression.span);
                    }
                    auto argument = builtin_arg(0, "value");
                    if (poisoned(argument)) {
                        type = argument;
                    } else {
                        if (argument.kind != TypeKind::String &&
                            argument.kind != TypeKind::Array &&
                            argument.kind != TypeKind::Bytes) {
                            error("TYPE_MISMATCH", "len requires a string, array, or bytes.", expression.span);
                        }
                        type = simple(TypeKind::Int);
                    }
                    break;
                }
                case BuiltinCallable::Abs: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "abs requires one numeric argument.", expression.span);
                    }
                    auto argument = builtin_arg(0, "value");
                    if (!poisoned(argument) && !is_numeric(argument)) {
                        error("TYPE_MISMATCH", "abs requires a numeric value.", expression.span);
                    }
                    type = argument;
                    break;
                }
                case BuiltinCallable::Sqrt: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "sqrt requires one floating-point argument.", expression.span);
                    }
                    auto argument = builtin_arg(0, "value");
                    if (!poisoned(argument) && !is_float(argument)) {
                        error("TYPE_MISMATCH", "sqrt requires a floating-point value.", expression.span);
                    }
                    type = argument;
                    break;
                }
                case BuiltinCallable::Min:
                case BuiltinCallable::Max: {
                    if (node->args.size() != 2) {
                        error("ARGUMENT_MISMATCH", name + " requires two numeric arguments.", expression.span);
                    }
                    auto left = builtin_arg(0, "a");
                    auto right = builtin_arg(1, "b", &left);
                    if (poisoned(left) || poisoned(right)) {
                        type = simple(TypeKind::Invalid);
                    } else {
                        if (!is_numeric(left) || right != left) {
                            error("TYPE_MISMATCH",
                                  name + " requires two values of the same numeric type.",
                                  expression.span);
                        }
                        type = left;
                    }
                    break;
                }
                case BuiltinCallable::MathSin:
                case BuiltinCallable::MathCos:
                case BuiltinCallable::MathTan:
                case BuiltinCallable::MathLog:
                case BuiltinCallable::MathExp: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", name + " requires one floating-point argument.", expression.span);
                    }
                    auto argument = builtin_arg(0, "value");
                    if (!poisoned(argument) && !is_float(argument)) {
                        error("TYPE_MISMATCH", name + " requires a floating-point value.", expression.span);
                    }
                    type = argument;
                    break;
                }
                case BuiltinCallable::MathTrunc:
                case BuiltinCallable::MathRound:
                case BuiltinCallable::MathFloor:
                case BuiltinCallable::MathCeil: {
                    if (node->args.size() != 1) error("ARGUMENT_MISMATCH", name + " requires one floating-point argument.", expression.span);
                    auto argument = builtin_arg(0, "value");
                    if (!poisoned(argument) && !is_float(argument)) error("TYPE_MISMATCH", name + " requires a floating-point value.", expression.span);
                    type = poisoned(argument) ? simple(TypeKind::Invalid) : simple(TypeKind::Int);
                    break;
                }
                case BuiltinCallable::MathPow: {
                    if (node->args.size() != 2) {
                        error("ARGUMENT_MISMATCH", "math.pow requires two floating-point arguments.", expression.span);
                    }
                    auto left = builtin_arg(0, "base");
                    auto right = builtin_arg(1, "exponent", &left);
                    if (poisoned(left) || poisoned(right)) {
                        type = simple(TypeKind::Invalid);
                    } else {
                        if (!is_float(left) || right != left) {
                            error("TYPE_MISMATCH", "math.pow requires two floating-point values of the same type.", expression.span);
                        }
                        type = left;
                    }
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
                    if (expected->kind != TypeKind::String && expected->kind != TypeKind::Int &&
                        expected->kind != TypeKind::Float && expected->kind != TypeKind::Bool) {
                        error("TYPE_MISMATCH", "CLI arguments support string, int, float, and bool.", expression.span);
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
                    if (expected->kind != TypeKind::String && expected->kind != TypeKind::Int &&
                        expected->kind != TypeKind::Float && expected->kind != TypeKind::Bool) {
                        error("TYPE_MISMATCH", "CLI options support string, int, float, and bool.", expression.span);
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
                case BuiltinCallable::FileRead: {
                    if (node->args.size() != 1) error("ARGUMENT_MISMATCH", "file.read requires one path.", expression.span);
                    auto string_type = simple(TypeKind::String);
                    auto path = builtin_arg(0, "path", &string_type);
                    type = poisoned(path) ? simple(TypeKind::Invalid)
                        : Type::union_of({string_type, simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::FileReadBytes: {
                    if (node->args.size() != 1) error("ARGUMENT_MISMATCH", "file.read_bytes requires one path.", expression.span);
                    auto string_type = simple(TypeKind::String);
                    auto path = builtin_arg(0, "path", &string_type);
                    type = poisoned(path) ? simple(TypeKind::Invalid)
                        : Type::union_of({simple(TypeKind::Bytes), simple(TypeKind::Error)});
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
                case BuiltinCallable::FileWriteBytes: {
                    if (node->args.size() != 2) error("ARGUMENT_MISMATCH", "file.write_bytes requires path and bytes.", expression.span);
                    auto string_type = simple(TypeKind::String);
                    auto bytes_type = simple(TypeKind::Bytes);
                    auto path = builtin_arg(0, "path", &string_type);
                    auto bytes = builtin_arg(1, "bytes", &bytes_type);
                    type = poisoned(path) || poisoned(bytes) ? simple(TypeKind::Invalid)
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
                    auto actual = builtin_arg(0, "actual");
                    auto expected_value = builtin_arg(1, "expected");
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
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "time.now takes no arguments.", expression.span);
                    }
                    type = Type::class_type("$std.time.Instant");
                    class_expr_initialized_paths_[&expression] = {"$seconds"};
                    break;
                }
                case BuiltinCallable::TimeSince: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "time.since requires one Instant.", expression.span);
                    }
                    auto instant_type = Type::class_type("$std.time.Instant");
                    auto start = builtin_arg(0, "start", &instant_type);
                    if (!poisoned(start) &&
                        !initialized_paths_for_expr(*node->args[0].value).contains("$seconds")) {
                        error("UNINITIALIZED", "time.since requires an initialized Instant.", expression.span);
                    }
                    type = poisoned(start) ? simple(TypeKind::Invalid)
                                           : Type::class_type("$std.time.Duration");
                    if (!poisoned(start)) class_expr_initialized_paths_[&expression] = {"$seconds"};
                    break;
                }
                case BuiltinCallable::TimeSeconds: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "time.seconds requires one float value.", expression.span);
                    }
                    auto float_type = simple(TypeKind::Float);
                    auto seconds = builtin_arg(0, "value", &float_type);
                    type = poisoned(seconds) ? simple(TypeKind::Invalid)
                                             : Type::class_type("$std.time.Duration");
                    if (!poisoned(seconds)) class_expr_initialized_paths_[&expression] = {"$seconds"};
                    break;
                }
                case BuiltinCallable::TimeSleep: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "time.sleep requires one Duration.", expression.span);
                    }
                    auto duration_type = Type::class_type("$std.time.Duration");
                    auto duration = builtin_arg(0, "duration", &duration_type);
                    if (!poisoned(duration) &&
                        !initialized_paths_for_expr(*node->args[0].value).contains("$seconds")) {
                        error("UNINITIALIZED", "time.sleep requires an initialized Duration.", expression.span);
                    }
                    type = poisoned(duration) ? simple(TypeKind::Invalid) : simple(TypeKind::Void);
                    break;
                }
                case BuiltinCallable::RandomGenerator: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "random.generator requires one int seed.", expression.span);
                    }
                    auto int_type = simple(TypeKind::Int);
                    auto seed = builtin_arg(0, "seed", &int_type);
                    type = poisoned(seed) ? simple(TypeKind::Invalid)
                                          : Type::class_type("$std.random.Generator");
                    if (!poisoned(seed)) class_expr_initialized_paths_[&expression] = {"$state"};
                    break;
                }
                case BuiltinCallable::RandomInt: {
                    if (current_class_ != "$std.random.Generator") {
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
                    if (current_class_ != "$std.random.Generator") {
                        error("INVALID_CONTEXT", "random generator operation requires a Generator receiver.", expression.span);
                    }
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "Generator.float takes no arguments.", expression.span);
                    }
                    current_receiver_effect_.required.insert("$state");
                    current_receiver_effect_.writes.insert("$state");
                    current_receiver_effect_.initializes.insert("$state");
                    type = simple(TypeKind::Float);
                    break;
                }
                case BuiltinCallable::RandomBool: {
                    if (current_class_ != "$std.random.Generator") {
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
                        : Type::class_type("$std.process.Result");
                    if (!poisoned(program) && !poisoned(args)) {
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
                            Type::class_type("$std.json.Value"), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::JsonKind: {
                    if (current_class_ != "$std.json.Value") {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "Value.kind takes no arguments.", expression.span);
                    }
                    type = simple(TypeKind::String);
                    break;
                }
                case BuiltinCallable::JsonSize: {
                    if (current_class_ != "$std.json.Value") {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "Value.size takes no arguments.", expression.span);
                    }
                    type = Type::union_of({simple(TypeKind::Int), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::JsonGet: {
                    if (current_class_ != "$std.json.Value") {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "Value.get requires one string key.", expression.span);
                    }
                    auto string_type = simple(TypeKind::String);
                    auto key = builtin_arg(0, "key", &string_type);
                    type = poisoned(key) ? simple(TypeKind::Invalid)
                        : Type::union_of({
                            Type::class_type("$std.json.Value"),
                            simple(TypeKind::None),
                            simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::JsonAt: {
                    if (current_class_ != "$std.json.Value") {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "Value.at requires one int index.", expression.span);
                    }
                    auto int_type = simple(TypeKind::Int);
                    auto index = builtin_arg(0, "index", &int_type);
                    type = poisoned(index) ? simple(TypeKind::Invalid)
                        : Type::union_of({
                            Type::class_type("$std.json.Value"),
                            simple(TypeKind::None),
                            simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::JsonText: {
                    if (current_class_ != "$std.json.Value") {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "Value.text takes no arguments.", expression.span);
                    }
                    type = Type::union_of({simple(TypeKind::String), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::JsonInteger: {
                    if (current_class_ != "$std.json.Value") {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "Value.integer takes no arguments.", expression.span);
                    }
                    type = Type::union_of({simple(TypeKind::Int), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::JsonNumber: {
                    if (current_class_ != "$std.json.Value") {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "Value.number takes no arguments.", expression.span);
                    }
                    type = Type::union_of({simple(TypeKind::Float), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::JsonBoolean: {
                    if (current_class_ != "$std.json.Value") {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "Value.boolean takes no arguments.", expression.span);
                    }
                    type = Type::union_of({simple(TypeKind::Bool), simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::JsonEncode: {
                    if (current_class_ != "$std.json.Value") {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (!node->args.empty()) {
                        error("ARGUMENT_MISMATCH", "Value.encode takes no arguments.", expression.span);
                    }
                    type = simple(TypeKind::String);
                    break;
                }
                case BuiltinCallable::JsonEqual: {
                    if (current_class_ != "$std.json.Value") {
                        error("INVALID_CONTEXT", "json Value operation requires a Value receiver.", expression.span);
                    }
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH", "Value.equal requires one Value.", expression.span);
                    }
                    auto value_type = Type::class_type("$std.json.Value");
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
                            Type::class_type("$std.http.Response"), simple(TypeKind::Error)});
                    if (!poisoned(url)) {
                        class_expr_initialized_paths_[&expression] = {
                            "status", "body", "$headers"
                        };
                    }
                    break;
                }
                case BuiltinCallable::HttpHeader: {
                    if (current_class_ != "$std.http.Response") {
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
                case BuiltinCallable::NeuralTrack:
                case BuiltinCallable::NeuralParameterTrack: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH",
                              name + " requires one floating-point tensor.",
                              expression.span);
                    }
                    auto input = builtin_arg(0, "value");
                    if (!poisoned(input) && (input.kind != TypeKind::Tensor ||
                        (input.first->kind != TypeKind::Float32 && input.first->kind != TypeKind::Float))) {
                        error("TYPE_MISMATCH",
                              name + " requires tensor<float32> or tensor<float>.",
                              expression.span);
                    }
                    type = poisoned(input) ? simple(TypeKind::Invalid)
                        : Type::neural(*input.first, input.length,
                                       input.tensor_shape_prefix,
                                       input.tensor_known_shape_prefix);
                    break;
                }
                case BuiltinCallable::NeuralConvolve2D: {
                    if (node->args.size() != 5) {
                        error("ARGUMENT_MISMATCH",
                              "neural.convolve2d requires value, weight, bias, stride, and padding.",
                              expression.span);
                    }
                    auto input = builtin_arg(0, "value");
                    auto weight = builtin_arg(1, "weight");
                    auto bias = builtin_arg(2, "bias");
                    const auto int_type = simple(TypeKind::Int);
                    auto stride = builtin_arg(3, "stride", &int_type);
                    auto padding = builtin_arg(4, "padding", &int_type);
                    const bool input_valid = !poisoned(input) &&
                        (input.kind == TypeKind::Neural ||
                         (input.kind == TypeKind::Tensor &&
                          (input.first->kind == TypeKind::Float32 ||
                           input.first->kind == TypeKind::Float)));
                    if (!poisoned(input) && !input_valid)
                        error("TYPE_MISMATCH", "neural.convolve2d requires neural or a floating-point tensor.", expression.span);
                    const auto parameter_element = [&](const Type& parameter) -> std::optional<Type> {
                        if (parameter.kind != TypeKind::Class ||
                            parameter.class_name.rfind("__quidra_gc__std_neural_Parameter_", 0) != 0) return std::nullopt;
                        const auto* value_field = find_field(parameter.class_name, "value");
                        if (!value_field || value_field->type.kind != TypeKind::Tensor) return std::nullopt;
                        return *value_field->type.first;
                    };
                    const auto weight_element = parameter_element(weight);
                    const auto bias_element = parameter_element(bias);
                    if (!poisoned(weight) && !weight_element)
                        error("TYPE_MISMATCH", "convolve2d weight must be neural.Parameter<T>.", expression.span);
                    if (!poisoned(bias) && !bias_element)
                        error("TYPE_MISMATCH", "convolve2d bias must be neural.Parameter<T>.", expression.span);
                    if (input_valid && weight_element && bias_element &&
                        (*input.first != *weight_element || *input.first != *bias_element))
                        error("TYPE_MISMATCH", "convolve2d input, weight, and bias element types must match.", expression.span);
                    type = (input_valid && !poisoned(stride) && !poisoned(padding))
                        ? input : simple(TypeKind::Invalid);
                    break;
                }
                case BuiltinCallable::NeuralAffine: {
                    if (node->args.size() != 3) {
                        error("ARGUMENT_MISMATCH",
                              "neural.affine requires value plus weight and bias parameters.",
                              expression.span);
                    }
                    auto input = builtin_arg(0, "value");
                    auto weight = builtin_arg(1, "weight");
                    auto bias = builtin_arg(2, "bias");
                    const bool input_valid = !poisoned(input) &&
                        (input.kind == TypeKind::Neural ||
                         (input.kind == TypeKind::Tensor &&
                          (input.first->kind == TypeKind::Float32 ||
                           input.first->kind == TypeKind::Float)));
                    if (!poisoned(input) && !input_valid) {
                        error("TYPE_MISMATCH",
                              "neural.affine requires neural or a floating-point tensor.",
                              expression.span);
                    }
                    const auto parameter_element = [&](const Type& parameter) -> std::optional<Type> {
                        if (parameter.kind != TypeKind::Class ||
                            parameter.class_name.rfind("__quidra_gc__std_neural_Parameter_", 0) != 0) {
                            return std::nullopt;
                        }
                        const auto* value_field = find_field(parameter.class_name, "value");
                        if (!value_field || value_field->type.kind != TypeKind::Tensor) {
                            return std::nullopt;
                        }
                        return *value_field->type.first;
                    };
                    const auto weight_element = parameter_element(weight);
                    const auto bias_element = parameter_element(bias);
                    if (!poisoned(weight) && !weight_element) {
                        error("TYPE_MISMATCH", "affine weight must be neural.Parameter<T>.", expression.span);
                    }
                    if (!poisoned(bias) && !bias_element) {
                        error("TYPE_MISMATCH", "affine bias must be neural.Parameter<T>.", expression.span);
                    }
                    if (input_valid && weight_element && bias_element &&
                        (*input.first != *weight_element || *input.first != *bias_element)) {
                        error("TYPE_MISMATCH",
                              "affine input, weight, and bias element types must match.",
                              expression.span);
                    }
                    type = input_valid ? input : simple(TypeKind::Invalid);
                    break;
                }
                case BuiltinCallable::NeuralSave:
                case BuiltinCallable::NeuralLoad: {
                    const bool loading=builtin==BuiltinCallable::NeuralLoad;
                    if (node->args.size()!=2 && node->args.size()!=3) {
                        error("ARGUMENT_MISMATCH",
                              loading
                                ? "neural.load requires &model [, &optimizer], path = ...."
                                : "neural.save requires model [, optimizer], path = ....",
                              expression.span);
                        type=simple(TypeKind::Invalid);
                        break;
                    }
                    const auto object_count=node->args.size()-1;
                    bool any_poison=false;
                    std::vector<Type> object_types;
                    object_types.reserve(object_count);
                    const auto string_type=simple(TypeKind::String);

                    std::function<bool(const Type&,std::unordered_set<std::string>&)> serializable;
                    serializable=[&](const Type& current,std::unordered_set<std::string>& active)->bool{
                        if(is_numeric(current)||current.kind==TypeKind::Bool||
                           current.kind==TypeKind::String||current.kind==TypeKind::Bytes||
                           current.kind==TypeKind::Tensor) return true;
                        if(current.kind!=TypeKind::Class) return false;
                        if(!active.insert(current.class_name).second) return false;
                        const auto ci=classes_.find(current.class_name);
                        if(ci==classes_.end()){active.erase(current.class_name);return false;}
                        for(const auto& field:ci->second.fields){
                            if(!serializable(field.type,active)){
                                active.erase(current.class_name);
                                return false;
                            }
                        }
                        active.erase(current.class_name);
                        return true;
                    };

                    for(std::size_t i=0;i<object_count;++i){
                        const auto& argument=node->args[i];
                        const char* expected_label=i==0?"model":"optimizer";
                        if(argument.name && *argument.name!=expected_label)
                            error("ARGUMENT_MISMATCH",
                                  std::string("neural.")+(loading?"load":"save")+
                                  " object labels are model and optimizer.",
                                  argument.span);
                        if(loading){
                            if(!argument.writable){
                                error("WRITE_CAPABILITY",
                                      "neural.load requires writable object arguments.",
                                      argument.span);
                            }
                            if(!stable_writable_storage(*argument.value)){
                                error("WRITE_CAPABILITY",
                                      "neural.load targets must be existing stable storage.",
                                      argument.span);
                            }
                        }else if(argument.writable){
                            error("WRITE_CAPABILITY",
                                  "neural.save object arguments are read-only.",
                                  argument.span);
                        }
                        auto current=loading
                            ? check_address_target(*argument.value)
                            : check_expr(*argument.value);
                        any_poison|=poisoned(current);
                        if(!poisoned(current)){
                            if(current.kind!=TypeKind::Class){
                                error("TYPE_MISMATCH",
                                      "neural.save/load objects must be class values.",
                                      argument.span);
                            }else{
                                std::unordered_set<std::string> active;
                                if(!serializable(current,active)){
                                    error("TYPE_MISMATCH",
                                          "neural.save/load object graph contains an unsupported or recursive field type.",
                                          argument.span);
                                }
                                if(!fully_initialized_for_equality(*argument.value,current)){
                                    error("UNINITIALIZED_ARGUMENT",
                                          loading
                                            ? "neural.load requires every target field to be definitely initialized because state is restored into existing storage."
                                            : "neural.save requires every object field to be definitely initialized.",
                                          argument.span);
                                }
                            }
                        }
                        object_types.push_back(current);
                    }

                    const auto& path_arg=node->args.back();
                    if(path_arg.writable || (path_arg.name && *path_arg.name!="path")){
                        error("ARGUMENT_MISMATCH",
                              "neural.save/load final argument must be path = string.",
                              path_arg.span);
                    }
                    any_poison|=poisoned(check_expr(*path_arg.value,&string_type));

                    if(loading && !any_poison){
                        StorageEffect write_effect;
                        write_effect.required.insert("");
                        write_effect.writes.insert("");
                        for(std::size_t i=0;i<object_count;++i)
                            apply_storage_effect_to_target(*node->args[i].value,write_effect,node->args[i].span);
                    }
                    type=any_poison?simple(TypeKind::Invalid):simple(TypeKind::Void);
                    break;
                }
                case BuiltinCallable::NeuralUpdate: {
                    if (node->args.size() != 3) {
                        error("ARGUMENT_MISMATCH",
                              "neural.update requires &model, gradients, and rate.",
                              expression.span);
                        type = simple(TypeKind::Invalid);
                        break;
                    }
                    const auto& model_arg=node->args[0];
                    const auto& gradients_arg=node->args[1];
                    const auto& rate_arg=node->args[2];
                    if (!model_arg.writable || gradients_arg.writable || rate_arg.writable ||
                        (model_arg.name && *model_arg.name!="model") ||
                        (gradients_arg.name && *gradients_arg.name!="gradients") ||
                        (rate_arg.name && *rate_arg.name!="rate")) {
                        error("WRITE_CAPABILITY",
                              "neural.update requires a writable model and read-only gradients/rate.",
                              expression.span);
                    }
                    if (!stable_writable_storage(*model_arg.value)) {
                        error("WRITE_CAPABILITY",
                              "neural.update model requires existing stable storage.",
                              model_arg.span);
                    }
                    auto model=check_address_target(*model_arg.value);
                    auto gradients=check_expr(*gradients_arg.value);
                    const auto float_type=simple(TypeKind::Float);
                    auto rate=check_expr(*rate_arg.value,&float_type);
                    if (!poisoned(model) && model.kind!=TypeKind::Class)
                        error("TYPE_MISMATCH","neural.update model must be a class value.",model_arg.span);
                    if (!poisoned(gradients) && gradients.kind!=TypeKind::Gradients)
                        error("TYPE_MISMATCH","neural.update requires neural.Gradients.",gradients_arg.span);
                    StorageEffect write_effect;
                    write_effect.required.insert("");
                    write_effect.writes.insert("");
                    if (!poisoned(model))
                        apply_storage_effect_to_target(*model_arg.value,write_effect,model_arg.span);
                    type=(poisoned(model)||poisoned(gradients)||poisoned(rate))
                        ? simple(TypeKind::Invalid) : simple(TypeKind::Void);
                    break;
                }
                case BuiltinCallable::NeuralNormalize:
                case BuiltinCallable::NeuralNormalizeInference: {
                    const bool training=builtin==BuiltinCallable::NeuralNormalize;
                    const std::size_t expected_arguments=training?7:6;
                    if (node->args.size()!=expected_arguments) {
                        error("ARGUMENT_MISMATCH",
                              training
                                ? "neural.normalize requires value, scale, bias, running mean, running variance, momentum, and epsilon."
                                : "neural.normalize_inference requires value, scale, bias, running mean, running variance, and epsilon.",
                              expression.span);
                        type=simple(TypeKind::Invalid);
                        break;
                    }
                    auto input=builtin_arg(0,"value");
                    auto scale=builtin_arg(1,"scale");
                    auto bias=builtin_arg(2,"bias");
                    auto running_mean=builtin_arg(3,"running_mean");
                    auto running_variance=builtin_arg(4,"running_variance");
                    const auto float_type=simple(TypeKind::Float);
                    auto momentum=training
                        ? builtin_arg(5,"momentum",&float_type)
                        : float_type;
                    auto epsilon=builtin_arg(training?6:5,"epsilon",&float_type);
                    const bool input_valid=!poisoned(input) &&
                        ((training && input.kind==TypeKind::Neural) ||
                         (!training && input.kind==TypeKind::Tensor && input.first &&
                          (input.first->kind==TypeKind::Float32||input.first->kind==TypeKind::Float)));
                    if(!poisoned(input)&&!input_valid)
                        error("TYPE_MISMATCH", training
                            ? "neural.normalize requires a neural value."
                            : "neural.normalize_inference requires a floating-point tensor.", expression.span);
                    const auto parameter_element=[&](const Type& value)->std::optional<Type>{
                        if(value.kind!=TypeKind::Class) return std::nullopt;
                        const auto* field=find_field(value.class_name,"value");
                        if(!field||field->type.kind!=TypeKind::Tensor||!field->type.first)
                            return std::nullopt;
                        if(value.class_name.rfind("__quidra_gc__std_neural_Parameter_",0)!=0)
                            return std::nullopt;
                        return *field->type.first;
                    };
                    const auto state_element=[&](const Type& value)->std::optional<Type>{
                        if(value.kind!=TypeKind::Class ||
                           value.class_name.rfind("__quidra_gc__std_neural_State_",0)!=0)
                            return std::nullopt;
                        const auto* field=find_field(value.class_name,"value");
                        if(!field||field->type.kind!=TypeKind::Tensor||!field->type.first)
                            return std::nullopt;
                        return *field->type.first;
                    };
                    const auto scale_element=parameter_element(scale);
                    const auto bias_element=parameter_element(bias);
                    const auto mean_element=state_element(running_mean);
                    const auto variance_element=state_element(running_variance);
                    if(!poisoned(scale)&&!scale_element)
                        error("TYPE_MISMATCH","normalize scale must be neural.Parameter<T>.",node->args[1].span);
                    if(!poisoned(bias)&&!bias_element)
                        error("TYPE_MISMATCH","normalize bias must be neural.Parameter<T>.",node->args[2].span);
                    if(!poisoned(running_mean)&&!mean_element)
                        error("TYPE_MISMATCH","normalize running_mean must be neural.State<tensor<T>>.",node->args[3].span);
                    if(!poisoned(running_variance)&&!variance_element)
                        error("TYPE_MISMATCH","normalize running_variance must be neural.State<tensor<T>>.",node->args[4].span);
                    if(input_valid&&scale_element&&bias_element&&mean_element&&variance_element &&
                       (*input.first!=*scale_element||*input.first!=*bias_element||
                        *input.first!=*mean_element||*input.first!=*variance_element))
                        error("TYPE_MISMATCH","normalize operands must share an element type.",expression.span);
                    if(training){
                        StorageEffect running_effect;
                        running_effect.required.insert("value");
                        running_effect.writes.insert("value");
                        if(mean_element)
                            apply_storage_effect_to_target(*node->args[3].value,running_effect,node->args[3].span);
                        if(variance_element)
                            apply_storage_effect_to_target(*node->args[4].value,running_effect,node->args[4].span);
                    }
                    type=(input_valid&&!poisoned(momentum)&&!poisoned(epsilon))
                        ? input : simple(TypeKind::Invalid);
                    break;
                }
                case BuiltinCallable::NeuralRandomMask: {
                    if(node->args.size()!=3){
                        error("ARGUMENT_MISMATCH",
                              "neural.random_mask requires value, state, and rate.",
                              expression.span);
                        type=simple(TypeKind::Invalid);
                        break;
                    }
                    auto input=builtin_arg(0,"value");
                    auto state=builtin_arg(1,"state");
                    const auto float_type=simple(TypeKind::Float);
                    auto rate=builtin_arg(2,"rate",&float_type);
                    if(!poisoned(input)&&input.kind!=TypeKind::Neural)
                        error("TYPE_MISMATCH","neural.random_mask requires a neural value.",node->args[0].span);
                    bool state_valid=false;
                    if(!poisoned(state)&&state.kind==TypeKind::Class&&
                       state.class_name.rfind("__quidra_gc__std_neural_State_",0)==0){
                        const auto* field=find_field(state.class_name,"value");
                        state_valid=field&&field->type.kind==TypeKind::UInt64;
                    }
                    if(!poisoned(state)&&!state_valid)
                        error("TYPE_MISMATCH","neural.random_mask state must be neural.State<uint64>.",node->args[1].span);
                    if(state_valid){
                        StorageEffect state_effect;
                        state_effect.required.insert("value");
                        state_effect.writes.insert("value");
                        apply_storage_effect_to_target(*node->args[1].value,state_effect,node->args[1].span);
                    }
                    type=(!poisoned(input)&&input.kind==TypeKind::Neural&&state_valid&&!poisoned(rate))
                        ? input : simple(TypeKind::Invalid);
                    break;
                }
                case BuiltinCallable::NeuralMomentUpdate: {
                    if(node->args.size()!=8){
                        error("ARGUMENT_MISMATCH",
                              "neural.moment_update requires &model, rate, beta1, beta2, epsilon, &step, &moments, and gradients.",
                              expression.span);
                        type=simple(TypeKind::Invalid);
                        break;
                    }
                    const auto& model_arg=node->args[0];
                    const auto& step_arg=node->args[5];
                    const auto& moments_arg=node->args[6];
                    const auto& gradients_arg=node->args[7];
                    if(!model_arg.writable||!step_arg.writable||!moments_arg.writable||
                       gradients_arg.writable||
                       (model_arg.name&&*model_arg.name!="model")||
                       (step_arg.name&&*step_arg.name!="step")||
                       (moments_arg.name&&*moments_arg.name!="moments")||
                       (gradients_arg.name&&*gradients_arg.name!="gradients"))
                        error("WRITE_CAPABILITY",
                              "neural.moment_update requires writable model/step/moments and read-only settings/gradients.",
                              expression.span);
                    if(!stable_writable_storage(*model_arg.value)||
                       !stable_writable_storage(*step_arg.value)||
                       !stable_writable_storage(*moments_arg.value))
                        error("WRITE_CAPABILITY",
                              "neural.moment_update writable arguments require existing stable storage.",
                              expression.span);
                    auto model=check_address_target(*model_arg.value);
                    const auto float_type=simple(TypeKind::Float);
                    auto rate=builtin_arg(1,"rate",&float_type);
                    auto beta1=builtin_arg(2,"beta1",&float_type);
                    auto beta2=builtin_arg(3,"beta2",&float_type);
                    auto epsilon=builtin_arg(4,"epsilon",&float_type);
                    auto step=check_address_target(*step_arg.value);
                    auto moments=check_address_target(*moments_arg.value);
                    auto gradients=check_expr(*gradients_arg.value);
                    if(!poisoned(model)&&model.kind!=TypeKind::Class)
                        error("TYPE_MISMATCH","moment_update model must be a class value.",model_arg.span);
                    const auto state_value_kind=[&](const Type& state,TypeKind kind){
                        if(state.kind!=TypeKind::Class||
                           state.class_name.rfind("__quidra_gc__std_neural_State_",0)!=0)
                            return false;
                        const auto* value=find_field(state.class_name,"value");
                        return value&&value->type.kind==kind;
                    };
                    const bool step_valid=!poisoned(step)&&state_value_kind(step,TypeKind::Int);
                    const bool moments_valid=!poisoned(moments)&&state_value_kind(moments,TypeKind::Bytes);
                    if(!poisoned(step)&&!step_valid)
                        error("TYPE_MISMATCH","moment_update step must be neural.State<int>.",step_arg.span);
                    if(!poisoned(moments)&&!moments_valid)
                        error("TYPE_MISMATCH","moment_update moments must be neural.State<bytes>.",moments_arg.span);
                    if(!poisoned(gradients)&&gradients.kind!=TypeKind::Gradients)
                        error("TYPE_MISMATCH","moment_update requires neural.Gradients.",gradients_arg.span);
                    StorageEffect write_effect;
                    write_effect.required.insert("");
                    write_effect.writes.insert("");
                    if(!poisoned(model)) apply_storage_effect_to_target(*model_arg.value,write_effect,model_arg.span);
                    if(step_valid) apply_storage_effect_to_target(*step_arg.value,write_effect,step_arg.span);
                    if(moments_valid) apply_storage_effect_to_target(*moments_arg.value,write_effect,moments_arg.span);
                    type=(poisoned(model)||poisoned(rate)||poisoned(beta1)||poisoned(beta2)||
                          poisoned(epsilon)||!step_valid||!moments_valid||poisoned(gradients))
                        ? simple(TypeKind::Invalid):simple(TypeKind::Void);
                    break;
                }
                case BuiltinCallable::NeuralAbsolute:
                case BuiltinCallable::NeuralExponential:
                case BuiltinCallable::NeuralLogarithm:
                case BuiltinCallable::NeuralMean:
                case BuiltinCallable::NeuralSumLast:
                case BuiltinCallable::NeuralMaxLast: {
                    if (node->args.size() != 1) error("ARGUMENT_MISMATCH", name + " requires one value.", expression.span);
                    auto input = builtin_arg(0, "value");
                    const bool valid = !poisoned(input) &&
                        ((input.kind == TypeKind::Neural) ||
                         (input.kind == TypeKind::Tensor &&
                          (input.first->kind == TypeKind::Float32 || input.first->kind == TypeKind::Float)));
                    if (!poisoned(input) && !valid)
                        error("TYPE_MISMATCH", name + " requires neural or a floating-point tensor.", expression.span);
                    type = valid ? input : simple(TypeKind::Invalid);
                    if (valid && builtin == BuiltinCallable::NeuralMean) {
                        type.length = 0;
                        type.tensor_shape_prefix.clear();
                        type.tensor_known_shape_prefix.clear();
                    }
                    break;
                }
                case BuiltinCallable::NeuralGrad: {
                    if (node->args.size() != 1) error("ARGUMENT_MISMATCH", "neural.grad requires one loss.", expression.span);
                    auto loss = builtin_arg(0, "loss");
                    if (!poisoned(loss) && loss.kind != TypeKind::Neural)
                        error("TYPE_MISMATCH", "neural.grad requires a neural loss.", expression.span);
                    type = poisoned(loss) ? simple(TypeKind::Invalid) : simple(TypeKind::Gradients);
                    break;
                }
                case BuiltinCallable::StatsMean: {
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH",
                              "stats.mean requires one tensor value.", expression.span);
                        type = simple(TypeKind::Invalid);
                        break;
                    }
                    if (node->args[0].writable ||
                        (node->args[0].name && *node->args[0].name != "value")) {
                        error("ARGUMENT_MISMATCH",
                              "stats.mean requires value = tensor or one positional tensor.",
                              node->args[0].span);
                    }
                    auto value = check_expr(*node->args[0].value);
                    if (!poisoned(value) && value.kind != TypeKind::Tensor) {
                        error("TYPE_MISMATCH", "stats.mean requires a tensor.", expression.span);
                    }
                    type = poisoned(value) ? simple(TypeKind::Invalid) : simple(TypeKind::Float);
                    break;
                }
                case BuiltinCallable::LinearMatmul: {
                    if (node->args.size() != 2) {
                        error("ARGUMENT_MISMATCH",
                              "linear.matmul requires two tensors.", expression.span);
                        type = simple(TypeKind::Invalid);
                        break;
                    }
                    if (node->args[0].writable || node->args[1].writable ||
                        (node->args[0].name && *node->args[0].name != "a") ||
                        (node->args[1].name && *node->args[1].name != "b")) {
                        error("ARGUMENT_MISMATCH",
                              "linear.matmul accepts a and b tensor values.", expression.span);
                    }
                    auto left = check_expr(*node->args[0].value);
                    auto right = check_expr(*node->args[1].value);
                    if (!poisoned(left) && left.kind != TypeKind::Tensor) {
                        error("TYPE_MISMATCH", "linear.matmul left operand must be a tensor.",
                              node->args[0].span);
                    }
                    if (!poisoned(right) && right.kind != TypeKind::Tensor) {
                        error("TYPE_MISMATCH", "linear.matmul right operand must be a tensor.",
                              node->args[1].span);
                    }
                    if (!poisoned(left) && !poisoned(right) &&
                        left.kind == TypeKind::Tensor && right.kind == TypeKind::Tensor) {
                        if (*left.first != *right.first) {
                            error("TYPE_MISMATCH",
                                  "linear.matmul requires identical tensor element types.",
                                  expression.span);
                        }
                        if ((left.length >= 0 && left.length != 2) ||
                            (right.length >= 0 && right.length != 2)) {
                            error("TYPE_MISMATCH", "linear.matmul requires rank-2 tensors.", expression.span);
                        }
                    }
                    type = poisoned(left) || poisoned(right)
                        ? simple(TypeKind::Invalid)
                        : Type::tensor(*left.first, 2);
                    break;
                }
                case BuiltinCallable::LinearDot: {
                    if (node->args.size() != 2) {
                        error("ARGUMENT_MISMATCH",
                              "linear.dot requires two tensors.", expression.span);
                        type = simple(TypeKind::Invalid);
                        break;
                    }
                    if (node->args[0].writable || node->args[1].writable ||
                        (node->args[0].name && *node->args[0].name != "a") ||
                        (node->args[1].name && *node->args[1].name != "b")) {
                        error("ARGUMENT_MISMATCH",
                              "linear.dot accepts a and b tensor values.", expression.span);
                    }
                    auto left = check_expr(*node->args[0].value);
                    auto right = check_expr(*node->args[1].value);
                    if (!poisoned(left) && left.kind != TypeKind::Tensor) {
                        error("TYPE_MISMATCH", "linear.dot left operand must be a tensor.",
                              node->args[0].span);
                    }
                    if (!poisoned(right) && right.kind != TypeKind::Tensor) {
                        error("TYPE_MISMATCH", "linear.dot right operand must be a tensor.",
                              node->args[1].span);
                    }
                    if (!poisoned(left) && !poisoned(right) &&
                        left.kind == TypeKind::Tensor && right.kind == TypeKind::Tensor) {
                        if (*left.first != *right.first) {
                            error("TYPE_MISMATCH",
                                  "linear.dot requires identical tensor element types.",
                                  expression.span);
                        }
                        if ((left.length >= 0 && left.length != 1) ||
                            (right.length >= 0 && right.length != 1)) {
                            error("TYPE_MISMATCH", "linear.dot requires rank-1 tensors.", expression.span);
                        }
                    }
                    type = poisoned(left) || poisoned(right)
                        ? simple(TypeKind::Invalid)
                        : *left.first;
                    break;
                }
                case BuiltinCallable::ImageRead: {
                    if (!node->type_arguments.empty()) {
                        error("GENERIC_TARGET",
                              "image.read does not take type arguments.", expression.span);
                    }
                    if (node->args.empty() || node->args.size() > 3) {
                        error("ARGUMENT_MISMATCH",
                              "image.read requires path and optional channels/dtype conversions.",
                              expression.span);
                        type = simple(TypeKind::Invalid);
                        break;
                    }

                    auto string_type = simple(TypeKind::String);
                    auto int_type = simple(TypeKind::Int);
                    auto path_type = check_expr(*node->args[0].value, &string_type);
                    bool bad = poisoned(path_type);
                    if (node->args[0].writable ||
                        (node->args[0].name && *node->args[0].name != "path")) {
                        error("ARGUMENT_MISMATCH",
                              "image.read path has an invalid label or write capability.",
                              node->args[0].span);
                        bad = true;
                    }

                    std::optional<long long> target_channels;
                    std::optional<Type> target_dtype;
                    for (std::size_t i = 1; i < node->args.size(); ++i) {
                        const auto& argument = node->args[i];
                        if (argument.writable || !argument.name) {
                            error("ARGUMENT_MISMATCH",
                                  "image.read conversion options must be named.",
                                  argument.span);
                            bad = true;
                            continue;
                        }
                        if (*argument.name == "channels") {
                            if (target_channels) {
                                error("DUPLICATE_ARGUMENT",
                                      "image.read channels is specified more than once.",
                                      argument.span);
                                bad = true;
                                continue;
                            }
                            const auto channel_type = check_expr(*argument.value, &int_type);
                            const auto channels = constant_integer_value(*argument.value);
                            if (poisoned(channel_type) || !channels ||
                                (*channels != 1 && *channels != 3 && *channels != 4)) {
                                error("ARGUMENT_MISMATCH",
                                      "image.read channels must be the literal 1, 3, or 4.",
                                      argument.span);
                                bad = true;
                            } else {
                                target_channels = *channels;
                            }
                            continue;
                        }
                        if (*argument.name == "dtype") {
                            if (target_dtype) {
                                error("DUPLICATE_ARGUMENT",
                                      "image.read dtype is specified more than once.",
                                      argument.span);
                                bad = true;
                                continue;
                            }
                            const auto* name = std::get_if<NameExpr>(&argument.value->data);
                            const auto dtype = name ? builtin_scalar_type(name->name)
                                                    : std::optional<Type>{};
                            if (!dtype || !is_numeric(*dtype)) {
                                error("ARGUMENT_MISMATCH",
                                      "image.read dtype must name a numeric built-in type.",
                                      argument.span);
                                bad = true;
                            } else {
                                target_dtype = *dtype;
                                raw_types_[argument.value.get()] = *dtype;
                                expr_types_[argument.value.get()] = *dtype;
                            }
                            continue;
                        }
                        error("ARGUMENT_MISMATCH",
                              "image.read supports only channels = 1|3|4 and dtype = numeric_type.",
                              argument.span);
                        bad = true;
                    }
                    if (bad) {
                        type = simple(TypeKind::Invalid);
                        break;
                    }

                    const auto error_type = simple(TypeKind::Error);
                    std::optional<Type> expected_tensor;
                    if (expected && expected->kind == TypeKind::Union &&
                        case_index(*expected, error_type) >= 0) {
                        std::vector<Type> non_error;
                        for (const auto& item : expected->cases) {
                            if (item != error_type) non_error.push_back(item);
                        }
                        if (non_error.size() == 1 &&
                            non_error.front().kind == TypeKind::Tensor &&
                            non_error.front().first &&
                            is_numeric(*non_error.front().first)) {
                            expected_tensor = non_error.front();
                        }
                    }

                    if (expected_tensor &&
                        !expected_tensor->tensor_shape_prefix.empty() &&
                        expected_tensor->tensor_shape_prefix.size() != 3) {
                        error("TYPE_MISMATCH",
                              "image.read returns rank-3 CHW tensors; an expected shape pattern must contain exactly three axes.",
                              expression.span);
                        type = simple(TypeKind::Invalid);
                        break;
                    }
                    if (target_dtype && expected_tensor &&
                        *expected_tensor->first != *target_dtype) {
                        error("TYPE_MISMATCH",
                              "image.read dtype conversion conflicts with the expected tensor dtype.",
                              expression.span);
                        type = simple(TypeKind::Invalid);
                        break;
                    }
                    if (target_channels && expected_tensor &&
                        !expected_tensor->tensor_shape_prefix.empty() &&
                        expected_tensor->tensor_shape_prefix.front() >= 0 &&
                        expected_tensor->tensor_shape_prefix.front() != *target_channels) {
                        error("TYPE_MISMATCH",
                              "image.read channel conversion conflicts with the expected tensor shape.",
                              expression.span);
                        type = simple(TypeKind::Invalid);
                        break;
                    }

                    std::vector<Type> result_cases;
                    const auto append_case = [&](Type element) {
                        std::vector<long long> shape_contract;
                        std::vector<long long> known_shape;
                        if (expected_tensor) {
                            shape_contract = expected_tensor->tensor_shape_prefix;
                            for (const auto extent : shape_contract) {
                                if (extent < 0) break;
                                known_shape.push_back(extent);
                            }
                        }
                        if (target_channels) {
                            if (known_shape.empty()) known_shape.push_back(*target_channels);
                            else known_shape.front() = *target_channels;
                        }
                        result_cases.push_back(Type::tensor(
                            element, 3, std::move(shape_contract), std::move(known_shape)));
                    };

                    if (target_dtype) {
                        append_case(*target_dtype);
                    } else if (expected_tensor) {
                        // The expected dtype is a source/output constraint, not permission
                        // to convert the decoded samples.
                        append_case(*expected_tensor->first);
                    } else {
                        append_case(simple(TypeKind::Int8));
                        append_case(simple(TypeKind::Int16));
                        append_case(simple(TypeKind::Int32));
                        append_case(simple(TypeKind::Int));
                        append_case(simple(TypeKind::UInt8));
                        append_case(simple(TypeKind::UInt16));
                        append_case(simple(TypeKind::UInt32));
                        append_case(simple(TypeKind::UInt64));
                        append_case(simple(TypeKind::Float32));
                        append_case(simple(TypeKind::Float));
                    }
                    result_cases.push_back(error_type);
                    type = Type::union_of(std::move(result_cases));
                    break;
                }
                case BuiltinCallable::ImageWrite: {
                    if (!node->type_arguments.empty()) {
                        error("GENERIC_TARGET",
                              "image.write does not take type arguments.", expression.span);
                    }
                    if (node->args.size() < 2 || node->args.size() > 3) {
                        error("ARGUMENT_MISMATCH",
                              "image.write requires path, image, and optional quality = int.",
                              expression.span);
                        type = simple(TypeKind::Invalid);
                        break;
                    }
                    auto string_type = simple(TypeKind::String);
                    auto int_type = simple(TypeKind::Int);
                    auto path_type = check_expr(*node->args[0].value, &string_type);
                    auto value_type = check_expr(*node->args[1].value);
                    bool bad = poisoned(path_type) || poisoned(value_type);
                    if (!poisoned(value_type) &&
                        (value_type.kind != TypeKind::Tensor || !value_type.first ||
                         !is_numeric(*value_type.first))) {
                        error("TYPE_MISMATCH",
                              "image.write requires a numeric CHW tensor.",
                              node->args[1].span);
                    }
                    if (!poisoned(value_type) && value_type.kind == TypeKind::Tensor) {
                        if (value_type.length >= 0 && value_type.length != 3) {
                            error("TYPE_MISMATCH",
                                  "image.write requires a rank-3 CHW tensor.",
                                  node->args[1].span);
                        }
                        if (!value_type.tensor_shape_prefix.empty() &&
                            value_type.tensor_shape_prefix.size() != 3) {
                            error("TYPE_MISMATCH",
                                  "image.write requires an exact-rank three-axis CHW shape pattern.",
                                  node->args[1].span);
                        }
                        if (!value_type.tensor_shape_prefix.empty()) {
                            const auto channels = value_type.tensor_shape_prefix.front();
                            if (channels >= 0 && channels != 1 && channels != 3 && channels != 4) {
                                error("TYPE_MISMATCH",
                                      "image.write requires CHW channel count 1, 3, or 4.",
                                      node->args[1].span);
                            }
                        }
                    }
                    if (node->args[0].writable || node->args[1].writable ||
                        (node->args[0].name && *node->args[0].name != "path") ||
                        (node->args[1].name && *node->args[1].name != "image")) {
                        error("ARGUMENT_MISMATCH",
                              "image.write path/image arguments have invalid labels or write capability.",
                              expression.span);
                    }
                    if (node->args.size() == 3) {
                        if (node->args[2].writable || !node->args[2].name ||
                            *node->args[2].name != "quality") {
                            error("ARGUMENT_MISMATCH",
                                  "image.write third argument must be quality = value.",
                                  node->args[2].span);
                        }
                        bad |= poisoned(check_expr(*node->args[2].value, &int_type));
                    }
                    type = bad ? simple(TypeKind::Invalid)
                               : Type::union_of({simple(TypeKind::Void),
                                                 simple(TypeKind::Error)});
                    break;
                }
                case BuiltinCallable::TensorCreate:
                case BuiltinCallable::TensorZeros:
                case BuiltinCallable::TensorOnes: {
                    if (node->type_arguments.size() != 1) {
                        error("GENERIC_ARITY",
                              "tensor construction requires exactly one numeric element type.",
                              expression.span);
                        type = simple(TypeKind::Invalid);
                        break;
                    }
                    auto element = resolve_type(node->type_arguments.front());
                    if (!is_numeric(element)) {
                        error("INVALID_TYPE", "tensor element type must be numeric.",
                              node->type_arguments.front().span);
                    }
                    if (node->args.size() != 1 || node->args[0].writable ||
                        (node->args[0].name && *node->args[0].name != "shape")) {
                        error("ARGUMENT_MISMATCH",
                              "tensor construction requires one shape array.", expression.span);
                    }
                    const auto shape_type = Type::array(simple(TypeKind::Int));
                    auto shape = check_expr(*node->args[0].value, &shape_type);
                    long long rank = -1;
                    if (!poisoned(shape)) {
                        if (const auto* literal =
                                std::get_if<ArrayExpr>(&node->args[0].value->data)) {
                            rank = static_cast<long long>(literal->elements.size());
                        } else {
                            const auto raw = raw_types_.find(node->args[0].value.get());
                            if (raw != raw_types_.end() &&
                                raw->second.kind == TypeKind::Array &&
                                raw->second.length >= 0) {
                                rank = raw->second.length;
                            }
                        }
                    }
                    std::vector<long long> known_shape;
                    if (!poisoned(shape)) {
                        if (const auto* literal =
                                std::get_if<ArrayExpr>(&node->args[0].value->data)) {
                            bool known = true;
                            for (const auto& item : literal->elements) {
                                const auto extent = constant_integer_value(*item);
                                if (!extent || *extent < 0) {
                                    known = false;
                                    break;
                                }
                                known_shape.push_back(*extent);
                            }
                            if (!known) known_shape.clear();
                        }
                    }
                    type = poisoned(shape)
                        ? simple(TypeKind::Invalid)
                        : Type::tensor(element, rank, {}, std::move(known_shape));
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
        if (!node->type_arguments.empty() && !tensor_generic_call) {
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

        if (!current_class_.empty() && find_method(current_class_, name)) {
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
            auto source = check_expr(*node->args[0].value);

            std::function<std::optional<Type>(const Type&)> cast_result =
                [&](const Type& current) -> std::optional<Type> {
                    if (is_numeric(current)) {
                        if (!explicit_numeric_cast_supported(current, *target)) return std::nullopt;
                        return *target;
                    }
                    if (current.kind == TypeKind::Array && current.first) {
                        const auto child = cast_result(*current.first);
                        if (!child) return std::nullopt;
                        return Type::array(*child, current.length);
                    }
                    if (current.kind == TypeKind::Tensor && current.first &&
                        is_numeric(*current.first) &&
                        explicit_numeric_cast_supported(*current.first, *target)) {
                        return Type::tensor(*target, current.length,
                                            current.tensor_shape_prefix,
                                            current.tensor_known_shape_prefix);
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
                    if (leaf.kind == TypeKind::Tensor && leaf.first) leaf = *leaf.first;
                    const std::string detail = is_float(leaf) && is_integer(*target)
                        ? " Floating-point to integer conversion requires math.trunc, math.round, math.floor, or math.ceil."
                        : "";
                    error("NUMERIC_CAST",
                          "Explicit cast from " + type_name(source) +
                          " to element type " + type_name(*target) +
                          " is not supported." + detail,
                          expression.span);
                    type = simple(TypeKind::Invalid);
                } else {
                    if (is_numeric(source)) {
                        if (const auto* literal =
                                std::get_if<IntegerExpr>(&node->args[0].value->data)) {
                            if (is_integer(*target) &&
                                !integer_literal_value_fits(
                                    static_cast<unsigned long long>(literal->value), *target)) {
                                error("NUMERIC_CAST",
                                      "Integer literal is outside the range of " +
                                          type_name(*target) + ".",
                                      expression.span);
                            }
                        }
                    }
                    type = *result;
                }
                call_resolutions_[&expression] =
                    CallResolution{CallKind::NumericCast, name, std::nullopt, type};
            }
        } else if (name == "bytes") {
            call_resolutions_[&expression] =
                CallResolution{CallKind::Constructor, "bytes", std::nullopt, simple(TypeKind::Bytes)};
            if (node->args.size() > 2) {
                error("ARGUMENT_MISMATCH", "bytes takes zero, one, or two arguments.", expression.span);
            }
            auto int_type = simple(TypeKind::Int);
            auto byte_type = simple(TypeKind::UInt8);
            bool any_poison = false;
            if (!node->args.empty()) {
                if (node->args[0].writable || (node->args[0].name && *node->args[0].name != "n")) {
                    error("ARGUMENT_MISMATCH", "bytes length must be positional or n = value.", node->args[0].span);
                }
                any_poison |= poisoned(check_expr(*node->args[0].value, &int_type));
                bool negative_length = false;
                if (std::holds_alternative<IntegerExpr>(node->args[0].value->data)) {
                    negative_length = false;
                } else if (const auto* unary = std::get_if<UnaryExpr>(&node->args[0].value->data);
                           unary && unary->op == "-") {
                    if (const auto* literal = std::get_if<IntegerExpr>(&unary->operand->data)) {
                        negative_length = literal->value > 0;
                    }
                }
                if (negative_length) {
                    error("ARGUMENT_MISMATCH", "bytes length cannot be negative.", node->args[0].span);
                }
            }
            if (node->args.size() == 2) {
                if (node->args[1].writable || !node->args[1].name || *node->args[1].name != "fill") {
                    error("ARGUMENT_MISMATCH", "bytes second argument must be fill = value.", node->args[1].span);
                }
                any_poison |= poisoned(check_expr(*node->args[1].value, &byte_type));
            }
            type = any_poison ? simple(TypeKind::Invalid) : simple(TypeKind::Bytes);
        } else if (classes_.contains(name)) {
            call_resolutions_[&expression] =
                CallResolution{CallKind::Constructor, name, std::nullopt, Type::class_type(name)};
            if (name.rfind("$std.", 0) == 0) {
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
            if (!functions_.contains(name) || (!name.empty() && name.front() == '$')) {
                error("UNKNOWN_NAME", "Unknown function '" + name + "'.", expression.span);
            }
            const auto& function = functions_.at(name);
            call_resolutions_[&expression] =
                CallResolution{CallKind::Function, name, std::nullopt, function.result};
            std::vector<bool> filled(function.parameters.size());
            std::size_t positional = 0;
            bool any_poison = false;
            std::vector<PendingReferenceEffect> pending_reference_effects;

            for (const auto& argument : node->args) {
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

            for (std::size_t i = 0; i < filled.size(); ++i) {
                if (!filled[i] && !function.parameters[i].default_value) {
                    error("ARGUMENT_MISMATCH",
                          "Missing required argument '" + function.parameters[i].name + "'.",
                          expression.span);
                }
            }
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

Type Checker::check_expr(const Expr& expression, const Type* expected) {
    Type type = simple(TypeKind::Void);

    if (const auto* node = std::get_if<IntegerExpr>(&expression.data)) {
        if (expected && is_integer(*expected) && integer_literal_value_fits(static_cast<unsigned long long>(node->value), *expected)) {
            type = *expected;
        } else if (expected && is_float(*expected) &&
                   integer_literal_fits_exactly_in_float(static_cast<unsigned long long>(node->value), *expected)) {
            type = *expected;
        } else {
            if (node->value >
                static_cast<unsigned long long>(std::numeric_limits<std::int64_t>::max())) {
                error("INTEGER_RANGE",
                      "Integer literal exceeds int; use an explicit uint64 context for values up to uint64 maximum.",
                      expression.span);
            }
            type = simple(TypeKind::Int);
        }
    } else if (const auto* node = std::get_if<FloatExpr>(&expression.data)) {
        if (!std::isfinite(node->value)) {
            error("FLOAT_RANGE", "Float literal must be finite.", expression.span);
        }
        if (expected && is_float(*expected) && float_value_fits_exactly(node->value, *expected)) {
            type = *expected;
        } else {
            type = simple(TypeKind::Float);
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
        type = check_name_expr(expression, *node);
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
            auto element = check_expr(*node->elements[0], context ? context->first.get() : nullptr);
            if (poisoned(element)) {
                type = element;
            } else {
                if (!is_storable(element)) {
                    error("TYPE_MISMATCH", "Array elements must be storable.", expression.span);
                }
                for (std::size_t i = 1; i < node->elements.size(); ++i) {
                    check_expr(*node->elements[i], &element);
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
        const Type* operand_expected =
            node->op == "-" && expected && is_numeric(*expected) ? expected : nullptr;
        type = check_expr(*node->operand, operand_expected);
        if (!poisoned(type)) {
            if (node->op == "not") {
                if (type.kind != TypeKind::Bool) {
                    error("TYPE_MISMATCH", "not requires bool.", expression.span);
                }
            } else if (!is_numeric(type)) {
                error("TYPE_MISMATCH", "Negation requires a number.", expression.span);
            } else if (is_integer(type) && !is_signed_integer(type)) {
                error("TYPE_MISMATCH", "Negation requires a signed numeric type.", expression.span);
            }
        }
    } else if (const auto* node = std::get_if<BinaryExpr>(&expression.data)) {
        const auto literal_kind = [](const Expr& value) {
            if (std::holds_alternative<IntegerExpr>(value.data)) return 1;
            if (std::holds_alternative<FloatExpr>(value.data)) return 2;
            if (const auto* unary = std::get_if<UnaryExpr>(&value.data);
                unary && unary->op == "-") {
                if (std::holds_alternative<IntegerExpr>(unary->operand->data)) return 1;
                if (std::holds_alternative<FloatExpr>(unary->operand->data)) return 2;
            }
            return 0;
        };

        const int left_literal = literal_kind(*node->left);
        const int right_literal = literal_kind(*node->right);
        Type left;
        Type right;

        if (left_literal && right_literal) {
            const auto context = simple(
                left_literal == 2 || right_literal == 2 ? TypeKind::Float : TypeKind::Int);
            left = check_expr(*node->left, &context);
            right = check_expr(*node->right, &context);
        } else if (left_literal) {
            right = check_expr(*node->right);
            left = is_numeric(right) ? check_expr(*node->left, &right)
                                     : check_expr(*node->left);
        } else {
            left = check_expr(*node->left);
            right = right_literal && is_numeric(left)
                ? check_expr(*node->right, &left)
                : check_expr(*node->right);
        }

        if (poisoned(left) || poisoned(right)) {
            type = simple(TypeKind::Invalid);
        } else if (left.kind == TypeKind::Neural || right.kind == TypeKind::Neural) {
            const bool left_neural = left.kind == TypeKind::Neural;
            const bool right_neural = right.kind == TypeKind::Neural;
            const Type& neural_type = left_neural ? left : right;
            const Type element = *neural_type.first;
            if (left_neural && right_neural) {
                if (left != right) error("TYPE_MISMATCH", "neural arithmetic requires identical element types.", expression.span);
            } else {
                const Type& scalar = left_neural ? right : left;
                const Expr& scalar_expr = left_neural ? *node->right : *node->left;
                bool compatible = is_numeric(scalar) && scalar == element;
                if (!compatible) {
                    if (const auto integer = constant_integer_value(scalar_expr)) {
                        compatible = is_integer(element)
                            ? integer_value_fits(*integer, element)
                            : (is_float(element) &&
                               integer_value_fits_exactly_in_float(*integer, element));
                    } else if (const auto* value = std::get_if<FloatExpr>(&scalar_expr.data)) {
                        compatible = is_float(element) &&
                            float_value_fits_exactly(value->value, element);
                    }
                }
                if (!compatible)
                    error("TYPE_MISMATCH", "neural scalar arithmetic requires an exactly representable scalar.", expression.span);
            }
            if (node->op != "+" && node->op != "-" && node->op != "*" && node->op != "/")
                error("TYPE_MISMATCH", "neural operators are elementwise +, -, *, and /.", expression.span);
            type = neural_type;
        } else if (left.kind == TypeKind::Tensor || right.kind == TypeKind::Tensor) {
            const bool left_tensor = left.kind == TypeKind::Tensor;
            const bool right_tensor = right.kind == TypeKind::Tensor;
            const Type& tensor_type = left_tensor ? left : right;
            const auto element = *tensor_type.first;
            const auto scalar_compatible = [&](const Expr& source, const Type& actual) {
                if (actual == element) return true;
                if (const auto integer = constant_integer_value(source)) {
                    if (is_integer(element)) return integer_value_fits(*integer, element);
                    if (is_float(element)) return integer_value_fits_exactly_in_float(*integer, element);
                }
                if (const auto* value = std::get_if<FloatExpr>(&source.data)) {
                    if (is_float(element)) return float_value_fits_exactly(value->value, element);
                    if (is_integer(element)) return float_value_fits_exactly_in_integer(value->value, element);
                }
                return false;
            };
            Type tensor_result = tensor_type;
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
            } else {
                const auto& scalar_type = left_tensor ? right : left;
                const auto& scalar_expr = left_tensor ? *node->right : *node->left;
                if (!is_numeric(scalar_type) || !scalar_compatible(scalar_expr, scalar_type)) {
                    error("TYPE_MISMATCH",
                          "Tensor scalar arithmetic requires an exactly representable scalar.",
                          expression.span);
                }
            }
            if (node->op != "+" && node->op != "-" && node->op != "*" &&
                node->op != "/" && node->op != "%") {
                error("TYPE_MISMATCH",
                      "Tensor operators are elementwise +, -, *, /, and %.", expression.span);
            }
            if (node->op == "%" && !is_integer(element)) {
                error("TYPE_MISMATCH", "Tensor remainder requires an integer element type.",
                      expression.span);
            }
            type = tensor_result;
        } else {
            if (left != right) {
                error("TYPE_MISMATCH", "Operands must have identical types.", expression.span);
            }
            if (node->op == "and" || node->op == "or") {
                if (left.kind != TypeKind::Bool) {
                    error("TYPE_MISMATCH", "Logical operands must be bool.", expression.span);
                }
                type = left;
            } else if (node->op == "==" || node->op == "!=") {
                if (!equality_supported(left)) {
                    error("TYPE_MISMATCH", "Equality is not defined for this type.", expression.span);
                }
                if (left.kind == TypeKind::Class &&
                    (!fully_initialized_for_equality(*node->left, left) ||
                     !fully_initialized_for_equality(*node->right, right))) {
                    error("UNINITIALIZED_FIELD_EQUALITY",
                          "Class equality requires every compared field to be definitely initialized.",
                          expression.span);
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
                if (!is_numeric(left) || (node->op == "%" && !is_integer(left))) {
                    error("TYPE_MISMATCH", "Arithmetic requires compatible numbers.", expression.span);
                }
                if (is_integer(left) && (node->op == "/" || node->op == "%")) {
                    const auto divisor = constant_integer_value(*node->right);
                    if (divisor && *divisor == 0) {
                        error("DIVIDE_BY_ZERO",
                              node->op == "/" ? "Integer division by zero is known at compile time."
                                              : "Integer remainder by zero is known at compile time.",
                              node->right->span);
                    }
                }
                type = left;
            }
        }
    } else if (const auto* node = std::get_if<TryExpr>(&expression.data)) {
        const auto error_type = simple(TypeKind::Error);
        Type source;
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
        type = check_method_call_expr(expression, *node);
    } else if (const auto* node = std::get_if<CallExpr>(&expression.data)) {
        type = check_call_expr(expression, *node, expected);
    }

    raw_types_[&expression] = type;
    if (expected && type.kind != TypeKind::Never && type.kind != TypeKind::Invalid) {
        if (!assignable(type, *expected)) {
            error("TYPE_MISMATCH",
                  "Expected " + type_name(*expected) + " but received " + type_name(type) + ".",
                  expression.span);
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
        if (variables_.contains(node.name) || functions_.contains(node.name) ||
            class_names_.contains(node.name) || is_reserved_value_name(node.name) ||
            member_name_visible(node.name)) {
            error("SHADOWING", "Name is already visible or reserved.", statement.span);
        }

        auto type = resolve_type(node.declared_type, true);

        if (node.is_const && !node.value) {
            error("CONST_INITIALIZATION", "const bindings require an initializer.", statement.span);
        }

        if (node.reference) {
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
            } else if (const auto* source = std::get_if<NameExpr>(&node.value->data);
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
            if (!node.value) error("INVALID_AUTO", "auto requires an initializer.", statement.span);
            type = check_expr(*node.value);
            if (type.kind == TypeKind::Array &&
                std::holds_alternative<ArrayExpr>(node.value->data)) {
                // Array literals infer a runtime-sized array so common auto bindings remain
                // appendable. Other expressions preserve their declared static shape contract.
                type.length = -1;
                expr_types_[node.value.get()] = type;
            }
        } else if (node.value) {
            check_expr(*node.value, &type);
            if (type.kind == TypeKind::Tensor || type.kind == TypeKind::Neural) {
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
        if (node.is_const) const_bindings_.insert(node.name);
        binding_types_[&statement] = type;
        if (node.value) {
            initialized_.insert(node.name);
            const auto paths = initialized_paths_for_expr(*node.value);
            if (!paths.empty()) {
                class_initialized_paths_[node.name] = paths;
            }
        } else if (type.kind == TypeKind::Array && type.length >= 0) {
            // A fixed array declaration creates storage immediately. Its elements are tracked
            // independently and may still be uninitialized.
            initialized_.insert(node.name);
        }
        return;
}

void Checker::check_rebind_stmt(const Stmt& statement, const RebindStmt& node) {
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
        } else if (const auto* source = std::get_if<NameExpr>(&node.target->data);
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
        if (const_access_path(*node.target)) {
            error("WRITE_CAPABILITY", "Cannot write through a const access path.", statement.span);
        }
        if (const auto* indexed = std::get_if<IndexExpr>(&node.target->data)) {
            const auto base_type = check_expr(*indexed->base);
            if (base_type.kind == TypeKind::Neural) {
                error("WRITE_CAPABILITY", "neural values are immutable; indexing is read-only.", statement.span);
            }
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
                    (node.compound_op == "%" && is_integer(read_type));
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
            if (is_builtin_text_constant(name->name)) {
                error("WRITE_CAPABILITY", "Built-in text constants are immutable.", statement.span);
            }
            if (variables_.contains(name->name)) {
                const auto root = reference_root(name->name);
                if (narrowed_.contains(root) || borrowed_.contains(root)) {
                    error("WRITE_CAPABILITY",
                          "Cannot replace a binding while narrowed or borrowed by a writable loop.",
                          statement.span);
                }
                type = variables_.at(name->name);
                expr_types_[node.target.get()] = raw_types_[node.target.get()] = type;
                auto expected = type;
                if (expected.kind == TypeKind::Tensor || expected.kind == TypeKind::Neural) {
                    if (expected.tensor_shape_prefix.empty()) expected.length = -1;
                    expected.tensor_known_shape_prefix.clear();
                }
                check_expr(*node.value, &expected);
                if (type.kind == TypeKind::Tensor || type.kind == TypeKind::Neural) {
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
                    initialized_.insert(root);
                    initialized_.insert(name->name);
                    if (type.kind == TypeKind::Class) {
                        class_initialized_paths_[root] = initialized_paths_for_expr(*node.value);
                        if (root == name->name) class_initialized_paths_[name->name] = class_initialized_paths_[root];
                    }
                }
            } else if (!current_class_.empty()) {
                const auto* field = find_field(current_class_, name->name);
                if (!field) error("UNKNOWN_NAME", "Undefined assignment target.", statement.span);
                type = field->type;
                field_accesses_[node.target.get()] = FieldAccessInfo{current_class_, field->index, field->type};
                expr_types_[node.target.get()] = raw_types_[node.target.get()] = type;
                check_expr(*node.value, &type);
                record_current_receiver_assignment(name->name, type, *node.value);
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
                error("UNKNOWN_NAME", "Undefined assignment target.", statement.span);
            }
        } else if (std::holds_alternative<IndexExpr>(node.target->data) ||
                   std::holds_alternative<MemberExpr>(node.target->data)) {
            if (!stable_writable_storage(*node.target)) {
                error("WRITE_CAPABILITY",
                      "Assignment storage must be rooted in an existing binding or receiver storage.",
                      node.target->span);
            }
            type = check_address_target(
                *node.target, std::holds_alternative<IndexExpr>(node.target->data));
            check_expr(*node.value, &type);
            if (const auto receiver_path = current_receiver_path(*node.target)) {
                if (std::holds_alternative<IndexExpr>(node.target->data)) {
                    current_receiver_effect_.writes.insert(*receiver_path);
                } else {
                    record_current_receiver_assignment(*receiver_path, type, *node.value);
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
                    if (type.kind == TypeKind::Class) {
                        for (const auto& nested : initialized_paths_for_expr(*node.value)) {
                            initialized_paths.insert(path->second + "." + nested);
                        }
                    }
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
        auto type = check_expr(*node.value);
        if (type.kind == TypeKind::Range) {
            error("RANGE_CONTEXT", "range is only a for iterable.", statement.span);
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
        auto before_paths = class_initialized_paths_;
        auto before_method = current_receiver_effect_.initializes;
        auto before_method_written = current_receiver_effect_.writes;
        auto before_method_invalidated = current_receiver_effect_.invalidates;
        auto before_reference_effects = current_reference_effects_;

        check_block(node.then_body);
        auto yes_variables = variables_;
        auto yes = initialized_;
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
        class_initialized_paths_ = before_paths;
        current_receiver_effect_.initializes = before_method;
        current_receiver_effect_.writes = before_method_written;
        current_receiver_effect_.invalidates = before_method_invalidated;
        current_reference_effects_ = before_reference_effects;
        check_block(node.else_body);
        auto no_variables = variables_;
        auto no = initialized_;
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
            if (base_type.kind == TypeKind::Tensor || base_type.kind == TypeKind::Neural) {
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
                if (yes_terminates) merged = np;
                else if (no_terminates) merged = yp;
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
        ++loop_depth_;
        check_block(node.body);
        --loop_depth_;
        auto body_variables = variables_;
        auto body_written = current_receiver_effect_.writes;
        auto body_invalidated = current_receiver_effect_.invalidates;
        variables_ = variables;
        reference_roots_ = references;
        reference_paths_ = reference_paths;
        unknown_reference_targets_ = unknown_references;
        const_bindings_ = const_bindings;
        initialized_ = initialized;
        class_initialized_paths_ = paths;
        for (const auto& [name, base_type] : variables) {
            if (base_type.kind != TypeKind::Tensor && base_type.kind != TypeKind::Neural) continue;
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
        if (type.kind != TypeKind::Array && type.kind != TypeKind::Bytes && type.kind != TypeKind::Range) {
            error("TYPE_MISMATCH", "for requires an array, bytes, or range.", statement.span);
        }
        if (variables_.contains(node.name) || functions_.contains(node.name) ||
            class_names_.contains(node.name) || is_reserved_value_name(node.name) ||
            member_name_visible(node.name)) {
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
            auto* name = std::get_if<NameExpr>(&node.iterable->data);
            if ((type.kind != TypeKind::Array && type.kind != TypeKind::Bytes) ||
                !name || !variables_.contains(name->name)) {
                error("WRITE_CAPABILITY", "Writable iteration requires an array or bytes binding.", statement.span);
            }
            const auto root = reference_root(name->name);
            if (borrowed_.contains(root) || narrowed_.contains(root)) {
                error("WRITE_CAPABILITY", "Writable iteration requires an unaliased array or bytes binding.", statement.span);
            }
            borrowed_.insert(root);
        }

        Type item_type = simple(TypeKind::Int);
        if (type.kind == TypeKind::Array) {
            item_type = *type.first;
        } else if (type.kind == TypeKind::Bytes) {
            item_type = simple(TypeKind::UInt8);
        }
        variables_[node.name] = item_type;
        initialized_.insert(node.name);
        ++loop_depth_;
        check_block(node.body);
        --loop_depth_;
        auto body_variables = variables_;
        auto body_written = current_receiver_effect_.writes;
        auto body_invalidated = current_receiver_effect_.invalidates;
        variables_ = variables;
        reference_roots_ = references;
        reference_paths_ = reference_paths;
        unknown_reference_targets_ = unknown_references;
        const_bindings_ = const_bindings;
        initialized_ = initialized;
        class_initialized_paths_ = class_paths;
        for (const auto& [name, base_type] : variables) {
            if (base_type.kind != TypeKind::Tensor && base_type.kind != TypeKind::Neural) continue;
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
        error("TYPE_MISMATCH", "match requires a union.", statement.span);
    }

    auto variables = variables_;
    auto references = reference_roots_;
    auto reference_paths = reference_paths_;
    auto unknown_references = unknown_reference_targets_;
    auto const_bindings = const_bindings_;
    auto initialized = initialized_;
    auto class_paths = class_initialized_paths_;
    auto receiver_before = current_receiver_effect_;
    auto reference_before = current_reference_effects_;
    auto narrowed = narrowed_;
    std::unordered_set<int> seen;
    std::vector<std::unordered_map<std::string, Type>> continuing_variables;
    std::vector<std::unordered_set<std::string>> continuing_initialized;
    std::vector<std::unordered_map<std::string, std::unordered_set<std::string>>>
        continuing_class_paths;
    std::vector<std::unordered_set<std::string>> continuing_receiver_initialized;
    std::vector<std::unordered_map<std::string, StorageEffect>> continuing_reference_effects;
    std::vector<ReferenceTargetState> continuing_reference_targets;
    StorageEffect matched_receiver = receiver_before;
    auto matched_reference_effects = reference_before;

    for (auto& match_case : node.cases) {
        const auto requested_case_type = resolve_type(match_case.type);
        int tag = case_index(type, requested_case_type);
        if (tag < 0 &&
            (requested_case_type.kind == TypeKind::Tensor ||
             requested_case_type.kind == TypeKind::Neural) &&
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
        variables_ = variables;
        reference_roots_ = references;
        reference_paths_ = reference_paths;
        unknown_reference_targets_ = unknown_references;
        const_bindings_ = const_bindings;
        initialized_ = initialized;
        class_initialized_paths_ = class_paths;
        current_receiver_effect_ = receiver_before;
        current_reference_effects_ = reference_before;
        narrowed_ = narrowed;

        auto matched_paths = initialized_paths_for_expr(*node.value);
        if (case_type.kind == TypeKind::Class) {
            matched_paths = complete_class_paths(case_type);
        }
        if (auto* name = std::get_if<NameExpr>(&node.value->data)) {
            variables_[name->name] = case_type;
            narrowed_.insert(reference_root(name->name));
            if (case_type.kind == TypeKind::Class) {
                class_initialized_paths_[reference_root(name->name)] = matched_paths;
            }
        }

        if (match_case.binder) {
            if (variables_.contains(*match_case.binder) || functions_.contains(*match_case.binder) ||
                class_names_.contains(*match_case.binder) ||
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
            continuing_class_paths.push_back(class_initialized_paths_);
            continuing_receiver_initialized.push_back(current_receiver_effect_.initializes);
            continuing_reference_effects.push_back(current_reference_effects_);
            continuing_reference_targets.push_back(
                ReferenceTargetState{reference_paths_, unknown_reference_targets_});
        }
    }

    if (seen.size() != type.cases.size()) {
        error("MATCH_EXHAUSTIVE", "match must cover every union case exactly once.", statement.span);
    }

    variables_ = variables;
    reference_roots_ = references;
    reference_paths_ = reference_paths;
    unknown_reference_targets_ = unknown_references;
    const_bindings_ = const_bindings;
    initialized_ = initialized;
    class_initialized_paths_ = class_paths;
    narrowed_ = narrowed;

    if (!continuing_initialized.empty()) {
        for (const auto& [name, variable_type] : variables) {
            if (variable_type.kind == TypeKind::Tensor || variable_type.kind == TypeKind::Neural) {
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
                std::unordered_set<std::string> merged;
                const auto first = continuing_class_paths.front().find(name);
                if (first != continuing_class_paths.front().end()) merged = first->second;
                for (std::size_t i = 1; i < continuing_class_paths.size(); ++i) {
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
}

void Checker::check_stmt(const Stmt& statement) {
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

bool Checker::stmt_always_terminates(const Stmt& statement) const {
    if (std::holds_alternative<ReturnStmt>(statement.data)) return true;
    if (const auto* node = std::get_if<ExprStmt>(&statement.data)) {
        return expr_types_.contains(node->value.get()) &&
               expr_types_.at(node->value.get()).kind == TypeKind::Never;
    }
    if (const auto* node = std::get_if<IfStmt>(&statement.data)) {
        return !node->else_body.empty() && block_always_terminates(node->then_body) &&
               block_always_terminates(node->else_body);
    }
    if (const auto* node = std::get_if<MatchStmt>(&statement.data)) {
        return std::all_of(node->cases.begin(), node->cases.end(),
                           [&](const auto& match_case) { return block_always_terminates(match_case.body); });
    }
    return false;
}

bool Checker::block_always_terminates(const std::vector<StmtPtr>& body) const {
    for (const auto& statement : body) {
        if (stmt_always_terminates(*statement)) return true;
    }
    return false;
}

CheckedProgram Checker::check(ConcreteProgram concrete) {
    Program program = std::move(concrete.program);
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
    classes_.clear();
    class_names_.clear();
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
    initialized_.clear();
    const_bindings_.clear();
    class_initialized_paths_.clear();
    class_expr_initialized_paths_.clear();
    reset_current_effect_state();

    narrowed_.clear();
    borrowed_.clear();
    invalid_functions_.clear();
    diagnostics_.clear();
    current_class_.clear();
    loop_depth_ = 0;

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

    std::unordered_set<std::string> building;
    std::unordered_set<std::string> invalid_classes;

    std::function<void(ClassDecl&)> build_class = [&](ClassDecl& declaration) {
        if (classes_.contains(declaration.name)) return;
        if (invalid_classes.contains(declaration.name)) {
            error("INVALID_CLASS", "Parent class is invalid.", declaration.span);
        }
        if (!building.insert(declaration.name).second) {
            error("INHERITANCE_CYCLE", "Class inheritance cannot contain a cycle.", declaration.span);
        }

        try {
            ClassTypeInfo info;
            info.name = declaration.name;
            info.parent = declaration.parent;

            if (declaration.parent) {
                const auto parent_it = class_decls.find(*declaration.parent);
                if (parent_it == class_decls.end()) {
                    error("UNKNOWN_TYPE", "Unknown parent class '" + *declaration.parent + "'.", declaration.span);
                }
                build_class(*parent_it->second);
                const auto& parent = classes_.at(*declaration.parent);
                info.fields = parent.fields;
                info.methods = parent.methods;
            }

            std::unordered_set<std::string> own_fields;
            const bool standard_generated =
                declaration.name.rfind("$std.", 0) == 0 ||
                declaration.name.rfind("__quidra_gc__std_", 0) == 0;
            for (auto& field : declaration.fields) {
                if ((!standard_generated && is_reserved_value_name(field.name)) ||
                    class_names_.contains(field.name)) {
                    error("SHADOWING", "Class field name is reserved or conflicts with a class name.", field.span);
                }
                if (!own_fields.insert(field.name).second || find_field(info.name, field.name)) {
                    error("DUPLICATE_NAME", "Duplicate class field.", field.span);
                }
                if (std::any_of(info.fields.begin(), info.fields.end(), [&](const auto& existing) { return existing.name == field.name; }) ||
                    info.methods.contains(field.name)) {
                    error("SHADOWING", "Class member cannot shadow an inherited member.", field.span);
                }
                auto field_type = resolve_type(field.type);
                if (!is_storable(field_type)) {
                    error("INVALID_TYPE", "Class fields require storable explicit types.", field.span);
                }
                info.fields.push_back(ClassFieldType{field.name, field_type, info.fields.size(), field.default_value.get(), field.is_const});
            }

            std::unordered_set<std::string> own_methods;
            for (auto& method : declaration.methods) {
                if ((!standard_generated && is_reserved_value_name(method.name)) ||
                    class_names_.contains(method.name) || method.name == "main") {
                    error("DUPLICATE_NAME", "Method name is reserved or conflicts with a class/entry point.", method.span);
                }
                if (!own_methods.insert(method.name).second) {
                    error("DUPLICATE_NAME", "Duplicate method name.", method.span);
                }
                if (std::any_of(info.fields.begin(), info.fields.end(), [&](const auto& field) { return field.name == method.name; })) {
                    error("SHADOWING", "Method name conflicts with a field.", method.span);
                }

                FunctionType public_signature;
                public_signature.result = resolve_type(method.return_type);
                if (public_signature.result.kind == TypeKind::None) {
                    error("INVALID_TYPE", "none is only a union case.", method.span);
                }

                std::unordered_set<std::string> parameter_names;
                bool defaults = false;
                for (auto& parameter : method.parameters) {
                    if (is_reserved_value_name(parameter.name) || class_names_.contains(parameter.name) ||
                        std::any_of(info.fields.begin(), info.fields.end(), [&](const auto& field) { return field.name == parameter.name; }) ||
                        info.methods.contains(parameter.name) || parameter.name == method.name) {
                        error("SHADOWING", "Method parameter shadows a class member or reserved name.", parameter.span);
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

                const auto inherited = info.methods.find(method.name);
                if (inherited != info.methods.end()) {
                    if (!method.is_override) {
                        error("OVERRIDE_REQUIRED", "Overriding an inherited method requires override.", method.span);
                    }
                    const auto& inherited_internal = functions_.at(inherited->second);
                    FunctionType inherited_public;
                    inherited_public.result = inherited_internal.result;
                    inherited_public.parameters.assign(inherited_internal.parameters.begin() + 1, inherited_internal.parameters.end());
                    if (!same_public_signature(public_signature, inherited_public)) {
                        error("OVERRIDE_MISMATCH", "override must match the inherited method signature exactly.", method.span);
                    }
                } else if (method.is_override) {
                    error("INVALID_OVERRIDE", "override requires an inherited method with the same name.", method.span);
                }

                const auto internal_name = "$method." + declaration.name + "." + method.name;
                FunctionType internal_signature;
                internal_signature.result = public_signature.result;
                internal_signature.parameters.push_back(
                    {"$receiver", Type::class_type(declaration.name), false, nullptr, false});
                internal_signature.parameters.insert(internal_signature.parameters.end(),
                                                     public_signature.parameters.begin(),
                                                     public_signature.parameters.end());
                functions_[internal_name] = std::move(internal_signature);
                info.methods[method.name] = internal_name;
            }

            classes_[declaration.name] = std::move(info);
            building.erase(declaration.name);
        } catch (...) {
            building.erase(declaration.name);
            throw;
        }
    };

    for (auto& class_decl : program.classes) {
        if (!class_names_.contains(class_decl.name)) continue;
        try {
            build_class(class_decl);
        } catch (const CompileError& compile_error) {
            record(compile_error);
            invalid_classes.insert(class_decl.name);
        }
    }

    std::unordered_set<std::string> external_c_symbols;
    for (auto& function : program.functions) {
        try {
            if (!function.type_parameters.empty()) {
                throw std::logic_error("ConcreteProgram contains an unresolved generic function.");
            }
            if (function.name == "main") {
                error("RESERVED_MAIN", "Top-level code is the entrypoint.", function.span);
            }
            if (functions_.contains(function.name) || class_names_.contains(function.name) ||
                is_reserved_value_name(function.name)) {
                error("DUPLICATE_NAME", "Reserved or duplicate function name.", function.span);
            }

            FunctionType signature;
            signature.result = resolve_type(function.return_type);
            const auto ffi_scalar=[](const Type& type) {
                switch(type.kind) {
                    case TypeKind::Int:
                    case TypeKind::Int8:
                    case TypeKind::Int16:
                    case TypeKind::Int32:
                    case TypeKind::UInt8:
                    case TypeKind::UInt16:
                    case TypeKind::UInt32:
                    case TypeKind::UInt64:
                    case TypeKind::Float:
                    case TypeKind::Float32:
                    case TypeKind::Bool:
                        return true;
                    default:
                        return false;
                }
            };
            const auto ffi_borrowed_buffer=[](const Type& type) {
                return type.kind==TypeKind::String || type.kind==TypeKind::Bytes;
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
            if (signature.result.kind == TypeKind::None) {
                error("INVALID_TYPE", "none is only a union case.", function.span);
            }

            std::unordered_set<std::string> names;
            bool defaults = false;
            for (auto& parameter : function.parameters) {
                if (is_reserved_value_name(parameter.name) || class_names_.contains(parameter.name)) {
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
                    } else if(ffi_borrowed_buffer(type)) {
                        if(!parameter.writable || !parameter.is_const)
                            error("FFI_REFERENCE","External C string/bytes inputs must be explicit call-scoped read-only borrows written as const T &.",parameter.span);
                    } else {
                        error("FFI_TYPE","External C parameters must use explicit numeric/bool scalar values or const string/bytes references.",parameter.span);
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
            if (function.external_symbol && !external_c_symbols.insert(*function.external_symbol).second) {
                error("FFI_SYMBOL_CONFLICT", "External C symbol is already bound by another extern declaration; each C symbol may be bound once per compilation.", function.span);
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
            const auto method_it = classes_.at(class_decl.name).methods.find(method.name);
            if (method_it == classes_.at(class_decl.name).methods.end()) continue;
            const auto expected_internal = "$method." + class_decl.name + "." + method.name;
            if (method_it->second != expected_internal || !functions_.contains(expected_internal)) continue;
            auto& signature = functions_.at(expected_internal);
            variables_.clear();
            reference_roots_.clear();
            reference_paths_.clear();
            unknown_reference_targets_.clear();
            initialized_.clear();
            const_bindings_.clear();
            current_class_.clear();
            in_function_ = false;
            for (std::size_t i = 1; i < signature.parameters.size(); ++i) {
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
        signature.return_initialized_fields =
            signature.result.kind == TypeKind::Class
                ? complete_class_paths(signature.result)
                : std::unordered_set<std::string>{};
    }

    const auto summaries_equal = [](const FunctionType& left, const FunctionType& right) {
        return left.receiver_effect.required == right.receiver_effect.required &&
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
                in_function_ = true;
                check_block(function.body);
                finalize_reference_effects(signature, !block_always_terminates(function.body));
                signature.return_initialized_fields =
                    signature.result.kind == TypeKind::Class && current_return_summary_seen_
                        ? current_return_initialized_
                        : std::unordered_set<std::string>{};
            }

            for (auto& class_decl : program.classes) {
                if (!classes_.contains(class_decl.name)) continue;
                for (auto& method : class_decl.methods) {
                    const auto internal_name =
                        "$method." + class_decl.name + "." + method.name;
                    if (!functions_.contains(internal_name)) continue;
                    const auto method_it = classes_.at(class_decl.name).methods.find(method.name);
                    if (method_it == classes_.at(class_decl.name).methods.end() ||
                        method_it->second != internal_name) {
                        continue;
                    }

                    variables_.clear();
                    reference_roots_.clear();
                    reference_paths_.clear();
                    unknown_reference_targets_.clear();
                    initialized_.clear();
                    const_bindings_.clear();
                    class_initialized_paths_.clear();
                    reset_current_effect_state();

                    auto& signature = functions_.at(internal_name);
                    for (std::size_t i = 1; i < signature.parameters.size(); ++i) {
                        const auto& parameter = signature.parameters[i];
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
                    current_class_ = class_decl.name;
                    in_function_ = true;
                    check_block(method.body);
                    finalize_receiver_effects(signature, !block_always_terminates(method.body));
                    finalize_reference_effects(signature, !block_always_terminates(method.body));
                    signature.return_initialized_fields =
                        signature.result.kind == TypeKind::Class && current_return_summary_seen_
                            ? current_return_initialized_
                            : std::unordered_set<std::string>{};
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
        in_function_ = true;
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
            const auto internal_name = "$method." + class_decl.name + "." + method.name;
            if (!functions_.contains(internal_name)) continue;
            if (classes_.at(class_decl.name).methods.at(method.name) != internal_name) continue;

            variables_.clear();
            reference_roots_.clear();
            reference_paths_.clear();
            unknown_reference_targets_.clear();
            initialized_.clear();
            const_bindings_.clear();
            class_initialized_paths_.clear();
            reset_current_effect_state();

            auto& signature = functions_.at(internal_name);
            for (std::size_t i = 1; i < signature.parameters.size(); ++i) {
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
            check_block(method.body);
            finalize_receiver_effects(signature, !block_always_terminates(method.body));
            finalize_reference_effects(signature, !block_always_terminates(method.body));
            signature.return_initialized_fields =
                signature.result.kind == TypeKind::Class && current_return_summary_seen_
                    ? current_return_initialized_
                    : std::unordered_set<std::string>{};
            if (signature.result.kind != TypeKind::Void && !block_always_terminates(method.body)) {
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
    class_initialized_paths_.clear();
    reset_current_effect_state();

    current_return_ = simple(TypeKind::Void);
    current_class_.clear();
    in_function_ = false;
    check_block(program.statements);

    if (!diagnostics_.empty()) {
        throw CompileErrors(std::move(diagnostics_));
    }

    return CheckedProgram{std::move(program), functions_, classes_, expr_types_, raw_types_,
                          field_accesses_, method_calls_, call_resolutions_, binding_types_, case_types_,
                          bounds_proven_, class_expr_initialized_paths_};
}

} // namespace quidra
