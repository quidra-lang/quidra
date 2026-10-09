#pragma once

// Typed-IR instructions of the aggregate domain (arrays, classes, variants).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace quidra::ir {

struct ArrayMake { ValueId out; std::vector<ValueId> elements; Type type; };
struct ArrayAlloc { ValueId out; ValueId length; Type type; bool fully_initialized{}; };
struct ClassMake { ValueId out; Type type; std::vector<std::optional<ValueId>> fields; };
struct FieldGet { ValueId out; ValueId object; std::size_t index; Type field_type; };
struct FieldSet { ValueId object; std::size_t index; ValueId value; Type field_type; bool replace_without_release{}; };
struct ArrayLength { ValueId out; ValueId array; };
struct ArrayCanAppendMove { ValueId out; ValueId array; };
struct ArrayGrowMove { ValueId out; ValueId array; Type array_type; };
struct ArraySorted { ValueId out; ValueId array; Type array_type; std::uint32_t line{}; std::uint32_t column{}; };
struct ArrayInitializationComplete { ValueId out; ValueId array; };
struct ArrayGet { ValueId out; ValueId array; ValueId index; Type element_type; std::uint32_t line{}; std::uint32_t column{}; bool initialization_proven{}; bool bounds_proven{}; std::optional<ValueId> initialization_guard{}; std::optional<ValueId> bounds_guard{}; };
struct ArraySet { ValueId array; ValueId index; ValueId value; Type element_type; std::uint32_t line{}; std::uint32_t column{}; bool initialization_proven{}; bool bounds_proven{}; std::optional<ValueId> initialization_guard{}; std::optional<ValueId> bounds_guard{}; };
struct VariantMake { ValueId out; int tag; ValueId payload; Type container_type; Type payload_type; };
struct VariantTag { ValueId out; ValueId container; };
struct VariantPayload { ValueId out; ValueId container; Type payload_type; };

template <> struct InstructionTraits<ArrayMake> : InDomain<Domain::aggregate> {};
template <> struct InstructionTraits<ArrayAlloc> : InDomain<Domain::aggregate> {};
template <> struct InstructionTraits<ClassMake> : InDomain<Domain::aggregate> {};
template <> struct InstructionTraits<FieldGet> : InDomain<Domain::aggregate> {};
template <> struct InstructionTraits<FieldSet> : InDomain<Domain::aggregate> {};
template <> struct InstructionTraits<ArrayLength> : InDomain<Domain::aggregate> {};
template <> struct InstructionTraits<ArrayCanAppendMove> : InDomain<Domain::aggregate> {};
template <> struct InstructionTraits<ArrayGrowMove> : InDomain<Domain::aggregate> {};
template <> struct InstructionTraits<ArraySorted> : InDomain<Domain::aggregate> {};
template <> struct InstructionTraits<ArrayInitializationComplete> : InDomain<Domain::aggregate> {};
template <> struct InstructionTraits<ArrayGet> : InDomain<Domain::aggregate> {};
template <> struct InstructionTraits<ArraySet> : InDomain<Domain::aggregate> {};
template <> struct InstructionTraits<VariantMake> : InDomain<Domain::aggregate> {};
template <> struct InstructionTraits<VariantTag> : InDomain<Domain::aggregate> {};
template <> struct InstructionTraits<VariantPayload> : InDomain<Domain::aggregate> {};

} // namespace quidra::ir
