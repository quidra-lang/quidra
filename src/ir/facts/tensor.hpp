#pragma once

#include "quidra/types.hpp"

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <utility>

namespace quidra::ir::facts {
enum class BackendKind : std::uint8_t { Unknown, AnyGpu, Cpu, Cuda, Hip, Metal, Test };
enum class Tri : std::uint8_t { Unknown, False, True };
struct DeviceFact {
    BackendKind kind{BackendKind::Unknown};
    std::optional<std::int64_t> index;
    bool operator==(const DeviceFact&) const = default;
};
struct TensorFacts {
    Type type{Type::simple(TypeKind::Invalid)};
    bool has_type{};
    DeviceFact device;
    Tri contiguous{Tri::Unknown}, tracked{Tri::Unknown}, unique{Tri::Unknown}, fully_initialized{Tri::Unknown};
    bool owns_storage{}; // Legacy v1 rule meaning; not a donation proof.
    std::vector<ShapeExtent> extents;
    // Validated unknown-rank patterns keep leading/trailing axes separately.
    std::vector<ShapeExtent> head_extents, tail_extents;
    std::size_t minimum_rank{};
    ShapeExtent rank_extent;
    bool operator==(const TensorFacts& other) const {
        return type == other.type && type.length == other.type.length && has_type == other.has_type &&
            device == other.device && contiguous == other.contiguous && tracked == other.tracked &&
            unique == other.unique && fully_initialized == other.fully_initialized && owns_storage == other.owns_storage &&
            extents == other.extents && head_extents == other.head_extents && tail_extents == other.tail_extents &&
            minimum_rank == other.minimum_rank && rank_extent == other.rank_extent;
    }
};
// Descriptor policy labels remain package-owned opaque strings.
using PolicyFacts = std::map<std::string, std::string, std::less<>>;

enum class TensorShapeOp : std::uint8_t { Preserve, Broadcast, ExplicitShape, Transpose, Index, CallOrigins };
struct TensorShapeTransfer {
    struct IndexAxis {
        enum Kind : std::uint8_t { Drop, Preserve, Slice } kind{Preserve};
    };
    TensorShapeOp operation{TensorShapeOp::Preserve};
    Type result;
    std::vector<Type> inputs;
    std::optional<std::vector<ShapeExtent>> explicit_shape;
    std::optional<std::pair<std::int64_t, std::int64_t>> axes;
    std::vector<IndexAxis> indices;
    std::vector<ExtentOrigin> result_origins;
    std::vector<ShapeExtent> scalar_inputs;
    std::optional<Type> known_result;
    std::function<ShapeExtent(std::size_t)> fresh;
};
// The common S1 transfer table. Clients adapt their AST/IR operands and own
// symbol identities; shape algebra is implemented here exactly once.
Type transfer_tensor_shape(const TensorShapeTransfer&);
Type merge_tensor_shapes(Type base, std::span<const Type> states,
                         const std::function<ShapeExtent(std::size_t)>& merge_symbol,
                         const std::function<bool(const ShapeExtent&)>& admissible = {});
} // namespace quidra::ir::facts
