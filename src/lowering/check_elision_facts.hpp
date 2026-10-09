#pragma once

// Check elision facts: lowering-time proofs that remove runtime checks.
//
//   IntegerRangeFacts         nonnegative ranges of int64 and bare integer
//                             locals that prove an int64 addition,
//                             subtraction or multiplication cannot overflow
//                             (Binary.overflow_proven), and magnitude bounds
//                             that prove a bare integer operation inline
//                             (Binary/Unary.inline_proven)
//   ArrayBoundsProof          range loops bounded by an array's length, and
//                             bounds of reference parameters checked once at
//                             entry (ArrayGet/ArraySet bounds_proven, guards)
//   ArrayInitializationProof  arrays known to be fully initialized
//                             (initialization_proven, guards)
//
// The facts belong to the function being lowered: reset() clears them when a
// function begins. The proofs read the checker's facts and the source scope;
// two of them emit the entry-block loads their guards test, through the
// function's builder (check_elision_facts.cpp). Each proof is moved, never
// copied: a copy of an unordered container may iterate in another order.

#include "ir/function_builder.hpp"
#include "lowering/local_scope.hpp"
#include "quidra/checker.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace quidra::lowering {

// A range loop whose index stays below `end` while `body` runs.
struct ActiveRangeBound {
    std::string index_name;
    const Expr* end{};
    const std::vector<StmtPtr>* body{};
};

// The inclusive range of a nonnegative int64 or bare integer value.
struct NonnegativeIntegerRange {
    std::uint64_t minimum{};
    std::uint64_t maximum{};
};

// A local whose range may be proven: its initial range and the modulo
// assignments that keep it below the modulus.
struct IntegerRangeCandidate {
    NonnegativeIntegerRange initial;
    std::optional<std::uint64_t> modulus;
    std::vector<const Expr*> numerators;
    std::size_t assignments{};
    bool invalid{};
};

// The source arrays known to be fully initialized at a point of the function.
using FullArrays = std::unordered_set<std::string>;

// The arrays fully initialized on both of two joining paths.
FullArrays intersect_full_arrays(const FullArrays& left, const FullArrays& right);

class IntegerRangeFacts {
public:
    IntegerRangeFacts(const CheckedProgram& checked, const LocalScope& scope);
    IntegerRangeFacts(const IntegerRangeFacts&) = delete;
    IntegerRangeFacts& operator=(const IntegerRangeFacts&) = delete;
    IntegerRangeFacts(IntegerRangeFacts&&) = default;
    void reset();

    // Proves the ranges of the int64 and bare integer locals of `body`:
    // const bindings, and locals whose every assignment is a modulo by one
    // exact divisor.
    void prepare(const std::vector<StmtPtr>& body);
    // Whether the int64 +, - or * `expression` cannot overflow.
    bool overflow_proven(const Expr& expression) const;
    // Whether the bare integer `expression` (a +, -, * or comparison, or a
    // negation) has inline operands and an inline result: every operand and
    // the result have a proven magnitude within the inline range.
    bool inline_proven(const Expr& expression) const;
    // Whether the explicit conversion of `argument` from `source` to
    // `target` takes an inline word without a check: a conversion to a bare
    // integer whose value is proven inline, or one from a bare integer whose
    // value is proven within the fixed-width target's range.
    bool conversion_inline_proven(
        const Expr& argument, const Type& source, const Type& target) const;
    // Whether every value of the integer `expression` is proven inline.
    bool inline_bounded(const Expr& expression) const;
    // A range loop over `name` from `start` (0 when absent) below `end` is
    // entered: while its body runs, `name` lies within [start, end) when
    // the body never writes it and both bounds are nonnegative and bounded
    // (leave_range_loop ends the fact).
    void enter_range_loop(const std::string& name, const std::vector<StmtPtr>& body,
                          const Expr* start, const Expr& end);
    void leave_range_loop(const std::string& name);

private:
    // A bound on the magnitude of the integer `expression`, from literals,
    // conversions from fixed-width integers of at most 32 bits, the declared
    // result range of len, the nonnegative ranges of locals and the
    // arithmetic over them.
    std::optional<std::uint64_t> magnitude_bound(const Expr& expression) const;
    std::optional<NonnegativeIntegerRange> fixed_nonnegative_range(
        const Type& type) const;
    std::optional<NonnegativeIntegerRange> nonnegative_integer_range(
        const Expr& expression,
        const std::unordered_map<std::string, NonnegativeIntegerRange>& facts) const;
    std::optional<NonnegativeIntegerRange> nonnegative_integer_range(
        const Expr& expression) const;
    void inspect_integer_range_candidate(
        const std::vector<StmtPtr>& body, const std::string& name,
        const std::unordered_map<std::string, NonnegativeIntegerRange>& constants,
        IntegerRangeCandidate& candidate) const;

    const CheckedProgram& checked_;
    const LocalScope& scope_;
    std::unordered_map<std::string, NonnegativeIntegerRange> ranges_;
    // The facts that range loop variables hide, innermost last.
    struct HiddenRange {
        std::string name;
        std::optional<NonnegativeIntegerRange> range;
    };
    std::vector<HiddenRange> hidden_ranges_;
};

