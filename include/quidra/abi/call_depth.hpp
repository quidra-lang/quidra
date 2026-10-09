#pragma once

// The depth of self-recursive calls a program may reach. A function that
// calls itself counts its depth, and a call deeper than the limit stops the
// program with CALL_DEPTH_LIMIT instead of overflowing the native stack
// (the backend's self-depth guard and the prelude's quidra_stack_enter).

namespace quidra::abi {

inline constexpr int self_call_depth_limit = 4096;

} // namespace quidra::abi
