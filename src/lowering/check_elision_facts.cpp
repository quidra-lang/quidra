// Check elision facts: lowering-time proofs that remove runtime checks
// (check_elision_facts.hpp). Each proof answers for the function being
// lowered, from the checker's facts, the source scope and what the lowering
// recorded so far.

#include "lowering/check_elision_facts.hpp"
#include "lowering/checked_types.hpp"
#include "lowering/syntax_queries.hpp"
#include "quidra/abi/layout.hpp"
#include <algorithm>
#include <limits>

namespace quidra::lowering {

using namespace quidra::ir;

IntegerRangeFacts::IntegerRangeFacts(const CheckedProgram& checked, const LocalScope& scope)
    : checked_(checked), scope_(scope) {}

void IntegerRangeFacts::reset() {
    ranges_.clear();
    hidden_ranges_.clear();
}

ArrayBoundsProof::ArrayBoundsProof(const CheckedProgram& checked, const LocalScope& scope)
    : checked_(checked), scope_(scope) {}

void ArrayBoundsProof::reset() {
    reference_bounds_.clear();
    active_ranges_.clear();
    scalar_length_of_array_.clear();
    array_length_from_scalar_.clear();
}

ArrayInitializationProof::ArrayInitializationProof(const CheckedProgram& checked, const LocalScope& scope)
    : checked_(checked), scope_(scope) {}

void ArrayInitializationProof::reset() {
    full_arrays_.clear();
    reference_initialization_.clear();
}

CheckElisionFacts::CheckElisionFacts(const CheckedProgram& checked, const LocalScope& scope)
    : integer_ranges(checked, scope), array_bounds(checked, scope),
      array_initialization(checked, scope), checked_(checked) {}

void CheckElisionFacts::reset() {
    array_initialization.reset();
    array_bounds.reset();
    integer_ranges.reset();
}

void CheckElisionFacts::record_local_store(
    const std::string& name, const Type& type, const Expr& value, bool array_full) {
    array_bounds.invalidate_length_relation(name);
    if (type.kind == TypeKind::Int64 || is_bare_integer(type)) {
        if (const auto source = array_bounds.direct_array_length_source(value))
            array_bounds.record_scalar_length(name, *source);
    }
    if (type.kind == TypeKind::Array) {
        if (array_full) array_initialization.mark_full(name);
        else array_initialization.forget_full(name);
        if (const auto* array_call =
                std::get_if<CallExpr>(&value.data)) {
            const auto resolution =
                checked_.call_resolutions.find(&value);
            if (resolution != checked_.call_resolutions.end() &&
                resolution->second.kind == CallKind::Builtin &&
                resolution->second.builtin == BuiltinCallable::Array &&
                !array_call->args.empty() &&
                array_call->args.front().value) {
                if (const auto* length_name =
                        std::get_if<NameExpr>(
                            &array_call->args.front().value->data)) {
                    array_bounds.record_array_length(name,
                        source_key(*length_name));
                }
            }
        }
    }
}

std::unordered_set<std::string> intersect_full_arrays(
    const std::unordered_set<std::string>& left,
    const std::unordered_set<std::string>& right) {
    std::unordered_set<std::string> result;
    const auto& small = left.size() <= right.size() ? left : right;
    const auto& large = left.size() <= right.size() ? right : left;
    for (const auto& name : small) if (large.contains(name)) result.insert(name);
    return result;
}

std::optional<NonnegativeIntegerRange> IntegerRangeFacts::fixed_nonnegative_range(
    const Type& type) const {
    switch (type.kind) {
        case TypeKind::Bool: return NonnegativeIntegerRange{0, 1};
        case TypeKind::Nat8: return NonnegativeIntegerRange{0, 255};
        case TypeKind::Nat16: return NonnegativeIntegerRange{0, 65535};
        case TypeKind::Nat32:
            return NonnegativeIntegerRange{
                0, std::numeric_limits<std::uint32_t>::max()};
        default: return std::nullopt;
    }
}

namespace {

// The largest value an int64 range or a bare integer range may reach: the
// int64 maximum, and the largest inline value of a bare integer.
constexpr auto int64_limit =
    static_cast<std::uint64_t>(std::numeric_limits<long long>::max());
constexpr auto inline_limit =
    static_cast<std::uint64_t>(abi::bare_integer_layout::inline_max);

bool range_tracked(const Type& type) {
    return type.kind == TypeKind::Int64 || is_bare_integer(type);
}

std::uint64_t range_limit(const Type& type) {
    return is_bare_integer(type) ? inline_limit : int64_limit;
}

} // namespace

std::optional<NonnegativeIntegerRange> IntegerRangeFacts::nonnegative_integer_range(
    const Expr& expression,
    const std::unordered_map<std::string, NonnegativeIntegerRange>& facts) const {
    if (const auto fixed = fixed_nonnegative_range(type_of(checked_, expression)))
        return fixed;

    const auto signed_limit = range_limit(type_of(checked_, expression));

    if (const auto* literal = std::get_if<IntegerExpr>(&expression.data)) {
        if (!literal->fits_u64 || literal->value > signed_limit)
            return std::nullopt;
        return NonnegativeIntegerRange{literal->value, literal->value};
    }

    if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        if (checked_.field_accesses.contains(&expression) ||
            scope_.is_source_reference(name->name))
            return std::nullopt;
        const auto found = facts.find(source_key(*name));
        if (found != facts.end()) return found->second;
        return std::nullopt;
    }

