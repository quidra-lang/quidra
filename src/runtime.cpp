#include "device_backend.hpp"
#include "ieee_decimal.hpp"
#include "platform/environment.hpp"
#include "platform/clock.hpp"
#include "runtime_counters.hpp"
#include "runtime/profile.hpp"
#include "runtime/timeline.hpp"
#include "runtime/device_observation.hpp"
#include "runtime_internal.hpp"
#include "runtime_parallel.hpp"
#include "runtime_report.hpp"
#include "node_runtime_owner.hpp"
#include "runtime_trace.hpp"
#include "tensor_view_bounds.hpp"
#include "unified_storage.hpp"
#include "quidra/abi/dtype.hpp"
#include "quidra/abi/half_format.hpp"
#include "quidra/abi/complex_format.hpp"
#include "quidra/abi/io_status.hpp"
#include "quidra/abi/layout.hpp"
#include "quidra/abi/process_status.hpp"
#include "quidra/abi/string_codes.hpp"
#include "quidra/abi/tensor_index.hpp"
#include "quidra/abi/tensor_codes.hpp"
#include "quidra/abi/runtime_entry_points.hpp"
#include "quidra/abi/runtime_failure.hpp"
#include "quidra/slice_bounds.hpp"
#include "quidra/native_extension.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <bit>
#include <cerrno>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <system_error>
#include <thread>
#include <new>
#include <type_traits>
#include <variant>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// The ABI this runtime shares with generated code (include/quidra/abi).
namespace abi = quidra::abi;
extern "C" void quidra_runtime_start_observation() noexcept;

namespace {

// The element types of real16 and real16b tensors: their arithmetic computes
// in binary32 and rounds once (abi/half_format.hpp).
using Real16 = quidra::abi::half::Real16;
using Real16b = quidra::abi::half::Real16b;

template <typename T>
inline constexpr bool half_element = std::is_same_v<T, Real16> || std::is_same_v<T, Real16b>;

// A real element type: binary64, binary32 or a 16-bit real.
template <typename T>
inline constexpr bool real_element = std::is_floating_point_v<T> || half_element<T>;

using quidra::abi::complex::ComplexElement;
using quidra::abi::complex::complex_element;
using quidra::abi::complex::part_is_finite;

template <typename T>
bool element_is_finite(T value) {
    if constexpr (complex_element<T>) return part_is_finite(value.re) && part_is_finite(value.im);
    else if constexpr (half_element<T>) return quidra::abi::half::isfinite(value);
    else return std::isfinite(value);
}

// The observable extended-real contract excludes NaN, not signed infinity.
// Keep element_is_finite separate for range-checked storage conversions.
template <typename T>
bool element_is_nan(T value) {
    if constexpr (complex_element<T>)
        return std::isnan(static_cast<double>(value.re)) || std::isnan(static_cast<double>(value.im));
    else return std::isnan(static_cast<double>(value));
}

// The value of a real element as binary64, exactly.
template <typename T>
double element_value(T value) {
    return static_cast<double>(value);
}

} // namespace

// Stops the program (GPU_SCOPE, at the current statement) when the calling
// thread still holds a device stream for a package encode that the package
// code which began it left open (runtime_system.cpp).
void quidra_runtime_check_package_hold(const char* returned);

namespace {
// GPU_SCOPE at a location the runtime operation knows, in the format of the
// TENSOR and AUTOGRAD failures (no provenance suffix).
[[noreturn]] void fail_package_scope_at(const std::string& message,
                                        unsigned long long line,
                                        unsigned long long column) {
    quidra::runtime::report_failure(abi::FailureReason::package_scope_violation,
                                    abi::FailureArgs{.message = message}, line, column);
}

// The runtime's failure paths end a package encode hold the calling thread
// still has (native_extension.h), so that exit does not wait for its stream.
// When the hold recorded a protocol violation, that violation is the failure
// reported (GPU_SCOPE at the failing operation, in the format of the failure
// it replaces): Core work refused inside a package encoder scope, for
// example because an extern call returned with its scope open, ends the
// program this way.
void end_package_hold_before_failure(unsigned long long line,
                                     unsigned long long column) {
    if (quidra::device::open_package_holds.load(std::memory_order_relaxed) == 0)
        return;
    std::string violation;
    if (quidra::device::end_left_package_hold("the program failed", violation) !=
        quidra::device::LeftHold::Violated)
        return;
    fail_package_scope_at(violation, line, column);
}

// Where Core takes control back from package code inside a statement, a
// package encode the calling thread left open stops the program at the
// operation's own location (GPU_SCOPE): `returned` names what came back.
void check_package_hold_at(const char* returned, unsigned long long line,
                           unsigned long long column) {
    if (quidra::device::open_package_holds.load(std::memory_order_relaxed) == 0)
        return;
    std::string message;
    if (quidra::device::end_left_package_hold(returned, message) ==
        quidra::device::LeftHold::None)
        return;
    fail_package_scope_at(message, line, column);
}

// A custom autograd backward callback is package code that runs inside the
// backward() call at line:column, which is where a package encode it breaks
// or leaves open is reported: the statement provenance may still name a
// statement of a function that returned earlier in the same statement.
struct PackageBackwardCall {
    unsigned long long line{};
    unsigned long long column{};
};
thread_local const PackageBackwardCall* package_backward_call = nullptr;

class PackageBackwardScope {
public:
    PackageBackwardScope(unsigned long long line, unsigned long long column)
        : call_{line, column}, previous_(package_backward_call),
          held_before_(quidra::device::package_hold_active()) {
        package_backward_call = &call_;
    }
    ~PackageBackwardScope() { package_backward_call = previous_; }
    PackageBackwardScope(const PackageBackwardScope&) = delete;
    PackageBackwardScope& operator=(const PackageBackwardScope&) = delete;

    // After the callback returned. A hold the thread had before the call is
    // not the callback's; it is reported where that hold was left open.
    void returned() const {
        if (!held_before_)
            check_package_hold_at("a custom autograd backward callback returned",
                                  call_.line, call_.column);
    }

private:
    PackageBackwardCall call_;
    const PackageBackwardCall* previous_;
    bool held_before_;
};
} // namespace

extern "C" void qcore_execution_policy_set(int policy) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if (policy == QCORE_EXECUTION_FAST) {
        quidra::device::set_execution_mode(quidra::device::ExecutionMode::Fast);
        return;
    }
    if (policy == QCORE_EXECUTION_DETERMINISTIC) {
        quidra::device::set_execution_mode(
            quidra::device::ExecutionMode::Deterministic);
        return;
    }
    quidra::runtime::report_failure(abi::FailureReason::invalid_execution_policy, {}, 0, 0);
}

extern "C" int qcore_execution_policy_get() {
    return quidra::device::execution_mode() ==
        quidra::device::ExecutionMode::Deterministic
        ? QCORE_EXECUTION_DETERMINISTIC
        : QCORE_EXECUTION_FAST;
}

// Compatibility symbols for package sources created before the generic
// execution-policy ABI was formalized.
extern "C" void qcore_execution_fast() {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    qcore_execution_policy_set(QCORE_EXECUTION_FAST);
}

extern "C" void qcore_execution_deterministic() {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    qcore_execution_policy_set(QCORE_EXECUTION_DETERMINISTIC);
}

namespace {
int runtime_argc = 0;
char** runtime_argv = nullptr;
std::vector<unsigned char> runtime_used;
std::mutex runtime_args_mutex;

[[noreturn]] void cli_fail(const char* message) {
    std::fprintf(stderr, "Quidra CLI error: %s\n", message);
    std::exit(2);
}

int find_option(const char* name) {
    if (!name || !runtime_argv) return -1;
    const std::string target = std::string("--") + name;
    for (int i = 1; i < runtime_argc; ++i) {
        if (runtime_argv[i] && target == runtime_argv[i]) return i;
    }
    return -1;
}

void mark_used(int index) {
    std::lock_guard<std::mutex> lock(runtime_args_mutex);
    if (index >= 0 && index < static_cast<int>(runtime_used.size())) {
        runtime_used[static_cast<std::size_t>(index)] = 1;
    }
}

[[noreturn]] void runtime_allocation_failure() {
    quidra::runtime::report_uncoded("allocation failed");
}

using ManagedDrop = void (*)(void*);

struct InitializationTracker {
    std::uintptr_t data_base{};
    std::mutex parallel_mutex;
    std::size_t count{};
    std::size_t unit_bytes{};
    std::size_t data_offset{};
    std::size_t initialized_count{};
    bool fully_initialized{};
    std::vector<unsigned char> bits;
};

struct ManagedAllocation {
    void* base{};
    std::size_t size{};
    unsigned char small_pool_class{};
    std::size_t owners{1};
    std::size_t pins{};
    std::size_t array_capacity{};
    bool interior_range_tracked{true};
    bool shared_string_slab{};
    bool tensor_descriptor{};
    // Strings are immutable at the source level. The only internal mutation is
    // unique-owner suffix append, so a cursor into the existing prefix remains
    // valid across that optimization. This avoids a retained O(n) offset table.
    bool string_byte_length_known{};
    std::size_t string_byte_length{};
    // Set once the bytes have been checked as UTF-8. Unique-owner suffix append
    // only ever adds separately validated bytes, so the property is preserved and
    // the prefix never has to be rescanned again.
    bool string_utf8_validated{};
    bool string_codepoint_length_known{};
    std::size_t string_codepoint_length{};
    // Once validation proves byte length == code-point length, every byte is
    // ASCII. Keep that proof with the allocation so integer indexing can lower
    // to a direct byte load instead of re-entering the UTF-8 decoder.
    bool string_ascii_known{};
    bool string_ascii{};
    bool string_index_cursor_valid{};
    std::size_t string_index_cursor_codepoint{};
    std::size_t string_index_cursor_byte{};
    ManagedDrop drop{};
    std::unique_ptr<InitializationTracker> initialization;
};

struct ManagedFinalization {
    void* base{};
    ManagedDrop drop{};
    unsigned char small_pool_class{};
};

using ManagedAllocations = std::unordered_map<std::uintptr_t, ManagedAllocation>;

constexpr std::array<std::size_t, 5> small_managed_pool_sizes{
    16, 32, 64, 128, 256
};

struct SmallManagedPool {
    std::array<std::vector<void*>, small_managed_pool_sizes.size()> free_lists;
    ~SmallManagedPool() {
        for (auto& list : free_lists)
            for (void* value : list) std::free(value);
    }
};

struct ManagedRangeCacheEntry {
    ManagedAllocation* allocation{};
    std::uintptr_t begin{};
    std::size_t size{};
};

constexpr std::size_t managed_range_cache_slots = 4;

// One lifetime for the allocation registry, its pool and every pointer cache.
// C host exit handlers can call exported Quidra functions after this thread's
// C++ TLS destructors have run. Keep access through a trivial TLS pointer so
// such a call can start a fresh context without using a destroyed container.
struct ManagedState {
    ManagedAllocations allocations;
    SmallManagedPool pool;
    std::map<std::uintptr_t, std::size_t> ranges;
    long long append_last_codepoints{};
    const char* string_text{};
    ManagedAllocation* string_allocation{};
    const char* shared_text{};
    std::size_t shared_length{};
    ManagedAllocation* shared_allocation{};
    void* array_append_base{};
    ManagedAllocation* array_append_allocation{};
    std::array<ManagedRangeCacheEntry, managed_range_cache_slots> range_cache{};
    std::size_t range_cache_victim{};
    ManagedState* next_preserved{};
    bool preserved_for_exit{};
};

thread_local ManagedState* current_managed_state{};
thread_local bool managed_thread_teardown_complete{};
std::atomic<ManagedState*> preserved_managed_states{};

void destroy_managed_state() {
    auto* state = current_managed_state;
    if (!state || state->preserved_for_exit) return;
    current_managed_state = nullptr;
    delete state;
}

struct ManagedStateLifetime {
    ~ManagedStateLifetime() {
        managed_thread_teardown_complete = true;
        destroy_managed_state();
    }
};

ManagedState& managed_state() {
    if (!current_managed_state) {
        if (!managed_thread_teardown_complete) {
            static thread_local ManagedStateLifetime lifetime;
            (void)lifetime;
        }
        auto* state = new (std::nothrow) ManagedState;
        if (!state) runtime_allocation_failure();
        current_managed_state = state;
        // Usually the TLS guard deletes this context first. Also register an
        // exit callback: first-ever use can itself be inside a host exit
        // handler, after the TLS destructor phase has already finished.
        // An older handler can enter Quidra again with another context.
        if (std::atexit(destroy_managed_state) != 0) {
            destroy_managed_state();
            runtime_allocation_failure();
        }
    }
    return *current_managed_state;
}

void preserve_current_managed_state() {
    auto* state = current_managed_state;
    if (!state || state->preserved_for_exit) return;
    // Fatal exits do not unwind Quidra frames. Their active allocations must
    // remain valid throughout host exit callbacks. Preserve this context until
    // process termination, without allocating or invoking user drop callbacks.
    state->preserved_for_exit = true;
    auto* head = preserved_managed_states.load(std::memory_order_relaxed);
    do {
        state->next_preserved = head;
    } while (!preserved_managed_states.compare_exchange_weak(
        head, state, std::memory_order_release, std::memory_order_relaxed));
}

unsigned char small_managed_pool_class(std::size_t bytes) {
    for (std::size_t i = 0; i < small_managed_pool_sizes.size(); ++i)
        if (bytes <= small_managed_pool_sizes[i])
            return static_cast<unsigned char>(i + 1);
    return 0;
}

void* acquire_managed_memory(std::size_t requested, unsigned char pool_class,
                             std::size_t& actual_bytes) {
    if (pool_class != 0) {
        const auto index = static_cast<std::size_t>(pool_class - 1);
        actual_bytes = small_managed_pool_sizes[index];
        auto& list = managed_state().pool.free_lists[index];
        if (!list.empty()) {
            void* value = list.back();
            list.pop_back();
            return value;
        }
        void* value = std::malloc(actual_bytes);
        if (!value) runtime_allocation_failure();
        return value;
    }
    actual_bytes = requested;
    void* value = std::malloc(actual_bytes);
    if (!value) runtime_allocation_failure();
    return value;
}

void recycle_managed_memory(void* base, unsigned char pool_class) {
    if (!base) return;
    if (pool_class == 0) {
        std::free(base);
        return;
    }
    if (quidra::runtime::node_failure_active()) {
        try {
            managed_state().pool.free_lists[
                static_cast<std::size_t>(pool_class - 1)].push_back(base);
        } catch (...) {
            std::free(base);
            return;
        }
        return;
    }
    managed_state().pool.free_lists[
        static_cast<std::size_t>(pool_class - 1)].push_back(base);
}

// Repeated xs = xs.append(value) loops hit the same exact allocation for long
// stretches. Keep only the allocation-record lookup cached; every move-safety
// property is still re-read on each operation, so aliases, pins, and
// initialization state retain their ordinary semantics.
ManagedAllocation* exact_array_append_allocation(void* array) {
    if (array && array == managed_state().array_append_base &&
        managed_state().array_append_allocation) {
        return managed_state().array_append_allocation;
    }
    if (!array) return nullptr;
    const auto it =
        managed_state().allocations.find(reinterpret_cast<std::uintptr_t>(array));
    if (it == managed_state().allocations.end()) return nullptr;
    managed_state().array_append_base = array;
    managed_state().array_append_allocation = &it->second;
    return managed_state().array_append_allocation;
}

void invalidate_array_append_cache(const ManagedAllocation* allocation) {
    if (managed_state().array_append_allocation != allocation) return;
    managed_state().array_append_base = nullptr;
    managed_state().array_append_allocation = nullptr;
}

ManagedAllocation* exact_managed_string(const char* text) {
    if (text && text == managed_state().string_text &&
        managed_state().string_allocation) {
        return managed_state().string_allocation;
    }
    if (!text) return nullptr;
    const auto it =
        managed_state().allocations.find(reinterpret_cast<std::uintptr_t>(text));
    if (it == managed_state().allocations.end()) return nullptr;
    managed_state().string_text = text;
    managed_state().string_allocation = &it->second;
    return managed_state().string_allocation;
}

void invalidate_managed_string_cache(const ManagedAllocation* allocation) {
    if (managed_state().string_allocation != allocation) return;
    managed_state().string_text = nullptr;
    managed_state().string_allocation = nullptr;
}

void invalidate_shared_string_cache(const ManagedAllocation* allocation = nullptr) {
    if (allocation && managed_state().shared_allocation != allocation) return;
    managed_state().shared_text = nullptr;
    managed_state().shared_length = 0;
    managed_state().shared_allocation = nullptr;
}

// Interior references need an ordered range index, but exact owner operations do not.
// Generated element accesses cluster in a small working set of allocations, so cache
// those ranges and avoid a tree lookup on every initialization check. The set holds
// several entries because the common loops read one array while writing another; a
// single entry would miss on every access of a two-array algorithm such as a merge.
void clear_managed_range_cache(const ManagedAllocation* allocation = nullptr) {
    for (auto& entry : managed_state().range_cache) {
        if (allocation && entry.allocation != allocation) continue;
        entry = ManagedRangeCacheEntry{};
    }
    if (!allocation) managed_state().range_cache_victim = 0;
}

void finalize_managed(ManagedFinalization finalization) {
    if (!finalization.base) return;
    if (quidra::runtime::node_failure_active()) {
        if(finalization.drop)quidra::runtime::cleanup_node_slot(finalization.drop,finalization.base);
        recycle_managed_memory(finalization.base, finalization.small_pool_class);
        const auto* owner=quidra::runtime::current_node_failure;
        if(owner && (owner->report || owner->exit_status))
            throw quidra::runtime::CapturedNodeFailure{};
        return;
    }
    if (finalization.drop) finalization.drop(finalization.base);
    recycle_managed_memory(
        finalization.base, finalization.small_pool_class);
}

ManagedAllocation* managed_containing(const void* address) {
    if (!address) return nullptr;
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    for (const auto& entry : managed_state().range_cache) {
        if (entry.allocation && target >= entry.begin && target - entry.begin < entry.size) {
            return entry.allocation;
        }
    }
    if (managed_state().ranges.empty()) return nullptr;
    auto range = managed_state().ranges.upper_bound(target);
    if (range == managed_state().ranges.begin()) return nullptr;
    --range;
    const auto begin = range->first;
    const auto size = range->second;
    if (target < begin || target - begin >= size) return nullptr;
    const auto it = managed_state().allocations.find(begin);
    if (it == managed_state().allocations.end()) return nullptr;
    auto& entry = managed_state().range_cache[managed_state().range_cache_victim];
    managed_state().range_cache_victim = (managed_state().range_cache_victim + 1) % managed_range_cache_slots;
    entry.allocation = &it->second;
    entry.begin = begin;
    entry.size = size;
    return entry.allocation;
}

void* managed_allocate_impl(std::size_t bytes, bool track_interior_range) {
    if (bytes == 0) bytes = 1;
    const auto pool_class = small_managed_pool_class(bytes);
    std::size_t actual_bytes = 0;
    auto* memory = acquire_managed_memory(bytes, pool_class, actual_bytes);
    const auto key = reinterpret_cast<std::uintptr_t>(memory);
    ManagedAllocation allocation;
    allocation.base = memory;
    allocation.size = actual_bytes;
    allocation.small_pool_class = pool_class;
    allocation.interior_range_tracked = track_interior_range;
    if (quidra::runtime::node_failure_active()) {
        bool installed = false;
        try {
            managed_state().allocations.emplace(key, std::move(allocation));
            installed = true;
            if (track_interior_range) managed_state().ranges.emplace(key, actual_bytes);
        } catch (...) {
            if (installed) managed_state().allocations.erase(key);
            managed_state().ranges.erase(key);
            // The acquired pool block is detached already; free needs no allocation.
            std::free(memory);
            throw;
        }
        return memory;
    }
    managed_state().allocations.emplace(key, std::move(allocation));

    // std::unordered_map rehash invalidates iterators, not references or pointers
    // to elements. The range cache stores pointers to mapped values, so inserting
    // another exact allocation does not require flushing the cache.
    //
    // Strings never expose interior references: indexing and slicing return new
    // values. Keep them out of the ordered range index entirely; exact ownership
    // still lives in managed_state().allocations.
    if (track_interior_range) managed_state().ranges.emplace(key, actual_bytes);
    return memory;
}

void* managed_allocate(std::size_t bytes) {
    return managed_allocate_impl(bytes, true);
}

void* managed_allocate_string(std::size_t bytes) {
    return managed_allocate_impl(bytes, false);
}

bool tracker_bit(const InitializationTracker& tracker, std::size_t index) {
    if (tracker.fully_initialized) return true;
    const auto byte = index / 8;
    const auto bit = static_cast<unsigned char>(1U << (index % 8));
    return byte < tracker.bits.size() && (tracker.bits[byte] & bit) != 0;
}

void tracker_set(InitializationTracker& tracker, std::size_t index) {
    if (tracker.fully_initialized || index >= tracker.count) return;
    // A pending write-only output carries no bitset until an element is
    // written individually (qcore_tensor_output_handle).
    if (tracker.bits.empty()) tracker.bits.assign((tracker.count + 7) / 8, 0);
    const auto byte = index / 8;
    const auto bit = static_cast<unsigned char>(1U << (index % 8));
    if ((tracker.bits[byte] & bit) != 0) return;
    tracker.bits[byte] = static_cast<unsigned char>(tracker.bits[byte] | bit);
    ++tracker.initialized_count;
    if (tracker.initialized_count == tracker.count) {
        tracker.fully_initialized = true;
        tracker.bits.clear();
        tracker.bits.shrink_to_fit();
    }
}

// Marks every tracked element initialized at once, for outputs whose producer
// has already proven that it wrote an initialized value to each element.
void tracker_mark_complete(InitializationTracker& tracker) {
    tracker.fully_initialized = true;
    tracker.initialized_count = tracker.count;
    tracker.bits.clear();
    tracker.bits.shrink_to_fit();
}

// True when the address has no initialization tracking left to prove: either it is not
// tracked storage at all, or every element of its allocation is already initialized.
bool tracker_is_complete(const void* address) {
    const auto* allocation = managed_containing(address);
    if (!allocation || !allocation->initialization) return true;
    return allocation->initialization->fully_initialized;
}

std::optional<std::pair<ManagedAllocation*, std::size_t>>
tracked_unit_for_address(const void* address) {
    auto* allocation = managed_containing(address);
    if (!allocation || !allocation->initialization) return std::nullopt;
    auto& tracker = *allocation->initialization;
    const auto base = reinterpret_cast<std::uintptr_t>(allocation->base);
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    if (target < base + tracker.data_offset || tracker.unit_bytes == 0) return std::nullopt;
    const auto relative = static_cast<std::size_t>(target - base - tracker.data_offset);
    if (relative % tracker.unit_bytes != 0) return std::nullopt;
    const auto index = relative / tracker.unit_bytes;
    if (index >= tracker.count) return std::nullopt;
    return std::pair<ManagedAllocation*, std::size_t>{allocation, index};
}

[[noreturn]] void runtime_uninitialized_failure(unsigned long long line,
                                                unsigned long long column) {
    quidra::runtime::report_failure(abi::FailureReason::value_uninitialized, {}, line,
                                    column);
}

[[noreturn]] void runtime_text_failure(const char* message) {
    quidra::runtime::report_uncoded(message ? message : "runtime failure");
}

bool valid_utf8(std::string_view text, std::size_t* codepoints = nullptr,
                bool* contains_nul = nullptr) {
    std::size_t index = 0;
    std::size_t count = 0;
    bool nul = false;
    while (index < text.size()) {
        // ASCII dominates source text, protocol fields, and decoded byte
        // buffers. Validate eight bytes at once when all are non-NUL ASCII,
        // then fall back to the exact scalar UTF-8 decoder at the first block
        // that may contain a control terminator or multibyte code point.
        while (text.size() - index >= sizeof(std::uint64_t)) {
            std::uint64_t word = 0;
            std::memcpy(&word, text.data() + index, sizeof(word));
            constexpr std::uint64_t high_bits = 0x8080808080808080ULL;
            constexpr std::uint64_t low_bits = 0x0101010101010101ULL;
            const bool has_non_ascii = (word & high_bits) != 0;
            const bool has_nul =
                ((word - low_bits) & ~word & high_bits) != 0;
            if (has_non_ascii || has_nul) break;
            index += sizeof(word);
            count += sizeof(word);
        }
        if (index >= text.size()) break;

        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7fU) {
            nul = nul || first == 0;
            ++index;
            ++count;
            continue;
        }

        std::size_t width = 0;
        std::uint32_t value = 0;
        std::uint32_t minimum = 0;
        if ((first & 0xe0U) == 0xc0U) {
            width = 2;
            value = first & 0x1fU;
            minimum = 0x80U;
        } else if ((first & 0xf0U) == 0xe0U) {
            width = 3;
            value = first & 0x0fU;
            minimum = 0x800U;
        } else if ((first & 0xf8U) == 0xf0U) {
            width = 4;
            value = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (index + width > text.size()) return false;
        for (std::size_t offset = 1; offset < width; ++offset) {
            const auto byte = static_cast<unsigned char>(text[index + offset]);
            if ((byte & 0xc0U) != 0x80U) return false;
            value = (value << 6U) | (byte & 0x3fU);
        }
        if (value < minimum || value > 0x10ffffU ||
            (value >= 0xd800U && value <= 0xdfffU)) {
            return false;
        }
        index += width;
        ++count;
    }
    if (codepoints) *codepoints = count;
    if (contains_nul) *contains_nul = nul;
    return true;
}

bool valid_runtime_text(std::string_view text) {
    bool contains_nul = false;
    return valid_utf8(text, nullptr, &contains_nul) && !contains_nul;
}

void mark_managed_string(char* value, std::size_t byte_length,
                         std::optional<std::size_t> codepoints = std::nullopt) {
    const auto it =
        managed_state().allocations.find(reinterpret_cast<std::uintptr_t>(value));
    if (it == managed_state().allocations.end())
        runtime_text_failure("string storage is not managed");
    auto& allocation = it->second;
    allocation.string_byte_length_known = true;
    allocation.string_byte_length = byte_length;
    allocation.string_utf8_validated = true;
    allocation.string_codepoint_length_known = codepoints.has_value();
    allocation.string_codepoint_length = codepoints.value_or(0);
    allocation.string_ascii_known = codepoints.has_value();
    allocation.string_ascii =
        codepoints.has_value() && codepoints.value() == byte_length;
    allocation.string_index_cursor_valid = false;
    allocation.string_index_cursor_codepoint = 0;
    allocation.string_index_cursor_byte = 0;
}

char* copy_validated_runtime_text(
    std::string_view value,
    std::optional<std::size_t> codepoints = std::nullopt) {
    if (value.size() == std::numeric_limits<std::size_t>::max())
        runtime_allocation_failure();
    auto* result =
        static_cast<char*>(managed_allocate_string(value.size() + 1));
    if (!value.empty()) std::memcpy(result, value.data(), value.size());
    result[value.size()] = '\0';
    mark_managed_string(result, value.size(), codepoints);
    return result;
}

char* copy_runtime_text(std::string_view value) {
    std::size_t codepoints = 0;
    bool contains_nul = false;
    if (!valid_utf8(value, &codepoints, &contains_nul))
        runtime_text_failure("string text is not valid UTF-8");
    if (contains_nul) runtime_text_failure("string text cannot contain NUL");
    return copy_validated_runtime_text(value, codepoints);
}

char* runtime_copy_string(const std::string& value) {
    return copy_runtime_text(value);
}

std::uint32_t utf8_next(std::string_view text, std::size_t& index) {
    if(index>=text.size())runtime_text_failure("invalid UTF-8 string");
    const auto first=static_cast<unsigned char>(text[index]);
    if(first<=0x7fU){++index;return first;}
    std::size_t width=0;std::uint32_t value=0,minimum=0;
    if((first&0xe0U)==0xc0U){width=2;value=first&0x1fU;minimum=0x80U;}
    else if((first&0xf0U)==0xe0U){width=3;value=first&0x0fU;minimum=0x800U;}
    else if((first&0xf8U)==0xf0U){width=4;value=first&0x07U;minimum=0x10000U;}
    else runtime_text_failure("invalid UTF-8 string");
    if(index+width>text.size())runtime_text_failure("invalid UTF-8 string");
    for(std::size_t o=1;o<width;++o){const auto b=static_cast<unsigned char>(text[index+o]);if((b&0xc0U)!=0x80U)runtime_text_failure("invalid UTF-8 string");value=(value<<6U)|(b&0x3fU);}
    if(value<minimum||value>0x10ffffU||(value>=0xd800U&&value<=0xdfffU))runtime_text_failure("invalid UTF-8 string");
    index+=width;return value;
}
std::size_t utf8_length(std::string_view text) {
    std::size_t index = 0;
    std::size_t count = 0;
    while (index < text.size()) {
        (void)utf8_next(text, index);
        ++count;
    }
    return count;
}

std::size_t utf8_prefix_length(std::string_view text, std::size_t byte_end) {
    std::size_t index = 0;
    std::size_t count = 0;
    while (index < byte_end) {
        (void)utf8_next(text, index);
        ++count;
    }
    if (index != byte_end) runtime_text_failure("invalid UTF-8 boundary");
    return count;
}

struct StringIndexBounds {
    bool found{};
    std::size_t start{};
    std::size_t end{};
};

std::string_view cached_string_view(const char* text, ManagedAllocation*& allocation) {
    allocation = nullptr;
    if (!text) runtime_text_failure("null string");

    if (text == managed_state().shared_text && managed_state().shared_allocation) {
        // The cached length belongs to this slice, but UTF-8/code-point metadata
        // in ManagedAllocation belongs to the backing slab as a whole. Keep
        // allocation null so validated_string_view never reuses one slice's
        // semantic metadata for another slice from the same slab.
        return std::string_view(text, managed_state().shared_length);
    }

    ManagedAllocation* found = exact_managed_string(text);
    if (!found) {
        auto* containing = managed_containing(text);
        if (containing && containing->shared_string_slab) found = containing;
    }
    if (!found) return std::string_view(text);

    if (found->shared_string_slab) {
        const auto begin = reinterpret_cast<std::uintptr_t>(found->base);
        const auto address = reinterpret_cast<std::uintptr_t>(text);
        if (address < begin || address - begin >= found->size)
            runtime_text_failure("invalid shared string slice");
        const auto remaining =
            found->size - static_cast<std::size_t>(address - begin);
        const auto* end =
            static_cast<const char*>(std::memchr(text, '\0', remaining));
        if (!end)
            runtime_text_failure("shared string slice is missing a terminator");
        return std::string_view(
            text, static_cast<std::size_t>(end - text));
    }

    allocation = found;
    if (!allocation->string_byte_length_known) {
        const auto* end = static_cast<const char*>(
            std::memchr(text, '\0', allocation->size));
        if (!end) runtime_text_failure("managed string is missing a terminator");
        allocation->string_byte_length = static_cast<std::size_t>(end - text);
        allocation->string_byte_length_known = true;
    }
    return std::string_view(text, allocation->string_byte_length);
}

std::string_view validated_string_view(
    const char* text, ManagedAllocation*& allocation,
    std::size_t* codepoints = nullptr) {
    // Cached iterator slices come from an already validated UTF-8 source.
    // Splitting a valid UTF-8 string on a valid UTF-8 separator cannot create
    // an invalid code-point boundary, so reparsing every hot-loop slice is
    // redundant. Keep per-slice length metadata out of the backing allocation.
    if (text && text == managed_state().shared_text &&
        managed_state().shared_allocation) {
        allocation = nullptr;
        const auto source =
            std::string_view(text, managed_state().shared_length);
        if (codepoints) {
            if (managed_state().shared_allocation->string_ascii_known &&
                managed_state().shared_allocation->string_ascii) {
                *codepoints = source.size();
            } else {
                *codepoints = utf8_length(source);
            }
        }
        return source;
    }

    const auto source = cached_string_view(text, allocation);
    if (allocation && allocation->string_utf8_validated) {
        if (codepoints) {
            if (!allocation->string_codepoint_length_known) {
                allocation->string_codepoint_length = utf8_length(source);
                allocation->string_codepoint_length_known = true;
            }
            *codepoints = allocation->string_codepoint_length;
        }
        return source;
    }

    std::size_t count = 0;
    if (!valid_utf8(source, &count)) runtime_text_failure("invalid UTF-8 string");
    if (allocation) {
        allocation->string_utf8_validated = true;
        allocation->string_codepoint_length_known = true;
        allocation->string_codepoint_length = count;
    }
    if (codepoints) *codepoints = count;
    return source;
}

StringIndexBounds utf8_index_bounds(const char* text, std::size_t position) {
    ManagedAllocation* allocation = nullptr;
    const auto source = cached_string_view(text, allocation);

    std::size_t codepoint = 0;
    std::size_t byte = 0;
    if (allocation && allocation->string_index_cursor_valid &&
        allocation->string_index_cursor_codepoint <= position &&
        allocation->string_index_cursor_byte <= source.size()) {
        codepoint = allocation->string_index_cursor_codepoint;
        byte = allocation->string_index_cursor_byte;
    }

    while (codepoint < position && byte < source.size()) {
        (void)utf8_next(source, byte);
        ++codepoint;
    }

    if (codepoint != position || byte >= source.size()) {
        if (allocation) {
            allocation->string_index_cursor_valid = true;
            allocation->string_index_cursor_codepoint = codepoint;
            allocation->string_index_cursor_byte = byte;
            if (byte == source.size()) {
                allocation->string_codepoint_length_known = true;
                allocation->string_codepoint_length = codepoint;
            }
        }
        return {};
    }

    const auto start = byte;
    (void)utf8_next(source, byte);
    ++codepoint;
    if (allocation) {
        allocation->string_index_cursor_valid = true;
        allocation->string_index_cursor_codepoint = codepoint;
        allocation->string_index_cursor_byte = byte;
        if (byte == source.size()) {
            allocation->string_codepoint_length_known = true;
            allocation->string_codepoint_length = codepoint;
        }
    }
    return {true, start, byte};
}

bool unicode_space(std::uint32_t v){return(v>=0x09U&&v<=0x0dU)||v==0x20U||v==0x85U||v==0xa0U||v==0x1680U||(v>=0x2000U&&v<=0x200aU)||v==0x2028U||v==0x2029U||v==0x202fU||v==0x205fU||v==0x3000U;}


}

// Signed infinities are observable real values; NaN is never valid text.
// Formatting options do not pad or append decimal digits to inf/-inf.
[[noreturn]] void runtime_nonfinite_text() {
    runtime_text_failure("internal error: NaN reached real text formatting");
}

extern "C" char* quidra_format_float(double value) {
    if (std::isnan(value)) runtime_nonfinite_text();
    if (std::isinf(value)) return runtime_copy_string(std::signbit(value) ? "-inf" : "inf");
    if (value == 0.0) return runtime_copy_string(std::signbit(value) ? "-0.0" : "0.0");

    char buffer[64];
    const auto result =
        std::to_chars(std::begin(buffer), std::end(buffer), value, std::chars_format::general);
    if (result.ec != std::errc{}) runtime_text_failure("real formatting failed");

    std::string text(buffer, result.ptr);
    if (text.find('.') == std::string::npos) {
        const auto exponent = text.find_first_of("eE");
        if (exponent == std::string::npos) text += ".0";
        else text.insert(exponent, ".0");
    }
    return runtime_copy_string(text);
}

namespace {

std::string format_integer_padding(std::string text, int width, bool zero) {
    if (width < 0) return text;
    const std::size_t sign = !text.empty() && (text[0] == '-' || text[0] == '+') ? 1 : 0;
    auto end = text.find('.');
    if (end == std::string::npos) end = text.find_first_of("eE");
    if (end == std::string::npos) end = text.size();
    const auto digits = end > sign ? end - sign : 0;
    if (digits >= static_cast<std::size_t>(width)) return text;
    const auto count = static_cast<std::size_t>(width) - digits;
    if (zero) text.insert(sign, count, '0');
    else text.insert(0, count, ' ');
    return text;
}

std::string format_decimal(double value, std::chars_format format, int digits) {
    const auto capacity = static_cast<std::size_t>(digits) + 384;
    std::string text(capacity, '\0');
    const auto result = std::to_chars(text.data(), text.data() + text.size(), value, format, digits);
    if (result.ec != std::errc{}) runtime_text_failure("numeric formatting failed");
    text.resize(static_cast<std::size_t>(result.ptr - text.data()));
    return text;
}

std::string format_fixed(double value, int digits) {
    return format_decimal(value, std::chars_format::fixed, digits);
}

std::string integer_significant(std::string text, int significant) {
    const bool negative = !text.empty() && text[0] == '-';
    const std::size_t offset = negative ? 1 : 0;
    std::string digits = text.substr(offset);
    if (static_cast<int>(digits.size()) < significant) {
        const auto zeros = static_cast<std::size_t>(significant) - digits.size();
        digits += ".";
        digits.append(zeros, '0');
    } else if (static_cast<int>(digits.size()) > significant) {
        const bool round_up = digits[static_cast<std::size_t>(significant)] >= '5';
        for (std::size_t i = static_cast<std::size_t>(significant); i < digits.size(); ++i) digits[i] = '0';
        if (round_up) {
            std::size_t i = static_cast<std::size_t>(significant);
            while (i > 0) {
                --i;
                if (digits[i] != '9') { ++digits[i]; break; }
                digits[i] = '0';
                if (i == 0) digits.insert(digits.begin(), '1');
            }
        }
    }
    return negative ? "-" + digits : digits;
}

std::string finish_integer_format(std::string text, int integer_width, int fractional, int significant, bool zero) {
    if (significant >= 0) {
        text = integer_significant(std::move(text), significant);
    } else if (fractional > 0) {
        text += ".";
        text.append(static_cast<std::size_t>(fractional), '0');
    }
    return format_integer_padding(std::move(text), integer_width, zero);
}

std::string format_float_significant(double value, int significant) {
    if (std::isnan(value)) runtime_nonfinite_text();
    if (std::isinf(value)) return std::signbit(value) ? "-inf" : "inf";
    if (value == 0.0) return format_fixed(value, significant - 1);

    const auto exponent = static_cast<int>(std::floor(std::log10(std::fabs(value))));
    const int fractional = significant - exponent - 1;
    if (fractional >= 0) return format_fixed(value, fractional);

    const int places = -fractional;
    if (places <= 18) {
        const double scale = std::pow(10.0, places);
        const double rounded = std::round(value / scale) * scale;
        if (std::isfinite(rounded)) return format_fixed(rounded, 0);
    }

    return format_decimal(value, std::chars_format::scientific, significant - 1);
}

template<class Float>
std::string canonical_float_text(Float value) {
    if (std::isnan(value)) runtime_nonfinite_text();
    if (std::isinf(value)) return std::signbit(value) ? "-inf" : "inf";
    if (value == 0.0) return std::signbit(value) ? "-0.0" : "0.0";
    if constexpr (std::is_same_v<Float, float>) {
        // Choose the first decimal precision that round-trips to binary32.
        // Explicit binary64 formatting avoids library-specific float defaults.
        namespace ieee = quidra::ieee_decimal;
        const auto bits = std::bit_cast<std::uint32_t>(value);
        const bool asymmetric = (bits & 0x007fffffU) == 0 && (bits & 0x7f800000U) != 0;
        char candidate[64];
        for (int digits = 1; digits <= std::numeric_limits<float>::max_digits10; ++digits) {
            const auto written = std::to_chars(std::begin(candidate), std::end(candidate) - 1,
                static_cast<double>(value), std::chars_format::scientific, digits - 1);
            if (written.ec != std::errc{}) runtime_text_failure("real formatting failed");
            *written.ptr = '\0';
            ieee::Decimal decimal;
            if (!ieee::parse(std::string_view(candidate, static_cast<std::size_t>(written.ptr - candidate)), decimal))
                runtime_text_failure("real formatting failed");
            if (ieee::round(decimal, ieee::binary32).bits == bits)
                return canonical_float_text(ieee::binary64_value(ieee::round(decimal, ieee::binary64)));
            // Powers of two have a narrower interval on the lower side.
            // A neighbouring decimal can round-trip when the nearest cannot.
            if (asymmetric) {
                std::int64_t significand = 0;
                for (const auto digit : decimal.integer_digits)
                    significand = significand * 10 + (digit - '0');
                for (const auto digit : decimal.fraction_digits)
                    significand = significand * 10 + (digit - '0');
                for (const int delta : {-1, 1}) {
                    const auto adjacent_digits = std::to_string(significand + delta);
                    const ieee::Decimal adjacent{decimal.negative, adjacent_digits, {},
                        decimal.exponent - static_cast<long long>(decimal.fraction_digits.size())};
                    if (ieee::round(adjacent, ieee::binary32).bits == bits)
                        return canonical_float_text(ieee::binary64_value(ieee::round(adjacent, ieee::binary64)));
                }
            }
        }
        runtime_text_failure("real formatting failed");
    }
    char buffer[64];
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value, std::chars_format::general);
    if (result.ec != std::errc{}) runtime_text_failure("real formatting failed");
    std::string text(buffer, result.ptr);
    if (text.find('.') == std::string::npos) {
        const auto exponent = text.find_first_of("eE");
        if (exponent == std::string::npos) text += ".0";
        else text.insert(exponent, ".0");
    }
    return text;
}

} // namespace

namespace {

// The format of a 16-bit real code (HalfPrecision's half_format_*): 0 is
// real16, 1 real16b.
const quidra::abi::half::HalfFormat& half_format_of(int format) {
    return format == 1 ? quidra::abi::half::bfloat16 : quidra::abi::half::binary16;
}

const quidra::ieee_decimal::Format& half_decimal_format(int format) {
    return format == 1 ? quidra::ieee_decimal::bfloat16 : quidra::ieee_decimal::binary16;
}

} // namespace

// The text of a 16-bit real: without a fraction or significant-digit count,
// the shortest decimal that reads back to the same value of its format,
// written as real32 and real64 values are (`0.1`, `4.0`, `1.0e+20`); with
// one, the value's digits as quidra_format_number writes them; padded to
// `integer_width` as there.
extern "C" char* quidra_half_format(unsigned int bits, int format, int integer_width,
                                    int fractional, int significant, int zero) {
    namespace ieee = quidra::ieee_decimal;
    const auto value = quidra::abi::half::value_of(static_cast<std::uint16_t>(bits),
                                                   half_format_of(format));
    if (std::isnan(value)) runtime_nonfinite_text();
    if (std::isinf(value)) return runtime_copy_string(std::signbit(value) ? "-inf" : "inf");
    std::string text;
    if (fractional >= 0) {
        text = format_fixed(value, fractional);
    } else if (significant >= 0) {
        text = format_float_significant(value, significant);
    } else if (value == 0.0) {
        text = std::signbit(value) ? "-0.0" : "0.0";
    } else {
        text = canonical_float_text(value);
        char buffer[64];
        for (int digits = 1; digits <= 17; ++digits) {
            const auto written = std::to_chars(std::begin(buffer), std::end(buffer) - 1, value,
                                               std::chars_format::scientific, digits - 1);
            if (written.ec != std::errc{}) break;
            *written.ptr = '\0';
            ieee::Decimal decimal;
            if (!ieee::parse(std::string_view(buffer, static_cast<std::size_t>(written.ptr - buffer)),
                             decimal))
                break;
            const auto rounded = ieee::round(decimal, half_decimal_format(format));
            if ((rounded.bits & 0xffffU) == (bits & 0xffffU)) {
                text = canonical_float_text(std::strtod(buffer, nullptr));
                break;
            }
        }
    }
    return runtime_copy_string(format_integer_padding(std::move(text), integer_width, zero != 0));
}

// Reads a 16-bit real: the decimal value of the text rounded once to the
// format, ties to even, as the real32 and real64 parsers read; text beyond
// the finite range, or not zero but rounding to zero, is no value.
extern "C" bool quidra_half_parse(const char* text, int format, unsigned short* out) {
    namespace ieee = quidra::ieee_decimal;
    // A failure records its reason's position in NUMERIC_PARSE's list
    // (INVALID_TEXT, OUT_OF_RANGE), as the other numeric parsers do.
    const auto failed = [](bool out_of_range) {
        quidra::runtime::record_failure_reason(out_of_range ? 1 : 0);
        return false;
    };
    if (!text || !out || !*text) return failed(false);
    const char* start = text;
    while (*start == ' ' || (*start >= '\t' && *start <= '\r')) ++start;
    ieee::Decimal decimal;
    if (ieee::parse(start, decimal)) {
        const auto rounded = ieee::round(decimal, half_decimal_format(format));
        if (rounded.overflow || rounded.underflow_to_zero) return failed(true);
        *out = static_cast<unsigned short>(rounded.bits & 0xffffU);
        return true;
    }
    // Other text strtod reads (hexadecimal): its exact binary64 value, rounded
    // once.
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (errno == ERANGE) return failed(true);
    if (end == text || !end || *end != '\0' || std::isnan(value)) return failed(false);
    const auto& half = half_format_of(format);
    const auto bits = quidra::abi::half::round_to(value, half);
    if (std::isfinite(value) && !quidra::abi::half::is_finite(bits, half)) return failed(true);
    if (value != 0.0 && quidra::abi::half::value_of(bits, half) == 0.0) return failed(true);
    *out = bits;
    return true;
}

// real64 and the 64-bit integers to real16b, rounded once. An integer beyond
// 2^53 first keeps its 53 leading bits with the rest ORed into the last one,
// which decides every rounding to 8 bits as the full value does.
extern "C" unsigned int quidra_real16_from_float64(double value) {
    return quidra::abi::half::round_to(value, quidra::abi::half::binary16);
}

extern "C" unsigned int quidra_real16b_from_float64(double value) {
    return quidra::abi::half::round_to(value, quidra::abi::half::bfloat16);
}

namespace {

double integer_for_half_rounding(unsigned long long magnitude) {
    int bits = 0;
    for (auto rest = magnitude; rest != 0; rest >>= 1) ++bits;
    if (bits <= 53) return static_cast<double>(magnitude);
    const int shift = bits - 53;
    const auto dropped = magnitude & ((1ULL << shift) - 1ULL);
    const auto kept = (magnitude >> shift) | (dropped != 0 ? 1ULL : 0ULL);
    return std::ldexp(static_cast<double>(kept), shift);
}

} // namespace

extern "C" unsigned int quidra_real16b_from_int64(long long value) {
    const auto magnitude = value < 0 ? 0ULL - static_cast<unsigned long long>(value)
                                     : static_cast<unsigned long long>(value);
    const auto rounded = integer_for_half_rounding(magnitude);
    return quidra::abi::half::round_to(value < 0 ? -rounded : rounded, quidra::abi::half::bfloat16);
}

extern "C" unsigned int quidra_real16b_from_uint64(unsigned long long value) {
    return quidra::abi::half::round_to(integer_for_half_rounding(value),
                                       quidra::abi::half::bfloat16);
}

extern "C" char* quidra_runtime_copy_text(
    const char* data, unsigned long long raw_size) {
    if (raw_size >
        static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        runtime_allocation_failure();
    }
    const auto size = static_cast<std::size_t>(raw_size);
    if (!data && size != 0) runtime_text_failure("null string text");
    return copy_runtime_text(std::string_view(data ? data : "", size));
}

extern "C" char* quidra_format_signed(long long value, int integer_width, int fractional, int significant, int zero) {
    char buffer[32];
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
    if (result.ec != std::errc{}) runtime_text_failure("numeric formatting failed");
    return runtime_copy_string(finish_integer_format(std::string(buffer, result.ptr), integer_width, fractional, significant, zero != 0));
}

extern "C" char* quidra_format_unsigned(unsigned long long value, int integer_width, int fractional, int significant, int zero) {
    char buffer[32];
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
    if (result.ec != std::errc{}) runtime_text_failure("numeric formatting failed");
    return runtime_copy_string(finish_integer_format(std::string(buffer, result.ptr), integer_width, fractional, significant, zero != 0));
}

// An arbitrary-precision integer word, formatted as quidra_format_signed
// formats an int64.
extern "C" char* quidra_int_format(long long value, int integer_width, int fractional, int significant, int zero) {
    if ((value & abi::bare_integer_layout::boxed_bit) == 0)
        return quidra_format_signed(value >> 1, integer_width, fractional, significant, zero);
    char* text = quidra_int_text(value);
    std::string digits(text);
    quidra_managed_release(text, nullptr);
    return runtime_copy_string(finish_integer_format(std::move(digits), integer_width, fractional, significant, zero != 0));
}

extern "C" char* quidra_format_number(double value, int integer_width, int fractional, int significant, int zero) {
    if (std::isnan(value)) runtime_nonfinite_text();
    if (std::isinf(value)) return runtime_copy_string(std::signbit(value) ? "-inf" : "inf");
    std::string text;
    if (fractional >= 0) {
        text = format_fixed(value, fractional);
    } else if (significant >= 0) {
        text = format_float_significant(value, significant);
    } else {
        text = canonical_float_text(value);
    }
    return runtime_copy_string(format_integer_padding(std::move(text), integer_width, zero != 0));
}

// Preserve binary32 for canonical text; explicit digit formats retain the
// same exactly widened value used by the binary64 formatting functions.
extern "C" char* quidra_format_number32(float value, int integer_width, int fractional, int significant, int zero) {
    if (std::isnan(value)) runtime_nonfinite_text();
    if (std::isinf(value)) return runtime_copy_string(std::signbit(value) ? "-inf" : "inf");
    if (fractional >= 0 || significant >= 0)
        return quidra_format_number(static_cast<double>(value), integer_width, fractional, significant, zero);
    return runtime_copy_string(format_integer_padding(canonical_float_text(value), integer_width, zero != 0));
}

extern "C" void* quidra_managed_alloc(unsigned long long bytes) {
    if (bytes > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        runtime_allocation_failure();
    }
    return managed_allocate(static_cast<std::size_t>(bytes));
}

extern "C" bool quidra_runtime_text_valid_bytes(
    const char* data, unsigned long long size) {
    if (!data && size != 0) return false;
    if (size > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
        return false;
    return valid_runtime_text(
        std::string_view(data ? data : "", static_cast<std::size_t>(size)));
}

extern "C" void quidra_runtime_text_error(const char* message) {
    runtime_text_failure(message ? message : "invalid runtime text");
}

extern "C" char* quidra_runtime_copy_text_bytes(
    const char* data, unsigned long long size) {
    if (!data && size != 0) runtime_text_failure("null text data");
    if (size > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
        runtime_allocation_failure();
    return copy_runtime_text(
        std::string_view(data ? data : "", static_cast<std::size_t>(size)));
}

extern "C" char* quidra_runtime_copy_validated_text_bytes(
    const char* data, unsigned long long size) {
    if (!data && size != 0) runtime_text_failure("null text data");
    if (size > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
        runtime_allocation_failure();
    return copy_validated_runtime_text(
        std::string_view(data ? data : "", static_cast<std::size_t>(size)));
}

extern "C" char* quidra_runtime_try_copy_text_bytes(
    const char* data, unsigned long long size) {
    if (!data && size != 0) return nullptr;
    if (size > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
        return nullptr;
    const auto view =
        std::string_view(data ? data : "", static_cast<std::size_t>(size));
    std::size_t codepoints = 0;
    bool contains_nul = false;
    if (!valid_utf8(view, &codepoints, &contains_nul) || contains_nul)
        return nullptr;
    return copy_validated_runtime_text(view, codepoints);
}

extern "C" char* quidra_runtime_allocate_text_buffer(
    unsigned long long size) {
    if (size >
        static_cast<unsigned long long>(
            std::numeric_limits<std::size_t>::max() - 1)) {
        return nullptr;
    }
    const auto bytes = static_cast<std::size_t>(size);
    auto* result =
        static_cast<char*>(managed_allocate_string(bytes + 1));
    result[bytes] = '\0';
    return result;
}

extern "C" bool quidra_runtime_commit_text_buffer(
    char* data, unsigned long long size) {
    if (!data ||
        size > static_cast<unsigned long long>(
            std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    const auto bytes = static_cast<std::size_t>(size);
    const auto it =
        managed_state().allocations.find(reinterpret_cast<std::uintptr_t>(data));
    if (it == managed_state().allocations.end() ||
        it->second.size < bytes + 1) {
        return false;
    }
    std::size_t codepoints = 0;
    bool contains_nul = false;
    const auto view = std::string_view(data, bytes);
    if (!valid_utf8(view, &codepoints, &contains_nul) || contains_nul)
        return false;
    data[bytes] = '\0';
    mark_managed_string(data, bytes, codepoints);
    return true;
}

extern "C" unsigned long long quidra_runtime_text_byte_length(
    const char* text) {
    ManagedAllocation* allocation = nullptr;
    const auto source = cached_string_view(text, allocation);
    return static_cast<unsigned long long>(source.size());
}

extern "C" void quidra_managed_retain(void* value) {
    if (!value) return;
    ManagedAllocation* allocation = nullptr;
    const auto exact =
        managed_state().allocations.find(reinterpret_cast<std::uintptr_t>(value));
    if (exact != managed_state().allocations.end()) {
        allocation = &exact->second;
    } else {
        auto* containing = managed_containing(value);
        if (containing && containing->shared_string_slab) allocation = containing;
    }
    if (!allocation) return;
    if (allocation->owners == std::numeric_limits<std::size_t>::max()) {
        runtime_text_failure("managed owner count overflow");
    }
    ++allocation->owners;
}

extern "C" void quidra_managed_release(void* value, void* drop_function) {
    if (!value) return;
    ManagedAllocation* allocation = nullptr;
    const auto exact =
        managed_state().allocations.find(reinterpret_cast<std::uintptr_t>(value));
    if (exact != managed_state().allocations.end()) {
        allocation = &exact->second;
    } else {
        auto* containing = managed_containing(value);
        if (containing && containing->shared_string_slab) allocation = containing;
    }
    if (!allocation) return;

    if (drop_function && !allocation->drop && !allocation->shared_string_slab) {
        allocation->drop = reinterpret_cast<ManagedDrop>(drop_function);
    }
    if (allocation->owners == 0) runtime_text_failure("managed owner count underflow");
    --allocation->owners;
    if (allocation->owners == 0 && allocation->pins == 0) {
        const auto key = reinterpret_cast<std::uintptr_t>(allocation->base);
        const ManagedFinalization finalization{
            allocation->base, allocation->drop, allocation->small_pool_class};
        invalidate_managed_string_cache(allocation);
        invalidate_shared_string_cache(allocation);
        invalidate_array_append_cache(allocation);
        if (allocation->interior_range_tracked) {
            clear_managed_range_cache(allocation);
            managed_state().ranges.erase(key);
        }
        managed_state().allocations.erase(key);
        finalize_managed(finalization);
    }
}

extern "C" std::uint64_t quidra_reference_root_serial() {
    static std::atomic<std::uint64_t> serial{0};
    auto current=serial.load(std::memory_order_relaxed);
    for (;;) {
        if (current == std::numeric_limits<std::uint64_t>::max())
            quidra::runtime::report_failure(abi::FailureReason::reference_serial_exhausted,{},0,0);
        if (serial.compare_exchange_weak(current,current+1,std::memory_order_relaxed)) return current+1;
    }
}

extern "C" void quidra_managed_pin(void* address) {
    if (!address) return;
    ManagedAllocation* allocation = nullptr;
    const auto exact =
        managed_state().allocations.find(reinterpret_cast<std::uintptr_t>(address));
    if (exact != managed_state().allocations.end()) allocation = &exact->second;
    else allocation = managed_containing(address);
    if (!allocation) return;
    if (allocation->pins == std::numeric_limits<std::size_t>::max()) {
        runtime_text_failure("managed pin count overflow");
    }
    ++allocation->pins;
}

extern "C" void quidra_managed_unpin(void* address) {
    if (!address) return;
    ManagedAllocation* allocation = nullptr;
    const auto exact =
        managed_state().allocations.find(reinterpret_cast<std::uintptr_t>(address));
    if (exact != managed_state().allocations.end()) allocation = &exact->second;
    else allocation = managed_containing(address);
    if (!allocation) return;
    if (allocation->pins == 0) runtime_text_failure("managed pin count underflow");
    --allocation->pins;
    if (allocation->owners == 0 && allocation->pins == 0) {
        const auto key = reinterpret_cast<std::uintptr_t>(allocation->base);
        const ManagedFinalization finalization{
            allocation->base, allocation->drop, allocation->small_pool_class};
        invalidate_managed_string_cache(allocation);
        invalidate_array_append_cache(allocation);
        if (allocation->interior_range_tracked) {
            clear_managed_range_cache(allocation);
            managed_state().ranges.erase(key);
        }
        managed_state().allocations.erase(key);
        finalize_managed(finalization);
    }
}

// The runtime's internal entries for error records (runtime_internal.hpp).
namespace quidra::runtime {

void preserve_managed_state_for_exit() {
    preserve_current_managed_state();
}

void* managed_allocate_with_drop(std::size_t bytes, void (*drop)(void*)) {
    void* memory = managed_allocate_impl(bytes, false);
    managed_state().allocations.find(reinterpret_cast<std::uintptr_t>(memory))->second.drop = drop;
    return memory;
}

bool managed_release_take(void* value, TakenAllocation& taken) {
    if (!value) return false;
    const auto exact = managed_state().allocations.find(reinterpret_cast<std::uintptr_t>(value));
    if (exact == managed_state().allocations.end()) return false;
    auto* allocation = &exact->second;
    if (allocation->owners == 0) runtime_text_failure("managed owner count underflow");
    --allocation->owners;
    if (allocation->owners != 0 || allocation->pins != 0) return false;
    const auto key = reinterpret_cast<std::uintptr_t>(allocation->base);
    taken = TakenAllocation{allocation->base, allocation->small_pool_class};
    invalidate_managed_string_cache(allocation);
    invalidate_shared_string_cache(allocation);
    invalidate_array_append_cache(allocation);
    if (allocation->interior_range_tracked) {
        clear_managed_range_cache(allocation);
        managed_state().ranges.erase(key);
    }
    managed_state().allocations.erase(key);
    return true;
}

void recycle_taken(const TakenAllocation& taken) {
    recycle_managed_memory(taken.base, taken.small_pool_class);
}

bool is_managed(const void* value) {
    return value && managed_state().allocations.contains(reinterpret_cast<std::uintptr_t>(value));
}

std::size_t managed_owners(const void* value) {
    if (!value) return 0;
    const auto exact = managed_state().allocations.find(reinterpret_cast<std::uintptr_t>(value));
    return exact == managed_state().allocations.end() ? 0 : exact->second.owners;
}

char* managed_text_copy(std::string_view text) { return copy_runtime_text(text); }

} // namespace quidra::runtime

extern "C" void quidra_init_create(void* base, unsigned long long count,
                                     unsigned long long unit_bytes,
                                     unsigned long long data_offset,
                                     int fully_initialized) {
    if (!base || unit_bytes == 0 ||
        count > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()) ||
        unit_bytes > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()) ||
        data_offset > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        runtime_text_failure("invalid initialization tracker");
    }
    const auto it = managed_state().allocations.find(reinterpret_cast<std::uintptr_t>(base));
    if (it == managed_state().allocations.end()) runtime_text_failure("initialization tracker requires managed storage");
    auto tracker = std::make_unique<InitializationTracker>();
    tracker->count = static_cast<std::size_t>(count);
    tracker->unit_bytes = static_cast<std::size_t>(unit_bytes);
    tracker->data_offset = static_cast<std::size_t>(data_offset);
    tracker->data_base=reinterpret_cast<std::uintptr_t>(base)+tracker->data_offset;
    tracker->fully_initialized = fully_initialized != 0 || tracker->count == 0;
    tracker->initialized_count = tracker->fully_initialized ? tracker->count : 0;
    if (!tracker->fully_initialized) {
        tracker->bits.assign((tracker->count + 7) / 8, 0);
    }
    if (tracker->data_offset == abi::array_layout::payload_offset) {
        const auto physical_capacity =
            it->second.size >= tracker->data_offset
                ? (it->second.size - tracker->data_offset) / tracker->unit_bytes
                : 0;
        it->second.array_capacity = std::max(tracker->count, physical_capacity);
    }
    it->second.initialization = std::move(tracker);
}

extern "C" void quidra_init_create_unowned(void* base,unsigned long long count,
    unsigned long long unit_bytes,unsigned long long data_offset,int fully_initialized) {
    quidra::runtime::NodeRuntimeOwner partial(base);
    quidra_init_create(base,count,unit_bytes,data_offset,fully_initialized);
    partial.relinquish();
}

extern "C" bool quidra_array_initialization_complete(void* array) {
    if (!array) return false;
    const auto* allocation = managed_containing(array);
    return allocation && allocation->initialization &&
           allocation->initialization->fully_initialized;
}

extern "C" void quidra_init_mark_range(void* address, unsigned long long bytes) {
    if (!address || bytes == 0) return;
    // tracked_unit_for_address already resolves the containing allocation and
    // returns no unit for untracked storage. Calling tracker_is_complete first
    // repeated that range lookup on every initializing array write.
    const auto unit = tracked_unit_for_address(address);
    if (!unit) return;
    auto& tracker = *unit->first->initialization;
    if (tracker.fully_initialized) return;
    const auto count = static_cast<std::size_t>(
        (bytes + tracker.unit_bytes - 1) / tracker.unit_bytes);
    if (unit->second > tracker.count || count > tracker.count - unit->second) {
        runtime_text_failure("initialization mark exceeds tracked storage");
    }
    for (std::size_t i = 0; i < count; ++i) tracker_set(tracker, unit->second + i);
}

extern "C" void* quidra_parallel_array_prepare(void* array,bool read_only,bool bare_integer,
    unsigned long long line,unsigned long long column) {
    auto* allocation=managed_containing(array);
    if(!allocation||!allocation->initialization)
        quidra::runtime::report_failure(abi::FailureReason::parallel_for_unsupported,
            abi::FailureArgs{.message="parallel for needs stable, tracked array storage"},line,column);
    auto& tracker=*allocation->initialization;
    if(read_only && !tracker.fully_initialized)runtime_uninitialized_failure(line,column);
    if(!read_only && bare_integer) {
        // No foreign managed integer drop may run on a worker. The normal
        // owner thread checks all old cells before scheduling any iteration.
        for(std::size_t i=0;i<tracker.count;++i) {
            std::uint64_t word{};
            std::memcpy(&word,reinterpret_cast<const void*>(tracker.data_base+i*tracker.unit_bytes),sizeof word);
            if(word & abi::bare_integer_layout::boxed_bit)
                quidra::runtime::report_failure(abi::FailureReason::parallel_for_unsupported,
                    abi::FailureArgs{.message="parallel integer output has a boxed previous element"},line,column);
        }
    }
    // Reserve before any body write. Worker marking never allocates or
    // resolves a foreign thread's managed allocation registry.
    if(!tracker.fully_initialized && tracker.bits.empty())tracker.bits.assign((tracker.count+7)/8,0);
    return &tracker;
}
extern "C" bool quidra_parallel_array_inline(void* receipt) {
    const auto& tracker=*static_cast<const InitializationTracker*>(receipt);
    for(std::size_t i=0;i<tracker.count;++i) {
        std::uint64_t word{};
        std::memcpy(&word,reinterpret_cast<const void*>(tracker.data_base+i*tracker.unit_bytes),sizeof word);
        if(word & abi::bare_integer_layout::boxed_bit)return false;
    }
    return true;
}
extern "C" void quidra_parallel_array_mark(void* receipt,void* address,unsigned long long bytes) {
    auto& tracker=*static_cast<InitializationTracker*>(receipt);
    const auto raw=reinterpret_cast<std::uintptr_t>(address);
    if(raw<tracker.data_base || tracker.unit_bytes==0 ||
       (raw-tracker.data_base)%tracker.unit_bytes || bytes!=tracker.unit_bytes)
        quidra::runtime::report_failure(abi::FailureReason::parallel_for_unsupported,
            abi::FailureArgs{.message="parallel initialization receipt does not cover this element"},0,0);
    const auto index=(raw-tracker.data_base)/tracker.unit_bytes;
    if(index>=tracker.count)
        quidra::runtime::report_failure(abi::FailureReason::parallel_for_unsupported,
            abi::FailureArgs{.message="parallel initialization receipt is out of range"},0,0);
    std::lock_guard guard(tracker.parallel_mutex);
    if(tracker.fully_initialized)return;
    const auto bit=static_cast<unsigned char>(1U<<(index%8));
    auto& byte=tracker.bits[index/8];
    if(byte&bit)return;
    byte=static_cast<unsigned char>(byte|bit);
    if(++tracker.initialized_count==tracker.count)tracker.fully_initialized=true;
}

extern "C" void quidra_init_check(void* address, unsigned long long line,
                                   unsigned long long column) {
    if (!address) return;
    // The overwhelmingly common case is an element of a fully initialized array. Answer
    // it from the tracker alone, before tracked_unit_for_address divides to recover the
    // element index; that division ran on every checked element access.
    if (tracker_is_complete(address)) return;
    const auto unit = tracked_unit_for_address(address);
    if (!unit) return;
    const auto& tracker = *unit->first->initialization;
    if (!tracker_bit(tracker, unit->second)) runtime_uninitialized_failure(line, column);
}

extern "C" void quidra_init_require_range(void* address, unsigned long long bytes,
                                           unsigned long long line,
                                           unsigned long long column) {
    if (!address || bytes == 0) return;
    if (tracker_is_complete(address)) return;
    const auto unit = tracked_unit_for_address(address);
    if (!unit) return;
    const auto& tracker = *unit->first->initialization;
    if (tracker.fully_initialized) return;
    const auto count = static_cast<std::size_t>(
        (bytes + tracker.unit_bytes - 1) / tracker.unit_bytes);
    if (unit->second > tracker.count || count > tracker.count - unit->second) {
        runtime_text_failure("initialization check exceeds tracked storage");
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (!tracker_bit(tracker, unit->second + i)) {
            runtime_uninitialized_failure(line, column);
        }
    }
}

extern "C" void quidra_init_clone(void* destination, void* source_data,
                                   unsigned long long count,
                                   unsigned long long unit_bytes,
                                   unsigned long long destination_offset) {
    if (!destination || !source_data) return;
    const auto source_unit = tracked_unit_for_address(source_data);
    if (!source_unit) {
        quidra_init_create(destination, count, unit_bytes, destination_offset, 1);
        return;
    }
    quidra_init_create(destination, count, unit_bytes, destination_offset, 0);
    const auto destination_it =
        managed_state().allocations.find(reinterpret_cast<std::uintptr_t>(destination));
    auto& destination_tracker = *destination_it->second.initialization;
    const auto& source_tracker = *source_unit->first->initialization;
    const auto requested = static_cast<std::size_t>(count);
    if (source_unit->second > source_tracker.count ||
        requested > source_tracker.count - source_unit->second) {
        runtime_text_failure("initialization clone exceeds source storage");
    }
    if (source_tracker.fully_initialized) {
        destination_tracker.fully_initialized = true;
        destination_tracker.initialized_count = destination_tracker.count;
        destination_tracker.bits.clear();
        return;
    }
    for (std::size_t i = 0; i < requested; ++i) {
        if (tracker_bit(source_tracker, source_unit->second + i)) {
            tracker_set(destination_tracker, i);
        }
    }
}


extern "C" bool quidra_array_can_append_move(void* array) {
    auto* allocation = exact_array_append_allocation(array);
    if (!allocation || allocation->owners != 1 || allocation->pins != 0 ||
        !allocation->initialization) {
        return false;
    }
    const auto& tracker = *allocation->initialization;
    return tracker.data_offset == abi::array_layout::payload_offset && tracker.unit_bytes != 0 &&
           tracker.fully_initialized &&
           tracker.count <= allocation->array_capacity;
}

extern "C" void* quidra_array_grow_move(void* array, unsigned long long raw_stride) {
    if (!array || raw_stride == 0 ||
        raw_stride > static_cast<unsigned long long>(
            std::numeric_limits<std::size_t>::max())) {
        runtime_text_failure("array append move requires unique initialized storage");
    }

    // can_append_move and grow_move are adjacent on the generated fast path.
    // Reuse their exact-allocation lookup, but independently re-check every
    // safety property because evaluating the appended value may have changed
    // ownership or pin state in between.
    auto* allocation = exact_array_append_allocation(array);
    if (!allocation || allocation->owners != 1 || allocation->pins != 0 ||
        !allocation->initialization) {
        runtime_text_failure("array append move requires unique initialized storage");
    }
    auto* tracker = allocation->initialization.get();
    if (tracker->data_offset != abi::array_layout::payload_offset || tracker->unit_bytes == 0 ||
        !tracker->fully_initialized ||
        tracker->count > allocation->array_capacity) {
        runtime_text_failure("array append move requires unique initialized storage");
    }
    const auto stride = static_cast<std::size_t>(raw_stride);
    if (stride != tracker->unit_bytes) runtime_text_failure("array append stride mismatch");

    long long signed_length = 0;
    std::memcpy(&signed_length, array, sizeof(signed_length));
    if (signed_length < 0 ||
        static_cast<unsigned long long>(signed_length) != tracker->count ||
        signed_length == std::numeric_limits<long long>::max()) {
        runtime_text_failure("invalid array length during append");
    }

    const auto old_count = static_cast<std::size_t>(signed_length);
    const auto new_count = old_count + 1;
    void* result = array;

    if (allocation->array_capacity < new_count) {
        std::size_t new_capacity = allocation->array_capacity < 4
            ? 4
            : allocation->array_capacity;
        while (new_capacity < new_count) {
            if (new_capacity > std::numeric_limits<std::size_t>::max() / 2) {
                new_capacity = new_count;
                break;
            }
            new_capacity *= 2;
        }
        if (new_capacity >
            (std::numeric_limits<std::size_t>::max() - abi::array_layout::payload_offset) / stride) {
            runtime_allocation_failure();
        }

        const auto old_key = reinterpret_cast<std::uintptr_t>(array);
        const auto new_bytes = static_cast<std::size_t>(abi::array_layout::payload_offset) + new_capacity * stride;
        const auto old_bytes = allocation->size;
        clear_managed_range_cache(allocation);
        std::map<std::uintptr_t,std::size_t>::node_type node_range;
        if(quidra::runtime::node_failure_active()) {
            // Reserve an existing tree node before realloc changes the sole
            // owner pointer; reinserting its node handle cannot allocate.
            if(!managed_state().ranges.contains(old_key))managed_state().ranges.emplace(old_key,old_bytes);
            node_range=managed_state().ranges.extract(old_key);
        }
        result = std::realloc(array, new_bytes);
        if (!result) {
            if(node_range)managed_state().ranges.insert(std::move(node_range));
            runtime_allocation_failure();
        }
        if (new_bytes > old_bytes) {
            std::memset(static_cast<unsigned char*>(result) + old_bytes, 0,
                        new_bytes - old_bytes);
        }

        const auto new_key = reinterpret_cast<std::uintptr_t>(result);
        if(!quidra::runtime::node_failure_active())managed_state().ranges.erase(old_key);
        if (new_key != old_key) {
            auto node = managed_state().allocations.extract(old_key);
            if (node.empty())
                runtime_text_failure("array append storage disappeared");
            node.key() = new_key;
            node.mapped().base = result;
            node.mapped().size = new_bytes;
            node.mapped().small_pool_class = 0;
            node.mapped().array_capacity = new_capacity;
            const auto inserted = managed_state().allocations.insert(std::move(node));
            allocation = &inserted.position->second;
        } else {
            allocation->base = result;
            allocation->size = new_bytes;
            allocation->small_pool_class = 0;
            allocation->array_capacity = new_capacity;
        }
        if(node_range) {
            node_range.key()=new_key;node_range.mapped()=new_bytes;
            managed_state().ranges.insert(std::move(node_range));
        }else managed_state().ranges.emplace(new_key,new_bytes);
        managed_state().array_append_base = result;
        managed_state().array_append_allocation = allocation;
        tracker = allocation->initialization.get();
    }

    // The newly exposed append slot was outside the old logical length.
    // Small managed allocations can have spare pooled capacity whose bytes retain
    // data from a previous allocation. ArraySet releases a managed value already
    // present in a slot before replacing it, so make the fresh logical slot
    // explicitly empty before publishing the longer array. Without this, a stale
    // pointer in spare capacity can be released as if it were an existing element.
    std::memset(
        static_cast<unsigned char*>(result) + abi::array_layout::payload_offset + old_count * stride,
        0, stride);

    tracker->count = new_count;
    tracker->initialized_count = new_count;
    tracker->fully_initialized = true;
    tracker->bits.clear();

    const auto new_length = static_cast<long long>(new_count);
    std::memcpy(result, &new_length, sizeof(new_length));
    return result;
}


namespace {

// The order every sort of quidra_array_sorted applies when its elements
// define one with '<'.
struct NaturalOrder {};

// std::stable_sort of [begin, end) in ascending order, or in descending
// order, the reverse of the ascending order: equal elements keep their
// relative order in both directions, so a descending sort is not a reversed
// ascending one.
template <bool Descending, class Iterator, class Less = NaturalOrder>
void stable_sort_in_order(Iterator begin, Iterator end, Less less = {}) {
    if constexpr (std::is_same_v<Less, NaturalOrder>) {
        if constexpr (Descending)
            std::stable_sort(begin, end,
                             [](const auto& left, const auto& right) { return right < left; });
        else
            std::stable_sort(begin, end);
    } else {
        if constexpr (Descending)
            std::stable_sort(begin, end, [&less](const auto& left, const auto& right) {
                return less(right, left);
            });
        else
            std::stable_sort(begin, end, less);
    }
}

// The sorted copy of a dynamic array (quidra_array_sorted,
// quidra_array_sorted_desc): the receiver is never changed.
template <bool Descending>
void* array_sorted_copy(void* raw, int kind, unsigned long long raw_stride,
                        unsigned long long line, unsigned long long column) {
    if (!raw || raw_stride == 0 ||
        raw_stride > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        runtime_text_failure("invalid sorted array input");
    }

    long long signed_count = 0;
    std::memcpy(&signed_count, raw, sizeof(signed_count));
    if (signed_count < 0) runtime_text_failure("invalid array length during sort");
    const auto count = static_cast<std::size_t>(signed_count);
    const auto stride = static_cast<std::size_t>(raw_stride);
    if (count > (std::numeric_limits<std::size_t>::max() - abi::array_layout::payload_offset) / stride) {
        runtime_allocation_failure();
    }

    const auto expected_stride = [&]() -> std::size_t {
        switch (static_cast<abi::SortElementKind>(kind)) {
            case abi::SortElementKind::int64: return sizeof(std::int64_t);
            case abi::SortElementKind::int8: return sizeof(std::int8_t);
            case abi::SortElementKind::int16: return sizeof(std::int16_t);
            case abi::SortElementKind::int32: return sizeof(std::int32_t);
            case abi::SortElementKind::uint8: return sizeof(std::uint8_t);
            case abi::SortElementKind::uint16: return sizeof(std::uint16_t);
            case abi::SortElementKind::uint32: return sizeof(std::uint32_t);
            case abi::SortElementKind::uint64: return sizeof(std::uint64_t);
            case abi::SortElementKind::float64: return sizeof(double);
            case abi::SortElementKind::float32: return sizeof(float);
            case abi::SortElementKind::boolean: return 1;
            case abi::SortElementKind::string: return sizeof(char*);
            case abi::SortElementKind::bare_integer: return sizeof(long long);
            case abi::SortElementKind::real16:
            case abi::SortElementKind::real16b: return sizeof(std::uint16_t);
            default: runtime_text_failure("unsupported sorted array element type");
        }
    }();
    if (stride != expected_stride) runtime_text_failure("sorted array stride mismatch");

    if (count != 0) {
        quidra_init_require_range(
            static_cast<unsigned char*>(raw) + abi::array_layout::payload_offset,
            static_cast<unsigned long long>(count * stride), line, column);
    }

    auto* result = static_cast<unsigned char*>(managed_allocate(abi::array_layout::payload_offset + count * stride));
    struct PartialSortCopy {
        unsigned char* result;
        int kind;
        std::size_t retained{};
    } partial{result, kind};
    quidra::runtime::NodeRuntimeOwner partial_owner(&partial, +[](void* raw_state) {
        auto& state = *static_cast<PartialSortCopy*>(raw_state);
        auto* payload = state.result + abi::array_layout::payload_offset;
        while (state.retained != 0) {
            const auto index = --state.retained;
            if (state.kind == static_cast<int>(abi::SortElementKind::string)) {
                auto** slot = reinterpret_cast<char**>(payload) + index;
                auto* value = std::exchange(*slot, nullptr);
                if (value) quidra::runtime::cleanup_node_slot(
                    +[](void* value) { quidra_managed_release(value, nullptr); }, value);
            } else if (state.kind == static_cast<int>(abi::SortElementKind::bare_integer)) {
                auto* slot = reinterpret_cast<long long*>(payload) + index;
                const auto word = std::exchange(*slot, 0);
                if ((word & abi::bare_integer_layout::boxed_bit) != 0) {
                    auto* value = reinterpret_cast<void*>(static_cast<std::uintptr_t>(word) &
                        ~static_cast<std::uintptr_t>(abi::bare_integer_layout::boxed_bit));
                    quidra::runtime::cleanup_node_slot(
                        +[](void* value) { quidra_managed_release(value, nullptr); }, value);
                }
            }
        }
        quidra_managed_release(state.result, nullptr);
    });
    std::memcpy(result, &signed_count, sizeof(signed_count));
    if (count != 0) {
        std::memcpy(result + abi::array_layout::payload_offset, static_cast<unsigned char*>(raw) + abi::array_layout::payload_offset, count * stride);
    }
    quidra_init_create(result, static_cast<unsigned long long>(count),
                       static_cast<unsigned long long>(stride), abi::array_layout::payload_offset, 1);

    auto* data = result + abi::array_layout::payload_offset;
    // Real arrays hold no NaN (no observable real value is one); -0.0 sorts
    // before 0.0.
    const auto float_less = [](auto left, auto right) {
        if (std::isnan(left) || std::isnan(right))
            runtime_text_failure("internal error: a NaN reached sorting");
        if (left == right) {
            if (left == 0) return std::signbit(left) && !std::signbit(right);
            return false;
        }
        return left < right;
    };

    switch (static_cast<abi::SortElementKind>(kind)) {
        case abi::SortElementKind::int64:
            stable_sort_in_order<Descending>(reinterpret_cast<std::int64_t*>(data),
                             reinterpret_cast<std::int64_t*>(data) + count);
            break;
        case abi::SortElementKind::int8:
            stable_sort_in_order<Descending>(reinterpret_cast<std::int8_t*>(data),
                             reinterpret_cast<std::int8_t*>(data) + count);
            break;
        case abi::SortElementKind::int16:
            stable_sort_in_order<Descending>(reinterpret_cast<std::int16_t*>(data),
                             reinterpret_cast<std::int16_t*>(data) + count);
            break;
        case abi::SortElementKind::int32:
            stable_sort_in_order<Descending>(reinterpret_cast<std::int32_t*>(data),
                             reinterpret_cast<std::int32_t*>(data) + count);
            break;
        case abi::SortElementKind::uint8:
            stable_sort_in_order<Descending>(reinterpret_cast<std::uint8_t*>(data),
                             reinterpret_cast<std::uint8_t*>(data) + count);
            break;
        case abi::SortElementKind::uint16:
            stable_sort_in_order<Descending>(reinterpret_cast<std::uint16_t*>(data),
                             reinterpret_cast<std::uint16_t*>(data) + count);
            break;
        case abi::SortElementKind::uint32:
            stable_sort_in_order<Descending>(reinterpret_cast<std::uint32_t*>(data),
                             reinterpret_cast<std::uint32_t*>(data) + count);
            break;
        case abi::SortElementKind::uint64:
            stable_sort_in_order<Descending>(reinterpret_cast<std::uint64_t*>(data),
                             reinterpret_cast<std::uint64_t*>(data) + count);
            break;
        case abi::SortElementKind::float64:
            stable_sort_in_order<Descending>(reinterpret_cast<double*>(data),
                             reinterpret_cast<double*>(data) + count, float_less);
            break;
        case abi::SortElementKind::float32:
            stable_sort_in_order<Descending>(reinterpret_cast<float*>(data),
                             reinterpret_cast<float*>(data) + count, float_less);
            break;
        case abi::SortElementKind::real16:
        case abi::SortElementKind::real16b: {
            const auto& format = static_cast<abi::SortElementKind>(kind) == abi::SortElementKind::real16
                ? quidra::abi::half::binary16 : quidra::abi::half::bfloat16;
            stable_sort_in_order<Descending>(reinterpret_cast<std::uint16_t*>(data),
                             reinterpret_cast<std::uint16_t*>(data) + count,
                             [&](std::uint16_t left, std::uint16_t right) {
                                 return float_less(quidra::abi::half::value_of(left, format),
                                                   quidra::abi::half::value_of(right, format));
                             });
            break;
        }
        case abi::SortElementKind::boolean:
            stable_sort_in_order<Descending>(reinterpret_cast<std::uint8_t*>(data),
                             reinterpret_cast<std::uint8_t*>(data) + count);
            break;
        case abi::SortElementKind::string: {
            auto** begin = reinterpret_cast<char**>(data);
            struct SortableString {
                char* value{};
                std::string_view view;
            };
            std::vector<SortableString> strings;
            strings.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                if (!begin[i]) runtime_text_failure("null string in sorted array");
                ManagedAllocation* allocation = nullptr;
                const auto view = validated_string_view(begin[i], allocation);
                quidra_managed_retain(begin[i]);
                if (quidra::runtime::node_failure_active()) ++partial.retained;
                strings.push_back(SortableString{begin[i], view});
            }
            stable_sort_in_order<Descending>(strings.begin(), strings.end(),
                             [](const SortableString& left,
                                const SortableString& right) {
                                 return left.view < right.view;
                             });
            for (std::size_t i = 0; i < count; ++i) begin[i] = strings[i].value;
            break;
        }
        case abi::SortElementKind::bare_integer: {
            // Inline words order like their values. Boxed words are shared
            // by the copy and compared through the runtime.
            auto* words = reinterpret_cast<long long*>(data);
            bool boxed = false;
            for (std::size_t i = 0; i < count; ++i)
                boxed = boxed || (words[i] & abi::bare_integer_layout::boxed_bit) != 0;
            if (!boxed) {
                stable_sort_in_order<Descending>(words, words + count);
                break;
            }
            for (std::size_t i = 0; i < count; ++i) {
                if (quidra::runtime::node_failure_active()) ++partial.retained;
                if ((words[i] & abi::bare_integer_layout::boxed_bit) == 0) continue;
                quidra_managed_retain(reinterpret_cast<void*>(
                    static_cast<std::uintptr_t>(words[i]) &
                    ~static_cast<std::uintptr_t>(abi::bare_integer_layout::boxed_bit)));
            }
            stable_sort_in_order<Descending>(words, words + count, [](long long left, long long right) {
                return quidra_int_compare(left, right) < 0;
            });
            break;
        }
        default:
            runtime_text_failure("unsupported sorted array element type");
    }
    partial_owner.relinquish();
    return result;
}

} // namespace

extern "C" void* quidra_array_sorted(void* raw, int kind,
                                      unsigned long long raw_stride,
                                      unsigned long long line,
                                      unsigned long long column) {
    return array_sorted_copy<false>(raw, kind, raw_stride, line, column);
}

extern "C" void* quidra_array_sorted_desc(void* raw, int kind,
                                           unsigned long long raw_stride,
                                           unsigned long long line,
                                           unsigned long long column) {
    return array_sorted_copy<true>(raw, kind, raw_stride, line, column);
}


namespace {

struct AutogradNode;
struct AutogradSlot;
struct AutogradIdentity;
std::shared_ptr<AutogradSlot> clone_autograd_slot(
    const std::shared_ptr<AutogradSlot>& source);

enum class AutogradOp {
    Leaf, Add, Sub, Mul, Div, ScalarBinary,
    Reshape, Transpose, Gather, GatherBackward,
    CustomNative, Neg,
    // A device operand read with stride-0 broadcast axes. The node holds the
    // broadcast view itself (no materialized copy); its gradient sums over
    // the broadcast axes like the Gather it replaces.
    Broadcast,
    Cast
};

struct TensorStorage {
    // A fresh runtime intermediate is not yet held by a generated SSA owner.
    void* node_partial_frame{};
    std::size_t owners{1};
    int dtype{};
    std::size_t count{};
    int device{-1}; // -1 is an internal CPU representation; public gpu indices are >= 0.
    quidra::unified::HostBytes data;
    quidra::device::Buffer* gpu_buffer{};
    InitializationTracker initialization;
    // Non-null when `data` (CPU) or `gpu_buffer` (GPU) views a unified-memory
    // block shared with other storages (runtime_unified.inc); the block then
    // owns the memory.
    quidra::unified::Block* unified{};
    // Package code has received a native device handle of this storage, so
    // its command buffers may still use the buffer, even ones it commits in a
    // later call. `.cpu()` then never views the buffer (runtime_unified.inc).
    bool lent_to_package{};
};

struct TensorValue {
    TensorStorage* storage{};
    std::vector<long long> shape;
    std::vector<long long> strides;
    std::size_t offset{};
    std::shared_ptr<AutogradNode> graph;
    std::shared_ptr<AutogradSlot> grad_slot;
};

[[noreturn]] void tensor_fail(const char* message, unsigned long long line,
                              unsigned long long column) {
    end_package_hold_before_failure(line, column);
    quidra::runtime::report_failure(abi::FailureReason::tensor_failure,
                                    abi::FailureArgs{.message = message}, line, column);
}

// An indexing failure keeps its slice/index origin and any axis details.
[[noreturn]] void tensor_index_fail(abi::FailureReason reason, const abi::FailureArgs& args,
                                    unsigned long long line, unsigned long long column) {
    end_package_hold_before_failure(line, column);
    quidra::runtime::report_failure(reason, args, line, column);
}

[[noreturn]] void tensor_slice_step_fail(long long step, quidra::SliceStart start,
                                         quidra::SliceEnd end,
                                         unsigned long long line,
                                         unsigned long long column) {
    if (step == 0) {
        tensor_index_fail(abi::FailureReason::slice_step_zero, {}, line, column);
    }
    const auto start_direction = quidra::slice_marker_direction(start);
    const auto direction = start_direction != 0
        ? start_direction : quidra::slice_marker_direction(end);
    if (direction != 0 && (step < 0 ? -1 : 1) != direction) {
        const std::string_view marker = start_direction != 0
            ? quidra::slice_marker_text(start) : quidra::slice_marker_text(end);
        tensor_index_fail(abi::FailureReason::slice_step_direction,
                          abi::FailureArgs{.step = step, .marker = marker}, line, column);
    }
    tensor_index_fail(abi::FailureReason::tensor_slice_step_negative, {}, line, column);
}

std::size_t tensor_dtype_bytes(int dtype) {
    switch (dtype) {
        case QCORE_DTYPE_INT64: case QCORE_DTYPE_UINT64: case QCORE_DTYPE_FLOAT64: return 8;
        case QCORE_DTYPE_INT8: case QCORE_DTYPE_UINT8: case QCORE_DTYPE_BOOL: return 1;
        case QCORE_DTYPE_INT16: case QCORE_DTYPE_UINT16: return 2;
        case QCORE_DTYPE_FLOAT16: case QCORE_DTYPE_BFLOAT16: return 2;
        case QCORE_DTYPE_INT32: case QCORE_DTYPE_UINT32: case QCORE_DTYPE_FLOAT32: return 4;
        case QCORE_DTYPE_COMPLEX_FLOAT16: case QCORE_DTYPE_COMPLEX_BFLOAT16: return 4;
        case QCORE_DTYPE_COMPLEX_FLOAT32: return 8;
        case QCORE_DTYPE_COMPLEX_FLOAT64: return 16;
        default: runtime_text_failure("invalid tensor dtype");
    }
}

std::vector<long long> tensor_int_array_from_array(
    void* raw, unsigned long long line, unsigned long long column) {
    if (!raw) tensor_fail("integer array is null", line, column);
    long long count = 0;
    std::memcpy(&count, raw, sizeof(count));
    if (count < 0) tensor_fail("integer array length cannot be negative", line, column);
    const auto size = static_cast<unsigned long long>(count);
    if (size > std::numeric_limits<unsigned long long>::max() / sizeof(long long))
        tensor_fail("integer array is too large", line, column);
    auto* data = static_cast<unsigned char*>(raw) + abi::array_layout::payload_offset;
    quidra_init_require_range(data, size * sizeof(long long), line, column);
    std::vector<long long> values(static_cast<std::size_t>(count));
    for (long long i = 0; i < count; ++i) {
        std::memcpy(&values[static_cast<std::size_t>(i)],
                    data + static_cast<std::size_t>(i) * sizeof(long long),
                    sizeof(long long));
    }
    return values;
}

std::vector<long long> tensor_shape_from_array(
    void* raw, unsigned long long line, unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if (!raw) tensor_fail("shape array is null", line, column);
    auto shape = tensor_int_array_from_array(raw, line, column);
    if (shape.size() > 64)
        tensor_fail("tensor rank must be between 0 and 64", line, column);
    for (const auto dimension : shape) {
        if (dimension < 0)
            tensor_fail("tensor dimensions cannot be negative", line, column);
    }
    return shape;
}

std::size_t tensor_element_count(const std::vector<long long>& shape,
                                 unsigned long long line,
                                 unsigned long long column) {
    std::size_t count = 1;
    for (const auto dimension : shape) {
        const auto current = static_cast<std::size_t>(dimension);
        if (current != 0 && count > std::numeric_limits<std::size_t>::max() / current) {
            tensor_fail("tensor element count overflow", line, column);
        }
        count *= current;
    }
    return count;
}

std::vector<long long> tensor_contiguous_strides(const std::vector<long long>& shape) {
    std::vector<long long> strides(shape.size(), 1);
    long long stride = 1;
    for (std::size_t i = shape.size(); i-- > 0;) {
        strides[i] = stride;
        if (shape[i] != 0 &&
            stride > std::numeric_limits<long long>::max() / shape[i]) {
            runtime_text_failure("tensor stride overflow");
        }
        stride *= shape[i];
    }
    return strides;
}

bool tensor_is_contiguous_value(const TensorValue& tensor) {
    return tensor.strides == tensor_contiguous_strides(tensor.shape);
}

std::size_t tensor_logical_count(const TensorValue& tensor) {
    return tensor_element_count(tensor.shape, 0, 0);
}

std::size_t tensor_storage_index(const TensorValue& tensor, std::size_t logical) {
    return quidra::tensor_view::storage_index(
        tensor.offset, tensor.shape, tensor.strides, logical);
}

// O(rank) proof that every tensor_storage_index() of the view lies inside its
// storage. False means "not proven", never "invalid": callers keep their exact
// per-element checks for those views.
bool tensor_view_within_storage(const TensorValue& tensor) {
    return tensor.storage &&
           quidra::tensor_view::within_storage(
               tensor.offset, tensor.shape, tensor.strides, tensor.storage->count);
}

// True when the view's `count` logical elements are the storage elements
// [offset, offset + count) in order, all inside storage, so the whole view can
// move with one bulk copy. False keeps the caller's per-element path.
bool tensor_view_dense_in_storage(const TensorValue& tensor, std::size_t count) {
    return tensor.storage &&
           quidra::tensor_view::dense_run(tensor.shape, tensor.strides) &&
           tensor.offset <= tensor.storage->count &&
           count <= tensor.storage->count - tensor.offset;
}

// Copies the `count` logical elements of a CPU view into the fresh contiguous
// `output` with one memcpy when tensor_view_dense_in_storage() holds. The
// source storage indices are then offset + i, so initialization propagates
// without address arithmetic, in one step for fully initialized storage.
// Returns false, copying nothing, for every other view; the caller keeps its
// per-element path for those.
bool tensor_copy_dense_view(const TensorValue& source, TensorStorage& output,
                            std::size_t count) {
    if (!tensor_view_dense_in_storage(source, count)) return false;
    const auto width = tensor_dtype_bytes(source.storage->dtype);
    if (count != 0) {
        std::memcpy(output.data.data(),
                    source.storage->data.data() + source.offset * width,
                    count * width);
    }
    if (source.storage->initialization.fully_initialized) {
        tracker_mark_complete(output.initialization);
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            if (tracker_bit(source.storage->initialization, source.offset + i)) {
                tracker_set(output.initialization, i);
            }
        }
    }
    return true;
}

void tensor_storage_release(TensorStorage* storage);
void forget_tensor_storage_partial(TensorStorage* storage) noexcept {
    if (!storage || !storage->node_partial_frame) return;
    auto* frame = std::exchange(storage->node_partial_frame, nullptr);
    *static_cast<TensorStorage**>(frame) = nullptr;
    quidra::runtime::leave_node_frame(frame);
}
void* make_tensor_storage_partial_frame() {
    return quidra::runtime::create_node_frame(sizeof(TensorStorage*), alignof(TensorStorage*),
        +[](void* raw) {
            auto* storage = std::exchange(*static_cast<TensorStorage**>(raw), nullptr);
            if (!storage) return;
            // finish_node_frames owns this frame allocation until the callback returns.
            storage->node_partial_frame = nullptr;
            tensor_storage_release(storage);
        });
}

TensorValue* tensor_descriptor(TensorStorage* storage, std::vector<long long> shape,
                               std::vector<long long> strides, std::size_t offset) {
    auto* memory = managed_allocate(sizeof(TensorValue));
    auto* result = new (memory) TensorValue{
        storage, std::move(shape), std::move(strides), offset, {}, {}};
    managed_state().allocations.at(reinterpret_cast<std::uintptr_t>(memory)).tensor_descriptor = true;
    forget_tensor_storage_partial(storage);
    return result;
}

void tensor_storage_release(TensorStorage* storage) {
    if (!storage) return;
    forget_tensor_storage_partial(storage);
    if (storage->owners == 0) runtime_text_failure("tensor storage owner underflow");
    --storage->owners;
    if (storage->owners == 0) {
        if (storage->unified) {
            quidra::unified::detach(storage->unified, storage->device >= 0);
            storage->unified = nullptr;
        } else {
            quidra::device::release(storage->gpu_buffer);
        }
        storage->gpu_buffer = nullptr;
        delete storage;
    }
}

bool tensor_on_cpu(const TensorStorage& storage) {
    return storage.device < 0;
}

void tensor_require_untracked_transform(
    const TensorValue& tensor,const char* operation,
    unsigned long long line,unsigned long long column) {
    if(!tensor.graph) return;
    const auto message=std::string(operation)+
        " on a tracked tensor requires explicit untrack() first";
    tensor_fail(message.c_str(),line,column);
}

[[noreturn]] void tensor_gpu_unsupported(
    const char* operation, const TensorStorage& storage,
    unsigned long long line, unsigned long long column) {
    const auto message = std::string(operation) +
        " is not supported on gpu(" + std::to_string(storage.device) + ")";
    tensor_fail(message.c_str(), line, column);
}

void tensor_require_cpu(
    const TensorStorage& storage, const char* operation,
    unsigned long long line, unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeDeviceChecks);
    if (!tensor_on_cpu(storage)) {
        tensor_gpu_unsupported(operation, storage, line, column);
    }
}

// Fill mode of a Core GPU kernel output of `dtype` (see below).
// Floating-point kernels store every element. Integer and bool kernels are
// checked: an element that overflows (or divides by zero) is skipped and only
// flags the deferred check, so their outputs keep the zero fill and a failing
// element reads 0, as it did before outputs skipped the fill.
int gpu_kernel_output_fill(int dtype) {
    return dtype == QCORE_DTYPE_FLOAT64 || dtype == QCORE_DTYPE_FLOAT32
               ? abi::tensor_fill_mode::write_only
               : abi::tensor_fill_mode::zeros;
}

// fill_mode: abi::tensor_fill_mode (uninitialized storage is tracked per
// element; write_only storage needs no fill on a device).
TensorStorage* tensor_storage_create(
    int dtype, std::size_t count, int fill_mode, int device_index = -1,
    unsigned long long line = 0, unsigned long long column = 0) {
    const auto width = tensor_dtype_bytes(dtype);
    if (count != 0 && width > std::numeric_limits<std::size_t>::max() / count) {
        runtime_allocation_failure();
    }
    quidra::counters::add(quidra::counters::Id::RuntimeDeviceChecks);
    if (device_index < -1) {
        tensor_fail("invalid internal tensor device", line, column);
    }
    auto* partial_frame = make_tensor_storage_partial_frame();
    auto* storage = new (std::nothrow) TensorStorage;
    quidra::runtime::NodeRuntimeOwner partial_storage(storage,[](void* raw){tensor_storage_release(static_cast<TensorStorage*>(raw));});
    if (!storage) runtime_allocation_failure();
    storage->node_partial_frame = partial_frame;
    if (partial_frame) *static_cast<TensorStorage**>(partial_frame) = storage;
    storage->dtype = dtype;
    storage->count = count;
    storage->device = device_index;
    const auto bytes = count * width;
    try {
        storage->initialization.count = count;
        storage->initialization.unit_bytes = width;
        storage->initialization.fully_initialized = fill_mode != abi::tensor_fill_mode::uninitialized || count == 0;
        storage->initialization.initialized_count =
            storage->initialization.fully_initialized ? count : 0;
        if (!storage->initialization.fully_initialized) {
            storage->initialization.bits.assign((count + 7) / 8, 0);
        }

        if (device_index < 0) {
            const auto allocation_begin = quidra::profile::enabled()
                ? quidra::platform::monotonic_ns() : 0;
            storage->data.resize(bytes);
            if (bytes != 0) {
                quidra::counters::add(quidra::counters::Id::HostTensorAllocations);
                quidra::counters::add(quidra::counters::Id::HostTensorAllocationBytes, bytes);
                if (quidra::profile::enabled()) {
                    quidra::profile::ProfileEvent event;
                    event.kind = quidra::profile::ProfileEvent::Kind::Alloc;
                    event.owner = "core"; event.name = "tensor storage";
                    event.host_begin_ns = allocation_begin;
                    event.host_end_ns = quidra::platform::monotonic_ns();
                    event.bytes = bytes;
                    quidra::profile::record(event);
                }
            }
        }
    } catch (...) {
        partial_storage.relinquish();forget_tensor_storage_partial(storage);delete storage;
        runtime_allocation_failure();
    }

    auto fill_ones = [&](quidra::unified::HostBytes& target) {
        target.resize(bytes);
        for (std::size_t i = 0; i < count; ++i) {
            auto* slot = target.data() + i * width;
            switch (dtype) {
                case QCORE_DTYPE_INT64: { std::int64_t v=1; std::memcpy(slot,&v,8); break; }
                case QCORE_DTYPE_INT8: { std::int8_t v=1; std::memcpy(slot,&v,1); break; }
                case QCORE_DTYPE_INT16: { std::int16_t v=1; std::memcpy(slot,&v,2); break; }
                case QCORE_DTYPE_INT32: { std::int32_t v=1; std::memcpy(slot,&v,4); break; }
                case QCORE_DTYPE_UINT8: { std::uint8_t v=1; std::memcpy(slot,&v,1); break; }
                case QCORE_DTYPE_UINT16: { std::uint16_t v=1; std::memcpy(slot,&v,2); break; }
                case QCORE_DTYPE_UINT32: { std::uint32_t v=1; std::memcpy(slot,&v,4); break; }
                case QCORE_DTYPE_UINT64: { std::uint64_t v=1; std::memcpy(slot,&v,8); break; }
                case QCORE_DTYPE_FLOAT64: { double v=1.0; std::memcpy(slot,&v,8); break; }
                case QCORE_DTYPE_FLOAT32: { float v=1.0F; std::memcpy(slot,&v,4); break; }
                case QCORE_DTYPE_FLOAT16: { std::uint16_t v=0x3c00; std::memcpy(slot,&v,2); break; }
                case QCORE_DTYPE_BFLOAT16: { std::uint16_t v=0x3f80; std::memcpy(slot,&v,2); break; }
                // 1 + 0i: the real part's one, then zero bits.
                case QCORE_DTYPE_COMPLEX_FLOAT16: { std::uint16_t v[2]={0x3c00,0}; std::memcpy(slot,v,4); break; }
                case QCORE_DTYPE_COMPLEX_BFLOAT16: { std::uint16_t v[2]={0x3f80,0}; std::memcpy(slot,v,4); break; }
                case QCORE_DTYPE_COMPLEX_FLOAT32: { float v[2]={1.0F,0.0F}; std::memcpy(slot,v,8); break; }
                case QCORE_DTYPE_COMPLEX_FLOAT64: { double v[2]={1.0,0.0}; std::memcpy(slot,v,16); break; }
                case QCORE_DTYPE_BOOL: { std::uint8_t v=1; std::memcpy(slot,&v,1); break; }
                default: tensor_fail("invalid tensor dtype", line, column);
            }
        }
    };

    if (device_index < 0) {
        if (fill_mode == abi::tensor_fill_mode::zeros) {
            std::fill(storage->data.begin(), storage->data.end(), std::uint8_t{0});
        } else if (fill_mode == abi::tensor_fill_mode::ones) {
            try {
                fill_ones(storage->data);
            } catch (...) {
                partial_storage.relinquish();forget_tensor_storage_partial(storage);delete storage;
                runtime_allocation_failure();
            }
        }
        partial_storage.relinquish();return storage;
    }

    std::string backend_error;
    storage->gpu_buffer = quidra::device::allocate(device_index, bytes, backend_error);
    if (!storage->gpu_buffer) {
        partial_storage.relinquish();forget_tensor_storage_partial(storage);delete storage;
        tensor_fail(backend_error.c_str(), line, column);
    }
    // write_only: the producing kernel writes every element, so a zero fill
    // would be overwritten unobserved (and failures release the storage).
    if (fill_mode == abi::tensor_fill_mode::zeros ||
        (fill_mode == abi::tensor_fill_mode::write_only &&
         !quidra::device::written_outputs_skip_fill(device_index))) {
        if (!quidra::device::zero(storage->gpu_buffer, 0, bytes, backend_error)) {
            quidra::device::release(storage->gpu_buffer);
            storage->gpu_buffer = nullptr;
            partial_storage.relinquish();forget_tensor_storage_partial(storage);delete storage;
            tensor_fail(backend_error.c_str(), line, column);
        }
    } else if (fill_mode == abi::tensor_fill_mode::ones) {
        if (!quidra::device::compute_fill_ones(
                storage->gpu_buffer, dtype, count, backend_error)) {
            quidra::device::release(storage->gpu_buffer);
            storage->gpu_buffer = nullptr;
            partial_storage.relinquish();forget_tensor_storage_partial(storage);delete storage;
            tensor_fail(backend_error.c_str(), line, column);
        }
    }
    partial_storage.relinquish();return storage;
}

// True when every element the view references is inside storage and
// initialized. Fully initialized storage leaves only the view bounds to prove,
// which tensor_view_within_storage() does in O(rank); partially initialized
// storage and unproven views keep the exact per-element scan.
bool tensor_view_initialized(const TensorValue& tensor) {
    if (tensor.storage && tensor.storage->initialization.fully_initialized &&
        tensor_view_within_storage(tensor)) {
        return true;
    }
    const auto count = tensor_logical_count(tensor);
    for (std::size_t i = 0; i < count; ++i) {
        const auto storage_index = tensor_storage_index(tensor, i);
        if (storage_index >= tensor.storage->count ||
            !tracker_bit(tensor.storage->initialization, storage_index)) {
            return false;
        }
    }
    return true;
}

void tensor_require_initialized(const TensorValue& tensor,
                                unsigned long long line,
                                unsigned long long column) {
    if (!tensor_view_initialized(tensor)) runtime_uninitialized_failure(line, column);
}

// Runtime switch `name` selects between a new behaviour (`new_value`) and
// the previous one (`old_value`). Unset or empty, it takes `default_new`.
// Any other value also selects the previous behaviour, which a kill switch
// exists to restore, and is reported on stderr. Runtime switches read their
// variable once, on first use, into a function-local static.
bool runtime_switch_enabled(const char* name, const char* new_value,
                            const char* old_value, bool default_new) {
    const auto value = quidra::platform::environment_value(name);
    if (!value || value->empty()) return default_new;
    if (*value == new_value) return true;
    if (*value != old_value)
        std::fprintf(stderr,
                     "Quidra runtime warning: unrecognized %s=%s "
                     "(expected %s or %s); using %s\n",
                     name, value->c_str(), new_value, old_value, old_value);
    return false;
}

// Devices with strided compute (Metal, the test backend) read broadcast and
// strided views by index arithmetic in the kernel instead of host-built
// gather index maps; that is the default, which QUIDRA_BROADCAST=strided
// also names. QUIDRA_BROADCAST=gather restores the index maps; CUDA and HIP
// always use them.
bool tensor_gpu_strided_enabled(const TensorStorage& storage) {
    static const bool strided_requested = runtime_switch_enabled(
        "QUIDRA_BROADCAST", "strided", "gather", true);
    return strided_requested && storage.gpu_buffer &&
           quidra::device::supports_strided_compute(storage.gpu_buffer);
}

// Autograd keeps a dense tensor it retains for backward (a custom node's
// saved tensor, a CPU node's value, a host-engine gradient handed to a slot
// or callback) by sharing its storage copy-on-write instead of copying it.
// That is the default, which QUIDRA_SAVED_TENSORS=cow also names;
// QUIDRA_SAVED_TENSORS=copy copies every such tensor.
bool autograd_storage_sharing_enabled() {
    static const bool enabled = runtime_switch_enabled(
        "QUIDRA_SAVED_TENSORS", "cow", "copy", true);
    return enabled;
}

// Element strides that read `operand` broadcast to `output_shape` (identical
// ranks): the operand's own strides, and 0 on the axes it broadcasts.
std::vector<long long> tensor_broadcast_strides(
    const TensorValue& operand, const std::vector<long long>& output_shape) {
    std::vector<long long> strides(output_shape.size(), 0);
    for (std::size_t axis = 0; axis < output_shape.size(); ++axis)
        strides[axis] = operand.shape[axis] == 1 ? 0 : operand.strides[axis];
    return strides;
}

// O(rank) proof that every element the strided read of `source` over
// `shape` touches lies inside its storage; unproven views keep the
// per-element checked gather path.
bool tensor_strided_read_within_storage(
    const TensorValue& source, const std::vector<long long>& shape,
    const std::vector<long long>& strides) {
    return source.storage &&
           quidra::tensor_view::within_storage(
               source.offset, shape, strides, source.storage->count);
}

TensorStorage* tensor_gpu_materialize_storage(
    const TensorValue& source,
    unsigned long long line,
    unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeDeviceChecks);
    if (tensor_on_cpu(*source.storage)) {
        tensor_fail("internal GPU materialization received a CPU tensor", line, column);
    }
    const auto count = tensor_logical_count(source);
    auto* output = tensor_storage_create(
        source.storage->dtype, count, abi::tensor_fill_mode::uninitialized, source.storage->device, line, column);
    quidra::runtime::NodeRuntimeOwner partial_output(output,[](void* raw){tensor_storage_release(static_cast<TensorStorage*>(raw));});
    if (tensor_gpu_strided_enabled(*source.storage) &&
        tensor_view_within_storage(source)) {
        // Same elements as the gather below, addressed by the view's strides
        // on the device. Initialization propagates as in the gather path.
        std::string backend_error;
        if (!quidra::device::compute_strided_copy(
                output->gpu_buffer, source.storage->gpu_buffer,
                source.storage->dtype, source.offset, source.shape,
                source.strides, backend_error)) {
            partial_output.relinquish();tensor_storage_release(output);
            tensor_fail(backend_error.c_str(), line, column);
        }
        if (source.storage->initialization.fully_initialized) {
            tracker_mark_complete(output->initialization);
        } else {
            for (std::size_t logical = 0; logical < count; ++logical) {
                if (tracker_bit(source.storage->initialization,
                                tensor_storage_index(source, logical)))
                    tracker_set(output->initialization, logical);
            }
        }
        partial_output.relinquish();return output;
    }
    std::vector<std::uint64_t> indices;
    try {
        indices.resize(count);
    } catch (...) {
        partial_output.relinquish();tensor_storage_release(output);
        runtime_allocation_failure();
    }
    // A fully initialized source needs no per-element initialization
    // propagation: once the gather succeeds every output element holds an
    // initialized source element, so the output is marked complete in one
    // step. A view proven inside storage also skips the per-element range
    // check. Partially initialized sources keep exact per-element propagation.
    const bool source_complete = source.storage->initialization.fully_initialized;
    const bool in_storage = tensor_view_within_storage(source);
    for (std::size_t logical = 0; logical < count; ++logical) {
        const auto source_index = tensor_storage_index(source, logical);
        if (!in_storage && source_index >= source.storage->count) {
            partial_output.relinquish();tensor_storage_release(output);
            tensor_fail("tensor view exceeds storage", line, column);
        }
        indices[logical] = static_cast<std::uint64_t>(source_index);
        if (!source_complete &&
            tracker_bit(source.storage->initialization, source_index)) {
            tracker_set(output->initialization, logical);
        }
    }
    std::string backend_error;
    if (!quidra::device::compute_gather(
            output->gpu_buffer, source.storage->gpu_buffer,
            source.storage->dtype, indices.data(), count, backend_error)) {
        partial_output.relinquish();tensor_storage_release(output);
        tensor_fail(backend_error.c_str(), line, column);
    }
    if (source_complete) tracker_mark_complete(output->initialization);
    partial_output.relinquish();return output;
}

TensorStorage* tensor_transfer_storage(
    const TensorValue& source, int target_device,
    unsigned long long line, unsigned long long column) {
    const auto count = tensor_logical_count(source);
    auto* output = tensor_storage_create(
        source.storage->dtype, count, abi::tensor_fill_mode::uninitialized, target_device, line, column);
    quidra::runtime::NodeRuntimeOwner partial_output(output,[](void* raw){tensor_storage_release(static_cast<TensorStorage*>(raw));});
    const auto width = tensor_dtype_bytes(source.storage->dtype);
    if (count != 0 && width > std::numeric_limits<std::size_t>::max() / count) {
        partial_output.relinquish();tensor_storage_release(output);
        tensor_fail("tensor transfer size overflow", line, column);
    }
    const auto bytes = count * width;
    std::array<unsigned char, 8> element{};
    std::string backend_error;

    const auto mark_fully_initialized = [&] {
        output->initialization.fully_initialized = true;
        output->initialization.initialized_count = count;
        output->initialization.bits.clear();
    };

    // The common explicit-transfer case is fully initialized. Preserve the
    // logical transfer boundary while moving the payload in one bulk operation.
    // Non-contiguous GPU views are first gathered on the source GPU, so an
    // explicit .cpu() or .gpu(other) never degenerates into one transfer per
    // element.
    if (source.storage->initialization.fully_initialized) {
        TensorStorage* gpu_materialized = nullptr;
        quidra::runtime::NodeRuntimeOwner partial_materialized(nullptr, +[](void* value) {
            tensor_storage_release(static_cast<TensorStorage*>(value));
        });
        const TensorStorage* dense_gpu_source = nullptr;
        std::vector<unsigned char> host_materialized;
        const unsigned char* dense_host_source = nullptr;

        if (tensor_on_cpu(*source.storage)) {
            if (tensor_is_contiguous_value(source)) {
                if (source.offset > source.storage->count ||
                    count > source.storage->count - source.offset) {
                    partial_output.relinquish();tensor_storage_release(output);
                    tensor_fail("tensor view exceeds storage", line, column);
                }
                dense_host_source =
                    source.storage->data.data() + source.offset * width;
            } else {
                try {
                    host_materialized.resize(bytes);
                } catch (...) {
                    partial_output.relinquish();tensor_storage_release(output);
                    runtime_allocation_failure();
                }
                for (std::size_t logical = 0; logical < count; ++logical) {
                    const auto source_index = tensor_storage_index(source, logical);
                    if (source_index >= source.storage->count) {
                        partial_output.relinquish();tensor_storage_release(output);
                        tensor_fail("tensor view exceeds storage", line, column);
                    }
                    std::memcpy(
                        host_materialized.data() + logical * width,
                        source.storage->data.data() + source_index * width,
                        width);
                }
                dense_host_source = host_materialized.data();
            }
        } else {
            if (tensor_is_contiguous_value(source)) {
                if (source.offset > source.storage->count ||
                    count > source.storage->count - source.offset) {
                    partial_output.relinquish();tensor_storage_release(output);
                    tensor_fail("tensor view exceeds storage", line, column);
                }
                dense_gpu_source = source.storage;
            } else {
                gpu_materialized =
                    tensor_gpu_materialize_storage(source, line, column);
                partial_materialized.replace(gpu_materialized);
                dense_gpu_source = gpu_materialized;
            }
        }

        bool ok = true;
        if (dense_host_source) {
            if (tensor_on_cpu(*output)) {
                if (bytes != 0) {
                    std::memcpy(output->data.data(), dense_host_source, bytes);
                }
            } else {
                ok = quidra::device::copy_from_host(
                    output->gpu_buffer, 0, dense_host_source, bytes,
                    backend_error);
            }
        } else if (dense_gpu_source) {
            const auto source_offset =
                gpu_materialized ? std::size_t{0} : source.offset * width;
            if (tensor_on_cpu(*output)) {
                ok = quidra::device::copy_to_host(
                    dense_gpu_source->gpu_buffer, source_offset,
                    output->data.data(), bytes, backend_error);
            } else if (dense_gpu_source->device == target_device) {
                ok = quidra::device::copy_device_to_device(
                    output->gpu_buffer, 0, dense_gpu_source->gpu_buffer,
                    source_offset, bytes, backend_error);
            } else {
                // Cross-GPU movement is explicit at source level. Backends do
                // not currently promise peer access across arbitrary vendors,
                // so stage once through host memory rather than once per
                // element.
                try {
                    host_materialized.resize(bytes);
                } catch (...) {
                    if (gpu_materialized) { partial_materialized.relinquish(); tensor_storage_release(gpu_materialized); }
                    partial_output.relinquish();tensor_storage_release(output);
                    runtime_allocation_failure();
                }
                ok = quidra::device::copy_to_host(
                         dense_gpu_source->gpu_buffer, source_offset,
                         host_materialized.data(), bytes, backend_error) &&
                     quidra::device::copy_from_host(
                         output->gpu_buffer, 0, host_materialized.data(),
                         bytes, backend_error);
            }
        }

        if (gpu_materialized) { partial_materialized.relinquish(); tensor_storage_release(gpu_materialized); }
        if (!ok) {
            partial_output.relinquish();tensor_storage_release(output);
            tensor_fail(backend_error.c_str(), line, column);
        }
        mark_fully_initialized();
        if (bytes != 0 && source.storage->device != target_device)
            quidra::counters::add(quidra::counters::Id::UnifiedCopies);
        partial_output.relinquish();return output;
    }

    // Partially initialized tensors preserve initialization at element
    // granularity. This uncommon path intentionally copies only initialized
    // elements so uninitialized storage is never exposed as initialized data.
    for (std::size_t logical = 0; logical < count; ++logical) {
        const auto source_index = tensor_storage_index(source, logical);
        if (source_index >= source.storage->count) {
            partial_output.relinquish();tensor_storage_release(output);
            tensor_fail("tensor view exceeds storage", line, column);
        }
        if (!tracker_bit(source.storage->initialization, source_index)) continue;

        if (tensor_on_cpu(*source.storage)) {
            std::memcpy(element.data(),
                        source.storage->data.data() + source_index * width, width);
        } else if (!quidra::device::copy_to_host(
                       source.storage->gpu_buffer, source_index * width,
                       element.data(), width, backend_error)) {
            partial_output.relinquish();tensor_storage_release(output);
            tensor_fail(backend_error.c_str(), line, column);
        }

        if (tensor_on_cpu(*output)) {
            std::memcpy(output->data.data() + logical * width,
                        element.data(), width);
        } else if (!quidra::device::copy_from_host(
                       output->gpu_buffer, logical * width,
                       element.data(), width, backend_error)) {
            partial_output.relinquish();tensor_storage_release(output);
            tensor_fail(backend_error.c_str(), line, column);
        }
        tracker_set(output->initialization, logical);
    }
    if (output->initialization.initialized_count != 0 &&
        source.storage->device != target_device)
        quidra::counters::add(quidra::counters::Id::UnifiedCopies);
    partial_output.relinquish();return output;
}

#include "runtime_unified.inc"

template <typename Int>
std::uint64_t unsigned_magnitude(Int value) {
    static_assert(std::is_integral_v<Int>);
    if constexpr (std::is_signed_v<Int>) {
        if (value < 0) {
            using U = std::make_unsigned_t<Int>;
            return static_cast<std::uint64_t>(
                static_cast<U>(-(value + 1)) + static_cast<U>(1));
        }
    }
    return static_cast<std::uint64_t>(value);
}

template <typename Float, typename Int>
bool integer_exact_in_float(Int value) {
    auto magnitude = unsigned_magnitude(value);
    if (magnitude == 0) return true;
    int bits = 0;
    auto temp = magnitude;
    while (temp) { ++bits; temp >>= 1U; }
    const int precision = std::numeric_limits<Float>::digits;
    if (bits <= precision) return true;
    const int discarded = bits - precision;
    if (discarded >= 64) return false;
    const auto mask = (std::uint64_t{1} << discarded) - 1U;
    return (magnitude & mask) == 0;
}

template <typename Dst, typename Src>
bool exact_numeric_cast(Src source, Dst& destination) {
    if constexpr(std::is_integral_v<Dst>&&(std::is_floating_point_v<Src>||half_element<Src>)){
        const double value=static_cast<double>(source);
        if(!std::isfinite(value))return false;
        if(std::trunc(value)!=value){
            quidra::runtime::record_conversion_reason(abi::ConversionReason::not_integral);return false;
        }
        // Exclusive power-of-two upper bounds remain exact even where the
        // largest 64-bit integer itself is not representable in binary64.
        const double upper=std::ldexp(1.0,std::numeric_limits<Dst>::digits);
        const double lower=std::is_signed_v<Dst>?-upper:0.0;
        if(!std::isfinite(value)||value<lower||value>=upper)return false;
        destination=static_cast<Dst>(value);return true;
    } else if constexpr (complex_element<Dst>) {
        using Part = decltype(Dst{}.re);
        Part re{};
        Part im{};
        if constexpr (complex_element<Src>) {
            if (!exact_numeric_cast(source.re, re) || !exact_numeric_cast(source.im, im))
                return false;
        } else {
            if (!exact_numeric_cast(source, re)) return false;
        }
        destination = Dst{re, im};
        return true;
    } else if constexpr (complex_element<Src>) {
        return false;
    } else if constexpr (half_element<Dst> && std::is_integral_v<Src>) {
        // Rounded once from the integer (abi/half_format.hpp).
        if constexpr (std::is_same_v<Dst, Real16b> && sizeof(Src) == 8) {
            destination = Dst::from_bits(static_cast<std::uint16_t>(
                std::is_signed_v<Src> ? quidra_real16b_from_int64(static_cast<long long>(source))
                                      : quidra_real16b_from_uint64(static_cast<unsigned long long>(source))));
        } else {
            destination = Dst(static_cast<double>(source));
        }
        return element_is_finite(destination);
    } else if constexpr (half_element<Dst> || half_element<Src>) {
        if constexpr (std::is_integral_v<Dst>) {
            return false;
        } else {
            // Every real widens exactly to binary64; rounded once to Dst.
            const double value = element_value(source);
            if constexpr (half_element<Dst>) destination = Dst(value);
            else destination = static_cast<Dst>(value);
            return !element_is_nan(source) && !element_is_nan(destination) &&
                   (element_is_finite(destination) || !element_is_finite(source));
        }
    } else if constexpr (std::is_integral_v<Src> && std::is_integral_v<Dst>) {
        if (!std::in_range<Dst>(source)) return false;
        destination = static_cast<Dst>(source);
        return true;
    } else if constexpr (std::is_integral_v<Src> && std::is_floating_point_v<Dst>) {
        destination = static_cast<Dst>(source);
        return std::isfinite(destination);
    } else if constexpr (std::is_floating_point_v<Src> && std::is_integral_v<Dst>) {
        return false;
    } else {
        destination = static_cast<Dst>(source);
        if (std::isnan(source) || (std::isfinite(source) && !std::isfinite(destination))) return false;
        return true;
    }
}

template <typename Src, typename Dst>
bool tensor_cast_buffer(const TensorValue& tensor, TensorStorage& output) {
    const auto count = tensor_logical_count(tensor);
    const auto source_width = sizeof(Src);
    const auto destination_width = sizeof(Dst);
    for (std::size_t i = 0; i < count; ++i) {
        const auto source_index = tensor_storage_index(tensor, i);
        Src source{};
        std::memcpy(&source,
                    tensor.storage->data.data() + source_index * source_width,
                    source_width);
        Dst destination{};
        if (!exact_numeric_cast(source, destination)) return false;
        std::memcpy(output.data.data() + i * destination_width,
                    &destination, destination_width);
    }
    return true;
}

template <typename Src>
bool tensor_cast_from(const TensorValue& tensor, int target_dtype,
                      TensorStorage& output) {
    switch (target_dtype) {
        case QCORE_DTYPE_INT64: return tensor_cast_buffer<Src,std::int64_t>(tensor,output);
        case QCORE_DTYPE_INT8: return tensor_cast_buffer<Src,std::int8_t>(tensor,output);
        case QCORE_DTYPE_INT16: return tensor_cast_buffer<Src,std::int16_t>(tensor,output);
        case QCORE_DTYPE_INT32: return tensor_cast_buffer<Src,std::int32_t>(tensor,output);
        case QCORE_DTYPE_UINT8: return tensor_cast_buffer<Src,std::uint8_t>(tensor,output);
        case QCORE_DTYPE_UINT16: return tensor_cast_buffer<Src,std::uint16_t>(tensor,output);
        case QCORE_DTYPE_UINT32: return tensor_cast_buffer<Src,std::uint32_t>(tensor,output);
        case QCORE_DTYPE_UINT64: return tensor_cast_buffer<Src,std::uint64_t>(tensor,output);
        case QCORE_DTYPE_FLOAT64: return tensor_cast_buffer<Src,double>(tensor,output);
        case QCORE_DTYPE_FLOAT32: return tensor_cast_buffer<Src,float>(tensor,output);
        case QCORE_DTYPE_FLOAT16: return tensor_cast_buffer<Src,Real16>(tensor,output);
        case QCORE_DTYPE_BFLOAT16: return tensor_cast_buffer<Src,Real16b>(tensor,output);
        case QCORE_DTYPE_COMPLEX_FLOAT16: return tensor_cast_buffer<Src,ComplexElement<Real16>>(tensor,output);
        case QCORE_DTYPE_COMPLEX_BFLOAT16: return tensor_cast_buffer<Src,ComplexElement<Real16b>>(tensor,output);
        case QCORE_DTYPE_COMPLEX_FLOAT32: return tensor_cast_buffer<Src,ComplexElement<float>>(tensor,output);
        case QCORE_DTYPE_COMPLEX_FLOAT64: return tensor_cast_buffer<Src,ComplexElement<double>>(tensor,output);
        default: return false;
    }
}

// Package-native writable borrows use the same copy-on-write boundary as
// source-level tensor mutation. This keeps TensorStorage private while
// preserving independent-value semantics across package code.
void tensor_detach_for_write(
    TensorValue& tensor, unsigned long long line, unsigned long long column);

// Native borrows while the calling thread holds a device stream for a
// package encode (native_extension.h). A hold covers one device: a device
// borrow of a tensor on another device is refused. Inside the open encoder
// scope Core does no work on the scope's device stream, so only a borrow
// that is a pure lookup is served there. A borrow that would need Core work
// (the copy-on-write detach of tensor_detach_for_write, the relocation of a
// shared unified-memory view in tensor_unified_prepare_native_borrow, new
// output storage) is refused before any of that work. Each refusal is
// recorded as a protocol violation, which stops the program when the hold
// ends. The decision depends only on the tensor's state, never on the pool,
// poisoning, upload mode or GPU timing.
enum class NativeBorrow { Const, Mutable, Output };

bool native_borrow_refused_in_hold(const TensorValue& tensor, NativeBorrow kind,
                                   const char* call) {
    const auto& storage = *tensor.storage;
    if (quidra::device::package_hold_refuses_device(storage.device, call))
        return true;
    if (!quidra::device::package_scope_open(storage.device)) return false;
    const bool shared_view =
        storage.unified && quidra::unified::views(*storage.unified) > 1;
    const char* work = nullptr;
    switch (kind) {
        case NativeBorrow::Const:
            if (shared_view)
                work = "is a unified-memory view its source still shares (the "
                       "borrow first moves it to memory of its own)";
            break;
        case NativeBorrow::Mutable:
            // tensor_detach_for_write copies a view that does not cover its
            // whole storage densely from offset 0, and storage shared with
            // other owners.
            if (!(tensor.offset == 0 && tensor_is_contiguous_value(tensor) &&
                  tensor_logical_count(tensor) == storage.count))
                work = "is a view of part of its storage (the borrow first "
                       "copies it to storage of its own)";
            else if (!tensor_storage_writable_in_place(storage))
                work = "does not own its storage alone (the borrow first "
                       "detaches it copy-on-write)";
            break;
        case NativeBorrow::Output:
            if (!tensor_storage_writable_in_place(storage))
                work = "does not own its storage alone (the borrow first "
                       "gives it storage of its own)";
            break;
    }
    if (!work) return false;
    quidra::device::note_package_scope_violation(
        storage.device, std::string(call) + " of a tensor that " + work);
    return true;
}

// A mutable CPU borrow of a unified-memory view that device work may still
// read first waits for that work (I4 in runtime_unified.inc), and a package
// encode hold neither commits nor waits. Refused whenever the wait could be
// needed, whether or not that work has finished, so the outcome does not
// depend on GPU timing.
bool cpu_borrow_refused_in_hold(const TensorValue& tensor) {
    if (!quidra::device::package_hold_active()) return false;
    const auto* block = tensor.storage->unified;
    if (!block || quidra::unified::views(*block) != 1 ||
        !block->device_reads_possible.load(std::memory_order_acquire))
        return false;
    return quidra::device::note_package_hold_violation(
        "qcore_tensor_cpu_data of a CPU tensor whose unified memory device "
        "work may still read (the borrow first waits for that work)");
}

} // namespace

extern "C" unsigned int qcore_native_abi_version() {
    return 1U;
}

extern "C" [[noreturn]] void qcore_runtime_immediate_failure_exit() {
    quidra::runtime::immediate_failure_exit();
}

extern "C" int qcore_tensor_dtype(const void* raw) {
    if (!raw) return 0;
    const auto* tensor = static_cast<const TensorValue*>(raw);
    return tensor->storage ? tensor->storage->dtype : 0;
}

extern "C" long long qcore_tensor_device(const void* raw) {
    if (!raw) return -2;
    const auto* tensor = static_cast<const TensorValue*>(raw);
    return tensor->storage ? static_cast<long long>(tensor->storage->device) : -2;
}

extern "C" unsigned long long qcore_tensor_rank(const void* raw) {
    if (!raw) return 0;
    const auto* tensor = static_cast<const TensorValue*>(raw);
    return static_cast<unsigned long long>(tensor->shape.size());
}

extern "C" long long qcore_tensor_extent(
    const void* raw, unsigned long long axis) {
    if (!raw) return -1;
    const auto* tensor = static_cast<const TensorValue*>(raw);
    if (axis >= tensor->shape.size()) return -1;
    return tensor->shape[static_cast<std::size_t>(axis)];
}

extern "C" unsigned long long qcore_tensor_element_count(
    const void* raw) {
    if (!raw) return 0;
    const auto* tensor = static_cast<const TensorValue*>(raw);
    try {
        return static_cast<unsigned long long>(tensor_logical_count(*tensor));
    } catch (...) {
        return 0;
    }
}

extern "C" int qcore_tensor_is_contiguous(const void* raw) {
    if (!raw) return 0;
    const auto* tensor = static_cast<const TensorValue*>(raw);
    return tensor->storage && tensor_is_contiguous_value(*tensor) ? 1 : 0;
}

extern "C" int qcore_tensor_backend(const void* raw) {
    if (!raw) return -1;
    const auto* tensor = static_cast<const TensorValue*>(raw);
    if (!tensor->storage) return -1;
    if (tensor_on_cpu(*tensor->storage)) return QCORE_BACKEND_CPU;
    if (!tensor->storage->gpu_buffer) return -1;
    switch (quidra::device::buffer_backend(tensor->storage->gpu_buffer)) {
        case quidra::device::Backend::Cuda: return QCORE_BACKEND_CUDA;
        case quidra::device::Backend::Hip: return QCORE_BACKEND_HIP;
        case quidra::device::Backend::Metal: return QCORE_BACKEND_METAL;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
        case quidra::device::Backend::Test: return QCORE_BACKEND_TEST;
#endif
    }
    return -1;
}

extern "C" long long qcore_tensor_backend_device_index(const void* raw) {
    if (!raw) return -1;
    const auto* tensor = static_cast<const TensorValue*>(raw);
    if (!tensor->storage || tensor_on_cpu(*tensor->storage)) return -1;
    const auto* info = quidra::device::find(tensor->storage->device);
    if (!info) return -1;
    return static_cast<long long>(info->backend_index);
}

extern "C" void qcore_tensor_require_initialized(const void* raw) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if (!raw) tensor_fail("qcore_tensor_require_initialized received a null tensor", 0, 0);
    const auto* tensor = static_cast<const TensorValue*>(raw);
    if (!tensor->storage) tensor_fail("invalid tensor storage", 0, 0);
    tensor_require_initialized(*tensor, 0, 0);
}

// A package kernel may read every element of a borrowed device view, so the
// device handles refuse a view with an uninitialized element exactly as
// qcore_tensor_cpu_data(_const) do: the package then takes its portable path,
// which reports UNINITIALIZED at the Quidra source location.
// tensor_view_initialized() accepts fully initialized storage in O(rank) once
// the view is proven inside storage; only partially initialized storage has
// exactly the borrowed view scanned.
extern "C" std::uint64_t qcore_tensor_device_handle_const(
    const void* raw) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if (!raw) return 0;
    const auto* tensor = static_cast<const TensorValue*>(raw);
    if (!tensor->storage || tensor_on_cpu(*tensor->storage) ||
        !tensor_is_contiguous_value(*tensor) || !tensor->storage->gpu_buffer ||
        !tensor_view_initialized(*tensor)) {
        return 0;
    }
    if (native_borrow_refused_in_hold(*tensor, NativeBorrow::Const,
                                       "qcore_tensor_device_handle_const"))
        return 0;
    if (!tensor_unified_prepare_native_borrow(tensor->storage, false)) return 0;
    tensor_unified_note_device_lend(*tensor->storage);
    return quidra::device::buffer_native_handle(tensor->storage->gpu_buffer);
}

extern "C" std::uint64_t qcore_tensor_device_handle(void* raw) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if (!raw) return 0;
    auto* tensor = static_cast<TensorValue*>(raw);
    // The initialization check runs before the copy-on-write boundary, which
    // preserves per-element initialization, so a refused view is not copied.
    if (tensor->graph || !tensor->storage || tensor_on_cpu(*tensor->storage) ||
        !tensor_is_contiguous_value(*tensor) || !tensor->storage->gpu_buffer ||
        !tensor_view_initialized(*tensor)) {
        return 0;
    }
    if (native_borrow_refused_in_hold(*tensor, NativeBorrow::Mutable,
                                       "qcore_tensor_device_handle"))
        return 0;
    if (!tensor_unified_prepare_native_borrow(tensor->storage, true)) return 0;
    tensor_detach_for_write(*tensor, 0, 0);
    tensor_unified_note_device_lend(*tensor->storage);
    return quidra::device::buffer_native_handle(tensor->storage->gpu_buffer);
}

extern "C" std::uint64_t qcore_tensor_device_offset_bytes(
    const void* raw) {
    if (!raw) return 0;
    const auto* tensor = static_cast<const TensorValue*>(raw);
    if (!tensor->storage || tensor->offset > tensor->storage->count) return 0;
    return static_cast<std::uint64_t>(
        quidra::device::buffer_base_offset(tensor->storage->gpu_buffer) +
        tensor->offset * tensor_dtype_bytes(tensor->storage->dtype));
}

extern "C" int qcore_device_activate(long long device) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if (device < 0 ||
        device > static_cast<long long>(std::numeric_limits<int>::max())) {
        return 0;
    }
    try {
        std::string error;
        return quidra::device::activate(static_cast<int>(device), error) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

extern "C" std::uint64_t qcore_device_native_device(long long device) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if (device < 0 ||
        device > static_cast<long long>(std::numeric_limits<int>::max())) {
        return 0;
    }
    try {
        return quidra::device::native_device_handle(static_cast<int>(device));
    } catch (...) {
        return 0;
    }
}

extern "C" void qcore_register_warmup(void (*function)(long long device)) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    try {
        quidra::device::register_warmup(function);
    } catch (...) {
    }
}

extern "C" std::uint64_t qcore_device_queue_handle(long long device) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if (device < 0 ||
        device > static_cast<long long>(std::numeric_limits<int>::max())) {
        return 0;
    }
    try {
        if (quidra::device::package_hold_refuses_device(
                static_cast<int>(device), "qcore_device_queue_handle"))
            return 0;
        return quidra::device::queue_native_handle(static_cast<int>(device));
    } catch (...) {
        return 0;
    }
}

extern "C" void* qcore_device_buffer_allocate(
    long long device,std::uint64_t bytes) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if(device<0 ||
       device>static_cast<long long>(std::numeric_limits<int>::max()) ||
       bytes>static_cast<std::uint64_t>(
           std::numeric_limits<std::size_t>::max()))
        return nullptr;
    try {
        if (quidra::device::package_hold_refuses_device(
                static_cast<int>(device), "qcore_device_buffer_allocate"))
            return nullptr;
        std::string error;
        // Idle storage: the package may write it on the host at once.
        return quidra::device::allocate_buffer(
            static_cast<int>(device),static_cast<std::size_t>(bytes),true,error);
    } catch (...) {
        return nullptr;
    }
}

extern "C" std::uint64_t qcore_device_buffer_handle(const void* raw) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if(!raw) return 0;
    try {
        const auto* buffer = static_cast<const quidra::device::Buffer*>(raw);
        if (quidra::device::package_hold_refuses_device(
                quidra::device::buffer_device(buffer),
                "qcore_device_buffer_handle"))
            return 0;
        return quidra::device::buffer_native_handle(buffer);
    } catch (...) {
        return 0;
    }
}

extern "C" void qcore_device_buffer_release(void* raw) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if(!raw) return;
    try {
        quidra::device::release(
            static_cast<quidra::device::Buffer*>(raw));
    } catch (...) {
    }
}

namespace {
bool qcore_device_index(long long device, int& index) {
    if (device < 0 ||
        device > static_cast<long long>(std::numeric_limits<int>::max()))
        return false;
    index = static_cast<int>(device);
    return true;
}
} // namespace

extern "C" int qcore_device_encode_begin(long long device) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    int index = 0;
    if (!qcore_device_index(device, index)) return 0;
    try {
        return quidra::device::hold_package_stream(index) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

extern "C" std::uint64_t qcore_metal_command_buffer(long long device) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    int index = 0;
    if (!qcore_device_index(device, index)) return 0;
    try {
        std::string error;
        return quidra::device::begin_package_encode(index, true, error);
    } catch (...) {
        return 0;
    }
}

extern "C" std::uint64_t qcore_device_compute_encoder(long long device) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    int index = 0;
    if (!qcore_device_index(device, index)) return 0;
    try {
        std::string error;
        return quidra::device::begin_package_encode(index, false, error);
    } catch (...) {
        return 0;
    }
}

extern "C" int qcore_metal_note_work(
    long long device, std::uint32_t dispatches, std::uint64_t bytes) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    int index = 0;
    if (!qcore_device_index(device, index)) return 1;
    std::string error;
    bool violated = false;
    try {
        if (quidra::device::end_package_encode(
                index, dispatches, bytes, violated, error))
            return 0;
    } catch (...) {
        return 1;
    }
    if (!violated) return 1;
    // The package asked Core for work inside its encoder scope (or for a
    // commit or wait while it held the stream). Core refused it, and the
    // scope's command buffer was discarded; the program stops at the
    // statement that made the extern call, or at the backward() call whose
    // custom autograd callback this is (native_extension.h). A warm-up or
    // completion callback stopped the program in end_package_encode.
    if (const auto* call = package_backward_call)
        fail_package_scope_at(error, call->line, call->column);
    quidra::runtime::report_at_statement(
        abi::FailureReason::package_scope_violation, abi::FailureArgs{.message = error},
        quidra::runtime::current_user_statement(), {.provenance = true});
}

extern "C" int qcore_device_flush(long long device) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    int index = 0;
    if (!qcore_device_index(device, index)) return 1;
    try {
        if (quidra::device::package_hold_refuses_device(index, "qcore_device_flush"))
            return 1;
        std::string error;
        return quidra::device::flush(index, error) ? 0 : 1;
    } catch (...) {
        return 1;
    }
}

extern "C" int qcore_device_wait(long long device) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    int index = 0;
    if (!qcore_device_index(device, index)) return 1;
    try {
        if (quidra::device::package_hold_refuses_device(index, "qcore_device_wait"))
            return 1;
        std::string error;
        return quidra::device::wait_idle(index, error) ? 0 : 1;
    } catch (...) {
        return 1;
    }
}

extern "C" int qcore_device_status_slot(
    long long device, std::uint64_t* buffer, std::uint64_t* offset_bytes,
    std::uint64_t* slot) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    int index = 0;
    if (!qcore_device_index(device, index) || !buffer || !offset_bytes || !slot)
        return 1;
    try {
        if (quidra::device::package_hold_refuses_device(
                index, "qcore_device_status_slot"))
            return 1;
        // A slot may need a status page that Core clears first, so slots
        // are taken before the encoder scope opens, never inside it.
        if (quidra::device::package_scope_open(index)) {
            quidra::device::note_package_scope_violation(
                index, "qcore_device_status_slot (a slot may need Core to "
                       "clear a status page)");
            return 1;
        }
        std::string error;
        return quidra::device::status_slot(
            index, *buffer, *offset_bytes, *slot, error) ? 0 : 1;
    } catch (...) {
        return 1;
    }
}

extern "C" int qcore_device_defer_status(
    long long device, std::uint64_t slot, const char* message) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    int index = 0;
    if (!qcore_device_index(device, index)) return 1;
    try {
        if (quidra::device::package_hold_refuses_device(
                index, "qcore_device_defer_status"))
            return 1;
        std::string error;
        return quidra::device::defer_status(index, slot, message, error) ? 0 : 1;
    } catch (...) {
        return 1;
    }
}

extern "C" int qcore_device_defer_conversion_status(
    long long device, std::uint64_t slot, int target_dtype) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if (!abi::real_dtype_code(target_dtype)) return 1;
    int index = 0;
    if (!qcore_device_index(device, index)) return 1;
    try {
        if (quidra::device::package_hold_refuses_device(
                index, "qcore_device_defer_conversion_status"))
            return 1;
        std::string error;
        return quidra::device::defer_conversion_status(
            index, slot, target_dtype, error) ? 0 : 1;
    } catch (...) {
        return 1;
    }
}

extern "C" int qcore_device_status_wait(
    long long device, std::uint64_t slot, std::uint32_t* value) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    int index = 0;
    if (!qcore_device_index(device, index) || !value) return 1;
    try {
        if (quidra::device::package_hold_refuses_device(
                index, "qcore_device_status_wait"))
            return 1;
        std::string error;
        return quidra::device::status_wait(index, slot, *value, error) ? 0 : 1;
    } catch (...) {
        return 1;
    }
}

extern "C" void qcore_device_status_release(long long device, std::uint64_t slot) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    int index = 0;
    if (!qcore_device_index(device, index)) return;
    try {
        quidra::device::status_release(index, slot);
    } catch (...) {
    }
}

// Write-only package outputs. The package passes its output as a
// mutable tensor parameter; qcore_tensor_output_handle turns it into
// uninitialized device storage of the same dtype, shape and device, without
// a fill and without a per-element bitset, and returns its native handle.
// Reads fail as uninitialized until qcore_tensor_mark_written marks the
// whole storage written in O(1) after a successful encode. Exclusive storage
// is reused; shared storage stays with its other owners and the output gets
// fresh storage instead of a copy that would be overwritten anyway. This is
// the one way a package kernel may receive an uninitialized tensor
// (qcore_tensor_device_handle refuses uninitialized views): the package
// declares that it only writes it.
extern "C" std::uint64_t qcore_tensor_output_handle(void* raw) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if (!raw) return 0;
    auto* tensor = static_cast<TensorValue*>(raw);
    if (tensor->graph || !tensor->storage || tensor_on_cpu(*tensor->storage) ||
        !tensor->storage->gpu_buffer || tensor->offset != 0 ||
        !tensor_is_contiguous_value(*tensor))
        return 0;
    try {
        const auto count = tensor_logical_count(*tensor);
        if (count != tensor->storage->count) return 0;
        if (native_borrow_refused_in_hold(*tensor, NativeBorrow::Output,
                                           "qcore_tensor_output_handle"))
            return 0;
        // Storage shared with other owners, or with a unified-memory view of
        // another value, stays with them.
        if (!tensor_storage_writable_in_place(*tensor->storage)) {
            auto* fresh = tensor_storage_create(
                tensor->storage->dtype, count, abi::tensor_fill_mode::uninitialized, tensor->storage->device);
            tensor_storage_release(tensor->storage);
            tensor->storage = fresh;
        }
        if (!tensor_unified_prepare_native_borrow(tensor->storage, true)) return 0;
        tensor_unified_note_device_lend(*tensor->storage);
        auto& tracker = tensor->storage->initialization;
        tracker.fully_initialized = count == 0;
        tracker.initialized_count = tracker.fully_initialized ? count : 0;
        tracker.bits.clear();
        tracker.bits.shrink_to_fit();
        return quidra::device::buffer_native_handle(tensor->storage->gpu_buffer);
    } catch (...) {
        return 0;
    }
}

// Marks a write-only output (qcore_tensor_output_handle) written. It also
// accepts an output created uninitialized in Quidra code
// (tensor<T>(shape, gpu = n)); qcore_tensor_device_handle refuses handles to
// uninitialized views, so a package reaches such an output through
// qcore_tensor_output_handle. Only an exclusive, dense, untracked tensor that
// covers its storage qualifies.
extern "C" int qcore_tensor_mark_written(void* raw) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if (!raw) return 1;
    auto* tensor = static_cast<TensorValue*>(raw);
    if (tensor->graph || !tensor->storage || tensor->storage->owners != 1 ||
        tensor->offset != 0 || !tensor_is_contiguous_value(*tensor))
        return 1;
    try {
        if (tensor_logical_count(*tensor) != tensor->storage->count) return 1;
    } catch (...) {
        return 1;
    }
    tracker_mark_complete(tensor->storage->initialization);
    return 0;
}

extern "C" int qcore_device_on_complete(
    long long device, void (*callback)(void*), void* context) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    int index = 0;
    if (!qcore_device_index(device, index) || !callback) return 1;
    try {
        if (quidra::device::package_hold_refuses_device(
                index, "qcore_device_on_complete"))
            return 1;
        std::string error;
        return quidra::device::on_complete(index, callback, context, error) ? 0 : 1;
    } catch (...) {
        return 1;
    }
}

extern "C" int qcore_execution_is_deterministic() {
    return qcore_execution_policy_get() == QCORE_EXECUTION_DETERMINISTIC ? 1 : 0;
}

extern "C" const void* qcore_tensor_cpu_data_const(
    const void* raw) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if (!raw) return nullptr;
    const auto* tensor = static_cast<const TensorValue*>(raw);
    if (!tensor->storage || !tensor_on_cpu(*tensor->storage) ||
        !tensor_is_contiguous_value(*tensor)) {
        return nullptr;
    }
    if (!tensor_view_initialized(*tensor)) return nullptr;
    if (!tensor_unified_prepare_native_borrow(tensor->storage, false)) return nullptr;
    const auto width = tensor_dtype_bytes(tensor->storage->dtype);
    if (tensor->offset > tensor->storage->count) return nullptr;
    return tensor->storage->data.data() + tensor->offset * width;
}

extern "C" void* qcore_tensor_cpu_data(void* raw) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if (!raw) return nullptr;
    auto* tensor = static_cast<TensorValue*>(raw);
    // As in qcore_tensor_device_handle, the initialization check runs before
    // the copy-on-write boundary, so a refused view is not copied.
    if (tensor->graph || !tensor->storage || !tensor_on_cpu(*tensor->storage) ||
        !tensor_is_contiguous_value(*tensor) || !tensor_view_initialized(*tensor)) {
        return nullptr;
    }
    if (cpu_borrow_refused_in_hold(*tensor)) return nullptr;
    if (!tensor_unified_prepare_native_borrow(tensor->storage, true)) return nullptr;
    tensor_detach_for_write(*tensor, 0, 0);
    const auto width = tensor_dtype_bytes(tensor->storage->dtype);
    if (tensor->offset > tensor->storage->count) return nullptr;
    return tensor->storage->data.data() + tensor->offset * width;
}

template <typename Src>
bool numeric_cast_element_from(const void* source_raw, void* destination_raw,
                               int target_dtype) {
    Src source{};
    std::memcpy(&source, source_raw, sizeof(Src));
    const auto write = [&](auto tag) -> bool {
        using Dst = decltype(tag);
        Dst destination{};
        if (!exact_numeric_cast(source, destination)) return false;
        std::memcpy(destination_raw, &destination, sizeof(Dst));
        return true;
    };
    switch (target_dtype) {
        case QCORE_DTYPE_INT64: return write(std::int64_t{});
        case QCORE_DTYPE_INT8: return write(std::int8_t{});
        case QCORE_DTYPE_INT16: return write(std::int16_t{});
        case QCORE_DTYPE_INT32: return write(std::int32_t{});
        case QCORE_DTYPE_UINT8: return write(std::uint8_t{});
        case QCORE_DTYPE_UINT16: return write(std::uint16_t{});
        case QCORE_DTYPE_UINT32: return write(std::uint32_t{});
        case QCORE_DTYPE_UINT64: return write(std::uint64_t{});
        case QCORE_DTYPE_FLOAT64: return write(double{});
        case QCORE_DTYPE_FLOAT32: return write(float{});
        case QCORE_DTYPE_FLOAT16: return write(Real16{});
        case QCORE_DTYPE_BFLOAT16: return write(Real16b{});
        case QCORE_DTYPE_COMPLEX_FLOAT16: return write(ComplexElement<Real16>{});
        case QCORE_DTYPE_COMPLEX_BFLOAT16: return write(ComplexElement<Real16b>{});
        case QCORE_DTYPE_COMPLEX_FLOAT32: return write(ComplexElement<float>{});
        case QCORE_DTYPE_COMPLEX_FLOAT64: return write(ComplexElement<double>{});
        default:return false;
    }
}

static bool numeric_cast_element_try(
    const void* source, void* destination, int source_dtype, int target_dtype) {
    quidra::runtime::record_conversion_reason(abi::ConversionReason::out_of_range);
    bool ok=false;
    switch (source_dtype) {
        case QCORE_DTYPE_INT64: ok=numeric_cast_element_from<std::int64_t>(source,destination,target_dtype); break;
        case QCORE_DTYPE_INT8: ok=numeric_cast_element_from<std::int8_t>(source,destination,target_dtype); break;
        case QCORE_DTYPE_INT16: ok=numeric_cast_element_from<std::int16_t>(source,destination,target_dtype); break;
        case QCORE_DTYPE_INT32: ok=numeric_cast_element_from<std::int32_t>(source,destination,target_dtype); break;
        case QCORE_DTYPE_UINT8: ok=numeric_cast_element_from<std::uint8_t>(source,destination,target_dtype); break;
        case QCORE_DTYPE_UINT16: ok=numeric_cast_element_from<std::uint16_t>(source,destination,target_dtype); break;
        case QCORE_DTYPE_UINT32: ok=numeric_cast_element_from<std::uint32_t>(source,destination,target_dtype); break;
        case QCORE_DTYPE_UINT64: ok=numeric_cast_element_from<std::uint64_t>(source,destination,target_dtype); break;
        case QCORE_DTYPE_FLOAT64: ok=numeric_cast_element_from<double>(source,destination,target_dtype); break;
        case QCORE_DTYPE_FLOAT32: ok=numeric_cast_element_from<float>(source,destination,target_dtype); break;
        case QCORE_DTYPE_FLOAT16: ok=numeric_cast_element_from<Real16>(source,destination,target_dtype); break;
        case QCORE_DTYPE_BFLOAT16: ok=numeric_cast_element_from<Real16b>(source,destination,target_dtype); break;
        case QCORE_DTYPE_COMPLEX_FLOAT16: ok=numeric_cast_element_from<ComplexElement<Real16>>(source,destination,target_dtype); break;
        case QCORE_DTYPE_COMPLEX_BFLOAT16: ok=numeric_cast_element_from<ComplexElement<Real16b>>(source,destination,target_dtype); break;
        case QCORE_DTYPE_COMPLEX_FLOAT32: ok=numeric_cast_element_from<ComplexElement<float>>(source,destination,target_dtype); break;
        case QCORE_DTYPE_COMPLEX_FLOAT64: ok=numeric_cast_element_from<ComplexElement<double>>(source,destination,target_dtype); break;
        default: break;
    }
    return ok;
}

extern "C" bool quidra_numeric_cast_element_fits(
    const void* source, int source_dtype, int target_dtype,
    unsigned long long line, unsigned long long column) {
    if (!source) runtime_text_failure("null numeric cast storage");
    quidra_init_check(const_cast<void*>(source), line, column);
    std::array<unsigned char,sizeof(ComplexElement<double>)> scratch{};
    return numeric_cast_element_try(source,scratch.data(),source_dtype,target_dtype);
}

extern "C" void quidra_numeric_cast_element(
    void* destination, const void* source, int source_dtype, int target_dtype,
    unsigned long long line, unsigned long long column) {
    if (!destination || !source) runtime_text_failure("null numeric cast storage");
    quidra_init_check(const_cast<void*>(source), line, column);
    // The validator records integrality or range before any visible write.
    if (!numeric_cast_element_try(source,destination,source_dtype,target_dtype)) {
        quidra::runtime::report_conversion(quidra::runtime::last_conversion_reason(),
                                           abi::conversion_type_name(target_dtype),
                                           abi::ConversionSubject::array_element, line, column);
    }
}

extern "C" void* quidra_tensor_create(
    void* shape_array, int dtype, int fill_mode, bool has_gpu, long long gpu,
    unsigned long long line, unsigned long long column) {
    quidra::profile::Range create_range("core", "tensor create");
    if (fill_mode < abi::tensor_fill_mode::uninitialized || fill_mode > abi::tensor_fill_mode::ones) {
        tensor_fail("invalid tensor fill mode", line, column);
    }
    int device_index = -1;
    if (has_gpu) {
        quidra::counters::add(quidra::counters::Id::RuntimeDeviceChecks);
        if (gpu < 0 || gpu > std::numeric_limits<int>::max()) {
            tensor_fail("gpu index must be a non-negative supported index", line, column);
        }
        device_index = static_cast<int>(gpu);
    }
    auto shape = tensor_shape_from_array(shape_array, line, column);
    const auto count = tensor_element_count(shape, line, column);
    auto strides = tensor_contiguous_strides(shape);
    auto* storage = tensor_storage_create(
        dtype, count, fill_mode, device_index, line, column);
    quidra::runtime::NodeRuntimeOwner partial_storage(storage,[](void* raw){tensor_storage_release(static_cast<TensorStorage*>(raw));});
    return [&]{auto* complete=tensor_descriptor(
        storage, std::move(shape), std::move(strides), 0);partial_storage.relinquish();return complete;}();
}

extern "C" void* quidra_tensor_to_gpu(
    void* raw, long long gpu,
    unsigned long long line, unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeDeviceChecks);
    if (!raw) tensor_fail("null tensor", line, column);
    if (gpu < 0 || gpu > std::numeric_limits<int>::max()) {
        tensor_fail("gpu index must be a non-negative supported index", line, column);
    }
    auto* source = static_cast<TensorValue*>(raw);
    tensor_require_untracked_transform(*source,"gpu()",line,column);
    auto* storage = tensor_unified_transfer(
        *source, static_cast<int>(gpu), line, column);
    if (!storage)
        storage = tensor_transfer_storage(
            *source, static_cast<int>(gpu), line, column);
    quidra::runtime::NodeRuntimeOwner partial_storage(storage,[](void* raw){tensor_storage_release(static_cast<TensorStorage*>(raw));});
    return [&]{auto* complete=tensor_descriptor(
        storage, source->shape, tensor_contiguous_strides(source->shape), 0);partial_storage.relinquish();return complete;}();
}

extern "C" void* quidra_tensor_to_cpu(
    void* raw, unsigned long long line, unsigned long long column) {
    if (!raw) tensor_fail("null tensor", line, column);
    auto* source = static_cast<TensorValue*>(raw);
    tensor_require_untracked_transform(*source,"cpu()",line,column);
    auto* storage = tensor_unified_transfer(*source, -1, line, column);
    if (!storage) storage = tensor_transfer_storage(*source, -1, line, column);
    quidra::runtime::NodeRuntimeOwner partial_storage(storage,[](void* raw){tensor_storage_release(static_cast<TensorStorage*>(raw));});
    return [&]{auto* complete=tensor_descriptor(
        storage, source->shape, tensor_contiguous_strides(source->shape), 0);partial_storage.relinquish();return complete;}();
}

extern "C" void* quidra_tensor_clone(void* raw) {
    if (!raw) return nullptr;
    auto* source = static_cast<TensorValue*>(raw);
    if (!source->storage ||
        source->storage->owners == std::numeric_limits<std::size_t>::max()) {
        runtime_text_failure("invalid tensor storage");
    }
    ++source->storage->owners;
    quidra::runtime::NodeRuntimeOwner partial_retained_0(source->storage, +[](void* value) {
        tensor_storage_release(static_cast<TensorStorage*>(value));
    });
    auto* result=tensor_descriptor(
        source->storage, source->shape, source->strides, source->offset);
    partial_retained_0.relinquish();
    quidra::runtime::NodeRuntimeOwner partial_result(result,+[](void* value){
        quidra_managed_release(value,reinterpret_cast<void*>(&quidra_tensor_drop));
    });
    result->grad_slot=clone_autograd_slot(source->grad_slot);
    if(source->graph) result->graph=source->graph;
    partial_result.relinquish();
    return result;
}

extern "C" void quidra_tensor_drop(void* raw) {
    if (!raw) return;
    auto* tensor = static_cast<TensorValue*>(raw);
    auto* storage = tensor->storage;
    tensor->~TensorValue();
    tensor_storage_release(storage);
}

namespace {
void release_managed_tensor(void* raw) {
    if(!raw) return;
    quidra_managed_release(
        raw,reinterpret_cast<void*>(&quidra_tensor_drop));
}
void cleanup_node_tensor_vector(void* raw) {
    auto& values=*static_cast<std::vector<TensorValue*>*>(raw);
    for(auto*& slot:values) {
        auto* value=std::exchange(slot,nullptr);
        if(value)quidra::runtime::cleanup_node_slot(
            +[](void* owned){release_managed_tensor(owned);},value);
    }
}

} // namespace

extern "C" bool quidra_tensor_is_contiguous(void* raw) {
    if (!raw) runtime_text_failure("null tensor");
    return tensor_is_contiguous_value(*static_cast<TensorValue*>(raw));
}

extern "C" void* quidra_tensor_shape(void* raw) {
    if (!raw) runtime_text_failure("null tensor");
    const auto& shape = static_cast<TensorValue*>(raw)->shape;
    if (shape.size() > (std::numeric_limits<std::size_t>::max() - abi::array_layout::payload_offset) / sizeof(long long)) {
        runtime_allocation_failure();
    }
    const auto bytes = abi::array_layout::payload_offset + shape.size() * sizeof(long long);
    auto* result = static_cast<unsigned char*>(managed_allocate(bytes));
    const auto rank = static_cast<long long>(shape.size());
    std::memcpy(result, &rank, sizeof(rank));
    for (std::size_t i = 0; i < shape.size(); ++i) {
        std::memcpy(result + abi::array_layout::payload_offset + i * sizeof(long long), &shape[i], sizeof(long long));
    }
    const auto it = managed_state().allocations.find(reinterpret_cast<std::uintptr_t>(result));
    auto tracker = std::make_unique<InitializationTracker>();
    tracker->count = shape.size();
    tracker->unit_bytes = sizeof(long long);
    tracker->data_offset = abi::array_layout::payload_offset;
    tracker->initialized_count = shape.size();
    tracker->fully_initialized = true;
    it->second.initialization = std::move(tracker);
    return result;
}

extern "C" void* quidra_tensor_shape_fixed(void* raw, unsigned long long expected_rank) {
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if (!raw) runtime_text_failure("null tensor");
    const auto& shape = static_cast<TensorValue*>(raw)->shape;
    if (shape.size() != expected_rank) {
        runtime_text_failure("tensor static rank does not match runtime shape");
    }
    if (shape.size() > std::numeric_limits<std::size_t>::max() / sizeof(long long)) {
        runtime_allocation_failure();
    }
    const auto bytes = shape.size() * sizeof(long long);
    auto* result = static_cast<unsigned char*>(managed_allocate(bytes));
    for (std::size_t i = 0; i < shape.size(); ++i) {
        std::memcpy(result + i * sizeof(long long), &shape[i], sizeof(long long));
    }
    const auto it = managed_state().allocations.find(reinterpret_cast<std::uintptr_t>(result));
    auto tracker = std::make_unique<InitializationTracker>();
    tracker->count = shape.size();
    tracker->unit_bytes = sizeof(long long);
    tracker->data_offset = 0;
    tracker->initialized_count = shape.size();
    tracker->fully_initialized = true;
    it->second.initialization = std::move(tracker);
    return result;
}


namespace {
void tensor_attach_view_graph(
    TensorValue* result,const TensorValue& source,AutogradOp operation,
    std::vector<std::size_t> aux,
    unsigned long long line,unsigned long long column);

void tensor_attach_cast_graph(
    TensorValue* result,const TensorValue& source,
    unsigned long long line,unsigned long long column);

} // namespace

extern "C" void* quidra_tensor_reshape(void* raw, void* shape_array,
                                         unsigned long long line,
                                         unsigned long long column) {
    quidra::profile::Range operation_range("core", "tensor reshape");
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if (!raw) tensor_fail("null tensor", line, column);
    auto* source = static_cast<TensorValue*>(raw);
    if (!tensor_is_contiguous_value(*source)) {
        tensor_fail("reshape requires contiguous storage; call contiguous() explicitly", line, column);
    }
    auto shape = tensor_shape_from_array(shape_array, line, column);
    if (tensor_element_count(shape, line, column) != tensor_logical_count(*source)) {
        tensor_fail("reshape cannot change the number of elements", line, column);
    }
    if (source->storage->owners == std::numeric_limits<std::size_t>::max()) {
        runtime_text_failure("tensor storage owner overflow");
    }
    ++source->storage->owners;
    quidra::runtime::NodeRuntimeOwner partial_retained_1(source->storage, +[](void* value) {
        tensor_storage_release(static_cast<TensorStorage*>(value));
    });
    auto strides = tensor_contiguous_strides(shape);
    auto* result=tensor_descriptor(
        source->storage, std::move(shape), std::move(strides), source->offset);
    quidra::runtime::NodeRuntimeOwner partial_graph_result_1(result,+[](void* raw){release_managed_tensor(raw);});
    partial_retained_1.relinquish();
    tensor_attach_view_graph(
        result,*source,AutogradOp::Reshape,{},line,column);
    partial_graph_result_1.relinquish();
    return result;
}

extern "C" void* quidra_tensor_transpose(
    void* raw, long long axis0, long long axis1,
    unsigned long long line, unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if (!raw) tensor_fail("null tensor", line, column);
    auto* source = static_cast<TensorValue*>(raw);
    const auto rank = static_cast<long long>(source->shape.size());
    if (axis0 < 0 || axis1 < 0 || axis0 >= rank || axis1 >= rank) {
        tensor_fail("tensor.transpose axis is outside the tensor rank", line, column);
    }
    if (!source->storage ||
        source->storage->owners == std::numeric_limits<std::size_t>::max()) {
        runtime_text_failure("invalid tensor storage");
    }
    ++source->storage->owners;
    quidra::runtime::NodeRuntimeOwner partial_retained_2(source->storage, +[](void* value) {
        tensor_storage_release(static_cast<TensorStorage*>(value));
    });
    auto shape = source->shape;
    auto strides = source->strides;
    const auto a0 = static_cast<std::size_t>(axis0);
    const auto a1 = static_cast<std::size_t>(axis1);
    std::swap(shape[a0], shape[a1]);
    std::swap(strides[a0], strides[a1]);
    auto* result=tensor_descriptor(
        source->storage, std::move(shape), std::move(strides), source->offset);
    quidra::runtime::NodeRuntimeOwner partial_graph_result_2(result,+[](void* raw){release_managed_tensor(raw);});
    partial_retained_2.relinquish();
    tensor_attach_view_graph(
        result,*source,AutogradOp::Transpose,{a0,a1},line,column);
    partial_graph_result_2.relinquish();
    return result;
}

extern "C" void* quidra_tensor_contiguous(
    void* raw,unsigned long long line,unsigned long long column) {
    if (!raw) tensor_fail("null tensor",line,column);
    auto* source = static_cast<TensorValue*>(raw);
    tensor_require_untracked_transform(*source,"contiguous()",line,column);
    if (tensor_is_contiguous_value(*source)) return quidra_tensor_clone(raw);
    const auto count = tensor_logical_count(*source);
    TensorStorage* storage = nullptr;
    quidra::runtime::NodeRuntimeOwner partial_storage(nullptr, +[](void* value) {
        tensor_storage_release(static_cast<TensorStorage*>(value));
    });
    if (!tensor_on_cpu(*source->storage)) {
        storage = tensor_gpu_materialize_storage(*source, 0, 0);
        partial_storage.replace(storage);
    } else {
        storage = tensor_storage_create(source->storage->dtype, count, abi::tensor_fill_mode::uninitialized);
        partial_storage.replace(storage);
        // A view that is one dense run of storage without the canonical
        // strides (a size-1 axis carrying another stride) copies in bulk.
        if (!tensor_copy_dense_view(*source, *storage, count)) {
            const auto width = tensor_dtype_bytes(source->storage->dtype);
            const bool source_complete =
                source->storage->initialization.fully_initialized;
            for (std::size_t i = 0; i < count; ++i) {
                const auto source_index = tensor_storage_index(*source, i);
                std::memcpy(storage->data.data() + i * width,
                            source->storage->data.data() + source_index * width,
                            width);
                if (!source_complete &&
                    tracker_bit(source->storage->initialization, source_index)) {
                    tracker_set(storage->initialization, i);
                }
            }
            if (source_complete) tracker_mark_complete(storage->initialization);
        }
    }
    auto* complete = tensor_descriptor(storage, source->shape,
                             tensor_contiguous_strides(source->shape), 0);
    partial_storage.relinquish();
    return complete;
}

extern "C" void* quidra_tensor_item_ptr(void* raw, unsigned long long line,
                                         unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if (!raw) tensor_fail("null tensor", line, column);
    auto* tensor = static_cast<TensorValue*>(raw);
    if (!tensor->shape.empty()) {
        tensor_fail("item() requires a 0-D tensor", line, column);
    }
    if (tensor->offset >= tensor->storage->count ||
        !tracker_bit(tensor->storage->initialization, tensor->offset)) {
        runtime_uninitialized_failure(line, column);
    }
    const auto width = tensor_dtype_bytes(tensor->storage->dtype);
    if (tensor_on_cpu(*tensor->storage)) {
        return tensor->storage->data.data() + tensor->offset * width;
    }
    static thread_local std::array<unsigned char, 8> scalar{};
    std::string backend_error;
    if (!quidra::device::copy_to_host(
            tensor->storage->gpu_buffer, tensor->offset * width,
            scalar.data(), width, backend_error)) {
        tensor_fail(backend_error.c_str(), line, column);
    }
    return scalar.data();
}

// Numeric tensor casts apply each scalar's explicit conversion rule.
static bool tensor_cast_supported(int source, int target) {
    // Into a complex type from any numeric or complex type; out of a complex
    // type into a complex type only.
    if (abi::complex_dtype_code(target))
        return abi::numeric_dtype_code(source) || abi::complex_dtype_code(source);
    if (abi::complex_dtype_code(source)) return false;
    return abi::numeric_dtype_code(source) && abi::numeric_dtype_code(target);
}

static void* tensor_cast_impl(
    void* raw, int target_dtype, unsigned long long line,
    unsigned long long column, bool recoverable) {
    if (!raw) tensor_fail("null tensor", line, column);
    auto* source = static_cast<TensorValue*>(raw);
    tensor_require_initialized(*source, line, column);
    // An unsupported dtype pair is a TENSOR failure, before any device work;
    // only a value outside the destination's range is a conversion failure.
    if (!tensor_cast_supported(source->storage->dtype, target_dtype)) {
        const auto message = "tensor cast from " +
            std::string(abi::conversion_type_name(source->storage->dtype)) + " to " +
            std::string(abi::conversion_type_name(target_dtype)) + " is unsupported";
        tensor_fail(message.c_str(), line, column);
    }
    if (source->graph &&
        (!abi::real_dtype_code(source->storage->dtype) || !abi::real_dtype_code(target_dtype))) {
        tensor_fail(
            "tracked tensor cast requires floating source and destination types; call untrack() explicitly for other casts",
            line, column);
    }
    quidra::runtime::record_conversion_reason(abi::ConversionReason::out_of_range);
    const bool ieee_integer=abi::real_dtype_code(source->storage->dtype)&&
        target_dtype>=QCORE_DTYPE_INT64&&target_dtype<=QCORE_DTYPE_UINT64;
    const auto count = tensor_logical_count(*source);
    if (!tensor_on_cpu(*source->storage)) {
        if (recoverable||ieee_integer) {
            std::string pending_error;
            if (!quidra::device::synchronize(source->storage->device,pending_error)) {
                tensor_fail(pending_error.c_str(),line,column);
            }
        }
        TensorStorage* materialized = nullptr;
        quidra::runtime::NodeRuntimeOwner partial_materialized(nullptr, +[](void* value) {
            tensor_storage_release(static_cast<TensorStorage*>(value));
        });
        const TensorStorage* input_storage = source->storage;
        std::size_t input_offset = source->offset * tensor_dtype_bytes(source->storage->dtype);
        if (!tensor_is_contiguous_value(*source)) {
            materialized = tensor_gpu_materialize_storage(*source, line, column);
            partial_materialized.replace(materialized);
            input_storage = materialized;
            input_offset = 0;
        }
        auto* output = tensor_storage_create(
            target_dtype, count, gpu_kernel_output_fill(target_dtype),
            source->storage->device, line, column);
        quidra::runtime::NodeRuntimeOwner partial_output(output, +[](void* value) {
            tensor_storage_release(static_cast<TensorStorage*>(value));
        });
        std::string backend_error;
        const bool ok = quidra::device::compute_cast(
            output->gpu_buffer, input_storage->gpu_buffer, input_offset,
            source->storage->dtype, target_dtype, count, backend_error);
        if (materialized) { partial_materialized.relinquish(); tensor_storage_release(materialized); }
        if (!ok) {
            partial_output.relinquish();tensor_storage_release(output);
            quidra::runtime::record_conversion_reason(quidra::device::last_cast_reason());
            if (recoverable) return nullptr;
            if(ieee_integer)quidra::runtime::report_conversion(quidra::runtime::last_conversion_reason(),
                abi::conversion_type_name(target_dtype),abi::ConversionSubject::tensor_element,line,column);
            tensor_fail(backend_error.c_str(), line, column);
        }
        if (recoverable||ieee_integer) {
            std::string validation_error;
            if (!quidra::device::synchronize(source->storage->device,validation_error)) {
                partial_output.relinquish();tensor_storage_release(output);
                quidra::runtime::record_conversion_reason(quidra::device::last_cast_reason());
                if(recoverable)return nullptr;
                quidra::runtime::report_conversion(quidra::runtime::last_conversion_reason(),
                    abi::conversion_type_name(target_dtype),abi::ConversionSubject::tensor_element,line,column);
            }
        }
        auto* complete = tensor_descriptor(
            output,source->shape,tensor_contiguous_strides(source->shape),0);
        partial_output.relinquish();
        if(source->graph){
            quidra::runtime::NodeRuntimeOwner partial_complete(complete,+[](void* raw){release_managed_tensor(raw);});
            try {
                tensor_attach_cast_graph(complete,*source,line,column);
            } catch (...) {
                if(!quidra::runtime::node_failure_active()) release_managed_tensor(complete);
                throw;
            }
            partial_complete.relinquish();
        }
        return complete;
    }
    auto* output = tensor_storage_create(target_dtype, count, abi::tensor_fill_mode::zeros);
    quidra::runtime::NodeRuntimeOwner partial_output(output, +[](void* value) {
        tensor_storage_release(static_cast<TensorStorage*>(value));
    });
    bool exact = false;
    switch (source->storage->dtype) {
        case QCORE_DTYPE_INT64: exact=tensor_cast_from<std::int64_t>(*source,target_dtype,*output); break;
        case QCORE_DTYPE_INT8: exact=tensor_cast_from<std::int8_t>(*source,target_dtype,*output); break;
        case QCORE_DTYPE_INT16: exact=tensor_cast_from<std::int16_t>(*source,target_dtype,*output); break;
        case QCORE_DTYPE_INT32: exact=tensor_cast_from<std::int32_t>(*source,target_dtype,*output); break;
        case QCORE_DTYPE_UINT8: exact=tensor_cast_from<std::uint8_t>(*source,target_dtype,*output); break;
        case QCORE_DTYPE_UINT16: exact=tensor_cast_from<std::uint16_t>(*source,target_dtype,*output); break;
        case QCORE_DTYPE_UINT32: exact=tensor_cast_from<std::uint32_t>(*source,target_dtype,*output); break;
        case QCORE_DTYPE_UINT64: exact=tensor_cast_from<std::uint64_t>(*source,target_dtype,*output); break;
        case QCORE_DTYPE_FLOAT64: exact=tensor_cast_from<double>(*source,target_dtype,*output); break;
        case QCORE_DTYPE_FLOAT32: exact=tensor_cast_from<float>(*source,target_dtype,*output); break;
        case QCORE_DTYPE_FLOAT16: exact=tensor_cast_from<Real16>(*source,target_dtype,*output); break;
        case QCORE_DTYPE_BFLOAT16: exact=tensor_cast_from<Real16b>(*source,target_dtype,*output); break;
        case QCORE_DTYPE_COMPLEX_FLOAT16: exact=tensor_cast_from<ComplexElement<Real16>>(*source,target_dtype,*output); break;
        case QCORE_DTYPE_COMPLEX_BFLOAT16: exact=tensor_cast_from<ComplexElement<Real16b>>(*source,target_dtype,*output); break;
        case QCORE_DTYPE_COMPLEX_FLOAT32: exact=tensor_cast_from<ComplexElement<float>>(*source,target_dtype,*output); break;
        case QCORE_DTYPE_COMPLEX_FLOAT64: exact=tensor_cast_from<ComplexElement<double>>(*source,target_dtype,*output); break;
        default: break;
    }
    if (!exact) {
        partial_output.relinquish();tensor_storage_release(output);
        if (recoverable) return nullptr;
        end_package_hold_before_failure(line, column);
        quidra::runtime::report_conversion(quidra::runtime::last_conversion_reason(),
                                           abi::conversion_type_name(target_dtype),
                                           abi::ConversionSubject::tensor_element, line, column);
    }
    auto* complete = tensor_descriptor(
        output,source->shape,tensor_contiguous_strides(source->shape),0);
    partial_output.relinquish();
    if(source->graph){
        quidra::runtime::NodeRuntimeOwner partial_complete(complete,+[](void* raw){release_managed_tensor(raw);});
        try {
            tensor_attach_cast_graph(complete,*source,line,column);
        } catch (...) {
            if(!quidra::runtime::node_failure_active()) release_managed_tensor(complete);
            throw;
        }
        partial_complete.relinquish();
    }
    return complete;
}

extern "C" void* quidra_tensor_cast(
    void* raw, int target_dtype, unsigned long long line,
    unsigned long long column) {
    return tensor_cast_impl(raw,target_dtype,line,column,false);
}

extern "C" void* quidra_tensor_try_cast(
    void* raw, int target_dtype, unsigned long long line,
    unsigned long long column) {
    return tensor_cast_impl(raw,target_dtype,line,column,true);
}

extern "C" void quidra_tensor_rank_check(
    void* raw, long long expected_rank,
    unsigned long long line, unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if(!raw) tensor_fail("null tensor",line,column);
    const auto* tensor=static_cast<TensorValue*>(raw);
    // Negative ranks encode a minimum: -1 permits every rank, -2
    // requires one axis, and so on. Nonnegative ranks remain exact.
    const bool mismatch = expected_rank < 0
        ? tensor->shape.size() < static_cast<std::size_t>(-(expected_rank + 1))
        : tensor->shape.size() != static_cast<std::size_t>(expected_rank);
    if(mismatch)
        tensor_fail("tensor rank does not satisfy captured shape constraint",line,column);
}

extern "C" void quidra_tensor_extent_check(
    void* raw, long long axis, long long expected,
    unsigned long long line, unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if(!raw) tensor_fail("null tensor",line,column);
    const auto* tensor=static_cast<TensorValue*>(raw);
    if(axis < 0) axis += static_cast<long long>(tensor->shape.size());
    if(axis<0 || static_cast<std::size_t>(axis)>=tensor->shape.size())
        tensor_fail("tensor shape axis is outside rank",line,column);
    if(expected<0)
        tensor_fail("captured tensor extent cannot be negative",line,column);
    if(tensor->shape[static_cast<std::size_t>(axis)]!=expected)
        tensor_fail("tensor extent does not satisfy captured shape constraint",line,column);
}

extern "C" void quidra_tensor_shape_variable_check(
    void* raw, long long axis, long long expected, const char* variable, const char* subject,
    unsigned long long line, unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if(!raw) tensor_fail("null tensor",line,column);
    const auto* tensor=static_cast<TensorValue*>(raw);
    if(axis < 0) axis += static_cast<long long>(tensor->shape.size());
    if(axis<0 || static_cast<std::size_t>(axis)>=tensor->shape.size())
        tensor_fail("tensor shape axis is outside rank",line,column);
    if(expected<0)
        tensor_fail("captured tensor extent cannot be negative",line,column);
    const auto actual=tensor->shape[static_cast<std::size_t>(axis)];
    if(actual==expected) return;
    end_package_hold_before_failure(line,column);
    quidra::runtime::report_failure(
        abi::FailureReason::shape_variable_mismatch,
        abi::FailureArgs{.axis=axis,.path=subject?subject:"",.name=variable?variable:"",
                         .expected=expected,.actual=actual},
        line,column);
}

TensorValue* tensor_gather_logical_indices(
    TensorValue& source,std::vector<std::size_t> logical_indices,
    std::vector<long long> output_shape,
    unsigned long long line,unsigned long long column);

namespace {

void tensor_detach_for_write(TensorValue& tensor, unsigned long long line, unsigned long long column);
TensorStorage* tensor_gpu_materialize_storage(
    const TensorValue& source,
    unsigned long long line,
    unsigned long long column);


// Floating values of an autograd node or of a host-engine gradient. They
// either own a std::vector or retain a dense CPU TensorStorage of T (offset 0,
// every element initialized) by reference count. A retained storage is never
// written while it is shared: tensor writers detach shared storage first
// (tensor_detach_for_write behind eager writes and the mutable native
// accessors), and mutable_data() copies unless this value is the only owner.
// Retention lets forward snapshots, slot gradients and custom-callback
// gradients move between tensors and the host engine without element copies;
// every value read is the value a copy would have held. It follows
// autograd_storage_sharing_enabled() (the default); with
// QUIDRA_SAVED_TENSORS=copy every value is an owned vector.
template <typename T>
class HostValues {
public:
    static constexpr int element_dtype=std::is_same_v<T,float>?QCORE_DTYPE_FLOAT32:QCORE_DTYPE_FLOAT64;

    HostValues()=default;
    HostValues(std::vector<T> values) : owned_(std::move(values)) {}
    HostValues(const HostValues& other)
        : owned_(other.owned_),storage_(other.storage_),count_(other.count_) {
        if(storage_) retain(storage_);
    }
    HostValues(HostValues&& other) noexcept
        : owned_(std::move(other.owned_)),
          storage_(std::exchange(other.storage_,nullptr)),
          count_(std::exchange(other.count_,0)) {}
    HostValues& operator=(HostValues other) noexcept {
        owned_.swap(other.owned_);
        std::swap(storage_,other.storage_);
        std::swap(count_,other.count_);
        return *this;
    }
    ~HostValues() { drop_storage(); }

    // Shares `storage`, a CPU storage of T whose elements are all initialized.
    static HostValues share(TensorStorage* storage) {
        HostValues result;
        retain(storage);
        result.storage_=storage;
        result.count_=storage->count;
        return result;
    }

    // A buffer of `count` values that the caller writes completely through
    // mutable_data() before reading: storage-backed, so that it can be handed
    // out without a copy, or an owned vector when sharing is disabled.
    static HostValues allocate(std::size_t count) {
        if(!autograd_storage_sharing_enabled())
            return HostValues(std::vector<T>(count));
        HostValues result;
        result.storage_=tensor_storage_create(element_dtype,count,abi::tensor_fill_mode::uninitialized);
        tracker_mark_complete(result.storage_->initialization);
        result.count_=count;
        return result;
    }

    std::size_t size() const { return storage_?count_:owned_.size(); }
    bool empty() const { return size()==0; }
    const T* data() const {
        return storage_
            ? reinterpret_cast<const T*>(storage_->data.data()) : owned_.data();
    }
    const T& operator[](std::size_t index) const { return data()[index]; }
    const T* begin() const { return data(); }
    const T* end() const { return data()+size(); }

    // Writable values: a storage held only by this value is written in place,
    // a shared one is copied first.
    T* mutable_data() {
        if(!storage_) return owned_.data();
        if(!tensor_storage_writable_in_place(*storage_)){
            const auto count=count_;
            auto* copy=tensor_storage_create(element_dtype,count,abi::tensor_fill_mode::uninitialized);
            if(count!=0)
                std::memcpy(copy->data.data(),storage_->data.data(),count*sizeof(T));
            tracker_mark_complete(copy->initialization);
            drop_storage();
            storage_=copy;
            count_=count;
        }
        return reinterpret_cast<T*>(storage_->data.data());
    }

    // The values as an owned vector (copied out of a retained storage).
    std::vector<T>& vector() {
        if(storage_){
            std::vector<T> values(data(),data()+count_);
            drop_storage();
            owned_=std::move(values);
        }
        return owned_;
    }

    // A dense CPU tensor of `shape` holding the values: the retained storage
    // itself (reference counted) or a fresh copy of the owned vector.
    TensorValue* tensor(const std::vector<long long>& shape) const {
        TensorStorage* storage=storage_;
        if(storage){
            retain(storage);
        }else{
            storage=tensor_storage_create(element_dtype,owned_.size(),abi::tensor_fill_mode::uninitialized);
            if(!owned_.empty())
                std::memcpy(storage->data.data(),owned_.data(),owned_.size()*sizeof(T));
            tracker_mark_complete(storage->initialization);
        }
        quidra::runtime::NodeRuntimeOwner partial(storage,+[](void* raw){
            tensor_storage_release(static_cast<TensorStorage*>(raw));
        });
        auto* result=tensor_descriptor(storage,shape,tensor_contiguous_strides(shape),0);
        quidra::runtime::NodeRuntimeOwner partial_graph_result_3(result,+[](void* raw){release_managed_tensor(raw);});
        partial.relinquish();partial_graph_result_3.relinquish();
        return result;
    }

private:
    static void retain(TensorStorage* storage) {
        if(storage->owners==std::numeric_limits<std::size_t>::max())
            runtime_text_failure("tensor storage owner overflow");
        ++storage->owners;
    }
    void drop_storage() noexcept {
        if(quidra::runtime::node_failure_active()) {
            auto* value=std::exchange(storage_,nullptr);
            if(value)quidra::runtime::cleanup_node_slot(
                +[](void* raw){tensor_storage_release(static_cast<TensorStorage*>(raw));},value);
        } else if(storage_) tensor_storage_release(storage_);
        storage_=nullptr;
        count_=0;
    }

    std::vector<T> owned_;
    TensorStorage* storage_{};
    std::size_t count_{};
};

// The tensor dtypes autograd tracks: real32 and real64, and on the CPU
// real16 and real16b, whose values the host engine holds exactly as binary32.
// Each backward operation on a 16-bit node is computed in binary32 and its
// result is rounded once to the node's type; contributions to one gradient
// are added in that type, one rounding per addition (autograd_add_gradient).
bool autograd_half_dtype(int dtype) {
    return dtype==QCORE_DTYPE_FLOAT16||dtype==QCORE_DTYPE_BFLOAT16;
}

bool autograd_dtype(int dtype) {
    return dtype==QCORE_DTYPE_FLOAT32||dtype==QCORE_DTYPE_FLOAT64||autograd_half_dtype(dtype);
}

// The dtype of the host values of a node of `dtype`.
int autograd_compute_dtype(int dtype) {
    return autograd_half_dtype(dtype)?QCORE_DTYPE_FLOAT32:dtype;
}

const quidra::abi::half::HalfFormat& autograd_half_format(int dtype) {
    return dtype==QCORE_DTYPE_BFLOAT16?quidra::abi::half::bfloat16:quidra::abi::half::binary16;
}

// `value` rounded once to the 16-bit `dtype`, as the binary32 that holds it.
float autograd_round_half(float value,int dtype) {
    const auto& format=autograd_half_format(dtype);
    return static_cast<float>(quidra::abi::half::value_of(
        quidra::abi::half::round_to(static_cast<double>(value),format),format));
}

// A dense CPU tensor of the 16-bit `dtype` holding `values` rounded to it.
TensorValue* autograd_half_tensor(
    const float* values,std::size_t count,int dtype,const std::vector<long long>& shape) {
    auto* storage=tensor_storage_create(dtype,count,abi::tensor_fill_mode::uninitialized);
    const auto& format=autograd_half_format(dtype);
    for(std::size_t i=0;i<count;++i){
        const auto bits=quidra::abi::half::round_to(static_cast<double>(values[i]),format);
        std::memcpy(storage->data.data()+i*sizeof(bits),&bits,sizeof(bits));
    }
    tracker_mark_complete(storage->initialization);
    return tensor_descriptor(storage,shape,tensor_contiguous_strides(shape),0);
}

class AutogradBuffer {
public:
    explicit AutogradBuffer(int dtype=QCORE_DTYPE_FLOAT32) { set_dtype(dtype); }
    explicit AutogradBuffer(std::vector<float> values) : values_(HostValues<float>(std::move(values))) {}
    explicit AutogradBuffer(std::vector<double> values) : values_(HostValues<double>(std::move(values))) {}
    explicit AutogradBuffer(HostValues<float> values) : values_(std::move(values)) {}
    explicit AutogradBuffer(HostValues<double> values) : values_(std::move(values)) {}

    // real16 and real16b values are held as binary32 (half_dtype_).
    int dtype() const {
        if (half_dtype_!=0) return half_dtype_;
        return std::holds_alternative<HostValues<float>>(values_) ? QCORE_DTYPE_FLOAT32 : QCORE_DTYPE_FLOAT64;
    }

    void set_dtype(int dtype) {
        half_dtype_=autograd_half_dtype(dtype)?dtype:0;
        if (dtype==QCORE_DTYPE_FLOAT32||half_dtype_!=0) {
            if (!std::holds_alternative<HostValues<float>>(values_))
                values_.emplace<HostValues<float>>();
        } else if (dtype==QCORE_DTYPE_FLOAT64) {
            if (!std::holds_alternative<HostValues<double>>(values_))
                values_.emplace<HostValues<double>>();
        } else {
            runtime_text_failure("invalid autograd floating dtype");
        }
    }

    std::size_t size() const {
        return std::visit([](const auto& values){ return values.size(); }, values_);
    }

    bool empty() const { return size()==0; }

    void resize(std::size_t count) {
        std::visit([&](auto& values){ values.vector().resize(count); }, values_);
    }

    void assign(std::size_t count,double value) {
        if (auto* values=std::get_if<HostValues<float>>(&values_))
            values->vector().assign(count,static_cast<float>(value));
        else
            std::get<HostValues<double>>(values_).vector().assign(count,value);
    }

    double scalar_as_double(std::size_t index) const {
        if (const auto* values=std::get_if<HostValues<float>>(&values_))
            return static_cast<double>((*values)[index]);
        return std::get<HostValues<double>>(values_)[index];
    }

    template <typename T>
    const HostValues<T>& typed() const {
        return std::get<HostValues<T>>(values_);
    }

    template <typename T>
    HostValues<T>& typed() {
        return std::get<HostValues<T>>(values_);
    }

private:
    std::variant<HostValues<float>,HostValues<double>> values_;
    int half_dtype_{};
};

struct AutogradNode {
    explicit AutogradNode(int element_dtype=QCORE_DTYPE_FLOAT32)
        : dtype(element_dtype), data(element_dtype), aux(element_dtype) {
        quidra::counters::add(quidra::counters::Id::AutogradNodes);
    }
    // Called once after this node successfully retains its tensor value.
    // Sharing and empty values are still one logical save; parents and
    // scalar/index metadata do not create additional saved tensors.
    void note_saved_value() const {
        quidra::counters::add(quidra::counters::Id::AutogradSavedTensors);
    }

    int dtype{10};
    std::vector<long long> shape;
    AutogradBuffer data;
    AutogradOp op{AutogradOp::Leaf};
    std::vector<std::shared_ptr<AutogradNode>> parents;
    AutogradBuffer aux;
    std::vector<std::size_t> aux_index;
    std::shared_ptr<AutogradIdentity> target_identity;
    TensorValue* device_tensor{};
    TensorValue* device_aux{};
    qcore_autograd_backward_fn custom_backward{};
    qcore_autograd_backward_masked_fn custom_backward_masked{};
    qcore_autograd_backward_tracked_fn custom_backward_tracked{};
    std::vector<TensorValue*> custom_saved;
    std::vector<unsigned char> custom_metadata;
    // Masked callbacks only: input i was declared at attach time to be
    // written completely whenever its gradient is requested. Empty when
    // nothing was declared.
    std::vector<unsigned char> custom_full_writes;

    std::shared_ptr<AutogradNode> node_cleanup_next;
    ~AutogradNode() noexcept {
        if(quidra::runtime::node_failure_active()) {
            const auto drop_tensor=[](TensorValue*& slot) {
                auto* value=std::exchange(slot,nullptr);
                if(value)quidra::runtime::cleanup_node_slot(
                    +[](void* raw){release_managed_tensor(raw);},value);
            };
            drop_tensor(device_tensor);drop_tensor(device_aux);
            for(auto*& saved:custom_saved)drop_tensor(saved);
            custom_saved.clear();
            // Unique nodes temporarily hold the intrusive pending owner chain.
            // Moving shared_ptrs needs no allocation; nonunique parents simply
            // lose this reference and are destroyed by their remaining owner.
            std::shared_ptr<AutogradNode> pending;
            const auto enqueue=[&](std::shared_ptr<AutogradNode> value) {
                if(!value || value.use_count()!=1)return;
                value->node_cleanup_next=std::move(pending);
                pending=std::move(value);
            };
            for(auto& parent:parents)enqueue(std::move(parent));
            parents.clear();
            while(pending) {
                auto current=std::move(pending);
                pending=std::move(current->node_cleanup_next);
                for(auto& parent:current->parents)enqueue(std::move(parent));
                current->parents.clear();
            }
            return;
        }
        if(device_tensor){
            release_managed_tensor(device_tensor);
            device_tensor=nullptr;
        }
        if(device_aux){
            release_managed_tensor(device_aux);
            device_aux=nullptr;
        }
        for(auto*& saved:custom_saved){
            if(saved) release_managed_tensor(saved);
            saved=nullptr;
        }
        custom_saved.clear();
        // shared_ptr parent chains can otherwise recurse through destructors and
        // exhaust the native stack even though graph traversal itself is iterative.
        std::vector<std::shared_ptr<AutogradNode>> pending;
        pending.swap(parents);
        while(!pending.empty()){
            auto current=std::move(pending.back());
            pending.pop_back();
            if(!current || current.use_count()!=1) continue;
            for(auto& parent:current->parents)
                pending.push_back(std::move(parent));
            current->parents.clear();
        }
    }
};

struct AutogradValue {
    std::shared_ptr<AutogradNode> node;
};

struct AutogradIdentity {};

struct AutogradSlot {
    TensorValue* gradient{};
    int dtype{};
    std::shared_ptr<AutogradIdentity> identity;
    ~AutogradSlot() {
        if(quidra::runtime::node_failure_active()) {
            auto* value=std::exchange(gradient,nullptr);
            if(value)quidra::runtime::cleanup_node_slot(
                +[](void* raw){release_managed_tensor(raw);},value);
            return;
        }
        if(gradient) release_managed_tensor(gradient);
    }
};

std::shared_ptr<AutogradSlot> new_autograd_slot(
    int dtype=0,std::shared_ptr<AutogradIdentity> identity={}) {
    try{
        auto slot=std::make_shared<AutogradSlot>();
        slot->dtype=dtype;
        slot->identity=identity?std::move(identity):std::make_shared<AutogradIdentity>();
        return slot;
    }catch(...){
        runtime_allocation_failure();
    }
}

std::shared_ptr<AutogradSlot> clone_autograd_slot(
    const std::shared_ptr<AutogradSlot>& source) {
    if(!source) return {};
    // Gradient storage is value-local, but a copy of an already tracked leaf
    // still denotes the same logical leaf in the preserved computation graph.
    return new_autograd_slot(source->dtype,source->identity);
}

struct AutogradTargetHandle {
    std::shared_ptr<AutogradSlot> slot;
};

[[noreturn]] void autograd_fail(
    const char* message,unsigned long long line,unsigned long long column);

AutogradTargetHandle* autograd_target_from_value(void* value) {
    if(!value) return nullptr;
    std::uintptr_t bits{};
    std::memcpy(&bits,value,sizeof(bits));
    return reinterpret_cast<AutogradTargetHandle*>(bits);
}

void* make_autograd_target_value(AutogradTargetHandle* handle) {
    auto* value=managed_allocate(sizeof(std::uintptr_t));
    const auto bits=reinterpret_cast<std::uintptr_t>(handle);
    std::memcpy(value,&bits,sizeof(bits));
    return value;
}

extern "C" void* quidra_autograd_target_create() {
    try{
        auto slot=new_autograd_slot();
        return make_autograd_target_value(new AutogradTargetHandle{std::move(slot)});
    }catch(...){
        runtime_allocation_failure();
    }
}

extern "C" void* quidra_autograd_target_clone(void* value) {
    try{
        auto* source=autograd_target_from_value(value);
        if(!source||!source->slot) autograd_fail("invalid autograd target copy",0,0);
        auto slot=new_autograd_slot(source->slot->dtype);
        if(source->slot->gradient)
            slot->gradient=static_cast<TensorValue*>(quidra_tensor_clone(source->slot->gradient));
        return make_autograd_target_value(new AutogradTargetHandle{std::move(slot)});
    }catch(...){
        runtime_allocation_failure();
    }
}

extern "C" void quidra_autograd_target_drop(void* value) {
    if(!value) return;
    auto* handle=autograd_target_from_value(value);
    std::uintptr_t zero{};
    std::memcpy(value,&zero,sizeof(zero));
    delete handle;
}

extern "C" bool quidra_autograd_target_has_grad(void* value) {
    auto* handle=autograd_target_from_value(value);
    return handle&&handle->slot&&handle->slot->gradient;
}

extern "C" void quidra_autograd_target_clear_grad(
    void* value,unsigned long long line,unsigned long long column) {
    auto* handle=autograd_target_from_value(value);
    if(!handle||!handle->slot) autograd_fail("invalid autograd target",line,column);
    if(!handle->slot->gradient) return;
    if(quidra::runtime::node_failure_active()) {
        auto* old=std::exchange(handle->slot->gradient,nullptr);
        release_managed_tensor(old);
    } else {
    release_managed_tensor(handle->slot->gradient);
    handle->slot->gradient=nullptr;
    }
}

extern "C" void* quidra_autograd_target_gradient(
    void* value,int dtype,unsigned long long line,unsigned long long column) {
    auto* handle=autograd_target_from_value(value);
    if(!handle||!handle->slot) autograd_fail("invalid autograd target",line,column);
    if(!handle->slot->gradient)
        autograd_fail("autograd target gradient is not available",line,column);
    if(handle->slot->dtype!=dtype)
        autograd_fail("autograd target gradient dtype does not match requested type",line,column);
    return quidra_tensor_clone(handle->slot->gradient);
}

struct AutogradGradient {
    explicit AutogradGradient(int element_dtype=QCORE_DTYPE_FLOAT32) : dtype(element_dtype), data(element_dtype) {}
    AutogradGradient(const AutogradGradient&)=delete;
    AutogradGradient& operator=(const AutogradGradient&)=delete;
    AutogradGradient(AutogradGradient&& other) noexcept
        : dtype(other.dtype),
          shape(std::move(other.shape)),
          data(std::move(other.data)),
          device_tensor(std::exchange(other.device_tensor,nullptr)) {}
    AutogradGradient& operator=(AutogradGradient&& other) noexcept {
        if(this==&other) return *this;
        if(quidra::runtime::node_failure_active()) {
            auto* value=std::exchange(device_tensor,nullptr);
            if(value)quidra::runtime::cleanup_node_slot(+[](void* raw){release_managed_tensor(raw);},value);
        } else if(device_tensor) release_managed_tensor(device_tensor);
        dtype=other.dtype;
        shape=std::move(other.shape);
        data=std::move(other.data);
        device_tensor=std::exchange(other.device_tensor,nullptr);
        return *this;
    }
    ~AutogradGradient() {
        if(quidra::runtime::node_failure_active()) {
            auto* value=std::exchange(device_tensor,nullptr);
            if(value)quidra::runtime::cleanup_node_slot(+[](void* raw){release_managed_tensor(raw);},value);
        } else if(device_tensor) release_managed_tensor(device_tensor);
    }

    int dtype{10};
    std::vector<long long> shape;
    AutogradBuffer data;
    TensorValue* device_tensor{};
};

[[noreturn]] void autograd_fail(const char* message, unsigned long long line, unsigned long long column) {
    end_package_hold_before_failure(line, column);
    quidra::runtime::report_failure(abi::FailureReason::autograd_failure,
                                    abi::FailureArgs{.message = message}, line, column);
}

AutogradBuffer tensor_float_values(const TensorValue& tensor,
                                 unsigned long long line,
                                 unsigned long long column) {
    tensor_require_cpu(*tensor.storage, "autograd tensor conversion", line, column);
    tensor_require_initialized(tensor,line,column);
    if(!autograd_dtype(tensor.storage->dtype))
        autograd_fail("autograd values require real16, real16b, real32 or real64 tensors",line,column);
    const auto count=tensor_logical_count(tensor);
    if(autograd_half_dtype(tensor.storage->dtype)){
        // The 16-bit values widen exactly to binary32.
        const auto& format=autograd_half_format(tensor.storage->dtype);
        std::vector<float> values(count);
        for(std::size_t i=0;i<count;++i){
            const auto index=tensor_storage_index(tensor,i);
            std::uint16_t bits{};
            std::memcpy(&bits,tensor.storage->data.data()+index*sizeof(bits),sizeof(bits));
            values[i]=static_cast<float>(quidra::abi::half::value_of(bits,format));
        }
        AutogradBuffer result(std::move(values));
        result.set_dtype(tensor.storage->dtype);
        return result;
    }
    // A view that densely covers its whole (initialized) storage is retained
    // by reference count instead of copied when sharing is enabled: writers
    // detach shared storage, so the snapshot keeps the values it has now.
    // Other contiguous views (offset views) are one dense byte range of
    // storage and are copied with a single memcpy; the rest keep the
    // per-element gather.
    const bool dense=tensor_view_dense_in_storage(tensor,count);
    if(dense && tensor.offset==0 && count==tensor.storage->count &&
       autograd_storage_sharing_enabled()){
        if(tensor.storage->dtype==QCORE_DTYPE_FLOAT32)
            return AutogradBuffer(HostValues<float>::share(tensor.storage));
        return AutogradBuffer(HostValues<double>::share(tensor.storage));
    }
    const auto snapshot=[&](auto tag)->AutogradBuffer{
        using T=decltype(tag);
        std::vector<T> values(count);
        if(dense){
            if(count!=0)
                std::memcpy(values.data(),
                            tensor.storage->data.data()+tensor.offset*sizeof(T),
                            count*sizeof(T));
        }else{
            for(std::size_t i=0;i<count;++i){
                const auto index=tensor_storage_index(tensor,i);
                std::memcpy(&values[i],tensor.storage->data.data()+index*sizeof(T),sizeof(T));
            }
        }
        return AutogradBuffer(std::move(values));
    };
    if(tensor.storage->dtype==QCORE_DTYPE_FLOAT32) return snapshot(float{});
    return snapshot(double{});
}

std::shared_ptr<AutogradNode> autograd_constant_node(const TensorValue& tensor,
                                                 unsigned long long line,
                                                 unsigned long long column) {
    tensor_require_initialized(tensor,line,column);
    if(!autograd_dtype(tensor.storage->dtype))
        autograd_fail("autograd values require real16, real16b, real32 or real64 tensors",line,column);
    auto node=std::make_shared<AutogradNode>(tensor.storage->dtype);
    node->shape=tensor.shape;
    if(tensor_on_cpu(*tensor.storage)){
        node->data=tensor_float_values(tensor,line,column);
    }else{
        node->device_tensor=static_cast<TensorValue*>(
            quidra_tensor_clone(const_cast<TensorValue*>(&tensor)));
        if(!node->device_tensor)
            autograd_fail("failed to retain GPU tensor for autograd graph",line,column);
    }
    node->note_saved_value();
    return node;
}

void tensor_attach_view_graph(
    TensorValue* result,const TensorValue& source,AutogradOp operation,
    std::vector<std::size_t> aux,
    unsigned long long line,unsigned long long column) {
    if(!result||!source.graph) return;
    if(!autograd_dtype(source.storage->dtype))
        autograd_fail("tracked tensor view requires a floating dtype",line,column);
    auto node=std::make_shared<AutogradNode>(source.storage->dtype);
    node->shape=result->shape;
    node->parents={source.graph};
    node->op=operation;
    node->aux_index=std::move(aux);
    if(tensor_on_cpu(*result->storage))
        node->data=tensor_float_values(*result,line,column);
    else
        node->device_tensor=static_cast<TensorValue*>(quidra_tensor_clone(result));
    node->note_saved_value();
    result->graph=std::move(node);
}

void tensor_attach_cast_graph(
    TensorValue* result,const TensorValue& source,
    unsigned long long line,unsigned long long column) {
    if(!result||!source.graph) return;
    if(!autograd_dtype(source.storage->dtype)||!autograd_dtype(result->storage->dtype))
        autograd_fail("tracked tensor cast requires floating dtypes",line,column);
    auto node=std::make_shared<AutogradNode>(result->storage->dtype);
    node->shape=result->shape;
    node->parents={source.graph};
    node->op=AutogradOp::Cast;
    if(tensor_on_cpu(*result->storage))
        node->data=tensor_float_values(*result,line,column);
    else
        node->device_tensor=static_cast<TensorValue*>(quidra_tensor_clone(result));
    node->note_saved_value();
    result->graph=std::move(node);
}

// A contiguous CPU tensor holding gradient values: their retained storage
// when they have one, otherwise a copy.
template <typename T>
TensorValue* autograd_tensor_from_gradient(
    int dtype,const std::vector<long long>& shape,const HostValues<T>& values) {
    if(autograd_compute_dtype(dtype)!=HostValues<T>::element_dtype)
        runtime_text_failure("autograd buffer dtype mismatch");
    if(autograd_half_dtype(dtype)){
        if constexpr(std::is_same_v<T,float>)
            return autograd_half_tensor(values.data(),values.size(),dtype,shape);
    }
    return values.tensor(shape);
}

TensorValue* autograd_tensor_from_values(
    int dtype,const std::vector<long long>& shape,const AutogradBuffer& data) {
    if(autograd_compute_dtype(data.dtype())!=autograd_compute_dtype(dtype))
        runtime_text_failure("autograd buffer dtype mismatch");
    if(autograd_half_dtype(dtype)){
        const auto& values=data.typed<float>();
        return autograd_half_tensor(values.data(),values.size(),dtype,shape);
    }
    if(dtype==QCORE_DTYPE_FLOAT32) return data.typed<float>().tensor(shape);
    if(dtype==QCORE_DTYPE_FLOAT64) return data.typed<double>().tensor(shape);
    runtime_text_failure("invalid autograd floating dtype");
}

// Host values of a gradient tensor that a custom native callback wrote. With
// sharing enabled, the dense storage Core allocated for it is retained rather
// than copied; anything else is read through tensor_float_values().
template <typename T>
HostValues<T> autograd_gradient_values(
    const TensorValue& gradient,unsigned long long line,unsigned long long column) {
    if(autograd_storage_sharing_enabled() &&
       gradient.storage && tensor_on_cpu(*gradient.storage) &&
       gradient.storage->dtype==HostValues<T>::element_dtype &&
       gradient.storage->initialization.fully_initialized &&
       gradient.offset==0 && tensor_is_contiguous_value(gradient) &&
       tensor_logical_count(gradient)==gradient.storage->count)
        return HostValues<T>::share(gradient.storage);
    auto values=tensor_float_values(gradient,line,column);
    if(autograd_compute_dtype(values.dtype())!=HostValues<T>::element_dtype)
        runtime_text_failure("autograd gradient dtype mismatch");
    return std::move(values.template typed<T>());
}

TensorValue* autograd_tensor_from_node(const AutogradNode& node) {
    if(node.device_tensor)
        return static_cast<TensorValue*>(quidra_tensor_clone(node.device_tensor));
    return autograd_tensor_from_values(node.dtype,node.shape,node.data);
}

// Fails when `tensor`, a real tensor whose values code outside Core's checked
// kernels computed (a package's output, a gradient), holds NaN: on the CPU now
// with `reason` and `args`; on a GPU through a
// deferred check with the same message. Elements not yet initialized are not
// read; a GPU storage that is only partly initialized is not validated (its
// other elements are never read before they are written).
void tensor_validate_finite(const TensorValue& tensor, abi::FailureReason reason,
                            const abi::FailureArgs& args, unsigned long long line,
                            unsigned long long column) {
    if (!tensor.storage) return;
    // A complex element is its two parts of the part dtype.
    const int dtype = abi::complex_part_dtype_code(tensor.storage->dtype);
    const std::size_t parts = abi::complex_dtype_code(tensor.storage->dtype) ? 2 : 1;
    if (!abi::real_dtype_code(dtype)) return;
    const auto count = tensor_logical_count(tensor);
    if (count == 0) return;
    if (tensor_on_cpu(*tensor.storage)) {
        quidra::counters::add(quidra::counters::Id::FiniteValidations); // qcount
        const auto width = tensor_dtype_bytes(dtype);
        const auto* data = tensor.storage->data.data();
        for (std::size_t element = 0; element < count * parts; ++element) {
            const auto logical = element / parts;
            const auto element_index = tensor_storage_index(tensor, logical);
            if (element_index >= tensor.storage->count ||
                !tracker_bit(tensor.storage->initialization, element_index))
                continue;
            const auto index = element_index * parts + element % parts;
            bool valid = true;
            if (width == 8) {
                double value{};
                std::memcpy(&value, data + index * 8, 8);
                valid = !std::isnan(value);
            } else if (width == 2) {
                std::uint16_t bits{};
                std::memcpy(&bits, data + index * 2, 2);
                valid = !std::isnan(quidra::abi::half::value_of(bits, autograd_half_format(dtype)));
            } else {
                float value{};
                std::memcpy(&value, data + index * 4, 4);
                valid = !std::isnan(value);
            }
            if (!valid) {
                end_package_hold_before_failure(line, column);
                quidra::runtime::report_failure(reason, args, line, column);
            }
        }
        return;
    }
    if (!tensor.storage->initialization.fully_initialized) return;
    std::string error;
    if (!quidra::device::validate_finite(tensor.storage->gpu_buffer, 0, dtype,
                                         tensor.storage->count * parts, abi::format(reason, args),
                                         error))
        tensor_fail(error.c_str(), line, column);
}

// The element type a real tensor's failures name (real32).
std::string_view tensor_element_name(int dtype) {
    return abi::dtype_info(static_cast<abi::Dtype>(dtype)).quidra_name;
}

void autograd_accumulate_slot(
    const std::shared_ptr<AutogradSlot>& slot,TensorValue* gradient,
    unsigned long long line,unsigned long long column,
    bool preserve_graph=false) {
    quidra::runtime::NodeRuntimeOwner partial_gradient(gradient,+[](void* raw){release_managed_tensor(raw);});
    if(!slot||!gradient) autograd_fail("invalid autograd gradient slot",line,column);
    if(!autograd_dtype(slot->dtype))
        autograd_fail("invalid autograd gradient slot dtype",line,column);
    {
        // A delivered gradient may be infinite, but no NaN from Core or
        // a package backward callback becomes observable.
        const auto type = "tensor<" + std::string(tensor_element_name(gradient->storage->dtype)) + ">";
        tensor_validate_finite(
            *gradient, abi::FailureReason::real_invalid,
            abi::FailureArgs{.type = type,
                             .operation = abi::real_operation_info(abi::RealOperation::gradient).name},
            line, column);
    }
    if(gradient->storage->dtype!=slot->dtype){
        auto* converted=static_cast<TensorValue*>(
            quidra_tensor_cast(gradient,slot->dtype,line,column));
        auto* old=gradient;
        gradient=converted;partial_gradient.replace(converted);
        release_managed_tensor(old);
    }
    if(!preserve_graph) gradient->graph.reset();
    gradient->grad_slot.reset();
    if(!slot->gradient){
        slot->gradient=gradient;partial_gradient.relinquish();
        return;
    }
    auto* combined=static_cast<TensorValue*>(
        quidra_tensor_binary(slot->gradient,gradient,nullptr,abi::scalar_side::none,abi::tensor_binary_opcode::add,line,column));
    quidra::runtime::NodeRuntimeOwner partial_combined(combined,+[](void* raw){release_managed_tensor(raw);});
    if(quidra::runtime::node_failure_active()) {
        if(!preserve_graph) combined->graph.reset();
        combined->grad_slot.reset();
        auto* old=std::exchange(slot->gradient,combined);
        partial_combined.relinquish();
        release_managed_tensor(old);
        partial_gradient.relinquish();release_managed_tensor(gradient);
        return;
    }
    release_managed_tensor(slot->gradient);
    release_managed_tensor(gradient);
    if(!preserve_graph) combined->graph.reset();
    combined->grad_slot.reset();
    slot->gradient=combined;
}

std::size_t autograd_node_count(const AutogradNode& node) {
    return node.device_tensor
        ? tensor_logical_count(*node.device_tensor)
        : node.data.size();
}

void autograd_require_same_node_device(
    const char* operation,const AutogradNode& left,const AutogradNode& right,
    unsigned long long line,unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeDeviceChecks);
    const bool left_gpu=left.device_tensor!=nullptr;
    const bool right_gpu=right.device_tensor!=nullptr;
    if(left_gpu!=right_gpu){
        const auto message=std::string(operation)+
            " operands must be on the same device; transfer them explicitly before track()";
        autograd_fail(message.c_str(),line,column);
    }
    if(left_gpu &&
       left.device_tensor->storage->device!=right.device_tensor->storage->device){
        const auto message=std::string(operation)+
            " operands must use the same gpu(n)";
        autograd_fail(message.c_str(),line,column);
    }
}

void autograd_require_same_shape(const AutogradNode& a,const AutogradNode& b,
                               unsigned long long line,unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if(a.shape!=b.shape || autograd_node_count(a)!=autograd_node_count(b))
        autograd_fail("autograd operand shapes must match",line,column);
    if(a.dtype!=b.dtype)
        autograd_fail("autograd operand element types must match",line,column);
    autograd_require_same_node_device("autograd operation",a,b,line,column);
}

// Higher-order (backward(track = true)) symbolic nodes live on the device of
// the values they differentiate. CPU nodes keep host-backed AutogradBuffer
// values; GPU nodes hold a device TensorValue. A GPU node's value is computed
// by the same untracked device kernel the eager tensor operation uses, and
// the graph edge is recorded here, so the next backward differentiates it
// with the ordinary GPU engines.

// A borrowed, graph-free view of a GPU node's value: the eager kernels take
// it as an untracked operand. It does not own its storage.
TensorValue autograd_device_view(const AutogradNode& node) {
    const auto* value=node.device_tensor;
    return TensorValue{
        value->storage,value->shape,value->strides,value->offset,{},{}};
}

// A dense, offset-0 copy of a GPU value's logical elements (the value itself,
// retained, when it already is one). The caller owns the result.
TensorValue* autograd_device_dense_value(
    const TensorValue& value,unsigned long long line,unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeDeviceChecks);
    if(tensor_on_cpu(*value.storage))
        autograd_fail("internal autograd GPU path received a CPU tensor",line,column);
    if(tensor_is_contiguous_value(value)&&value.offset==0){
        if(value.storage->owners==std::numeric_limits<std::size_t>::max())
            runtime_text_failure("tensor storage owner overflow");
        ++value.storage->owners;
        quidra::runtime::NodeRuntimeOwner partial_retained_3(value.storage, +[](void* value) {
            tensor_storage_release(static_cast<TensorStorage*>(value));
        });
        return [&] { auto* complete=tensor_descriptor(value.storage,value.shape,value.strides,0); partial_retained_3.relinquish(); return complete; }();
    }
    auto* storage=tensor_gpu_materialize_storage(value,line,column);
    return tensor_descriptor(
        storage,value.shape,tensor_contiguous_strides(value.shape),0);
}

std::shared_ptr<AutogradNode> autograd_device_symbolic_node(
    int dtype,std::vector<long long> shape,AutogradOp operation,
    std::vector<std::shared_ptr<AutogradNode>> parents,TensorValue* value) {
    quidra::runtime::NodeRuntimeOwner partial_value(value,+[](void* raw){release_managed_tensor(raw);});
    value->graph.reset();
    value->grad_slot.reset();
    auto node=std::make_shared<AutogradNode>(dtype);
    node->shape=std::move(shape);
    node->op=operation;
    node->parents=std::move(parents);
    node->device_tensor=value;
    partial_value.relinquish();
    node->note_saved_value();
    return node;
}

std::shared_ptr<AutogradNode> autograd_device_symbolic_node_safe(
    int dtype,const std::vector<long long>& shape,AutogradOp operation,
    std::initializer_list<std::shared_ptr<AutogradNode>> parents,TensorValue* value) {
    if(!quidra::runtime::node_failure_active())
        return autograd_device_symbolic_node(dtype,shape,operation,
            std::vector<std::shared_ptr<AutogradNode>>(parents),value);
    quidra::runtime::NodeRuntimeOwner partial(value,+[](void* raw){release_managed_tensor(raw);});
    value->graph.reset();value->grad_slot.reset();
    auto node=std::make_shared<AutogradNode>(dtype);
    node->shape=shape;node->op=operation;node->parents=parents;
    node->device_tensor=value;partial.relinquish();
    node->note_saved_value();return node;
}

template <typename T>
std::shared_ptr<AutogradNode> autograd_symbolic_binary_t(
    const std::shared_ptr<AutogradNode>& left,
    const std::shared_ptr<AutogradNode>& right,
    int operation,
    unsigned long long line,unsigned long long column) {
    const auto& a=left->data.typed<T>();
    const auto& b=right->data.typed<T>();
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if(a.size()!=b.size()) autograd_fail("higher-order gradient shape mismatch",line,column);
    std::vector<T> values(a.size(),T{0});
    for(std::size_t i=0;i<values.size();++i){
        if(operation==abi::tensor_binary_opcode::add) values[i]=static_cast<T>(a[i]+b[i]);
        else if(operation==abi::tensor_binary_opcode::subtract) values[i]=static_cast<T>(a[i]-b[i]);
        else if(operation==abi::tensor_binary_opcode::multiply) values[i]=static_cast<T>(a[i]*b[i]);
        else {
            if(b[i]==T{0}) autograd_fail("division by zero",line,column);
            values[i]=static_cast<T>(a[i]/b[i]);
        }
    }
    auto node=std::make_shared<AutogradNode>(left->dtype);
    node->shape=left->shape;
    node->parents={left,right};
    node->op=operation==abi::tensor_binary_opcode::add?AutogradOp::Add:
        operation==abi::tensor_binary_opcode::subtract?AutogradOp::Sub:
        operation==abi::tensor_binary_opcode::multiply?AutogradOp::Mul:AutogradOp::Div;
    node->data=AutogradBuffer(std::move(values));
    node->note_saved_value();
    return node;
}

std::shared_ptr<AutogradNode> autograd_symbolic_binary(
    const std::shared_ptr<AutogradNode>& left,
    const std::shared_ptr<AutogradNode>& right,
    int operation,
    unsigned long long line,unsigned long long column) {
    if(!left||!right) autograd_fail("null higher-order gradient operand",line,column);
    autograd_require_same_shape(*left,*right,line,column);
    if(left->dtype!=QCORE_DTYPE_FLOAT64&&left->dtype!=QCORE_DTYPE_FLOAT32)
        autograd_fail("invalid higher-order gradient dtype",line,column);
    if(left->device_tensor){
        // Same element-wise device kernel as the eager tensor operation
        // (IEEE division, as for every GPU float tensor division).
        auto a=autograd_device_view(*left);
        auto b=autograd_device_view(*right);
        auto* value=static_cast<TensorValue*>(
            quidra_tensor_binary(&a,&b,nullptr,abi::scalar_side::none,operation,line,column));
        return autograd_device_symbolic_node_safe(
            left->dtype,left->shape,
            operation==abi::tensor_binary_opcode::add?AutogradOp::Add:
            operation==abi::tensor_binary_opcode::subtract?AutogradOp::Sub:
            operation==abi::tensor_binary_opcode::multiply?AutogradOp::Mul:AutogradOp::Div,
            {left,right},value);
    }
    if(left->dtype==QCORE_DTYPE_FLOAT32) return autograd_symbolic_binary_t<float>(
        left,right,operation,line,column);
    if(left->dtype==QCORE_DTYPE_FLOAT64) return autograd_symbolic_binary_t<double>(
        left,right,operation,line,column);
    autograd_fail("invalid higher-order gradient dtype",line,column);
}

template <typename T>
std::shared_ptr<AutogradNode> autograd_symbolic_scalar_t(
    const std::shared_ptr<AutogradNode>& input,double scalar,
    int operation,bool scalar_left,
    unsigned long long line,unsigned long long column) {
    const auto& source=input->data.typed<T>();
    const T scalar_value=static_cast<T>(scalar);
    std::vector<T> values(source.size(),T{0});
    for(std::size_t i=0;i<values.size();++i){
        const T left=scalar_left?scalar_value:source[i];
        const T right=scalar_left?source[i]:scalar_value;
        if(operation==abi::tensor_binary_opcode::add) values[i]=static_cast<T>(left+right);
        else if(operation==abi::tensor_binary_opcode::subtract) values[i]=static_cast<T>(left-right);
        else if(operation==abi::tensor_binary_opcode::multiply) values[i]=static_cast<T>(left*right);
        else if(operation==abi::tensor_binary_opcode::power) {
            if(scalar_left)
                autograd_fail("tensor power requires tensor ^ scalar",line,column);
            values[i]=static_cast<T>(std::pow(left,right));
        } else {
            if(right==T{0}) autograd_fail("division by zero",line,column);
            values[i]=static_cast<T>(left/right);
        }
    }
    auto node=std::make_shared<AutogradNode>(input->dtype);
    node->shape=input->shape;
    node->parents={input};
    node->op=AutogradOp::ScalarBinary;
    node->aux.assign(1,scalar);
    node->aux_index={
        static_cast<std::size_t>(operation),
        scalar_left?std::size_t{1}:std::size_t{0}};
    node->data=AutogradBuffer(std::move(values));
    node->note_saved_value();
    return node;
}

std::shared_ptr<AutogradNode> autograd_symbolic_scalar(
    const std::shared_ptr<AutogradNode>& input,double scalar,
    int operation,bool scalar_left,
    unsigned long long line,unsigned long long column) {
    if(!input) autograd_fail("null higher-order gradient operand",line,column);
    if(input->device_tensor){
        if(input->dtype!=QCORE_DTYPE_FLOAT64&&input->dtype!=QCORE_DTYPE_FLOAT32)
            autograd_fail("invalid higher-order gradient dtype",line,column);
        if(operation==abi::tensor_binary_opcode::power&&scalar_left)
            autograd_fail("tensor power requires tensor ^ scalar",line,column);
        // The scalar divisor is a host value: reject zero exactly as the CPU
        // symbolic path does, without reading device memory.
        const bool zero_divisor=input->dtype==QCORE_DTYPE_FLOAT32
            ? static_cast<float>(scalar)==0.0F : scalar==0.0;
        if(operation==abi::tensor_binary_opcode::divide&&!scalar_left&&zero_divisor)
            autograd_fail("division by zero",line,column);
        auto source=autograd_device_view(*input);
        const int side=scalar_left?1:2;
        TensorValue* value=nullptr;
        if(input->dtype==QCORE_DTYPE_FLOAT32){
            float typed=static_cast<float>(scalar);
            value=static_cast<TensorValue*>(quidra_tensor_binary(
                &source,nullptr,&typed,side,operation,line,column));
        }else{
            double typed=scalar;
            value=static_cast<TensorValue*>(quidra_tensor_binary(
                &source,nullptr,&typed,side,operation,line,column));
        }
        auto node=autograd_device_symbolic_node_safe(
            input->dtype,input->shape,AutogradOp::ScalarBinary,{input},value);
        node->aux.assign(1,scalar);
        node->aux_index={
            static_cast<std::size_t>(operation),
            scalar_left?std::size_t{1}:std::size_t{0}};
        return node;
    }
    if(input->dtype==QCORE_DTYPE_FLOAT32) return autograd_symbolic_scalar_t<float>(
        input,scalar,operation,scalar_left,line,column);
    if(input->dtype==QCORE_DTYPE_FLOAT64) return autograd_symbolic_scalar_t<double>(
        input,scalar,operation,scalar_left,line,column);
    autograd_fail("invalid higher-order gradient dtype",line,column);
}

// Symbolic tensor negation: the gradient of -x is -g, recorded as a Neg node
// so that higher-order backward differentiates it again.
std::shared_ptr<AutogradNode> autograd_symbolic_neg(
    const std::shared_ptr<AutogradNode>& input,
    unsigned long long line,unsigned long long column) {
    if(!input) autograd_fail("null higher-order gradient operand",line,column);
    if(input->dtype!=QCORE_DTYPE_FLOAT64&&input->dtype!=QCORE_DTYPE_FLOAT32)
        autograd_fail("invalid higher-order gradient dtype",line,column);
    if(input->device_tensor){
        auto source=autograd_device_view(*input);
        auto* value=static_cast<TensorValue*>(
            quidra_tensor_unary(&source,abi::tensor_unary_opcode::negate,line,column));
        return autograd_device_symbolic_node_safe(
            input->dtype,input->shape,AutogradOp::Neg,{input},value);
    }
    auto node=std::make_shared<AutogradNode>(input->dtype);
    node->shape=input->shape;
    node->parents={input};
    node->op=AutogradOp::Neg;
    const auto negate=[&](auto tag){
        using T=decltype(tag);
        const auto& source=input->data.template typed<T>();
        std::vector<T> values(source.begin(),source.end());
        for(auto& value:values) value=static_cast<T>(-value);
        node->data=AutogradBuffer(std::move(values));
    };
    if(input->dtype==QCORE_DTYPE_FLOAT32) negate(float{});
    else negate(double{});
    node->note_saved_value();
    return node;
}

template <typename T>
std::vector<T> autograd_gather_values(
    const HostValues<T>& input,const std::vector<std::size_t>& indices,
    unsigned long long line,unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    std::vector<T> output(indices.size());
    for(std::size_t i=0;i<indices.size();++i){
        if(indices[i]>=input.size())
            autograd_fail("tensor gather index is out of bounds for the input",line,column);
        output[i]=input[indices[i]];
    }
    return output;
}

template <typename T>
std::vector<T> autograd_gather_backward_values(
    const HostValues<T>& gradient,std::size_t source_count,
    const std::vector<std::size_t>& indices,
    unsigned long long line,unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if(gradient.size()!=indices.size())
        autograd_fail("tensor gather backward size mismatch",line,column);
    std::vector<T> output(source_count,T{0});
    for(std::size_t i=0;i<indices.size();++i){
        if(indices[i]>=source_count)
            autograd_fail("tensor gather backward index is out of bounds for the input",line,column);
        output[indices[i]]=static_cast<T>(output[indices[i]]+gradient[i]);
    }
    return output;
}



std::shared_ptr<AutogradNode> autograd_symbolic_reshape(
    const std::shared_ptr<AutogradNode>& input,
    const std::vector<long long>& output_shape,
    unsigned long long line,unsigned long long column);
std::shared_ptr<AutogradNode> autograd_symbolic_transpose(
    const std::shared_ptr<AutogradNode>& input,
    std::size_t axis0,std::size_t axis1,
    unsigned long long line,unsigned long long column);






std::shared_ptr<AutogradNode> autograd_symbolic_reshape(
    const std::shared_ptr<AutogradNode>& input,
    const std::vector<long long>& output_shape,
    unsigned long long line,unsigned long long column) {
    if(!input) autograd_fail("null higher-order reshape operand",line,column);
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if(tensor_element_count(input->shape,line,column)!=
       tensor_element_count(output_shape,line,column))
        autograd_fail("higher-order reshape cannot change element count",line,column);
    auto node=std::make_shared<AutogradNode>(input->dtype);
    node->shape=output_shape;
    node->parents={input};
    node->op=AutogradOp::Reshape;
    if(input->device_tensor){
        auto* source=input->device_tensor;
        if(!tensor_is_contiguous_value(*source)){
            // A strided GPU value (for example a transposed gradient) is
            // reshaped from a dense copy of its logical elements, which is
            // the element order the host-backed CPU node holds.
            auto* storage=tensor_gpu_materialize_storage(*source,line,column);
            node->device_tensor=tensor_descriptor(
                storage,output_shape,tensor_contiguous_strides(output_shape),0);
            node->note_saved_value();
            return node;
        }
        if(source->storage->owners==std::numeric_limits<std::size_t>::max())
            runtime_text_failure("tensor storage owner overflow");
        ++source->storage->owners;
        quidra::runtime::NodeRuntimeOwner partial_retained_4(source->storage, +[](void* value) {
            tensor_storage_release(static_cast<TensorStorage*>(value));
        });
        node->device_tensor=tensor_descriptor(
            source->storage,output_shape,tensor_contiguous_strides(output_shape),source->offset);
        partial_retained_4.relinquish();
    }else{
        node->data=input->data;
    }
    node->note_saved_value();
    return node;
}

template <typename T>
AutogradBuffer autograd_transpose_values(
    const AutogradBuffer& input,const std::vector<long long>& input_shape,
    std::size_t axis0,std::size_t axis1,
    unsigned long long line,unsigned long long column) {
    const auto count=tensor_element_count(input_shape,line,column);
    const auto& values=input.typed<T>();
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if(values.size()!=count) autograd_fail("transpose graph size mismatch",line,column);
    std::vector<long long> output_shape=input_shape;
    std::swap(output_shape[axis0],output_shape[axis1]);
    std::vector<T> output(count);
    auto input_strides=tensor_contiguous_strides(input_shape);
    auto output_strides=tensor_contiguous_strides(output_shape);
    std::vector<std::size_t> coordinates(output_shape.size());
    for(std::size_t linear=0;linear<count;++linear){
        std::size_t rest=linear;
        for(std::size_t axis=output_shape.size();axis-- >0;){
            const auto dim=static_cast<std::size_t>(output_shape[axis]);
            coordinates[axis]=dim==0?0:rest%dim;
            if(dim!=0) rest/=dim;
        }
        std::swap(coordinates[axis0],coordinates[axis1]);
        std::size_t source=0;
        for(std::size_t axis=0;axis<input_shape.size();++axis)
            source+=coordinates[axis]*static_cast<std::size_t>(input_strides[axis]);
        output[linear]=values[source];
    }
    return AutogradBuffer(std::move(output));
}

std::shared_ptr<AutogradNode> autograd_symbolic_transpose(
    const std::shared_ptr<AutogradNode>& input,
    std::size_t axis0,std::size_t axis1,
    unsigned long long line,unsigned long long column) {
    if(!input) autograd_fail("null higher-order transpose operand",line,column);
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if(axis0>=input->shape.size()||axis1>=input->shape.size())
        autograd_fail("higher-order transpose axis is outside rank",line,column);
    auto node=std::make_shared<AutogradNode>(input->dtype);
    node->shape=input->shape;
    std::swap(node->shape[axis0],node->shape[axis1]);
    node->parents={input};
    node->op=AutogradOp::Transpose;
    node->aux_index={axis0,axis1};
    if(input->device_tensor){
        auto* source=input->device_tensor;
        if(source->storage->owners==std::numeric_limits<std::size_t>::max())
            runtime_text_failure("tensor storage owner overflow");
        ++source->storage->owners;
        quidra::runtime::NodeRuntimeOwner partial_retained_5(source->storage, +[](void* value) {
            tensor_storage_release(static_cast<TensorStorage*>(value));
        });
        auto strides=source->strides;
        std::swap(strides[axis0],strides[axis1]);
        node->device_tensor=tensor_descriptor(
            source->storage,node->shape,std::move(strides),source->offset);
        partial_retained_5.relinquish();
    }else if(input->dtype==QCORE_DTYPE_FLOAT32){
        node->data=autograd_transpose_values<float>(
            input->data,input->shape,axis0,axis1,line,column);
    }else{
        node->data=autograd_transpose_values<double>(
            input->data,input->shape,axis0,axis1,line,column);
    }
    node->note_saved_value();
    return node;
}

std::shared_ptr<AutogradNode> autograd_symbolic_gather(
    const std::shared_ptr<AutogradNode>& input,
    const std::vector<long long>& output_shape,
    const std::vector<std::size_t>& indices,
    unsigned long long line,unsigned long long column) {
    if(!input) autograd_fail("null higher-order gather operand",line,column);
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if(tensor_element_count(output_shape,line,column)!=indices.size())
        autograd_fail("tensor gather output shape does not match index count",line,column);
    if(input->device_tensor){
        if(input->dtype!=QCORE_DTYPE_FLOAT64&&input->dtype!=QCORE_DTYPE_FLOAT32)
            autograd_fail("tensor gather autograd requires a floating dtype",line,column);
        const auto source_count=autograd_node_count(*input);
        for(const auto index:indices)
            if(index>=source_count)
                autograd_fail("tensor gather index is out of bounds for the input",line,column);
        auto source=autograd_device_view(*input);
        auto* value=tensor_gather_logical_indices(
            source,indices,output_shape,line,column);
        auto node=autograd_device_symbolic_node_safe(
            input->dtype,output_shape,AutogradOp::Gather,{input},value);
        node->aux_index=indices;
        return node;
    }
    auto node=std::make_shared<AutogradNode>(input->dtype);
    node->shape=output_shape;
    node->parents={input};
    node->op=AutogradOp::Gather;
    node->aux_index=indices;
    if(input->dtype==QCORE_DTYPE_FLOAT32)
        node->data=AutogradBuffer(autograd_gather_values<float>(
            input->data.typed<float>(),indices,line,column));
    else if(input->dtype==QCORE_DTYPE_FLOAT64)
        node->data=AutogradBuffer(autograd_gather_values<double>(
            input->data.typed<double>(),indices,line,column));
    else
        autograd_fail("tensor gather autograd requires a floating dtype",line,column);
    node->note_saved_value();
    return node;
}

std::shared_ptr<AutogradNode> autograd_symbolic_gather_backward(
    const std::shared_ptr<AutogradNode>& gradient,
    const std::vector<long long>& input_shape,
    const std::vector<std::size_t>& indices,
    unsigned long long line,unsigned long long column) {
    if(!gradient) autograd_fail("null higher-order gather operand",line,column);
    const auto source_count=tensor_element_count(input_shape,line,column);
    if(gradient->device_tensor){
        if(gradient->dtype!=QCORE_DTYPE_FLOAT64&&gradient->dtype!=QCORE_DTYPE_FLOAT32)
            autograd_fail("tensor gather autograd requires a floating dtype",line,column);
        quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
        if(autograd_node_count(*gradient)!=indices.size())
            autograd_fail("tensor gather backward size mismatch",line,column);
        std::vector<std::uint64_t> device_indices(indices.size());
        for(std::size_t i=0;i<indices.size();++i){
            if(indices[i]>=source_count)
                autograd_fail("tensor gather backward index is out of bounds for the input",line,column);
            device_indices[i]=static_cast<std::uint64_t>(indices[i]);
        }
        // Serial ascending accumulation from +0.0 per source element, the
        // same device kernel as the first-order GPU gather backward.
        auto* dense=autograd_device_dense_value(
            *gradient->device_tensor,line,column);
        quidra::runtime::NodeRuntimeOwner partial_dense(dense,+[](void* raw){release_managed_tensor(raw);});
        auto* storage=tensor_storage_create(
            gradient->dtype,source_count,abi::tensor_fill_mode::zeros,dense->storage->device,line,column);
        auto* value=tensor_descriptor(
            storage,input_shape,tensor_contiguous_strides(input_shape),0);
        quidra::runtime::NodeRuntimeOwner partial_value(value,+[](void* raw){release_managed_tensor(raw);});
        std::string backend_error;
        const bool ok=quidra::device::compute_gather_backward(
            storage->gpu_buffer,dense->storage->gpu_buffer,gradient->dtype,
            device_indices.data(),source_count,device_indices.size(),
            backend_error);
        partial_dense.relinquish();release_managed_tensor(dense);
        if(!ok){
            partial_value.relinquish();release_managed_tensor(value);
            autograd_fail(backend_error.c_str(),line,column);
        }
        partial_value.relinquish();
        auto node=autograd_device_symbolic_node_safe(
            gradient->dtype,input_shape,AutogradOp::GatherBackward,
            {gradient},value);
        node->aux_index=indices;
        return node;
    }
    auto node=std::make_shared<AutogradNode>(gradient->dtype);
    node->shape=input_shape;
    node->parents={gradient};
    node->op=AutogradOp::GatherBackward;
    node->aux_index=indices;
    if(gradient->dtype==QCORE_DTYPE_FLOAT32)
        node->data=AutogradBuffer(autograd_gather_backward_values<float>(
            gradient->data.typed<float>(),source_count,indices,line,column));
    else if(gradient->dtype==QCORE_DTYPE_FLOAT64)
        node->data=AutogradBuffer(autograd_gather_backward_values<double>(
            gradient->data.typed<double>(),source_count,indices,line,column));
    else
        autograd_fail("tensor gather autograd requires a floating dtype",line,column);
    node->note_saved_value();
    return node;
}

// Logical source index of every output element of a broadcast from
// `source_shape` to `output_shape` (identical ranks), the gather indices a
// Broadcast node stands for.
std::vector<std::size_t> autograd_broadcast_indices(
    const std::vector<long long>& source_shape,
    const std::vector<long long>& output_shape,
    unsigned long long line,unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeBroadcastValidations);
    if(source_shape.size()!=output_shape.size())
        autograd_fail("invalid tensor broadcast graph",line,column);
    const auto count=tensor_element_count(output_shape,line,column);
    const auto source_strides=tensor_contiguous_strides(source_shape);
    std::vector<std::size_t> indices(count);
    for(std::size_t logical=0;logical<count;++logical){
        auto remaining=logical;
        std::size_t source=0;
        for(std::size_t axis=output_shape.size();axis-->0;){
            const auto extent=static_cast<std::size_t>(output_shape[axis]);
            const auto coordinate=extent==0?std::size_t{0}:remaining%extent;
            if(extent!=0) remaining/=extent;
            if(source_shape[axis]!=1)
                source+=coordinate*static_cast<std::size_t>(source_strides[axis]);
        }
        indices[logical]=source;
    }
    return indices;
}

void autograd_topological(const std::shared_ptr<AutogradNode>& node,
                        std::unordered_set<const AutogradNode*>& seen,
                        std::vector<std::shared_ptr<AutogradNode>>& order,bool* cast_graph=nullptr);

// A tracked tensor for a symbolic node, as handed to a package's tracked
// backward callback. The native ABI carries no strides or offsets, so a GPU
// value is dense at offset 0, like the host-backed CPU values; the tensor's
// graph is the node itself, so the package can attach its second-order node
// to it on the node's own device.
TensorValue* autograd_tracked_callback_tensor(
    const std::shared_ptr<AutogradNode>& node,
    unsigned long long line,unsigned long long column) {
    TensorValue* value=node->device_tensor
        ? autograd_device_dense_value(*node->device_tensor,line,column)
        : autograd_tensor_from_node(*node);
    quidra::runtime::NodeRuntimeOwner partial(value,+[](void* raw){release_managed_tensor(raw);});
    value->graph=node;
    partial.relinquish();
    return value;
}

// The tracked gradient a target receives from its symbolic node. CPU nodes
// hold dense host values, so a CPU x.grad is always contiguous. A GPU node can
// hold a strided view (a Transpose node), and storing that view would make
// x.grad non-contiguous on gpu(n) only: reshape() of it would fail where the
// CPU succeeds, and contiguous() is rejected for tracked tensors. A strided
// GPU value is therefore stored as a dense copy of its logical elements (one
// device copy, no arithmetic); its graph is still the symbolic node.
TensorValue* autograd_tracked_target_tensor(
    const AutogradNode& node,unsigned long long line,unsigned long long column) {
    if(node.device_tensor&&!tensor_is_contiguous_value(*node.device_tensor))
        return autograd_device_dense_value(*node.device_tensor,line,column);
    return autograd_tensor_from_node(node);
}

std::vector<TensorValue*> autograd_custom_backward_tracked_tensors(
    const AutogradNode& node,
    const std::shared_ptr<AutogradNode>& gradient,
    unsigned long long line,unsigned long long column) {
    if(node.op!=AutogradOp::CustomNative || !node.custom_backward_tracked ||
       node.parents.empty())
        autograd_fail("invalid tracked custom native autograd node",line,column);
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if(!gradient || gradient->dtype!=node.dtype || gradient->shape!=node.shape)
        autograd_fail("tracked custom native gradient output does not match forward output",line,column);

    auto* gradient_tensor=autograd_tracked_callback_tensor(gradient,line,column);
    quidra::runtime::NodeRuntimeOwner partial_gradient(gradient_tensor,+[](void* raw){release_managed_tensor(raw);});
    std::vector<TensorValue*> inputs;
    std::vector<TensorValue*> outputs;
    const auto drop_tensors=+[](void* raw) {
        auto& values=*static_cast<std::vector<TensorValue*>*>(raw);
        for(auto*& slot:values) {
            auto* value=std::exchange(slot,nullptr);
            if(value)quidra::runtime::cleanup_node_slot(
                +[](void* owned){release_managed_tensor(owned);},value);
        }
    };
    quidra::runtime::NodeRuntimeOwner partial_inputs(&inputs,drop_tensors);
    quidra::runtime::NodeRuntimeOwner partial_outputs(&outputs,drop_tensors);
    inputs.reserve(node.parents.size());
    outputs.reserve(node.parents.size());
    try {
        for(const auto& parent:node.parents){
            if(!parent || parent->dtype!=node.dtype)
                autograd_fail("invalid tracked custom native autograd parent",line,column);
            inputs.push_back(autograd_tracked_callback_tensor(parent,line,column));

            const auto count=autograd_node_count(*parent);
            const int device=parent->device_tensor
                ? parent->device_tensor->storage->device : -1;
            auto* storage=tensor_storage_create(
                node.dtype,count,abi::tensor_fill_mode::zeros,device,line,column);
            outputs.push_back(tensor_descriptor(
                storage,parent->shape,tensor_contiguous_strides(parent->shape),0));
        }

        std::vector<const void*> input_borrows;
        std::vector<const void*> saved;
        std::vector<void*> writable;
        input_borrows.reserve(inputs.size());
        saved.reserve(node.custom_saved.size());
        writable.reserve(outputs.size());
        for(const auto* value:inputs) input_borrows.push_back(value);
        for(const auto* value:node.custom_saved){
            if(!value || !value->storage)
                autograd_fail("invalid tracked custom native saved tensor",line,column);
            saved.push_back(value);
        }
        for(auto* value:outputs) writable.push_back(value);

        int status=-1;
        {
            const PackageBackwardScope package_call(line,column);
            try {
                status=node.custom_backward_tracked(
                    input_borrows.data(),static_cast<std::uint64_t>(input_borrows.size()),
                    saved.empty()?nullptr:saved.data(),
                    static_cast<std::uint64_t>(saved.size()),
                    gradient_tensor,writable.data(),
                    static_cast<std::uint64_t>(writable.size()),
                    node.custom_metadata.empty()?nullptr:node.custom_metadata.data(),
                    static_cast<std::uint64_t>(node.custom_metadata.size()));
            } catch (...) {
                status=-1;
            }
            package_call.returned();
        }
        if(status!=0){
            const auto message=
                "tracked custom native autograd backward failed with status " +
                std::to_string(status);
            autograd_fail(message.c_str(),line,column);
        }
        for(const auto* value:outputs){
            if(!value || !value->graph)
                autograd_fail(
                    "tracked custom native backward returned a gradient without autograd provenance",
                    line,column);
        }

        for(auto*& value:inputs) release_managed_tensor(std::exchange(value,nullptr));
        partial_gradient.relinquish();if(gradient_tensor) release_managed_tensor(std::exchange(gradient_tensor,nullptr));
        partial_outputs.relinquish();
        return outputs;
    } catch (...) {
        for(auto*& value:inputs) release_managed_tensor(std::exchange(value,nullptr));
        for(auto*& value:outputs) release_managed_tensor(std::exchange(value,nullptr));
        partial_gradient.relinquish();if(gradient_tensor) release_managed_tensor(std::exchange(gradient_tensor,nullptr));
        throw;
    }
}


void autograd_add_symbolic_gradient(
    std::unordered_map<const AutogradNode*,std::shared_ptr<AutogradNode>>& gradients,
    const std::shared_ptr<AutogradNode>& target,
    std::shared_ptr<AutogradNode> value,
    unsigned long long line,unsigned long long column) {
    auto& current=gradients[target.get()];
    if(!current) current=std::move(value);
    else current=autograd_symbolic_binary(current,value,abi::tensor_binary_opcode::add,line,column);
}

// The higher-order seed d loss / d loss = 1. It keeps the graph shape the
// seed has always had, loss * 0 + 1, so every leaf the loss reaches still
// receives its (zero) higher-order contribution through the seed, but the
// two nodes hold exactly 0 and 1 instead of values computed from the loss.
// Computing them made the seed inf * 0 = NaN for a non-finite loss and
// poisoned every gradient. For a finite loss the seed is 1 as
// before; the 0 node's value (formerly loss * 0, possibly -0) is never read
// by any backward formula, so every finite result keeps its bits.
std::shared_ptr<AutogradNode> autograd_symbolic_seed(
    const std::shared_ptr<AutogradNode>& loss,
    unsigned long long line,unsigned long long column) {
    if(!loss) autograd_fail("null loss tensor",line,column);
    if(loss->dtype!=QCORE_DTYPE_FLOAT64&&loss->dtype!=QCORE_DTYPE_FLOAT32)
        autograd_fail("invalid higher-order gradient dtype",line,column);
    const auto count=autograd_node_count(*loss);
    const auto exact=[&](const std::shared_ptr<AutogradNode>& input,
                         int operation,double scalar,bool ones){
        auto node=std::make_shared<AutogradNode>(loss->dtype);
        node->shape=loss->shape;
        node->parents={input};
        node->op=AutogradOp::ScalarBinary;
        node->aux.assign(1,scalar);
        node->aux_index={static_cast<std::size_t>(operation),std::size_t{0}};
        if(loss->device_tensor){
            auto* storage=tensor_storage_create(
                loss->dtype,count,ones?abi::tensor_fill_mode::ones:abi::tensor_fill_mode::zeros,
                loss->device_tensor->storage->device,line,column);
            node->device_tensor=tensor_descriptor(
                storage,loss->shape,tensor_contiguous_strides(loss->shape),0);
        }else{
            node->data.assign(count,ones?1.0:0.0);
        }
        node->note_saved_value();
        return node;
    };
    return exact(exact(loss,3,0.0,false),1,1.0,true);
}

// The derivative of an explicit floating cast is a cast to the input dtype.
// Keep that cast as a graph edge when constructing higher-order gradients.
TensorValue* autograd_cast_gradient(
    TensorValue* input,int dtype,unsigned long long line,unsigned long long column) {
    // The ordinary recoverable conversion completes device range validation
    // before publishing the gradient. Backward has no error-valued result.
    auto* converted=static_cast<TensorValue*>(quidra_tensor_try_cast(input,dtype,line,column));
    if(!converted){
        end_package_hold_before_failure(line,column);
        quidra::runtime::report_conversion(quidra::runtime::last_conversion_reason(),
            abi::conversion_type_name(dtype),abi::ConversionSubject::tensor_element,line,column);
    }
    return converted;
}

std::shared_ptr<AutogradNode> autograd_symbolic_cast(
    const std::shared_ptr<AutogradNode>& input,int dtype,
    unsigned long long line,unsigned long long column) {
    if(!input||!autograd_dtype(input->dtype)||!autograd_dtype(dtype))
        autograd_fail("invalid autograd cast graph",line,column);
    if(input->device_tensor){
        auto* converted=autograd_cast_gradient(input->device_tensor,dtype,line,column);
        return autograd_device_symbolic_node_safe(
            dtype,input->shape,AutogradOp::Cast,{input},converted);
    }
    auto* value=autograd_tensor_from_values(input->dtype,input->shape,input->data);
    quidra::runtime::NodeRuntimeOwner partial_value(value,+[](void* raw){release_managed_tensor(raw);});
    auto* converted=autograd_cast_gradient(value,dtype,line,column);
    quidra::runtime::NodeRuntimeOwner partial_converted(converted,+[](void* raw){release_managed_tensor(raw);});
    partial_value.relinquish();release_managed_tensor(value);
    auto node=std::make_shared<AutogradNode>(dtype);
    node->shape=input->shape;node->op=AutogradOp::Cast;node->parents={input};
    node->data=tensor_float_values(*converted,line,column);
    partial_converted.relinquish();release_managed_tensor(converted);
    node->note_saved_value();
    return node;
}

void autograd_validate_cast_graph(
    const std::vector<std::shared_ptr<AutogradNode>>& order,bool higher_order,
    unsigned long long line,unsigned long long column) {
    for(const auto& node:order){
        if(!autograd_dtype(node->dtype))
            autograd_fail("invalid autograd floating dtype",line,column);
        if(higher_order&&autograd_half_dtype(node->dtype))
            autograd_fail("backward(track = true) is not available for real16 and real16b tensors",
                          line,column);
        if(node->op==AutogradOp::Cast){
            quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
            if(node->parents.size()!=1||!autograd_dtype(node->parents[0]->dtype)||
               node->shape!=node->parents[0]->shape)
                autograd_fail("invalid autograd cast graph",line,column);
        }else{
            for(const auto& parent:node->parents)
                if(parent->dtype!=node->dtype)
                    autograd_fail("autograd graph contains mixed dtypes outside a floating cast",line,column);
        }
    }
}

void autograd_backward_tracked(
    const std::shared_ptr<AutogradNode>& loss,
    const std::vector<std::shared_ptr<AutogradSlot>>& selected,
    unsigned long long line,unsigned long long column) {
    std::unordered_set<const AutogradNode*> seen;
    std::vector<std::shared_ptr<AutogradNode>> order;
    bool cast_graph=false;
    autograd_topological(loss,seen,order,&cast_graph);
    if(cast_graph) autograd_validate_cast_graph(order,true,line,column);

    // Graphs never mix devices (tracked transfers are rejected and every
    // tracked operation requires same-device operands), so every symbolic
    // gradient node below lives on the loss's device: host-backed on the CPU,
    // a device TensorValue on gpu(n).
    std::unordered_map<const AutogradNode*,std::shared_ptr<AutogradNode>> gradients;
    gradients.reserve(order.size());
    gradients.emplace(loss.get(),autograd_symbolic_seed(loss,line,column));

    for(auto it=order.rbegin();it!=order.rend();++it){
        const auto& node=*it;
        const auto found=gradients.find(node.get());
        if(found==gradients.end()) continue;
        const auto& g=found->second;

        if(node->target_identity){
            for(const auto& slot:selected){
                if(!slot || slot->identity!=node->target_identity) continue;
                auto* value=autograd_tracked_target_tensor(*g,line,column);
                quidra::runtime::NodeRuntimeOwner partial(value,+[](void* raw){release_managed_tensor(raw);});
                value->graph=g;
                partial.relinquish();
                autograd_accumulate_slot(slot,value,line,column,true);
            }
        }
        if(node->parents.empty()) continue;

        if(node->op==AutogradOp::Cast){
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_symbolic_cast(g,node->parents[0]->dtype,line,column),line,column);
        }else if(node->op==AutogradOp::Add){
            autograd_add_symbolic_gradient(gradients,node->parents[0],g,line,column);
            autograd_add_symbolic_gradient(gradients,node->parents[1],g,line,column);
        }else if(node->op==AutogradOp::Sub){
            autograd_add_symbolic_gradient(gradients,node->parents[0],g,line,column);
            autograd_add_symbolic_gradient(
                gradients,node->parents[1],
                autograd_symbolic_scalar(g,-1.0,abi::tensor_binary_opcode::multiply,false,line,column),
                line,column);
        }else if(node->op==AutogradOp::Mul){
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_symbolic_binary(g,node->parents[1],abi::tensor_binary_opcode::multiply,line,column),
                line,column);
            autograd_add_symbolic_gradient(
                gradients,node->parents[1],
                autograd_symbolic_binary(g,node->parents[0],abi::tensor_binary_opcode::multiply,line,column),
                line,column);
        }else if(node->op==AutogradOp::Div){
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_symbolic_binary(g,node->parents[1],abi::tensor_binary_opcode::divide,line,column),
                line,column);
            auto numerator=autograd_symbolic_binary(g,node->parents[0],abi::tensor_binary_opcode::multiply,line,column);
            auto denominator=autograd_symbolic_binary(
                node->parents[1],node->parents[1],abi::tensor_binary_opcode::multiply,line,column);
            auto right=autograd_symbolic_binary(numerator,denominator,abi::tensor_binary_opcode::divide,line,column);
            right=autograd_symbolic_scalar(right,-1.0,abi::tensor_binary_opcode::multiply,false,line,column);
            autograd_add_symbolic_gradient(
                gradients,node->parents[1],std::move(right),line,column);
        }else if(node->op==AutogradOp::ScalarBinary){
            const auto operation=static_cast<int>(node->aux_index[0]);
            const bool scalar_left=node->aux_index[1]!=0;
            const double scalar=node->aux.scalar_as_double(0);
            std::shared_ptr<AutogradNode> result;
            if(operation==abi::tensor_binary_opcode::add || (operation==abi::tensor_binary_opcode::subtract&&!scalar_left)) result=g;
            else if(operation==abi::tensor_binary_opcode::subtract) result=autograd_symbolic_scalar(
                g,-1.0,abi::tensor_binary_opcode::multiply,false,line,column);
            else if(operation==abi::tensor_binary_opcode::multiply) result=autograd_symbolic_scalar(
                g,scalar,abi::tensor_binary_opcode::multiply,false,line,column);
            else if(operation==abi::tensor_binary_opcode::power){
                if(scalar_left)
                    autograd_fail("tensor power requires tensor ^ scalar",line,column);
                auto coefficient=autograd_symbolic_scalar(
                    g,scalar,abi::tensor_binary_opcode::multiply,false,line,column);
                auto power=autograd_symbolic_scalar(
                    node->parents[0],scalar-1.0,abi::tensor_binary_opcode::power,false,line,column);
                result=autograd_symbolic_binary(
                    coefficient,power,abi::tensor_binary_opcode::multiply,line,column);
            }else if(!scalar_left) result=autograd_symbolic_scalar(
                g,scalar,abi::tensor_binary_opcode::divide,false,line,column);
            else{
                auto numerator=autograd_symbolic_scalar(g,-scalar,abi::tensor_binary_opcode::multiply,false,line,column);
                auto square=autograd_symbolic_binary(
                    node->parents[0],node->parents[0],abi::tensor_binary_opcode::multiply,line,column);
                result=autograd_symbolic_binary(numerator,square,abi::tensor_binary_opcode::divide,line,column);
            }
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],std::move(result),line,column);
        }else if(node->op==AutogradOp::Reshape){
            if(node->parents.size()!=1)
                autograd_fail("invalid tensor reshape graph",line,column);
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_symbolic_reshape(g,node->parents[0]->shape,line,column),
                line,column);
        }else if(node->op==AutogradOp::Transpose){
            if(node->parents.size()!=1||node->aux_index.size()!=2)
                autograd_fail("invalid tensor transpose graph",line,column);
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_symbolic_transpose(
                    g,node->aux_index[0],node->aux_index[1],line,column),
                line,column);
        }else if(node->op==AutogradOp::Gather){
            if(node->parents.size()!=1)
                autograd_fail("invalid tensor gather graph",line,column);
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_symbolic_gather_backward(
                    g,node->parents[0]->shape,node->aux_index,line,column),
                line,column);
        }else if(node->op==AutogradOp::Broadcast){
            // Same derivative as the Gather of broadcast indices it replaces.
            if(node->parents.size()!=1)
                autograd_fail("invalid tensor broadcast graph",line,column);
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_symbolic_gather_backward(
                    g,node->parents[0]->shape,
                    autograd_broadcast_indices(
                        node->parents[0]->shape,node->shape,line,column),
                    line,column),
                line,column);
        }else if(node->op==AutogradOp::GatherBackward){
            if(node->parents.size()!=1)
                autograd_fail("invalid tensor gather backward graph",line,column);
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_symbolic_gather(
                    g,node->parents[0]->shape,node->aux_index,line,column),
                line,column);
        }else if(node->op==AutogradOp::Neg){
            if(node->parents.size()!=1)
                autograd_fail("invalid tensor negation graph",line,column);
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_symbolic_neg(g,line,column),line,column);
        }else if(node->op==AutogradOp::CustomNative){
            if(!node->custom_backward_tracked)
                autograd_fail(
                    "custom native autograd operation does not provide tracked backward",
                    line,column);
            auto input_gradients=autograd_custom_backward_tracked_tensors(
                *node,g,line,column);
            quidra::runtime::NodeRuntimeOwner partial_tracked_inputs(&input_gradients,cleanup_node_tensor_vector);
            for(std::size_t index=0;index<input_gradients.size();++index){
                auto gradient_node=input_gradients[index]->graph;
                release_managed_tensor(std::exchange(input_gradients[index],nullptr));
                autograd_add_symbolic_gradient(
                    gradients,node->parents[index],std::move(gradient_node),
                    line,column);
            }
        }else{
            autograd_fail("unsupported higher-order autograd node",line,column);
        }
    }
}

template <typename T>
void autograd_add_gradient(std::unordered_map<const AutogradNode*,HostValues<T>>& gradients,
                         const std::shared_ptr<AutogradNode>& node,HostValues<T> value) {
    // A backward operation's result for a real16 or real16b node is rounded
    // once to its type, and so is each sum of two contributions.
    const bool half=std::is_same_v<T,float>&&autograd_half_dtype(node->dtype);
    const auto round=[&]([[maybe_unused]] HostValues<T>& values){
        if constexpr(std::is_same_v<T,float>){
            const auto count=values.size();
            if(count==0) return;
            auto* data=values.mutable_data();
            for(std::size_t i=0;i<count;++i) data[i]=autograd_round_half(data[i],node->dtype);
        }
    };
    if(half) round(value);
    auto& current=gradients[node.get()];
    if(current.empty()) current=std::move(value);
    else{
        quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
        if(current.size()!=value.size()) runtime_text_failure("autograd gradient size mismatch");
        const auto count=current.size();
        auto* out=current.mutable_data();
        const auto* in=value.data();
        for(std::size_t i=0;i<count;++i)
            out[i]=static_cast<T>(out[i]+in[i]);
        if(half) round(current);
    }
}

void autograd_topological(const std::shared_ptr<AutogradNode>& node,
                        std::unordered_set<const AutogradNode*>& seen,
                        std::vector<std::shared_ptr<AutogradNode>>& order,bool* cast_graph) {
    if(!node || !seen.insert(node.get()).second) return;
    struct Frame {
        std::shared_ptr<AutogradNode> node;
        std::size_t next_parent{};
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{node,0});
    while(!stack.empty()){
        auto& frame=stack.back();
        if(frame.next_parent<frame.node->parents.size()){
            auto parent=frame.node->parents[frame.next_parent++];
            if(parent && seen.insert(parent.get()).second)
                stack.push_back(Frame{std::move(parent),0});
            continue;
        }
        order.push_back(frame.node);
        if(cast_graph&&frame.node->op==AutogradOp::Cast) *cast_graph=true;
        stack.pop_back();
    }
}


// Saved state is a forward-time value snapshot, not an additional graph edge.
// A saved tensor that densely covers its whole storage (contiguous, offset
// 0, every storage element) is retained by reference count: every writer of
// a shared storage detaches first (tensor_detach_for_write behind eager
// writes and the mutable native accessors), so the snapshot keeps its
// forward-time values without a copy. Other views are materialized into
// independent contiguous storage on the same device, because the native ABI
// carries no strides. The retention follows autograd_storage_sharing_enabled()
// (the default; QUIDRA_SAVED_TENSORS=copy materializes every save). A
// mutable pointer a package obtained before saving is not detached, so the
// native ABI forbids writing a saved tensor through one after the save.
TensorValue* autograd_saved_snapshot(const TensorValue& source) {
    if(autograd_storage_sharing_enabled() && source.offset==0 &&
       tensor_is_contiguous_value(source) &&
       tensor_logical_count(source)==source.storage->count){
        if(source.storage->owners==std::numeric_limits<std::size_t>::max())
            runtime_text_failure("tensor storage owner overflow");
        ++source.storage->owners;
        quidra::runtime::NodeRuntimeOwner partial_retained_6(source.storage, +[](void* value) {
            tensor_storage_release(static_cast<TensorStorage*>(value));
        });
        return [&] { auto* complete=tensor_descriptor(
            source.storage,source.shape,source.strides,0); partial_retained_6.relinquish(); return complete; }();
    }
    auto* storage=tensor_transfer_storage(
        source,source.storage->device,0,0);
    return tensor_descriptor(
        storage,source.shape,tensor_contiguous_strides(source.shape),0);
}

} // namespace

namespace {

// Shared by the full-request and the masked attachment: exactly one of
// `backward` and `backward_masked` is set, and `full_writes` (input_count
// bytes, optional) only with `backward_masked`.
int attach_custom_autograd(
    void* output_raw,
    const void* const* input_raws,
    std::uint64_t input_count,
    const void* const* saved_raws,
    std::uint64_t saved_count,
    qcore_autograd_backward_fn backward,
    qcore_autograd_backward_masked_fn backward_masked,
    qcore_autograd_backward_tracked_fn backward_tracked,
    const std::uint8_t* full_writes,
    const void* metadata,
    std::uint64_t metadata_size) {
    try {
        if(!output_raw || !input_raws || input_count==0 ||
           (!backward && !backward_masked))
            return -1;
        if(saved_count!=0 && !saved_raws) return -1;
        if(metadata_size!=0 && !metadata) return -1;

        auto* output=static_cast<TensorValue*>(output_raw);
        if(!output->storage || output->graph) return -2;
        if(!autograd_dtype(output->storage->dtype))
            return -3;
        // Saving tensors may copy them on their devices, which Core does
        // not do inside the calling thread's package encoder scope on that
        // device, nor on another device while the thread holds a stream
        // (native_extension.h): attach after qcore_metal_note_work.
        if(quidra::device::package_hold_active()){
            std::vector<const TensorValue*> involved{output};
            for(std::uint64_t index=0;index<input_count;++index)
                involved.push_back(static_cast<const TensorValue*>(input_raws[index]));
            for(std::uint64_t index=0;index<saved_count;++index)
                involved.push_back(static_cast<const TensorValue*>(saved_raws[index]));
            for(const auto* tensor:involved){
                if(!tensor || !tensor->storage || tensor_on_cpu(*tensor->storage))
                    continue;
                const int device=tensor->storage->device;
                if(quidra::device::package_hold_refuses_device(
                       device,"a custom autograd attach"))
                    return -6;
                if(quidra::device::package_scope_open(device)){
                    quidra::device::note_package_scope_violation(
                        device,
                        "a custom autograd attach (saving tensors may copy them)");
                    return -6;
                }
            }
        }

        bool tracked=false;
        for(std::uint64_t index=0;index<input_count;++index){
            if(!input_raws[index]) return -1;
            const auto* input=static_cast<const TensorValue*>(input_raws[index]);
            quidra::counters::add(quidra::counters::Id::RuntimeDeviceChecks);
            if(!input->storage ||
               input->storage->dtype!=output->storage->dtype ||
               input->storage->device!=output->storage->device)
                return -4;
            tracked=tracked || static_cast<bool>(input->graph);
        }
        if(!tracked) return 0;

        auto node=std::make_shared<AutogradNode>(output->storage->dtype);
        node->shape=output->shape;
        node->op=AutogradOp::CustomNative;
        node->custom_backward=backward;
        node->custom_backward_masked=backward_masked;
        node->custom_backward_tracked=backward_tracked;
        node->parents.reserve(static_cast<std::size_t>(input_count));
        node->custom_saved.reserve(static_cast<std::size_t>(saved_count));

        for(std::uint64_t index=0;index<input_count;++index){
            auto* input=const_cast<TensorValue*>(
                static_cast<const TensorValue*>(input_raws[index]));
            node->parents.push_back(
                input->graph ? input->graph : autograd_constant_node(*input,0,0));
        }
        for(std::uint64_t index=0;index<saved_count;++index){
            if(!saved_raws[index]) return -5;
            const auto* saved_source=
                static_cast<const TensorValue*>(saved_raws[index]);
            if(!saved_source->storage) return -5;
            node->custom_saved.push_back(autograd_saved_snapshot(*saved_source));
            quidra::counters::add(quidra::counters::Id::AutogradSavedTensors);
        }
        if(metadata_size!=0){
            const auto* bytes=static_cast<const unsigned char*>(metadata);
            node->custom_metadata.assign(bytes,bytes+metadata_size);
        }
        if(full_writes){
            bool any=false;
            for(std::uint64_t index=0;index<input_count;++index)
                any=any || full_writes[index]!=0;
            if(any){
                node->custom_full_writes.resize(static_cast<std::size_t>(input_count));
                for(std::uint64_t index=0;index<input_count;++index)
                    node->custom_full_writes[static_cast<std::size_t>(index)]=
                        full_writes[index]!=0 ? 1 : 0;
            }
        }

        if(tensor_on_cpu(*output->storage))
            node->data=tensor_float_values(*output,0,0);
        else
            node->device_tensor=static_cast<TensorValue*>(
                quidra_tensor_clone(output));
        node->note_saved_value();
        output->graph=std::move(node);
        quidra::counters::add(quidra::counters::Id::AutogradCustomNodes);
        return 0;
    } catch (...) {
        return -9;
    }
}

} // namespace

extern "C" int qcore_tensor_attach_custom_autograd_with_saved_ex(
    void* output_raw,
    const void* const* input_raws,
    std::uint64_t input_count,
    const void* const* saved_raws,
    std::uint64_t saved_count,
    qcore_autograd_backward_fn backward,
    qcore_autograd_backward_tracked_fn backward_tracked,
    const void* metadata,
    std::uint64_t metadata_size) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if(!backward) return -1;
    return attach_custom_autograd(
        output_raw,input_raws,input_count,saved_raws,saved_count,
        backward,nullptr,backward_tracked,nullptr,metadata,metadata_size);
}

extern "C" int qcore_tensor_attach_custom_autograd_masked(
    void* output_raw,
    const void* const* input_raws,
    std::uint64_t input_count,
    const void* const* saved_raws,
    std::uint64_t saved_count,
    qcore_autograd_backward_masked_fn backward,
    qcore_autograd_backward_tracked_fn backward_tracked,
    const std::uint8_t* full_writes,
    const void* metadata,
    std::uint64_t metadata_size) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if(!backward) return -1;
    return attach_custom_autograd(
        output_raw,input_raws,input_count,saved_raws,saved_count,
        nullptr,backward,backward_tracked,full_writes,metadata,metadata_size);
}

extern "C" int qcore_tensor_attach_custom_autograd_with_saved(
    void* output_raw,
    const void* const* input_raws,
    std::uint64_t input_count,
    const void* const* saved_raws,
    std::uint64_t saved_count,
    qcore_autograd_backward_fn backward,
    const void* metadata,
    std::uint64_t metadata_size) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    return qcore_tensor_attach_custom_autograd_with_saved_ex(
        output_raw,input_raws,input_count,saved_raws,saved_count,
        backward,nullptr,metadata,metadata_size);
}

extern "C" int qcore_tensor_attach_custom_autograd_ex(
    void* output_raw,
    const void* const* input_raws,
    std::uint64_t input_count,
    qcore_autograd_backward_fn backward,
    qcore_autograd_backward_tracked_fn backward_tracked,
    const void* metadata,
    std::uint64_t metadata_size) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    return qcore_tensor_attach_custom_autograd_with_saved_ex(
        output_raw,input_raws,input_count,input_raws,input_count,
        backward,backward_tracked,metadata,metadata_size);
}

extern "C" int qcore_tensor_attach_custom_autograd(
    void* output_raw,
    const void* const* input_raws,
    std::uint64_t input_count,
    qcore_autograd_backward_fn backward,
    const void* metadata,
    std::uint64_t metadata_size) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    return qcore_tensor_attach_custom_autograd_ex(
        output_raw,input_raws,input_count,backward,nullptr,metadata,metadata_size);
}

extern "C" void* quidra_tensor_track(
    void* raw,unsigned long long line,unsigned long long column) {
    if(!raw) autograd_fail("null tensor",line,column);
    auto* source=static_cast<TensorValue*>(raw);
    if(!autograd_dtype(source->storage->dtype))
        autograd_fail("track() requires a tensor of real16, real16b, real32 or real64",line,column);
    auto* result=static_cast<TensorValue*>(quidra_tensor_clone(raw));
    if(result->graph) return result;
    if(!source->grad_slot){
        source->grad_slot=new_autograd_slot(source->storage->dtype);
    }else if(source->grad_slot->dtype==0){
        source->grad_slot->dtype=source->storage->dtype;
    }
    result->grad_slot=new_autograd_slot(
        source->storage->dtype,source->grad_slot->identity);
    result->graph=autograd_constant_node(*result,line,column);
    result->graph->target_identity=source->grad_slot->identity;
    return result;
}

extern "C" void* quidra_tensor_track_target(
    void* raw,void* target_raw,unsigned long long line,unsigned long long column) {
    if(!raw) autograd_fail("null tensor",line,column);
    auto* source=static_cast<TensorValue*>(raw);
    if(!autograd_dtype(source->storage->dtype))
        autograd_fail("track() requires a tensor of real16, real16b, real32 or real64",line,column);
    auto* handle=autograd_target_from_value(target_raw);
    if(!handle||!handle->slot) autograd_fail("invalid autograd target",line,column);
    if(handle->slot->dtype!=0&&handle->slot->dtype!=source->storage->dtype)
        autograd_fail("autograd target cannot mix tensor dtypes",line,column);
    handle->slot->dtype=source->storage->dtype;
    auto* result=static_cast<TensorValue*>(quidra_tensor_clone(raw));
    result->graph.reset();
    result->grad_slot=new_autograd_slot(
        source->storage->dtype,handle->slot->identity);
    result->graph=autograd_constant_node(*result,line,column);
    result->graph->target_identity=handle->slot->identity;
    return result;
}

extern "C" void* quidra_tensor_untrack(
    void* raw,unsigned long long line,unsigned long long column) {
    if(!raw) autograd_fail("null tensor",line,column);
    auto* result=static_cast<TensorValue*>(quidra_tensor_clone(raw));
    result->graph.reset();
    result->grad_slot.reset();
    return result;
}

extern "C" void* quidra_tensor_retrack(
    void* raw,unsigned long long line,unsigned long long column) {
    if(!raw) autograd_fail("null tensor",line,column);
    auto* source=static_cast<TensorValue*>(raw);
    if(!autograd_dtype(source->storage->dtype))
        autograd_fail("retrack() requires a tensor of real16, real16b, real32 or real64",line,column);
    auto* result=static_cast<TensorValue*>(quidra_tensor_clone(raw));
    result->graph.reset();
    result->grad_slot=new_autograd_slot(source->storage->dtype);
    result->graph=autograd_constant_node(*result,line,column);
    result->graph->target_identity=result->grad_slot->identity;
    return result;
}

extern "C" void* quidra_tensor_grad(
    void* raw,unsigned long long line,unsigned long long column) {
    if(!raw) autograd_fail("null tensor",line,column);
    auto* tensor=static_cast<TensorValue*>(raw);
    if(!tensor->grad_slot||!tensor->grad_slot->gradient)
        autograd_fail("tensor gradient is not available; call backward() first",line,column);
    return quidra_tensor_clone(tensor->grad_slot->gradient);
}

extern "C" bool quidra_tensor_is_tracked(void* raw) {
    if(!raw) return false;
    return static_cast<TensorValue*>(raw)->graph!=nullptr;
}

extern "C" bool quidra_tensor_has_grad(void* raw) {
    if(!raw) return false;
    const auto* tensor=static_cast<TensorValue*>(raw);
    return tensor->grad_slot && tensor->grad_slot->gradient;
}

extern "C" void quidra_tensor_clear_grad(
    void* raw,unsigned long long line,unsigned long long column) {
    if(!raw) autograd_fail("null tensor",line,column);
    auto* tensor=static_cast<TensorValue*>(raw);
    if(!tensor->grad_slot||!tensor->grad_slot->gradient) return;
    if(quidra::runtime::node_failure_active()) {
        auto* old=std::exchange(tensor->grad_slot->gradient,nullptr);
        release_managed_tensor(old);
    } else {
    release_managed_tensor(tensor->grad_slot->gradient);
    tensor->grad_slot->gradient=nullptr;
    }
}

TensorValue* tensor_gather_logical_indices(
    TensorValue& source,std::vector<std::size_t> logical_indices,
    std::vector<long long> output_shape,
    unsigned long long line,unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    const auto output_count=tensor_element_count(output_shape,line,column);
    if(logical_indices.size()!=output_count)
        tensor_fail(
            "tensor.gather index count must equal the output element count",
            line,column);
    const auto source_count=tensor_logical_count(source);
    std::vector<std::uint64_t> physical_indices;
    try{
        physical_indices.resize(output_count);
    }catch(...){
        runtime_allocation_failure();
    }
    // A dense source holds logical element i at storage index offset + i, so
    // the storage index needs no per-axis division.
    const bool dense_source=tensor_view_dense_in_storage(source,source_count);
    for(std::size_t i=0;i<output_count;++i){
        const auto index=logical_indices[i];
        if(index>=source_count)
            tensor_fail("tensor.gather index is out of bounds for the source tensor",line,column);
        const auto physical=dense_source
            ? source.offset+index : tensor_storage_index(source,index);
        if(physical>=source.storage->count)
            tensor_fail("tensor.gather source view exceeds storage",line,column);
        physical_indices[i]=static_cast<std::uint64_t>(physical);
    }

    auto* storage=tensor_storage_create(
        source.storage->dtype,output_count,abi::tensor_fill_mode::uninitialized,source.storage->device,line,column);
    const auto width=tensor_dtype_bytes(source.storage->dtype);
    // Every gathered element comes from the source storage, so a fully
    // initialized source makes the whole output initialized once the copy
    // succeeds. Partially initialized sources propagate per element.
    const bool source_complete=source.storage->initialization.fully_initialized;
    const auto propagate_initialization=[&]{
        if(source_complete){
            tracker_mark_complete(storage->initialization);
            return;
        }
        for(std::size_t i=0;i<output_count;++i)
            if(tracker_bit(
                    source.storage->initialization,
                    static_cast<std::size_t>(physical_indices[i])))
                tracker_set(storage->initialization,i);
    };
    if(tensor_on_cpu(*source.storage)){
        const auto copy_elements=[&](auto tag){
            using Element=decltype(tag);
            auto* output=storage->data.data();
            const auto* input=source.storage->data.data();
            for(std::size_t i=0;i<output_count;++i){
                const auto physical=static_cast<std::size_t>(physical_indices[i]);
                std::memcpy(output+i*sizeof(Element),
                            input+physical*sizeof(Element),sizeof(Element));
            }
        };
        switch(width){
            case 1: copy_elements(std::uint8_t{}); break;
            case 2: copy_elements(std::uint16_t{}); break;
            case 4: copy_elements(std::uint32_t{}); break;
            case 16: copy_elements(std::array<std::uint64_t,2>{}); break;
            default: copy_elements(std::uint64_t{}); break;
        }
        propagate_initialization();
    }else{
        std::string backend_error;
        if(!quidra::device::compute_gather(
                storage->gpu_buffer,source.storage->gpu_buffer,
                source.storage->dtype,physical_indices.data(),
                output_count,backend_error)){
            tensor_storage_release(storage);
            tensor_fail(backend_error.c_str(),line,column);
        }
        propagate_initialization();
    }

    auto* result=tensor_descriptor(
        storage,output_shape,tensor_contiguous_strides(output_shape),0);
    quidra::runtime::NodeRuntimeOwner partial_graph_result_4(result,+[](void* raw){release_managed_tensor(raw);});
    if(source.graph){
        if(!autograd_dtype(source.storage->dtype)){
            partial_graph_result_4.relinquish();
            release_managed_tensor(result);
            autograd_fail("tracked tensor gather requires a floating dtype",line,column);
        }
        auto node=std::make_shared<AutogradNode>(source.storage->dtype);
        node->shape=output_shape;
        node->parents={source.graph};
        node->op=AutogradOp::Gather;
        node->aux_index=std::move(logical_indices);
        if(tensor_on_cpu(*storage))
            node->data=tensor_float_values(*result,line,column);
        else
            node->device_tensor=static_cast<TensorValue*>(
                quidra_tensor_clone(result));
        node->note_saved_value();
        result->graph=std::move(node);
    }
    partial_graph_result_4.relinquish();
    return result;
}

// Logical row-major mask extraction. A GPU mask is transferred once for
// dynamic output-size discovery; the selected values stay on their original
// device and use the existing gather kernel/autograd backward graph.
extern "C" void* quidra_tensor_mask_select(
    void* source_raw,void* mask_raw,
    unsigned long long line,unsigned long long column) {
    if (!source_raw || !mask_raw)
        tensor_fail("tensor mask extraction received a null operand",line,column);
    auto& source=*static_cast<TensorValue*>(source_raw);
    auto& mask=*static_cast<TensorValue*>(mask_raw);
    if (!source.storage || !mask.storage)
        tensor_fail("tensor mask extraction requires initialized tensor descriptors",line,column);
    if (mask.storage->dtype != QCORE_DTYPE_BOOL)
        tensor_fail("tensor mask extraction requires tensor<bool>",line,column);
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if (source.shape != mask.shape)
        tensor_fail("tensor mask must match every input axis",line,column);
    tensor_require_initialized(mask,line,column);
    auto* host_mask=tensor_on_cpu(*mask.storage)
        ? nullptr : static_cast<TensorValue*>(
            quidra_tensor_to_cpu(mask_raw,line,column));
    quidra::runtime::NodeRuntimeOwner host_mask_owner(
        host_mask,+[](void* raw){release_managed_tensor(raw);});
    const auto& selector=host_mask ? *host_mask : mask;
    const auto count=tensor_logical_count(selector);
    std::vector<std::size_t> selected;
    try {
        selected.reserve(count);
    } catch (...) {
        runtime_allocation_failure();
    }
    for (std::size_t i=0;i<count;++i) {
        const auto position=tensor_storage_index(selector,i);
        if (position>=selector.storage->count)
            tensor_fail("tensor mask view exceeds its storage",line,column);
        if (selector.storage->data[position] != 0)
            selected.push_back(i);
    }
    if (selected.size() > static_cast<std::size_t>(std::numeric_limits<long long>::max()))
        tensor_fail("tensor mask result exceeds supported array length",line,column);
    if (host_mask) {
        host_mask_owner.relinquish();
        release_managed_tensor(host_mask);
    }
    const auto selected_length=static_cast<long long>(selected.size());
    return tensor_gather_logical_indices(
        source,std::move(selected),{selected_length},line,column);
}

extern "C" void* quidra_tensor_gather(
    void* raw,void* indices_array,void* shape_array,
    unsigned long long line,unsigned long long column) {
    if(!raw) tensor_fail("tensor.gather received a null tensor",line,column);
    auto* source=static_cast<TensorValue*>(raw);
    auto raw_indices=tensor_int_array_from_array(indices_array,line,column);
    auto output_shape=tensor_shape_from_array(shape_array,line,column);
    std::vector<std::size_t> logical_indices;
    try{
        logical_indices.reserve(raw_indices.size());
    }catch(...){
        runtime_allocation_failure();
    }
    for(const auto index:raw_indices){
        if(index<0)
            tensor_fail("tensor.gather index is out of bounds for the source tensor",line,column);
        logical_indices.push_back(static_cast<std::size_t>(index));
    }
    return tensor_gather_logical_indices(
        *source,std::move(logical_indices),std::move(output_shape),line,column);
}

extern "C" void* quidra_tensor_scatter(
    void* raw,void* indices_array,void* shape_array,
    unsigned long long line,unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if(!raw) tensor_fail("tensor.scatter received a null tensor",line,column);
    auto& source=*static_cast<TensorValue*>(raw);
    if(!abi::numeric_dtype_code(source.storage->dtype))
        tensor_fail(
            "tensor.scatter requires a numeric tensor element type",
            line,column);
    tensor_require_initialized(source,line,column);
    auto raw_indices=tensor_int_array_from_array(indices_array,line,column);
    auto output_shape=tensor_shape_from_array(shape_array,line,column);
    const auto source_count=tensor_logical_count(source);
    const auto output_count=tensor_element_count(output_shape,line,column);
    if(raw_indices.size()!=source_count)
        tensor_fail(
            "tensor.scatter index count must equal the source element count",
            line,column);

    std::vector<std::size_t> logical_indices;
    try{
        logical_indices.resize(source_count);
    }catch(...){
        runtime_allocation_failure();
    }
    for(std::size_t i=0;i<source_count;++i){
        if(raw_indices[i]<0)
            tensor_fail("tensor.scatter index is out of bounds for the output tensor",line,column);
        const auto index=static_cast<std::size_t>(raw_indices[i]);
        if(index>=output_count)
            tensor_fail("tensor.scatter index is out of bounds for the output tensor",line,column);
        logical_indices[i]=index;
    }

    auto* storage=tensor_storage_create(
        source.storage->dtype,output_count,abi::tensor_fill_mode::zeros,source.storage->device,line,column);
    if(tensor_on_cpu(*source.storage)){
        std::fill(storage->data.begin(), storage->data.end(), static_cast<unsigned char>(0));
        const auto scatter_typed=[&](auto tag){
            using T=decltype(tag);
            for(std::size_t i=0;i<source_count;++i){
                const auto physical=tensor_storage_index(source,i);
                T value{},current{},next{};
                std::memcpy(
                    &value,source.storage->data.data()+physical*sizeof(T),
                    sizeof(T));
                std::memcpy(
                    &current,storage->data.data()+logical_indices[i]*sizeof(T),
                    sizeof(T));
                bool valid=true;
                if constexpr(real_element<T>){
                    next=static_cast<T>(current+value);
                }else if constexpr(std::is_signed_v<T>){
                    if((value>0&&current>std::numeric_limits<T>::max()-value)||
                       (value<0&&current<std::numeric_limits<T>::min()-value))
                        valid=false;
                    else
                        next=static_cast<T>(current+value);
                }else{
                    if(current>std::numeric_limits<T>::max()-value)
                        valid=false;
                    else
                        next=static_cast<T>(current+value);
                }
                if(!valid){
                    tensor_storage_release(storage);
                    tensor_fail("tensor.scatter integer arithmetic overflow",line,column);
                }
                std::memcpy(
                    storage->data.data()+logical_indices[i]*sizeof(T),
                    &next,sizeof(T));
            }
        };
        switch(source.storage->dtype){
            case QCORE_DTYPE_INT64: scatter_typed(std::int64_t{}); break;
            case QCORE_DTYPE_INT8: scatter_typed(std::int8_t{}); break;
            case QCORE_DTYPE_INT16: scatter_typed(std::int16_t{}); break;
            case QCORE_DTYPE_INT32: scatter_typed(std::int32_t{}); break;
            case QCORE_DTYPE_UINT8: scatter_typed(std::uint8_t{}); break;
            case QCORE_DTYPE_UINT16: scatter_typed(std::uint16_t{}); break;
            case QCORE_DTYPE_UINT32: scatter_typed(std::uint32_t{}); break;
            case QCORE_DTYPE_UINT64: scatter_typed(std::uint64_t{}); break;
            case QCORE_DTYPE_FLOAT64: scatter_typed(double{}); break;
            case QCORE_DTYPE_FLOAT32: scatter_typed(float{}); break;
            case QCORE_DTYPE_FLOAT16: scatter_typed(Real16{}); break;
            case QCORE_DTYPE_BFLOAT16: scatter_typed(Real16b{}); break;
            default:
                tensor_storage_release(storage);
                tensor_fail("tensor.scatter requires a numeric tensor element type",line,column);
        }
    }else{
        auto* dense=tensor_gpu_materialize_storage(source,line,column);
        std::vector<std::uint64_t> indices(source_count);
        for(std::size_t i=0;i<source_count;++i)
            indices[i]=static_cast<std::uint64_t>(logical_indices[i]);
        std::string backend_error;
        const bool ok=quidra::device::compute_gather_backward(
            storage->gpu_buffer,dense->gpu_buffer,source.storage->dtype,
            indices.data(),output_count,source_count,backend_error,true);
        tensor_storage_release(dense);
        if(!ok){
            tensor_storage_release(storage);
            tensor_fail(backend_error.c_str(),line,column);
        }
    }

    auto* result=tensor_descriptor(
        storage,output_shape,tensor_contiguous_strides(output_shape),0);
    quidra::runtime::NodeRuntimeOwner partial_graph_result_5(result,+[](void* raw){release_managed_tensor(raw);});
    if(source.graph){
        auto node=std::make_shared<AutogradNode>(source.storage->dtype);
        node->shape=output_shape;
        node->parents={source.graph};
        node->op=AutogradOp::GatherBackward;
        node->aux_index=std::move(logical_indices);
        if(tensor_on_cpu(*storage))
            node->data=tensor_float_values(*result,line,column);
        else
            node->device_tensor=static_cast<TensorValue*>(
                quidra_tensor_clone(result));
        node->note_saved_value();
        result->graph=std::move(node);
    }
    partial_graph_result_5.relinquish();
    return result;
}

class AutogradDeviceDenseInput {
public:
    AutogradDeviceDenseInput()=default;
    AutogradDeviceDenseInput(
        const TensorValue& value,unsigned long long line,unsigned long long column) {
        reset(value,line,column);
    }
    AutogradDeviceDenseInput(const AutogradDeviceDenseInput&)=delete;
    AutogradDeviceDenseInput& operator=(const AutogradDeviceDenseInput&)=delete;
    ~AutogradDeviceDenseInput() {
        if(quidra::runtime::node_failure_active()) {
            auto* value=std::exchange(owned_,nullptr);
            if(value)quidra::runtime::cleanup_node_slot(+[](void* raw){release_managed_tensor(raw);},value);
        } else if(owned_) release_managed_tensor(owned_);
    }

    void reset(
        const TensorValue& value,unsigned long long line,unsigned long long column) {
        if(owned_) {
            if(quidra::runtime::node_failure_active()) {
                auto* previously_owned=std::exchange(owned_,nullptr);
                release_managed_tensor(previously_owned);
            } else {
                release_managed_tensor(owned_);
                owned_=nullptr;
            }
        }
        quidra::counters::add(quidra::counters::Id::RuntimeDeviceChecks);
        if(tensor_on_cpu(*value.storage))
            autograd_fail("internal autograd GPU path received a CPU tensor",line,column);
        if(tensor_is_contiguous_value(value)&&value.offset==0){
            value_=&value;
            return;
        }
        auto* storage=tensor_gpu_materialize_storage(value,line,column);
        owned_=tensor_descriptor(
            storage,value.shape,tensor_contiguous_strides(value.shape),0);
        value_=owned_;
    }

    const TensorValue* get() const { return value_; }
    const TensorValue* operator->() const { return value_; }

private:
    const TensorValue* value_{};
    TensorValue* owned_{};
};




TensorValue* autograd_device_binary_tensor(
    TensorValue* left,TensorValue* right,int operation,
    unsigned long long line,unsigned long long column) {
    return static_cast<TensorValue*>(
        quidra_tensor_binary(left,right,nullptr,abi::scalar_side::none,operation,line,column));
}

TensorValue* autograd_device_scalar_tensor(
    TensorValue* input,double scalar,int operation,bool scalar_left,
    unsigned long long line,unsigned long long column) {
    if(!input||!input->storage)
        autograd_fail("null GPU autograd scalar operand",line,column);
    const int side=scalar_left?1:2;
    if(autograd_half_dtype(input->storage->dtype)){
        auto value=quidra::abi::half::round_to(scalar,autograd_half_format(input->storage->dtype));
        return static_cast<TensorValue*>(
            quidra_tensor_binary(input,nullptr,&value,side,operation,line,column));
    }
    if(input->storage->dtype==QCORE_DTYPE_FLOAT32){
        float value=static_cast<float>(scalar);
        return static_cast<TensorValue*>(
            quidra_tensor_binary(input,nullptr,&value,side,operation,line,column));
    }
    if(input->storage->dtype==QCORE_DTYPE_FLOAT64){
        double value=scalar;
        return static_cast<TensorValue*>(
            quidra_tensor_binary(input,nullptr,&value,side,operation,line,column));
    }
    autograd_fail("invalid GPU autograd scalar dtype",line,column);
}

// The kernel computes both partial derivatives element by element from
// (g, a, b): Add and Sub read neither operand, Mul's left partial reads only
// b and its right partial only a, Div's left partial reads only b and its
// right partial both. left_needed/right_needed (split outputs only) say which
// partials the caller keeps. An operand that no kept partial reads is not
// densified: the kernel reads the dense gradient in its place, which changes
// only values that are discarded. A discarded partial goes to scratch that
// is never read, so that scratch is not zero-filled.
void cleanup_node_tensor_cell(void* raw) {
    auto*& cell=*static_cast<TensorValue**>(raw);
    auto* value=std::exchange(cell,nullptr);
    if(value) quidra::runtime::cleanup_node_slot(
        +[](void* tensor){release_managed_tensor(tensor);},value);
}

void autograd_device_binary_backward(
    TensorValue* gradient,TensorValue* left,TensorValue* right,int operation,
    bool shared_parent,TensorValue*& left_gradient,TensorValue*& right_gradient,
    unsigned long long line,unsigned long long column,
    bool left_needed=true,bool right_needed=true) {
    if(shared_parent&&left!=right)
        autograd_fail("shared autograd binary parent mismatch",line,column);
    if(shared_parent) left_needed=right_needed=true;
    const bool reads_left=operation>=abi::tensor_binary_opcode::multiply && right_needed;
    const bool reads_right=operation==abi::tensor_binary_opcode::divide || (operation==abi::tensor_binary_opcode::multiply && left_needed);
    AutogradDeviceDenseInput gd(*gradient,line,column);
    AutogradDeviceDenseInput ad;
    AutogradDeviceDenseInput bd;
    if(reads_left) ad.reset(*left,line,column);
    if(reads_right && !shared_parent) bd.reset(*right,line,column);
    const TensorValue* a=reads_left ? ad.get() : gd.get();
    const TensorValue* b=!reads_right ? gd.get() : shared_parent ? a : bd.get();
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if(gd->shape!=left->shape||gd->shape!=right->shape||
       gd->shape!=a->shape||gd->shape!=b->shape)
        autograd_fail("autograd binary backward shape mismatch",line,column);
    const auto count=tensor_logical_count(*gd.get());
    auto* left_storage=tensor_storage_create(
        gd->storage->dtype,count,left_needed?abi::tensor_fill_mode::write_only:abi::tensor_fill_mode::uninitialized,gd->storage->device,line,column);
    TensorStorage* right_storage=left_storage;
    left_gradient=tensor_descriptor(
        left_storage,left->shape,tensor_contiguous_strides(left->shape),0);
    quidra::runtime::NodeRuntimeOwner partial_left(&left_gradient,cleanup_node_tensor_cell);
    right_gradient=nullptr;
    quidra::runtime::NodeRuntimeOwner partial_right(&right_gradient,cleanup_node_tensor_cell);
    if(!shared_parent){
        right_storage=tensor_storage_create(
            gd->storage->dtype,count,right_needed?abi::tensor_fill_mode::write_only:abi::tensor_fill_mode::uninitialized,gd->storage->device,line,column);
        right_gradient=tensor_descriptor(
            right_storage,right->shape,tensor_contiguous_strides(right->shape),0);
    }

    std::string backend_error;
    const bool ok=quidra::device::compute_binary_backward(
        left_storage->gpu_buffer,right_storage->gpu_buffer,
        gd->storage->gpu_buffer,a->storage->gpu_buffer,b->storage->gpu_buffer,
        gd->storage->dtype,operation,count,backend_error);
    if(!ok){
        release_managed_tensor(std::exchange(left_gradient,nullptr));
        if(right_gradient) release_managed_tensor(std::exchange(right_gradient,nullptr));
        autograd_fail(backend_error.c_str(),line,column);
    }
    partial_left.relinquish();
    partial_right.relinquish();
}

TensorValue* autograd_device_negate_tensor(
    TensorValue* input,unsigned long long line,unsigned long long column) {
    return static_cast<TensorValue*>(
        quidra_tensor_unary(input,abi::tensor_unary_opcode::negate,line,column));
}

bool autograd_accumulate_device_gradient_in_place(
    TensorValue* destination,const TensorValue* source,
    unsigned long long line,unsigned long long column) {
    if(!destination||!source||destination==source||
       !destination->storage||!source->storage)
        return false;
    if(tensor_on_cpu(*destination->storage)||tensor_on_cpu(*source->storage)||
       !tensor_storage_writable_in_place(*destination->storage)||
       destination->storage->dtype!=source->storage->dtype||
       destination->storage->device!=source->storage->device||
       destination->shape!=source->shape||
       destination->offset!=0||source->offset!=0||
       !tensor_is_contiguous_value(*destination)||
       !tensor_is_contiguous_value(*source))
        return false;

    tensor_require_initialized(*destination,line,column);
    tensor_require_initialized(*source,line,column);
    std::string backend_error;
    const auto count=tensor_logical_count(*destination);
    if(!quidra::device::compute_binary(
            destination->storage->gpu_buffer,
            destination->storage->gpu_buffer,0,
            source->storage->gpu_buffer,0,
            nullptr,abi::scalar_side::none,destination->storage->dtype,abi::tensor_binary_opcode::add,count,backend_error))
        autograd_fail(backend_error.c_str(),line,column);
    return true;
}

void autograd_add_device_gradient(
    std::unordered_map<const AutogradNode*,TensorValue*>& gradients,
    const std::shared_ptr<AutogradNode>& node,TensorValue* value,
    unsigned long long line,unsigned long long column) {
    quidra::runtime::NodeRuntimeOwner partial_value(value,+[](void* raw){release_managed_tensor(raw);});
    if(!value) autograd_fail("null GPU autograd gradient",line,column);
    const auto found=gradients.find(node.get());
    if(found==gradients.end()){
        gradients.emplace(node.get(),value);
        partial_value.relinquish();
        return;
    }
    if(autograd_accumulate_device_gradient_in_place(
            found->second,value,line,column)){
        partial_value.relinquish();release_managed_tensor(value);
        return;
    }
    auto* combined=autograd_device_binary_tensor(
        found->second,value,abi::tensor_binary_opcode::add,line,column);
    auto* old=std::exchange(found->second,combined);
    release_managed_tensor(old);
    partial_value.relinquish();release_managed_tensor(value);
}

TensorValue* autograd_device_filled_like(
    const AutogradNode& node,bool ones,
    unsigned long long line,unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeDeviceChecks);
    if(!node.device_tensor)
        autograd_fail("internal autograd GPU fill requires a device tensor",line,column);
    const auto count=tensor_logical_count(*node.device_tensor);
    auto* storage=tensor_storage_create(
        node.dtype,count,ones?abi::tensor_fill_mode::ones:abi::tensor_fill_mode::zeros,node.device_tensor->storage->device,line,column);
    auto strides=tensor_contiguous_strides(node.shape);
    return tensor_descriptor(storage,node.shape,std::move(strides),0);
}

// First-order gradient pruning. A node needs a gradient when it is the leaf
// of a selected target or reaches one through its parents. Gradients flow
// only into such nodes, so the formulas of values that reach no selected
// target never run (user decision Q8: a custom callback still runs whenever
// any of its inputs needs a gradient). Every contribution to a needed node
// comes from a needed child and is accumulated in the same order as
// before, so the gradients that reach targets are bitwise unchanged.
// QUIDRA_AUTOGRAD_PRUNE=off restores the previous walk, in which every node
// counts as needed. QUIDRA_TEST_AUTOGRAD_STATS=1 reports the counts on
// stderr.
class AutogradNeed {
public:
    AutogradNeed(const std::vector<std::shared_ptr<AutogradNode>>& order,
                 const std::vector<std::shared_ptr<AutogradSlot>>& selected) {
        static const bool pruning=runtime_switch_enabled(
            "QUIDRA_AUTOGRAD_PRUNE","on","off",true);
        std::unordered_set<const AutogradIdentity*> identities;
        identities.reserve(selected.size());
        for(const auto& slot:selected)
            if(slot && slot->identity) identities.insert(slot->identity.get());
        needed_.reserve(order.size());
        // autograd_topological lists every parent before its children.
        for(const auto& node:order){
            bool need=!pruning || (node->target_identity &&
                identities.count(node->target_identity.get())!=0);
            for(const auto& parent:node->parents)
                if(!need && parent && needed_.count(parent.get())!=0) need=true;
            if(need){
                needed_.insert(node.get());
                if(!node->parents.empty()) ++formulas_;
            }else if(!node->parents.empty()){
                ++pruned_;
            }
        }
    }

    bool operator()(const std::shared_ptr<AutogradNode>& node) const {
        return node && needed_.count(node.get())!=0;
    }

    bool any_parent(const AutogradNode& node) const {
        for(const auto& parent:node.parents)
            if((*this)(parent)) return true;
        return false;
    }

    void report() const {
        quidra::counters::add(quidra::counters::Id::AutogradFormulas, formulas_);
        quidra::counters::add(quidra::counters::Id::AutogradPruned, pruned_);
        static const bool enabled=[]{
            const auto value=quidra::platform::environment_value("QUIDRA_TEST_AUTOGRAD_STATS");
            return value && *value=="1";
        }();
        if(enabled)
            std::fprintf(stderr,"autograd stats: formulas %zu pruned %zu\n",
                         formulas_,pruned_);
    }

private:
    std::unordered_set<const AutogradNode*> needed_;
    std::size_t formulas_{};
    std::size_t pruned_{};
};

// Runs a custom native backward callback. `needed[i]` says whether parent i
// needs its gradient. A full-request callback receives a gradient tensor for
// every input, as before; a masked callback receives one only where needed
// (NULL elsewhere) and the mask itself. Requested gradients are zero-filled
// before the call (the callbacks' contract), except those a masked callback
// declared fully written at attach time (full_writes): they are write-only
// outputs (fill mode write_only). The returned vector holds nullptr for every
// gradient that was not requested.
std::vector<TensorValue*> autograd_custom_backward_tensors(
    const AutogradNode& node,TensorValue* gradient,
    const std::vector<unsigned char>& needed,
    unsigned long long line,unsigned long long column) {
    if(node.op!=AutogradOp::CustomNative ||
       (!node.custom_backward && !node.custom_backward_masked) ||
       node.parents.empty() || needed.size()!=node.parents.size())
        autograd_fail("invalid custom native autograd node",line,column);
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if(!gradient || !gradient->storage || gradient->storage->dtype!=node.dtype ||
       gradient->shape!=node.shape)
        autograd_fail("custom native gradient output does not match forward output",line,column);

    const bool masked=node.custom_backward_masked!=nullptr;
    std::vector<TensorValue*> outputs;
    quidra::runtime::NodeRuntimeOwner partial_outputs(&outputs,cleanup_node_tensor_vector);
    outputs.reserve(node.parents.size());
    try {
        for(std::size_t index=0;index<node.parents.size();++index){
            const auto& parent=node.parents[index];
            if(!parent || parent->dtype!=node.dtype)
                autograd_fail("invalid custom native autograd parent",line,column);
            if(masked && !needed[index]){
                outputs.push_back(nullptr);
                continue;
            }
            const auto count=autograd_node_count(*parent);
            const int device=parent->device_tensor
                ? parent->device_tensor->storage->device : -1;
            // A declared full write is a write-only output, like a Core
            // kernel output that stores every element (fill mode write_only): Metal
            // and the fake GPU skip its zero fill, CUDA/HIP keep it and
            // QUIDRA_GPU_ZERO_FILL=always restores it. The callback writes
            // every element and must report it, or the backward fails
            // below, so the unfilled contents are never observed.
            const bool write_only=masked && !node.custom_full_writes.empty() &&
                node.custom_full_writes[index]!=0;
            auto* storage=tensor_storage_create(
                node.dtype,count,write_only?abi::tensor_fill_mode::write_only:abi::tensor_fill_mode::zeros,device,line,column);
            outputs.push_back(tensor_descriptor(
                storage,parent->shape,tensor_contiguous_strides(parent->shape),0));
        }

        std::vector<const void*> saved;
        std::vector<void*> writable;
        saved.reserve(node.custom_saved.size());
        writable.reserve(outputs.size());
        for(const auto* value:node.custom_saved){
            if(!value || !value->storage)
                autograd_fail("invalid custom native saved tensor",line,column);
            saved.push_back(value);
        }
        for(auto* value:outputs) writable.push_back(value);

        // The native callback ABI carries no strides, so a callback can only
        // read a dense upstream gradient. Device view backward (transpose)
        // yields strided gradients; hand the callback a dense copy at offset
        // 0 of the same values, as the host engine does with its dense
        // gradient buffers. A gradient already dense at offset 0 is borrowed.
        AutogradDeviceDenseInput dense_gradient;
        const TensorValue* callback_gradient=gradient;
        if(!tensor_on_cpu(*gradient->storage)){
            dense_gradient.reset(*gradient,line,column);
            callback_gradient=dense_gradient.get();
        }

        int status=-1;
        std::vector<std::uint8_t> fully_written;
        {
            const PackageBackwardScope package_call(line,column);
            try {
                if(masked){
                    // Undeclared gradients are zero-filled before the call, so a
                    // callback that writes only part of one leaves zeros
                    // elsewhere. A gradient declared fully written at attach
                    // time was allocated write-only above and must be reported
                    // in fully_written after a successful call; a broken
                    // promise fails the backward before the gradient is used.
                    std::vector<std::uint8_t> mask(needed.begin(),needed.end());
                    fully_written.assign(needed.size(),0);
                    status=node.custom_backward_masked(
                        saved.empty()?nullptr:saved.data(),
                        static_cast<std::uint64_t>(saved.size()),
                        callback_gradient,writable.data(),mask.data(),
                        fully_written.data(),
                        static_cast<std::uint64_t>(writable.size()),
                        node.custom_metadata.empty()?nullptr:node.custom_metadata.data(),
                        static_cast<std::uint64_t>(node.custom_metadata.size()));
                }else{
                    status=node.custom_backward(
                        saved.empty()?nullptr:saved.data(),
                        static_cast<std::uint64_t>(saved.size()),
                        callback_gradient,writable.data(),
                        static_cast<std::uint64_t>(writable.size()),
                        node.custom_metadata.empty()?nullptr:node.custom_metadata.data(),
                        static_cast<std::uint64_t>(node.custom_metadata.size()));
                }
            } catch (...) {
                status=-1;
            }
            package_call.returned();
        }
        if(status!=0){
            const auto message=
                "custom native autograd backward failed with status " +
                std::to_string(status);
            autograd_fail(message.c_str(),line,column);
        }
        if(masked && !node.custom_full_writes.empty()){
            for(std::size_t index=0;index<outputs.size();++index){
                if(outputs[index] && node.custom_full_writes[index] &&
                   !fully_written[index]){
                    const auto message=
                        "custom native autograd backward did not report a "
                        "complete write of gradient input " +
                        std::to_string(index) + " declared as fully written";
                    autograd_fail(message.c_str(),line,column);
                }
            }
        }
        partial_outputs.relinquish();
        return outputs;
    } catch (...) {
        for(auto*& value:outputs)
            if(value) release_managed_tensor(std::exchange(value,nullptr));
        throw;
    }
}

std::vector<unsigned char> autograd_parent_mask(
    const AutogradNode& node,const AutogradNeed& need) {
    std::vector<unsigned char> mask(node.parents.size(),0);
    for(std::size_t index=0;index<node.parents.size();++index)
        mask[index]=need(node.parents[index]) ? 1 : 0;
    return mask;
}

void autograd_grad_device(
    const std::shared_ptr<AutogradNode>& loss,
    const std::vector<std::shared_ptr<AutogradSlot>>& selected,
    unsigned long long line,unsigned long long column) {
    std::unordered_set<const AutogradNode*> seen;
    std::vector<std::shared_ptr<AutogradNode>> order;
    seen.reserve(64);
    order.reserve(64);
    bool cast_graph=false;
    autograd_topological(loss,seen,order,&cast_graph);
    if(cast_graph) autograd_validate_cast_graph(order,false,line,column);

    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);

    quidra::counters::add(quidra::counters::Id::RuntimeDeviceChecks);

    if(!loss->device_tensor||tensor_logical_count(*loss->device_tensor)!=1)
        autograd_fail("grad requires a scalar GPU loss",line,column);

    const AutogradNeed need(order,selected);
    need.report();
    std::unordered_map<const AutogradNode*,TensorValue*> gradients;
    quidra::runtime::NodeRuntimeOwner partial_gradients(&gradients,+[](void* raw){
        auto& owned=*static_cast<std::unordered_map<const AutogradNode*,TensorValue*>*>(raw);
        for(auto& [_,slot]:owned) {
            auto* value=std::exchange(slot,nullptr);
            if(value)quidra::runtime::cleanup_node_slot(
                +[](void* tensor){release_managed_tensor(tensor);},value);
        }
    });
    gradients.reserve(order.size());
    auto* initial=autograd_device_filled_like(*loss,true,line,column);
    quidra::runtime::NodeRuntimeOwner partial_initial(initial,+[](void* raw){release_managed_tensor(raw);});
    gradients.emplace(loss.get(),initial);
    partial_initial.relinquish();

    for(auto it=order.rbegin();it!=order.rend();++it){
        const auto& node=*it;
        if(!cast_graph&&node->dtype!=loss->dtype)
            autograd_fail("autograd graph contains mixed dtypes",line,column);
        const auto found=gradients.find(node.get());
        if(found==gradients.end()) continue;
        auto* g=found->second;
        auto take_gradient=[&]() {
            if(quidra::runtime::node_failure_active()) {
                const auto owner=gradients.find(node.get());
                if(owner!=gradients.end()) owner->second=nullptr;
            }
            return std::exchange(g,nullptr);
        };

        if(node->target_identity){
            for(const auto& slot:selected){
                if(!slot || slot->identity!=node->target_identity) continue;
                autograd_accumulate_slot(
                    slot,static_cast<TensorValue*>(quidra_tensor_clone(g)),line,column);
            }
        }
        if(node->parents.empty() || !need.any_parent(*node)){
            if(quidra::runtime::node_failure_active())found->second=nullptr;
            release_managed_tensor(g);
            gradients.erase(node.get());
            continue;
        }

        if(node->op==AutogradOp::Cast){
            auto* converted=autograd_cast_gradient(g,node->parents[0]->dtype,line,column);
            autograd_add_device_gradient(gradients,node->parents[0],converted,line,column);
        }else if(node->op==AutogradOp::CustomNative){
            const auto mask=autograd_parent_mask(*node,need);
            auto input_gradients=autograd_custom_backward_tensors(
                *node,g,mask,line,column);
            quidra::runtime::NodeRuntimeOwner partial_inputs(&input_gradients,cleanup_node_tensor_vector);
            for(std::size_t index=0;index<input_gradients.size();++index){
                if(!input_gradients[index]) continue;
                if(!mask[index]){
                    release_managed_tensor(std::exchange(input_gradients[index],nullptr));
                    continue;
                }
                autograd_add_device_gradient(
                    gradients,node->parents[index],std::exchange(input_gradients[index],nullptr),
                    line,column);
            }
        }else if(node->op==AutogradOp::Add||node->op==AutogradOp::Sub||
           node->op==AutogradOp::Mul||node->op==AutogradOp::Div){
            if(node->parents.size()!=2 ||
               !node->parents[0]->device_tensor ||
               !node->parents[1]->device_tensor)
                autograd_fail("invalid GPU autograd binary graph",line,column);
            auto* a=node->parents[0]->device_tensor;
            auto* b=node->parents[1]->device_tensor;
            const int operation=node->op==AutogradOp::Add?1:
                node->op==AutogradOp::Sub?2:node->op==AutogradOp::Mul?3:4;
            const bool shared_parent=node->parents[0].get()==node->parents[1].get();
            TensorValue* left_gradient=nullptr;
            TensorValue* right_gradient=nullptr;
            quidra::runtime::NodeRuntimeOwner partial_left(&left_gradient,cleanup_node_tensor_cell);
            quidra::runtime::NodeRuntimeOwner partial_right(&right_gradient,cleanup_node_tensor_cell);
            if(shared_parent){
                // Produce the sum of both partial derivatives directly. This
                // turns x+x / x*x style backward from two gradient tensors plus
                // an accumulation kernel into one gradient tensor and one kernel.
                autograd_device_binary_backward(
                    g,a,b,operation,true,left_gradient,right_gradient,line,column);
                autograd_add_device_gradient(
                    gradients,node->parents[0],std::exchange(left_gradient,nullptr),line,column);
            }else{
                const bool left_needed=need(node->parents[0]);
                const bool right_needed=need(node->parents[1]);
                if(node->op==AutogradOp::Add){
                    if(left_needed && right_needed)
                        right_gradient=static_cast<TensorValue*>(quidra_tensor_clone(g));
                    else if(right_needed)
                        right_gradient=take_gradient();
                    if(left_needed) left_gradient=take_gradient();
                }else if(node->op==AutogradOp::Sub){
                    if(right_needed)
                        right_gradient=autograd_device_negate_tensor(g,line,column);
                    if(left_needed) left_gradient=take_gradient();
                }else{
                    // One kernel produces both partial derivatives; the one
                    // no target needs is dropped.
                    autograd_device_binary_backward(
                        g,a,b,operation,false,
                        left_gradient,right_gradient,line,column,
                        left_needed,right_needed);
                    if(!left_needed)
                        release_managed_tensor(std::exchange(left_gradient,nullptr));
                    if(!right_needed)
                        release_managed_tensor(std::exchange(right_gradient,nullptr));
                }
                if(left_gradient)
                    autograd_add_device_gradient(
                        gradients,node->parents[0],std::exchange(left_gradient,nullptr),line,column);
                if(right_gradient)
                    autograd_add_device_gradient(
                        gradients,node->parents[1],std::exchange(right_gradient,nullptr),line,column);
            }
        }else if(node->op==AutogradOp::ScalarBinary){
            if(node->parents.size()!=1||node->aux.size()!=1||
               node->aux_index.size()!=2)
                autograd_fail("invalid GPU autograd scalar graph",line,column);
            const auto& input=node->parents[0];
            const auto operation=static_cast<int>(node->aux_index[0]);
            const bool scalar_left=node->aux_index[1]!=0;
            const auto scalar=node->aux.scalar_as_double(0);
            TensorValue* result=nullptr;
            quidra::runtime::NodeRuntimeOwner partial_result(&result,cleanup_node_tensor_cell);
            if(operation==abi::tensor_binary_opcode::add||(operation==abi::tensor_binary_opcode::subtract&&!scalar_left)){
                result=take_gradient();
            }else if(operation==abi::tensor_binary_opcode::subtract){
                result=autograd_device_negate_tensor(g,line,column);
            }else if(operation==abi::tensor_binary_opcode::power&&!autograd_half_dtype(node->dtype)){
                if(scalar_left)
                    autograd_fail("tensor power requires tensor ^ scalar",line,column);
                auto* coefficient=autograd_device_scalar_tensor(
                    g,scalar,abi::tensor_binary_opcode::multiply,false,line,column);
                quidra::runtime::NodeRuntimeOwner partial_coefficient(coefficient,+[](void* raw){release_managed_tensor(raw);});
                auto* power=autograd_device_scalar_tensor(
                    input->device_tensor,scalar-1.0,abi::tensor_binary_opcode::power,false,line,column);
                quidra::runtime::NodeRuntimeOwner partial_power(power,+[](void* raw){release_managed_tensor(raw);});
                result=autograd_device_binary_tensor(
                    coefficient,power,abi::tensor_binary_opcode::multiply,line,column);
                partial_coefficient.relinquish();
                release_managed_tensor(coefficient);
                partial_power.relinquish();
                release_managed_tensor(power);
            }else{
                AutogradDeviceDenseInput gd(*g,line,column);
                AutogradDeviceDenseInput xd;
                const TensorValue* input_dense=gd.get();
                if((operation==abi::tensor_binary_opcode::divide&&scalar_left)||
                   operation==abi::tensor_binary_opcode::power){
                    xd.reset(*input->device_tensor,line,column);
                    input_dense=xd.get();
                    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
                    if(gd->shape!=xd->shape)
                        autograd_fail("autograd scalar backward shape mismatch",line,column);
                }
                const auto count=tensor_logical_count(*gd.get());
                auto* storage=tensor_storage_create(
                    node->dtype,count,abi::tensor_fill_mode::write_only,gd->storage->device,line,column);
                result=tensor_descriptor(
                    storage,input->shape,tensor_contiguous_strides(input->shape),0);
                std::string backend_error;
                const bool ok=quidra::device::compute_scalar_backward(
                    storage->gpu_buffer,gd->storage->gpu_buffer,
                    input_dense->storage->gpu_buffer,node->dtype,operation,
                    scalar_left,node->aux.scalar_as_double(0),count,backend_error);
                if(!ok){
                    release_managed_tensor(std::exchange(result,nullptr));
                    autograd_fail(backend_error.c_str(),line,column);
                }
            }
            autograd_add_device_gradient(
                gradients,input,std::exchange(result,nullptr),line,column);
        }else if(node->op==AutogradOp::Reshape){
            if(node->parents.size()!=1||!node->parents[0]->device_tensor)
                autograd_fail("invalid GPU tensor reshape graph",line,column);
            auto* parent=node->parents[0]->device_tensor;
            AutogradDeviceDenseInput gd(*g,line,column);
            if(!tensor_is_contiguous_value(*gd.get()))
                autograd_fail("reshape backward gradient must be contiguous",line,column);
            if(gd->storage->owners==std::numeric_limits<std::size_t>::max())
                runtime_text_failure("tensor storage owner overflow");
            ++gd->storage->owners;
            quidra::runtime::NodeRuntimeOwner partial_retained_7(gd->storage, +[](void* value) {
                tensor_storage_release(static_cast<TensorStorage*>(value));
            });
            auto* result=tensor_descriptor(
                gd->storage,parent->shape,tensor_contiguous_strides(parent->shape),gd->offset);
            quidra::runtime::NodeRuntimeOwner partial_result(&result,cleanup_node_tensor_cell);
            partial_retained_7.relinquish();
            autograd_add_device_gradient(gradients,node->parents[0],std::exchange(result,nullptr),line,column);
        }else if(node->op==AutogradOp::Transpose){
            if(node->parents.size()!=1||node->aux_index.size()!=2||
               !node->parents[0]->device_tensor)
                autograd_fail("invalid GPU tensor transpose graph",line,column);
            auto* parent=node->parents[0]->device_tensor;
            if(g->storage->owners==std::numeric_limits<std::size_t>::max())
                runtime_text_failure("tensor storage owner overflow");
            ++g->storage->owners;
            quidra::runtime::NodeRuntimeOwner partial_retained_8(g->storage, +[](void* value) {
                tensor_storage_release(static_cast<TensorStorage*>(value));
            });
            auto shape=g->shape;
            auto strides=g->strides;
            const auto axis0=node->aux_index[0],axis1=node->aux_index[1];
            quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
            if(axis0>=shape.size()||axis1>=shape.size())
                autograd_fail("invalid GPU tensor transpose axis",line,column);
            std::swap(shape[axis0],shape[axis1]);
            std::swap(strides[axis0],strides[axis1]);
            auto* result=tensor_descriptor(g->storage,std::move(shape),std::move(strides),g->offset);
            quidra::runtime::NodeRuntimeOwner partial_result(&result,cleanup_node_tensor_cell);
            partial_retained_8.relinquish();
            quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
            if(result->shape!=parent->shape){
                release_managed_tensor(std::exchange(result,nullptr));
                autograd_fail("GPU tensor transpose backward shape mismatch",line,column);
            }
            autograd_add_device_gradient(gradients,node->parents[0],std::exchange(result,nullptr),line,column);
        }else if(node->op==AutogradOp::Gather){
            if(node->parents.size()!=1||!node->parents[0]->device_tensor)
                autograd_fail("invalid GPU tensor gather graph",line,column);
            const auto& input=node->parents[0];
            const auto input_count=autograd_node_count(*input);
            AutogradDeviceDenseInput gd(*g,line,column);
            auto* storage=tensor_storage_create(
                node->dtype,input_count,abi::tensor_fill_mode::write_only,input->device_tensor->storage->device,
                line,column);
            auto* result=tensor_descriptor(
                storage,input->shape,tensor_contiguous_strides(input->shape),0);
            quidra::runtime::NodeRuntimeOwner partial_result(&result,cleanup_node_tensor_cell);
            std::vector<std::uint64_t> indices(node->aux_index.size());
            for(std::size_t i=0;i<indices.size();++i)
                indices[i]=static_cast<std::uint64_t>(node->aux_index[i]);
            std::string backend_error;
            const bool ok=quidra::device::compute_gather_backward(
                storage->gpu_buffer,gd->storage->gpu_buffer,node->dtype,
                indices.data(),input_count,indices.size(),backend_error);
            if(!ok){
                release_managed_tensor(std::exchange(result,nullptr));
                autograd_fail(backend_error.c_str(),line,column);
            }
            autograd_add_device_gradient(gradients,input,std::exchange(result,nullptr),line,column);
        }else if(node->op==AutogradOp::Broadcast){
            if(node->parents.size()!=1||!node->parents[0]->device_tensor)
                autograd_fail("invalid GPU tensor broadcast graph",line,column);
            const auto& input=node->parents[0];
            const auto input_count=autograd_node_count(*input);
            AutogradDeviceDenseInput gd(*g,line,column);
            quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
            if(gd->shape!=node->shape)
                autograd_fail("GPU tensor broadcast backward shape mismatch",line,column);
            // One kernel thread per source element writes it (fill mode write_only).
            auto* storage=tensor_storage_create(
                node->dtype,input_count,abi::tensor_fill_mode::write_only,input->device_tensor->storage->device,
                line,column);
            auto* result=tensor_descriptor(
                storage,input->shape,tensor_contiguous_strides(input->shape),0);
            quidra::runtime::NodeRuntimeOwner partial_result(&result,cleanup_node_tensor_cell);
            std::string backend_error;
            if(!quidra::device::compute_broadcast_backward(
                   storage->gpu_buffer,gd->storage->gpu_buffer,node->dtype,
                   input->shape,node->shape,backend_error)){
                release_managed_tensor(std::exchange(result,nullptr));
                autograd_fail(backend_error.c_str(),line,column);
            }
            autograd_add_device_gradient(gradients,input,std::exchange(result,nullptr),line,column);
        }else if(node->op==AutogradOp::GatherBackward){
            if(node->parents.size()!=1||!node->parents[0]->device_tensor)
                autograd_fail("invalid GPU tensor scatter graph",line,column);
            const auto& input=node->parents[0];
            const auto input_count=autograd_node_count(*input);
            if(node->aux_index.size()!=input_count)
                autograd_fail("GPU tensor scatter index count mismatch",line,column);
            AutogradDeviceDenseInput gd(*g,line,column);
            const auto gradient_count=tensor_logical_count(*gd.get());
            std::vector<std::uint64_t> indices(input_count);
            for(std::size_t i=0;i<input_count;++i){
                if(node->aux_index[i]>=gradient_count)
                    autograd_fail("GPU tensor scatter index is out of bounds for the gradient",line,column);
                indices[i]=static_cast<std::uint64_t>(node->aux_index[i]);
            }
            auto* storage=tensor_storage_create(
                node->dtype,input_count,abi::tensor_fill_mode::write_only,input->device_tensor->storage->device,
                line,column);
            auto* result=tensor_descriptor(
                storage,input->shape,tensor_contiguous_strides(input->shape),0);
            quidra::runtime::NodeRuntimeOwner partial_result(&result,cleanup_node_tensor_cell);
            std::string backend_error;
            const bool ok=quidra::device::compute_gather(
                storage->gpu_buffer,gd->storage->gpu_buffer,node->dtype,
                indices.data(),input_count,backend_error);
            if(!ok){
                release_managed_tensor(std::exchange(result,nullptr));
                autograd_fail(backend_error.c_str(),line,column);
            }
            autograd_add_device_gradient(gradients,input,std::exchange(result,nullptr),line,column);
        }else if(node->op==AutogradOp::Neg){
            if(node->parents.size()!=1||!node->parents[0]->device_tensor)
                autograd_fail("invalid GPU tensor negation graph",line,column);
            autograd_add_device_gradient(
                gradients,node->parents[0],
                autograd_device_negate_tensor(g,line,column),line,column);
        }

        // Every child has already contributed in reverse-topological order.
        // Release this gradient now instead of retaining the entire backward pass.
        if(quidra::runtime::node_failure_active()) {
            const auto owner=gradients.find(node.get());
            if(owner!=gradients.end()) owner->second=nullptr;
        }
        if(g) release_managed_tensor(g);
        gradients.erase(node.get());
    }

    for(auto& [_,value]:gradients)
        release_managed_tensor(std::exchange(value,nullptr));
}

using AutogradCastGradients=std::unordered_map<const AutogradNode*,AutogradBuffer>;

template <typename T>
void autograd_add_cast_gradient(
    AutogradCastGradients& gradients,const std::shared_ptr<AutogradNode>& node,HostValues<T> value) {
    if(autograd_compute_dtype(node->dtype)!=HostValues<T>::element_dtype)
        runtime_text_failure("autograd cast buffer dtype mismatch");
    const bool half=std::is_same_v<T,float>&&autograd_half_dtype(node->dtype);
    const auto round=[&]([[maybe_unused]] HostValues<T>& values){
        if constexpr(std::is_same_v<T,float>){
            const auto count=values.size();
            if(count==0) return;
            auto* data=values.mutable_data();
            for(std::size_t i=0;i<count;++i) data[i]=autograd_round_half(data[i],node->dtype);
        }
    };
    if(half) round(value);
    auto [entry,inserted]=gradients.try_emplace(node.get(),node->dtype);
    auto& current=entry->second.typed<T>();
    if(inserted||current.empty()) current=std::move(value);
    else{
        quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
        if(current.size()!=value.size()) runtime_text_failure("autograd gradient size mismatch");
        const auto count=current.size();
        auto* out=current.mutable_data();
        const auto* in=value.data();
        for(std::size_t i=0;i<count;++i)
            out[i]=static_cast<T>(out[i]+in[i]);
        if(half) round(current);
    }
}

template <typename T>
void autograd_grad_cast_step(
    AutogradCastGradients& gradients,const std::shared_ptr<AutogradNode>& node,
    const AutogradNeed& need,const std::vector<std::shared_ptr<AutogradSlot>>& selected,
    unsigned long long line,unsigned long long column) {
    const auto found=gradients.find(node.get());
    if(found==gradients.end()) return;
    auto& incoming=found->second.typed<T>();
    const auto& g=incoming;
    if(node->target_identity){
        for(const auto& slot:selected){
            if(!slot || slot->identity!=node->target_identity) continue;
            autograd_accumulate_slot(
                slot,autograd_tensor_from_gradient<T>(node->dtype,node->shape,g),
                line,column);
        }
    }

    if(node->parents.empty() || !need.any_parent(*node)) return;

    if(node->op==AutogradOp::Cast){
        auto* value=autograd_tensor_from_gradient<T>(node->dtype,node->shape,g);
        quidra::runtime::NodeRuntimeOwner partial_value(value,+[](void* raw){release_managed_tensor(raw);});
        auto* converted=autograd_cast_gradient(value,node->parents[0]->dtype,line,column);
        quidra::runtime::NodeRuntimeOwner partial_converted(converted,+[](void* raw){release_managed_tensor(raw);});
        partial_value.relinquish();release_managed_tensor(value);
        auto next=tensor_float_values(*converted,line,column);
        partial_converted.relinquish();release_managed_tensor(converted);
        if(autograd_compute_dtype(node->parents[0]->dtype)==QCORE_DTYPE_FLOAT32)
            autograd_add_cast_gradient(gradients,node->parents[0],std::move(next.template typed<float>()));
        else
            autograd_add_cast_gradient(gradients,node->parents[0],std::move(next.template typed<double>()));
    }else if(node->op==AutogradOp::CustomNative){
        // The callback borrows the gradient as a tensor and its gradient
        // outputs come back as tensors; both move by reference, not copy.
        auto gradient_tensor=autograd_tensor_from_gradient<T>(
            node->dtype,node->shape,g);
        quidra::runtime::NodeRuntimeOwner partial_gradient_tensor(gradient_tensor,+[](void* raw){release_managed_tensor(raw);});
        const auto mask=autograd_parent_mask(*node,need);
        std::vector<TensorValue*> input_gradients;
        quidra::runtime::NodeRuntimeOwner partial_inputs(&input_gradients,cleanup_node_tensor_vector);
        try {
            input_gradients=autograd_custom_backward_tensors(
                *node,gradient_tensor,mask,line,column);
        } catch (...) {
            partial_gradient_tensor.relinquish();release_managed_tensor(gradient_tensor);
            throw;
        }
        partial_gradient_tensor.relinquish();release_managed_tensor(gradient_tensor);
        for(std::size_t index=0;index<input_gradients.size();++index){
            if(!input_gradients[index]) continue;
            if(!mask[index]){
                release_managed_tensor(std::exchange(input_gradients[index],nullptr));
                continue;
            }
            auto next=autograd_gradient_values<T>(
                *input_gradients[index],line,column);
            release_managed_tensor(std::exchange(input_gradients[index],nullptr));
            autograd_add_cast_gradient(
                gradients,node->parents[index],std::move(next));
        }
    }else if(node->op==AutogradOp::Add||node->op==AutogradOp::Sub||
       node->op==AutogradOp::Mul||node->op==AutogradOp::Div){
        const auto* a=node->parents[0]->data.typed<T>().data();
        const auto* b=node->parents[1]->data.typed<T>().data();
        const bool shared_parent=node->parents[0].get()==node->parents[1].get();
        if(shared_parent){
            HostValues<T> combined_values=std::move(incoming);
            const auto count=combined_values.size();
            auto* combined=combined_values.mutable_data();
            for(std::size_t i=0;i<count;++i){
                const T gradient=combined[i];
                T left_value{},right_value{};
                if(node->op==AutogradOp::Add){
                    left_value=gradient;right_value=gradient;
                }else if(node->op==AutogradOp::Sub){
                    left_value=gradient;right_value=static_cast<T>(-gradient);
                }else if(node->op==AutogradOp::Mul){
                    left_value=static_cast<T>(gradient*b[i]);
                    right_value=static_cast<T>(gradient*a[i]);
                }else{
                    left_value=static_cast<T>(gradient/b[i]);
                    const T ga=static_cast<T>(gradient*a[i]);
                    const T bb=static_cast<T>(b[i]*b[i]);
                    right_value=static_cast<T>(-static_cast<T>(ga/bb));
                }
                combined[i]=static_cast<T>(left_value+right_value);
            }
            autograd_add_cast_gradient(
                gradients,node->parents[0],std::move(combined_values));
        }else{
            // Each partial derivative is computed only for a parent that
            // needs it; every element keeps its expression.
            const bool left_needed=need(node->parents[0]);
            const bool right_needed=need(node->parents[1]);
            HostValues<T> left_gradient,right_gradient;
            if(node->op==AutogradOp::Add){
                if(left_needed && right_needed){
                    left_gradient=std::move(incoming);
                    right_gradient=left_gradient;
                }else if(left_needed){
                    left_gradient=std::move(incoming);
                }else{
                    right_gradient=std::move(incoming);
                }
            }else if(node->op==AutogradOp::Sub){
                if(right_needed){
                    const auto count=g.size();
                    right_gradient=HostValues<T>::allocate(count);
                    const auto* left=g.data();
                    auto* right=right_gradient.mutable_data();
                    for(std::size_t i=0;i<count;++i)
                        right[i]=static_cast<T>(-left[i]);
                }
                if(left_needed) left_gradient=std::move(incoming);
            }else{
                const auto count=g.size();
                const auto* gv=g.data();
                if(left_needed){
                    left_gradient=HostValues<T>::allocate(count);
                    auto* left=left_gradient.mutable_data();
                    if(node->op==AutogradOp::Mul){
                        for(std::size_t i=0;i<count;++i)
                            left[i]=static_cast<T>(gv[i]*b[i]);
                    }else{
                        for(std::size_t i=0;i<count;++i)
                            left[i]=static_cast<T>(gv[i]/b[i]);
                    }
                }
                if(right_needed){
                    right_gradient=HostValues<T>::allocate(count);
                    auto* right=right_gradient.mutable_data();
                    if(node->op==AutogradOp::Mul){
                        for(std::size_t i=0;i<count;++i)
                            right[i]=static_cast<T>(gv[i]*a[i]);
                    }else{
                        for(std::size_t i=0;i<count;++i)
                            right[i]=static_cast<T>(
                                -static_cast<T>(gv[i]*a[i])/
                                static_cast<T>(b[i]*b[i]));
                    }
                }
            }
            if(left_needed)
                autograd_add_cast_gradient(gradients,node->parents[0],std::move(left_gradient));
            if(right_needed)
                autograd_add_cast_gradient(gradients,node->parents[1],std::move(right_gradient));
        }
    }else if(node->op==AutogradOp::ScalarBinary){
        if(node->parents.size()!=1||node->aux.size()!=1||
           node->aux_index.size()!=2)
            autograd_fail("invalid autograd scalar graph",line,column);
        const auto* input=node->parents[0]->data.typed<T>().data();
        const auto operation=static_cast<int>(node->aux_index[0]);
        const bool scalar_left=node->aux_index[1]!=0;
        const T scalar_value=static_cast<T>(node->aux.scalar_as_double(0));
        // Scalar backward is one-to-one, so transform the completed gradient
        // buffer in place instead of allocating an equally sized temporary.
        HostValues<T> input_gradient_values=std::move(incoming);
        const auto count=input_gradient_values.size();
        auto* input_gradient=input_gradient_values.mutable_data();
        for(std::size_t i=0;i<count;++i){
            const T gradient=input_gradient[i];
            if(operation==abi::tensor_binary_opcode::add)input_gradient[i]=gradient;
            else if(operation==abi::tensor_binary_opcode::subtract)
                input_gradient[i]=scalar_left?static_cast<T>(-gradient):gradient;
            else if(operation==abi::tensor_binary_opcode::multiply)
                input_gradient[i]=static_cast<T>(gradient*scalar_value);
            else if(operation==abi::tensor_binary_opcode::power){
                if(scalar_left)
                    autograd_fail("tensor power requires tensor ^ scalar",line,column);
                input_gradient[i]=static_cast<T>(
                    gradient*scalar_value*
                    std::pow(input[i],static_cast<T>(scalar_value-T{1})));
            }else if(scalar_left){
                const T gs=static_cast<T>(gradient*scalar_value);
                const T xx=static_cast<T>(input[i]*input[i]);
                input_gradient[i]=static_cast<T>(-static_cast<T>(gs/xx));
            }else{
                input_gradient[i]=static_cast<T>(gradient/scalar_value);
            }
        }
        autograd_add_cast_gradient(
            gradients,node->parents[0],std::move(input_gradient_values));
    }else if(node->op==AutogradOp::Reshape){
        if(node->parents.size()!=1)
            autograd_fail("invalid tensor reshape graph",line,column);
        quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
        if(g.size()!=node->parents[0]->data.size())
            autograd_fail("tensor reshape backward size mismatch",line,column);
        // Reshape keeps element order and this node's gradient is erased
        // below, so hand the buffer to the parent instead of copying it.
        autograd_add_cast_gradient(
            gradients,node->parents[0],std::move(incoming));
    }else if(node->op==AutogradOp::Transpose){
        if(node->parents.size()!=1||node->aux_index.size()!=2)
            autograd_fail("invalid tensor transpose graph",line,column);
        AutogradBuffer gb(std::move(incoming));
        auto restored=autograd_transpose_values<T>(
            gb,node->shape,node->aux_index[0],node->aux_index[1],line,column);
        autograd_add_cast_gradient(
            gradients,node->parents[0],
            std::move(restored.template typed<T>()));
    }else if(node->op==AutogradOp::Gather){
        if(node->parents.size()!=1)
            autograd_fail("invalid tensor gather graph",line,column);
        auto input_gradient=autograd_gather_backward_values<T>(
            g,autograd_node_count(*node->parents[0]),node->aux_index,
            line,column);
        autograd_add_cast_gradient(
            gradients,node->parents[0],HostValues<T>(std::move(input_gradient)));
    }else if(node->op==AutogradOp::GatherBackward){
        if(node->parents.size()!=1)
            autograd_fail("invalid tensor gather backward graph",line,column);
        auto input_gradient=autograd_gather_values<T>(
            g,node->aux_index,line,column);
        autograd_add_cast_gradient(
            gradients,node->parents[0],HostValues<T>(std::move(input_gradient)));
    }else if(node->op==AutogradOp::Neg){
        if(node->parents.size()!=1)
            autograd_fail("invalid tensor negation graph",line,column);
        // Negation is one-to-one: negate the completed gradient in place.
        HostValues<T> input_gradient=std::move(incoming);
        const auto count=input_gradient.size();
        auto* values=input_gradient.mutable_data();
        for(std::size_t i=0;i<count;++i) values[i]=static_cast<T>(-values[i]);
        autograd_add_cast_gradient(
            gradients,node->parents[0],std::move(input_gradient));
    }

}

void autograd_grad_cast(
    const std::shared_ptr<AutogradNode>& loss,
    const std::vector<std::shared_ptr<AutogradNode>>& order,
    const std::vector<std::shared_ptr<AutogradSlot>>& selected,
    unsigned long long line,unsigned long long column) {
    autograd_validate_cast_graph(order,false,line,column);
    const AutogradNeed need(order,selected);
    need.report();
    AutogradCastGradients gradients;
    gradients.reserve(order.size());
    auto seed=gradients.try_emplace(loss.get(),loss->dtype).first;
    seed->second.assign(1,1.0);
    for(auto it=order.rbegin();it!=order.rend();++it){
        const auto& node=*it;
        if(autograd_compute_dtype(node->dtype)==QCORE_DTYPE_FLOAT32)
            autograd_grad_cast_step<float>(gradients,node,need,selected,line,column);
        else
            autograd_grad_cast_step<double>(gradients,node,need,selected,line,column);
        gradients.erase(node.get());
    }
}

template <typename T>
void autograd_grad_t(
    const std::shared_ptr<AutogradNode>& loss,
    const std::vector<std::shared_ptr<AutogradSlot>>& selected,
    unsigned long long line,unsigned long long column) {
    std::unordered_set<const AutogradNode*> seen;
    std::vector<std::shared_ptr<AutogradNode>> order;
    seen.reserve(64);
    order.reserve(64);
    bool cast_graph=false;
    autograd_topological(loss,seen,order,&cast_graph);
    if(cast_graph){
        autograd_grad_cast(loss,order,selected,line,column);
        return;
    }

    const AutogradNeed need(order,selected);
    need.report();
    std::unordered_map<const AutogradNode*,HostValues<T>> gradients;
    gradients.reserve(order.size());
    gradients[loss.get()]=HostValues<T>(std::vector<T>{T{1}});

    for(auto it=order.rbegin();it!=order.rend();++it){
        const auto& node=*it;
        if(node->dtype!=loss->dtype)
            autograd_fail("autograd graph contains mixed dtypes",line,column);
        const auto found=gradients.find(node.get());
        if(found==gradients.end()) continue;
        const auto& g=found->second;

        if(node->target_identity){
            for(const auto& slot:selected){
                if(!slot || slot->identity!=node->target_identity) continue;
                autograd_accumulate_slot(
                    slot,autograd_tensor_from_gradient<T>(node->dtype,node->shape,g),
                    line,column);
            }
        }

        if(node->parents.empty() || !need.any_parent(*node)){
            gradients.erase(node.get());
            continue;
        }

        if(node->op==AutogradOp::CustomNative){
            // The callback borrows the gradient as a tensor and its gradient
            // outputs come back as tensors; both move by reference, not copy.
            auto gradient_tensor=autograd_tensor_from_gradient<T>(
                node->dtype,node->shape,g);
            quidra::runtime::NodeRuntimeOwner partial_gradient_tensor(gradient_tensor,+[](void* raw){release_managed_tensor(raw);});
            const auto mask=autograd_parent_mask(*node,need);
            std::vector<TensorValue*> input_gradients;
            quidra::runtime::NodeRuntimeOwner partial_inputs(&input_gradients,cleanup_node_tensor_vector);
            try {
                input_gradients=autograd_custom_backward_tensors(
                    *node,gradient_tensor,mask,line,column);
            } catch (...) {
                partial_gradient_tensor.relinquish();release_managed_tensor(gradient_tensor);
                throw;
            }
            partial_gradient_tensor.relinquish();release_managed_tensor(gradient_tensor);
            for(std::size_t index=0;index<input_gradients.size();++index){
                if(!input_gradients[index]) continue;
                if(!mask[index]){
                    release_managed_tensor(std::exchange(input_gradients[index],nullptr));
                    continue;
                }
                auto next=autograd_gradient_values<T>(
                    *input_gradients[index],line,column);
                release_managed_tensor(std::exchange(input_gradients[index],nullptr));
                autograd_add_gradient(
                    gradients,node->parents[index],std::move(next));
            }
        }else if(node->op==AutogradOp::Add||node->op==AutogradOp::Sub||
           node->op==AutogradOp::Mul||node->op==AutogradOp::Div){
            const auto* a=node->parents[0]->data.typed<T>().data();
            const auto* b=node->parents[1]->data.typed<T>().data();
            const bool shared_parent=node->parents[0].get()==node->parents[1].get();
            if(shared_parent){
                HostValues<T> combined_values=std::move(found->second);
                const auto count=combined_values.size();
                auto* combined=combined_values.mutable_data();
                for(std::size_t i=0;i<count;++i){
                    const T gradient=combined[i];
                    T left_value{},right_value{};
                    if(node->op==AutogradOp::Add){
                        left_value=gradient;right_value=gradient;
                    }else if(node->op==AutogradOp::Sub){
                        left_value=gradient;right_value=static_cast<T>(-gradient);
                    }else if(node->op==AutogradOp::Mul){
                        left_value=static_cast<T>(gradient*b[i]);
                        right_value=static_cast<T>(gradient*a[i]);
                    }else{
                        left_value=static_cast<T>(gradient/b[i]);
                        const T ga=static_cast<T>(gradient*a[i]);
                        const T bb=static_cast<T>(b[i]*b[i]);
                        right_value=static_cast<T>(-static_cast<T>(ga/bb));
                    }
                    combined[i]=static_cast<T>(left_value+right_value);
                }
                autograd_add_gradient(
                    gradients,node->parents[0],std::move(combined_values));
            }else{
                // Each partial derivative is computed only for a parent that
                // needs it; every element keeps its expression.
                const bool left_needed=need(node->parents[0]);
                const bool right_needed=need(node->parents[1]);
                HostValues<T> left_gradient,right_gradient;
                if(node->op==AutogradOp::Add){
                    if(left_needed && right_needed){
                        left_gradient=std::move(found->second);
                        right_gradient=left_gradient;
                    }else if(left_needed){
                        left_gradient=std::move(found->second);
                    }else{
                        right_gradient=std::move(found->second);
                    }
                }else if(node->op==AutogradOp::Sub){
                    if(right_needed){
                        const auto count=g.size();
                        right_gradient=HostValues<T>::allocate(count);
                        const auto* left=g.data();
                        auto* right=right_gradient.mutable_data();
                        for(std::size_t i=0;i<count;++i)
                            right[i]=static_cast<T>(-left[i]);
                    }
                    if(left_needed) left_gradient=std::move(found->second);
                }else{
                    const auto count=g.size();
                    const auto* gv=g.data();
                    if(left_needed){
                        left_gradient=HostValues<T>::allocate(count);
                        auto* left=left_gradient.mutable_data();
                        if(node->op==AutogradOp::Mul){
                            for(std::size_t i=0;i<count;++i)
                                left[i]=static_cast<T>(gv[i]*b[i]);
                        }else{
                            for(std::size_t i=0;i<count;++i)
                                left[i]=static_cast<T>(gv[i]/b[i]);
                        }
                    }
                    if(right_needed){
                        right_gradient=HostValues<T>::allocate(count);
                        auto* right=right_gradient.mutable_data();
                        if(node->op==AutogradOp::Mul){
                            for(std::size_t i=0;i<count;++i)
                                right[i]=static_cast<T>(gv[i]*a[i]);
                        }else{
                            for(std::size_t i=0;i<count;++i)
                                right[i]=static_cast<T>(
                                    -static_cast<T>(gv[i]*a[i])/
                                    static_cast<T>(b[i]*b[i]));
                        }
                    }
                }
                if(left_needed)
                    autograd_add_gradient(gradients,node->parents[0],std::move(left_gradient));
                if(right_needed)
                    autograd_add_gradient(gradients,node->parents[1],std::move(right_gradient));
            }
        }else if(node->op==AutogradOp::ScalarBinary){
            if(node->parents.size()!=1||node->aux.size()!=1||
               node->aux_index.size()!=2)
                autograd_fail("invalid autograd scalar graph",line,column);
            const auto* input=node->parents[0]->data.typed<T>().data();
            const auto operation=static_cast<int>(node->aux_index[0]);
            const bool scalar_left=node->aux_index[1]!=0;
            const T scalar_value=static_cast<T>(node->aux.scalar_as_double(0));
            // Scalar backward is one-to-one, so transform the completed gradient
            // buffer in place instead of allocating an equally sized temporary.
            HostValues<T> input_gradient_values=std::move(found->second);
            const auto count=input_gradient_values.size();
            auto* input_gradient=input_gradient_values.mutable_data();
            for(std::size_t i=0;i<count;++i){
                const T gradient=input_gradient[i];
                if(operation==abi::tensor_binary_opcode::add)input_gradient[i]=gradient;
                else if(operation==abi::tensor_binary_opcode::subtract)
                    input_gradient[i]=scalar_left?static_cast<T>(-gradient):gradient;
                else if(operation==abi::tensor_binary_opcode::multiply)
                    input_gradient[i]=static_cast<T>(gradient*scalar_value);
                else if(operation==abi::tensor_binary_opcode::power){
                    if(scalar_left)
                        autograd_fail("tensor power requires tensor ^ scalar",line,column);
                    input_gradient[i]=static_cast<T>(
                        gradient*scalar_value*
                        std::pow(input[i],static_cast<T>(scalar_value-T{1})));
                }else if(scalar_left){
                    const T gs=static_cast<T>(gradient*scalar_value);
                    const T xx=static_cast<T>(input[i]*input[i]);
                    input_gradient[i]=static_cast<T>(-static_cast<T>(gs/xx));
                }else{
                    input_gradient[i]=static_cast<T>(gradient/scalar_value);
                }
            }
            autograd_add_gradient(
                gradients,node->parents[0],std::move(input_gradient_values));
        }else if(node->op==AutogradOp::Reshape){
            if(node->parents.size()!=1)
                autograd_fail("invalid tensor reshape graph",line,column);
            quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
            if(g.size()!=node->parents[0]->data.size())
                autograd_fail("tensor reshape backward size mismatch",line,column);
            // Reshape keeps element order and this node's gradient is erased
            // below, so hand the buffer to the parent instead of copying it.
            autograd_add_gradient(
                gradients,node->parents[0],std::move(found->second));
        }else if(node->op==AutogradOp::Transpose){
            if(node->parents.size()!=1||node->aux_index.size()!=2)
                autograd_fail("invalid tensor transpose graph",line,column);
            AutogradBuffer gb(std::move(found->second));
            auto restored=autograd_transpose_values<T>(
                gb,node->shape,node->aux_index[0],node->aux_index[1],line,column);
            autograd_add_gradient(
                gradients,node->parents[0],
                std::move(restored.template typed<T>()));
        }else if(node->op==AutogradOp::Gather){
            if(node->parents.size()!=1)
                autograd_fail("invalid tensor gather graph",line,column);
            auto input_gradient=autograd_gather_backward_values<T>(
                g,autograd_node_count(*node->parents[0]),node->aux_index,
                line,column);
            autograd_add_gradient(
                gradients,node->parents[0],HostValues<T>(std::move(input_gradient)));
        }else if(node->op==AutogradOp::GatherBackward){
            if(node->parents.size()!=1)
                autograd_fail("invalid tensor gather backward graph",line,column);
            auto input_gradient=autograd_gather_values<T>(
                g,node->aux_index,line,column);
            autograd_add_gradient(
                gradients,node->parents[0],HostValues<T>(std::move(input_gradient)));
        }else if(node->op==AutogradOp::Neg){
            if(node->parents.size()!=1)
                autograd_fail("invalid tensor negation graph",line,column);
            // Negation is one-to-one: negate the completed gradient in place.
            HostValues<T> input_gradient=std::move(found->second);
            const auto count=input_gradient.size();
            auto* values=input_gradient.mutable_data();
            for(std::size_t i=0;i<count;++i) values[i]=static_cast<T>(-values[i]);
            autograd_add_gradient(
                gradients,node->parents[0],std::move(input_gradient));
        }

        // No later child can contribute to a node after reverse-topological visit.
        gradients.erase(node.get());
    }
}

TensorValue* autograd_backward_loss(
    void* raw,unsigned long long line,unsigned long long column) {
    if(!raw) autograd_fail("null loss tensor",line,column);
    auto* tensor=static_cast<TensorValue*>(raw);
    if(!tensor->graph)
        autograd_fail("backward() requires a tracked tensor",line,column);
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if(autograd_node_count(*tensor->graph)!=1)
        autograd_fail("backward() requires a scalar tensor",line,column);
    return tensor;
}

void autograd_backward_selected(
    TensorValue* tensor,const std::vector<std::shared_ptr<AutogradSlot>>& selected,bool track,
    unsigned long long line,unsigned long long column) {
    if(track){
        if(autograd_half_dtype(tensor->graph->dtype))
            autograd_fail("backward(track = true) is not available for real16 and real16b tensors",
                          line,column);
        autograd_backward_tracked(tensor->graph,selected,line,column);
        return;
    }
    if(tensor->graph->device_tensor)
        autograd_grad_device(tensor->graph,selected,line,column);
    else if(autograd_compute_dtype(tensor->graph->dtype)==QCORE_DTYPE_FLOAT32)
        autograd_grad_t<float>(tensor->graph,selected,line,column);
    else
        autograd_grad_t<double>(tensor->graph,selected,line,column);
}

extern "C" void quidra_tensor_backward_many(
    void* raw,void** target_raws,const unsigned char* target_kinds,
    unsigned long long target_count,bool track,
    unsigned long long line,unsigned long long column) {
    quidra::counters::step_boundary(quidra::counters::StepEvent::Backward); // qcount
    quidra::device::observation_step_boundary();
    quidra::profile::advance_capture_step();
    quidra::timeline::note_point(quidra::timeline::Point::StepBoundary);
    quidra::profile::Range backward_range("quidra", "backward");
    auto* tensor=autograd_backward_loss(raw,line,column);
    if(target_count==0)
        autograd_fail("backward() requires at least one gradient target",line,column);
    if(!target_raws||!target_kinds)
        autograd_fail("invalid gradient target list",line,column);

    std::vector<std::shared_ptr<AutogradSlot>> selected;
    selected.reserve(static_cast<std::size_t>(target_count));
    std::unordered_set<const AutogradSlot*> unique;
    unique.reserve(static_cast<std::size_t>(target_count));

    for(unsigned long long i=0;i<target_count;++i){
        std::shared_ptr<AutogradSlot> slot;
        if(target_kinds[i]==0){
            if(!target_raws[i])
                autograd_fail("null gradient target tensor",line,column);
            auto* target=static_cast<TensorValue*>(target_raws[i]);
            if(!target->grad_slot)
                autograd_fail(
                    "tensor.backward gradient tensor target is not tracked; call track() first",
                    line,column);
            slot=target->grad_slot;
        }else if(target_kinds[i]==1){
            auto* handle=autograd_target_from_value(target_raws[i]);
            if(!handle||!handle->slot)
                autograd_fail("invalid autograd target",line,column);
            slot=handle->slot;
        }else{
            autograd_fail("invalid gradient target kind",line,column);
        }
        if(unique.insert(slot.get()).second) selected.push_back(std::move(slot));
    }

    autograd_backward_selected(tensor,selected,track,line,column);
}


extern "C" void quidra_tensor_backward_many_with_autograd_targets(
    void* raw,void** target_raws,const unsigned char* target_kinds,
    unsigned long long target_count,void* autograd_targets,bool track,
    unsigned long long line,unsigned long long column) {
    if(!autograd_targets)
        autograd_fail("invalid autograd target array",line,column);

    long long signed_count=0;
    std::memcpy(&signed_count,autograd_targets,sizeof(signed_count));
    if(signed_count<0)
        autograd_fail("invalid autograd target array length",line,column);
    const auto dynamic_count=static_cast<unsigned long long>(signed_count);
    if(dynamic_count>
       std::numeric_limits<unsigned long long>::max()-target_count)
        runtime_allocation_failure();

    std::vector<void*> combined_targets;
    std::vector<unsigned char> combined_kinds;
    try{
        const auto total=static_cast<std::size_t>(
            target_count+dynamic_count);
        combined_targets.reserve(total);
        combined_kinds.reserve(total);
        for(unsigned long long i=0;i<target_count;++i){
            combined_targets.push_back(target_raws[i]);
            combined_kinds.push_back(target_kinds[i]);
        }
        auto* data=static_cast<unsigned char*>(autograd_targets)
            +sizeof(long long);
        for(unsigned long long i=0;i<dynamic_count;++i){
            void* target=nullptr;
            std::memcpy(
                &target,
                data+static_cast<std::size_t>(i)*sizeof(void*),
                sizeof(target));
            combined_targets.push_back(target);
            combined_kinds.push_back(1);
        }
    }catch(...){
        runtime_allocation_failure();
    }

    quidra_tensor_backward_many(
        raw,
        combined_targets.empty()?nullptr:combined_targets.data(),
        combined_kinds.empty()?nullptr:combined_kinds.data(),
        static_cast<unsigned long long>(combined_targets.size()),
        track,line,column);
}


namespace {

std::size_t tensor_broadcast_index(const TensorValue& tensor,
                                   const std::vector<long long>& output_shape,
                                   std::size_t logical) {
    std::size_t index = tensor.offset;
    for (std::size_t axis = output_shape.size(); axis-- > 0;) {
        const auto out_dimension = static_cast<std::size_t>(output_shape[axis]);
        const auto coordinate = out_dimension == 0 ? 0 : logical % out_dimension;
        if (out_dimension != 0) logical /= out_dimension;
        const auto input_coordinate = tensor.shape[axis] == 1 ? 0 : coordinate;
        index += input_coordinate * static_cast<std::size_t>(tensor.strides[axis]);
    }
    return index;
}

template<bool CompatibilityProven = false>
std::vector<long long> tensor_broadcast_shape(const TensorValue& left,
                                              const TensorValue& right,
                                              unsigned long long line,
                                              unsigned long long column) {
    if constexpr (!CompatibilityProven) {
        quidra::counters::add(quidra::counters::Id::RuntimeBroadcastValidations);
        if (left.shape.size() != right.shape.size()) {
            tensor_fail("tensor broadcasting requires identical ranks", line, column);
        }
    }
    std::vector<long long> shape(left.shape.size());
    for (std::size_t i = 0; i < shape.size(); ++i) {
        const auto a = left.shape[i];
        const auto b = right.shape[i];
        if constexpr (!CompatibilityProven) {
            if (a != b && a != 1 && b != 1) {
                tensor_fail("tensor shapes are not broadcast-compatible", line, column);
            }
        }
        // A singleton axis takes the other side's extent, including 0;
        // max() would instead give 1 and read one element of an empty
        // operand.
        shape[i] = a == 1 ? b : a;
    }
    return shape;
}

template<bool RankProven = false>
std::vector<std::size_t> tensor_broadcast_logical_indices(
    const TensorValue& source,const std::vector<long long>& output_shape,
    unsigned long long line,unsigned long long column) {
    if constexpr (!RankProven) {
        quidra::counters::add(quidra::counters::Id::RuntimeBroadcastValidations);
        if(source.shape.size()!=output_shape.size())
            tensor_fail("tensor broadcasting requires identical ranks",line,column);
    }
    const auto count=tensor_element_count(output_shape,line,column);
    const auto source_strides=tensor_contiguous_strides(source.shape);
    std::vector<std::size_t> indices;
    try{
        indices.resize(count);
    }catch(...){
        runtime_allocation_failure();
    }
    for(std::size_t logical=0;logical<count;++logical){
        auto remaining=logical;
        std::size_t source_logical=0;
        for(std::size_t axis=output_shape.size();axis-->0;){
            const auto extent=static_cast<std::size_t>(output_shape[axis]);
            const auto coordinate=extent==0?std::size_t{0}:remaining%extent;
            if(extent!=0) remaining/=extent;
            const auto input_coordinate=source.shape[axis]==1?std::size_t{0}:coordinate;
            source_logical+=input_coordinate*
                static_cast<std::size_t>(source_strides[axis]);
        }
        indices[logical]=source_logical;
    }
    return indices;
}

TensorStorage* tensor_gpu_expand_storage(
    const TensorValue& source,
    const std::vector<long long>& output_shape,
    unsigned long long line,
    unsigned long long column) {
    tensor_require_initialized(source, line, column);
    const auto count = tensor_element_count(output_shape, line, column);
    auto* output = tensor_storage_create(
        source.storage->dtype, count, abi::tensor_fill_mode::uninitialized, source.storage->device, line, column);
    if (tensor_gpu_strided_enabled(*source.storage)) {
        const auto strides = tensor_broadcast_strides(source, output_shape);
        if (tensor_strided_read_within_storage(source, output_shape, strides)) {
            std::string backend_error;
            if (!quidra::device::compute_strided_copy(
                    output->gpu_buffer, source.storage->gpu_buffer,
                    source.storage->dtype, source.offset, output_shape,
                    strides, backend_error)) {
                tensor_storage_release(output);
                tensor_fail(backend_error.c_str(), line, column);
            }
            tracker_mark_complete(output->initialization);
            return output;
        }
    }
    std::vector<std::uint64_t> indices;
    try {
        indices.resize(count);
    } catch (...) {
        tensor_storage_release(output);
        runtime_allocation_failure();
    }
    for (std::size_t logical = 0; logical < count; ++logical) {
        const auto storage_index =
            tensor_broadcast_index(source, output_shape, logical);
        if (storage_index >= source.storage->count) {
            tensor_storage_release(output);
            tensor_fail("tensor broadcast view exceeds storage", line, column);
        }
        indices[logical] = static_cast<std::uint64_t>(storage_index);
    }
    std::string backend_error;
    if (!quidra::device::compute_gather(
            output->gpu_buffer, source.storage->gpu_buffer,
            source.storage->dtype, indices.data(), count, backend_error)) {
        tensor_storage_release(output);
        tensor_fail(backend_error.c_str(), line, column);
    }
    // The source was required to be initialized above and every output element
    // is gathered from it, so the whole output is initialized at once.
    tracker_mark_complete(output->initialization);
    return output;
}

template <typename T>
bool tensor_add_checked(T a, T b, T& out) {
    if constexpr (real_element<T> || complex_element<T>) {
        out = static_cast<T>(a + b);
        return true;
    } else if constexpr (std::is_signed_v<T>) {
        if ((b > 0 && a > std::numeric_limits<T>::max() - b) ||
            (b < 0 && a < std::numeric_limits<T>::min() - b)) return false;
        out = static_cast<T>(a + b);
        return true;
    } else {
        if (a > std::numeric_limits<T>::max() - b) return false;
        out = static_cast<T>(a + b);
        return true;
    }
}

template <typename T>
bool tensor_sub_checked(T a, T b, T& out) {
    if constexpr (real_element<T> || complex_element<T>) {
        out = static_cast<T>(a - b);
        return true;
    } else if constexpr (std::is_signed_v<T>) {
        if ((b > 0 && a < std::numeric_limits<T>::min() + b) ||
            (b < 0 && a > std::numeric_limits<T>::max() + b)) return false;
        out = static_cast<T>(a - b);
        return true;
    } else {
        if (a < b) return false;
        out = static_cast<T>(a - b);
        return true;
    }
}

template <typename T>
bool tensor_mul_checked(T a, T b, T& out) {
    if constexpr (real_element<T> || complex_element<T>) {
        out = static_cast<T>(a * b);
        return true;
    } else if constexpr (std::is_unsigned_v<T>) {
        if (b != 0 && a > std::numeric_limits<T>::max() / b) return false;
        out = static_cast<T>(a * b);
        return true;
    } else {
        if (a == 0 || b == 0) {
            out = 0;
            return true;
        }
        if (a == -1 && b == std::numeric_limits<T>::min()) return false;
        if (b == -1 && a == std::numeric_limits<T>::min()) return false;
        if (a > 0) {
            if ((b > 0 && a > std::numeric_limits<T>::max() / b) ||
                (b < 0 && b < std::numeric_limits<T>::min() / a)) return false;
        } else {
            if ((b > 0 && a < std::numeric_limits<T>::min() / b) ||
                (b < 0 && a < std::numeric_limits<T>::max() / b)) return false;
        }
        out = static_cast<T>(a * b);
        return true;
    }
}

template <typename T>
bool integer_power_checked(T base, std::uint64_t exponent, T& out) {
    out = T{1};
    T factor = base;
    while (exponent != 0) {
        if ((exponent & 1U) != 0) {
            T next{};
            if (!tensor_mul_checked(out, factor, next)) return false;
            out = next;
        }
        exponent >>= 1U;
        if (exponent != 0) {
            T next{};
            if (!tensor_mul_checked(factor, factor, next)) return false;
            factor = next;
        }
    }
    return true;
}

template <typename T>
bool tensor_apply_operator(T left, T right, int operation, T& out) {
    switch (operation) {
        case abi::tensor_binary_opcode::add: return tensor_add_checked(left, right, out);
        case abi::tensor_binary_opcode::subtract: return tensor_sub_checked(left, right, out);
        case abi::tensor_binary_opcode::multiply: return tensor_mul_checked(left, right, out);
        case abi::tensor_binary_opcode::divide:
            if constexpr (std::is_integral_v<T>) {
                if (right == 0) return false;
                if constexpr (std::is_signed_v<T>) {
                    if (left == std::numeric_limits<T>::min() && right == -1) return false;
                }
            }
            out = static_cast<T>(left / right);
            return true;
        case abi::tensor_binary_opcode::remainder:
            if constexpr (std::is_integral_v<T>) {
                if (right == 0) return false;
                if constexpr (std::is_signed_v<T>) {
                    if (left == std::numeric_limits<T>::min() && right == -1) {
                        out = 0;
                        return true;
                    }
                }
                out = static_cast<T>(left % right);
                return true;
            } else {
                return false;
            }
        case abi::tensor_binary_opcode::power:
            if constexpr (std::is_integral_v<T>) {
                if constexpr (std::is_signed_v<T>) {
                    if (right < 0) return false;
                }
                return integer_power_checked(
                    left, static_cast<std::uint64_t>(right), out);
            } else if constexpr (half_element<T>) {
                out = quidra::abi::half::pow(left, right);
                return true;
            } else if constexpr (complex_element<T>) {
                // Complex tensors have no power (the checker rejects it).
                return false;
            } else {
                out = static_cast<T>(std::pow(left, right));
                return true;
            }
        default:
            return false;
    }
}

// A real tensor element type: its operation results are tested for NaN.
template <typename T>
inline constexpr bool tensor_real_element = real_element<T> || complex_element<T>;

// The text a real tensor's failures name its type by (tensor<real32>).
std::string tensor_type_text(int dtype) {
    return "tensor<" + std::string(abi::dtype_info(static_cast<abi::Dtype>(dtype)).quidra_name) + ">";
}

// A real tensor operation whose result is NaN keeps the established error
// categories: NONFINITE_RESULT, or DIVISION_BY_ZERO for a zero/zero quotient.
[[noreturn]] void tensor_real_failure(int dtype, abi::RealOperation operation, bool zero_divisor,
                                      unsigned long long line, unsigned long long column) {
    end_package_hold_before_failure(line, column);
    const auto type = tensor_type_text(dtype);
    quidra::runtime::report_failure(
        zero_divisor ? abi::FailureReason::real_zero_divisor : abi::FailureReason::real_invalid,
        abi::FailureArgs{.type = type, .operation = abi::real_operation_info(operation).name},
        line, column);
}

abi::RealOperation tensor_real_operation(int operation) {
    switch (operation) {
        case abi::tensor_binary_opcode::add: return abi::RealOperation::addition;
        case abi::tensor_binary_opcode::subtract: return abi::RealOperation::subtraction;
        case abi::tensor_binary_opcode::multiply: return abi::RealOperation::multiplication;
        case abi::tensor_binary_opcode::divide: return abi::RealOperation::division;
        default: break;
    }
    return abi::RealOperation::power;
}

// The powers without a value (POWER_DOMAIN), as for scalars: 0 ^ 0 and a
// negative real base with a non-integer exponent. Zero to a negative real
// exponent is a signed infinity under the extended-real policy.
[[noreturn]] void tensor_power_domain_failure(abi::FailureReason reason, unsigned long long line,
                                              unsigned long long column) {
    end_package_hold_before_failure(line, column);
    quidra::runtime::report_failure(reason, {}, line, column);
}

template <typename T>
void tensor_power_domain(T base, T exponent, unsigned long long line, unsigned long long column) {
    if (base == T{0} && exponent == T{0})
        tensor_power_domain_failure(abi::FailureReason::zero_power_zero, line, column);
    if constexpr (real_element<T>) {
        const double whole = element_value(exponent);
        if (base < T{0} && std::trunc(whole) != whole)
            tensor_power_domain_failure(abi::FailureReason::negative_base_fractional_exponent,
                                        line, column);
    }
}

// One element of a tensor binary: the domain of a power, the operation
// (operation_failure() when a checked integer operation fails), and for a
// real type the absence of NaN. Signed infinity is valid. The first failing
// element in logical order is the one reported.
template <typename T, typename Failure>
T tensor_element_result(T left, T right, int operation, int dtype, const Failure& operation_failure,
                        unsigned long long line, unsigned long long column) {
    if constexpr (tensor_real_element<T>) {
        if (element_is_nan(left) || element_is_nan(right))
            tensor_real_failure(dtype, tensor_real_operation(operation), false, line, column);
    }
    if (operation == abi::tensor_binary_opcode::power) tensor_power_domain(left, right, line, column);
    T result{};
    if (!tensor_apply_operator(left, right, operation, result)) operation_failure();
    if constexpr (tensor_real_element<T>) {
        if (element_is_nan(result))
            tensor_real_failure(dtype, tensor_real_operation(operation),
                                operation == abi::tensor_binary_opcode::divide && right == T{0},
                                line, column);
    }
    return result;
}

template <typename T>
void tensor_binary_typed(const TensorValue& primary, const TensorValue* other,
                         const void* scalar, int scalar_side, int operation,
                         TensorStorage& output,
                         const std::vector<long long>& output_shape,
                         unsigned long long line, unsigned long long column) {
    const auto count = tensor_element_count(output_shape, line, column);
    T scalar_value{};
    if (scalar_side != abi::scalar_side::none) {
        if (!scalar) tensor_fail("missing tensor scalar operand", line, column);
        std::memcpy(&scalar_value, scalar, sizeof(T));
    }
    const auto operation_failure = [&]() {
        tensor_fail(operation == abi::tensor_binary_opcode::divide || operation == abi::tensor_binary_opcode::remainder
                        ? "invalid tensor division/remainder or integer overflow"
                        : operation == abi::tensor_binary_opcode::power
                            ? "invalid tensor power domain or integer overflow"
                            : "tensor integer arithmetic overflow",
                    line, column);
    };

    // Fully initialized operands that already have the output shape and are a
    // single dense run of storage hold logical element i at storage index
    // offset + i, all proven inside storage. Read them directly instead of
    // recomputing a broadcast index and a tracker bit per element. Element
    // order, arithmetic and the first failing element are unchanged.
    const auto dense_input = [&](const TensorValue& operand) {
        return operand.shape == output_shape &&
               operand.storage->initialization.fully_initialized &&
               tensor_view_dense_in_storage(operand, count);
    };
    if (dense_input(primary) && (!other || dense_input(*other))) {
        const auto* primary_data =
            primary.storage->data.data() + primary.offset * sizeof(T);
        auto* output_data = output.data.data();
        const auto load = [](const unsigned char* data, std::size_t logical) {
            T value{};
            std::memcpy(&value, data + logical * sizeof(T), sizeof(T));
            return value;
        };
        const auto store = [&](std::size_t logical, T left, T right) {
            const T result = tensor_element_result(left, right, operation, output.dtype,
                                                   operation_failure, line, column);
            std::memcpy(output_data + logical * sizeof(T), &result, sizeof(T));
        };
        if (other) {
            const auto* other_data =
                other->storage->data.data() + other->offset * sizeof(T);
            for (std::size_t logical = 0; logical < count; ++logical)
                store(logical, load(primary_data, logical), load(other_data, logical));
        } else if (scalar_side == abi::scalar_side::left) {
            for (std::size_t logical = 0; logical < count; ++logical)
                store(logical, scalar_value, load(primary_data, logical));
        } else {
            for (std::size_t logical = 0; logical < count; ++logical)
                store(logical, load(primary_data, logical), scalar_value);
        }
        return;
    }

    for (std::size_t logical = 0; logical < count; ++logical) {
        const auto primary_index =
            other ? tensor_broadcast_index(primary, output_shape, logical)
                  : tensor_storage_index(primary, logical);
        if (!tracker_bit(primary.storage->initialization, primary_index)) {
            runtime_uninitialized_failure(line, column);
        }
        T primary_value{};
        std::memcpy(&primary_value,
                    primary.storage->data.data() + primary_index * sizeof(T), sizeof(T));

        T left{};
        T right{};
        if (other) {
            const auto other_index = tensor_broadcast_index(*other, output_shape, logical);
            if (!tracker_bit(other->storage->initialization, other_index)) {
                runtime_uninitialized_failure(line, column);
            }
            T other_value{};
            std::memcpy(&other_value,
                        other->storage->data.data() + other_index * sizeof(T), sizeof(T));
            left = primary_value;
            right = other_value;
        } else if (scalar_side == abi::scalar_side::left) {
            left = scalar_value;
            right = primary_value;
        } else {
            left = primary_value;
            right = scalar_value;
        }

        const T result = tensor_element_result(left, right, operation, output.dtype,
                                               operation_failure, line, column);
        std::memcpy(output.data.data() + logical * sizeof(T), &result, sizeof(T));
    }
}

} // namespace

[[noreturn]] void scalar_power_failure(
    abi::FailureReason reason, unsigned long long line, unsigned long long column) {
    quidra::runtime::report_failure(reason, {}, line, column);
}

template <typename T>
long long scalar_signed_power(
    long long base, unsigned long long exponent,
    unsigned long long line, unsigned long long column) {
    T result{};
    if (!integer_power_checked(static_cast<T>(base), exponent, result))
        scalar_power_failure(abi::FailureReason::integer_overflow, line, column);
    return static_cast<long long>(result);
}

template <typename T>
unsigned long long scalar_unsigned_power(
    unsigned long long base, unsigned long long exponent,
    unsigned long long line, unsigned long long column) {
    T result{};
    if (!integer_power_checked(static_cast<T>(base), exponent, result))
        scalar_power_failure(abi::FailureReason::integer_overflow, line, column);
    return static_cast<unsigned long long>(result);
}

extern "C" long long quidra_integer_pow_signed(
    long long base, long long exponent, int bits,
    unsigned long long line, unsigned long long column) {
    if (exponent < 0)
        scalar_power_failure(abi::FailureReason::negative_integer_exponent, line, column);
    if (base == 0 && exponent == 0)
        scalar_power_failure(abi::FailureReason::zero_power_zero, line, column);
    const auto power = static_cast<unsigned long long>(exponent);
    switch (bits) {
        case 8: return scalar_signed_power<std::int8_t>(base, power, line, column);
        case 16: return scalar_signed_power<std::int16_t>(base, power, line, column);
        case 32: return scalar_signed_power<std::int32_t>(base, power, line, column);
        case 64: return scalar_signed_power<std::int64_t>(base, power, line, column);
        default:
            scalar_power_failure(abi::FailureReason::invalid_integer_power_width, line, column);
    }
}

extern "C" unsigned long long quidra_integer_pow_unsigned(
    unsigned long long base, unsigned long long exponent, int bits,
    unsigned long long line, unsigned long long column) {
    if (base == 0 && exponent == 0)
        scalar_power_failure(abi::FailureReason::zero_power_zero, line, column);
    switch (bits) {
        case 8: return scalar_unsigned_power<std::uint8_t>(base, exponent, line, column);
        case 16: return scalar_unsigned_power<std::uint16_t>(base, exponent, line, column);
        case 32: return scalar_unsigned_power<std::uint32_t>(base, exponent, line, column);
        case 64: return scalar_unsigned_power<std::uint64_t>(base, exponent, line, column);
        default:
            scalar_power_failure(abi::FailureReason::invalid_integer_power_width, line, column);
    }
}

extern "C" void* quidra_tensor_unary(void* raw, int operation,
                                      unsigned long long line,
                                      unsigned long long column) {
    quidra::profile::Range operation_range("core", "tensor unary");
    if (!raw || operation != abi::tensor_unary_opcode::negate) {
        tensor_fail("invalid tensor unary operation", line, column);
    }
    auto& source = *static_cast<TensorValue*>(raw);
    const auto dtype = source.storage->dtype;
    if (dtype == QCORE_DTYPE_UINT8 || dtype == QCORE_DTYPE_UINT16 || dtype == QCORE_DTYPE_UINT32 || dtype == QCORE_DTYPE_UINT64) {
        tensor_fail("tensor negation requires a signed numeric tensor", line, column);
    }
    tensor_require_initialized(source, line, column);
    const auto count = tensor_logical_count(source);

    if (!tensor_on_cpu(*source.storage)) {
        TensorStorage* materialized = nullptr;
        const TensorStorage* input = source.storage;
        std::size_t input_offset =
            source.offset * tensor_dtype_bytes(source.storage->dtype);
        if (!tensor_is_contiguous_value(source)) {
            materialized = tensor_gpu_materialize_storage(source, line, column);
            input = materialized;
            input_offset = 0;
        }
        auto* output = tensor_storage_create(
            dtype, count, gpu_kernel_output_fill(dtype), source.storage->device,
            line, column);
        std::string backend_error;
        const bool ok = quidra::device::compute_unary(
            output->gpu_buffer, input->gpu_buffer, input_offset,
            dtype, operation, count, backend_error);
        if (materialized) tensor_storage_release(materialized);
        if (!ok) {
            tensor_storage_release(output);
            tensor_fail(backend_error.c_str(), line, column);
        }
        auto* result = tensor_descriptor(
            output, source.shape, tensor_contiguous_strides(source.shape), 0);
        quidra::runtime::NodeRuntimeOwner partial_graph_result_11(result,+[](void* raw){release_managed_tensor(raw);});
        // Negation of a tracked tensor is recorded in the autograd graph
        // instead of silently cutting it.
        tensor_attach_view_graph(result, source, AutogradOp::Neg, {}, line, column);
        partial_graph_result_11.relinquish();
        return result;
    }

    auto* output = tensor_storage_create(dtype, count, abi::tensor_fill_mode::zeros);
    auto run = [&](auto tag) {
        using T = decltype(tag);
        for (std::size_t i = 0; i < count; ++i) {
            const auto source_index = tensor_storage_index(source, i);
            T value{};
            std::memcpy(&value,
                        source.storage->data.data() + source_index * sizeof(T),
                        sizeof(T));
            T result{};
            if constexpr (std::is_integral_v<T>) {
                if (!tensor_sub_checked(T{}, value, result)) {
                    tensor_storage_release(output);
                    tensor_fail("tensor integer negation overflow", line, column);
                }
            } else {
                result = static_cast<T>(-value);
            }
            std::memcpy(output->data.data() + i * sizeof(T), &result, sizeof(T));
        }
    };
    switch (dtype) {
        case QCORE_DTYPE_INT64: run(std::int64_t{}); break;
        case QCORE_DTYPE_INT8: run(std::int8_t{}); break;
        case QCORE_DTYPE_INT16: run(std::int16_t{}); break;
        case QCORE_DTYPE_INT32: run(std::int32_t{}); break;
        case QCORE_DTYPE_FLOAT64: run(double{}); break;
        case QCORE_DTYPE_FLOAT32: run(float{}); break;
        case QCORE_DTYPE_FLOAT16: run(Real16{}); break;
        case QCORE_DTYPE_BFLOAT16: run(Real16b{}); break;
        case QCORE_DTYPE_COMPLEX_FLOAT16: run(ComplexElement<Real16>{}); break;
        case QCORE_DTYPE_COMPLEX_BFLOAT16: run(ComplexElement<Real16b>{}); break;
        case QCORE_DTYPE_COMPLEX_FLOAT32: run(ComplexElement<float>{}); break;
        case QCORE_DTYPE_COMPLEX_FLOAT64: run(ComplexElement<double>{}); break;
        default:
            tensor_storage_release(output);
            tensor_fail("invalid tensor element type for negation", line, column);
    }
    auto* result = tensor_descriptor(
        output, source.shape, tensor_contiguous_strides(source.shape), 0);
    quidra::runtime::NodeRuntimeOwner partial_graph_result_12(result,+[](void* raw){release_managed_tensor(raw);});
    tensor_attach_view_graph(result, source, AutogradOp::Neg, {}, line, column);
    partial_graph_result_12.relinquish();
    return result;
}

template<bool ShapeProven>
static void* tensor_compare_entry(
    void* primary_raw, void* other_raw, void* scalar, int scalar_side, int operation,
    unsigned long long line, unsigned long long column) {
    if (!primary_raw) tensor_fail("null tensor comparison operand", line, column);
    auto* primary = static_cast<TensorValue*>(primary_raw);
    auto* other = static_cast<TensorValue*>(other_raw);
    if (other && scalar_side != abi::scalar_side::none) tensor_fail("invalid tensor comparison operands", line, column);
    if (!other && scalar_side != abi::scalar_side::left && scalar_side != abi::scalar_side::right)
        tensor_fail("invalid tensor scalar comparison side", line, column);
    if (other && primary->storage->dtype != other->storage->dtype)
        tensor_fail("tensor comparison requires identical element types", line, column);
    if (other) quidra::counters::add(quidra::counters::Id::RuntimeDeviceChecks);
    if (other && primary->storage->device != other->storage->device)
        tensor_fail("tensor operands are on different devices; use an explicit .gpu(n) or .cpu() transfer",
                    line, column);
    if constexpr (!ShapeProven) {
        if (other) quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
        if (other && primary->shape != other->shape)
            tensor_fail("tensor comparison requires identical shape", line, column);
    }
    if (operation < abi::tensor_comparison_opcode::equal || operation > abi::tensor_comparison_opcode::greater_equal)
        tensor_fail("invalid tensor comparison operation", line, column);
    tensor_require_initialized(*primary, line, column);
    if (other) tensor_require_initialized(*other, line, column);

    if(!tensor_on_cpu(*primary->storage) &&
       (autograd_half_dtype(primary->storage->dtype)||abi::complex_dtype_code(primary->storage->dtype))){
        AutogradDeviceDenseInput left(*primary,line,column);
        AutogradDeviceDenseInput right;
        if(other)right.reset(*other,line,column);
        const auto count=tensor_logical_count(*primary);
        auto* output=tensor_storage_create(QCORE_DTYPE_BOOL,count,abi::tensor_fill_mode::write_only,
                                           primary->storage->device,line,column);
        std::string error;
        if(!quidra::device::compute_compare_elements(output->gpu_buffer,left->storage->gpu_buffer,
             other?right->storage->gpu_buffer:nullptr,scalar,scalar_side,primary->storage->dtype,operation,count,error)){
            tensor_storage_release(output);tensor_fail(error.c_str(),line,column);
        }
        return tensor_descriptor(output,primary->shape,tensor_contiguous_strides(primary->shape),0);
    }

    const int original_device = primary->storage->device;
    TensorStorage* primary_cpu_storage = nullptr;
    TensorStorage* other_cpu_storage = nullptr;
    TensorValue primary_cpu{};
    TensorValue other_cpu{};
    TensorValue* left_tensor = primary;
    TensorValue* right_tensor = other;
    if (!tensor_on_cpu(*primary->storage)) {
        primary_cpu_storage = tensor_transfer_storage(*primary, -1, line, column);
        primary_cpu = TensorValue{primary_cpu_storage, primary->shape,
                                  tensor_contiguous_strides(primary->shape), 0, {}, {}};
        left_tensor = &primary_cpu;
        if (other) {
            other_cpu_storage = tensor_transfer_storage(*other, -1, line, column);
            other_cpu = TensorValue{other_cpu_storage, other->shape,
                                    tensor_contiguous_strides(other->shape), 0, {}, {}};
            right_tensor = &other_cpu;
        }
    }

    const auto count = tensor_logical_count(*left_tensor);
    auto* output = tensor_storage_create(QCORE_DTYPE_BOOL, count, abi::tensor_fill_mode::zeros);
    const int dtype = left_tensor->storage->dtype;
    const auto run = [&](auto tag) {
        using T = decltype(tag);
        T scalar_value{};
        if (!right_tensor) std::memcpy(&scalar_value, scalar, sizeof(T));
        for (std::size_t i = 0; i < count; ++i) {
            T primary_value{};
            const auto pi = tensor_storage_index(*left_tensor, i);
            std::memcpy(&primary_value,
                        left_tensor->storage->data.data() + pi * sizeof(T), sizeof(T));
            T a{}, b{};
            if (right_tensor) {
                T other_value{};
                const auto oi = tensor_storage_index(*right_tensor, i);
                std::memcpy(&other_value,
                            right_tensor->storage->data.data() + oi * sizeof(T), sizeof(T));
                a = primary_value; b = other_value;
            } else if (scalar_side == abi::scalar_side::left) {
                a = scalar_value; b = primary_value;
            } else {
                a = primary_value; b = scalar_value;
            }
            bool match = false;
            if constexpr (complex_element<T>) {
                // Complex values have no order (the checker rejects it).
                if (operation == abi::tensor_comparison_opcode::equal) match = a == b;
                else if (operation == abi::tensor_comparison_opcode::not_equal) match = a != b;
                else tensor_fail("complex values have no ordering", line, column);
            } else {
                switch (operation) {
                    case abi::tensor_comparison_opcode::equal: match = a == b; break;
                    case abi::tensor_comparison_opcode::not_equal: match = a != b; break;
                    case abi::tensor_comparison_opcode::less: match = a < b; break;
                    case abi::tensor_comparison_opcode::less_equal: match = a <= b; break;
                    case abi::tensor_comparison_opcode::greater: match = a > b; break;
                    case abi::tensor_comparison_opcode::greater_equal: match = a >= b; break;
                }
            }
            output->data[i] = static_cast<unsigned char>(match ? 1 : 0);
        }
    };
    switch (dtype) {
        case QCORE_DTYPE_INT64: run(std::int64_t{}); break;
        case QCORE_DTYPE_INT8: run(std::int8_t{}); break;
        case QCORE_DTYPE_INT16: run(std::int16_t{}); break;
        case QCORE_DTYPE_INT32: run(std::int32_t{}); break;
        case QCORE_DTYPE_UINT8: run(std::uint8_t{}); break;
        case QCORE_DTYPE_UINT16: run(std::uint16_t{}); break;
        case QCORE_DTYPE_UINT32: run(std::uint32_t{}); break;
        case QCORE_DTYPE_UINT64: run(std::uint64_t{}); break;
        case QCORE_DTYPE_FLOAT64: run(double{}); break;
        case QCORE_DTYPE_FLOAT32: run(float{}); break;
        case QCORE_DTYPE_FLOAT16: run(Real16{}); break;
        case QCORE_DTYPE_BFLOAT16: run(Real16b{}); break;
        case QCORE_DTYPE_COMPLEX_FLOAT16: run(ComplexElement<Real16>{}); break;
        case QCORE_DTYPE_COMPLEX_BFLOAT16: run(ComplexElement<Real16b>{}); break;
        case QCORE_DTYPE_COMPLEX_FLOAT32: run(ComplexElement<float>{}); break;
        case QCORE_DTYPE_COMPLEX_FLOAT64: run(ComplexElement<double>{}); break;
        default:
            tensor_storage_release(output);
            if (primary_cpu_storage) tensor_storage_release(primary_cpu_storage);
            if (other_cpu_storage) tensor_storage_release(other_cpu_storage);
            tensor_fail("invalid tensor comparison element type", line, column);
    }
    if (primary_cpu_storage) tensor_storage_release(primary_cpu_storage);
    if (other_cpu_storage) tensor_storage_release(other_cpu_storage);

    auto* cpu_result = tensor_descriptor(output, primary->shape,
                                         tensor_contiguous_strides(primary->shape), 0);
    if (original_device < 0) return cpu_result;
    auto* gpu_storage = tensor_transfer_storage(*cpu_result, original_device, line, column);
    release_managed_tensor(cpu_result);
    return tensor_descriptor(gpu_storage, primary->shape,
                             tensor_contiguous_strides(primary->shape), 0);
}

extern "C" void* quidra_tensor_compare(
    void* primary_raw, void* other_raw, void* scalar, int scalar_side, int operation,
    unsigned long long line, unsigned long long column) {
    return tensor_compare_entry<false>(primary_raw, other_raw, scalar, scalar_side, operation, line, column);
}

extern "C" void* quidra_tensor_compare_proven(
    void* primary_raw, void* other_raw, void* scalar, int scalar_side, int operation,
    unsigned long long line, unsigned long long column) {
    return tensor_compare_entry<true>(primary_raw, other_raw, scalar, scalar_side, operation, line, column);
}

extern "C" bool quidra_tensor_bool_reduce(
    void* raw, bool all, unsigned long long line, unsigned long long column) {
    if (!raw) tensor_fail("null boolean tensor", line, column);
    auto* tensor = static_cast<TensorValue*>(raw);
    if (tensor->storage->dtype != QCORE_DTYPE_BOOL)
        tensor_fail("all()/any() require tensor<bool>", line, column);
    tensor_require_initialized(*tensor, line, column);
    TensorStorage* cpu_storage = nullptr;
    TensorValue cpu{};
    TensorValue* source = tensor;
    if (!tensor_on_cpu(*tensor->storage)) {
        cpu_storage = tensor_transfer_storage(*tensor, -1, line, column);
        cpu = TensorValue{cpu_storage, tensor->shape,
                          tensor_contiguous_strides(tensor->shape), 0, {}, {}};
        source = &cpu;
    }
    const auto count = tensor_logical_count(*source);
    bool result = all;
    for (std::size_t i = 0; i < count; ++i) {
        const auto index = tensor_storage_index(*source, i);
        const bool value = source->storage->data[index] != 0;
        if (all && !value) { result = false; break; }
        if (!all && value) { result = true; break; }
    }
    if (cpu_storage) tensor_storage_release(cpu_storage);
    return result;
}

template<bool CompatibilityProven = false>
void tensor_attach_binary_graph(
    TensorValue* result,TensorValue* primary,TensorValue* other,
    void* scalar,int scalar_side,int operation,
    unsigned long long line,unsigned long long column) {
    if(!result||!primary) return;
    if(!primary->graph&&(!other||!other->graph)) return;
    if(!autograd_dtype(primary->storage->dtype))
        autograd_fail("tracked tensor arithmetic requires a floating dtype",line,column);

    auto graph_operand=[&](TensorValue* operand)->std::shared_ptr<AutogradNode>{
        if(operand->shape==result->shape)
            return operand->graph
                ? operand->graph : autograd_constant_node(*operand,line,column);

        // Devices with strided compute keep the broadcast operand as a
        // stride-0 view instead of gathering an expanded copy. The forward
        // pass required it to be initialized. The view shares the operand's
        // storage when the operand densely covers it; an operand that is a
        // view into a larger storage (one row of a big tensor, say) is first
        // copied densely, so that the graph holds operand-sized memory rather
        // than the whole storage, and later writes to the larger tensor need
        // no copy-on-write. That copy is never larger than the expanded copy
        // the gather path makes.
        if(!tensor_on_cpu(*operand->storage) &&
           tensor_gpu_strided_enabled(*operand->storage)){
            auto strides=tensor_broadcast_strides(*operand,result->shape);
            if(tensor_strided_read_within_storage(*operand,result->shape,strides)){
                TensorStorage* storage=operand->storage;
                auto offset=operand->offset;
                if(offset==0 && tensor_is_contiguous_value(*operand) &&
                   tensor_logical_count(*operand)==storage->count){
                    if(storage->owners==std::numeric_limits<std::size_t>::max())
                        runtime_text_failure("tensor storage owner overflow");
                    ++storage->owners;
                }else{
                    storage=tensor_transfer_storage(
                        *operand,operand->storage->device,line,column);
                    offset=0;
                    const auto dense=tensor_contiguous_strides(operand->shape);
                    for(std::size_t axis=0;axis<strides.size();++axis)
                        strides[axis]=operand->shape[axis]==1?0:dense[axis];
                }
                quidra::runtime::NodeRuntimeOwner partial_view_storage(storage,+[](void* value){
                    tensor_storage_release(static_cast<TensorStorage*>(value));
                });
                auto* view=tensor_descriptor(
                    storage,result->shape,std::move(strides),offset);
                partial_view_storage.relinquish();
                quidra::runtime::NodeRuntimeOwner partial_view(view,+[](void* value){
                    release_managed_tensor(value);
                });
                auto node=std::make_shared<AutogradNode>(operand->storage->dtype);
                node->shape=result->shape;
                node->device_tensor=view;
                partial_view.relinquish();
                if(operand->graph){
                    node->op=AutogradOp::Broadcast;
                    node->parents={operand->graph};
                }
                node->note_saved_value();
                return node;
            }
        }

        auto indices=tensor_broadcast_logical_indices<CompatibilityProven>(
            *operand,result->shape,line,column);
        auto* expanded=tensor_gather_logical_indices(
            *operand,std::move(indices),result->shape,line,column);
        auto graph=expanded->graph
            ? expanded->graph : autograd_constant_node(*expanded,line,column);
        release_managed_tensor(expanded);
        return graph;
    };

    auto node=std::make_shared<AutogradNode>(primary->storage->dtype);
    node->shape=result->shape;
    node->op=other
        ? (operation==abi::tensor_binary_opcode::add?AutogradOp::Add:operation==abi::tensor_binary_opcode::subtract?AutogradOp::Sub:
           operation==abi::tensor_binary_opcode::multiply?AutogradOp::Mul:AutogradOp::Div)
        : AutogradOp::ScalarBinary;
    const auto primary_node=graph_operand(primary);
    if(other){
        const auto other_node=graph_operand(other);
        node->parents={primary_node,other_node};
    }else{
        node->parents={primary_node};
        if(autograd_half_dtype(primary->storage->dtype)){
            std::uint16_t bits{};
            std::memcpy(&bits,scalar,sizeof(bits));
            node->aux.assign(1,quidra::abi::half::value_of(
                bits,autograd_half_format(primary->storage->dtype)));
        }else if(primary->storage->dtype==QCORE_DTYPE_FLOAT32){
            float value{};
            std::memcpy(&value,scalar,sizeof(value));
            node->aux.assign(1,static_cast<double>(value));
        }else{
            double value{};
            std::memcpy(&value,scalar,sizeof(value));
            node->aux.assign(1,value);
        }
        node->aux_index={
            static_cast<std::size_t>(operation),
            scalar_side==abi::scalar_side::left?std::size_t{1}:std::size_t{0}
        };
    }
    if(tensor_on_cpu(*result->storage))
        node->data=tensor_float_values(*result,line,column);
    else
        node->device_tensor=static_cast<TensorValue*>(quidra_tensor_clone(result));
    node->note_saved_value();
    result->graph=std::move(node);
}

namespace {

// The QCORE_OP_* flags package functions declared, by C symbol.
struct OpFlagRegistry {
    std::mutex mutex;
    std::unordered_map<std::string, std::uint32_t> flags;
};

OpFlagRegistry& op_flag_registry() {
    static auto* registry = new OpFlagRegistry;
    return *registry;
}

std::uint32_t op_flags(const char* symbol) {
    if (!symbol) return 0;
    auto& registry = op_flag_registry();
    std::lock_guard lock(registry.mutex);
    const auto found = registry.flags.find(symbol);
    return found == registry.flags.end() ? 0 : found->second;
}

} // namespace

extern "C" int qcore_op_register_flags(const char* symbol, uint32_t flags) {
    if (!symbol || !*symbol ||
        (flags & ~static_cast<uint32_t>(QCORE_OP_VALIDATES_NAN | QCORE_OP_FINITE_PRESERVING | QCORE_OP_NAN_PRESERVING)) != 0)
        return -1;
    auto& registry = op_flag_registry();
    std::lock_guard lock(registry.mutex);
    registry.flags[symbol] = flags;
    return 0;
}

extern "C" int qcore_tensor_report_status(const void* tensor, const char* operation,
                                          uint32_t status) {
    if (status == 0) return 0;
    const auto* value = static_cast<const TensorValue*>(tensor);
    const int dtype = value && value->storage ? value->storage->dtype : QCORE_DTYPE_FLOAT32;
    end_package_hold_before_failure(0, 0);
    quidra::runtime::report_failure(
        abi::FailureReason::package_nonfinite,
        abi::FailureArgs{.type = tensor_element_name(dtype),
                         .operation = operation && *operation ? std::string_view(operation)
                                                              : std::string_view("?")},
        0, 0);
}

extern "C" int qcore_tensor_report_conversion_status(const void* tensor, uint32_t status) {
    if (status == 0) return 0;
    const auto* value = static_cast<const TensorValue*>(tensor);
    if (!value || !value->storage || !abi::real_dtype_code(value->storage->dtype)) return 1;
    const int dtype = value->storage->dtype;
    end_package_hold_before_failure(0, 0);
    quidra::runtime::report_conversion(abi::ConversionReason::out_of_range,
        abi::conversion_type_name(dtype), abi::ConversionSubject::tensor_element, 0, 0);
}

// After a call of a package (extern) function that wrote the real tensor
// `raw` through a `&` argument: unless the function declared that it
// validates its outputs or preserves the absence of NaN for infinite inputs
// (qcore_op_register_flags under `symbol`), Core validates the tensor and NaN
// fails with NONFINITE_RESULT. The legacy finite-preserving flag is not enough.
extern "C" void quidra_tensor_validate_package(void* raw, const char* symbol, const char* name,
                                               unsigned long long line,
                                               unsigned long long column) {
    if (!raw) return;
    if ((op_flags(symbol) & (QCORE_OP_VALIDATES_NAN | QCORE_OP_NAN_PRESERVING)) != 0)
        return;
    // The package code has returned: an encode it left open is reported as
    // the next statement would report it, before any Core work.
    if (quidra::device::open_package_holds.load(std::memory_order_relaxed) != 0)
        quidra_runtime_check_package_hold("the package code returned to Quidra code");
    const auto& tensor = *static_cast<const TensorValue*>(raw);
    if (!tensor.storage) return;
    tensor_validate_finite(
        tensor, abi::FailureReason::package_nonfinite,
        abi::FailureArgs{.type = tensor_element_name(tensor.storage->dtype),
                         .operation = name && *name ? std::string_view(name) : std::string_view("?")},
        line, column);
}

namespace {
constexpr std::uint64_t tensor_array_borrow_magic = 0x514154454e534f52ULL;
struct TensorArrayBorrow {
    std::uint64_t magic{};
    void* array{};
    std::uint64_t count{};
    std::uint64_t dtype{};
    std::uint64_t writable{};
    std::uint64_t line{};
    std::uint64_t column{};
};
static_assert(sizeof(TensorArrayBorrow) == abi::tensor_array_borrow_layout::bytes);
static_assert(offsetof(TensorArrayBorrow, array) == 8);
static_assert(offsetof(TensorArrayBorrow, count) == 16);
static_assert(offsetof(TensorArrayBorrow, column) == 48);

const TensorArrayBorrow& tensor_array_borrow(const void* raw) {
    if (!raw) tensor_fail("null tensor array borrow", 0, 0);
    // Read the tag first; an ordinary array/tensor is never a borrow view.
    std::uint64_t magic = 0;
    std::memcpy(&magic, raw, sizeof(magic));
    if (magic != tensor_array_borrow_magic)
        tensor_fail("invalid or expired tensor array borrow", 0, 0);
    return *static_cast<const TensorArrayBorrow*>(raw);
}

TensorValue* tensor_array_borrow_element(const TensorArrayBorrow& view,
                                        std::uint64_t index) {
    if (index >= view.count)
        tensor_fail("tensor array borrow index is out of range", view.line, view.column);
    const auto* slot = static_cast<const unsigned char*>(view.array) +
        abi::array_layout::payload_offset + static_cast<std::size_t>(index) * sizeof(void*);
    quidra_init_check(const_cast<unsigned char*>(slot), view.line, view.column);
    TensorValue* tensor = nullptr;
    std::memcpy(&tensor, slot, sizeof(tensor));
    const auto allocation = managed_state().allocations.find(reinterpret_cast<std::uintptr_t>(tensor));
    if (allocation == managed_state().allocations.end() || !allocation->second.tensor_descriptor)
        tensor_fail("tensor array borrow contains a non-tensor element", view.line, view.column);
    if (!tensor->storage || static_cast<std::uint64_t>(tensor->storage->dtype) != view.dtype)
        tensor_fail("tensor array borrow has an incompatible element dtype", view.line, view.column);
    if (!tensor_view_initialized(*tensor))
        runtime_uninitialized_failure(view.line, view.column);
    return tensor;
}
} // namespace

extern "C" void quidra_tensor_array_borrow_begin(
    void* raw_view, void* array_cell, int dtype, bool writable,
    void* clone_function, void* drop_function,
    unsigned long long line, unsigned long long column) {
    if (!raw_view || !array_cell) tensor_fail("invalid tensor array borrow storage", line, column);
    std::memset(raw_view, 0, sizeof(TensorArrayBorrow));
    quidra_init_check(array_cell, line, column);
    void* array = nullptr;
    std::memcpy(&array, array_cell, sizeof(array));
    const auto allocation = managed_state().allocations.find(reinterpret_cast<std::uintptr_t>(array));
    if (allocation == managed_state().allocations.end() || !allocation->second.initialization ||
        allocation->second.initialization->data_offset != abi::array_layout::payload_offset ||
        allocation->second.initialization->unit_bytes != sizeof(void*) ||
        allocation->second.size < abi::array_layout::payload_offset)
        tensor_fail("tensor array borrow requires a dynamic tensor array", line, column);
    long long length = 0;
    std::memcpy(&length, array, sizeof(length));
    if (length < 0 || static_cast<std::uint64_t>(length) != allocation->second.initialization->count ||
        static_cast<std::uint64_t>(length) >
            (allocation->second.size - abi::array_layout::payload_offset) / sizeof(void*))
        tensor_fail("invalid tensor array borrow length", line, column);
    TensorArrayBorrow view{0, array, static_cast<std::uint64_t>(length),
        static_cast<std::uint64_t>(dtype), writable ? 1u : 0u, line, column};
    // Validate the entire borrow before detaching or exposing a writable leaf.
    for (std::uint64_t index = 0; index < view.count; ++index) {
        const auto* tensor = tensor_array_borrow_element(view, index);
        if (writable && tensor->graph)
            tensor_fail("mutable tensor array borrow requires untracked elements", line, column);
    }
    if (writable && allocation->second.owners > 1) {
        if (!clone_function || !drop_function)
            tensor_fail("mutable tensor array borrow is missing its typed copy", line, column);
        // The typed clone copies the outer array and TensorValue descriptors;
        // storage writes subsequently use the ordinary tensor COW boundary.
        auto* copy = reinterpret_cast<void* (*)(void*)>(clone_function)(array);
        if (!copy) tensor_fail("tensor array copy failed", line, column);
        // No fallible operation separates the copy from its actual owner cell.
        // A Node's typed local/field cleanup reads this same cell.
        std::memcpy(array_cell, &copy, sizeof(copy));
        view.array = copy;
        // The old allocation still has another owner, so this cannot drop it.
        quidra_managed_release(array, drop_function);
    }
    view.magic = tensor_array_borrow_magic;
    std::memcpy(raw_view, &view, sizeof(view));
}

extern "C" void quidra_tensor_array_borrow_end(void* raw) {
    (void)tensor_array_borrow(raw);
    static_cast<TensorArrayBorrow*>(raw)->magic = 0;
}

extern "C" std::uint64_t qcore_tensor_array_count(const void* raw) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    return tensor_array_borrow(raw).count;
}
extern "C" const void* qcore_tensor_array_element_const(const void* raw, std::uint64_t index) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    return tensor_array_borrow_element(tensor_array_borrow(raw), index);
}
extern "C" void* qcore_tensor_array_element(void* raw, std::uint64_t index) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    const auto& view = tensor_array_borrow(raw);
    if (!view.writable)
        tensor_fail("tensor array borrow is read-only", view.line, view.column);
    auto* tensor = tensor_array_borrow_element(view, index);
    if (tensor->graph)
        tensor_fail("mutable tensor array borrow requires untracked elements", view.line, view.column);
    return tensor;
}

extern "C" void quidra_tensor_array_validate_package(
    void* raw, const char* symbol, const char* name,
    unsigned long long line, unsigned long long column) {
    const auto& view = tensor_array_borrow(raw);
    if (!view.writable || !abi::real_dtype_code(static_cast<int>(view.dtype))) return;
    if ((op_flags(symbol) & (QCORE_OP_VALIDATES_NAN | QCORE_OP_NAN_PRESERVING)) != 0) return;
    for (std::uint64_t index = 0; index < view.count; ++index)
        quidra_tensor_validate_package(tensor_array_borrow_element(view, index), symbol, name, line, column);
}

template<bool BroadcastProven>
static void* tensor_binary_entry(void* primary_raw, void* other_raw,
                                        void* scalar, int scalar_side,
                                        int operation,
                                        unsigned long long line,
                                        unsigned long long column) {
    quidra::profile::Range operation_range("core", "tensor binary");
    if (!primary_raw) tensor_fail("null tensor operand", line, column);
    auto* primary = static_cast<TensorValue*>(primary_raw);
    auto* other = static_cast<TensorValue*>(other_raw);
    if (other && scalar_side != abi::scalar_side::none) {
        tensor_fail("invalid tensor binary operands", line, column);
    }
    if (!other && scalar_side != abi::scalar_side::left && scalar_side != abi::scalar_side::right) {
        tensor_fail("invalid tensor scalar operand side", line, column);
    }
    if (operation == abi::tensor_binary_opcode::power && (other || scalar_side != abi::scalar_side::right)) {
        tensor_fail("tensor power requires tensor ^ scalar", line, column);
    }
    if (other && primary->storage->dtype != other->storage->dtype) {
        tensor_fail("tensor operands must have identical element types", line, column);
    }
    if (other) quidra::counters::add(quidra::counters::Id::RuntimeDeviceChecks);
    if (other && primary->storage->device != other->storage->device) {
        tensor_fail("tensor operands are on different devices; use an explicit .gpu(n) or .cpu() transfer",
                    line, column);
    }
    const auto output_shape =
        other ? tensor_broadcast_shape<BroadcastProven>(*primary, *other, line, column) : primary->shape;
    const auto count = tensor_element_count(output_shape, line, column);

    if (!tensor_on_cpu(*primary->storage)) {
        tensor_require_initialized(*primary, line, column);
        if (other) tensor_require_initialized(*other, line, column);

        const auto needs_expansion = [&](const TensorValue& operand) {
            return !tensor_is_contiguous_value(operand) ||
                   operand.shape != output_shape;
        };
        // Broadcast and strided operands are read in place by index
        // arithmetic: the kernel applies the dense kernel's arithmetic to the
        // elements a gather would have produced, so the bits are identical.
        if ((needs_expansion(*primary) || (other && needs_expansion(*other))) &&
            tensor_gpu_strided_enabled(*primary->storage) &&
            quidra::device::supports_strided_binary(
                primary->storage->gpu_buffer, primary->storage->dtype,
                operation)) {
            const auto primary_strides =
                tensor_broadcast_strides(*primary, output_shape);
            const auto other_strides = other
                ? tensor_broadcast_strides(*other, output_shape)
                : std::vector<long long>{};
            if (tensor_strided_read_within_storage(
                    *primary, output_shape, primary_strides) &&
                (!other || tensor_strided_read_within_storage(
                               *other, output_shape, other_strides))) {
                // The strided kernel writes every element (fill mode write_only).
                auto* output = tensor_storage_create(
                    primary->storage->dtype, count, abi::tensor_fill_mode::write_only,
                    primary->storage->device, line, column);
                std::string backend_error;
                if (!quidra::device::compute_binary_strided(
                        output->gpu_buffer, primary->storage->gpu_buffer,
                        primary->offset, primary_strides,
                        other ? other->storage->gpu_buffer : nullptr,
                        other ? other->offset : 0, other_strides,
                        scalar, scalar_side, primary->storage->dtype,
                        operation, output_shape, backend_error)) {
                    tensor_storage_release(output);
                    tensor_fail(backend_error.c_str(), line, column);
                }
                auto* result = tensor_descriptor(
                    output, output_shape,
                    tensor_contiguous_strides(output_shape), 0);
                quidra::runtime::NodeRuntimeOwner partial_graph_result_13(result,+[](void* raw){release_managed_tensor(raw);});
                tensor_attach_binary_graph<BroadcastProven>(
                    result, primary, other, scalar, scalar_side, operation,
                    line, column);
                partial_graph_result_13.relinquish();
                return result;
            }
        }

        TensorStorage* primary_expanded = nullptr;
        TensorStorage* other_expanded = nullptr;
        const auto width = tensor_dtype_bytes(primary->storage->dtype);

        const quidra::device::Buffer* primary_buffer = primary->storage->gpu_buffer;
        std::size_t primary_offset = primary->offset * width;
        if (!tensor_is_contiguous_value(*primary) ||
            primary->shape != output_shape) {
            primary_expanded =
                tensor_gpu_expand_storage(*primary, output_shape, line, column);
            primary_buffer = primary_expanded->gpu_buffer;
            primary_offset = 0;
        }

        const quidra::device::Buffer* other_buffer = nullptr;
        std::size_t other_offset = 0;
        if (other) {
            other_buffer = other->storage->gpu_buffer;
            other_offset = other->offset * width;
            if (!tensor_is_contiguous_value(*other) ||
                other->shape != output_shape) {
                other_expanded =
                    tensor_gpu_expand_storage(*other, output_shape, line, column);
                other_buffer = other_expanded->gpu_buffer;
                other_offset = 0;
            }
        }

        auto* output = tensor_storage_create(
            primary->storage->dtype, count,
            gpu_kernel_output_fill(primary->storage->dtype),
            primary->storage->device, line, column);
        std::string backend_error;
        const bool ok = quidra::device::compute_binary(
            output->gpu_buffer, primary_buffer, primary_offset,
            other_buffer, other_offset, scalar, scalar_side,
            primary->storage->dtype, operation, count, backend_error);

        if (primary_expanded) tensor_storage_release(primary_expanded);
        if (other_expanded) tensor_storage_release(other_expanded);
        if (!ok) {
            tensor_storage_release(output);
            tensor_fail(backend_error.c_str(), line, column);
        }
        auto* result=tensor_descriptor(
            output, output_shape, tensor_contiguous_strides(output_shape), 0);
        quidra::runtime::NodeRuntimeOwner partial_graph_result_14(result,+[](void* raw){release_managed_tensor(raw);});
        tensor_attach_binary_graph<BroadcastProven>(
            result,primary,other,scalar,scalar_side,operation,line,column);
        partial_graph_result_14.relinquish();
        return result;
    }

    auto* output = tensor_storage_create(primary->storage->dtype, count, abi::tensor_fill_mode::zeros);

    switch (primary->storage->dtype) {
        case QCORE_DTYPE_INT64: tensor_binary_typed<std::int64_t>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case QCORE_DTYPE_INT8: tensor_binary_typed<std::int8_t>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case QCORE_DTYPE_INT16: tensor_binary_typed<std::int16_t>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case QCORE_DTYPE_INT32: tensor_binary_typed<std::int32_t>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case QCORE_DTYPE_UINT8: tensor_binary_typed<std::uint8_t>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case QCORE_DTYPE_UINT16: tensor_binary_typed<std::uint16_t>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case QCORE_DTYPE_UINT32: tensor_binary_typed<std::uint32_t>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case QCORE_DTYPE_UINT64: tensor_binary_typed<std::uint64_t>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case QCORE_DTYPE_FLOAT64: tensor_binary_typed<double>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case QCORE_DTYPE_FLOAT32: tensor_binary_typed<float>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case QCORE_DTYPE_FLOAT16: tensor_binary_typed<Real16>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case QCORE_DTYPE_BFLOAT16: tensor_binary_typed<Real16b>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case QCORE_DTYPE_COMPLEX_FLOAT16: tensor_binary_typed<ComplexElement<Real16>>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case QCORE_DTYPE_COMPLEX_BFLOAT16: tensor_binary_typed<ComplexElement<Real16b>>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case QCORE_DTYPE_COMPLEX_FLOAT32: tensor_binary_typed<ComplexElement<float>>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case QCORE_DTYPE_COMPLEX_FLOAT64: tensor_binary_typed<ComplexElement<double>>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        default:
            delete output;
            tensor_fail("invalid tensor element type", line, column);
    }
    auto* result=tensor_descriptor(
        output, output_shape, tensor_contiguous_strides(output_shape), 0);
    quidra::runtime::NodeRuntimeOwner partial_graph_result_15(result,+[](void* raw){release_managed_tensor(raw);});
    tensor_attach_binary_graph<BroadcastProven>(
        result,primary,other,scalar,scalar_side,operation,line,column);
    partial_graph_result_15.relinquish();
    return result;
}

extern "C" void* quidra_tensor_binary(void* primary_raw, void* other_raw,
                                        void* scalar, int scalar_side,
                                        int operation,
                                        unsigned long long line,
                                        unsigned long long column) {
    return tensor_binary_entry<false>(primary_raw, other_raw, scalar, scalar_side, operation, line, column);
}

extern "C" void* quidra_tensor_binary_proven(void* primary_raw, void* other_raw,
                                        void* scalar, int scalar_side,
                                        int operation,
                                        unsigned long long line,
                                        unsigned long long column) {
    return tensor_binary_entry<true>(primary_raw, other_raw, scalar, scalar_side, operation, line, column);
}



namespace {

} // namespace






namespace {

TensorStorage* tensor_materialize_storage(const TensorValue& source) {
    const auto count = tensor_logical_count(source);
    auto* output = tensor_storage_create(source.storage->dtype, count, abi::tensor_fill_mode::uninitialized);
    if (tensor_copy_dense_view(source, *output, count)) return output;
    const auto width = tensor_dtype_bytes(source.storage->dtype);
    const bool source_complete = source.storage->initialization.fully_initialized;
    for (std::size_t i = 0; i < count; ++i) {
        const auto source_index = tensor_storage_index(source, i);
        if (source_index >= source.storage->count) {
            delete output;
            runtime_text_failure("tensor view exceeds storage");
        }
        std::memcpy(output->data.data() + i * width,
                    source.storage->data.data() + source_index * width, width);
        if (!source_complete &&
            tracker_bit(source.storage->initialization, source_index)) {
            tracker_set(output->initialization, i);
        }
    }
    if (source_complete) tracker_mark_complete(output->initialization);
    return output;
}

void tensor_detach_for_write(
    TensorValue& tensor, unsigned long long line, unsigned long long column) {
    if(tensor.graph){
        tensor_fail(
            "tracked tensor mutation is forbidden; call untrack() before writing",
            line,column);
    }
    const auto logical_count = tensor_logical_count(tensor);
    const bool owns_full_contiguous_storage =
        tensor.offset == 0 && tensor_is_contiguous_value(tensor) &&
        logical_count == tensor.storage->count;
    if (owns_full_contiguous_storage &&
        tensor_storage_writable_in_place(*tensor.storage)) return;

    auto* old = tensor.storage;
    auto* replacement = tensor_on_cpu(*old)
        ? tensor_materialize_storage(tensor)
        : tensor_gpu_materialize_storage(tensor, line, column);
    tensor.storage = replacement;
    tensor.offset = 0;
    tensor.strides = tensor_contiguous_strides(tensor.shape);
    tensor_storage_release(old);
}

} // namespace

extern "C" void* quidra_tensor_index(void* raw, const long long* specs,
                                       unsigned long long count,
                                       unsigned long long line,
                                       unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if (!raw) tensor_fail("null tensor", line, column);
    auto* source = static_cast<TensorValue*>(raw);
    tensor_require_untracked_transform(*source,"indexing",line,column);
    if (count > source->shape.size()) {
        tensor_fail("too many tensor indices", line, column);
    }
    if (count != 0 && !specs) tensor_fail("missing tensor index data", line, column);

    std::vector<long long> shape;
    std::vector<long long> strides;
    shape.reserve(source->shape.size());
    strides.reserve(source->shape.size());
    std::size_t offset = source->offset;

    for (std::size_t axis = 0; axis < static_cast<std::size_t>(count); ++axis) {
        namespace spec = abi::tensor_index_spec;
        const auto* item = specs + axis * spec::fields;
        const auto kind = item[spec::kind_field];
        const auto first = item[spec::first_part_field];
        const auto second = item[spec::first_part_field + 1];
        const auto third = item[spec::first_part_field + 2];
        const auto dimension = source->shape[axis];
        const auto stride = source->strides[axis];

        if (kind == spec::index) {
            if (first < 0 || first >= dimension) {
                tensor_index_fail(abi::FailureReason::axis_index_out_of_bounds,
                                  abi::FailureArgs{.index = first,
                                                   .length = dimension,
                                                   .axis = static_cast<long long>(axis)},
                                  line, column);
            }
            offset += static_cast<std::size_t>(first) *
                      static_cast<std::size_t>(stride);
            continue;
        }
        if (spec::is_marked_slice(kind)) {
            // The forms preserve bound presence independently of its value.
            const auto start_form = spec::marked_start(kind);
            const auto end_form = spec::marked_end(kind);
            const bool has_step = (kind & spec::explicit_step) != 0 || third != spec::missing;
            const auto step = has_step ? third : 1;
            if (step <= 0) tensor_slice_step_fail(step, start_form, end_form, line, column);
            const auto planned = quidra::plan_slice(
                static_cast<std::uint64_t>(dimension),
                quidra::SliceRequest{start_form, first, end_form, second, step}, false);
            if (planned.status == quidra::SliceStatus::step_against_marker) {
                tensor_slice_step_fail(step, start_form, end_form, line, column);
            }
            if (planned.status == quidra::SliceStatus::out_of_bounds) {
                const auto written = quidra::slice_form_text(
                    start_form, first, end_form, second, has_step, third);
                tensor_index_fail(abi::FailureReason::axis_slice_out_of_bounds,
                                  abi::FailureArgs{.length = dimension,
                                                   .axis = static_cast<long long>(axis),
                                                   .slice = written.view()},
                                  line, column);
            }
            if (!planned) tensor_fail("invalid tensor slice", line, column);
            if (planned.plan.count != 0) {
                offset += static_cast<std::size_t>(planned.plan.first) *
                          static_cast<std::size_t>(stride);
            }
            shape.push_back(static_cast<long long>(planned.plan.count));
            if (stride != 0 && step > std::numeric_limits<long long>::max() / stride) {
                tensor_fail("tensor slice stride overflow", line, column);
            }
            strides.push_back(stride * step);
            continue;
        }
        if (kind != spec::slice) tensor_fail("invalid tensor index kind", line, column);

        const auto start = first == spec::missing ? 0 : first;
        const auto stop = second == spec::missing ? dimension : second;
        const auto step = third == spec::missing ? 1 : third;
        if (step <= 0) {
            tensor_slice_step_fail(
                step, first == spec::missing ? quidra::SliceStart::omitted
                                             : quidra::SliceStart::inclusive,
                second == spec::missing ? quidra::SliceEnd::omitted
                                        : quidra::SliceEnd::exclusive_ascending,
                line, column);
        }
        if (start < 0 || start > dimension || stop < 0 || stop > dimension) {
            // The slice as written: start:<end and the parts given.
            const auto written = quidra::slice_form_text(
                first == spec::missing ? quidra::SliceStart::omitted
                                       : quidra::SliceStart::inclusive,
                first,
                second == spec::missing ? quidra::SliceEnd::omitted
                                        : quidra::SliceEnd::exclusive_ascending,
                second, third != spec::missing, third);
            tensor_index_fail(abi::FailureReason::axis_slice_out_of_bounds,
                              abi::FailureArgs{.length = dimension,
                                               .axis = static_cast<long long>(axis),
                                               .slice = written.view()},
                              line, column);
        }
        // Existing tensor index ABI uses half-open ascending slices. Route
        // non-empty selections through the shared bounds planner, preserving
        // legacy empty [start, stop) results, including [dimension, dimension).
        // Unlike ceil((stop-start)/step), count is computed without a
        // potentially overflowing stop-start+step-1 intermediate.
        long long length = 0;
        if (stop > start) {
            const auto selection = quidra::plan_slice(
                static_cast<std::uint64_t>(dimension),
                quidra::SliceRequest{.start_form = quidra::SliceStart::inclusive,
                                     .start = start,
                                     .end_form = quidra::SliceEnd::exclusive_ascending,
                                     .end = stop,
                                     .step = step}, false);
            if (!selection) tensor_fail("invalid checked tensor slice plan", line, column);
            length = static_cast<long long>(selection.plan.count);
        }
        offset += static_cast<std::size_t>(start) *
                  static_cast<std::size_t>(stride);
        shape.push_back(length);
        if (stride != 0 &&
            step > std::numeric_limits<long long>::max() / stride) {
            tensor_fail("tensor slice stride overflow", line, column);
        }
        strides.push_back(stride * step);
    }

    for (std::size_t axis = static_cast<std::size_t>(count);
         axis < source->shape.size(); ++axis) {
        shape.push_back(source->shape[axis]);
        strides.push_back(source->strides[axis]);
    }

    if (source->storage->owners == std::numeric_limits<std::size_t>::max()) {
        runtime_text_failure("tensor storage owner overflow");
    }
    ++source->storage->owners;
    quidra::runtime::NodeRuntimeOwner partial_retained_9(source->storage, +[](void* value) {
        tensor_storage_release(static_cast<TensorStorage*>(value));
    });
    return [&] { auto* complete=tensor_descriptor(source->storage, std::move(shape), std::move(strides), offset); partial_retained_9.relinquish(); return complete; }();
}

extern "C" void quidra_tensor_set(void* raw, const long long* indices,
                                    unsigned long long count, const void* value,
                                    unsigned long long line,
                                    unsigned long long column) {
    quidra::counters::add(quidra::counters::Id::RuntimeShapeValidations);
    if (!raw || !value) tensor_fail("invalid tensor element assignment", line, column);
    auto* tensor = static_cast<TensorValue*>(raw);
    if (count != tensor->shape.size()) {
        tensor_fail("tensor element assignment requires one integer index per dimension",
                    line, column);
    }
    if (count != 0 && !indices) tensor_fail("missing tensor indices", line, column);

    tensor_detach_for_write(*tensor, line, column);

    std::size_t storage_index = tensor->offset;
    for (std::size_t axis = 0; axis < static_cast<std::size_t>(count); ++axis) {
        const auto index = indices[axis];
        if (index < 0 || index >= tensor->shape[axis]) {
            tensor_index_fail(abi::FailureReason::axis_index_out_of_bounds,
                              abi::FailureArgs{.index = index,
                                               .length = tensor->shape[axis],
                                               .axis = static_cast<long long>(axis)},
                              line, column);
        }
        storage_index += static_cast<std::size_t>(index) *
                         static_cast<std::size_t>(tensor->strides[axis]);
    }
    if (storage_index >= tensor->storage->count) {
        tensor_fail("tensor element assignment exceeds storage", line, column);
    }
    const auto width = tensor_dtype_bytes(tensor->storage->dtype);
    if (tensor_on_cpu(*tensor->storage)) {
        std::memcpy(tensor->storage->data.data() + storage_index * width, value, width);
    } else {
        std::string backend_error;
        if (!quidra::device::copy_from_host(
                tensor->storage->gpu_buffer, storage_index * width,
                value, width, backend_error)) {
            tensor_fail(backend_error.c_str(), line, column);
        }
    }
    tracker_set(tensor->storage->initialization, storage_index);
}


// An element index outside a string (a negative one included): the index
// and the string's length in code points, at the index operand.
[[noreturn]] static void string_index_failure(const char* text, long long index,
                                              unsigned long long line,
                                              unsigned long long column) {
    ManagedAllocation* allocation = nullptr;
    const auto source = cached_string_view(text, allocation);
    const auto length = allocation && allocation->string_codepoint_length_known
        ? allocation->string_codepoint_length
        : utf8_length(source);
    quidra::runtime::report_failure(
        abi::FailureReason::index_out_of_bounds,
        abi::FailureArgs{.index = index, .length = static_cast<long long>(length)},
        line, column);
}

extern "C" char* quidra_string_index(const char* text, long long index,
                                      unsigned long long line,
                                      unsigned long long column) {
    if (!text) runtime_text_failure("null string");
    if (index < 0) string_index_failure(text, index, line, column);

    // Managed ASCII strings are the overwhelmingly common case for protocol,
    // file and benchmark text. UTF-8 validation already proved that code-point
    // and byte offsets are identical, so preserve full Quidra string semantics
    // while making s[i] as cheap as an ordinary byte-indexed string access.
    if (auto* allocation = exact_managed_string(text);
        allocation && allocation->string_ascii_known &&
        allocation->string_ascii) {
        const auto length = allocation->string_byte_length;
        const auto position = static_cast<std::size_t>(index);
        if (position >= length) {
            quidra::runtime::report_failure(
                abi::FailureReason::index_out_of_bounds,
                abi::FailureArgs{.index = index, .length = static_cast<long long>(length)},
                line, column);
        }
        const auto byte = static_cast<unsigned char>(text[position]);
        static const auto ascii_singletons = [] {
            std::array<std::array<char, 2>, 128> values{};
            for (std::size_t i = 1; i < values.size(); ++i) {
                values[i][0] = static_cast<char>(i);
                values[i][1] = '\0';
            }
            return values;
        }();
        return const_cast<char*>(ascii_singletons[byte].data());
    }

    const auto bounds = utf8_index_bounds(text, static_cast<std::size_t>(index));
    if (!bounds.found) {
        ManagedAllocation* allocation = nullptr;
        const auto source = cached_string_view(text, allocation);
        const auto length = allocation && allocation->string_codepoint_length_known
            ? allocation->string_codepoint_length
            : utf8_length(source);
        if (allocation) {
            allocation->string_codepoint_length_known = true;
            allocation->string_codepoint_length = length;
        }
        quidra::runtime::report_failure(
            abi::FailureReason::index_out_of_bounds,
            abi::FailureArgs{.index = index, .length = static_cast<long long>(length)},
            line, column);
    }

    if (bounds.end == bounds.start + 1) {
        const auto byte = static_cast<unsigned char>(text[bounds.start]);
        if (byte != 0 && byte < 0x80) {
            // Compiler string literals already use immutable unmanaged storage.
            // Reuse the same ownership convention for ASCII index results so
            // character-by-character scans do not allocate one managed string
            // per code point.
            static const auto ascii_singletons = [] {
                std::array<std::array<char, 2>, 128> values{};
                for (std::size_t i = 1; i < values.size(); ++i) {
                    values[i][0] = static_cast<char>(i);
                    values[i][1] = '\0';
                }
                return values;
            }();
            return const_cast<char*>(ascii_singletons[byte].data());
        }
    }

    return copy_validated_runtime_text(
        std::string_view(text + bounds.start, bounds.end - bounds.start), 1);
}

extern "C" bool quidra_string_index_equal_ascii(
    const char* text, long long index, unsigned char expected,
    unsigned long long line, unsigned long long column) {
    if (!text) runtime_text_failure("null string");
    if (index < 0) string_index_failure(text, index, line, column);

    if (auto* allocation = exact_managed_string(text);
        allocation && allocation->string_ascii_known &&
        allocation->string_ascii) {
        const auto position = static_cast<std::size_t>(index);
        if (position >= allocation->string_byte_length) {
            quidra::runtime::report_failure(
                abi::FailureReason::index_out_of_bounds,
                abi::FailureArgs{.index = index, .length = static_cast<long long>(allocation->string_byte_length)},
                line, column);
        }
        return static_cast<unsigned char>(text[position]) == expected;
    }

    const auto bounds = utf8_index_bounds(text, static_cast<std::size_t>(index));
    if (!bounds.found) {
        ManagedAllocation* allocation = nullptr;
        const auto source = cached_string_view(text, allocation);
        const auto length = allocation && allocation->string_codepoint_length_known
            ? allocation->string_codepoint_length
            : utf8_length(source);
        if (allocation) {
            allocation->string_codepoint_length_known = true;
            allocation->string_codepoint_length = length;
        }
        quidra::runtime::report_failure(
            abi::FailureReason::index_out_of_bounds,
            abi::FailureArgs{.index = index, .length = static_cast<long long>(length)},
            line, column);
    }
    return bounds.end == bounds.start + 1 &&
           static_cast<unsigned char>(text[bounds.start]) == expected;
}


extern "C" long long quidra_string_count_ascii_prefix(
    const char* text, long long count, unsigned char expected, bool negate,
    long long initial, unsigned long long index_line,
    unsigned long long index_column, unsigned long long overflow_line,
    unsigned long long overflow_column) {
    if (!text) runtime_text_failure("null string");
    if (count <= 0) return initial;

    auto overflow = [&]() -> void {
        quidra::runtime::report_failure(abi::FailureReason::integer_overflow, {},
                                        overflow_line, overflow_column);
    };
    auto bounds = [&](long long index, std::size_t length) -> void {
        quidra::runtime::report_failure(
            abi::FailureReason::index_out_of_bounds,
            abi::FailureArgs{.index = index, .length = static_cast<long long>(length)},
            index_line, index_column);
    };
    auto add_one = [&](long long& value) {
        if (value == std::numeric_limits<long long>::max()) overflow();
        ++value;
    };
    const auto selected = [&](bool equal) {
        return negate ? !equal : equal;
    };

    if (auto* allocation = exact_managed_string(text);
        allocation && allocation->string_ascii_known &&
        allocation->string_ascii) {
        const auto length = allocation->string_byte_length;
        const auto raw_count = static_cast<unsigned long long>(count);
        const bool count_fits_size =
            raw_count <= static_cast<unsigned long long>(
                std::numeric_limits<std::size_t>::max());
        const bool fully_in_bounds =
            count_fits_size &&
            static_cast<std::size_t>(raw_count) <= length;

        if (fully_in_bounds) {
            const auto requested = static_cast<std::size_t>(raw_count);
            std::size_t equal_count = 0;
            for (std::size_t i = 0; i < requested; ++i)
                equal_count +=
                    static_cast<unsigned char>(text[i]) == expected ? 1U : 0U;
            const auto matches =
                negate ? requested - equal_count : equal_count;
            const auto delta = static_cast<long long>(matches);
            if (initial > std::numeric_limits<long long>::max() - delta)
                overflow();
            return initial + delta;
        }

        // If a later index is out of bounds, preserve source error ordering:
        // an earlier counter overflow must still win.
        auto value = initial;
        for (std::size_t i = 0; i < length; ++i) {
            const bool equal =
                static_cast<unsigned char>(text[i]) == expected;
            if (selected(equal)) add_one(value);
        }
        bounds(static_cast<long long>(length), length);
    }

    ManagedAllocation* allocation = nullptr;
    const auto source = validated_string_view(text, allocation);
    std::size_t byte_index = 0;
    long long value = initial;
    for (long long position = 0; position < count; ++position) {
        if (byte_index >= source.size())
            bounds(position, static_cast<std::size_t>(position));
        const auto codepoint = utf8_next(source, byte_index);
        if (selected(codepoint == expected)) add_one(value);
    }
    return value;
}

extern "C" long long quidra_string_length(const char* text) {
    ManagedAllocation* allocation = nullptr;
    std::size_t length = 0;
    (void)validated_string_view(text, allocation, &length);
    return static_cast<long long>(length);
}

extern "C" bool quidra_string_contains(const char* text, const char* needle) {
    if (!text || !needle) runtime_text_failure("null string");
    ManagedAllocation* source_allocation = nullptr;
    ManagedAllocation* query_allocation = nullptr;
    const auto source = validated_string_view(text, source_allocation);
    const auto query = validated_string_view(needle, query_allocation);
    return source.find(query) != std::string_view::npos;
}

extern "C" bool quidra_string_starts_with(const char* text, const char* prefix) {
    if (!text || !prefix) runtime_text_failure("null string");
    ManagedAllocation* source_allocation = nullptr;
    ManagedAllocation* query_allocation = nullptr;
    const auto source = validated_string_view(text, source_allocation);
    const auto query = validated_string_view(prefix, query_allocation);
    return source.size() >= query.size() && source.substr(0, query.size()) == query;
}

extern "C" bool quidra_string_ends_with(const char* text, const char* suffix) {
    if (!text || !suffix) runtime_text_failure("null string");
    ManagedAllocation* source_allocation = nullptr;
    ManagedAllocation* query_allocation = nullptr;
    const auto source = validated_string_view(text, source_allocation);
    const auto query = validated_string_view(suffix, query_allocation);
    return source.size() >= query.size() &&
           source.substr(source.size() - query.size()) == query;
}

extern "C" long long quidra_string_find(const char* text, const char* needle) {
    if (!text || !needle) runtime_text_failure("null string");
    ManagedAllocation* source_allocation = nullptr;
    ManagedAllocation* query_allocation = nullptr;
    const auto source = validated_string_view(text, source_allocation);
    const auto query = validated_string_view(needle, query_allocation);
    const auto pos = source.find(query);
    if (pos == std::string_view::npos) return -1;
    // Valid UTF-8 is self-synchronizing: a valid query cannot begin at a
    // continuation byte in a valid source. Count code points only up to the match.
    return static_cast<long long>(utf8_prefix_length(source, pos));
}

// A slice start:<end outside a string: the slice as written and the
// string's length in code points, at the slice.
[[noreturn]] static void string_slice_failure(const char* text, long long start,
                                              long long end, unsigned long long line,
                                              unsigned long long column) {
    ManagedAllocation* allocation = nullptr;
    const auto length = utf8_length(cached_string_view(text, allocation));
    const auto written = quidra::slice_form_text(
        quidra::SliceStart::inclusive, start, quidra::SliceEnd::exclusive_ascending, end);
    quidra::runtime::report_failure(
        abi::FailureReason::slice_out_of_bounds,
        abi::FailureArgs{.length = static_cast<long long>(length), .slice = written.view()},
        line, column);
}

// The slice plan for generated code: validates a slice item of a
// value of `length` elements (the direction and step rules, then the five
// bound rules) and returns how many indices it selects, writing the first
// one to *first; the indices continue by the step, or by +1 or -1 by
// `direction` without one. A step error is SLICE_STEP, a bounds error
// INDEX_BOUNDS with the slice as written, at the slice.
static quidra::SliceIndexPlan plan_slice_or_fail(long long length, long long start,
                                                 int start_form, long long end, int end_form,
                                                 long long step, int has_step, int direction,
                                                 unsigned long long line,
                                                 unsigned long long column) {
    if (length < 0 || start_form < 0 || start_form > 3 || end_form < 0 || end_form > 3 ||
        direction < 0 || direction > static_cast<int>(quidra::SliceDirection::by_bounds)) {
        runtime_text_failure("invalid slice plan request");
    }
    const auto start_kind = static_cast<quidra::SliceStart>(start_form);
    const auto end_kind = static_cast<quidra::SliceEnd>(end_form);
    // Construct the two requests independently. GCC 11 can report a false
    // maybe-uninitialized optional payload when a request is reset in-place
    // and plan_slice is inlined with warnings treated as errors.
    const auto size = static_cast<std::uint64_t>(length);
    const auto planned = has_step
        ? quidra::plan_slice(size, {start_kind, start, end_kind, end,
                                   std::optional<std::int64_t>{step}})
        : quidra::plan_slice(size, {start_kind, start, end_kind, end, std::nullopt});
    switch (planned.status) {
        case quidra::SliceStatus::ok:
            break;
        case quidra::SliceStatus::zero_step:
            quidra::runtime::report_failure(abi::FailureReason::slice_step_zero, {}, line,
                                            column);
        case quidra::SliceStatus::step_against_marker: {
            // The first marker written names the direction.
            const std::string_view marker =
                quidra::slice_marker_direction(start_kind) != 0
                    ? quidra::slice_marker_text(start_kind)
                    : quidra::slice_marker_text(end_kind);
            quidra::runtime::report_failure(abi::FailureReason::slice_step_direction,
                                            abi::FailureArgs{.step = step, .marker = marker},
                                            line, column);
        }
        case quidra::SliceStatus::out_of_bounds: {
            const auto written =
                quidra::slice_form_text(start_kind, start, end_kind, end, has_step != 0, step);
            quidra::runtime::report_failure(
                abi::FailureReason::slice_out_of_bounds,
                abi::FailureArgs{.length = length, .slice = written.view()}, line, column);
        }
        case quidra::SliceStatus::marker_conflict:
            // The checker rejects markers that point different ways.
            runtime_text_failure("slice markers point different ways");
        case quidra::SliceStatus::length_overflow:
            runtime_text_failure("slice length overflow");
    }
    const auto resolved = static_cast<quidra::SliceDirection>(direction);
    if ((resolved == quidra::SliceDirection::ascending && planned.plan.step < 0) ||
        (resolved == quidra::SliceDirection::descending && planned.plan.step > 0)) {
        runtime_text_failure("slice direction differs from the checked direction");
    }
    return planned.plan;
}

extern "C" long long quidra_slice_plan(long long length, long long start, int start_form,
                                       long long end, int end_form, long long step,
                                       int has_step, int direction, unsigned long long line,
                                       unsigned long long column, long long* first) {
    if (!first) runtime_text_failure("invalid slice plan request");
    const auto plan = plan_slice_or_fail(length, start, start_form, end, end_form, step,
                                         has_step, direction, line, column);
    *first = plan.first;
    return static_cast<long long>(plan.count);
}

// A slice assignment stores exactly as many values as the slice selects.
extern "C" void quidra_slice_assignment_check(long long count, long long given,
                                              unsigned long long line,
                                              unsigned long long column) {
    if (count == given) return;
    quidra::runtime::report_failure(abi::FailureReason::slice_length_mismatch,
                                    abi::FailureArgs{.length = count, .given = given}, line,
                                    column);
}

// text[start:<end] over code points: [start, end), or empty when the start
// is past the end (slice bound rule 5); the start is still a position of the
// string or its length, and the excluded end lies in [-1, length].
extern "C" char* quidra_string_slice(
    const char* text, long long start, long long end, unsigned long long line,
    unsigned long long column) {
    if (!text) runtime_text_failure("null string");
    if (start < 0 || end < -1) string_slice_failure(text, start, end, line, column);
    if (end < start) {
        ManagedAllocation* allocation = nullptr;
        const auto length = utf8_length(validated_string_view(text, allocation));
        if (static_cast<unsigned long long>(start) > length)
            string_slice_failure(text, start, end, line, column);
        return copy_validated_runtime_text(std::string_view{}, 0);
    }

    ManagedAllocation* allocation = nullptr;
    const auto source = validated_string_view(text, allocation);
    const auto target_start = static_cast<std::size_t>(start);
    const auto target_end = static_cast<std::size_t>(end);

    std::size_t codepoint = 0;
    std::size_t byte = 0;
    if (allocation && allocation->string_index_cursor_valid &&
        allocation->string_index_cursor_codepoint <= target_start &&
        allocation->string_index_cursor_byte <= source.size()) {
        codepoint = allocation->string_index_cursor_codepoint;
        byte = allocation->string_index_cursor_byte;
    }

    while (codepoint < target_start && byte < source.size()) {
        (void)utf8_next(source, byte);
        ++codepoint;
    }
    if (codepoint != target_start) string_slice_failure(text, start, end, line, column);
    const auto byte_start = byte;

    while (codepoint < target_end && byte < source.size()) {
        (void)utf8_next(source, byte);
        ++codepoint;
    }
    if (codepoint != target_end) string_slice_failure(text, start, end, line, column);

    // UTF-8 scanning has already established that the prefix [0, end) is
    // valid. Use that prefix as the planner's bounds instead of making a
    // second pass over the entire string to count code points.
    const auto selection = quidra::plan_slice(
        static_cast<std::uint64_t>(end),
        quidra::SliceRequest{.start_form = quidra::SliceStart::inclusive,
                             .start = start,
                             .end_form = quidra::SliceEnd::exclusive_ascending,
                             .end = end,
                             .step = std::nullopt});
    if (!selection) runtime_text_failure("invalid checked string slice plan");
    const auto selected_codepoints = selection.plan.count;

    if (allocation) {
        allocation->string_index_cursor_valid = true;
        allocation->string_index_cursor_codepoint = codepoint;
        allocation->string_index_cursor_byte = byte;
        if (byte == source.size()) {
            allocation->string_codepoint_length_known = true;
            allocation->string_codepoint_length = codepoint;
        }
    }
    return copy_validated_runtime_text(
        source.substr(byte_start, byte - byte_start),
        static_cast<std::size_t>(selected_codepoints));
}

// A slice of a string in any form over its code points: the plan
// of quidra_slice_plan over the string's length in code points, then the
// selected code points in order.
extern "C" char* quidra_string_slice_stepped(
    const char* text, long long start, int start_form, long long end, int end_form,
    long long step, int has_step, int direction, unsigned long long line,
    unsigned long long column) {
    if (!text) runtime_text_failure("null string");
    ManagedAllocation* allocation = nullptr;
    const auto source = validated_string_view(text, allocation);
    std::vector<std::size_t> starts;
    for (std::size_t byte = 0; byte < source.size();) {
        starts.push_back(byte);
        (void)utf8_next(source, byte);
    }
    const auto plan = plan_slice_or_fail(static_cast<long long>(starts.size()), start,
                                         start_form, end, end_form, step, has_step,
                                         direction, line, column);
    if (plan.count != 0 && plan.step == 1) {
        const auto first = static_cast<std::size_t>(plan.first);
        const auto past = first + static_cast<std::size_t>(plan.count);
        const auto end_byte = past < starts.size() ? starts[past] : source.size();
        return copy_validated_runtime_text(
            source.substr(starts[first], end_byte - starts[first]),
            static_cast<std::size_t>(plan.count));
    }
    std::string selected;
    for (std::uint64_t offset = 0; offset < plan.count; ++offset) {
        const auto index = static_cast<std::size_t>(plan.index_at(offset));
        const auto end_byte = index + 1 < starts.size() ? starts[index + 1] : source.size();
        selected.append(source.substr(starts[index], end_byte - starts[index]));
    }
    return copy_validated_runtime_text(selected, static_cast<std::size_t>(plan.count));
}

extern "C" char* quidra_string_trim(const char* text) {
    if (!text) runtime_text_failure("null string");
    ManagedAllocation* allocation = nullptr;
    const auto source = validated_string_view(text, allocation);

    std::size_t first = 0;
    std::size_t removed_front = 0;
    while (first < source.size()) {
        auto next = first;
        const auto codepoint = utf8_next(source, next);
        if (!unicode_space(codepoint)) break;
        first = next;
        ++removed_front;
    }

    std::size_t last = source.size();
    std::size_t removed_back = 0;
    while (last > first) {
        auto begin = last - 1;
        while (begin > first &&
               (static_cast<unsigned char>(source[begin]) & 0xc0U) == 0x80U) {
            --begin;
        }
        auto next = begin;
        const auto codepoint = utf8_next(source, next);
        if (next != last) runtime_text_failure("invalid UTF-8 boundary");
        if (!unicode_space(codepoint)) break;
        last = begin;
        ++removed_back;
    }

    std::optional<std::size_t> count;
    if (allocation && allocation->string_codepoint_length_known) {
        const auto original = allocation->string_codepoint_length;
        if (removed_front + removed_back <= original)
            count = original - removed_front - removed_back;
    }
    return copy_validated_runtime_text(source.substr(first, last - first), count);
}


struct QuidraStringSplitIterator {
    char* slab{};
    ManagedAllocation* allocation{};
    std::size_t size{};
    std::size_t next{};
    std::string separator;
    bool finished{};
};

static void* quidra_string_split_iter_begin_impl(
    const char* text, const char* separator, bool move_source) {
    if (!text || !separator) runtime_text_failure("null string split input");
    ManagedAllocation* source_allocation = nullptr;
    ManagedAllocation* delimiter_allocation = nullptr;
    const auto source = validated_string_view(text, source_allocation);
    const auto delimiter = validated_string_view(separator, delimiter_allocation);
    if (delimiter.empty())
        runtime_text_failure("string split separator cannot be empty");
    if (source.size() == std::numeric_limits<std::size_t>::max())
        runtime_allocation_failure();

    auto* iterator = new (std::nothrow) QuidraStringSplitIterator;
    if (!iterator) runtime_allocation_failure();
    // separator.assign and slab allocation may unwind before the cursor is
    // visible to generated code. No slab exists until its final installation.
    quidra::runtime::NodeRuntimeOwner partial_iterator(iterator,[](void* raw){
        delete static_cast<QuidraStringSplitIterator*>(raw);
    });
    iterator->size = source.size();
    iterator->separator.assign(delimiter.data(), delimiter.size());

    const bool can_reuse =
        move_source && source_allocation &&
        source_allocation->base == text &&
        source_allocation->owners == 1 &&
        source_allocation->pins == 0 &&
        !source_allocation->shared_string_slab &&
        !source_allocation->initialization &&
        source_allocation->drop == nullptr &&
        source_allocation->size >= source.size() + 1;

    if (can_reuse) {
        auto& allocation = *source_allocation;
        const auto key = reinterpret_cast<std::uintptr_t>(allocation.base);
        if (!allocation.interior_range_tracked) {
            managed_state().ranges.emplace(key, allocation.size);
            allocation.interior_range_tracked = true;
        }
        if (allocation.owners == std::numeric_limits<std::size_t>::max()) {
            partial_iterator.relinquish();
            delete iterator;
            runtime_text_failure("managed owner count overflow");
        }
        ++allocation.owners;
        allocation.shared_string_slab = true;
        iterator->slab = const_cast<char*>(text);
        iterator->allocation = source_allocation;
        iterator->slab[source.size()] = '\0';
        partial_iterator.relinquish();
        return iterator;
    }

    auto* slab = static_cast<char*>(
        managed_allocate_impl(source.size() + 1, true));
    if (!source.empty()) std::memcpy(slab, source.data(), source.size());
    slab[source.size()] = '\0';

    const auto slab_key = reinterpret_cast<std::uintptr_t>(slab);
    auto slab_it = managed_state().allocations.find(slab_key);
    if (slab_it == managed_state().allocations.end()) {
        partial_iterator.relinquish();
        delete iterator;
        runtime_text_failure("split iterator backing storage disappeared");
    }
    slab_it->second.shared_string_slab = true;
    slab_it->second.owners = 1;
    if (source_allocation && source_allocation->string_ascii_known) {
        slab_it->second.string_ascii_known = true;
        slab_it->second.string_ascii = source_allocation->string_ascii;
    }
    iterator->slab = slab;
    iterator->allocation = &slab_it->second;
    partial_iterator.relinquish();
    return iterator;
}

extern "C" void* quidra_string_split_iter_begin(
    const char* text, const char* separator) {
    return quidra_string_split_iter_begin_impl(text, separator, false);
}

extern "C" void* quidra_string_split_iter_begin_move(
    const char* text, const char* separator) {
    return quidra_string_split_iter_begin_impl(text, separator, true);
}

extern "C" char* quidra_string_split_iter_next(void* raw) {
    if (!raw) runtime_text_failure("null string split iterator");
    auto& iterator = *static_cast<QuidraStringSplitIterator*>(raw);
    if (iterator.finished) return nullptr;

    const auto start = iterator.next;
    if (start > iterator.size)
        runtime_text_failure("invalid string split iterator state");

    const std::string_view remaining(
        iterator.slab + start, iterator.size - start);
    std::size_t relative = std::string_view::npos;
    if (iterator.separator.size() == 1) {
        const auto* found = static_cast<const char*>(
            std::memchr(remaining.data(),
                        static_cast<unsigned char>(iterator.separator[0]),
                        remaining.size()));
        if (found)
            relative = static_cast<std::size_t>(found - remaining.data());
    } else {
        relative = remaining.find(iterator.separator);
    }
    if (relative == std::string_view::npos) {
        iterator.finished = true;
        iterator.slab[iterator.size] = '\0';
        managed_state().shared_text = iterator.slab + start;
        managed_state().shared_length = iterator.size - start;
        managed_state().shared_allocation = iterator.allocation;
        return iterator.slab + start;
    }

    const auto position = start + relative;
    iterator.slab[position] = '\0';
    iterator.next = position + iterator.separator.size();
    managed_state().shared_text = iterator.slab + start;
    managed_state().shared_length = position - start;
    managed_state().shared_allocation = iterator.allocation;
    return iterator.slab + start;
}

extern "C" void quidra_string_split_iter_end(void* raw) {
    if (!raw) return;
    auto* iterator = static_cast<QuidraStringSplitIterator*>(raw);
    if (managed_state().shared_allocation == iterator->allocation) {
        invalidate_shared_string_cache(iterator->allocation);
    }
    quidra_managed_release(iterator->slab, nullptr);
    delete iterator;
}

extern "C" void* quidra_string_split(const char* text, const char* separator) {
    if (!text || !separator) runtime_text_failure("null string");
    ManagedAllocation* source_allocation = nullptr;
    ManagedAllocation* delimiter_allocation = nullptr;
    const auto source = validated_string_view(text, source_allocation);
    const auto delimiter = validated_string_view(separator, delimiter_allocation);
    if (delimiter.empty()) runtime_text_failure("string split separator cannot be empty");

    std::size_t piece_count = 1;
    for (std::size_t start = 0;;) {
        const auto pos = source.find(delimiter, start);
        if (pos == std::string_view::npos) break;
        if (piece_count == std::numeric_limits<std::size_t>::max())
            runtime_allocation_failure();
        ++piece_count;
        start = pos + delimiter.size();
    }

    if (piece_count > (std::numeric_limits<std::size_t>::max() - abi::array_layout::payload_offset) / sizeof(char*) ||
        source.size() == std::numeric_limits<std::size_t>::max()) {
        runtime_allocation_failure();
    }

    auto* slab = static_cast<char*>(
        managed_allocate_impl(source.size() + 1, true));
    quidra::runtime::NodeRuntimeOwner partial_slab(slab);
    if (!source.empty()) std::memcpy(slab, source.data(), source.size());
    slab[source.size()] = '\0';

    const auto slab_key = reinterpret_cast<std::uintptr_t>(slab);
    auto slab_it = managed_state().allocations.find(slab_key);
    if (slab_it == managed_state().allocations.end())
        runtime_text_failure("split backing storage disappeared");
    slab_it->second.shared_string_slab = true;
    if (!quidra::runtime::node_failure_active()) slab_it->second.owners = piece_count;

    const auto bytes = abi::array_layout::payload_offset + piece_count * sizeof(char*);
    auto* result = static_cast<unsigned char*>(managed_allocate(bytes));
    const auto count = static_cast<long long>(piece_count);
    std::memcpy(result, &count, sizeof(count));

    std::size_t start = 0;
    for (std::size_t i = 0; i < piece_count; ++i) {
        const auto pos = source.find(delimiter, start);
        const auto end = pos == std::string_view::npos ? source.size() : pos;
        slab[end] = '\0';
        auto* item = slab + start;
        std::memcpy(result + abi::array_layout::payload_offset + i * sizeof(char*), &item, sizeof(item));
        start = pos == std::string_view::npos
            ? source.size() : pos + delimiter.size();
    }
    if (quidra::runtime::node_failure_active())
        managed_state().allocations.find(slab_key)->second.owners = piece_count;
    partial_slab.relinquish();
    return result;
}
extern "C" bool quidra_string_can_append_move(void* raw) {
    if (!raw) return false;
    const auto* allocation =
        exact_managed_string(static_cast<const char*>(raw));
    if (!allocation) return false;
    return allocation->owners == 1 && allocation->pins == 0 &&
           !allocation->shared_string_slab &&
           !allocation->initialization && allocation->drop == nullptr &&
           allocation->size != 0;
}


namespace {
std::string_view quoted_string_escape(unsigned char byte) {
    switch(byte) {
    case '"':return "{DQ}";case '{':return "{{";case '}':return "}}";
    case '\n':return "{NL}";case '\t':return "{HT}";case '\r':return "{CR}";
    case '\b':return "{BS}";case '\f':return "{FF}";case '\v':return "{VT}";case '\a':return "{BL}";
    default:return {};
    }
}
std::pair<std::size_t,std::size_t> quoted_string_size(std::string_view piece,std::size_t codepoints) {
    std::size_t bytes=2,points=codepoints;
    if(points>std::numeric_limits<std::size_t>::max()-2)runtime_allocation_failure();
    points+=2;
    for(unsigned char byte:piece) {
        const auto escape=quoted_string_escape(byte);const auto size=escape.empty()?1:escape.size();
        if(size>std::numeric_limits<std::size_t>::max()-bytes)runtime_allocation_failure();
        bytes+=size;
        if(!escape.empty()){if(size-1>std::numeric_limits<std::size_t>::max()-points)runtime_allocation_failure();points+=size-1;}
    }
    return {bytes,points};
}
std::size_t quoted_string_fill(char* destination,std::string_view piece) {
    std::size_t offset=0;destination[offset++]='"';
    for(unsigned char byte:piece){const auto escape=quoted_string_escape(byte);if(escape.empty())destination[offset++]=static_cast<char>(byte);else{std::memcpy(destination+offset,escape.data(),escape.size());offset+=escape.size();}}
    destination[offset++]='"';return offset;
}
} // namespace

extern "C" char* quidra_string_build_append_move_unique_direct(
    char* raw, const unsigned char* kinds,
    const unsigned long long* raw_values,
    unsigned long long raw_count, const char* separator,
    long long* added_length_out) {
    if (!raw || !separator || !added_length_out)
        runtime_text_failure("null typed string build-append input");
    if (raw_count >
        static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
        runtime_allocation_failure();
    const auto count = static_cast<std::size_t>(raw_count);
    if (count != 0 && (!kinds || !raw_values))
        runtime_text_failure("null typed string build-append parts");
    if (separator == raw)
        runtime_text_failure("string build-append separator aliases destination");

    ManagedAllocation* receiver = nullptr;
    std::size_t old_codepoints = 0;
    const auto original =
        validated_string_view(raw, receiver, &old_codepoints);
    const auto old_length = original.size();

    ManagedAllocation* separator_allocation = nullptr;
    std::size_t separator_codepoints = 0;
    std::string_view delimiter;
    if (separator[0] != '\0') {
        delimiter = validated_string_view(
            separator, separator_allocation, &separator_codepoints);
    }

    auto checked_add = [](std::size_t& target, std::size_t value) {
        if (value > std::numeric_limits<std::size_t>::max() - target)
            runtime_allocation_failure();
        target += value;
    };

    constexpr std::size_t small_part_count = 16;
    std::array<std::size_t, small_part_count> small_lengths{};
    std::array<unsigned char, small_part_count> small_aliases{};
    std::array<std::array<char, 32>, small_part_count> small_numeric{};
    std::vector<std::size_t> large_lengths;
    std::vector<unsigned char> large_aliases;
    std::vector<std::array<char, 32>> large_numeric;
    if (count > small_part_count) {
        large_lengths.resize(count);
        large_aliases.assign(count, 0);
        large_numeric.resize(count);
    }
    auto* lengths = count <= small_part_count
        ? small_lengths.data() : large_lengths.data();
    auto* aliases = count <= small_part_count
        ? small_aliases.data() : large_aliases.data();
    auto* numeric_parts = count <= small_part_count
        ? small_numeric.data() : large_numeric.data();

    std::size_t added = 0;
    std::size_t added_codepoints = 0;
    if (count > 1 && !delimiter.empty()) {
        if (count - 1 >
            std::numeric_limits<std::size_t>::max() / delimiter.size())
            runtime_allocation_failure();
        added = (count - 1) * delimiter.size();
        if (separator_codepoints != 0 &&
            count - 1 >
                std::numeric_limits<std::size_t>::max() / separator_codepoints)
            runtime_allocation_failure();
        added_codepoints = (count - 1) * separator_codepoints;
    }

    for (std::size_t i = 0; i < count; ++i) {
        aliases[i] = 0;
        switch (kinds[i]) {
            case abi::string_build_part_kind::quoted_string: {
                const auto* text=reinterpret_cast<const char*>(static_cast<std::uintptr_t>(raw_values[i]));
                if(!text)runtime_text_failure("null quoted string builder value");
                aliases[i]=text==raw?1:0;ManagedAllocation* allocation=nullptr;std::size_t points=0;
                const auto piece=validated_string_view(text,allocation,&points);const auto size=quoted_string_size(piece,points);
                lengths[i]=size.first;checked_add(added,size.first);checked_add(added_codepoints,size.second);break;
            }
            case abi::string_build_part_kind::text: {
                const auto* text = reinterpret_cast<const char*>(
                    static_cast<std::uintptr_t>(raw_values[i]));
                if (!text) runtime_text_failure("null string builder value");
                aliases[i] = text == raw ? 1 : 0;
                ManagedAllocation* allocation = nullptr;
                std::size_t codepoints = 0;
                const auto piece =
                    validated_string_view(text, allocation, &codepoints);
                lengths[i] = piece.size();
                checked_add(added, piece.size());
                checked_add(added_codepoints, codepoints);
                break;
            }
            case abi::string_build_part_kind::ascii_character: {
                const auto* text = reinterpret_cast<const char*>(
                    static_cast<std::uintptr_t>(raw_values[i]));
                if (!text || text[0] == '\0' || text[1] != '\0' ||
                    static_cast<unsigned char>(text[0]) >= 0x80U) {
                    runtime_text_failure(
                        "invalid proven one-byte ASCII string builder value");
                }
                lengths[i] = 1;
                checked_add(added, 1);
                checked_add(added_codepoints, 1);
                break;
            }
            case abi::string_build_part_kind::signed_integer: {
                const auto value = std::bit_cast<long long>(raw_values[i]);
                auto& numeric = numeric_parts[i];
                const auto converted = std::to_chars(
                    numeric.data(), numeric.data() + numeric.size(), value, 10);
                if (converted.ec != std::errc{})
                    runtime_text_failure("integer formatting failed");
                lengths[i] = static_cast<std::size_t>(
                    converted.ptr - numeric.data());
                checked_add(added, lengths[i]);
                checked_add(added_codepoints, lengths[i]);
                break;
            }
            case abi::string_build_part_kind::unsigned_integer: {
                auto& numeric = numeric_parts[i];
                const auto converted = std::to_chars(
                    numeric.data(), numeric.data() + numeric.size(),
                    raw_values[i], 10);
                if (converted.ec != std::errc{})
                    runtime_text_failure("integer formatting failed");
                lengths[i] = static_cast<std::size_t>(
                    converted.ptr - numeric.data());
                checked_add(added, lengths[i]);
                checked_add(added_codepoints, lengths[i]);
                break;
            }
            case abi::string_build_part_kind::boolean: {
                lengths[i] = raw_values[i] ? 4U : 5U;
                checked_add(added, lengths[i]);
                checked_add(added_codepoints, lengths[i]);
                break;
            }
            default:
                runtime_text_failure("invalid typed string build-append part");
        }
    }

    if (added > std::numeric_limits<std::size_t>::max() - old_length)
        runtime_allocation_failure();
    const auto new_length = old_length + added;

    // validated_string_view already resolved the exact managed receiver.
    // Re-looking it up in managed_state().allocations on every append is pure overhead
    // in hot builders such as MB10. Keep the resolved metadata pointer and only
    // touch the hash table when a rare realloc actually changes the base key.
    auto* destination_allocation = receiver;
    if (!destination_allocation || destination_allocation->base != raw)
        runtime_text_failure("string build-append storage is not uniquely managed");
    if (destination_allocation->size == 0 ||
        destination_allocation->size - 1 < old_length)
        runtime_text_failure("invalid managed string build-append capacity");

    const auto old_key = reinterpret_cast<std::uintptr_t>(raw);
    std::size_t capacity = destination_allocation->size - 1;
    char* result = raw;
    if (capacity < new_length) {
        std::size_t new_capacity = capacity < 16 ? 16 : capacity;
        while (new_capacity < new_length) {
            if (new_capacity > std::numeric_limits<std::size_t>::max() / 2) {
                new_capacity = new_length;
                break;
            }
            new_capacity *= 2;
        }
        if (new_capacity == std::numeric_limits<std::size_t>::max())
            runtime_allocation_failure();
        const auto new_bytes = new_capacity + 1;
        const auto old_bytes = destination_allocation->size;
        invalidate_managed_string_cache(destination_allocation);
        result = static_cast<char*>(std::realloc(raw, new_bytes));
        if (!result) runtime_allocation_failure();
        if (new_bytes > old_bytes)
            std::memset(result + old_bytes, 0, new_bytes - old_bytes);

        const auto new_key = reinterpret_cast<std::uintptr_t>(result);
        if (new_key != old_key) {
            auto node = managed_state().allocations.extract(old_key);
            if (node.empty())
                runtime_text_failure("string build-append storage disappeared");
            node.key() = new_key;
            node.mapped().base = result;
            node.mapped().size = new_bytes;
            node.mapped().small_pool_class = 0;
            const auto inserted = managed_state().allocations.insert(std::move(node));
            destination_allocation = &inserted.position->second;
        } else {
            destination_allocation->base = result;
            destination_allocation->size = new_bytes;
            destination_allocation->small_pool_class = 0;
        }
    }

    std::size_t offset = old_length;
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0 && !delimiter.empty()) {
            std::memcpy(result + offset, delimiter.data(), delimiter.size());
            offset += delimiter.size();
        }
        switch (kinds[i]) {
            case abi::string_build_part_kind::quoted_string: {
                ManagedAllocation* allocation=nullptr;const auto* source=aliases[i]?result:reinterpret_cast<const char*>(static_cast<std::uintptr_t>(raw_values[i]));
                const auto piece=aliases[i]?std::string_view(result,old_length):cached_string_view(source,allocation);
                offset+=quoted_string_fill(result+offset,piece);break;
            }
            case abi::string_build_part_kind::text:
            case abi::string_build_part_kind::ascii_character: {
                const auto* source = aliases[i]
                    ? result
                    : reinterpret_cast<const char*>(
                          static_cast<std::uintptr_t>(raw_values[i]));
                if (lengths[i] != 0) {
                    std::memcpy(result + offset, source, lengths[i]);
                    offset += lengths[i];
                }
                break;
            }
            case abi::string_build_part_kind::signed_integer:
            case abi::string_build_part_kind::unsigned_integer: {
                if (lengths[i] != 0) {
                    std::memcpy(
                        result + offset, numeric_parts[i].data(), lengths[i]);
                    offset += lengths[i];
                }
                break;
            }
            case abi::string_build_part_kind::boolean: {
                const char* value = raw_values[i] ? "true" : "false";
                std::memcpy(result + offset, value, lengths[i]);
                offset += lengths[i];
                break;
            }
            default:
                runtime_text_failure("invalid typed string build-append part");
        }
    }

    result[new_length] = '\0';
    destination_allocation->string_byte_length_known = true;
    destination_allocation->string_byte_length = new_length;
    destination_allocation->string_codepoint_length_known = true;
    destination_allocation->string_codepoint_length =
        old_codepoints + added_codepoints;
    destination_allocation->string_utf8_validated = true;
    destination_allocation->string_ascii_known = true;
    destination_allocation->string_ascii =
        (old_codepoints + added_codepoints) == new_length;
    *added_length_out = static_cast<long long>(added_codepoints);
    return result;
}

extern "C" char* quidra_string_build_append_move_unique(
    char* raw, const unsigned char* kinds,
    const unsigned long long* raw_values,
    unsigned long long raw_count, const char* separator) {
    long long added_length = 0;
    auto* result = quidra_string_build_append_move_unique_direct(
        raw, kinds, raw_values, raw_count, separator, &added_length);
    managed_state().append_last_codepoints = added_length;
    return result;
}

extern "C" char* quidra_string_build_append_move(
    char* raw, const unsigned char* kinds,
    const unsigned long long* raw_values,
    unsigned long long raw_count, const char* separator) {
    if (!quidra_string_can_append_move(raw))
        runtime_text_failure("string build-append requires unique storage");
    return quidra_string_build_append_move_unique(
        raw, kinds, raw_values, raw_count, separator);
}

extern "C" long long quidra_string_build_append_last_length() {
    return managed_state().append_last_codepoints;
}

extern "C" char* quidra_string_append_move_many(
    char* raw, const char* const* suffixes, unsigned long long raw_count) {
    if (raw_count >
        static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max() - 1)) {
        runtime_allocation_failure();
    }
    const auto count = static_cast<std::size_t>(raw_count);
    if (count != 0 && !suffixes) runtime_text_failure("null string append values");
    if (!quidra_string_can_append_move(raw)) {
        std::vector<const char*> values;
        values.reserve(count + 1);
        values.push_back(raw);
        for (std::size_t i = 0; i < count; ++i) values.push_back(suffixes[i]);
        return quidra_string_concat_many(values.data(),
                                         static_cast<unsigned long long>(values.size()));
    }

    // Both of the obvious ways to read the receiver here are O(its length): a
    // strlen through string_view, and a full UTF-8 rescan. Doing either on every
    // append is what made `text = text + piece` in a loop quadratic even though
    // the capacity below already grows geometrically. The length is cached, and
    // validated bytes stay validated because every suffix is checked before it is
    // appended, so a validated prefix plus validated suffixes is still valid.
    ManagedAllocation* receiver = nullptr;
    std::size_t old_codepoints = 0;
    const auto original =
        validated_string_view(raw, receiver, &old_codepoints);
    const auto old_length = original.size();

    constexpr std::size_t small_append_count = 16;
    std::array<std::size_t, small_append_count> small_lengths{};
    std::array<unsigned char, small_append_count> small_aliases{};
    std::vector<std::size_t> large_lengths;
    std::vector<unsigned char> large_aliases;
    if (count > small_append_count) {
        large_lengths.resize(count);
        large_aliases.assign(count, 0);
    }
    auto* lengths = count <= small_append_count
        ? small_lengths.data() : large_lengths.data();
    auto* aliases = count <= small_append_count
        ? small_aliases.data() : large_aliases.data();
    std::size_t added = 0;
    std::size_t added_codepoints = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (!suffixes[i]) runtime_text_failure("null string in append");
        aliases[i] = suffixes[i] == raw ? 1 : 0;
        ManagedAllocation* suffix_allocation = nullptr;
        std::size_t suffix_codepoints = 0;
        const auto suffix = validated_string_view(
            suffixes[i], suffix_allocation, &suffix_codepoints);
        lengths[i] = suffix.size();
        if (lengths[i] > std::numeric_limits<std::size_t>::max() - added) {
            runtime_allocation_failure();
        }
        added += lengths[i];
        if (suffix_codepoints >
            std::numeric_limits<std::size_t>::max() - added_codepoints) {
            runtime_allocation_failure();
        }
        added_codepoints += suffix_codepoints;
    }
    if (added > std::numeric_limits<std::size_t>::max() - old_length) {
        runtime_allocation_failure();
    }
    const auto new_length = old_length + added;

    const auto old_key = reinterpret_cast<std::uintptr_t>(raw);
    auto it = managed_state().allocations.find(old_key);
    if (it == managed_state().allocations.end()) {
        runtime_text_failure("string append storage disappeared");
    }
    auto& allocation = it->second;
    if (allocation.size == 0 || allocation.size - 1 < old_length) {
        runtime_text_failure("invalid managed string capacity");
    }
    allocation.string_byte_length_known = true;
    allocation.string_byte_length = old_length;

    std::size_t capacity = allocation.size - 1;
    char* result = raw;
    if (capacity < new_length) {
        std::size_t new_capacity = capacity < 16 ? 16 : capacity;
        while (new_capacity < new_length) {
            if (new_capacity > std::numeric_limits<std::size_t>::max() / 2) {
                new_capacity = new_length;
                break;
            }
            new_capacity *= 2;
        }
        if (new_capacity == std::numeric_limits<std::size_t>::max()) {
            runtime_allocation_failure();
        }
        const auto new_bytes = new_capacity + 1;
        const auto old_bytes = allocation.size;
        invalidate_managed_string_cache(&allocation);
        result = static_cast<char*>(std::realloc(raw, new_bytes));
        if (!result) runtime_allocation_failure();
        if (new_bytes > old_bytes) {
            std::memset(result + old_bytes, 0, new_bytes - old_bytes);
        }

        const auto new_key = reinterpret_cast<std::uintptr_t>(result);
        if (new_key != old_key) {
            auto node = managed_state().allocations.extract(old_key);
            node.key() = new_key;
            node.mapped().base = result;
            node.mapped().size = new_bytes;
            node.mapped().small_pool_class = 0;
            managed_state().allocations.insert(std::move(node));
        } else {
            allocation.base = result;
            allocation.size = new_bytes;
            allocation.small_pool_class = 0;
        }
        it = managed_state().allocations.find(new_key);
        capacity = new_capacity;
    }

    std::size_t offset = old_length;
    for (std::size_t i = 0; i < count; ++i) {
        const char* source = aliases[i] ? result : suffixes[i];
        if (lengths[i] != 0) {
            std::memcpy(result + offset, source, lengths[i]);
            offset += lengths[i];
        }
    }
    result[new_length] = '\0';
    it->second.string_byte_length_known = true;
    it->second.string_byte_length = new_length;
    it->second.string_codepoint_length_known = true;
    it->second.string_codepoint_length = old_codepoints + added_codepoints;
    it->second.string_utf8_validated = true;
    it->second.string_ascii_known = true;
    it->second.string_ascii =
        (old_codepoints + added_codepoints) == new_length;
    return result;
}

extern "C" char* quidra_string_build(
    const unsigned char* kinds, const unsigned long long* raw_values,
    unsigned long long raw_count, const char* separator) {
    if (raw_count >
        static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
        runtime_allocation_failure();
    const auto count = static_cast<std::size_t>(raw_count);
    if (count != 0 && (!kinds || !raw_values))
        runtime_text_failure("null typed string builder parts");
    if (!separator) runtime_text_failure("null string builder separator");

    ManagedAllocation* separator_allocation = nullptr;
    std::size_t separator_codepoints = 0;
    const auto delimiter = validated_string_view(
        separator, separator_allocation, &separator_codepoints);

    auto checked_add = [](std::size_t& target, std::size_t value) {
        if (value > std::numeric_limits<std::size_t>::max() - target)
            runtime_allocation_failure();
        target += value;
    };

    std::size_t total = 0;
    std::size_t total_codepoints = 0;
    if (count > 1 && !delimiter.empty()) {
        if (count - 1 >
            std::numeric_limits<std::size_t>::max() / delimiter.size())
            runtime_allocation_failure();
        total = (count - 1) * delimiter.size();
        if (separator_codepoints != 0 &&
            count - 1 >
                std::numeric_limits<std::size_t>::max() / separator_codepoints)
            runtime_allocation_failure();
        total_codepoints = (count - 1) * separator_codepoints;
    }

    std::array<char, 32> numeric{};
    for (std::size_t i = 0; i < count; ++i) {
        switch (kinds[i]) {
            case abi::string_build_part_kind::quoted_string: {
                const auto* text=reinterpret_cast<const char*>(static_cast<std::uintptr_t>(raw_values[i]));if(!text)runtime_text_failure("null quoted string builder value");ManagedAllocation* allocation=nullptr;std::size_t points=0;const auto piece=validated_string_view(text,allocation,&points);const auto size=quoted_string_size(piece,points);checked_add(total,size.first);checked_add(total_codepoints,size.second);break;
            }
            case abi::string_build_part_kind::text: {
                const auto* text = reinterpret_cast<const char*>(
                    static_cast<std::uintptr_t>(raw_values[i]));
                if (!text) runtime_text_failure("null string builder value");
                ManagedAllocation* allocation = nullptr;
                std::size_t codepoints = 0;
                const auto piece =
                    validated_string_view(text, allocation, &codepoints);
                checked_add(total, piece.size());
                checked_add(total_codepoints, codepoints);
                break;
            }
            case abi::string_build_part_kind::signed_integer: {
                const auto value = std::bit_cast<long long>(raw_values[i]);
                const auto converted = std::to_chars(
                    numeric.data(), numeric.data() + numeric.size(), value, 10);
                if (converted.ec != std::errc{})
                    runtime_text_failure("integer formatting failed");
                const auto bytes = static_cast<std::size_t>(
                    converted.ptr - numeric.data());
                checked_add(total, bytes);
                checked_add(total_codepoints, bytes);
                break;
            }
            case abi::string_build_part_kind::unsigned_integer: {
                const auto converted = std::to_chars(
                    numeric.data(), numeric.data() + numeric.size(),
                    raw_values[i], 10);
                if (converted.ec != std::errc{})
                    runtime_text_failure("integer formatting failed");
                const auto bytes = static_cast<std::size_t>(
                    converted.ptr - numeric.data());
                checked_add(total, bytes);
                checked_add(total_codepoints, bytes);
                break;
            }
            case abi::string_build_part_kind::boolean: {
                const auto bytes = raw_values[i] ? 4U : 5U;
                checked_add(total, bytes);
                checked_add(total_codepoints, bytes);
                break;
            }
            default:
                runtime_text_failure("invalid typed string builder part");
        }
    }

    if (total == std::numeric_limits<std::size_t>::max())
        runtime_allocation_failure();
    auto* result =
        static_cast<char*>(managed_allocate_string(total + 1));
    std::size_t offset = 0;

    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0 && !delimiter.empty()) {
            std::memcpy(result + offset, delimiter.data(), delimiter.size());
            offset += delimiter.size();
        }
        switch (kinds[i]) {
            case abi::string_build_part_kind::quoted_string: {
                const auto* text=reinterpret_cast<const char*>(static_cast<std::uintptr_t>(raw_values[i]));ManagedAllocation* allocation=nullptr;const auto piece=cached_string_view(text,allocation);offset+=quoted_string_fill(result+offset,piece);break;
            }
            case abi::string_build_part_kind::text: {
                const auto* text = reinterpret_cast<const char*>(
                    static_cast<std::uintptr_t>(raw_values[i]));
                ManagedAllocation* allocation = nullptr;
                const auto piece = cached_string_view(text, allocation);
                if (!piece.empty())
                    std::memcpy(result + offset, piece.data(), piece.size());
                offset += piece.size();
                break;
            }
            case abi::string_build_part_kind::signed_integer: {
                const auto value = std::bit_cast<long long>(raw_values[i]);
                const auto converted = std::to_chars(
                    result + offset, result + total, value, 10);
                if (converted.ec != std::errc{})
                    runtime_text_failure("integer formatting failed");
                offset = static_cast<std::size_t>(converted.ptr - result);
                break;
            }
            case abi::string_build_part_kind::unsigned_integer: {
                const auto converted = std::to_chars(
                    result + offset, result + total, raw_values[i], 10);
                if (converted.ec != std::errc{})
                    runtime_text_failure("integer formatting failed");
                offset = static_cast<std::size_t>(converted.ptr - result);
                break;
            }
            case abi::string_build_part_kind::boolean: {
                const char* value = raw_values[i] ? "true" : "false";
                const auto bytes = raw_values[i] ? 4U : 5U;
                std::memcpy(result + offset, value, bytes);
                offset += bytes;
                break;
            }
            default:
                runtime_text_failure("invalid typed string builder part");
        }
    }
    result[total] = '\0';
    mark_managed_string(result, total, total_codepoints);
    return result;
}

extern "C" char* quidra_string_concat_many(const char* const* values,
                                                 unsigned long long raw_count) {
    if (raw_count > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        runtime_allocation_failure();
    }
    const auto count = static_cast<std::size_t>(raw_count);
    if (count != 0 && !values) runtime_text_failure("null string concat values");

    constexpr std::size_t small_concat_count = 16;
    std::array<std::string_view, small_concat_count> small_pieces{};
    std::vector<std::string_view> large_pieces;
    if (count > small_concat_count) large_pieces.resize(count);
    auto* pieces = count <= small_concat_count
        ? small_pieces.data() : large_pieces.data();
    std::size_t total = 0;
    std::size_t total_codepoints = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (!values[i]) runtime_text_failure("null string in concatenation");
        ManagedAllocation* allocation = nullptr;
        std::size_t codepoints = 0;
        const auto piece =
            validated_string_view(values[i], allocation, &codepoints);
        if (piece.size() > std::numeric_limits<std::size_t>::max() - total - 1) {
            runtime_allocation_failure();
        }
        total += piece.size();
        if (codepoints >
            std::numeric_limits<std::size_t>::max() - total_codepoints) {
            runtime_allocation_failure();
        }
        total_codepoints += codepoints;
        pieces[i] = piece;
    }

    auto* result =
        static_cast<char*>(managed_allocate_string(total + 1));
    std::size_t offset = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto piece = pieces[i];
        if (!piece.empty()) std::memcpy(result + offset, piece.data(), piece.size());
        offset += piece.size();
    }
    result[total] = '\0';
    mark_managed_string(result, total, total_codepoints);
    return result;
}

extern "C" char* quidra_string_concat2(
    const char* left, const char* right) {
    const char* values[2]{left, right};
    return quidra_string_concat_many(values, 2);
}

extern "C" bool quidra_string_equal(
    const char* left, const char* right) {
    if (left == right) return true;
    if (!left || !right) return false;
    ManagedAllocation* left_allocation = nullptr;
    ManagedAllocation* right_allocation = nullptr;
    const auto left_view = cached_string_view(left, left_allocation);
    const auto right_view = cached_string_view(right, right_allocation);
    return left_view.size() == right_view.size() &&
           (left_view.empty() ||
            std::memcmp(left_view.data(), right_view.data(), left_view.size()) == 0);
}

namespace {
std::size_t bin_payload_bytes(long long bit_count) {
    if (bit_count < 0) runtime_text_failure("bin length cannot be negative");
    const auto bits = static_cast<unsigned long long>(bit_count);
    if (bits > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()) * 8ULL)
        runtime_allocation_failure();
    const auto bytes = (bits + 7ULL) / 8ULL;
    if (bytes > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
        runtime_allocation_failure();
    return static_cast<std::size_t>(bytes);
}

long long bin_length(const void* raw) {
    if (!raw) runtime_text_failure("null bin");
    long long bit_count = 0;
    std::memcpy(&bit_count, raw, sizeof(bit_count));
    if (bit_count < 0) runtime_text_failure("invalid bin length");
    return bit_count;
}

bool bin_bit_at(const void* raw, long long index) {
    const auto length = bin_length(raw);
    if (index < 0 || index >= length) runtime_text_failure("bin index out of bounds");
    const auto* data = static_cast<const unsigned char*>(raw) + abi::bin_layout::payload_offset;
    const auto byte_index = static_cast<std::size_t>(index / 8);
    const auto shift = static_cast<unsigned>(7 - (index % 8));
    return ((data[byte_index] >> shift) & 1U) != 0;
}

void bin_set_bit(void* raw, long long index, bool value) {
    const auto length = bin_length(raw);
    if (index < 0 || index >= length) runtime_text_failure("bin index out of bounds");
    auto* data = static_cast<unsigned char*>(raw) + abi::bin_layout::payload_offset;
    const auto byte_index = static_cast<std::size_t>(index / 8);
    const auto shift = static_cast<unsigned>(7 - (index % 8));
    const auto mask = static_cast<unsigned char>(1U << shift);
    if (value) data[byte_index] = static_cast<unsigned char>(data[byte_index] | mask);
    else data[byte_index] = static_cast<unsigned char>(data[byte_index] & static_cast<unsigned char>(~mask));
}
}

extern "C" void* quidra_bin_alloc(long long bit_count, long long fill) {
    if (fill != 0 && fill != 1) runtime_text_failure("bin fill must be 0 or 1");
    const auto bytes = bin_payload_bytes(bit_count);
    if (bytes > std::numeric_limits<std::size_t>::max() - abi::bin_layout::payload_offset) runtime_allocation_failure();
    auto* result = static_cast<unsigned char*>(managed_allocate(abi::bin_layout::payload_offset + bytes));
    std::memcpy(result, &bit_count, sizeof(bit_count));
    if (bytes != 0) std::memset(result + abi::bin_layout::payload_offset, fill ? 0xff : 0x00, bytes);
    if (fill && bit_count % 8 != 0 && bytes != 0) {
        const auto used = static_cast<unsigned>(bit_count % 8);
        result[abi::bin_layout::payload_offset + bytes - 1] &= static_cast<unsigned char>(0xffU << (8U - used));
    }
    return result;
}

// A bin element index outside the bin's length, at the index operand.
[[noreturn]] static void bin_index_failure(long long index, long long length,
                                           unsigned long long line, unsigned long long column) {
    quidra::runtime::report_failure(abi::FailureReason::index_out_of_bounds,
                                    abi::FailureArgs{.index = index, .length = length}, line,
                                    column);
}

extern "C" void* quidra_bin_index(void* raw, long long index, unsigned long long line,
                                  unsigned long long column) {
    const auto length = bin_length(raw);
    if (index < 0 || index >= length) bin_index_failure(index, length, line, column);
    auto* result = quidra_bin_alloc(1, 0);
    bin_set_bit(result, 0, bin_bit_at(raw, index));
    return result;
}

extern "C" void quidra_bin_set(void* raw, long long index, void* bit, unsigned long long line,
                               unsigned long long column) {
    const auto length = bin_length(raw);
    if (index < 0 || index >= length) bin_index_failure(index, length, line, column);
    if (bin_length(bit) != 1) runtime_text_failure("bin element assignment requires exactly one bit");
    bin_set_bit(raw, index, bin_bit_at(bit, 0));
}

extern "C" void* quidra_bin_slice(void* raw, long long start, long long end,
                                  unsigned long long line, unsigned long long column) {
    const auto length = bin_length(raw);
    // start:<end: [start, end), or empty when the start is past the end
    // (slice bound rule 5); the start is a position or the length, and the
    // excluded end lies in [-1, length].
    if (start < 0 || start > length || end < -1 || end > length) {
        const auto written = quidra::slice_form_text(
            quidra::SliceStart::inclusive, start, quidra::SliceEnd::exclusive_ascending, end);
        quidra::runtime::report_failure(
            abi::FailureReason::slice_out_of_bounds,
            abi::FailureArgs{.length = length, .slice = written.view()}, line, column);
    }
    if (end < start) return quidra_bin_alloc(0, 0);
    // Current bin.slice callers supply a half-open ascending interval,
    // including empty [length, length), which the shared planner accepts.
    const auto selection = quidra::plan_slice(
        static_cast<std::uint64_t>(length),
        quidra::SliceRequest{.start_form = quidra::SliceStart::inclusive,
                             .start = start,
                             .end_form = quidra::SliceEnd::exclusive_ascending,
                             .end = end,
                             .step = std::nullopt});
    // All endpoint conditions were checked above, so failure is a programming
    // error in the adapter, never a user-facing bounds error.
    if (!selection) runtime_text_failure("invalid checked bin slice plan");
    auto* result = quidra_bin_alloc(end - start, 0);
    for (std::uint64_t offset = 0; offset < selection.plan.count; ++offset) {
        bin_set_bit(result, static_cast<long long>(offset),
                    bin_bit_at(raw, selection.plan.index_at(offset)));
    }
    return result;
}

// A slice of a bin in any form (no step): the plan of
// quidra_slice_plan over its bits, then the selected bits in order.
extern "C" void* quidra_bin_slice_planned(void* raw, long long start, int start_form,
                                          long long end, int end_form, int direction,
                                          unsigned long long line, unsigned long long column) {
    const auto length = bin_length(raw);
    const auto plan = plan_slice_or_fail(length, start, start_form, end, end_form, 0, 0,
                                         direction, line, column);
    auto* result = quidra_bin_alloc(static_cast<long long>(plan.count), 0);
    for (std::uint64_t offset = 0; offset < plan.count; ++offset) {
        bin_set_bit(result, static_cast<long long>(offset),
                    bin_bit_at(raw, plan.index_at(offset)));
    }
    return result;
}

extern "C" void* quidra_bin_parse(const char* text) {
    if (!text) return nullptr;
    const auto length_size = std::strlen(text);
    if (length_size > static_cast<std::size_t>(std::numeric_limits<long long>::max())) return nullptr;
    for (std::size_t i = 0; i < length_size; ++i)
        if (text[i] != '0' && text[i] != '1') return nullptr;
    auto* result = quidra_bin_alloc(static_cast<long long>(length_size), 0);
    for (std::size_t i = 0; i < length_size; ++i)
        if (text[i] == '1') bin_set_bit(result, static_cast<long long>(i), true);
    return result;
}

extern "C" char* quidra_bin_string(void* raw) {
    const auto bit_count = bin_length(raw);
    const auto count = static_cast<unsigned long long>(bit_count);
    if (count > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()) - 1ULL)
        runtime_allocation_failure();
    auto* result = static_cast<char*>(
        managed_allocate_string(static_cast<std::size_t>(count) + 1));
    for (long long i = 0; i < bit_count; ++i)
        result[i] = bin_bit_at(raw, i) ? '1' : '0';
    result[bit_count] = '\0';
    mark_managed_string(result, static_cast<std::size_t>(count),
                        static_cast<std::size_t>(count));
    return result;
}

extern "C" void* quidra_bin_from_u64(unsigned long long value, int width) {
    if (width <= 0 || width > 64) runtime_text_failure("unsupported bin scalar width");
    auto* result = quidra_bin_alloc(width, 0);
    for (int i = 0; i < width; ++i) {
        const auto shift = static_cast<unsigned>(width - 1 - i);
        bin_set_bit(result, i, ((value >> shift) & 1ULL) != 0);
    }
    return result;
}

extern "C" unsigned long long quidra_bin_to_u64(void* raw, int width) {
    if (width <= 0 || width > 64) runtime_text_failure("unsupported bin scalar width");
    if (bin_length(raw) != width) runtime_text_failure("bin length does not match destination type width");
    unsigned long long value = 0;
    for (int i = 0; i < width; ++i)
        value = (value << 1U) | (bin_bit_at(raw, i) ? 1ULL : 0ULL);
    return value;
}

extern "C" void* quidra_bin_clone(void* raw) {
    const auto bit_count = bin_length(raw);
    const auto bytes = bin_payload_bytes(bit_count);
    auto* result = static_cast<unsigned char*>(managed_allocate(abi::bin_layout::payload_offset + bytes));
    std::memcpy(result, raw, abi::bin_layout::payload_offset + bytes);
    return result;
}

extern "C" bool quidra_bin_equal(void* left, void* right) {
    if (left == right) return true;
    if (!left || !right) return false;
    const auto left_bits = bin_length(left);
    const auto right_bits = bin_length(right);
    if (left_bits != right_bits) return false;
    const auto bytes = bin_payload_bytes(left_bits);
    return bytes == 0 || std::memcmp(
        static_cast<unsigned char*>(left) + abi::bin_layout::payload_offset,
        static_cast<unsigned char*>(right) + abi::bin_layout::payload_offset,
        bytes) == 0;
}

extern "C" long long quidra_bin_byte_length(void* raw) {
    const auto bits = bin_length(raw);
    if (bits % 8 != 0)
        runtime_text_failure("bin length is not byte-aligned for this operation");
    return bits / 8;
}

extern "C" char* quidra_u8_array_try_utf8(void* raw) {
    if (!raw) return nullptr;
    long long signed_count = 0;
    std::memcpy(&signed_count, raw, sizeof(signed_count));
    if (signed_count < 0) return nullptr;
    const auto count = static_cast<unsigned long long>(signed_count);
    return quidra_runtime_try_copy_text_bytes(
        static_cast<const char*>(raw) + abi::array_layout::payload_offset, count);
}

extern "C" char* quidra_bin_try_utf8(void* raw) {
    if (!raw) return nullptr;
    const auto bits = bin_length(raw);
    if (bits % 8 != 0) return nullptr;
    return quidra_runtime_try_copy_text_bytes(
        static_cast<const char*>(raw) + abi::bin_layout::payload_offset,
        static_cast<unsigned long long>(bits / 8));
}

extern "C" void* quidra_bin_from_array(void* raw, int width, int stride) {
    if (!raw || width <= 0 || width > 64 || stride <= 0)
        runtime_text_failure("invalid bin array conversion");
    long long signed_count = 0;
    std::memcpy(&signed_count, raw, sizeof(signed_count));
    if (signed_count < 0) runtime_text_failure("invalid array length in bin conversion");
    const auto count = static_cast<unsigned long long>(signed_count);
    if (count != 0 && static_cast<unsigned long long>(width) >
        static_cast<unsigned long long>(std::numeric_limits<long long>::max()) / count)
        runtime_allocation_failure();
    const auto total_bits = static_cast<long long>(count * static_cast<unsigned long long>(width));
    auto* result = quidra_bin_alloc(total_bits, 0);
    const auto* data = static_cast<const unsigned char*>(raw) + abi::array_layout::payload_offset;
    if (width == 8 && stride == 1) {
        if (count != 0)
            std::memcpy(static_cast<unsigned char*>(result) + abi::bin_layout::payload_offset, data,
                        static_cast<std::size_t>(count));
        return result;
    }
    for (unsigned long long i = 0; i < count; ++i) {
        unsigned long long value = 0;
        if (width == 1) {
            value = data[i * static_cast<unsigned long long>(stride)] ? 1ULL : 0ULL;
        } else {
            std::memcpy(&value, data + i * static_cast<unsigned long long>(stride),
                        static_cast<std::size_t>(stride));
        }
        for (int bit = 0; bit < width; ++bit) {
            const auto shift = static_cast<unsigned>(width - 1 - bit);
            bin_set_bit(result,
                        static_cast<long long>(i * static_cast<unsigned long long>(width) +
                                               static_cast<unsigned long long>(bit)),
                        ((value >> shift) & 1ULL) != 0);
        }
    }
    return result;
}

extern "C" void* quidra_bin_to_array(void* raw, int width, int stride) {
    if (!raw || width <= 0 || width > 64 || stride <= 0)
        runtime_text_failure("invalid bin array conversion");
    const auto bits = bin_length(raw);
    if (bits % width != 0)
        runtime_text_failure("bin length is not divisible by destination element width");
    const auto count = bits / width;
    const auto count_size = static_cast<std::size_t>(count);
    if (count < 0 || static_cast<long long>(count_size) != count ||
        (count_size != 0 &&
         static_cast<std::size_t>(stride) >
             (std::numeric_limits<std::size_t>::max() - abi::array_layout::payload_offset) / count_size))
        runtime_allocation_failure();
    auto* result = static_cast<unsigned char*>(
        managed_allocate(abi::array_layout::payload_offset + count_size * static_cast<std::size_t>(stride)));
    std::memcpy(result, &count, sizeof(count));
    auto* data = result + abi::array_layout::payload_offset;
    if (width == 8 && stride == 1) {
        if (count_size != 0)
            std::memcpy(data, static_cast<const unsigned char*>(raw) + abi::bin_layout::payload_offset,
                        count_size);
        quidra_init_create(result, static_cast<unsigned long long>(count_size),
                           1, abi::array_layout::payload_offset, 1);
        return result;
    }
    for (long long i = 0; i < count; ++i) {
        unsigned long long value = 0;
        for (int bit = 0; bit < width; ++bit)
            value = (value << 1U) |
                    (bin_bit_at(raw, i * static_cast<long long>(width) + bit) ? 1ULL : 0ULL);
        if (width == 1) {
            data[static_cast<std::size_t>(i) * static_cast<std::size_t>(stride)] =
                value ? 1 : 0;
        } else {
            std::memcpy(data + static_cast<std::size_t>(i) * static_cast<std::size_t>(stride),
                        &value, static_cast<std::size_t>(stride));
        }
    }
    quidra_init_create(result, static_cast<unsigned long long>(count_size),
                       static_cast<unsigned long long>(stride), abi::array_layout::payload_offset, 1);
    return result;
}

extern "C" char* quidra_string_repeat(long long count, const char* fill) {
    if (count < 0) runtime_text_failure("string length cannot be negative");
    if (!fill) runtime_text_failure("null string fill");
    ManagedAllocation* fill_allocation = nullptr;
    std::size_t fill_codepoints = 0;
    const auto unit =
        validated_string_view(fill, fill_allocation, &fill_codepoints);
    if (fill_codepoints != 1)
        runtime_text_failure("string fill must contain exactly one Unicode code point");
    const auto n = static_cast<std::size_t>(count);
    if (count != static_cast<long long>(n) ||
        (!unit.empty() && n > (std::numeric_limits<std::size_t>::max() - 1) / unit.size()))
        runtime_allocation_failure();
    const auto bytes = n * unit.size();
    auto* result = static_cast<char*>(managed_allocate_string(bytes + 1));
    for (std::size_t i = 0; i < n; ++i)
        if (!unit.empty()) std::memcpy(result + i * unit.size(), unit.data(), unit.size());
    result[bytes] = '\0';
    mark_managed_string(result, bytes, n);
    return result;
}

extern "C" void* quidra_string_utf8(const char* text) {
    if (!text) runtime_text_failure("null string");
    ManagedAllocation* allocation = nullptr;
    const auto source = validated_string_view(text, allocation);
    if (source.size() > (std::numeric_limits<std::size_t>::max() - abi::bin_layout::payload_offset) ||
        source.size() > static_cast<std::size_t>(std::numeric_limits<long long>::max() / 8)) {
        runtime_allocation_failure();
    }
    auto* result = static_cast<unsigned char*>(managed_allocate(abi::bin_layout::payload_offset + source.size()));
    const auto count = static_cast<long long>(source.size() * 8);
    std::memcpy(result, &count, sizeof(count));
    if (!source.empty()) std::memcpy(result + abi::bin_layout::payload_offset, source.data(), source.size());
    return result;
}

extern "C" void* quidra_string_codepoints(const char* text) {
    if (!text) runtime_text_failure("null string");
    ManagedAllocation* allocation = nullptr;
    std::size_t count_size = 0;
    const auto source =
        validated_string_view(text, allocation, &count_size);
    if (count_size > (std::numeric_limits<std::size_t>::max() - abi::array_layout::payload_offset) / sizeof(long long)) {
        runtime_allocation_failure();
    }
    auto* result = static_cast<unsigned char*>(
        managed_allocate(abi::array_layout::payload_offset + count_size * sizeof(long long)));
    const auto count = static_cast<long long>(count_size);
    std::memcpy(result, &count, sizeof(count));
    std::size_t byte_index = 0;
    std::size_t out_index = 0;
    while (byte_index < source.size()) {
        // An int element is the inline word of the code point.
        const auto codepoint = static_cast<long long>(utf8_next(source, byte_index)) << 1;
        std::memcpy(result + abi::array_layout::payload_offset + out_index * sizeof(long long),
                    &codepoint, sizeof(codepoint));
        ++out_index;
    }
    return result;
}

extern "C" char* quidra_string_join(void* raw, const char* separator,
                                      unsigned long long line,
                                      unsigned long long column) {
    if (!raw || !separator) runtime_text_failure("null string join input");
    long long signed_count = 0;
    std::memcpy(&signed_count, raw, sizeof(signed_count));
    if (signed_count < 0) runtime_text_failure("invalid string array length");
    const auto count = static_cast<std::size_t>(signed_count);
    if (count > (std::numeric_limits<std::size_t>::max() - abi::array_layout::payload_offset) / sizeof(char*)) {
        runtime_allocation_failure();
    }
    if (count != 0) {
        quidra_init_require_range(
            static_cast<unsigned char*>(raw) + abi::array_layout::payload_offset,
            static_cast<unsigned long long>(count * sizeof(char*)), line, column);
    }

    ManagedAllocation* delimiter_allocation = nullptr;
    std::size_t delimiter_codepoints = 0;
    const auto delimiter = validated_string_view(
        separator, delimiter_allocation, &delimiter_codepoints);

    // A common builder shape is ["a", "b", ...].join(""). Every element is
    // already a valid immutable string, so single-byte ASCII pieces can be
    // copied directly without per-piece managed lookup or UTF-8 validation.
    if (delimiter.empty() && count != 0) {
        bool single_ascii_join = true;
        for (std::size_t i = 0; i < count; ++i) {
            char* item = nullptr;
            std::memcpy(&item,
                        static_cast<unsigned char*>(raw) + abi::array_layout::payload_offset + i * sizeof(char*),
                        sizeof(item));
            if (!item) runtime_text_failure("null string in join");
            const auto byte = static_cast<unsigned char>(item[0]);
            if (byte == 0 || byte >= 0x80U || item[1] != '\0') {
                single_ascii_join = false;
                break;
            }
        }
        if (single_ascii_join) {
            if (count == std::numeric_limits<std::size_t>::max())
                runtime_allocation_failure();
            auto* result =
                static_cast<char*>(managed_allocate_string(count + 1));
            for (std::size_t i = 0; i < count; ++i) {
                char* item = nullptr;
                std::memcpy(&item,
                            static_cast<unsigned char*>(raw) + abi::array_layout::payload_offset + i * sizeof(char*),
                            sizeof(item));
                result[i] = item[0];
            }
            result[count] = '\0';
            mark_managed_string(result, count, count);
            return result;
        }
    }

    std::size_t total = 0;
    std::size_t total_codepoints = 0;
    if (count > 1 && delimiter.size() != 0) {
        if (count - 1 > std::numeric_limits<std::size_t>::max() / delimiter.size()) {
            runtime_allocation_failure();
        }
        total = (count - 1) * delimiter.size();
        if (delimiter_codepoints != 0 &&
            count - 1 > std::numeric_limits<std::size_t>::max() / delimiter_codepoints) {
            runtime_allocation_failure();
        }
        total_codepoints = (count - 1) * delimiter_codepoints;
    }

    constexpr std::size_t small_join_count = 16;
    std::array<std::string_view, small_join_count> small_pieces{};
    std::vector<std::string_view> large_pieces;
    if (count > small_join_count) large_pieces.resize(count);
    auto* pieces = count <= small_join_count
        ? small_pieces.data() : large_pieces.data();
    for (std::size_t i = 0; i < count; ++i) {
        char* item = nullptr;
        std::memcpy(&item,
                    static_cast<unsigned char*>(raw) + abi::array_layout::payload_offset + i * sizeof(char*),
                    sizeof(item));
        if (!item) runtime_text_failure("null string in join");
        ManagedAllocation* item_allocation = nullptr;
        std::size_t item_codepoints = 0;
        const auto piece =
            validated_string_view(item, item_allocation, &item_codepoints);
        if (piece.size() > std::numeric_limits<std::size_t>::max() - total) {
            runtime_allocation_failure();
        }
        total += piece.size();
        if (item_codepoints >
            std::numeric_limits<std::size_t>::max() - total_codepoints) {
            runtime_allocation_failure();
        }
        total_codepoints += item_codepoints;
        pieces[i] = piece;
    }
    if (total == std::numeric_limits<std::size_t>::max()) {
        runtime_allocation_failure();
    }

    auto* result = static_cast<char*>(managed_allocate_string(total + 1));
    std::size_t offset = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0 && !delimiter.empty()) {
            std::memcpy(result + offset, delimiter.data(), delimiter.size());
            offset += delimiter.size();
        }
        if (!pieces[i].empty()) {
            std::memcpy(result + offset, pieces[i].data(), pieces[i].size());
            offset += pieces[i].size();
        }
    }
    result[total] = '\0';
    mark_managed_string(result, total, total_codepoints);
    return result;
}



extern "C" void quidra_runtime_set_args(int argc, char** argv) {
    quidra::counters::mark_program_started();
    quidra_runtime_start_observation();
    std::lock_guard<std::mutex> lock(runtime_args_mutex);
    runtime_argc = argc;
    runtime_argv = argv;
    runtime_used.assign(static_cast<std::size_t>(argc > 0 ? argc : 0), 0);
    if (!runtime_used.empty()) runtime_used[0] = 1;
}

extern "C" int quidra_runtime_argc() {
    return runtime_argc;
}

extern "C" const char* quidra_runtime_argv(int index) {
    if (index < 0 || index >= runtime_argc || !runtime_argv) return nullptr;
    return runtime_argv[index];
}

extern "C" const char* quidra_cli_argument(long long index) {
    const long long actual = index + 1;
    if (actual <= 0 || actual >= runtime_argc || !runtime_argv) {
        cli_fail("missing positional argument");
    }
    const char* value = runtime_argv[actual];
    if (!value || (value[0] == '-' && value[1] == '-')) {
        cli_fail("missing positional argument");
    }
    if (!valid_runtime_text(value)) {
        cli_fail("argument value must be valid UTF-8 text without NUL");
    }
    mark_used(static_cast<int>(actual));
    return value;
}

extern "C" const char* quidra_cli_argument_optional(long long index) {
    const long long actual = index + 1;
    if (actual <= 0 || actual >= runtime_argc || !runtime_argv) return nullptr;
    const char* value = runtime_argv[actual];
    if (!value || (value[0] == '-' && value[1] == '-')) return nullptr;
    if (!valid_runtime_text(value)) {
        cli_fail("argument value must be valid UTF-8 text without NUL");
    }
    mark_used(static_cast<int>(actual));
    return value;
}

extern "C" const char* quidra_cli_option(const char* name) {
    const int index = find_option(name);
    if (index < 0) return nullptr;
    if (index + 1 >= runtime_argc) cli_fail("option requires a value");
    const char* value = runtime_argv[index + 1];
    if (!value || !valid_runtime_text(value)) {
        cli_fail("option value must be valid UTF-8 text without NUL");
    }
    mark_used(index);
    mark_used(index + 1);
    return value;
}

extern "C" bool quidra_cli_flag(const char* name) {
    const int index = find_option(name);
    if (index < 0) return false;
    mark_used(index);
    return true;
}

extern "C" void quidra_cli_finish() {
    std::lock_guard<std::mutex> lock(runtime_args_mutex);
    for (int i = 1; i < runtime_argc; ++i) {
        if (i >= static_cast<int>(runtime_used.size()) || !runtime_used[static_cast<std::size_t>(i)]) {
            cli_fail("unknown, duplicate, or misplaced argument");
        }
    }
}

extern "C" int quidra_input_read(char** out) {
    if (!out) return abi::read_line_status::failed;
    *out = nullptr;

    // A failure records its reason's position in INPUT_READ's list
    // (INVALID_UTF8, EMBEDDED_NUL, IO_FAILURE) for the error block.
    std::string text;
    if (!std::getline(std::cin, text)) {
        if (!std::cin.bad() && std::cin.eof()) return abi::read_line_status::end_of_input;
        quidra::runtime::record_failure_reason(2);
        return abi::read_line_status::failed;
    }

    std::size_t codepoints = 0;
    bool contains_nul = false;
    if (!valid_utf8(text, &codepoints, &contains_nul) || contains_nul) {
        quidra::runtime::record_failure_reason(contains_nul ? 1 : 0);
        return abi::read_line_status::failed;
    }
    *out = copy_validated_runtime_text(text, codepoints);
    return abi::read_line_status::line;
}

extern "C" bool quidra_string_parse_two_signed(
    const char* text, unsigned char separator,
    long long* out_left, long long* out_right) {
    if (!out_left || !out_right) return false;
    *out_left = 0;
    *out_right = 0;
    if (!text || separator == 0) return false;

    ManagedAllocation* allocation = nullptr;
    const auto source = cached_string_view(text, allocation);
    const char* cursor = source.data();
    const char* const end = cursor + source.size();

    // Internal success-only fast path for split(single-byte separator) followed
    // by two signed int parses. Delimiter discovery and decimal parsing share
    // one pass over each field. Spellings outside this canonical subset return
    // false so the compiler-emitted original split/parse path preserves every
    // public parsing and error semantic.
    const auto parse_field =
        [separator](const char*& current, const char* finish,
                    bool require_separator, long long& out) {
            if (current == finish) return false;

            bool negative = false;
            if (*current == '-') {
                negative = true;
                ++current;
                if (current == finish ||
                    static_cast<unsigned char>(*current) == separator) {
                    return false;
                }
            }

            constexpr auto positive_limit =
                static_cast<unsigned long long>(
                    std::numeric_limits<long long>::max());
            constexpr auto magnitude_cutoff = positive_limit / 10ULL;
            constexpr auto positive_last_digit =
                static_cast<unsigned>(positive_limit % 10ULL);
            const auto last_digit_limit =
                positive_last_digit + (negative ? 1U : 0U);

            unsigned long long magnitude = 0;
            bool saw_digit = false;
            while (current != finish &&
                   static_cast<unsigned char>(*current) != separator) {
                const auto byte = static_cast<unsigned char>(*current);
                if (byte < static_cast<unsigned char>('0') ||
                    byte > static_cast<unsigned char>('9')) {
                    return false;
                }
                const auto digit = static_cast<unsigned>(byte - '0');
                if (magnitude > magnitude_cutoff ||
                    (magnitude == magnitude_cutoff &&
                     digit > last_digit_limit)) {
                    return false;
                }
                magnitude = magnitude * 10ULL + digit;
                saw_digit = true;
                ++current;
            }
            if (!saw_digit) return false;

            if (require_separator) {
                if (current == finish) return false;
                ++current;
            }

            if (negative) {
                if (magnitude == positive_limit + 1ULL) {
                    out = std::numeric_limits<long long>::min();
                } else {
                    out = -static_cast<long long>(magnitude);
                }
            } else {
                out = static_cast<long long>(magnitude);
            }
            return true;
        };

    long long left = 0;
    long long right = 0;
    if (!parse_field(cursor, end, true, left) ||
        !parse_field(cursor, end, false, right)) {
        return false;
    }

    *out_left = left;
    *out_right = right;
    return true;
}

namespace {

// A failed numeric parse: records its reason's position in NUMERIC_PARSE's
// list (INVALID_TEXT, OUT_OF_RANGE) for the error block that reads it.
bool parse_failed(bool out_of_range) {
    quidra::runtime::record_failure_reason(out_of_range ? 1 : 0);
    return false;
}

// Text that spells a negative integer ("-" and digits): out of the range of
// an unsigned type rather than not a number.
bool negative_integer_text(const char* text) {
    if (*text != '-' || text[1] == '\0') return false;
    for (const char* c = text + 1; *c; ++c)
        if (*c < '0' || *c > '9') return false;
    return true;
}

} // namespace

extern "C" bool quidra_parse_signed(const char* text, long long* out) {
    if (!text || !out || !*text) return parse_failed(false);

    // Fast path for canonical decimal text produced by Quidra itself and by the
    // common file/serialization path. from_chars is locale-free and allocation-free.
    ManagedAllocation* allocation = nullptr;
    const auto view = cached_string_view(text, allocation);
    long long value = 0;
    const auto parsed = std::from_chars(
        view.data(), view.data() + view.size(), value, 10);
    if (parsed.ec == std::errc{} && parsed.ptr == view.data() + view.size()) {
        *out = value;
        return true;
    }

    // Keep the historical acceptance rules (leading whitespace / '+') for
    // non-canonical user input instead of changing language behaviour.
    errno = 0;
    char* end = nullptr;
    value = std::strtoll(text, &end, 10);
    if (errno == ERANGE) return parse_failed(true);
    if (end == text || !end || *end != '\0') return parse_failed(false);
    *out = value;
    return true;
}

extern "C" bool quidra_parse_unsigned(const char* text, unsigned long long* out) {
    if (!text || !out || !*text) return parse_failed(false);
    if (*text == '-') return parse_failed(negative_integer_text(text));

    ManagedAllocation* allocation = nullptr;
    const auto view = cached_string_view(text, allocation);
    unsigned long long value = 0;
    const auto parsed = std::from_chars(
        view.data(), view.data() + view.size(), value, 10);
    if (parsed.ec == std::errc{} && parsed.ptr == view.data() + view.size()) {
        *out = value;
        return true;
    }

    errno = 0;
    char* end = nullptr;
    value = std::strtoull(text, &end, 10);
    if (errno == ERANGE) return parse_failed(true);
    if (end == text || !end || *end != '\0') return parse_failed(false);
    *out = value;
    return true;
}

extern "C" char* quidra_integer_text_signed(long long value) {
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 10);
    if (converted.ec != std::errc{}) runtime_text_failure("integer formatting failed");
    return copy_validated_runtime_text(
        std::string_view(buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())),
        static_cast<std::size_t>(converted.ptr - buffer.data()));
}

extern "C" char* quidra_integer_text_unsigned(unsigned long long value) {
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 10);
    if (converted.ec != std::errc{}) runtime_text_failure("integer formatting failed");
    return copy_validated_runtime_text(
        std::string_view(buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())),
        static_cast<std::size_t>(converted.ptr - buffer.data()));
}

namespace {

enum class DecimalReal { not_decimal, out_of_range, ok };

// Reads real text that is decimal, after optional leading white space, by
// rounding its exact value once to the nearest value of Real's format, ties
// to even, subnormals included (ieee_decimal.hpp). A value beyond the finite
// range, and a value that is not zero but rounds to zero, is out of range.
// Other text that strtod reads (hexadecimal, infinities, NaNs) is not
// decimal; the caller decides.
template <class Real>
DecimalReal read_decimal_real(const char* text, Real& out) {
    namespace ieee = quidra::ieee_decimal;
    static_assert(std::is_same_v<Real, float> || std::is_same_v<Real, double>);
    const char* start = text;
    while (*start == ' ' || (*start >= '\t' && *start <= '\r')) ++start;
    ieee::Decimal decimal;
    if (!ieee::parse(start, decimal)) return DecimalReal::not_decimal;
    if constexpr (std::is_same_v<Real, float>) {
        const auto rounded = ieee::round(decimal, ieee::binary32);
        if (rounded.overflow || rounded.underflow_to_zero) return DecimalReal::out_of_range;
        out = ieee::binary32_value(rounded);
    } else {
        const auto rounded = ieee::round(decimal, ieee::binary64);
        if (rounded.overflow || rounded.underflow_to_zero) return DecimalReal::out_of_range;
        out = ieee::binary64_value(rounded);
    }
    return DecimalReal::ok;
}

} // namespace

extern "C" bool quidra_parse_float32(const char* text, float* out) {
    if (!text || !out || !*text) return parse_failed(false);
    float value = 0.0F;
    switch (read_decimal_real(text, value)) {
        case DecimalReal::ok: *out = value; return true;
        case DecimalReal::out_of_range: return parse_failed(true);
        case DecimalReal::not_decimal: break;
    }
    errno = 0;
    char* end = nullptr;
    value = std::strtof(text, &end);
    if (errno == ERANGE) return parse_failed(true);
    if (end == text || !end || *end != '\0' || std::isnan(value)) return parse_failed(false);
    *out = value;
    return true;
}

extern "C" bool quidra_parse_float64(const char* text, double* out) {
    if (!text || !out || !*text) return parse_failed(false);
    double value = 0.0;
    switch (read_decimal_real(text, value)) {
        case DecimalReal::ok: *out = value; return true;
        case DecimalReal::out_of_range: return parse_failed(true);
        case DecimalReal::not_decimal: break;
    }
    errno = 0;
    char* end = nullptr;
    value = std::strtod(text, &end);
    if (errno == ERANGE) return parse_failed(true);
    if (end == text || !end || *end != '\0' || std::isnan(value)) return parse_failed(false);
    *out = value;
    return true;
}

extern "C" long long quidra_cli_parse_int(const char* text) {
    if (!text || !*text) cli_fail("invalid int64 value");
    errno = 0;
    char* end = nullptr;
    const auto value = std::strtoll(text, &end, 10);
    if (errno == ERANGE || end == text || !end || *end != '\0') cli_fail("invalid int64 value");
    return value;
}

extern "C" double quidra_cli_parse_float(const char* text) {
    if (!text || !*text) cli_fail("invalid real64 value");
    double value = 0.0;
    switch (read_decimal_real(text, value)) {
        case DecimalReal::ok: return value;
        case DecimalReal::out_of_range: cli_fail("invalid real64 value");
        case DecimalReal::not_decimal: break;
    }
    errno = 0;
    char* end = nullptr;
    value = std::strtod(text, &end);
    if (errno == ERANGE || end == text || !end || *end != '\0' || std::isnan(value)) cli_fail("invalid real64 value");
    return value;
}

extern "C" void* quidra_cli_parse_bigint(const char* text) {
    auto* value = quidra_bigint_parse(text);
    if (!value) cli_fail("invalid int value");
    return value;
}

extern "C" void* quidra_cli_parse_bigreal(const char* text) {
    auto* value = quidra_bigreal_parse(text);
    if (!value) cli_fail("invalid real value");
    return value;
}

extern "C" long long quidra_cli_parse_nat(const char* text){
    auto* parsed=quidra_bigint_parse(text);
    if(!parsed)cli_fail("invalid nat value");
    const auto value=quidra_int_adopt(parsed);
    if(quidra_int_compare(value,0)<0){
        const auto word=static_cast<std::uintptr_t>(value);
        if((word & abi::bare_integer_layout::boxed_bit)!=0)
            quidra_managed_release(reinterpret_cast<void*>(word &
                ~static_cast<std::uintptr_t>(abi::bare_integer_layout::boxed_bit)),
                reinterpret_cast<void*>(&quidra_bigint_drop));
        cli_fail("invalid nat value");
    }
    return value;
}

extern "C" unsigned long long quidra_cli_parse_numeric(const char* text,int dtype){
    const auto invalid=[&](){const auto message="invalid "+std::string(abi::conversion_type_name(dtype))+" value";cli_fail(message.c_str());};
    if(dtype>=QCORE_DTYPE_INT64&&dtype<=QCORE_DTYPE_INT32){
        long long value=0;
        if(!quidra_parse_signed(text,&value))invalid();
        const int width=dtype==QCORE_DTYPE_INT8?8:dtype==QCORE_DTYPE_INT16?16:dtype==QCORE_DTYPE_INT32?32:64;
        if(width<64&&(value<-(1LL<<(width-1))||value>((1LL<<(width-1))-1)))invalid();
        return static_cast<unsigned long long>(value);
    }
    if(dtype>=QCORE_DTYPE_UINT8&&dtype<=QCORE_DTYPE_UINT64){
        unsigned long long value=0;
        if(!quidra_parse_unsigned(text,&value))invalid();
        const int width=dtype==QCORE_DTYPE_UINT8?8:dtype==QCORE_DTYPE_UINT16?16:dtype==QCORE_DTYPE_UINT32?32:64;
        if(width<64&&value>((1ULL<<width)-1))invalid();
        return value;
    }
    if(dtype==QCORE_DTYPE_FLOAT32){float value=0;if(!quidra_parse_float32(text,&value))invalid();return std::bit_cast<std::uint32_t>(value);}
    if(dtype==QCORE_DTYPE_FLOAT16||dtype==QCORE_DTYPE_BFLOAT16){
        unsigned short value=0;if(!quidra_half_parse(text,dtype==QCORE_DTYPE_BFLOAT16?1:0,&value))invalid();return value;
    }
    invalid();return 0;
}

extern "C" bool quidra_cli_parse_bool(const char* text) {
    if (text && std::strcmp(text, "true") == 0) return true;
    if (text && std::strcmp(text, "false") == 0) return false;
    cli_fail("bool values must be true or false");
}
