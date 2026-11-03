#pragma once

// The operands of a slice item in the typed IR, shared by the
// instructions that slice.
//
// Owns: SliceBounds. The fields, their order and their default member
// initializers are part of the IR data model.

#include "quidra/ir/value_id.hpp"
#include "quidra/slice_form.hpp"

#include <optional>

namespace quidra::ir {

// A slice item as written: the values of its start, end and step (int64;
// absent when omitted), the form of each side, and the direction the
// checker resolved (by_step_sign: a run-time step decides it;
// by_bounds: the two explicit endpoint values decide it).
struct SliceBounds {
    std::optional<ValueId> start;
    std::optional<ValueId> end;
    std::optional<ValueId> step;
    SliceStart start_form{SliceStart::omitted};
    SliceEnd end_form{SliceEnd::omitted};
    SliceDirection direction{SliceDirection::ascending};
};

} // namespace quidra::ir