    if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
        const auto resolution = checked_.call_resolutions.find(&expression);
        if (resolution != checked_.call_resolutions.end() &&
            resolution->second.kind == CallKind::NumericCast &&
            range_tracked(type_of(checked_, expression)) &&
            call->args.size() == 1 && call->args.front().value) {
            const auto source =
                nonnegative_integer_range(*call->args.front().value, facts);
            if (source && source->maximum <= signed_limit) return source;
        }
        return std::nullopt;
    }

    const auto* binary = std::get_if<BinaryExpr>(&expression.data);
    if (!binary || !range_tracked(type_of(checked_, expression)))
        return std::nullopt;
    const auto left = nonnegative_integer_range(*binary->left, facts);
    const auto right = nonnegative_integer_range(*binary->right, facts);
    if (!left || !right) return std::nullopt;

    if (binary->op == "+") {
        if (left->maximum > signed_limit - right->maximum)
            return std::nullopt;
        return NonnegativeIntegerRange{
            left->minimum + right->minimum,
            left->maximum + right->maximum};
    }
    if (binary->op == "-") {
        if (left->minimum < right->maximum)
            return std::nullopt;
        return NonnegativeIntegerRange{
            left->minimum - right->maximum,
            left->maximum - right->minimum};
    }
    if (binary->op == "*") {
        if (left->maximum != 0 &&
            right->maximum > signed_limit / left->maximum)
            return std::nullopt;
        return NonnegativeIntegerRange{
            left->minimum * right->minimum,
            left->maximum * right->maximum};
    }
    if ((binary->op == "%" || binary->op == "/") &&
        right->minimum == right->maximum && right->minimum != 0) {
        const auto divisor = right->minimum;
        if (binary->op == "%") {
            return NonnegativeIntegerRange{
                0, std::min(left->maximum, divisor - 1)};
        }
        return NonnegativeIntegerRange{
            left->minimum / divisor, left->maximum / divisor};
    }
    return std::nullopt;
}

std::optional<NonnegativeIntegerRange> IntegerRangeFacts::nonnegative_integer_range(
    const Expr& expression) const {
    return nonnegative_integer_range(
        expression, ranges_);
}

bool IntegerRangeFacts::overflow_proven(const Expr& expression) const {
    const auto* binary = std::get_if<BinaryExpr>(&expression.data);
    if (!binary || type_of(checked_, expression).kind != TypeKind::Int64 ||
        (binary->op != "+" && binary->op != "-" && binary->op != "*"))
        return false;
    return nonnegative_integer_range(expression).has_value();
}