class ArrayBoundsProof {
public:
    ArrayBoundsProof(const CheckedProgram& checked, const LocalScope& scope);
    ArrayBoundsProof(const ArrayBoundsProof&) = delete;
    ArrayBoundsProof& operator=(const ArrayBoundsProof&) = delete;
    ArrayBoundsProof(ArrayBoundsProof&&) = default;
    void reset();

    // Loads, at function entry, each reference array parameter's length and
    // compares it with every Int parameter the body never replaces.
    void cache_reference_bounds(
        ir::FunctionBuilder& builder, const FunctionType& signature,
        const std::vector<StmtPtr>& body);
    // The entry-block comparison of an Int parameter with the length of the
    // reference array parameter `base`, when a range loop over `index` ends
    // at that parameter.
    std::optional<ir::ValueId> reference_guard(
        const Expr& base, const Expr& index) const;
    // Whether a range loop bounded by the length of the array `base` keeps
    // `index` within it.
    bool proven_by_range_loop(
        const Expr& base, const Expr& index) const;
    // The operand of an exact integer conversion of a nat length
    // (int(len(a))), else the expression itself.
    const Expr& without_exact_length_cast(const Expr& expression) const;
    std::optional<std::string> direct_array_length_source(
        const Expr& expression) const;

    // A range loop over `index_name` bounded by `end` is entered or left.
    void push_range(ActiveRangeBound range) { active_ranges_.push_back(std::move(range)); }
    void pop_range() { active_ranges_.pop_back(); }
    // `scalar` was bound to len(`array`); `array` was made with `scalar` elements.
    void record_scalar_length(const std::string& scalar, const std::string& array) { scalar_length_of_array_[scalar] = array; }
    void record_array_length(const std::string& array, const std::string& scalar) { array_length_from_scalar_[array] = scalar; }
    // `name` may be written: every length relation it takes part in ends.
    void invalidate_length_relation(const std::string& name);

private:
    bool scalar_names_array_length(
        const std::string& scalar, const std::string& array) const;
    std::optional<long long> range_end_distance_from_array_length(
        const Expr& end, const std::string& array) const;
    bool range_bound_stable(
        const ActiveRangeBound& range, const std::string& array) const;

    const CheckedProgram& checked_;
    const LocalScope& scope_;
    std::unordered_map<std::string, std::unordered_map<std::string, ir::ValueId>> reference_bounds_;
    std::vector<ActiveRangeBound> active_ranges_;
    std::unordered_map<std::string, std::string> scalar_length_of_array_;
    std::unordered_map<std::string, std::string> array_length_from_scalar_;
};

class ArrayInitializationProof {
public:
    ArrayInitializationProof(const CheckedProgram& checked, const LocalScope& scope);
    ArrayInitializationProof(const ArrayInitializationProof&) = delete;
    ArrayInitializationProof& operator=(const ArrayInitializationProof&) = delete;
    ArrayInitializationProof(ArrayInitializationProof&&) = default;
    void reset();

    // Loads, at function entry, the "all elements initialized" bit of each
    // reference array parameter whose binding the body never replaces.
    void cache_reference_initialization(
        ir::FunctionBuilder& builder, const FunctionType& signature,
        const std::vector<StmtPtr>& body);
    // The entry-block bit that tells whether the reference array parameter
    // `expression` is fully initialized.
    std::optional<ir::ValueId> reference_guard(
        const Expr& expression) const;
    // Whether the array `expression` is known to be fully initialized here.
    bool fully_initialized(const Expr& expression) const;
    std::optional<std::string> full_array_written_by_for(const ForStmt& loop) const;

    // The fully initialized arrays here. Branches, loops and match cases save
    // them, restore them and join them with intersect_full_arrays.
    const FullArrays& full_arrays() const { return full_arrays_; }
    void set_full_arrays(const FullArrays& arrays) { full_arrays_ = arrays; }
    void set_full_arrays(FullArrays&& arrays) { full_arrays_ = std::move(arrays); }
    void clear_full_arrays() { full_arrays_.clear(); }
    void mark_full(const std::string& array) { full_arrays_.insert(array); }
    void forget_full(const std::string& array) { full_arrays_.erase(array); }

private:
    const CheckedProgram& checked_;
    const LocalScope& scope_;
    FullArrays full_arrays_;
    std::unordered_map<std::string, ir::ValueId> reference_initialization_;
};

// The three proofs of the function being lowered.
struct CheckElisionFacts {
    CheckElisionFacts(const CheckedProgram& checked, const LocalScope& scope);
    void reset();

    // The facts a store of `value` to the source local `name` leaves behind:
    // every length relation of `name` ends; an Int bound to len(array)
    // records that relation; an array is fully initialized exactly when
    // `array_full`, and one made by array(length, ...) records its length. A
    // binding and an assignment to a local record the same.
    void record_local_store(
        const std::string& name, const Type& type, const Expr& value, bool array_full);

    IntegerRangeFacts integer_ranges;
    ArrayBoundsProof array_bounds;
    ArrayInitializationProof array_initialization;

private:
    const CheckedProgram& checked_;
};

} // namespace quidra::lowering
