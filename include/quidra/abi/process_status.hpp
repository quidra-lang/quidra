#pragma once

// The exit status of a program that a runtime failure stops: a trap, a
// fail-fast error, a failed deferred device check. Generated code (the
// prelude's quidra_fail) and the runtime both end a failing program with it.

namespace quidra::abi {

inline constexpr int failure_exit_status = 101;

} // namespace quidra::abi