std::optional<std::uint64_t> IntegerRangeFacts::magnitude_bound(
    const Expr& expression) const {
    const auto type = type_of(checked_, expression);
    if (is_fixed_integer(type) && integer_width(type) <= 32) {
        // Every value of a fixed-width integer of at most 32 bits.
        return std::uint64_t{1} << integer_width(type);
    }
    if (const auto range = nonnegative_integer_range(expression))
        return range->maximum;
    if (const auto* literal = std::get_if<IntegerExpr>(&expression.data)) {
        if (!literal->fits_u64) return std::nullopt;
        return literal->value;
    }
    if (const auto* unary = std::get_if<UnaryExpr>(&expression.data)) {
        if (unary->op != "-" || !unary->operand) return std::nullopt;
        return magnitude_bound(*unary->operand);
    }
    if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
        const auto resolution = checked_.call_resolutions.find(&expression);
        if (resolution == checked_.call_resolutions.end()) return std::nullopt;
        if (resolution->second.kind == CallKind::Builtin &&
            resolution->second.builtin == BuiltinCallable::Len)
            return inline_limit;  // the declared result range of len
        if (resolution->second.kind == CallKind::NumericCast &&
            call->args.size() == 1 && call->args.front().value)
            return magnitude_bound(*call->args.front().value);
        return std::nullopt;
    }
    const auto* binary = std::get_if<BinaryExpr>(&expression.data);
    if (!binary || !binary->left || !binary->right) return std::nullopt;
    const auto left = magnitude_bound(*binary->left);
    const auto right = magnitude_bound(*binary->right);
    if (!left || !right) return std::nullopt;
    constexpr auto unbounded = std::numeric_limits<std::uint64_t>::max();
    if (binary->op == "+" || binary->op == "-") {
        if (*left > unbounded - *right) return std::nullopt;
        return *left + *right;
    }
    if (binary->op == "*") {
        if (*left != 0 && *right > unbounded / *left) return std::nullopt;
        return *left * *right;
    }
    if (binary->op == "/") return *left;
    if (binary->op == "%") return std::min(*left, *right);
    return std::nullopt;
}

bool IntegerRangeFacts::conversion_inline_proven(
    const Expr& argument, const Type& source, const Type& target) const {
    if (target.kind == TypeKind::Int && is_fixed_integer(source)) {
        const auto bound = magnitude_bound(argument);
        return bound && *bound <= inline_limit;
    }
    if (!is_bare_integer(source) || !is_fixed_integer(target)) return false;
    const auto width = integer_width(target);
    if (is_signed_integer(target)) {
        const auto bound = magnitude_bound(argument);
        const auto limit = static_cast<std::uint64_t>(signed_integer_max(width));
        return bound && *bound <= limit && *bound <= inline_limit;
    }
    const auto range = nonnegative_integer_range(argument);
    return range && range->maximum <= unsigned_integer_max(width) &&
           range->maximum <= inline_limit;
}

bool IntegerRangeFacts::inline_bounded(const Expr& expression) const {
    const auto bound = magnitude_bound(expression);
    return bound && *bound <= inline_limit;
}

void IntegerRangeFacts::enter_range_loop(
    const std::string& name, const std::vector<StmtPtr>& body,
    const Expr* start, const Expr& end) {
    // The loop variable hides any fact of another local of its name while
    // the body runs; leave_range_loop restores it.
    const auto previous = ranges_.find(name);
    hidden_ranges_.push_back(HiddenRange{
        name, previous == ranges_.end() ? std::nullopt
                                        : std::optional<NonnegativeIntegerRange>{previous->second}});
    if (previous != ranges_.end()) ranges_.erase(previous);
    std::uint64_t minimum = 0;
    if (start) {
        const auto range = nonnegative_integer_range(*start);
        if (!range) return;
        minimum = range->minimum;
    }
    const auto end_range = nonnegative_integer_range(end);
    const auto end_bound = end_range ? std::optional<std::uint64_t>{end_range->maximum}
                                     : magnitude_bound(end);
    if (!end_bound || *end_bound > inline_limit || *end_bound == 0) return;
    IntegerRangeCandidate candidate{};
    inspect_integer_range_candidate(body, name, ranges_, candidate);
    if (candidate.invalid || candidate.assignments != 0) return;
    if (minimum > *end_bound - 1) return;
    ranges_[name] = NonnegativeIntegerRange{minimum, *end_bound - 1};
}

void IntegerRangeFacts::leave_range_loop(const std::string& name) {
    ranges_.erase(name);
    if (hidden_ranges_.empty() || hidden_ranges_.back().name != name) return;
    if (hidden_ranges_.back().range) ranges_[name] = *hidden_ranges_.back().range;
    hidden_ranges_.pop_back();
}

bool IntegerRangeFacts::inline_proven(const Expr& expression) const {
    const auto inline_bound = [&](const Expr& operand) {
        const auto bound = magnitude_bound(operand);
        return bound && *bound <= inline_limit;
    };
    if (const auto* unary = std::get_if<UnaryExpr>(&expression.data)) {
        return unary->op == "-" && unary->operand &&
               type_of(checked_, expression).kind == TypeKind::Int &&
               inline_bound(*unary->operand);
    }
    const auto* binary = std::get_if<BinaryExpr>(&expression.data);
    if (!binary || !binary->left || !binary->right ||
        !is_bare_integer(type_of(checked_, *binary->left)))
        return false;
    if (!inline_bound(*binary->left) || !inline_bound(*binary->right)) return false;
    if (binary->op == "+" || binary->op == "-" || binary->op == "*")
        return inline_bound(expression);
    return binary->op == "<" || binary->op == "<=" || binary->op == ">" ||
           binary->op == ">=" || binary->op == "==" || binary->op == "!=";
}

