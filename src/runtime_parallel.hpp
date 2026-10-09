#pragma once

#include <cstdint>

// Runtime-internal side of the qcore_parallel_for mechanism declared in
// quidra/native_extension.h. Package code uses only the C ABI.
namespace quidra::runtime_parallel {

// Marks the current thread as running inside a region that must not fan out
// further: a qcore_parallel_for body or a task.all operation. While at least
// one scope is alive on a thread, qcore_parallel_for runs serially there and
// qcore_parallel_thread_count() reports 1.
class SerialScope {
public:
    SerialScope() noexcept;
    ~SerialScope();
    SerialScope(const SerialScope&) = delete;
    SerialScope& operator=(const SerialScope&) = delete;
};

// True while the calling thread runs a qcore_parallel_for body, at every
// thread count (serial calls included) and inside nested calls. A task.all
// operation alone does not count, because qcore_* calls are legal there.
bool inside_parallel_body() noexcept;

// The guard of every qcore_* entry point that is not pure data access (see
// "Inside a body" in quidra/native_extension.h). Inside a qcore_parallel_for
// body it stops the program with runtime error PARALLEL_BODY (exit status
// abi::failure_exit_status) before the call touches Core state; elsewhere it
// returns at once. Such a call races on Core state at more than one thread
// but works at one, so the guard depends on inside_parallel_body(), never on
// the thread count.
void reject_inside_parallel_body(const char* function) noexcept;

// Parses a QUIDRA_CPU_THREADS value: a decimal integer from 1 to 256 with no
// sign, spaces or suffix. Returns false for anything else.
bool parse_thread_count(const char* text, std::uint64_t& count) noexcept;

// Test hooks. A thread count of 0 restores the QUIDRA_CPU_THREADS/hardware
// default; a forced count is used as given, even above the hardware thread
// count, so the pool can be stressed with more threads than cores.
// QUIDRA_CPU_THREADS is still validated. The pool backend is the one used off
// Apple platforms; selecting it on Apple lets the portable path be tested there.
enum class Backend { Platform, Pool };
void set_thread_count_for_testing(std::uint64_t count) noexcept;
void set_backend_for_testing(Backend backend) noexcept;

} // namespace quidra::runtime_parallel
