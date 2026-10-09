#pragma once

// Typed-IR instructions of the autograd domain (tracking, backward, gradients, autograd targets).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstdint>
#include <vector>

namespace quidra::ir {

struct TensorIsTracked { ValueId out; ValueId tensor; };
struct TensorHasGrad { ValueId out; ValueId tensor; };
struct TensorClearGrad { ValueId tensor; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorTrack { ValueId out; ValueId tensor; ValueId target{}; Type type; int mode{}; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorBackwardTarget { ValueId value; bool autograd_target{}; };
struct TensorBackward { ValueId tensor; std::vector<TensorBackwardTarget> targets; ValueId autograd_targets{}; ValueId track; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorGrad { ValueId out; ValueId tensor; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct AutogradTargetCreate { ValueId out; };
struct AutogradTargetHasGrad { ValueId out; ValueId target; };
struct AutogradTargetClearGrad { ValueId target; std::uint32_t line{}; std::uint32_t column{}; };
struct AutogradTargetGradient { ValueId out; ValueId target; Type type; std::uint32_t line{}; std::uint32_t column{}; };

// TensorTrack::mode: which tracking method the instruction stands for. The
// field stays an int; the backend calls the runtime function of the mode.
namespace tensor_track_mode {
inline constexpr int untrack = 0;
inline constexpr int track = 1;
inline constexpr int retrack = 2;
} // namespace tensor_track_mode

template <> struct InstructionTraits<TensorIsTracked> : InDomain<Domain::autograd> {};
template <> struct InstructionTraits<TensorHasGrad> : InDomain<Domain::autograd> {};
template <> struct InstructionTraits<TensorClearGrad> : InDomain<Domain::autograd> {};
template <> struct InstructionTraits<TensorTrack> : InDomain<Domain::autograd> {};
template <> struct InstructionTraits<TensorBackward> : InDomain<Domain::autograd> {};
template <> struct InstructionTraits<TensorGrad> : InDomain<Domain::autograd> {};
template <> struct InstructionTraits<AutogradTargetCreate> : InDomain<Domain::autograd> {};
template <> struct InstructionTraits<AutogradTargetHasGrad> : InDomain<Domain::autograd> {};
template <> struct InstructionTraits<AutogradTargetClearGrad> : InDomain<Domain::autograd> {};
template <> struct InstructionTraits<AutogradTargetGradient> : InDomain<Domain::autograd> {};

} // namespace quidra::ir