void IntegerRangeFacts::inspect_integer_range_candidate(
    const std::vector<StmtPtr>& body, const std::string& name,
    const std::unordered_map<std::string, NonnegativeIntegerRange>& constants,
    IntegerRangeCandidate& candidate) const {
    const std::unordered_set<std::string> rooted_name{name};
    const auto inspect_expression = [&](const Expr& expression) {
        return expression_may_replace_array_reference(
            expression, rooted_name);
    };

    for (const auto& statement : body) {
        const auto& data = statement->data;
        if (const auto* binding = std::get_if<BindingStmt>(&data)) {
            if (binding->value &&
                ((binding->reference || binding->reference_initializer) &&
                 storage_root_is(*binding->value, name)))
                candidate.invalid = true;
            if (binding->value && inspect_expression(*binding->value))
                candidate.invalid = true;
        } else if (const auto* assign = std::get_if<AssignStmt>(&data)) {
            const auto* target =
                std::get_if<NameExpr>(&assign->target->data);
            if (target && source_key(*target) == name &&
                !checked_.field_accesses.contains(assign->target.get()) &&
                !scope_.is_source_reference(target->name)) {
                ++candidate.assignments;
                if (!assign->compound_op.empty()) {
                    candidate.invalid = true;
                } else {
                    const auto* modulo =
                        std::get_if<BinaryExpr>(&assign->value->data);
                    if (!modulo || modulo->op != "%") {
                        candidate.invalid = true;
                    } else {
                        const auto divisor =
                            nonnegative_integer_range(
                                *modulo->right, constants);
                        if (!divisor ||
                            divisor->minimum != divisor->maximum ||
                            divisor->minimum == 0) {
                            candidate.invalid = true;
                        } else {
                            if (candidate.modulus &&
                                *candidate.modulus != divisor->minimum)
                                candidate.invalid = true;
                            candidate.modulus = divisor->minimum;
                            candidate.numerators.push_back(
                                modulo->left.get());
                        }
                    }
                }
            }
            if (inspect_expression(*assign->target) ||
                inspect_expression(*assign->value))
                candidate.invalid = true;
        } else if (const auto* rebind =
                       std::get_if<RebindStmt>(&data)) {
            if (storage_root_is(*rebind->target, name) ||
                inspect_expression(*rebind->target))
                candidate.invalid = true;
        } else if (const auto* returned =
                       std::get_if<ReturnStmt>(&data)) {
            if (returned->value && inspect_expression(*returned->value))
                candidate.invalid = true;
        } else if (const auto* expression =
                       std::get_if<ExprStmt>(&data)) {
            if (inspect_expression(*expression->value))
                candidate.invalid = true;
        } else if (const auto* branch = std::get_if<IfStmt>(&data)) {
            if (inspect_expression(*branch->condition))
                candidate.invalid = true;
            inspect_integer_range_candidate(
                branch->then_body, name, constants, candidate);
            inspect_integer_range_candidate(
                branch->else_body, name, constants, candidate);
        } else if (const auto* loop = std::get_if<WhileStmt>(&data)) {
            if (inspect_expression(*loop->condition))
                candidate.invalid = true;
            inspect_integer_range_candidate(
                loop->body, name, constants, candidate);
        } else if (const auto* loop = std::get_if<ForStmt>(&data)) {
            if ((loop->writable &&
                 storage_root_is(*loop->iterable, name)) ||
                inspect_expression(*loop->iterable))
                candidate.invalid = true;
            inspect_integer_range_candidate(
                loop->body, name, constants, candidate);
        } else if (const auto* match = std::get_if<MatchStmt>(&data)) {
            if (inspect_expression(*match->value))
                candidate.invalid = true;
            for (const auto& match_case : match->cases)
                inspect_integer_range_candidate(
                    match_case.body, name, constants, candidate);
        }
    }
}

