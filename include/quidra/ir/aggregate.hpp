#pragma once

// Typed-IR instructions of the aggregate domain (arrays, classes, variants).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/debug.hpp"
#include "quidra/ir/domain.hpp"
#include "quidra/ir/slice.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace quidra::ir {

struct FieldReferenceAddress { ValueId out; ValueId object; std::size_t index; };
struct FieldReferenceRoot { ValueId out; ValueId object; std::size_t index; };
struct BindFieldReference { ValueId object; std::size_t index; ValueId address; ValueId root; };
struct RootSerial { ValueId out; };
struct TupleMake { ValueId out; std::vector<ValueId> elements; Type type; };
struct TupleGet { ValueId out; ValueId tuple; std::size_t index; Type element_type; bool take{}; };
struct AddressTupleElement { ValueId out; ValueId address; std::size_t index; Type tuple_type; };
struct ArrayMake { ValueId out; std::vector<ValueId> elements; Type type; };
struct ArrayAlloc { ValueId out; ValueId length; Type type; bool fully_initialized{}; };
struct ClassMake { ValueId out; Type type; std::vector<std::optional<ValueId>> fields; };
struct FieldGet { ValueId out; ValueId object; std::size_t index; Type field_type; };
struct FieldSet { ValueId object; std::size_t index; ValueId value; Type field_type; bool replace_without_release{}; };
struct ArrayLength { ValueId out; ValueId array; };
struct ArrayCanAppendMove { ValueId out; ValueId array; };
struct ArrayGrowMove { ValueId out; ValueId array; Type array_type; };
// sorted(): ascending unless descending_constant, or unless the run-time
// flag `descending` (a bool) is true.
struct ArraySorted { ValueId out; ValueId array; Type array_type; SourcePosition position{}; bool descending_constant{}; std::optional<ValueId> descending{}; };
struct ArrayInitializationComplete { ValueId out; ValueId array; };
struct ArrayGet { ValueId out; ValueId array; ValueId index; Type element_type; SourcePosition position{}; bool initialization_proven{}; bool bounds_proven{}; std::optional<ValueId> initialization_guard{}; std::optional<ValueId> bounds_guard{}; };
struct ArraySet { ValueId array; ValueId index; ValueId value; Type element_type; SourcePosition position{}; bool initialization_proven{}; bool bounds_proven{}; std::optional<ValueId> initialization_guard{}; std::optional<ValueId> bounds_guard{}; std::optional<ValueId> parallel_receipt{}; };
// The elements a slice of an array selects, copied into a new runtime-sized
// array (L5); the selected elements must be initialized.
struct ArraySlice { ValueId out; ValueId array; SliceBounds bounds; Type array_type; SourcePosition position{}; bool bounds_proven{}; bool initialization_proven{}; };
// Stores the elements of `value` (a runtime-sized array, evaluated before)
// into the elements a slice of an array selects (L6).
struct ArraySliceSet { ValueId array; SliceBounds bounds; ValueId value; Type array_type; SourcePosition position{}; bool bounds_proven{}; };
// A collection loop borrows the selected elements directly when its body
// cannot write the source. `first_address` is a function-entry int64 slot.
struct SliceLoopPlan { ValueId out; ValueId length; ValueId first_address; SliceBounds bounds; SourcePosition position{}; };
struct VariantMake { ValueId out; int tag; ValueId payload; Type container_type; Type payload_type; };
struct VariantTag { ValueId out; ValueId container; };
struct VariantPayload { ValueId out; ValueId container; Type payload_type; };

template <> struct InstructionTraits<FieldReferenceAddress> : WithOperands<Domain::aggregate, FieldReferenceAddress> {};
template <> struct InstructionTraits<FieldReferenceRoot> : WithOperands<Domain::aggregate, FieldReferenceRoot> {};
template <> struct InstructionTraits<BindFieldReference> : WithOperands<Domain::aggregate, BindFieldReference> {};
template <> struct InstructionTraits<RootSerial> : WithOperands<Domain::aggregate, RootSerial> {};
template <> struct InstructionTraits<TupleMake> : WithOperands<Domain::aggregate, TupleMake> {};
template <> struct InstructionTraits<TupleGet> : WithOperands<Domain::aggregate, TupleGet> {};
template <> struct InstructionTraits<AddressTupleElement> : WithOperands<Domain::aggregate, AddressTupleElement> {};
template <> struct InstructionTraits<ArrayMake> : WithOperands<Domain::aggregate, ArrayMake> {};
template <> struct InstructionTraits<ArrayAlloc> : WithOperands<Domain::aggregate, ArrayAlloc> {};
template <> struct InstructionTraits<ClassMake> : WithOperands<Domain::aggregate, ClassMake> {};
template <> struct InstructionTraits<FieldGet> : WithOperands<Domain::aggregate, FieldGet> {};
template <> struct InstructionTraits<FieldSet> : WithOperands<Domain::aggregate, FieldSet> {};
template <> struct InstructionTraits<ArrayLength> : WithOperands<Domain::aggregate, ArrayLength> {};
template <> struct InstructionTraits<ArrayCanAppendMove> : WithOperands<Domain::aggregate, ArrayCanAppendMove> {};
template <> struct InstructionTraits<ArrayGrowMove> : WithOperands<Domain::aggregate, ArrayGrowMove> {};
template <> struct InstructionTraits<ArraySorted> : WithOperands<Domain::aggregate, ArraySorted> {};
template <> struct InstructionTraits<ArrayInitializationComplete> : WithOperands<Domain::aggregate, ArrayInitializationComplete> {};
template <> struct InstructionTraits<ArrayGet> : WithOperands<Domain::aggregate, ArrayGet> {};
template <> struct InstructionTraits<ArraySet> : WithOperands<Domain::aggregate, ArraySet> {};
template <> struct InstructionTraits<SliceLoopPlan> : WithOperands<Domain::aggregate, SliceLoopPlan> {};
template <> struct InstructionTraits<ArraySlice> : WithOperands<Domain::aggregate, ArraySlice> {};
template <> struct InstructionTraits<ArraySliceSet> : WithOperands<Domain::aggregate, ArraySliceSet> {};
template <> struct InstructionTraits<VariantMake> : WithOperands<Domain::aggregate, VariantMake> {};
template <> struct InstructionTraits<VariantTag> : WithOperands<Domain::aggregate, VariantTag> {};
template <> struct InstructionTraits<VariantPayload> : WithOperands<Domain::aggregate, VariantPayload> {};

} // namespace quidra::ir
