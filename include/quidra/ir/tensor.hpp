#pragma once

// Typed-IR instructions of the tensor domain (tensor values, views, arithmetic, device synchronization).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/abi/tensor_codes.hpp"
#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace quidra::ir {

struct TensorCreate { ValueId out; ValueId shape; std::optional<ValueId> gpu; Type type; int fill_mode{}; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorTransfer { ValueId out; ValueId tensor; std::optional<ValueId> gpu; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorReshape { ValueId out; ValueId tensor; ValueId shape; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorTranspose { ValueId out; ValueId tensor; ValueId axis0; ValueId axis1; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorContiguous { ValueId out; ValueId tensor; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorGather { ValueId out; ValueId tensor; ValueId indices; ValueId shape; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorScatter { ValueId out; ValueId tensor; ValueId indices; ValueId shape; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorShape { ValueId out; ValueId tensor; Type type; };
struct TensorDevice { ValueId out; ValueId tensor; };
struct TensorIsContiguous { ValueId out; ValueId tensor; };
struct TensorItem { ValueId out; ValueId tensor; Type element_type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorCast { ValueId out; ValueId tensor; Type source_type; Type target_type; Type result_type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorBinary {
    ValueId out;
    std::string op;
    ValueId left;
    ValueId right;
    Type left_type;
    Type right_type;
    Type result_type;
    std::uint32_t line{};
    std::uint32_t column{};
};
struct TensorCompare {
    ValueId out;
    std::string op;
    ValueId left;
    ValueId right;
    Type left_type;
    Type right_type;
    Type result_type;
    std::uint32_t line{};
    std::uint32_t column{};
};
struct TensorBoolReduce {
    ValueId out;
    ValueId tensor;
    bool all{};
    std::uint32_t line{};
    std::uint32_t column{};
};
struct TensorIndexPart {
    bool slice{};
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
    std::uint32_t line{};
    std::uint32_t column{};
};
struct TensorSet {
    ValueId tensor;
    std::vector<ValueId> indices;
    ValueId value;
    Type element_type;
    std::uint32_t line{};
    std::uint32_t column{};
};
struct GpuSync { ValueId index; std::uint32_t line{}; std::uint32_t column{}; };

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

template <> struct InstructionTraits<TensorCreate> : InDomain<Domain::tensor> {};
template <> struct InstructionTraits<TensorTransfer> : InDomain<Domain::tensor> {};
template <> struct InstructionTraits<TensorReshape> : InDomain<Domain::tensor> {};
template <> struct InstructionTraits<TensorTranspose> : InDomain<Domain::tensor> {};
template <> struct InstructionTraits<TensorContiguous> : InDomain<Domain::tensor> {};
template <> struct InstructionTraits<TensorGather> : InDomain<Domain::tensor> {};
template <> struct InstructionTraits<TensorScatter> : InDomain<Domain::tensor> {};
template <> struct InstructionTraits<TensorShape> : InDomain<Domain::tensor> {};
template <> struct InstructionTraits<TensorDevice> : InDomain<Domain::tensor> {};
template <> struct InstructionTraits<TensorIsContiguous> : InDomain<Domain::tensor> {};
template <> struct InstructionTraits<TensorItem> : InDomain<Domain::tensor> {};
template <> struct InstructionTraits<TensorCast> : InDomain<Domain::tensor> {};
template <> struct InstructionTraits<TensorBinary> : InDomain<Domain::tensor> {};
template <> struct InstructionTraits<TensorCompare> : InDomain<Domain::tensor> {};
template <> struct InstructionTraits<TensorBoolReduce> : InDomain<Domain::tensor> {};
template <> struct InstructionTraits<TensorIndex> : InDomain<Domain::tensor> {};
template <> struct InstructionTraits<TensorSet> : InDomain<Domain::tensor> {};
template <> struct InstructionTraits<GpuSync> : InDomain<Domain::tensor> {};

} // namespace quidra::ir