void IntegerRangeFacts::prepare(const std::vector<StmtPtr>& body) {
    ranges_.clear();

    // Function-scope const integers are immutable range facts and may also
    // serve as exact positive divisors in modulo invariants.
    for (const auto& statement : body) {
        const auto* binding =
            std::get_if<BindingStmt>(&statement->data);
        if (!binding || !binding->is_const || binding->reference ||
            !binding->value)
            continue;
        const auto type = checked_.binding_types.find(statement.get());
        if (type == checked_.binding_types.end() ||
            !range_tracked(type->second))
            continue;
        if (const auto range =
                nonnegative_integer_range(
                    *binding->value, ranges_))
            ranges_[binding->name] = *range;
    }

    struct PendingCandidate {
        std::string name;
        IntegerRangeCandidate candidate;
    };
    std::vector<PendingCandidate> pending;

    for (const auto& statement : body) {
        const auto* binding =
            std::get_if<BindingStmt>(&statement->data);
        if (!binding || binding->is_const || binding->reference ||
            !binding->value)
            continue;
        const auto type = checked_.binding_types.find(statement.get());
        if (type == checked_.binding_types.end() ||
            !range_tracked(type->second))
            continue;
        const auto initial =
            nonnegative_integer_range(
                *binding->value, ranges_);
        if (!initial) continue;
        IntegerRangeCandidate candidate{*initial, std::nullopt, {}, 0, false};
        inspect_integer_range_candidate(
            body, binding->name,
            ranges_, candidate);
        pending.push_back(
            PendingCandidate{binding->name, std::move(candidate)});
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& item : pending) {
            if (ranges_.contains(item.name) ||
                item.candidate.invalid)
                continue;

            if (item.candidate.assignments == 0) {
                ranges_[item.name] =
                    item.candidate.initial;
                changed = true;
                continue;
            }
            if (!item.candidate.modulus ||
                item.candidate.initial.maximum >=
                    *item.candidate.modulus)
                continue;

            auto provisional = ranges_;
            provisional[item.name] =
                NonnegativeIntegerRange{
                    0, *item.candidate.modulus - 1};

            bool safe = true;
            for (const auto* numerator : item.candidate.numerators) {
                if (!nonnegative_integer_range(
                        *numerator, provisional)) {
                    safe = false;
                    break;
                }
            }
            if (!safe) continue;

            ranges_[item.name] =
                provisional[item.name];
            changed = true;
        }
    }
}

void ArrayInitializationProof::cache_reference_initialization(
    FunctionBuilder& builder, const FunctionType& signature,
    const std::vector<StmtPtr>& body) {
    std::unordered_set<std::string> array_references;
    for (const auto& parameter : signature.parameters) {
        if (parameter.writable && parameter.type.kind == TypeKind::Array) {
            array_references.insert(parameter.name);
        }
    }
    if (array_references.empty() ||
        block_may_replace_array_reference(body, array_references)) {
        return;
    }
    for (const auto& parameter : signature.parameters) {
        if (!array_references.contains(parameter.name)) continue;
        // A reference parameter is always bound at function entry even when
        // some of the array's elements are not initialized. Cache only the
        // runtime "all elements initialized" bit; false keeps the ordinary
        // per-element check/mark path, while true remains valid as long as
        // the binding itself cannot be replaced or escaped.
        const auto array = builder.fresh();
        builder.emit(LoadLocal{array, parameter.name, parameter.type});
        const auto complete = builder.fresh();
        builder.emit(ArrayInitializationComplete{complete, array});
        reference_initialization_[parameter.name] = complete;
    }
}

std::optional<ValueId> ArrayInitializationProof::reference_guard(
    const Expr& expression) const {
    const auto* name = std::get_if<NameExpr>(&expression.data);
    if (!name || checked_.field_accesses.contains(&expression) ||
        scope_.is_source_reference(name->name)) {
        return std::nullopt;
    }
    const auto cached = reference_initialization_.find(source_key(*name));
    if (cached == reference_initialization_.end()) return std::nullopt;
    return cached->second;
}

