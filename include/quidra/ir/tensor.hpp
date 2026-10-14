#pragma once

// Typed-IR instructions of the tensor domain (tensor values, views, arithmetic, device synchronization).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/abi/tensor_codes.hpp"
#include "quidra/ir/debug.hpp"
#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace quidra::ir {

struct TensorCreate { ValueId out; ValueId shape; std::optional<ValueId> gpu; Type type; int fill_mode{}; SourcePosition position{}; };
struct TensorTransfer { ValueId out; ValueId tensor; std::optional<ValueId> gpu; Type type; SourcePosition position{}; };
struct TensorReshape { ValueId out; ValueId tensor; ValueId shape; Type type; SourcePosition position{}; };
struct TensorTranspose { ValueId out; ValueId tensor; ValueId axis0; ValueId axis1; Type type; SourcePosition position{}; };
struct TensorContiguous { ValueId out; ValueId tensor; Type type; SourcePosition position{}; };
struct TensorGather { ValueId out; ValueId tensor; ValueId indices; ValueId shape; Type type; SourcePosition position{}; };
struct TensorScatter { ValueId out; ValueId tensor; ValueId indices; ValueId shape; Type type; SourcePosition position{}; };
struct TensorShape { ValueId out; ValueId tensor; Type type; };
struct TensorDevice { ValueId out; ValueId tensor; };
struct TensorIsContiguous { ValueId out; ValueId tensor; };
struct TensorItem { ValueId out; ValueId tensor; Type element_type; SourcePosition position{}; };
struct TensorCast { ValueId out; ValueId tensor; Type source_type; Type target_type; Type result_type; SourcePosition position{}; };
struct TensorBinary {
    ValueId out;
    std::string op;
    ValueId left;
    ValueId right;
    Type left_type;
    Type right_type;
    Type result_type;
    SourcePosition position{};
};
struct TensorCompare {
    ValueId out;
    std::string op;
    ValueId left;
    ValueId right;
    Type left_type;
    Type right_type;
    Type result_type;
    SourcePosition position{};
};
struct TensorBoolReduce {
    ValueId out;
    ValueId tensor;
    bool all{};
    SourcePosition position{};
};
// An item of a tensor index: its kind is abi::tensor_index_spec's (index,
// the half-open slice start:<end, or a marked slice naming its forms).
struct TensorIndexPart {
    std::uint8_t kind{};
    std::optional<ValueId> index;
    std::optional<ValueId> start;
    std::optional<ValueId> stop;
    std::optional<ValueId> step;
};
struct TensorIndex {
    ValueId out;
    ValueId tensor;
    std::vector<TensorIndexPart> items;
    Type type;
    SourcePosition position{};
};
struct TensorSet {
    ValueId tensor;
    std::vector<ValueId> indices;
    ValueId value;
    Type element_type;
    SourcePosition position{};
};
struct GpuSync { ValueId index; SourcePosition position{}; };

// TensorCreate::fill_mode: how the elements of the new tensor start. The
// field stays an int; the backend passes it to the runtime's tensor
// creation unchanged.
namespace tensor_fill_mode {
inline constexpr int uninitialized = 0;
inline constexpr int zeros = 1;
inline constexpr int ones = 2;
} // namespace tensor_fill_mode

static_assert(tensor_fill_mode::uninitialized == abi::tensor_fill_mode::uninitialized &&
                  tensor_fill_mode::zeros == abi::tensor_fill_mode::zeros &&
                  tensor_fill_mode::ones == abi::tensor_fill_mode::ones,
              "the IR's fill modes are the runtime's codes");

template <> struct InstructionTraits<TensorCreate> : WithOperands<Domain::tensor, TensorCreate> {};
template <> struct InstructionTraits<TensorTransfer> : WithOperands<Domain::tensor, TensorTransfer> {};
template <> struct InstructionTraits<TensorReshape> : WithOperands<Domain::tensor, TensorReshape> {};
template <> struct InstructionTraits<TensorTranspose> : WithOperands<Domain::tensor, TensorTranspose> {};
template <> struct InstructionTraits<TensorContiguous> : WithOperands<Domain::tensor, TensorContiguous> {};
template <> struct InstructionTraits<TensorGather> : WithOperands<Domain::tensor, TensorGather> {};
template <> struct InstructionTraits<TensorScatter> : WithOperands<Domain::tensor, TensorScatter> {};
template <> struct InstructionTraits<TensorShape> : WithOperands<Domain::tensor, TensorShape> {};
template <> struct InstructionTraits<TensorDevice> : WithOperands<Domain::tensor, TensorDevice> {};
template <> struct InstructionTraits<TensorIsContiguous> : WithOperands<Domain::tensor, TensorIsContiguous> {};
template <> struct InstructionTraits<TensorItem> : WithOperands<Domain::tensor, TensorItem> {};
template <> struct InstructionTraits<TensorCast> : WithOperands<Domain::tensor, TensorCast> {};
template <> struct InstructionTraits<TensorBinary> : WithOperands<Domain::tensor, TensorBinary> {};
template <> struct InstructionTraits<TensorCompare> : WithOperands<Domain::tensor, TensorCompare> {};
template <> struct InstructionTraits<TensorBoolReduce> : WithOperands<Domain::tensor, TensorBoolReduce> {};
template <> struct InstructionTraits<TensorIndex> : WithOperands<Domain::tensor, TensorIndex> {};
template <> struct InstructionTraits<TensorSet> : WithOperands<Domain::tensor, TensorSet> {};
template <> struct InstructionTraits<GpuSync> : WithOperands<Domain::tensor, GpuSync> {};

} // namespace quidra::ir
