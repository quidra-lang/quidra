#pragma once

// Typed-IR instructions of the autograd domain (tracking, backward, gradients, autograd targets).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/debug.hpp"
#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstdint>
#include <vector>

namespace quidra::ir {

struct TensorIsTracked { ValueId out; ValueId tensor; };
struct TensorHasGrad { ValueId out; ValueId tensor; };
struct TensorClearGrad { ValueId tensor; SourcePosition position{}; };
struct TensorTrack { ValueId out; ValueId tensor; ValueId target{}; Type type; int mode{}; SourcePosition position{}; };
struct TensorBackwardTarget { ValueId value; bool autograd_target{}; };
struct TensorBackward { ValueId tensor; std::vector<TensorBackwardTarget> targets; ValueId autograd_targets{}; ValueId track; SourcePosition position{}; };
struct TensorGrad { ValueId out; ValueId tensor; Type type; SourcePosition position{}; };
struct AutogradTargetCreate { ValueId out; };
struct AutogradTargetHasGrad { ValueId out; ValueId target; };
struct AutogradTargetClearGrad { ValueId target; SourcePosition position{}; };
struct AutogradTargetGradient { ValueId out; ValueId target; Type type; SourcePosition position{}; };

// TensorTrack::mode: which tracking method the instruction stands for. The
// field stays an int; the backend calls the runtime function of the mode.
namespace tensor_track_mode {
inline constexpr int untrack = 0;
inline constexpr int track = 1;
inline constexpr int retrack = 2;
} // namespace tensor_track_mode

template <> struct InstructionTraits<TensorIsTracked> : WithOperands<Domain::autograd, TensorIsTracked> {};
template <> struct InstructionTraits<TensorHasGrad> : WithOperands<Domain::autograd, TensorHasGrad> {};
template <> struct InstructionTraits<TensorClearGrad> : WithOperands<Domain::autograd, TensorClearGrad> {};
template <> struct InstructionTraits<TensorTrack> : WithOperands<Domain::autograd, TensorTrack> {};
template <> struct InstructionTraits<TensorBackward> : WithOperands<Domain::autograd, TensorBackward> {};
template <> struct InstructionTraits<TensorGrad> : WithOperands<Domain::autograd, TensorGrad> {};
template <> struct InstructionTraits<AutogradTargetCreate> : WithOperands<Domain::autograd, AutogradTargetCreate> {};
template <> struct InstructionTraits<AutogradTargetHasGrad> : WithOperands<Domain::autograd, AutogradTargetHasGrad> {};
template <> struct InstructionTraits<AutogradTargetClearGrad> : WithOperands<Domain::autograd, AutogradTargetClearGrad> {};
template <> struct InstructionTraits<AutogradTargetGradient> : WithOperands<Domain::autograd, AutogradTargetGradient> {};

} // namespace quidra::ir