void ArrayBoundsProof::cache_reference_bounds(
    FunctionBuilder& builder, const FunctionType& signature,
    const std::vector<StmtPtr>& body) {
    std::vector<std::pair<std::string, Type>> stable_bounds;
    for (const auto& parameter : signature.parameters) {
        if (parameter.writable ||
            (parameter.type.kind != TypeKind::Int64 && !is_bare_integer(parameter.type)))
            continue;
        const std::unordered_set<std::string> name{parameter.name};
        if (!block_may_replace_array_reference(body, name))
            stable_bounds.emplace_back(parameter.name, parameter.type);
    }
    if (stable_bounds.empty()) return;

    for (const auto& parameter : signature.parameters) {
        if (!parameter.writable || !parameter.is_const ||
            parameter.type.kind != TypeKind::Array)
            continue;
        const std::unordered_set<std::string> array_name{parameter.name};
        if (block_may_replace_array_reference(body, array_name))
            continue;

        auto array = builder.fresh();
        builder.emit(
            LoadLocal{array, parameter.name, parameter.type});
        auto length = builder.fresh();
        builder.emit(ArrayLength{length, array});

        for (const auto& [bound_name, bound_type] : stable_bounds) {
            auto bound = builder.fresh();
            builder.emit(LoadLocal{bound, bound_name, bound_type});
            // An int bound is compared as a word: the length becomes one
            // (inline, being a length), and a boxed bound exceeds it.
            auto limit = length;
            if (bound_type.kind != TypeKind::Int64) {
                limit = builder.fresh();
                builder.emit(NumericConvert{
                    limit, length, Type::simple(TypeKind::Int64), bound_type, false, 0, 0, true});
            }
            auto safe = builder.fresh();
            builder.emit(Binary{
                safe, "<=", bound, limit, bound_type,
                Type::simple(TypeKind::Bool)});
            reference_bounds_[parameter.name][bound_name] = safe;
        }
    }
}

std::optional<ValueId> ArrayBoundsProof::reference_guard(
    const Expr& base, const Expr& index) const {
    const auto* base_name = std::get_if<NameExpr>(&base.data);
    const auto* index_name = std::get_if<NameExpr>(&index.data);
    if (!base_name || !index_name ||
        checked_.field_accesses.contains(&base) ||
        scope_.is_source_reference(base_name->name))
        return std::nullopt;

    const auto array = reference_bounds_.find(source_key(*base_name));
    if (array == reference_bounds_.end()) return std::nullopt;

    for (auto range = active_ranges_.rbegin();
         range != active_ranges_.rend(); ++range) {
        if (range->index_name != source_key(*index_name) || !range->end)
            continue;
        const auto* bound =
            std::get_if<NameExpr>(&range->end->data);
        if (!bound) continue;
        const auto guard = array->second.find(source_key(*bound));
        if (guard != array->second.end()) return guard->second;
    }
    return std::nullopt;
}

const Expr& ArrayBoundsProof::without_exact_length_cast(const Expr& expression) const {
    // int(len(a)): converting a nat length to another integer kind keeps
    // its value, so the length relation holds through it.
    const auto* cast = std::get_if<CallExpr>(&expression.data);
    if (!cast || cast->args.size() != 1 || !cast->args.front().value) return expression;
    const auto resolution = checked_.call_resolutions.find(&expression);
    if (resolution == checked_.call_resolutions.end() ||
        resolution->second.kind != CallKind::NumericCast ||
        type_of(checked_, *cast->args.front().value).kind != TypeKind::Nat ||
        !is_bare_integer(type_of(checked_, expression)))
        return expression;
    return *cast->args.front().value;
}

std::optional<std::string> ArrayBoundsProof::direct_array_length_source(
    const Expr& source) const {
    const Expr& expression = without_exact_length_cast(source);
    const auto* call = std::get_if<CallExpr>(&expression.data);
    if (!call || call->callee != "len" || call->args.size() != 1 ||
        !call->args.front().value)
        return std::nullopt;
    const auto resolution = checked_.call_resolutions.find(&expression);
    if (resolution == checked_.call_resolutions.end() ||
        resolution->second.kind != CallKind::Builtin ||
        resolution->second.builtin != BuiltinCallable::Len)
        return std::nullopt;
    const auto* name =
        std::get_if<NameExpr>(&call->args.front().value->data);
    if (!name || type_of(checked_, *call->args.front().value).kind != TypeKind::Array)
        return std::nullopt;
    return source_key(*name);
}

void ArrayBoundsProof::invalidate_length_relation(const std::string& name) {
    scalar_length_of_array_.erase(name);
    array_length_from_scalar_.erase(name);
    for (auto it = scalar_length_of_array_.begin();
         it != scalar_length_of_array_.end();) {
        if (it->second == name) it = scalar_length_of_array_.erase(it);
        else ++it;
    }
    for (auto it = array_length_from_scalar_.begin();
         it != array_length_from_scalar_.end();) {
        if (it->second == name) it = array_length_from_scalar_.erase(it);
        else ++it;
    }
}

bool ArrayBoundsProof::scalar_names_array_length(
    const std::string& scalar, const std::string& array) const {
    const auto scalar_relation = scalar_length_of_array_.find(scalar);
    if (scalar_relation != scalar_length_of_array_.end() &&
        scalar_relation->second == array)
        return true;
    const auto array_relation = array_length_from_scalar_.find(array);
    return array_relation != array_length_from_scalar_.end() &&
           array_relation->second == scalar;
}

