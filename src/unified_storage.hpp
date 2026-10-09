#pragma once

// Unified-memory tensor transfers (11th optimization, Apple shared storage).
//
// `.gpu(n)` and `.cpu()` stay explicit value boundaries. On a device whose
// buffers are host-addressable (Metal on unified memory), the runtime may
// implement such a transfer as a second *view* of one physical allocation
// instead of a copy. A Block is that allocation; every TensorStorage that
// views it holds one reference. The rules that keep the sharing invisible
// live in runtime_unified.inc; this header only owns the memory objects.
//
//   QUIDRA_UNIFIED_MEMORY=0        every transfer copies (previous behaviour)
//   QUIDRA_UNIFIED_MEMORY=upload   only `.gpu(n)` may be a view; `.cpu()` copies
//   QUIDRA_UNIFIED_MEMORY_STATS=1  print transfer counters to stderr at exit

#include "device_backend.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <utility>

namespace quidra::unified {

// True unless QUIDRA_UNIFIED_MEMORY=0 (or "off"/"false"). Read once.
bool enabled();
// enabled(), and `.cpu()` may be a view too (not QUIDRA_UNIFIED_MEMORY=upload).
bool cpu_views_enabled();

// Host allocations at least this large are page aligned and page rounded on
// Apple platforms, so that `.gpu(n)` can hand them to Metal without a copy.
// Smaller ones are cheaper to copy than to wrap.
inline constexpr std::size_t wrap_minimum_bytes = std::size_t{64} * 1024;
// `.cpu()` serves smaller tensors as copies too: copying them costs a few
// microseconds, less than the view's bookkeeping and any later host wait.
inline constexpr std::size_t cpu_view_minimum_bytes = wrap_minimum_bytes;

using Deallocate = void (*)(void* pointer, std::size_t capacity);

// The host VM page size on Apple platforms (the granularity Metal requires for
// wrapping host memory); 0 elsewhere.
std::size_t host_page_bytes();

// Allocates `bytes` zero-filled host bytes. `capacity` receives the usable
// size of the allocation (>= bytes). Throws std::bad_alloc on failure.
unsigned char* allocate_host(std::size_t bytes, std::size_t& capacity);
void free_host(void* pointer, std::size_t capacity);

// The CPU byte container of a tensor storage (and of the test-only fake GPU
// buffer). It behaves like the std::vector<unsigned char> it replaces, and can
// additionally either view bytes owned elsewhere (a Block) or hand ownership of
// its allocation to a device buffer that wraps it.
class HostBytes {
public:
    HostBytes() noexcept = default;
    ~HostBytes() { reset(); }
    HostBytes(const HostBytes&) = delete;
    HostBytes& operator=(const HostBytes&) = delete;
    HostBytes(HostBytes&& other) noexcept { steal(other); }
    HostBytes& operator=(HostBytes&& other) noexcept {
        if (this != &other) {
            reset();
            steal(other);
        }
        return *this;
    }

    unsigned char* data() noexcept { return data_; }
    const unsigned char* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    unsigned char* begin() noexcept { return data_; }
    unsigned char* end() noexcept { return data_ + size_; }
    const unsigned char* begin() const noexcept { return data_; }
    const unsigned char* end() const noexcept { return data_ + size_; }
    unsigned char& operator[](std::size_t index) noexcept { return data_[index]; }
    const unsigned char& operator[](std::size_t index) const noexcept {
        return data_[index];
    }

    // std::vector::resize semantics: keeps the common prefix, zero-fills the
    // rest. Always leaves the container owning its bytes.
    void resize(std::size_t bytes);

    bool owns() const noexcept { return deallocate_ != nullptr; }
    std::size_t capacity() const noexcept { return capacity_; }
    Deallocate deallocator() const noexcept { return deallocate_; }

    // Non-owning view of bytes kept alive by someone else (a Block).
    void view(unsigned char* data, std::size_t bytes) noexcept {
        reset();
        data_ = data;
        size_ = bytes;
        capacity_ = bytes;
    }
    // Takes ownership of an allocation released later with `deallocate`.
    void adopt(unsigned char* data, std::size_t bytes, std::size_t capacity,
               Deallocate deallocate) noexcept {
        reset();
        data_ = data;
        size_ = bytes;
        capacity_ = capacity;
        deallocate_ = deallocate;
    }
    // Keeps pointing at the same bytes but stops owning them: the caller has
    // handed the allocation to an object that frees it (a wrapping buffer).
    void release_ownership() noexcept { deallocate_ = nullptr; }

private:
    void reset() noexcept {
        if (deallocate_ && data_) deallocate_(data_, capacity_);
        data_ = nullptr;
        size_ = 0;
        capacity_ = 0;
        deallocate_ = nullptr;
    }
    void steal(HostBytes& other) noexcept {
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        capacity_ = std::exchange(other.capacity_, 0);
        deallocate_ = std::exchange(other.deallocate_, nullptr);
    }

    unsigned char* data_{};
    std::size_t size_{};
    std::size_t capacity_{};
    Deallocate deallocate_{};
};

// One physical allocation shared by a device view and CPU views.
struct Block {
    std::atomic<std::size_t> views{0};
    // Device work that may still read the bytes. The host writes a CPU view in
    // place only while it is the only view (no device view exists) and this is
    // false. A device view that leaves the block sets it when it may have been
    // used since the block was last settled: Core submitted device work in
    // the meantime (device::submission_serial changed) or package code
    // borrowed a device handle of a view.
    std::atomic<bool> device_reads_possible{false};
    std::atomic<bool> device_borrowed{false};
    std::atomic<std::uint64_t> settled_serial{0};
    quidra::device::Buffer* buffer{}; // owned; released with the last view
    unsigned char* host{};            // host address of the same bytes
    std::size_t bytes{};
    int device{-1};                   // global gpu index of `buffer`
};

Block* create_block(quidra::device::Buffer* buffer, unsigned char* host,
                    std::size_t bytes, int device);
void attach(Block& block);
// Records that no device work submitted before `serial` was read can still
// use the block. Only valid while the caller owns every view of the block.
void settle(Block& block, std::uint64_t serial);
// Drops one view; the last one releases the device buffer and the block.
// `device_view` says whether the departing view is the device view.
void detach(Block* block, bool device_view);
inline std::size_t views(const Block& block) {
    return block.views.load(std::memory_order_acquire);
}

// Counters for QUIDRA_UNIFIED_MEMORY_STATS and the tests.
enum class Counter : unsigned {
    CpuView,     // gpu(n).cpu() served as a view
    DeviceView,  // cpu.gpu(n) served as a view
    Wrap,        // host allocation handed to the device without a copy
    Relocate,    // a shared view separated before a native borrow
    HostWait,    // host waited for the device before writing a CPU view
    Count
};
void count(Counter counter);

} // namespace quidra::unified
