#include "ir/facts/tensor.hpp"

#include <limits>
#include <stdexcept>

namespace quidra::ir::facts {
namespace {
ShapeExtent fresh(const TensorShapeTransfer& transfer, std::size_t axis) {
    return transfer.fresh ? transfer.fresh(axis) : ShapeExtent{};
}
void explicit_shape(Type& result, const std::vector<ShapeExtent>& extents) {
    if (extents.size() > static_cast<std::size_t>(std::numeric_limits<long long>::max()))
        throw std::length_error("tensor fact rank is out of range");
    result.length = static_cast<long long>(extents.size());
    result.tensor_known_shape_prefix = extents;
}
} // namespace
Type transfer_tensor_shape(const TensorShapeTransfer& transfer) {
    auto result = transfer.result;
    if (result.kind != TypeKind::Tensor) return result;
    const Type* tensor = nullptr;
    for (const auto& input : transfer.inputs) if (input.kind == TypeKind::Tensor) { tensor = &input; break; }
    switch (transfer.operation) {
    case TensorShapeOp::Preserve:
        if (tensor) {
            result.length = tensor->length;
            result.tensor_known_shape_prefix = tensor->tensor_known_shape_prefix;
        }
        break;
    case TensorShapeOp::Broadcast:
        if (!tensor) break;
        result.length = tensor->length;
        result.tensor_known_shape_prefix = tensor->tensor_known_shape_prefix;
        if (transfer.inputs.size() >= 2 && transfer.inputs[0].kind == TypeKind::Tensor &&
            transfer.inputs[1].kind == TypeKind::Tensor) {
            result.tensor_known_shape_prefix.clear();
            for (long long axis = 0; axis < result.length; ++axis)
                result.tensor_known_shape_prefix.push_back(broadcast_extent(
                    tensor_extent(transfer.inputs[0], static_cast<std::size_t>(axis)),
                    tensor_extent(transfer.inputs[1], static_cast<std::size_t>(axis))));
        }
        break;
    case TensorShapeOp::ExplicitShape:
        if (transfer.explicit_shape) explicit_shape(result, *transfer.explicit_shape);
        break;
    case TensorShapeOp::Transpose:
        if (!tensor) break;
        result.length = tensor->length;
        result.tensor_known_shape_prefix.clear();
        for (long long axis = 0; axis < result.length; ++axis)
            result.tensor_known_shape_prefix.push_back(tensor_extent(*tensor, static_cast<std::size_t>(axis)));
        if (transfer.axes && transfer.axes->first >= 0 && transfer.axes->second >= 0 &&
            transfer.axes->first < result.length && transfer.axes->second < result.length)
            std::swap(result.tensor_known_shape_prefix[static_cast<std::size_t>(transfer.axes->first)],
                      result.tensor_known_shape_prefix[static_cast<std::size_t>(transfer.axes->second)]);
        else result.tensor_known_shape_prefix.clear();
        break;
    case TensorShapeOp::Index:
        if (!tensor || tensor->length < 0) break;
        result.tensor_known_shape_prefix.clear();
        for (std::size_t axis = 0; axis < static_cast<std::size_t>(tensor->length); ++axis) {
            if (axis < transfer.indices.size() && transfer.indices[axis].kind == TensorShapeTransfer::IndexAxis::Drop) continue;
            auto extent = tensor_extent(*tensor, axis);
            if (axis < transfer.indices.size() && transfer.indices[axis].kind == TensorShapeTransfer::IndexAxis::Slice) {
                const auto output_axis = result.tensor_known_shape_prefix.size();
                extent = transfer.known_result ? tensor_extent(*transfer.known_result, output_axis) : ShapeExtent{};
                if (extent.kind == ShapeExtent::Kind::Unknown) extent = fresh(transfer, output_axis);
            }
            result.tensor_known_shape_prefix.push_back(std::move(extent));
        }
        break;
    case TensorShapeOp::CallOrigins: {
        if (transfer.result_origins.empty()) break;
        if (transfer.result_origins.size() > static_cast<std::size_t>(std::numeric_limits<long long>::max()))
            throw std::length_error("tensor fact rank is out of range");
        const auto declared_result = result;
        result.length = static_cast<long long>(transfer.result_origins.size());
        result.tensor_known_shape_prefix.clear();
        const auto input = [&](std::size_t parameter, std::size_t axis) {
            if (axis == std::numeric_limits<std::size_t>::max())
                return parameter < transfer.scalar_inputs.size() ? transfer.scalar_inputs[parameter] : ShapeExtent{};
            return parameter < transfer.inputs.size() ? tensor_extent(transfer.inputs[parameter], axis) : ShapeExtent{};
        };
        for (std::size_t axis = 0; axis < transfer.result_origins.size(); ++axis) {
            const auto& origin = transfer.result_origins[axis];
            ShapeExtent extent;
            if (origin.kind == ExtentOrigin::Kind::Known) extent = origin.value;
            else if (origin.kind == ExtentOrigin::Kind::ParameterAxis) extent = input(origin.parameter, origin.axis);
            else if (origin.kind == ExtentOrigin::Kind::ParameterValue) extent = input(origin.parameter, std::numeric_limits<std::size_t>::max());
            else if (origin.kind == ExtentOrigin::Kind::Broadcast) {
                extent = 1;
                for (const auto& [parameter, input_axis] : origin.axes) extent = broadcast_extent(extent, input(parameter, input_axis));
                if (origin.local) extent = broadcast_extent(extent, fresh(transfer, axis));
            } else if (origin.kind == ExtentOrigin::Kind::Expr) {
                extent = substitute_extent(origin.expression, [&](std::int64_t id) {
                    const auto found = origin.expression_axes.find(id);
                    return found == origin.expression_axes.end() ? fresh(transfer, axis) : input(found->second.first, found->second.second);
                });
            }
            if (const auto declared = tensor_extent(declared_result, axis); declared.kind != ShapeExtent::Kind::Unknown) extent = declared;
            if (transfer.known_result) if (const auto known = tensor_known_extent(*transfer.known_result, axis)) extent = *known;
            if (extent.kind == ShapeExtent::Kind::Unknown) extent = fresh(transfer, axis);
            result.tensor_known_shape_prefix.push_back(std::move(extent));
        }
        break;
    }
    }
    return result;
}
Type merge_tensor_shapes(Type base, std::span<const Type> states,
                         const std::function<ShapeExtent(std::size_t)>& merge_symbol,
                         const std::function<bool(const ShapeExtent&)>& admissible) {
    if (base.kind != TypeKind::Tensor || states.empty()) return base;
    base.length = states.front().length;
    for (const auto& state : states) if (state.kind != TypeKind::Tensor || state.length != base.length) base.length = -1;
    base.tensor_known_shape_prefix.clear();
    for (long long axis = 0; axis < base.length; ++axis) {
        auto extent = tensor_extent(states.front(), static_cast<std::size_t>(axis));
        bool equal = extent.kind != ShapeExtent::Kind::Unknown;
        for (const auto& state : states) {
            const auto current = tensor_extent(state, static_cast<std::size_t>(axis));
            equal = equal && same_extent(extent, current) && (!admissible || admissible(current));
        }
        if (!equal) extent = merge_symbol ? merge_symbol(static_cast<std::size_t>(axis)) : ShapeExtent{};
        base.tensor_known_shape_prefix.push_back(std::move(extent));
    }
    return base;
}
} // namespace quidra::ir::facts