std::optional<long long> ArrayBoundsProof::range_end_distance_from_array_length(
    const Expr& end, const std::string& array) const {
    if (const auto direct = direct_array_length_source(end);
        direct && *direct == array)
        return 0;
    if (const auto* name = std::get_if<NameExpr>(&end.data)) {
        if (scalar_names_array_length(source_key(*name), array)) return 0;
        return std::nullopt;
    }
    const auto* binary = std::get_if<BinaryExpr>(&end.data);
    if (!binary || binary->op != "-") return std::nullopt;
    const auto* amount =
        std::get_if<IntegerExpr>(&binary->right->data);
    if (!amount || !amount->fits_u64 ||
        amount->value >
            static_cast<std::uint64_t>(std::numeric_limits<long long>::max()))
        return std::nullopt;
    const auto distance = static_cast<long long>(amount->value);
    if (const auto direct = direct_array_length_source(*binary->left);
        direct && *direct == array)
        return distance;
    const auto* name =
        std::get_if<NameExpr>(&binary->left->data);
    if (name && scalar_names_array_length(source_key(*name), array))
        return distance;
    return std::nullopt;
}

bool ArrayBoundsProof::range_bound_stable(
    const ActiveRangeBound& range, const std::string& array) const {
    if (!range.body) return false;

    const std::unordered_set<std::string> array_name{array};
    if (block_may_replace_array_reference(*range.body, array_name))
        return false;

    const auto scalar_is_stable = [&](const std::string& name) {
        const std::unordered_set<std::string> scalar_name{name};
        return !block_may_replace_array_reference(
            *range.body, scalar_name);
    };

    if (range.end) {
        const Expr* relation = range.end;
        if (const auto* binary =
                std::get_if<BinaryExpr>(&relation->data);
            binary && binary->op == "-") {
            relation = binary->left.get();
        }
        if (const auto* name =
                std::get_if<NameExpr>(&relation->data);
            name && !scalar_is_stable(source_key(*name))) {
            return false;
        }
    }

    if (const auto related = array_length_from_scalar_.find(array);
        related != array_length_from_scalar_.end() &&
        !scalar_is_stable(related->second)) {
        return false;
    }
    return true;
}

bool ArrayBoundsProof::proven_by_range_loop(
    const Expr& base, const Expr& index) const {
    const auto* base_name = std::get_if<NameExpr>(&base.data);
    if (!base_name || checked_.field_accesses.contains(&base) ||
        scope_.is_source_reference(base_name->name))
        return false;

    for (auto range = active_ranges_.rbegin();
         range != active_ranges_.rend(); ++range) {
        if (!range->end ||
            !range_bound_stable(*range, source_key(*base_name)))
            continue;
        const auto distance =
            range_end_distance_from_array_length(
                *range->end, source_key(*base_name));
        if (!distance || *distance < 0) continue;

        if (const auto* name = std::get_if<NameExpr>(&index.data)) {
            if (source_key(*name) == range->index_name)
                return true;
        }

        if (const auto* binary = std::get_if<BinaryExpr>(&index.data)) {
            if (binary->op == "+") {
                const NameExpr* name =
                    std::get_if<NameExpr>(&binary->left->data);
                const IntegerExpr* amount =
                    std::get_if<IntegerExpr>(&binary->right->data);
                if (!name || !amount) {
                    name = std::get_if<NameExpr>(&binary->right->data);
                    amount = std::get_if<IntegerExpr>(&binary->left->data);
                }
                if (name && amount && amount->fits_u64 &&
                    source_key(*name) == range->index_name &&
                    amount->value <=
                        static_cast<std::uint64_t>(*distance)) {
                    return true;
                }
            }

            // len(array) - 1 - i is in [0, len(array)) for
            // i in range(0, len(array)).
            if (binary->op == "-") {
                const auto* loop_index =
                    std::get_if<NameExpr>(&binary->right->data);
                const auto* prefix =
                    std::get_if<BinaryExpr>(&binary->left->data);
                if (loop_index &&
                    source_key(*loop_index) == range->index_name &&
                    prefix && prefix->op == "-") {
                    const auto* one =
                        std::get_if<IntegerExpr>(&prefix->right->data);
                    if (one && one->fits_u64 && one->value == 1) {
                        if (const auto direct =
                                direct_array_length_source(*prefix->left);
                            direct && *direct == source_key(*base_name))
                            return true;
                        if (const auto* length_name =
                                std::get_if<NameExpr>(&prefix->left->data);
                            length_name &&
                            scalar_names_array_length(
                                source_key(*length_name), source_key(*base_name)))
                            return true;
                    }
                }
            }
        }
    }
    return false;
}

