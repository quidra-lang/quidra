#pragma once

// ShapeConstraintState: the declared extents of the function being lowered.
//
// A declaration, parameter or return type that spells extents (Tensor[n, m],
// T[n][m]) has them evaluated once into hidden Int locals; the names of those
// locals, one per axis (nullopt for an axis without an extent), are kept here
// so that every later store to the local, and every return, is checked
// against them. A binding also hands its extents to a tensor.zeros/ones
// initializer without an explicit shape: the contextual shape channel, keyed
// by the initializer expression. ShapeConstraintLowering
// (shape_constraint_lowering.cpp) evaluates and checks the extents. reset()
// clears the state when a function begins.

#include "quidra/ast.hpp"
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace quidra::lowering {

// The hidden Int locals holding the extents of each axis.
using CapturedExtents = std::vector<std::optional<std::string>>;

class ShapeConstraintState {
public:
    void reset() {
        tensors_.clear();
        arrays_.clear();
        contextual_.clear();
        return_tensor_.clear();
        return_array_.clear();
    }

    // The extents of the tensor (or array) local `local`, or null.
    void constrain_tensor(const std::string& local, const CapturedExtents& extents) { tensors_[local] = extents; }
    const CapturedExtents* tensor_extents(const std::string& local) const {
        const auto found = tensors_.find(local);
        return found == tensors_.end() ? nullptr : &found->second;
    }
    void constrain_array(const std::string& local, const CapturedExtents& extents) { arrays_[local] = extents; }
    const CapturedExtents* array_extents(const std::string& local) const {
        const auto found = arrays_.find(local);
        return found == arrays_.end() ? nullptr : &found->second;
    }

    // The shape a binding gives its tensor.zeros/ones initializer, or null.
    void set_initializer_shape(const Expr* initializer, const CapturedExtents& extents) { contextual_[initializer] = extents; }
    const CapturedExtents* initializer_shape(const Expr* initializer) const {
        const auto found = contextual_.find(initializer);
        return found == contextual_.end() ? nullptr : &found->second;
    }

    // The extents of the function's result; empty when it declares none.
    void constrain_return_tensor(CapturedExtents&& extents) { return_tensor_ = std::move(extents); }
    const CapturedExtents& return_tensor_extents() const { return return_tensor_; }
    void constrain_return_array(CapturedExtents&& extents) { return_array_ = std::move(extents); }
    const CapturedExtents& return_array_extents() const { return return_array_; }

private:
    std::unordered_map<std::string, CapturedExtents> tensors_;
    std::unordered_map<std::string, CapturedExtents> arrays_;
    std::unordered_map<const Expr*, CapturedExtents> contextual_;
    CapturedExtents return_tensor_;
    CapturedExtents return_array_;
};

} // namespace quidra::lowering
