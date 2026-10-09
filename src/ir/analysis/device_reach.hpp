#pragma once

// Device reach: which functions of a module may leave device work behind
// when they return.
//
// Owns the one answer the backend asks before it writes the return of an
// exported function (export "C"): a function that reaches device work gets
// the boundary hook (quidra_runtime_export_leave), every other one returns to
// C with no extra work. The answer is conservative: a function reaches device
// work when it, or a function it may call, contains a tensor or autograd
// instruction (device synchronization included), starts task.all tasks, or
// calls a C function (an extern declaration; package natives may encode
// device work). An indirect call may call any function whose address the
// module takes.

#include <string>
#include <unordered_set>

namespace quidra::ir {
struct Module;
} // namespace quidra::ir

namespace quidra::ir::analysis {

// The names of the module's functions that may leave device work behind.
std::unordered_set<std::string> device_reaching_functions(const ir::Module& module);

} // namespace quidra::ir::analysis