bool ArrayInitializationProof::fully_initialized(const Expr& expression) const {
    const auto type = type_of(checked_, expression);
    if (type.kind != TypeKind::Array) return false;
    if (std::holds_alternative<ArrayExpr>(expression.data)) return true;
    if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        if (checked_.field_accesses.contains(&expression) || scope_.is_source_reference(name->name)) {
            return false;
        }
        return full_arrays_.contains(source_key(*name));
    }
    if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
        const auto found = checked_.call_resolutions.find(&expression);
        if (found == checked_.call_resolutions.end()) return false;
        if (found->second.kind == CallKind::Builtin &&
            found->second.builtin == BuiltinCallable::Array &&
            call->args.size() == 2) {
            return true;
        }
        if (found->second.kind == CallKind::NumericCast &&
            call->args.size() == 1 && call->args.front().value &&
            type_of(checked_, *call->args.front().value).kind == TypeKind::Bin) {
            // bin -> T[] materializes every destination element.
            return true;
        }
        return false;
    }
    if (const auto* method = std::get_if<MethodCallExpr>(&expression.data)) {
        const auto receiver_type = type_of(checked_, *method->receiver);
        if (receiver_type.kind == TypeKind::Array &&
            (method->method == "append" || method->method == "concat" ||
             method->method == "sorted")) {
            return true;
        }
        if (receiver_type.kind == TypeKind::String &&
            (method->method == "split" || method->method == "codepoints")) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> ArrayInitializationProof::full_array_written_by_for(const ForStmt& loop) const {
    if (loop.writable || loop.body.size() != 1) return std::nullopt;

    const auto* range = std::get_if<CallExpr>(&loop.iterable->data);
    const auto range_resolution = checked_.call_resolutions.find(loop.iterable.get());
    if (!range || range_resolution == checked_.call_resolutions.end() ||
        range_resolution->second.kind != CallKind::Builtin ||
        range_resolution->second.builtin != BuiltinCallable::Range) {
        return std::nullopt;
    }
    for (const auto& argument : range->args) {
        if (argument.name || argument.writable) return std::nullopt;
    }

    const Expr* limit = nullptr;
    if (range->args.size() == 1) {
        limit = range->args[0].value.get();
    } else if (range->args.size() == 2) {
        const auto* zero = std::get_if<IntegerExpr>(&range->args[0].value->data);
        if (!zero || zero->value != 0) return std::nullopt;
        limit = range->args[1].value.get();
    } else {
        return std::nullopt;
    }

    const auto* assignment = std::get_if<AssignStmt>(&loop.body.front()->data);
    if (!assignment || !assignment->compound_op.empty()) return std::nullopt;
    const auto* indexed = std::get_if<IndexExpr>(&assignment->target->data);
    if (!indexed || indexed->items.size() != 1 || !indexed->items[0].index ||
        indexed->items[0].start || indexed->items[0].stop || indexed->items[0].step) {
        return std::nullopt;
    }
    const auto* array_name = std::get_if<NameExpr>(&indexed->base->data);
    const auto* index_name = std::get_if<NameExpr>(&indexed->items[0].index->data);
    if (!array_name || !index_name || source_key(*index_name) != loop.name ||
        array_name->this_qualifier ||
        !scope_.has_source_local(array_name->name) ||
        scope_.is_source_reference(array_name->name) ||
        type_of(checked_, *indexed->base).kind != TypeKind::Array ||
        expression_contains_writable_argument(*assignment->value)) {
        return std::nullopt;
    }

    const auto* len_call = std::get_if<CallExpr>(&limit->data);
    const auto len_resolution = checked_.call_resolutions.find(limit);
    if (!len_call || len_call->args.size() != 1 ||
        len_call->args[0].name || len_call->args[0].writable ||
        len_resolution == checked_.call_resolutions.end() ||
        len_resolution->second.kind != CallKind::Builtin ||
        len_resolution->second.builtin != BuiltinCallable::Len) {
        return std::nullopt;
    }
    const auto* len_name = std::get_if<NameExpr>(&len_call->args[0].value->data);
    if (!len_name || source_key(*len_name) != source_key(*array_name)) return std::nullopt;
    return source_key(*array_name);
}

} // namespace quidra::lowering
