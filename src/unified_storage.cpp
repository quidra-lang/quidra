#include "unified_storage.hpp"
#include "platform/environment.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__APPLE__)
#include <unistd.h>
#endif

namespace quidra::unified {
namespace {

// The value of an environment variable, "" when unset.
std::string environment_text(const char* name) {
    return platform::environment_value(name).value_or("");
}

bool environment_disabled(const char* name) {
    const auto text = environment_text(name);
    return text == "0" || text == "off" || text == "false";
}

bool environment_enabled(const char* name) {
    const auto text = environment_text(name);
    return !text.empty() && text != "0" && text != "off" && text != "false";
}

std::array<std::atomic<std::uint64_t>,
           static_cast<std::size_t>(Counter::Count)> counters{};

void print_counters() {
    std::fprintf(
        stderr,
        "quidra unified memory: cpu_views=%llu device_views=%llu wraps=%llu "
        "relocations=%llu host_waits=%llu\n",
        static_cast<unsigned long long>(counters[0].load()),
        static_cast<unsigned long long>(counters[1].load()),
        static_cast<unsigned long long>(counters[2].load()),
        static_cast<unsigned long long>(counters[3].load()),
        static_cast<unsigned long long>(counters[4].load()));
}

bool statistics_enabled() {
    static const bool value = [] {
        const bool on = environment_enabled("QUIDRA_UNIFIED_MEMORY_STATS");
        if (on) std::atexit(print_counters);
        return on;
    }();
    return value;
}

} // namespace

std::size_t host_page_bytes() {
#if defined(__APPLE__)
    static const std::size_t value = [] {
        const long page = ::getpagesize();
        return page > 0 ? static_cast<std::size_t>(page) : std::size_t{16384};
    }();
    return value;
#else
    return 0;
#endif
}

bool enabled() {
    static const bool value = [] {
        // Registers the exit report now, so that it also appears when no
        // transfer becomes a view.
        statistics_enabled();
        return !environment_disabled("QUIDRA_UNIFIED_MEMORY");
    }();
    return value;
}

bool cpu_views_enabled() {
    static const bool value =
        enabled() && environment_text("QUIDRA_UNIFIED_MEMORY") != "upload";
    return value;
}

unsigned char* allocate_host(std::size_t bytes, std::size_t& capacity) {
    capacity = 0;
    if (bytes == 0) return nullptr;
    void* pointer = nullptr;
    std::size_t allocated = bytes;
#if defined(__APPLE__)
    // posix_memalign exists only on POSIX hosts (not the MSVC CRT); the page
    // shape is needed only by Metal, so other platforms keep plain malloc.
    const auto page = host_page_bytes();
    if (page != 0 && bytes >= wrap_minimum_bytes && enabled()) {
        // Page aligned and page rounded: the exact shape Metal accepts for
        // newBufferWithBytesNoCopy, so this allocation can later be viewed by
        // the GPU without copying it.
        if (bytes > static_cast<std::size_t>(-1) - (page - 1)) throw std::bad_alloc();
        allocated = (bytes + page - 1) / page * page;
        if (::posix_memalign(&pointer, page, allocated) != 0) pointer = nullptr;
    } else {
        pointer = std::malloc(bytes);
    }
#else
    pointer = std::malloc(bytes);
#endif
    if (!pointer) throw std::bad_alloc();
    std::memset(pointer, 0, allocated);
    capacity = allocated;
    return static_cast<unsigned char*>(pointer);
}

void free_host(void* pointer, std::size_t) {
    std::free(pointer);
}

void HostBytes::resize(std::size_t bytes) {
    if (bytes == size_) return;
    if (bytes == 0) {
        reset();
        return;
    }
    std::size_t capacity = 0;
    auto* fresh = allocate_host(bytes, capacity);
    if (data_ && size_ != 0) std::memcpy(fresh, data_, size_ < bytes ? size_ : bytes);
    reset();
    data_ = fresh;
    size_ = bytes;
    capacity_ = capacity;
    deallocate_ = &free_host;
}

Block* create_block(quidra::device::Buffer* buffer, unsigned char* host,
                    std::size_t bytes, int device) {
    auto* block = new Block;
    block->buffer = buffer;
    block->host = host;
    block->bytes = bytes;
    block->device = device;
    return block;
}

void attach(Block& block) {
    block.views.fetch_add(1, std::memory_order_acq_rel);
}

void settle(Block& block, std::uint64_t serial) {
    block.settled_serial.store(serial, std::memory_order_relaxed);
    block.device_borrowed.store(false, std::memory_order_relaxed);
    block.device_reads_possible.store(false, std::memory_order_release);
}

void detach(Block* block, bool device_view) {
    if (!block) return;
    if (device_view) {
        // Published before the view count drops, so whoever then finds itself
        // the only view (acquire on `views`) sees it.
        const auto serial = quidra::device::submission_serial(block->device);
        if (block->device_borrowed.load(std::memory_order_relaxed) ||
            serial == quidra::device::unknown_submission_serial ||
            serial != block->settled_serial.load(std::memory_order_relaxed))
            block->device_reads_possible.store(true, std::memory_order_release);
    }
    if (block->views.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    // The device keeps the underlying memory alive until submitted work that
    // references the buffer has completed (Metal retains bound buffers, and a
    // wrapped host allocation is freed by the buffer's deallocator).
    quidra::device::release(block->buffer);
    delete block;
}

void count(Counter counter) {
    if (!statistics_enabled()) return;
    counters[static_cast<std::size_t>(counter)].fetch_add(1, std::memory_order_relaxed);
}

} // namespace quidra::unified
