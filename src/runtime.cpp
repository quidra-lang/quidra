#include "device_backend.hpp"
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

extern "C" void* quidra_bigint_parse(const char*);
extern "C" void* quidra_bigreal_parse(const char*);

extern "C" void dnn_quidra_fast() {
    quidra::device::set_dnn_mode(quidra::device::DnnMode::Fast);
}

extern "C" void dnn_quidra_deterministic() {
    quidra::device::set_dnn_mode(quidra::device::DnnMode::Deterministic);
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
    std::fprintf(stderr, "Quidra runtime error: allocation failed\n");
    std::exit(101);
}

using ManagedDrop = void (*)(void*);

struct InitializationTracker {
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
thread_local ManagedAllocations managed_allocations;

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

thread_local SmallManagedPool small_managed_pool;
thread_local long long string_build_append_last_codepoints = 0;

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
        auto& list = small_managed_pool.free_lists[index];
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
    small_managed_pool.free_lists[
        static_cast<std::size_t>(pool_class - 1)].push_back(base);
}

thread_local const char* cached_managed_string_text = nullptr;
thread_local ManagedAllocation* cached_managed_string_allocation = nullptr;

thread_local const char* cached_shared_string_text = nullptr;
thread_local std::size_t cached_shared_string_length = 0;
thread_local ManagedAllocation* cached_shared_string_allocation = nullptr;

// Repeated xs = xs.append(value) loops hit the same exact allocation for long
// stretches. Keep only the allocation-record lookup cached; every move-safety
// property is still re-read on each operation, so aliases, pins, and
// initialization state retain their ordinary semantics.
thread_local void* cached_array_append_base = nullptr;
thread_local ManagedAllocation* cached_array_append_allocation = nullptr;

ManagedAllocation* exact_array_append_allocation(void* array) {
    if (array && array == cached_array_append_base &&
        cached_array_append_allocation) {
        return cached_array_append_allocation;
    }
    if (!array) return nullptr;
    const auto it =
        managed_allocations.find(reinterpret_cast<std::uintptr_t>(array));
    if (it == managed_allocations.end()) return nullptr;
    cached_array_append_base = array;
    cached_array_append_allocation = &it->second;
    return cached_array_append_allocation;
}

void invalidate_array_append_cache(const ManagedAllocation* allocation) {
    if (cached_array_append_allocation != allocation) return;
    cached_array_append_base = nullptr;
    cached_array_append_allocation = nullptr;
}

ManagedAllocation* exact_managed_string(const char* text) {
    if (text && text == cached_managed_string_text &&
        cached_managed_string_allocation) {
        return cached_managed_string_allocation;
    }
    if (!text) return nullptr;
    const auto it =
        managed_allocations.find(reinterpret_cast<std::uintptr_t>(text));
    if (it == managed_allocations.end()) return nullptr;
    cached_managed_string_text = text;
    cached_managed_string_allocation = &it->second;
    return cached_managed_string_allocation;
}

void invalidate_managed_string_cache(const ManagedAllocation* allocation) {
    if (cached_managed_string_allocation != allocation) return;
    cached_managed_string_text = nullptr;
    cached_managed_string_allocation = nullptr;
}

void invalidate_shared_string_cache(const ManagedAllocation* allocation = nullptr) {
    if (allocation && cached_shared_string_allocation != allocation) return;
    cached_shared_string_text = nullptr;
    cached_shared_string_length = 0;
    cached_shared_string_allocation = nullptr;
}

// Interior references need an ordered range index, but exact owner operations do not.
// Generated element accesses cluster in a small working set of allocations, so cache
// those ranges and avoid a tree lookup on every initialization check. The set holds
// several entries because the common loops read one array while writing another; a
// single entry would miss on every access of a two-array algorithm such as a merge.
thread_local std::map<std::uintptr_t, std::size_t> managed_ranges;

struct ManagedRangeCacheEntry {
    ManagedAllocation* allocation{};
    std::uintptr_t begin{};
    std::size_t size{};
};

constexpr std::size_t managed_range_cache_slots = 4;
thread_local ManagedRangeCacheEntry managed_range_cache[managed_range_cache_slots];
thread_local std::size_t managed_range_cache_victim = 0;

void clear_managed_range_cache(const ManagedAllocation* allocation = nullptr) {
    for (auto& entry : managed_range_cache) {
        if (allocation && entry.allocation != allocation) continue;
        entry = ManagedRangeCacheEntry{};
    }
    if (!allocation) managed_range_cache_victim = 0;
}

void finalize_managed(ManagedFinalization finalization) {
    if (!finalization.base) return;
    if (finalization.drop) finalization.drop(finalization.base);
    recycle_managed_memory(
        finalization.base, finalization.small_pool_class);
}

ManagedAllocation* managed_containing(const void* address) {
    if (!address) return nullptr;
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    for (const auto& entry : managed_range_cache) {
        if (entry.allocation && target >= entry.begin && target - entry.begin < entry.size) {
            return entry.allocation;
        }
    }
    if (managed_ranges.empty()) return nullptr;
    auto range = managed_ranges.upper_bound(target);
    if (range == managed_ranges.begin()) return nullptr;
    --range;
    const auto begin = range->first;
    const auto size = range->second;
    if (target < begin || target - begin >= size) return nullptr;
    const auto it = managed_allocations.find(begin);
    if (it == managed_allocations.end()) return nullptr;
    auto& entry = managed_range_cache[managed_range_cache_victim];
    managed_range_cache_victim = (managed_range_cache_victim + 1) % managed_range_cache_slots;
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
    managed_allocations.emplace(key, std::move(allocation));

    // std::unordered_map rehash invalidates iterators, not references or pointers
    // to elements. The range cache stores pointers to mapped values, so inserting
    // another exact allocation does not require flushing the cache.
    //
    // Strings never expose interior references: indexing and slicing return new
    // values. Keep them out of the ordered range index entirely; exact ownership
    // still lives in managed_allocations.
    if (track_interior_range) managed_ranges.emplace(key, actual_bytes);
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
    std::fprintf(stderr,
                 "Quidra runtime error[UNINITIALIZED] at %llu:%llu: value is uninitialized\n",
                 line, column);
    std::exit(101);
}

[[noreturn]] void runtime_text_failure(const char* message) {
    std::fprintf(stderr, "Quidra runtime error: %s\n", message);
    std::exit(101);
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
        managed_allocations.find(reinterpret_cast<std::uintptr_t>(value));
    if (it == managed_allocations.end())
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

    if (text == cached_shared_string_text && cached_shared_string_allocation) {
        // The cached length belongs to this slice, but UTF-8/code-point metadata
        // in ManagedAllocation belongs to the backing slab as a whole. Keep
        // allocation null so validated_string_view never reuses one slice's
        // semantic metadata for another slice from the same slab.
        return std::string_view(text, cached_shared_string_length);
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
    if (text && text == cached_shared_string_text &&
        cached_shared_string_allocation) {
        allocation = nullptr;
        const auto source =
            std::string_view(text, cached_shared_string_length);
        if (codepoints) {
            if (cached_shared_string_allocation->string_ascii_known &&
                cached_shared_string_allocation->string_ascii) {
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

extern "C" char* quidra_format_float(double value) {
    if (std::isnan(value)) return runtime_copy_string("nan");
    if (std::isinf(value)) return runtime_copy_string(value < 0.0 ? "-inf" : "inf");
    if (value == 0.0) return runtime_copy_string(std::signbit(value) ? "-0.0" : "0.0");

    char buffer[64];
    const auto result =
        std::to_chars(std::begin(buffer), std::end(buffer), value, std::chars_format::general);
    if (result.ec != std::errc{}) runtime_text_failure("float formatting failed");

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
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value < 0.0 ? "-inf" : "inf";
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

std::string canonical_float_text(double value) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value < 0.0 ? "-inf" : "inf";
    if (value == 0.0) return std::signbit(value) ? "-0.0" : "0.0";
    char buffer[64];
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value, std::chars_format::general);
    if (result.ec != std::errc{}) runtime_text_failure("float formatting failed");
    std::string text(buffer, result.ptr);
    if (text.find('.') == std::string::npos) {
        const auto exponent = text.find_first_of("eE");
        if (exponent == std::string::npos) text += ".0";
        else text.insert(exponent, ".0");
    }
    return text;
}

} // namespace

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

extern "C" char* quidra_format_number(double value, int integer_width, int fractional, int significant, int zero) {
    std::string text;
    if (fractional >= 0) {
        if (std::isnan(value)) text = "nan";
        else if (std::isinf(value)) text = value < 0.0 ? "-inf" : "inf";
        else text = format_fixed(value, fractional);
    } else if (significant >= 0) {
        text = format_float_significant(value, significant);
    } else {
        text = canonical_float_text(value);
    }
    return runtime_copy_string(format_integer_padding(std::move(text), integer_width, zero != 0));
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
        managed_allocations.find(reinterpret_cast<std::uintptr_t>(data));
    if (it == managed_allocations.end() ||
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
        managed_allocations.find(reinterpret_cast<std::uintptr_t>(value));
    if (exact != managed_allocations.end()) {
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
        managed_allocations.find(reinterpret_cast<std::uintptr_t>(value));
    if (exact != managed_allocations.end()) {
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
            managed_ranges.erase(key);
        }
        managed_allocations.erase(key);
        finalize_managed(finalization);
    }
}

extern "C" void quidra_managed_pin(void* address) {
    if (!address) return;
    ManagedAllocation* allocation = nullptr;
    const auto exact =
        managed_allocations.find(reinterpret_cast<std::uintptr_t>(address));
    if (exact != managed_allocations.end()) allocation = &exact->second;
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
        managed_allocations.find(reinterpret_cast<std::uintptr_t>(address));
    if (exact != managed_allocations.end()) allocation = &exact->second;
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
            managed_ranges.erase(key);
        }
        managed_allocations.erase(key);
        finalize_managed(finalization);
    }
}

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
    const auto it = managed_allocations.find(reinterpret_cast<std::uintptr_t>(base));
    if (it == managed_allocations.end()) runtime_text_failure("initialization tracker requires managed storage");
    auto tracker = std::make_unique<InitializationTracker>();
    tracker->count = static_cast<std::size_t>(count);
    tracker->unit_bytes = static_cast<std::size_t>(unit_bytes);
    tracker->data_offset = static_cast<std::size_t>(data_offset);
    tracker->fully_initialized = fully_initialized != 0 || tracker->count == 0;
    tracker->initialized_count = tracker->fully_initialized ? tracker->count : 0;
    if (!tracker->fully_initialized) {
        tracker->bits.assign((tracker->count + 7) / 8, 0);
    }
    if (tracker->data_offset == 8) {
        const auto physical_capacity =
            it->second.size >= tracker->data_offset
                ? (it->second.size - tracker->data_offset) / tracker->unit_bytes
                : 0;
        it->second.array_capacity = std::max(tracker->count, physical_capacity);
    }
    it->second.initialization = std::move(tracker);
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
        managed_allocations.find(reinterpret_cast<std::uintptr_t>(destination));
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


struct AtomicCounterHandle {
    std::shared_ptr<std::atomic<long long>> state;
};

AtomicCounterHandle* atomic_counter_from_value(void* value) {
    if (!value) return nullptr;
    std::uintptr_t bits{};
    std::memcpy(&bits, value, sizeof(bits));
    return reinterpret_cast<AtomicCounterHandle*>(bits);
}

void* make_atomic_counter_value(AtomicCounterHandle* handle) {
    auto* value = managed_allocate(sizeof(std::uintptr_t));
    const auto bits = reinterpret_cast<std::uintptr_t>(handle);
    std::memcpy(value, &bits, sizeof(bits));
    return value;
}

extern "C" void* quidra_atomic_counter_create(long long initial) {
    try {
        auto state = std::make_shared<std::atomic<long long>>(initial);
        return make_atomic_counter_value(new AtomicCounterHandle{std::move(state)});
    } catch (...) {
        std::fprintf(stderr, "Quidra runtime error: allocation failed\n");
        std::exit(101);
    }
}

extern "C" void* quidra_atomic_counter_clone(void* value) {
    try {
        auto* source = atomic_counter_from_value(value);
        if (!source || !source->state) {
            std::fprintf(stderr, "Quidra runtime error: invalid atomic counter copy\n");
            std::exit(101);
        }
        return make_atomic_counter_value(new AtomicCounterHandle{source->state});
    } catch (...) {
        std::fprintf(stderr, "Quidra runtime error: allocation failed\n");
        std::exit(101);
    }
}

extern "C" void quidra_atomic_counter_drop(void* value) {
    if (!value) return;
    auto* handle = atomic_counter_from_value(value);
    std::uintptr_t zero{};
    std::memcpy(value, &zero, sizeof(zero));
    delete handle;
}

extern "C" long long quidra_atomic_counter_load(void* value) {
    auto* handle = atomic_counter_from_value(value);
    if (!handle || !handle->state) {
        std::fprintf(stderr, "Quidra runtime error: invalid atomic counter\n");
        std::exit(101);
    }
    return handle->state->load(std::memory_order_seq_cst);
}

extern "C" long long quidra_atomic_counter_add(
    void* value, long long delta,
    unsigned long long line, unsigned long long column) {
    auto* handle = atomic_counter_from_value(value);
    if (!handle || !handle->state) {
        std::fprintf(stderr, "Quidra runtime error: invalid atomic counter\n");
        std::exit(101);
    }
    auto current = handle->state->load(std::memory_order_seq_cst);
    while (true) {
        if ((delta > 0 && current > std::numeric_limits<long long>::max() - delta) ||
            (delta < 0 && current < std::numeric_limits<long long>::min() - delta)) {
            std::fprintf(stderr,
                "Quidra runtime error[INTEGER_OVERFLOW] at %llu:%llu: integer overflow\n",
                line, column);
            std::exit(101);
        }
        const auto next = static_cast<long long>(current + delta);
        if (handle->state->compare_exchange_weak(
                current, next, std::memory_order_seq_cst,
                std::memory_order_seq_cst)) {
            return next;
        }
    }
}

extern "C" void quidra_task_all_atomic_counter(
    void* raw, void* shared,
    unsigned long long line, unsigned long long column) {
    if (!raw || !shared) {
        std::fprintf(stderr,
            "Quidra runtime error[TASK_ARRAY] at %llu:%llu: task.all received invalid shared storage\n",
            line, column);
        std::exit(101);
    }
    long long signed_count = 0;
    std::memcpy(&signed_count, raw, sizeof(signed_count));
    if (signed_count < 0) {
        std::fprintf(stderr,
            "Quidra runtime error[TASK_ARRAY] at %llu:%llu: task.all received an invalid operation array\n",
            line, column);
        std::exit(101);
    }
    using TaskFunction = void (*)(void*);
    static_assert(sizeof(TaskFunction) == sizeof(void*));
    const auto count = static_cast<std::size_t>(signed_count);
    const auto* payload = static_cast<const unsigned char*>(raw) + 8;
    std::vector<TaskFunction> operations;
    std::vector<std::thread> threads;
    std::atomic<std::size_t> next{0};
    try {
        operations.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            TaskFunction operation = nullptr;
            std::memcpy(&operation,
                payload + index * sizeof(TaskFunction),
                sizeof(TaskFunction));
            if (!operation) {
                std::fprintf(stderr,
                    "Quidra runtime error[TASK_NULL] at %llu:%llu: task.all received a null operation\n",
                    line, column);
                std::exit(101);
            }
            operations.push_back(operation);
        }
        if (operations.empty()) return;
        const auto hardware = static_cast<std::size_t>(std::thread::hardware_concurrency());
        const auto practical_limit = std::min<std::size_t>(hardware == 0 ? 4 : hardware, 64);
        const auto worker_count = std::min<std::size_t>(operations.size(), practical_limit);
        threads.reserve(worker_count);
        for (std::size_t worker = 0; worker < worker_count; ++worker) {
            threads.emplace_back([&operations, &next, shared] {
                while (true) {
                    const auto index = next.fetch_add(1, std::memory_order_relaxed);
                    if (index >= operations.size()) return;
                    void* argument = quidra_atomic_counter_clone(shared);
                    operations[index](argument);
                    const auto task_argument_still_owned =
                        managed_allocations.find(reinterpret_cast<std::uintptr_t>(argument)) !=
                        managed_allocations.end();
                    if (task_argument_still_owned) {
                        quidra_managed_release(
                            argument,
                            reinterpret_cast<void*>(&quidra_atomic_counter_drop));
                    }
                }
            });
        }
    } catch (const std::exception& error) {
        for (auto& thread : threads) if (thread.joinable()) thread.join();
        std::fprintf(stderr,
            "Quidra runtime error[TASK_START] at %llu:%llu: cannot start task: %s\n",
            line, column, error.what());
        std::exit(101);
    }
    for (auto& thread : threads) thread.join();
}

extern "C" void quidra_task_all(
    void* raw, unsigned long long line, unsigned long long column) {
    if (!raw) {
        std::fprintf(
            stderr,
            "Quidra runtime error[TASK_ARRAY] at %llu:%llu: task.all received a null operation array\n",
            line, column);
        std::exit(101);
    }

    long long signed_count = 0;
    std::memcpy(&signed_count, raw, sizeof(signed_count));
    if (signed_count < 0) {
        std::fprintf(
            stderr,
            "Quidra runtime error[TASK_ARRAY] at %llu:%llu: task.all received an invalid operation array\n",
            line, column);
        std::exit(101);
    }

    using TaskFunction = void (*)();
    static_assert(sizeof(TaskFunction) == sizeof(void*));
    const auto count = static_cast<std::size_t>(signed_count);
    const auto* payload = static_cast<const unsigned char*>(raw) + 8;

    std::vector<TaskFunction> operations;
    std::vector<std::thread> threads;
    std::atomic<std::size_t> next{0};
    try {
        operations.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            TaskFunction operation = nullptr;
            std::memcpy(
                &operation, payload + index * sizeof(TaskFunction),
                sizeof(TaskFunction));
            if (!operation) {
                std::fprintf(
                    stderr,
                    "Quidra runtime error[TASK_NULL] at %llu:%llu: task.all received a null operation\n",
                    line, column);
                std::exit(101);
            }
            operations.push_back(operation);
        }
        if (operations.empty()) return;

        const auto hardware = static_cast<std::size_t>(std::thread::hardware_concurrency());
        const auto practical_limit = std::min<std::size_t>(hardware == 0 ? 4 : hardware, 64);
        const auto worker_count = std::min<std::size_t>(operations.size(), practical_limit);
        threads.reserve(worker_count);
        for (std::size_t worker = 0; worker < worker_count; ++worker) {
            threads.emplace_back([&operations, &next] {
                while (true) {
                    const auto index = next.fetch_add(1, std::memory_order_relaxed);
                    if (index >= operations.size()) return;
                    operations[index]();
                }
            });
        }
    } catch (const std::exception& error) {
        for (auto& thread : threads) if (thread.joinable()) thread.join();
        std::fprintf(
            stderr,
            "Quidra runtime error[TASK_START] at %llu:%llu: cannot start task: %s\n",
            line, column, error.what());
        std::exit(101);
    }

    for (auto& thread : threads) thread.join();
}

template <class Result>
void quidra_task_all_results(
    void* raw, void* output, unsigned long long line, unsigned long long column) {
    if (!raw || !output) {
        std::fprintf(stderr,
            "Quidra runtime error[TASK_ARRAY] at %llu:%llu: task.all received invalid storage\n",
            line, column);
        std::exit(101);
    }
    long long signed_count = 0;
    std::memcpy(&signed_count, raw, sizeof(signed_count));
    if (signed_count < 0) {
        std::fprintf(stderr,
            "Quidra runtime error[TASK_ARRAY] at %llu:%llu: task.all received an invalid operation array\n",
            line, column);
        std::exit(101);
    }
    using TaskFunction = Result (*)();
    static_assert(sizeof(TaskFunction) == sizeof(void*));
    const auto count = static_cast<std::size_t>(signed_count);
    const auto* operation_payload = static_cast<const unsigned char*>(raw) + 8;
    auto* result_payload = static_cast<unsigned char*>(output) + 8;
    std::vector<TaskFunction> operations;
    std::vector<std::thread> threads;
    std::atomic<std::size_t> next{0};
    try {
        operations.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            TaskFunction operation = nullptr;
            std::memcpy(&operation,
                operation_payload + index * sizeof(TaskFunction),
                sizeof(TaskFunction));
            if (!operation) {
                std::fprintf(stderr,
                    "Quidra runtime error[TASK_NULL] at %llu:%llu: task.all received a null operation\n",
                    line, column);
                std::exit(101);
            }
            operations.push_back(operation);
        }
        if (operations.empty()) return;
        const auto hardware = static_cast<std::size_t>(std::thread::hardware_concurrency());
        const auto practical_limit = std::min<std::size_t>(hardware == 0 ? 4 : hardware, 64);
        const auto worker_count = std::min<std::size_t>(operations.size(), practical_limit);
        threads.reserve(worker_count);
        for (std::size_t worker = 0; worker < worker_count; ++worker) {
            threads.emplace_back([&operations, &next, result_payload] {
                while (true) {
                    const auto index = next.fetch_add(1, std::memory_order_relaxed);
                    if (index >= operations.size()) return;
                    const Result result = operations[index]();
                    std::memcpy(result_payload + index * sizeof(Result),
                                &result, sizeof(Result));
                }
            });
        }
    } catch (const std::exception& error) {
        for (auto& thread : threads) if (thread.joinable()) thread.join();
        std::fprintf(stderr,
            "Quidra runtime error[TASK_START] at %llu:%llu: cannot start task: %s\n",
            line, column, error.what());
        std::exit(101);
    }
    for (auto& thread : threads) thread.join();
}

extern "C" void quidra_task_all_i64(
    void* raw, void* output, unsigned long long line, unsigned long long column) {
    quidra_task_all_results<long long>(raw, output, line, column);
}
extern "C" void quidra_task_all_f64(
    void* raw, void* output, unsigned long long line, unsigned long long column) {
    quidra_task_all_results<double>(raw, output, line, column);
}

extern "C" bool quidra_array_can_append_move(void* array) {
    auto* allocation = exact_array_append_allocation(array);
    if (!allocation || allocation->owners != 1 || allocation->pins != 0 ||
        !allocation->initialization) {
        return false;
    }
    const auto& tracker = *allocation->initialization;
    return tracker.data_offset == 8 && tracker.unit_bytes != 0 &&
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
    if (tracker->data_offset != 8 || tracker->unit_bytes == 0 ||
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
            (std::numeric_limits<std::size_t>::max() - 8) / stride) {
            runtime_allocation_failure();
        }

        const auto old_key = reinterpret_cast<std::uintptr_t>(array);
        const auto new_bytes = static_cast<std::size_t>(8) + new_capacity * stride;
        const auto old_bytes = allocation->size;
        clear_managed_range_cache(allocation);
        result = std::realloc(array, new_bytes);
        if (!result) runtime_allocation_failure();
        if (new_bytes > old_bytes) {
            std::memset(static_cast<unsigned char*>(result) + old_bytes, 0,
                        new_bytes - old_bytes);
        }

        const auto new_key = reinterpret_cast<std::uintptr_t>(result);
        managed_ranges.erase(old_key);
        if (new_key != old_key) {
            auto node = managed_allocations.extract(old_key);
            if (node.empty())
                runtime_text_failure("array append storage disappeared");
            node.key() = new_key;
            node.mapped().base = result;
            node.mapped().size = new_bytes;
            node.mapped().small_pool_class = 0;
            node.mapped().array_capacity = new_capacity;
            const auto inserted = managed_allocations.insert(std::move(node));
            allocation = &inserted.position->second;
        } else {
            allocation->base = result;
            allocation->size = new_bytes;
            allocation->small_pool_class = 0;
            allocation->array_capacity = new_capacity;
        }
        managed_ranges.emplace(new_key, new_bytes);
        cached_array_append_base = result;
        cached_array_append_allocation = allocation;
        tracker = allocation->initialization.get();
    }

    // The newly exposed append slot was outside the old logical length.
    // Small managed allocations can have spare pooled capacity whose bytes retain
    // data from a previous allocation. ArraySet releases a managed value already
    // present in a slot before replacing it, so make the fresh logical slot
    // explicitly empty before publishing the longer array. Without this, a stale
    // pointer in spare capacity can be released as if it were an existing element.
    std::memset(
        static_cast<unsigned char*>(result) + 8 + old_count * stride,
        0, stride);

    tracker->count = new_count;
    tracker->initialized_count = new_count;
    tracker->fully_initialized = true;
    tracker->bits.clear();

    const auto new_length = static_cast<long long>(new_count);
    std::memcpy(result, &new_length, sizeof(new_length));
    return result;
}


extern "C" void* quidra_array_sorted(void* raw, int kind,
                                      unsigned long long raw_stride,
                                      unsigned long long line,
                                      unsigned long long column) {
    if (!raw || raw_stride == 0 ||
        raw_stride > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        runtime_text_failure("invalid sorted array input");
    }

    long long signed_count = 0;
    std::memcpy(&signed_count, raw, sizeof(signed_count));
    if (signed_count < 0) runtime_text_failure("invalid array length during sort");
    const auto count = static_cast<std::size_t>(signed_count);
    const auto stride = static_cast<std::size_t>(raw_stride);
    if (count > (std::numeric_limits<std::size_t>::max() - 8) / stride) {
        runtime_allocation_failure();
    }

    const auto expected_stride = [&]() -> std::size_t {
        switch (kind) {
            case 1: return sizeof(std::int64_t);
            case 2: return sizeof(std::int8_t);
            case 3: return sizeof(std::int16_t);
            case 4: return sizeof(std::int32_t);
            case 5: return sizeof(std::uint8_t);
            case 6: return sizeof(std::uint16_t);
            case 7: return sizeof(std::uint32_t);
            case 8: return sizeof(std::uint64_t);
            case 9: return sizeof(double);
            case 10: return sizeof(float);
            case 11: return 1;
            case 12: return sizeof(char*);
            default: runtime_text_failure("unsupported sorted array element type");
        }
    }();
    if (stride != expected_stride) runtime_text_failure("sorted array stride mismatch");

    if (count != 0) {
        quidra_init_require_range(
            static_cast<unsigned char*>(raw) + 8,
            static_cast<unsigned long long>(count * stride), line, column);
    }

    auto* result = static_cast<unsigned char*>(managed_allocate(8 + count * stride));
    std::memcpy(result, &signed_count, sizeof(signed_count));
    if (count != 0) {
        std::memcpy(result + 8, static_cast<unsigned char*>(raw) + 8, count * stride);
    }
    quidra_init_create(result, static_cast<unsigned long long>(count),
                       static_cast<unsigned long long>(stride), 8, 1);

    auto* data = result + 8;
    const auto float_less = [](auto left, auto right) {
        const bool left_nan = std::isnan(left);
        const bool right_nan = std::isnan(right);
        if (left_nan || right_nan) return !left_nan && right_nan;
        if (left == right) {
            if (left == 0) return std::signbit(left) && !std::signbit(right);
            return false;
        }
        return left < right;
    };

    switch (kind) {
        case 1:
            std::stable_sort(reinterpret_cast<std::int64_t*>(data),
                             reinterpret_cast<std::int64_t*>(data) + count);
            break;
        case 2:
            std::stable_sort(reinterpret_cast<std::int8_t*>(data),
                             reinterpret_cast<std::int8_t*>(data) + count);
            break;
        case 3:
            std::stable_sort(reinterpret_cast<std::int16_t*>(data),
                             reinterpret_cast<std::int16_t*>(data) + count);
            break;
        case 4:
            std::stable_sort(reinterpret_cast<std::int32_t*>(data),
                             reinterpret_cast<std::int32_t*>(data) + count);
            break;
        case 5:
            std::stable_sort(reinterpret_cast<std::uint8_t*>(data),
                             reinterpret_cast<std::uint8_t*>(data) + count);
            break;
        case 6:
            std::stable_sort(reinterpret_cast<std::uint16_t*>(data),
                             reinterpret_cast<std::uint16_t*>(data) + count);
            break;
        case 7:
            std::stable_sort(reinterpret_cast<std::uint32_t*>(data),
                             reinterpret_cast<std::uint32_t*>(data) + count);
            break;
        case 8:
            std::stable_sort(reinterpret_cast<std::uint64_t*>(data),
                             reinterpret_cast<std::uint64_t*>(data) + count);
            break;
        case 9:
            std::stable_sort(reinterpret_cast<double*>(data),
                             reinterpret_cast<double*>(data) + count, float_less);
            break;
        case 10:
            std::stable_sort(reinterpret_cast<float*>(data),
                             reinterpret_cast<float*>(data) + count, float_less);
            break;
        case 11:
            std::stable_sort(reinterpret_cast<std::uint8_t*>(data),
                             reinterpret_cast<std::uint8_t*>(data) + count);
            break;
        case 12: {
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
                strings.push_back(SortableString{begin[i], view});
            }
            std::stable_sort(strings.begin(), strings.end(),
                             [](const SortableString& left,
                                const SortableString& right) {
                                 return left.view < right.view;
                             });
            for (std::size_t i = 0; i < count; ++i) begin[i] = strings[i].value;
            break;
        }
        default:
            runtime_text_failure("unsupported sorted array element type");
    }
    return result;
}


namespace {
[[noreturn]] void numeric_round_failure(unsigned long long line, unsigned long long column) {
    std::fprintf(stderr, "Quidra runtime error[NUMERIC_CONVERSION] at %llu:%llu: rounded value is outside int range or is not finite\n", line, column);
    std::exit(101);
}
long long checked_rounded_int(double value, double rounded, unsigned long long line, unsigned long long column) {
    if (!std::isfinite(value) || !std::isfinite(rounded)) numeric_round_failure(line, column);
    const long double result = static_cast<long double>(rounded);
    if (result < static_cast<long double>(std::numeric_limits<long long>::min()) ||
        result > static_cast<long double>(std::numeric_limits<long long>::max())) numeric_round_failure(line, column);
    return static_cast<long long>(rounded);
}
}
extern "C" long long quidra_math_trunc_int(double value, unsigned long long line, unsigned long long column) { return checked_rounded_int(value, std::trunc(value), line, column); }
extern "C" long long quidra_math_round_int(double value, unsigned long long line, unsigned long long column) { return checked_rounded_int(value, std::round(value), line, column); }
extern "C" long long quidra_math_floor_int(double value, unsigned long long line, unsigned long long column) { return checked_rounded_int(value, std::floor(value), line, column); }
extern "C" long long quidra_math_ceil_int(double value, unsigned long long line, unsigned long long column) { return checked_rounded_int(value, std::ceil(value), line, column); }
extern "C" bool quidra_math_is_finite(double value) { return std::isfinite(value); }

namespace {

struct AutogradNode;
struct AutogradSlot;
struct AutogradIdentity;
std::shared_ptr<AutogradSlot> clone_autograd_slot(
    const std::shared_ptr<AutogradSlot>& source);

enum class AutogradOp {
    Leaf, Add, Sub, Mul, Div, ScalarBinary,
    Absolute, Exponential, Logarithm, Mean, SumLast, MaxLast, MinLast,
    Reshape, Transpose, Matmul, Gather, GatherBackward, MeanBackward, SumLastBackward
};

struct TensorStorage {
    std::size_t owners{1};
    int dtype{};
    std::size_t count{};
    int device{-1}; // -1 is an internal CPU representation; public gpu indices are >= 0.
    std::vector<unsigned char> data;
    quidra::device::Buffer* gpu_buffer{};
    InitializationTracker initialization;
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
    std::fprintf(stderr, "Quidra runtime error[TENSOR] at %llu:%llu: %s\n",
                 line, column, message);
    std::exit(101);
}

std::size_t tensor_dtype_bytes(int dtype) {
    switch (dtype) {
        case 1: case 8: case 9: return 8;
        case 2: case 5: case 11: return 1;
        case 3: case 6: return 2;
        case 4: case 7: case 10: return 4;
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
    auto* data = static_cast<unsigned char*>(raw) + 8;
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
    if (tensor.shape.empty()) return tensor.offset;
    std::size_t index = tensor.offset;
    for (std::size_t axis = tensor.shape.size(); axis-- > 0;) {
        const auto dimension = static_cast<std::size_t>(tensor.shape[axis]);
        const auto coordinate = dimension == 0 ? 0 : logical % dimension;
        if (dimension != 0) logical /= dimension;
        index += coordinate * static_cast<std::size_t>(tensor.strides[axis]);
    }
    return index;
}

TensorValue* tensor_descriptor(TensorStorage* storage, std::vector<long long> shape,
                               std::vector<long long> strides, std::size_t offset) {
    auto* memory = managed_allocate(sizeof(TensorValue));
    return new (memory) TensorValue{
        storage, std::move(shape), std::move(strides), offset, {}, {}};
}

void tensor_storage_release(TensorStorage* storage) {
    if (!storage) return;
    if (storage->owners == 0) runtime_text_failure("tensor storage owner underflow");
    --storage->owners;
    if (storage->owners == 0) {
        quidra::device::release(storage->gpu_buffer);
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
    if (!tensor_on_cpu(storage)) {
        tensor_gpu_unsupported(operation, storage, line, column);
    }
}

TensorStorage* tensor_storage_create(
    int dtype, std::size_t count, int fill_mode, int device_index = -1,
    unsigned long long line = 0, unsigned long long column = 0) {
    const auto width = tensor_dtype_bytes(dtype);
    if (count != 0 && width > std::numeric_limits<std::size_t>::max() / count) {
        runtime_allocation_failure();
    }
    if (device_index < -1) {
        tensor_fail("invalid internal tensor device", line, column);
    }
    auto* storage = new (std::nothrow) TensorStorage;
    if (!storage) runtime_allocation_failure();
    storage->dtype = dtype;
    storage->count = count;
    storage->device = device_index;
    const auto bytes = count * width;
    try {
        storage->initialization.count = count;
        storage->initialization.unit_bytes = width;
        storage->initialization.fully_initialized = fill_mode != 0 || count == 0;
        storage->initialization.initialized_count =
            storage->initialization.fully_initialized ? count : 0;
        if (!storage->initialization.fully_initialized) {
            storage->initialization.bits.assign((count + 7) / 8, 0);
        }

        if (device_index < 0) {
            storage->data.resize(bytes);
        }
    } catch (...) {
        delete storage;
        runtime_allocation_failure();
    }

    auto fill_ones = [&](std::vector<unsigned char>& target) {
        target.resize(bytes);
        for (std::size_t i = 0; i < count; ++i) {
            auto* slot = target.data() + i * width;
            switch (dtype) {
                case 1: { std::int64_t v=1; std::memcpy(slot,&v,8); break; }
                case 2: { std::int8_t v=1; std::memcpy(slot,&v,1); break; }
                case 3: { std::int16_t v=1; std::memcpy(slot,&v,2); break; }
                case 4: { std::int32_t v=1; std::memcpy(slot,&v,4); break; }
                case 5: { std::uint8_t v=1; std::memcpy(slot,&v,1); break; }
                case 6: { std::uint16_t v=1; std::memcpy(slot,&v,2); break; }
                case 7: { std::uint32_t v=1; std::memcpy(slot,&v,4); break; }
                case 8: { std::uint64_t v=1; std::memcpy(slot,&v,8); break; }
                case 9: { double v=1.0; std::memcpy(slot,&v,8); break; }
                case 10:{ float v=1.0F; std::memcpy(slot,&v,4); break; }
                case 11:{ std::uint8_t v=1; std::memcpy(slot,&v,1); break; }
                default: tensor_fail("invalid tensor dtype", line, column);
            }
        }
    };

    if (device_index < 0) {
        if (fill_mode == 1) {
            std::fill(storage->data.begin(), storage->data.end(), 0);
        } else if (fill_mode == 2) {
            try {
                fill_ones(storage->data);
            } catch (...) {
                delete storage;
                runtime_allocation_failure();
            }
        }
        return storage;
    }

    std::string backend_error;
    storage->gpu_buffer = quidra::device::allocate(device_index, bytes, backend_error);
    if (!storage->gpu_buffer) {
        delete storage;
        tensor_fail(backend_error.c_str(), line, column);
    }
    if (fill_mode == 1) {
        if (!quidra::device::zero(storage->gpu_buffer, 0, bytes, backend_error)) {
            quidra::device::release(storage->gpu_buffer);
            storage->gpu_buffer = nullptr;
            delete storage;
            tensor_fail(backend_error.c_str(), line, column);
        }
    } else if (fill_mode == 2) {
        if (!quidra::device::compute_fill_ones(
                storage->gpu_buffer, dtype, count, backend_error)) {
            quidra::device::release(storage->gpu_buffer);
            storage->gpu_buffer = nullptr;
            delete storage;
            tensor_fail(backend_error.c_str(), line, column);
        }
    }
    return storage;
}

void tensor_require_initialized(const TensorValue& tensor,
                                unsigned long long line,
                                unsigned long long column) {
    const auto count = tensor_logical_count(tensor);
    for (std::size_t i = 0; i < count; ++i) {
        const auto storage_index = tensor_storage_index(tensor, i);
        if (storage_index >= tensor.storage->count ||
            !tracker_bit(tensor.storage->initialization, storage_index)) {
            runtime_uninitialized_failure(line, column);
        }
    }
}

TensorStorage* tensor_gpu_materialize_storage(
    const TensorValue& source,
    unsigned long long line,
    unsigned long long column) {
    if (tensor_on_cpu(*source.storage)) {
        tensor_fail("internal GPU materialization received a CPU tensor", line, column);
    }
    const auto count = tensor_logical_count(source);
    auto* output = tensor_storage_create(
        source.storage->dtype, count, 0, source.storage->device, line, column);
    std::vector<std::uint64_t> indices;
    try {
        indices.resize(count);
    } catch (...) {
        tensor_storage_release(output);
        runtime_allocation_failure();
    }
    for (std::size_t logical = 0; logical < count; ++logical) {
        const auto source_index = tensor_storage_index(source, logical);
        if (source_index >= source.storage->count) {
            tensor_storage_release(output);
            tensor_fail("tensor view exceeds storage", line, column);
        }
        indices[logical] = static_cast<std::uint64_t>(source_index);
        if (tracker_bit(source.storage->initialization, source_index)) {
            tracker_set(output->initialization, logical);
        }
    }
    std::string backend_error;
    if (!quidra::device::compute_gather(
            output->gpu_buffer, source.storage->gpu_buffer,
            source.storage->dtype, indices.data(), count, backend_error)) {
        tensor_storage_release(output);
        tensor_fail(backend_error.c_str(), line, column);
    }
    return output;
}

TensorStorage* tensor_transfer_storage(
    const TensorValue& source, int target_device,
    unsigned long long line, unsigned long long column) {
    const auto count = tensor_logical_count(source);
    auto* output = tensor_storage_create(
        source.storage->dtype, count, 0, target_device, line, column);
    const auto width = tensor_dtype_bytes(source.storage->dtype);
    if (count != 0 && width > std::numeric_limits<std::size_t>::max() / count) {
        tensor_storage_release(output);
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
        const TensorStorage* dense_gpu_source = nullptr;
        std::vector<unsigned char> host_materialized;
        const unsigned char* dense_host_source = nullptr;

        if (tensor_on_cpu(*source.storage)) {
            if (tensor_is_contiguous_value(source)) {
                if (source.offset > source.storage->count ||
                    count > source.storage->count - source.offset) {
                    tensor_storage_release(output);
                    tensor_fail("tensor view exceeds storage", line, column);
                }
                dense_host_source =
                    source.storage->data.data() + source.offset * width;
            } else {
                try {
                    host_materialized.resize(bytes);
                } catch (...) {
                    tensor_storage_release(output);
                    runtime_allocation_failure();
                }
                for (std::size_t logical = 0; logical < count; ++logical) {
                    const auto source_index = tensor_storage_index(source, logical);
                    if (source_index >= source.storage->count) {
                        tensor_storage_release(output);
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
                    tensor_storage_release(output);
                    tensor_fail("tensor view exceeds storage", line, column);
                }
                dense_gpu_source = source.storage;
            } else {
                gpu_materialized =
                    tensor_gpu_materialize_storage(source, line, column);
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
                    if (gpu_materialized) tensor_storage_release(gpu_materialized);
                    tensor_storage_release(output);
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

        if (gpu_materialized) tensor_storage_release(gpu_materialized);
        if (!ok) {
            tensor_storage_release(output);
            tensor_fail(backend_error.c_str(), line, column);
        }
        mark_fully_initialized();
        return output;
    }

    // Partially initialized tensors preserve initialization at element
    // granularity. This uncommon path intentionally copies only initialized
    // elements so uninitialized storage is never exposed as initialized data.
    for (std::size_t logical = 0; logical < count; ++logical) {
        const auto source_index = tensor_storage_index(source, logical);
        if (source_index >= source.storage->count) {
            tensor_storage_release(output);
            tensor_fail("tensor view exceeds storage", line, column);
        }
        if (!tracker_bit(source.storage->initialization, source_index)) continue;

        if (tensor_on_cpu(*source.storage)) {
            std::memcpy(element.data(),
                        source.storage->data.data() + source_index * width, width);
        } else if (!quidra::device::copy_to_host(
                       source.storage->gpu_buffer, source_index * width,
                       element.data(), width, backend_error)) {
            tensor_storage_release(output);
            tensor_fail(backend_error.c_str(), line, column);
        }

        if (tensor_on_cpu(*output)) {
            std::memcpy(output->data.data() + logical * width,
                        element.data(), width);
        } else if (!quidra::device::copy_from_host(
                       output->gpu_buffer, logical * width,
                       element.data(), width, backend_error)) {
            tensor_storage_release(output);
            tensor_fail(backend_error.c_str(), line, column);
        }
        tracker_set(output->initialization, logical);
    }
    return output;
}

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
    if constexpr (std::is_integral_v<Src> && std::is_integral_v<Dst>) {
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
        if (std::isfinite(source) && !std::isfinite(destination)) return false;
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
        case 1: return tensor_cast_buffer<Src,std::int64_t>(tensor,output);
        case 2: return tensor_cast_buffer<Src,std::int8_t>(tensor,output);
        case 3: return tensor_cast_buffer<Src,std::int16_t>(tensor,output);
        case 4: return tensor_cast_buffer<Src,std::int32_t>(tensor,output);
        case 5: return tensor_cast_buffer<Src,std::uint8_t>(tensor,output);
        case 6: return tensor_cast_buffer<Src,std::uint16_t>(tensor,output);
        case 7: return tensor_cast_buffer<Src,std::uint32_t>(tensor,output);
        case 8: return tensor_cast_buffer<Src,std::uint64_t>(tensor,output);
        case 9: return tensor_cast_buffer<Src,double>(tensor,output);
        case 10:return tensor_cast_buffer<Src,float>(tensor,output);
        default: return false;
    }
}

} // namespace

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
        case 1: return write(std::int64_t{});
        case 2: return write(std::int8_t{});
        case 3: return write(std::int16_t{});
        case 4: return write(std::int32_t{});
        case 5: return write(std::uint8_t{});
        case 6: return write(std::uint16_t{});
        case 7: return write(std::uint32_t{});
        case 8: return write(std::uint64_t{});
        case 9: return write(double{});
        case 10:return write(float{});
        default:return false;
    }
}

extern "C" void quidra_numeric_cast_element(
    void* destination, const void* source, int source_dtype, int target_dtype,
    unsigned long long line, unsigned long long column) {
    if (!destination || !source) runtime_text_failure("null numeric cast storage");
    quidra_init_check(const_cast<void*>(source), line, column);
    bool ok=false;
    switch (source_dtype) {
        case 1: ok=numeric_cast_element_from<std::int64_t>(source,destination,target_dtype); break;
        case 2: ok=numeric_cast_element_from<std::int8_t>(source,destination,target_dtype); break;
        case 3: ok=numeric_cast_element_from<std::int16_t>(source,destination,target_dtype); break;
        case 4: ok=numeric_cast_element_from<std::int32_t>(source,destination,target_dtype); break;
        case 5: ok=numeric_cast_element_from<std::uint8_t>(source,destination,target_dtype); break;
        case 6: ok=numeric_cast_element_from<std::uint16_t>(source,destination,target_dtype); break;
        case 7: ok=numeric_cast_element_from<std::uint32_t>(source,destination,target_dtype); break;
        case 8: ok=numeric_cast_element_from<std::uint64_t>(source,destination,target_dtype); break;
        case 9: ok=numeric_cast_element_from<double>(source,destination,target_dtype); break;
        case 10:ok=numeric_cast_element_from<float>(source,destination,target_dtype); break;
        default: break;
    }
    if (!ok) {
        std::fprintf(stderr,
            "Quidra runtime error[NUMERIC_CAST_RANGE] at %llu:%llu: numeric cast outside destination range\n",
            line,column);
        std::exit(101);
    }
}

extern "C" void* quidra_tensor_create(
    void* shape_array, int dtype, int fill_mode, bool has_gpu, long long gpu,
    unsigned long long line, unsigned long long column) {
    if (fill_mode < 0 || fill_mode > 2) {
        tensor_fail("invalid tensor fill mode", line, column);
    }
    int device_index = -1;
    if (has_gpu) {
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
    return tensor_descriptor(
        storage, std::move(shape), std::move(strides), 0);
}

extern "C" void* quidra_tensor_to_gpu(
    void* raw, long long gpu,
    unsigned long long line, unsigned long long column) {
    if (!raw) tensor_fail("null tensor", line, column);
    if (gpu < 0 || gpu > std::numeric_limits<int>::max()) {
        tensor_fail("gpu index must be a non-negative supported index", line, column);
    }
    auto* source = static_cast<TensorValue*>(raw);
    tensor_require_untracked_transform(*source,"gpu()",line,column);
    auto* storage = tensor_transfer_storage(
        *source, static_cast<int>(gpu), line, column);
    return tensor_descriptor(
        storage, source->shape, tensor_contiguous_strides(source->shape), 0);
}

extern "C" void* quidra_tensor_to_cpu(
    void* raw, unsigned long long line, unsigned long long column) {
    if (!raw) tensor_fail("null tensor", line, column);
    auto* source = static_cast<TensorValue*>(raw);
    tensor_require_untracked_transform(*source,"cpu()",line,column);
    auto* storage = tensor_transfer_storage(*source, -1, line, column);
    return tensor_descriptor(
        storage, source->shape, tensor_contiguous_strides(source->shape), 0);
}

extern "C" void* quidra_tensor_clone(void* raw) {
    if (!raw) return nullptr;
    auto* source = static_cast<TensorValue*>(raw);
    if (!source->storage ||
        source->storage->owners == std::numeric_limits<std::size_t>::max()) {
        runtime_text_failure("invalid tensor storage");
    }
    ++source->storage->owners;
    auto* result=tensor_descriptor(
        source->storage, source->shape, source->strides, source->offset);
    result->grad_slot=clone_autograd_slot(source->grad_slot);
    if(source->graph) result->graph=source->graph;
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
} // namespace

extern "C" bool quidra_tensor_is_contiguous(void* raw) {
    if (!raw) runtime_text_failure("null tensor");
    return tensor_is_contiguous_value(*static_cast<TensorValue*>(raw));
}

extern "C" void* quidra_tensor_shape(void* raw) {
    if (!raw) runtime_text_failure("null tensor");
    const auto& shape = static_cast<TensorValue*>(raw)->shape;
    if (shape.size() > (std::numeric_limits<std::size_t>::max() - 8) / sizeof(long long)) {
        runtime_allocation_failure();
    }
    const auto bytes = 8 + shape.size() * sizeof(long long);
    auto* result = static_cast<unsigned char*>(managed_allocate(bytes));
    const auto rank = static_cast<long long>(shape.size());
    std::memcpy(result, &rank, sizeof(rank));
    for (std::size_t i = 0; i < shape.size(); ++i) {
        std::memcpy(result + 8 + i * sizeof(long long), &shape[i], sizeof(long long));
    }
    const auto it = managed_allocations.find(reinterpret_cast<std::uintptr_t>(result));
    auto tracker = std::make_unique<InitializationTracker>();
    tracker->count = shape.size();
    tracker->unit_bytes = sizeof(long long);
    tracker->data_offset = 8;
    tracker->initialized_count = shape.size();
    tracker->fully_initialized = true;
    it->second.initialization = std::move(tracker);
    return result;
}

extern "C" void* quidra_tensor_shape_fixed(void* raw, unsigned long long expected_rank) {
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
    const auto it = managed_allocations.find(reinterpret_cast<std::uintptr_t>(result));
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

} // namespace

extern "C" void* quidra_tensor_reshape(void* raw, void* shape_array,
                                         unsigned long long line,
                                         unsigned long long column) {
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
    auto strides = tensor_contiguous_strides(shape);
    auto* result=tensor_descriptor(
        source->storage, std::move(shape), std::move(strides), source->offset);
    tensor_attach_view_graph(
        result,*source,AutogradOp::Reshape,{},line,column);
    return result;
}

extern "C" void* quidra_tensor_transpose(
    void* raw, long long axis0, long long axis1,
    unsigned long long line, unsigned long long column) {
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
    auto shape = source->shape;
    auto strides = source->strides;
    const auto a0 = static_cast<std::size_t>(axis0);
    const auto a1 = static_cast<std::size_t>(axis1);
    std::swap(shape[a0], shape[a1]);
    std::swap(strides[a0], strides[a1]);
    auto* result=tensor_descriptor(
        source->storage, std::move(shape), std::move(strides), source->offset);
    tensor_attach_view_graph(
        result,*source,AutogradOp::Transpose,{a0,a1},line,column);
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
    if (!tensor_on_cpu(*source->storage)) {
        storage = tensor_gpu_materialize_storage(*source, 0, 0);
    } else {
        storage = tensor_storage_create(source->storage->dtype, count, 0);
        const auto width = tensor_dtype_bytes(source->storage->dtype);
        for (std::size_t i = 0; i < count; ++i) {
            const auto source_index = tensor_storage_index(*source, i);
            std::memcpy(storage->data.data() + i * width,
                        source->storage->data.data() + source_index * width, width);
            if (tracker_bit(source->storage->initialization, source_index)) {
                tracker_set(storage->initialization, i);
            }
        }
    }
    return tensor_descriptor(storage, source->shape,
                             tensor_contiguous_strides(source->shape), 0);
}

extern "C" void* quidra_tensor_item_ptr(void* raw, unsigned long long line,
                                         unsigned long long column) {
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

extern "C" void* quidra_tensor_cast(void* raw, int target_dtype,
                                      unsigned long long line,
                                      unsigned long long column) {
    if (!raw) tensor_fail("null tensor", line, column);
    auto* source = static_cast<TensorValue*>(raw);
    tensor_require_initialized(*source, line, column);
    if (source->graph) {
        tensor_fail(
            "tracked tensor cannot be cast; call untrack() explicitly before changing dtype",
            line, column);
    }
    const auto count = tensor_logical_count(*source);
    if (!tensor_on_cpu(*source->storage)) {
        TensorStorage* materialized = nullptr;
        const TensorStorage* input_storage = source->storage;
        std::size_t input_offset = source->offset * tensor_dtype_bytes(source->storage->dtype);
        if (!tensor_is_contiguous_value(*source)) {
            materialized = tensor_gpu_materialize_storage(*source, line, column);
            input_storage = materialized;
            input_offset = 0;
        }
        auto* output = tensor_storage_create(
            target_dtype, count, 1, source->storage->device, line, column);
        std::string backend_error;
        const bool ok = quidra::device::compute_cast(
            output->gpu_buffer, input_storage->gpu_buffer, input_offset,
            source->storage->dtype, target_dtype, count, backend_error);
        if (materialized) tensor_storage_release(materialized);
        if (!ok) {
            tensor_storage_release(output);
            tensor_fail(backend_error.c_str(), line, column);
        }
        auto* result=tensor_descriptor(
            output,source->shape,tensor_contiguous_strides(source->shape),0);
        return result;
    }
    auto* output = tensor_storage_create(target_dtype, count, 1);
    bool exact = false;
    switch (source->storage->dtype) {
        case 1: exact=tensor_cast_from<std::int64_t>(*source,target_dtype,*output); break;
        case 2: exact=tensor_cast_from<std::int8_t>(*source,target_dtype,*output); break;
        case 3: exact=tensor_cast_from<std::int16_t>(*source,target_dtype,*output); break;
        case 4: exact=tensor_cast_from<std::int32_t>(*source,target_dtype,*output); break;
        case 5: exact=tensor_cast_from<std::uint8_t>(*source,target_dtype,*output); break;
        case 6: exact=tensor_cast_from<std::uint16_t>(*source,target_dtype,*output); break;
        case 7: exact=tensor_cast_from<std::uint32_t>(*source,target_dtype,*output); break;
        case 8: exact=tensor_cast_from<std::uint64_t>(*source,target_dtype,*output); break;
        case 9: exact=tensor_cast_from<double>(*source,target_dtype,*output); break;
        case 10:exact=tensor_cast_from<float>(*source,target_dtype,*output); break;
        default: break;
    }
    if (!exact) {
        tensor_storage_release(output);
        tensor_fail("tensor cast is unsupported or a value is outside the target range", line, column);
    }
    auto* result=tensor_descriptor(
        output,source->shape,tensor_contiguous_strides(source->shape),0);
    return result;
}

extern "C" void quidra_tensor_rank_check(
    void* raw, long long expected_rank,
    unsigned long long line, unsigned long long column) {
    if(!raw) tensor_fail("null tensor",line,column);
    const auto* tensor=static_cast<TensorValue*>(raw);
    if(expected_rank<0 ||
       tensor->shape.size()!=static_cast<std::size_t>(expected_rank))
        tensor_fail("tensor rank does not satisfy captured shape constraint",line,column);
}

extern "C" void quidra_tensor_extent_check(
    void* raw, long long axis, long long expected,
    unsigned long long line, unsigned long long column) {
    if(!raw) tensor_fail("null tensor",line,column);
    const auto* tensor=static_cast<TensorValue*>(raw);
    if(axis<0 || static_cast<std::size_t>(axis)>=tensor->shape.size())
        tensor_fail("tensor shape axis is outside rank",line,column);
    if(expected<0)
        tensor_fail("captured tensor extent cannot be negative",line,column);
    if(tensor->shape[static_cast<std::size_t>(axis)]!=expected)
        tensor_fail("tensor extent does not satisfy captured shape constraint",line,column);
}


namespace {

void tensor_detach_for_write(TensorValue& tensor, unsigned long long line, unsigned long long column);
TensorStorage* tensor_gpu_materialize_storage(
    const TensorValue& source,
    unsigned long long line,
    unsigned long long column);

extern "C" void* quidra_tensor_autograd_unary(
    void* raw,int op,unsigned long long line,unsigned long long column);
extern "C" void* quidra_tensor_unary(
    void* raw,int operation,unsigned long long line,unsigned long long column);
extern "C" void* quidra_tensor_binary(
    void* primary_raw,void* other_raw,void* scalar,int scalar_side,
    int operation,unsigned long long line,unsigned long long column);

extern "C" void* quidra_linear_matmul(
    void* left_raw,void* right_raw,
    unsigned long long line,unsigned long long column);


class AutogradBuffer {
public:
    explicit AutogradBuffer(int dtype=10) { set_dtype(dtype); }
    explicit AutogradBuffer(std::vector<float> values) : values_(std::move(values)) {}
    explicit AutogradBuffer(std::vector<double> values) : values_(std::move(values)) {}

    int dtype() const {
        return std::holds_alternative<std::vector<float>>(values_) ? 10 : 9;
    }

    void set_dtype(int dtype) {
        if (dtype==10) {
            if (!std::holds_alternative<std::vector<float>>(values_))
                values_.emplace<std::vector<float>>();
        } else if (dtype==9) {
            if (!std::holds_alternative<std::vector<double>>(values_))
                values_.emplace<std::vector<double>>();
        } else {
            runtime_text_failure("invalid autograd floating dtype");
        }
    }

    std::size_t size() const {
        return std::visit([](const auto& values){ return values.size(); }, values_);
    }

    bool empty() const { return size()==0; }

    void resize(std::size_t count) {
        std::visit([&](auto& values){ values.resize(count); }, values_);
    }

    void assign(std::size_t count,double value) {
        if (auto* values=std::get_if<std::vector<float>>(&values_))
            values->assign(count,static_cast<float>(value));
        else
            std::get<std::vector<double>>(values_).assign(count,value);
    }

    double scalar_as_double(std::size_t index) const {
        if (const auto* values=std::get_if<std::vector<float>>(&values_))
            return static_cast<double>((*values)[index]);
        return std::get<std::vector<double>>(values_)[index];
    }

    template <typename T>
    const std::vector<T>& typed() const {
        return std::get<std::vector<T>>(values_);
    }

    template <typename T>
    std::vector<T>& typed() {
        return std::get<std::vector<T>>(values_);
    }

private:
    std::variant<std::vector<float>,std::vector<double>> values_;
};

struct AutogradNode {
    explicit AutogradNode(int element_dtype=10)
        : dtype(element_dtype), data(element_dtype), aux(element_dtype) {}

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

    ~AutogradNode() noexcept {
        if(device_tensor){
            release_managed_tensor(device_tensor);
            device_tensor=nullptr;
        }
        if(device_aux){
            release_managed_tensor(device_aux);
            device_aux=nullptr;
        }
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
        return make_autograd_target_value(new AutogradTargetHandle{source->slot});
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
    release_managed_tensor(handle->slot->gradient);
    handle->slot->gradient=nullptr;
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
    explicit AutogradGradient(int element_dtype=10) : dtype(element_dtype), data(element_dtype) {}
    AutogradGradient(const AutogradGradient&)=delete;
    AutogradGradient& operator=(const AutogradGradient&)=delete;
    AutogradGradient(AutogradGradient&& other) noexcept
        : dtype(other.dtype),
          shape(std::move(other.shape)),
          data(std::move(other.data)),
          device_tensor(std::exchange(other.device_tensor,nullptr)) {}
    AutogradGradient& operator=(AutogradGradient&& other) noexcept {
        if(this==&other) return *this;
        if(device_tensor) release_managed_tensor(device_tensor);
        dtype=other.dtype;
        shape=std::move(other.shape);
        data=std::move(other.data);
        device_tensor=std::exchange(other.device_tensor,nullptr);
        return *this;
    }
    ~AutogradGradient() {
        if(device_tensor) release_managed_tensor(device_tensor);
    }

    int dtype{10};
    std::vector<long long> shape;
    AutogradBuffer data;
    TensorValue* device_tensor{};
};

[[noreturn]] void autograd_fail(const char* message, unsigned long long line, unsigned long long column) {
    std::fprintf(stderr, "Quidra runtime error[AUTOGRAD] at %llu:%llu: %s\n", line, column, message);
    std::exit(101);
}

AutogradBuffer tensor_float_values(const TensorValue& tensor,
                                 unsigned long long line,
                                 unsigned long long column) {
    tensor_require_cpu(*tensor.storage, "autograd tensor conversion", line, column);
    tensor_require_initialized(tensor,line,column);
    if(tensor.storage->dtype!=9&&tensor.storage->dtype!=10)
        autograd_fail("autograd values require float32 or float tensors",line,column);
    const auto count=tensor_logical_count(tensor);
    AutogradBuffer result(tensor.storage->dtype);
    if(tensor.storage->dtype==10){
        auto& values=result.typed<float>();
        values.resize(count);
        for(std::size_t i=0;i<count;++i){
            const auto index=tensor_storage_index(tensor,i);
            std::memcpy(&values[i],tensor.storage->data.data()+index*sizeof(float),sizeof(float));
        }
    }else{
        auto& values=result.typed<double>();
        values.resize(count);
        for(std::size_t i=0;i<count;++i){
            const auto index=tensor_storage_index(tensor,i);
            std::memcpy(&values[i],tensor.storage->data.data()+index*sizeof(double),sizeof(double));
        }
    }
    return result;
}

std::shared_ptr<AutogradNode> autograd_constant_node(const TensorValue& tensor,
                                                 unsigned long long line,
                                                 unsigned long long column) {
    tensor_require_initialized(tensor,line,column);
    if(tensor.storage->dtype!=9&&tensor.storage->dtype!=10)
        autograd_fail("autograd values require float32 or float tensors",line,column);
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
    return node;
}

void tensor_attach_view_graph(
    TensorValue* result,const TensorValue& source,AutogradOp operation,
    std::vector<std::size_t> aux,
    unsigned long long line,unsigned long long column) {
    if(!result||!source.graph) return;
    if(source.storage->dtype!=9&&source.storage->dtype!=10)
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
    result->graph=std::move(node);
}

TensorValue* autograd_tensor_from_values(
    int dtype,const std::vector<long long>& shape,const AutogradBuffer& data) {
    if(data.dtype()!=dtype) runtime_text_failure("autograd buffer dtype mismatch");
    auto* storage=tensor_storage_create(dtype,data.size(),1);
    if(dtype==10){
        const auto& values=data.typed<float>();
        if(!values.empty())
            std::memcpy(storage->data.data(),values.data(),values.size()*sizeof(float));
    }else if(dtype==9){
        const auto& values=data.typed<double>();
        if(!values.empty())
            std::memcpy(storage->data.data(),values.data(),values.size()*sizeof(double));
    }else{
        delete storage;
        runtime_text_failure("invalid autograd floating dtype");
    }
    return tensor_descriptor(storage,shape,tensor_contiguous_strides(shape),0);
}

TensorValue* autograd_tensor_from_node(const AutogradNode& node) {
    if(node.device_tensor)
        return static_cast<TensorValue*>(quidra_tensor_clone(node.device_tensor));
    return autograd_tensor_from_values(node.dtype,node.shape,node.data);
}

void autograd_accumulate_slot(
    const std::shared_ptr<AutogradSlot>& slot,TensorValue* gradient,
    unsigned long long line,unsigned long long column,
    bool preserve_graph=false) {
    if(!slot||!gradient) autograd_fail("invalid autograd gradient slot",line,column);
    if(slot->dtype!=9&&slot->dtype!=10)
        autograd_fail("invalid autograd gradient slot dtype",line,column);
    if(gradient->storage->dtype!=slot->dtype){
        auto* converted=static_cast<TensorValue*>(
            quidra_tensor_cast(gradient,slot->dtype,line,column));
        release_managed_tensor(gradient);
        gradient=converted;
    }
    if(!preserve_graph) gradient->graph.reset();
    gradient->grad_slot.reset();
    if(!slot->gradient){
        slot->gradient=gradient;
        return;
    }
    auto* combined=static_cast<TensorValue*>(
        quidra_tensor_binary(slot->gradient,gradient,nullptr,0,1,line,column));
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
    if(a.shape!=b.shape || autograd_node_count(a)!=autograd_node_count(b))
        autograd_fail("autograd operand shapes must match",line,column);
    if(a.dtype!=b.dtype)
        autograd_fail("autograd operand element types must match",line,column);
    autograd_require_same_node_device("autograd operation",a,b,line,column);
}

template <typename T>
void autograd_apply_unary_t(std::vector<T>& values,int op,const std::vector<long long>& shape,
                          unsigned long long line,unsigned long long column) {
    if(op==1){for(auto&v:values)v=std::abs(v);return;}
    if(op==2){for(auto&v:values)v=std::exp(v);return;}
    if(op==3){
        for(auto&v:values){
            if(!(v>T{0})||!std::isfinite(v))
                autograd_fail("logarithm requires finite positive values",line,column);
            v=std::log(v);
        }
        return;
    }
    if(op==4){
        if(values.empty()) autograd_fail("mean requires at least one element",line,column);
        T total=T{0};
        for(const auto value:values) total=static_cast<T>(total+value);
        const T average=static_cast<T>(total/static_cast<T>(values.size()));
        values.clear();
        values.push_back(average);
        return;
    }
    if(op==5||op==6||op==7){
        if(shape.empty()) autograd_fail("last-axis reduction requires rank >= 1",line,column);
        if(shape.back()<=0) autograd_fail("last-axis reduction requires a non-empty last axis",line,column);
        const auto width=static_cast<std::size_t>(shape.back());
        for(std::size_t base=0;base<values.size();base+=width){
            T reduced=op==5?T{0}:values[base];
            for(std::size_t j=0;j<width;++j){
                if(op==5) reduced=static_cast<T>(reduced+values[base+j]);
                else if(op==6) reduced=std::max(reduced,values[base+j]);
                else reduced=std::min(reduced,values[base+j]);
            }
            for(std::size_t j=0;j<width;++j) values[base+j]=reduced;
        }
        return;
    }
    autograd_fail("unknown autograd unary operation",line,column);
}



void autograd_apply_unary(AutogradBuffer& values,int dtype,int op,const std::vector<long long>& shape,
                        unsigned long long line,unsigned long long column) {
    if(dtype==10) autograd_apply_unary_t(values.typed<float>(),op,shape,line,column);
    else if(dtype==9) autograd_apply_unary_t(values.typed<double>(),op,shape,line,column);
    else autograd_fail("invalid autograd dtype",line,column);
}
TensorValue* autograd_tensor_unary_raw_gpu(
    TensorValue& input,int op,unsigned long long line,unsigned long long column) {
    tensor_require_initialized(input,line,column);
    if(input.storage->dtype!=9&&input.storage->dtype!=10&&op!=6&&op!=7)
        autograd_fail("autograd tensor unary operation requires a floating dtype",line,column);
    if(tensor_on_cpu(*input.storage))
        autograd_fail("internal GPU unary path received a CPU tensor",line,column);

    TensorStorage* materialized=nullptr;
    const TensorStorage* source=input.storage;
    std::size_t source_offset=input.offset*tensor_dtype_bytes(input.storage->dtype);
    if(!tensor_is_contiguous_value(input)){
        materialized=tensor_gpu_materialize_storage(input,line,column);
        source=materialized;
        source_offset=0;
    }
    std::vector<long long> output_shape=input.shape;
    std::size_t output_count=tensor_logical_count(input);
    if(op==4){ output_shape.clear(); output_count=1; }
    auto* output=tensor_storage_create(
        input.storage->dtype,output_count,1,input.storage->device,line,column);
    std::string backend_error;
    bool ok=false;
    if(op>=1&&op<=3){
        const int compute_op=op==1?2:op==2?3:4;
        ok=quidra::device::compute_unary(
            output->gpu_buffer,source->gpu_buffer,source_offset,
            input.storage->dtype,compute_op,tensor_logical_count(input),backend_error);
    }else if(op==4){
        ok=quidra::device::compute_mean_to(
            output->gpu_buffer,source->gpu_buffer,input.storage->dtype,
            tensor_logical_count(input),backend_error);
    }else if(op==5||op==6||op==7){
        if(input.shape.empty()||input.shape.back()<=0){
            if(materialized)tensor_storage_release(materialized);
            tensor_storage_release(output);
            autograd_fail("last-axis reduction requires a non-empty last axis",line,column);
        }
        ok=quidra::device::compute_last_reduce_broadcast(
            output->gpu_buffer,source->gpu_buffer,input.storage->dtype,
            tensor_logical_count(input),static_cast<std::size_t>(input.shape.back()),
            op==5?1:op==6?2:3,backend_error);
    }else{
        if(materialized)tensor_storage_release(materialized);
        tensor_storage_release(output);
        autograd_fail("unknown autograd tensor unary operation",line,column);
    }
    if(materialized)tensor_storage_release(materialized);
    if(!ok){
        tensor_storage_release(output);
        autograd_fail(backend_error.c_str(),line,column);
    }
    auto output_strides=tensor_contiguous_strides(output_shape);
    return tensor_descriptor(
        output,std::move(output_shape),std::move(output_strides),0);
}

std::shared_ptr<AutogradNode> autograd_unary_node(const std::shared_ptr<AutogradNode>& input,int op,
                                              unsigned long long line,unsigned long long column) {
    auto node=std::make_shared<AutogradNode>(input->dtype);
    node->dtype=input->dtype;
    node->shape=input->shape;
    node->parents={input};
    node->op=op==1?AutogradOp::Absolute:
        op==2?AutogradOp::Exponential:
        op==3?AutogradOp::Logarithm:
        op==4?AutogradOp::Mean:
        op==5?AutogradOp::SumLast:
        op==6?AutogradOp::MaxLast:AutogradOp::MinLast;
    if(input->device_tensor){
        node->device_tensor=autograd_tensor_unary_raw_gpu(
            *input->device_tensor,op,line,column);
        if(!node->device_tensor)
            autograd_fail("GPU autograd unary operation returned null",line,column);
    }else{
        node->data=input->data;
        autograd_apply_unary(node->data,node->dtype,op,node->shape,line,column);
    }
    if(op==4) node->shape={};
    return node;
}

template <typename T>
std::shared_ptr<AutogradNode> autograd_symbolic_binary_t(
    const std::shared_ptr<AutogradNode>& left,
    const std::shared_ptr<AutogradNode>& right,
    int operation,
    unsigned long long line,unsigned long long column) {
    const auto& a=left->data.typed<T>();
    const auto& b=right->data.typed<T>();
    if(a.size()!=b.size()) autograd_fail("higher-order gradient shape mismatch",line,column);
    std::vector<T> values(a.size(),T{0});
    for(std::size_t i=0;i<values.size();++i){
        if(operation==1) values[i]=static_cast<T>(a[i]+b[i]);
        else if(operation==2) values[i]=static_cast<T>(a[i]-b[i]);
        else if(operation==3) values[i]=static_cast<T>(a[i]*b[i]);
        else {
            if(b[i]==T{0}) autograd_fail("division by zero",line,column);
            values[i]=static_cast<T>(a[i]/b[i]);
        }
    }
    auto node=std::make_shared<AutogradNode>(left->dtype);
    node->shape=left->shape;
    node->parents={left,right};
    node->op=operation==1?AutogradOp::Add:
        operation==2?AutogradOp::Sub:
        operation==3?AutogradOp::Mul:AutogradOp::Div;
    node->data=AutogradBuffer(std::move(values));
    return node;
}

std::shared_ptr<AutogradNode> autograd_symbolic_binary(
    const std::shared_ptr<AutogradNode>& left,
    const std::shared_ptr<AutogradNode>& right,
    int operation,
    unsigned long long line,unsigned long long column) {
    if(!left||!right) autograd_fail("null higher-order gradient operand",line,column);
    if(left->device_tensor||right->device_tensor)
        autograd_fail("backward(track = true) currently requires CPU tensors",line,column);
    autograd_require_same_shape(*left,*right,line,column);
    if(left->dtype==10) return autograd_symbolic_binary_t<float>(
        left,right,operation,line,column);
    if(left->dtype==9) return autograd_symbolic_binary_t<double>(
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
        if(operation==1) values[i]=static_cast<T>(left+right);
        else if(operation==2) values[i]=static_cast<T>(left-right);
        else if(operation==3) values[i]=static_cast<T>(left*right);
        else {
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
    return node;
}

std::shared_ptr<AutogradNode> autograd_symbolic_scalar(
    const std::shared_ptr<AutogradNode>& input,double scalar,
    int operation,bool scalar_left,
    unsigned long long line,unsigned long long column) {
    if(!input) autograd_fail("null higher-order gradient operand",line,column);
    if(input->device_tensor)
        autograd_fail("backward(track = true) currently requires CPU tensors",line,column);
    if(input->dtype==10) return autograd_symbolic_scalar_t<float>(
        input,scalar,operation,scalar_left,line,column);
    if(input->dtype==9) return autograd_symbolic_scalar_t<double>(
        input,scalar,operation,scalar_left,line,column);
    autograd_fail("invalid higher-order gradient dtype",line,column);
}

std::shared_ptr<AutogradNode> autograd_symbolic_sign(
    const std::shared_ptr<AutogradNode>& input) {
    auto node=std::make_shared<AutogradNode>(input->dtype);
    node->shape=input->shape;
    node->op=AutogradOp::Leaf;
    node->data=input->data;
    if(input->dtype==10){
        auto& values=node->data.typed<float>();
        for(auto& value:values) value=value>0.0f?1.0f:value<0.0f?-1.0f:0.0f;
    }else{
        auto& values=node->data.typed<double>();
        for(auto& value:values) value=value>0.0?1.0:value<0.0?-1.0:0.0;
    }
    return node;
}

std::shared_ptr<AutogradNode> autograd_symbolic_mean_backward(
    const std::shared_ptr<AutogradNode>& gradient,
    const std::vector<long long>& target_shape,
    unsigned long long line,unsigned long long column) {
    if(gradient->data.size()!=1)
        autograd_fail("mean higher-order gradient requires scalar input",line,column);
    const auto count=tensor_element_count(target_shape,line,column);
    if(count==0) autograd_fail("mean gradient requires at least one element",line,column);
    auto node=std::make_shared<AutogradNode>(gradient->dtype);
    node->shape=target_shape;
    node->parents={gradient};
    node->op=AutogradOp::MeanBackward;
    node->data.assign(
        count,gradient->data.scalar_as_double(0)/static_cast<double>(count));
    return node;
}

std::shared_ptr<AutogradNode> autograd_symbolic_sum_last_backward(
    const std::shared_ptr<AutogradNode>& gradient,
    const std::vector<long long>& target_shape,
    unsigned long long line,unsigned long long column) {
    if(target_shape.empty()||target_shape.back()<=0)
        autograd_fail("sum_last higher-order gradient requires a non-empty last axis",line,column);
    auto node=std::make_shared<AutogradNode>(gradient->dtype);
    node->shape=target_shape;
    node->parents={gradient};
    node->op=AutogradOp::SumLastBackward;
    node->data=gradient->data;
    autograd_apply_unary(node->data,node->dtype,5,target_shape,line,column);
    return node;
}

template <typename T>
std::shared_ptr<AutogradNode> autograd_symbolic_extrema_backward_t(
    const std::shared_ptr<AutogradNode>& gradient,
    const std::shared_ptr<AutogradNode>& input,
    bool maximum,
    unsigned long long line,unsigned long long column) {
    if(input->shape.empty()||input->shape.back()<=0)
        autograd_fail("extrema higher-order gradient requires a non-empty last axis",line,column);
    if(gradient->shape!=input->shape)
        autograd_fail("extrema higher-order gradient shape mismatch",line,column);
    const auto width=static_cast<std::size_t>(input->shape.back());
    const auto& source=input->data.typed<T>();
    if(source.size()!=gradient->data.size()||source.size()%width!=0)
        autograd_fail("extrema higher-order gradient size mismatch",line,column);

    std::vector<T> mask_values(source.size(),T{0});
    for(std::size_t base=0;base<source.size();base+=width){
        std::size_t selected=0;
        for(std::size_t j=1;j<width;++j){
            const bool better=maximum
                ? source[base+j]>source[base+selected]
                : source[base+j]<source[base+selected];
            if(better) selected=j;
        }
        mask_values[base+selected]=T{1};
    }

    auto mask=std::make_shared<AutogradNode>(input->dtype);
    mask->shape=input->shape;
    mask->op=AutogradOp::Leaf;
    mask->data=AutogradBuffer(std::move(mask_values));

    auto summed=autograd_symbolic_sum_last_backward(
        gradient,input->shape,line,column);
    return autograd_symbolic_binary(summed,mask,3,line,column);
}

std::shared_ptr<AutogradNode> autograd_symbolic_extrema_backward(
    const std::shared_ptr<AutogradNode>& gradient,
    const std::shared_ptr<AutogradNode>& input,
    bool maximum,
    unsigned long long line,unsigned long long column) {
    if(!gradient||!input)
        autograd_fail("null extrema higher-order gradient operand",line,column);
    if(gradient->device_tensor||input->device_tensor)
        autograd_fail("backward(track = true) currently requires CPU tensors",line,column);
    if(gradient->dtype!=input->dtype)
        autograd_fail("extrema higher-order gradient dtype mismatch",line,column);
    if(input->dtype==10)
        return autograd_symbolic_extrema_backward_t<float>(
            gradient,input,maximum,line,column);
    if(input->dtype==9)
        return autograd_symbolic_extrema_backward_t<double>(
            gradient,input,maximum,line,column);
    autograd_fail("invalid extrema higher-order gradient dtype",line,column);
}


template <typename T>
std::vector<T> autograd_gather_values(
    const std::vector<T>& input,const std::vector<std::size_t>& indices,
    unsigned long long line,unsigned long long column) {
    std::vector<T> output(indices.size());
    for(std::size_t i=0;i<indices.size();++i){
        if(indices[i]>=input.size())
            autograd_fail("tensor gather index is outside input",line,column);
        output[i]=input[indices[i]];
    }
    return output;
}

template <typename T>
std::vector<T> autograd_gather_backward_values(
    const std::vector<T>& gradient,std::size_t source_count,
    const std::vector<std::size_t>& indices,
    unsigned long long line,unsigned long long column) {
    if(gradient.size()!=indices.size())
        autograd_fail("tensor gather backward size mismatch",line,column);
    std::vector<T> output(source_count,T{0});
    for(std::size_t i=0;i<indices.size();++i){
        if(indices[i]>=source_count)
            autograd_fail("tensor gather backward index is outside input",line,column);
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

template <typename T>
AutogradBuffer autograd_matmul_values(
    const AutogradBuffer& left,const std::vector<long long>& left_shape,
    const AutogradBuffer& right,const std::vector<long long>& right_shape,
    std::vector<long long>& output_shape,
    unsigned long long line,unsigned long long column) {
    if(left_shape.empty()||(right_shape.size()!=1&&right_shape.size()!=2))
        autograd_fail("matmul autograd requires left rank >= 1 and right rank 1 or 2",line,column);
    std::vector<long long> leading(left_shape.begin(),left_shape.end()-1);
    const auto rows=tensor_element_count(leading,line,column);
    const auto inner=static_cast<std::size_t>(left_shape.back());
    const auto right_inner=static_cast<std::size_t>(right_shape[0]);
    const bool right_vector=right_shape.size()==1;
    const auto columns=right_vector?std::size_t{1}:static_cast<std::size_t>(right_shape[1]);
    if(inner!=right_inner) autograd_fail("matmul autograd inner dimensions do not match",line,column);
    output_shape=leading;
    if(!right_vector) output_shape.push_back(static_cast<long long>(columns));
    const auto& a=left.typed<T>();
    const auto& b=right.typed<T>();
    if(a.size()!=rows*inner||b.size()!=inner*columns)
        autograd_fail("matmul autograd storage size mismatch",line,column);
    std::vector<T> out(rows*columns,T{0});
    for(std::size_t i=0;i<rows;++i)
        for(std::size_t j=0;j<columns;++j){
            T total=T{0};
            for(std::size_t k=0;k<inner;++k)
                total=static_cast<T>(total+static_cast<T>(a[i*inner+k]*b[k*columns+j]));
            out[i*columns+j]=total;
        }
    return AutogradBuffer(std::move(out));
}


std::shared_ptr<AutogradNode> autograd_symbolic_matmul(
    const std::shared_ptr<AutogradNode>& left,
    const std::shared_ptr<AutogradNode>& right,
    unsigned long long line,unsigned long long column) {
    if(!left||!right) autograd_fail("null higher-order matmul operand",line,column);
    autograd_require_same_node_device("matmul",*left,*right,line,column);
    if(left->dtype!=right->dtype)
        autograd_fail("higher-order matmul requires identical dtypes",line,column);
    auto node=std::make_shared<AutogradNode>(left->dtype);
    node->parents={left,right};
    node->op=AutogradOp::Matmul;
    if(left->device_tensor){
        node->device_tensor=static_cast<TensorValue*>(quidra_linear_matmul(
            left->device_tensor,right->device_tensor,line,column));
        node->shape=node->device_tensor->shape;
    }else if(left->dtype==10){
        node->data=autograd_matmul_values<float>(
            left->data,left->shape,right->data,right->shape,node->shape,line,column);
    }else if(left->dtype==9){
        node->data=autograd_matmul_values<double>(
            left->data,left->shape,right->data,right->shape,node->shape,line,column);
    }else{
        autograd_fail("higher-order matmul requires a floating dtype",line,column);
    }
    return node;
}

std::shared_ptr<AutogradNode> autograd_symbolic_matmul_left_gradient(
    const std::shared_ptr<AutogradNode>& gradient,
    const std::shared_ptr<AutogradNode>& left,
    const std::shared_ptr<AutogradNode>& right,
    unsigned long long line,unsigned long long column) {
    if(left->shape.empty()||(right->shape.size()!=1&&right->shape.size()!=2))
        autograd_fail("invalid matmul gradient shape",line,column);
    std::vector<long long> leading(left->shape.begin(),left->shape.end()-1);
    const auto rows=tensor_element_count(leading,line,column);
    const auto inner=left->shape.back();
    const auto columns=right->shape.size()==1?1:right->shape[1];
    auto l2=autograd_symbolic_reshape(left,{static_cast<long long>(rows),inner},line,column);
    auto r2=right->shape.size()==1
        ? autograd_symbolic_reshape(right,{inner,1},line,column) : right;
    auto g2=autograd_symbolic_reshape(
        gradient,{static_cast<long long>(rows),columns},line,column);
    auto rt=autograd_symbolic_transpose(r2,0,1,line,column);
    auto da2=autograd_symbolic_matmul(g2,rt,line,column);
    return autograd_symbolic_reshape(da2,left->shape,line,column);
}

std::shared_ptr<AutogradNode> autograd_symbolic_matmul_right_gradient(
    const std::shared_ptr<AutogradNode>& gradient,
    const std::shared_ptr<AutogradNode>& left,
    const std::shared_ptr<AutogradNode>& right,
    unsigned long long line,unsigned long long column) {
    if(left->shape.empty()||(right->shape.size()!=1&&right->shape.size()!=2))
        autograd_fail("invalid matmul gradient shape",line,column);
    std::vector<long long> leading(left->shape.begin(),left->shape.end()-1);
    const auto rows=tensor_element_count(leading,line,column);
    const auto inner=left->shape.back();
    const auto columns=right->shape.size()==1?1:right->shape[1];
    auto l2=autograd_symbolic_reshape(left,{static_cast<long long>(rows),inner},line,column);
    auto g2=autograd_symbolic_reshape(
        gradient,{static_cast<long long>(rows),columns},line,column);
    auto lt=autograd_symbolic_transpose(l2,0,1,line,column);
    auto db2=autograd_symbolic_matmul(lt,g2,line,column);
    return autograd_symbolic_reshape(db2,right->shape,line,column);
}

std::shared_ptr<AutogradNode> autograd_symbolic_reshape(
    const std::shared_ptr<AutogradNode>& input,
    const std::vector<long long>& output_shape,
    unsigned long long line,unsigned long long column) {
    if(!input) autograd_fail("null higher-order reshape operand",line,column);
    if(tensor_element_count(input->shape,line,column)!=
       tensor_element_count(output_shape,line,column))
        autograd_fail("higher-order reshape cannot change element count",line,column);
    auto node=std::make_shared<AutogradNode>(input->dtype);
    node->shape=output_shape;
    node->parents={input};
    node->op=AutogradOp::Reshape;
    if(input->device_tensor){
        auto* source=input->device_tensor;
        if(!tensor_is_contiguous_value(*source))
            autograd_fail("higher-order reshape requires contiguous storage",line,column);
        if(source->storage->owners==std::numeric_limits<std::size_t>::max())
            runtime_text_failure("tensor storage owner overflow");
        ++source->storage->owners;
        node->device_tensor=tensor_descriptor(
            source->storage,output_shape,tensor_contiguous_strides(output_shape),source->offset);
    }else{
        node->data=input->data;
    }
    return node;
}

template <typename T>
AutogradBuffer autograd_transpose_values(
    const AutogradBuffer& input,const std::vector<long long>& input_shape,
    std::size_t axis0,std::size_t axis1,
    unsigned long long line,unsigned long long column) {
    const auto count=tensor_element_count(input_shape,line,column);
    const auto& values=input.typed<T>();
    if(values.size()!=count) autograd_fail("transpose graph size mismatch",line,column);
    std::vector<long long> output_shape=input_shape;
    std::swap(output_shape[axis0],output_shape[axis1]);
    std::vector<T> output(count);
    auto input_strides=tensor_contiguous_strides(input_shape);
    auto output_strides=tensor_contiguous_strides(output_shape);
    for(std::size_t linear=0;linear<count;++linear){
        std::size_t rest=linear;
        std::vector<std::size_t> coordinates(output_shape.size());
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
        auto strides=source->strides;
        std::swap(strides[axis0],strides[axis1]);
        node->device_tensor=tensor_descriptor(
            source->storage,node->shape,std::move(strides),source->offset);
    }else if(input->dtype==10){
        node->data=autograd_transpose_values<float>(
            input->data,input->shape,axis0,axis1,line,column);
    }else{
        node->data=autograd_transpose_values<double>(
            input->data,input->shape,axis0,axis1,line,column);
    }
    return node;
}

std::shared_ptr<AutogradNode> autograd_symbolic_gather(
    const std::shared_ptr<AutogradNode>& input,
    const std::vector<long long>& output_shape,
    const std::vector<std::size_t>& indices,
    unsigned long long line,unsigned long long column) {
    if(!input||input->device_tensor)
        autograd_fail("higher-order tensor gather requires CPU tensors",line,column);
    if(tensor_element_count(output_shape,line,column)!=indices.size())
        autograd_fail("tensor gather output shape does not match index count",line,column);
    auto node=std::make_shared<AutogradNode>(input->dtype);
    node->shape=output_shape;
    node->parents={input};
    node->op=AutogradOp::Gather;
    node->aux_index=indices;
    if(input->dtype==10)
        node->data=AutogradBuffer(autograd_gather_values<float>(
            input->data.typed<float>(),indices,line,column));
    else if(input->dtype==9)
        node->data=AutogradBuffer(autograd_gather_values<double>(
            input->data.typed<double>(),indices,line,column));
    else
        autograd_fail("tensor gather autograd requires a floating dtype",line,column);
    return node;
}

std::shared_ptr<AutogradNode> autograd_symbolic_gather_backward(
    const std::shared_ptr<AutogradNode>& gradient,
    const std::vector<long long>& input_shape,
    const std::vector<std::size_t>& indices,
    unsigned long long line,unsigned long long column) {
    if(!gradient||gradient->device_tensor)
        autograd_fail("higher-order tensor gather requires CPU tensors",line,column);
    const auto source_count=tensor_element_count(input_shape,line,column);
    auto node=std::make_shared<AutogradNode>(gradient->dtype);
    node->shape=input_shape;
    node->parents={gradient};
    node->op=AutogradOp::GatherBackward;
    node->aux_index=indices;
    if(gradient->dtype==10)
        node->data=AutogradBuffer(autograd_gather_backward_values<float>(
            gradient->data.typed<float>(),source_count,indices,line,column));
    else if(gradient->dtype==9)
        node->data=AutogradBuffer(autograd_gather_backward_values<double>(
            gradient->data.typed<double>(),source_count,indices,line,column));
    else
        autograd_fail("tensor gather autograd requires a floating dtype",line,column);
    return node;
}

void autograd_topological(const std::shared_ptr<AutogradNode>& node,
                        std::unordered_set<const AutogradNode*>& seen,
                        std::vector<std::shared_ptr<AutogradNode>>& order);

void autograd_add_symbolic_gradient(
    std::unordered_map<const AutogradNode*,std::shared_ptr<AutogradNode>>& gradients,
    const std::shared_ptr<AutogradNode>& target,
    std::shared_ptr<AutogradNode> value,
    unsigned long long line,unsigned long long column) {
    auto& current=gradients[target.get()];
    if(!current) current=std::move(value);
    else current=autograd_symbolic_binary(current,value,1,line,column);
}

void autograd_backward_tracked(
    const std::shared_ptr<AutogradNode>& loss,
    const std::vector<std::shared_ptr<AutogradSlot>>& selected,
    unsigned long long line,unsigned long long column) {
    std::unordered_set<const AutogradNode*> seen;
    std::vector<std::shared_ptr<AutogradNode>> order;
    autograd_topological(loss,seen,order);

    for(const auto& node:order){
        if(node->device_tensor)
            autograd_fail("backward(track = true) currently requires CPU tensors",line,column);
    }

    std::unordered_map<const AutogradNode*,std::shared_ptr<AutogradNode>> gradients;
    gradients.reserve(order.size());
    auto seed=autograd_symbolic_scalar(loss,0.0,3,false,line,column);
    seed=autograd_symbolic_scalar(seed,1.0,1,false,line,column);
    gradients.emplace(loss.get(),seed);

    for(auto it=order.rbegin();it!=order.rend();++it){
        const auto& node=*it;
        const auto found=gradients.find(node.get());
        if(found==gradients.end()) continue;
        const auto& g=found->second;

        if(node->target_identity){
            for(const auto& slot:selected){
                if(!slot || slot->identity!=node->target_identity) continue;
                auto* value=autograd_tensor_from_node(*g);
                value->graph=g;
                autograd_accumulate_slot(slot,value,line,column,true);
            }
        }
        if(node->parents.empty()) continue;

        if(node->op==AutogradOp::Add){
            autograd_add_symbolic_gradient(gradients,node->parents[0],g,line,column);
            autograd_add_symbolic_gradient(gradients,node->parents[1],g,line,column);
        }else if(node->op==AutogradOp::Sub){
            autograd_add_symbolic_gradient(gradients,node->parents[0],g,line,column);
            autograd_add_symbolic_gradient(
                gradients,node->parents[1],
                autograd_symbolic_scalar(g,-1.0,3,false,line,column),
                line,column);
        }else if(node->op==AutogradOp::Mul){
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_symbolic_binary(g,node->parents[1],3,line,column),
                line,column);
            autograd_add_symbolic_gradient(
                gradients,node->parents[1],
                autograd_symbolic_binary(g,node->parents[0],3,line,column),
                line,column);
        }else if(node->op==AutogradOp::Div){
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_symbolic_binary(g,node->parents[1],4,line,column),
                line,column);
            auto numerator=autograd_symbolic_binary(g,node->parents[0],3,line,column);
            auto denominator=autograd_symbolic_binary(
                node->parents[1],node->parents[1],3,line,column);
            auto right=autograd_symbolic_binary(numerator,denominator,4,line,column);
            right=autograd_symbolic_scalar(right,-1.0,3,false,line,column);
            autograd_add_symbolic_gradient(
                gradients,node->parents[1],std::move(right),line,column);
        }else if(node->op==AutogradOp::ScalarBinary){
            const auto operation=static_cast<int>(node->aux_index[0]);
            const bool scalar_left=node->aux_index[1]!=0;
            const double scalar=node->aux.scalar_as_double(0);
            std::shared_ptr<AutogradNode> result;
            if(operation==1 || (operation==2&&!scalar_left)) result=g;
            else if(operation==2) result=autograd_symbolic_scalar(
                g,-1.0,3,false,line,column);
            else if(operation==3) result=autograd_symbolic_scalar(
                g,scalar,3,false,line,column);
            else if(!scalar_left) result=autograd_symbolic_scalar(
                g,scalar,4,false,line,column);
            else{
                auto numerator=autograd_symbolic_scalar(g,-scalar,3,false,line,column);
                auto square=autograd_symbolic_binary(
                    node->parents[0],node->parents[0],3,line,column);
                result=autograd_symbolic_binary(numerator,square,4,line,column);
            }
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],std::move(result),line,column);
        }else if(node->op==AutogradOp::Absolute){
            auto sign=autograd_symbolic_sign(node->parents[0]);
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_symbolic_binary(g,sign,3,line,column),line,column);
        }else if(node->op==AutogradOp::Exponential){
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_symbolic_binary(g,node,3,line,column),line,column);
        }else if(node->op==AutogradOp::Logarithm){
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_symbolic_binary(g,node->parents[0],4,line,column),line,column);
        }else if(node->op==AutogradOp::Mean){
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_symbolic_mean_backward(
                    g,node->parents[0]->shape,line,column),
                line,column);
        }else if(node->op==AutogradOp::SumLast){
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_symbolic_sum_last_backward(
                    g,node->parents[0]->shape,line,column),
                line,column);
        }else if(node->op==AutogradOp::MaxLast ||
                 node->op==AutogradOp::MinLast){
            if(node->parents.size()!=1)
                autograd_fail("invalid tensor extrema graph",line,column);
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_symbolic_extrema_backward(
                    g,node->parents[0],
                    node->op==AutogradOp::MaxLast,line,column),
                line,column);
        }else if(node->op==AutogradOp::Matmul){
            if(node->parents.size()!=2)
                autograd_fail("invalid matmul graph",line,column);
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_symbolic_matmul_left_gradient(
                    g,node->parents[0],node->parents[1],line,column),
                line,column);
            autograd_add_symbolic_gradient(
                gradients,node->parents[1],
                autograd_symbolic_matmul_right_gradient(
                    g,node->parents[0],node->parents[1],line,column),
                line,column);
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
        }else if(node->op==AutogradOp::GatherBackward){
            if(node->parents.size()!=1)
                autograd_fail("invalid tensor gather backward graph",line,column);
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_symbolic_gather(
                    g,node->parents[0]->shape,node->aux_index,line,column),
                line,column);
        }else if(node->op==AutogradOp::MeanBackward){
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_unary_node(g,4,line,column),line,column);
        }else if(node->op==AutogradOp::SumLastBackward){
            autograd_add_symbolic_gradient(
                gradients,node->parents[0],
                autograd_unary_node(g,5,line,column),line,column);
        }else{
            autograd_fail("unsupported higher-order autograd node",line,column);
        }
    }
}

template <typename T>
void autograd_add_gradient(std::unordered_map<const AutogradNode*,std::vector<T>>& gradients,
                         const std::shared_ptr<AutogradNode>& node,std::vector<T> value) {
    auto& current=gradients[node.get()];
    if(current.empty()) current=std::move(value);
    else{
        if(current.size()!=value.size()) runtime_text_failure("autograd gradient size mismatch");
        for(std::size_t i=0;i<current.size();++i)
            current[i]=static_cast<T>(current[i]+value[i]);
    }
}

void autograd_topological(const std::shared_ptr<AutogradNode>& node,
                        std::unordered_set<const AutogradNode*>& seen,
                        std::vector<std::shared_ptr<AutogradNode>>& order) {
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
        stack.pop_back();
    }
}


} // namespace

extern "C" void* quidra_tensor_track(
    void* raw,unsigned long long line,unsigned long long column) {
    if(!raw) autograd_fail("null tensor",line,column);
    auto* source=static_cast<TensorValue*>(raw);
    if(source->storage->dtype!=9&&source->storage->dtype!=10)
        autograd_fail("track() requires tensor<float32> or tensor<float>",line,column);
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
    if(source->storage->dtype!=9&&source->storage->dtype!=10)
        autograd_fail("track() requires tensor<float32> or tensor<float>",line,column);
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
    if(source->storage->dtype!=9&&source->storage->dtype!=10)
        autograd_fail("retrack() requires tensor<float32> or tensor<float>",line,column);
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
    release_managed_tensor(tensor->grad_slot->gradient);
    tensor->grad_slot->gradient=nullptr;
}

TensorValue* tensor_gather_logical_indices(
    TensorValue& source,std::vector<std::size_t> logical_indices,
    std::vector<long long> output_shape,
    unsigned long long line,unsigned long long column) {
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
    for(std::size_t i=0;i<output_count;++i){
        const auto index=logical_indices[i];
        if(index>=source_count)
            tensor_fail("tensor.gather index is outside the source tensor",line,column);
        const auto physical=tensor_storage_index(source,index);
        if(physical>=source.storage->count)
            tensor_fail("tensor.gather source view exceeds storage",line,column);
        physical_indices[i]=static_cast<std::uint64_t>(physical);
    }

    auto* storage=tensor_storage_create(
        source.storage->dtype,output_count,0,source.storage->device,line,column);
    const auto width=tensor_dtype_bytes(source.storage->dtype);
    if(tensor_on_cpu(*source.storage)){
        for(std::size_t i=0;i<output_count;++i){
            const auto physical=static_cast<std::size_t>(physical_indices[i]);
            std::memcpy(
                storage->data.data()+i*width,
                source.storage->data.data()+physical*width,width);
            if(tracker_bit(source.storage->initialization,physical))
                tracker_set(storage->initialization,i);
        }
    }else{
        std::string backend_error;
        if(!quidra::device::compute_gather(
                storage->gpu_buffer,source.storage->gpu_buffer,
                source.storage->dtype,physical_indices.data(),
                output_count,backend_error)){
            tensor_storage_release(storage);
            tensor_fail(backend_error.c_str(),line,column);
        }
        for(std::size_t i=0;i<output_count;++i)
            if(tracker_bit(
                    source.storage->initialization,
                    static_cast<std::size_t>(physical_indices[i])))
                tracker_set(storage->initialization,i);
    }

    auto* result=tensor_descriptor(
        storage,output_shape,tensor_contiguous_strides(output_shape),0);
    if(source.graph){
        if(source.storage->dtype!=9&&source.storage->dtype!=10){
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
        result->graph=std::move(node);
    }
    return result;
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
            tensor_fail("tensor.gather index is outside the source tensor",line,column);
        logical_indices.push_back(static_cast<std::size_t>(index));
    }
    return tensor_gather_logical_indices(
        *source,std::move(logical_indices),std::move(output_shape),line,column);
}

extern "C" void* quidra_tensor_scatter(
    void* raw,void* indices_array,void* shape_array,
    unsigned long long line,unsigned long long column) {
    if(!raw) tensor_fail("tensor.scatter received a null tensor",line,column);
    auto& source=*static_cast<TensorValue*>(raw);
    if(source.storage->dtype<1||source.storage->dtype>10)
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
            tensor_fail("tensor.scatter index is outside the output tensor",line,column);
        const auto index=static_cast<std::size_t>(raw_indices[i]);
        if(index>=output_count)
            tensor_fail("tensor.scatter index is outside the output tensor",line,column);
        logical_indices[i]=index;
    }

    auto* storage=tensor_storage_create(
        source.storage->dtype,output_count,1,source.storage->device,line,column);
    if(tensor_on_cpu(*source.storage)){
        std::fill(storage->data.begin(),storage->data.end(),0);
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
                if constexpr(std::is_floating_point_v<T>){
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
            case 1: scatter_typed(std::int64_t{}); break;
            case 2: scatter_typed(std::int8_t{}); break;
            case 3: scatter_typed(std::int16_t{}); break;
            case 4: scatter_typed(std::int32_t{}); break;
            case 5: scatter_typed(std::uint8_t{}); break;
            case 6: scatter_typed(std::uint16_t{}); break;
            case 7: scatter_typed(std::uint32_t{}); break;
            case 8: scatter_typed(std::uint64_t{}); break;
            case 9: scatter_typed(double{}); break;
            case 10:scatter_typed(float{}); break;
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
            indices.data(),output_count,source_count,backend_error);
        tensor_storage_release(dense);
        if(!ok){
            tensor_storage_release(storage);
            tensor_fail(backend_error.c_str(),line,column);
        }
    }

    auto* result=tensor_descriptor(
        storage,output_shape,tensor_contiguous_strides(output_shape),0);
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
        result->graph=std::move(node);
    }
    return result;
}

extern "C" void* quidra_tensor_convolve(
    void* raw,void* kernel_raw,long long stride_raw,long long padding_raw,
    long long dilation_raw,unsigned long long line,unsigned long long column) {
    if(!raw||!kernel_raw)
        tensor_fail("tensor.convolve received a null tensor",line,column);
    auto& source=*static_cast<TensorValue*>(raw);
    auto& kernel=*static_cast<TensorValue*>(kernel_raw);
    if(source.storage->dtype!=kernel.storage->dtype)
        tensor_fail(
            "tensor.convolve requires input and kernel element types to match",
            line,column);
    if(source.storage->dtype<1||source.storage->dtype>10)
        tensor_fail(
            "tensor.convolve requires a numeric tensor element type",
            line,column);
    if(source.storage->device!=kernel.storage->device)
        tensor_fail(
            "tensor.convolve input and kernel are on different devices; transfer them explicitly",
            line,column);
    if(stride_raw<=0||dilation_raw<=0||padding_raw<0)
        tensor_fail(
            "tensor.convolve requires stride > 0, dilation > 0, and padding >= 0",
            line,column);
    if(kernel.shape.empty())
        tensor_fail("tensor.convolve kernel rank must be at least 1",line,column);
    if(kernel.shape.size()>source.shape.size())
        tensor_fail("tensor.convolve kernel rank cannot exceed input rank",line,column);

    tensor_require_initialized(source,line,column);
    tensor_require_initialized(kernel,line,column);

    const auto stride=static_cast<std::size_t>(stride_raw);
    const auto padding=static_cast<std::size_t>(padding_raw);
    const auto dilation=static_cast<std::size_t>(dilation_raw);
    const auto rank=source.shape.size();
    const auto kernel_rank=kernel.shape.size();
    const auto leading_rank=rank-kernel_rank;
    std::vector<long long> output_shape=source.shape;
    for(std::size_t axis=0;axis<kernel_rank;++axis){
        const auto input_extent=source.shape[leading_rank+axis];
        const auto kernel_extent=kernel.shape[axis];
        if(input_extent<=0||kernel_extent<=0)
            tensor_fail(
                "tensor.convolve requires positive extents on convolved axes",
                line,column);
        const auto input_size=static_cast<std::size_t>(input_extent);
        const auto kernel_size=static_cast<std::size_t>(kernel_extent);
        if(kernel_size-1>
           (std::numeric_limits<std::size_t>::max()-1)/dilation)
            tensor_fail("tensor.convolve effective kernel size overflow",line,column);
        const auto effective=std::size_t{1}+(kernel_size-1)*dilation;
        if(padding>(std::numeric_limits<std::size_t>::max()-input_size)/2)
            tensor_fail("tensor.convolve padded size overflow",line,column);
        const auto padded=input_size+padding*2;
        if(effective>padded)
            tensor_fail(
                "tensor.convolve effective kernel is larger than the padded input",
                line,column);
        const auto output_extent=(padded-effective)/stride+1;
        if(output_extent>
           static_cast<std::size_t>(std::numeric_limits<long long>::max()))
            tensor_fail("tensor.convolve output extent overflow",line,column);
        output_shape[leading_rank+axis]=static_cast<long long>(output_extent);
    }

    const auto output_count=tensor_element_count(output_shape,line,column);
    const auto kernel_count=tensor_logical_count(kernel);
    std::vector<long long> expanded_shape=output_shape;
    expanded_shape.insert(
        expanded_shape.end(),kernel.shape.begin(),kernel.shape.end());
    const auto expanded_count=tensor_element_count(expanded_shape,line,column);
    if(kernel_count!=0&&output_count>
       std::numeric_limits<std::size_t>::max()/kernel_count)
        tensor_fail("tensor.convolve expanded size overflow",line,column);
    if(expanded_count!=output_count*kernel_count)
        tensor_fail("tensor.convolve expanded size mismatch",line,column);

    std::vector<std::size_t> source_indices;
    std::vector<std::size_t> kernel_indices;
    std::vector<unsigned char> valid_mask;
    try{
        source_indices.resize(expanded_count);
        kernel_indices.resize(expanded_count);
        valid_mask.assign(expanded_count,1);
    }catch(...){
        runtime_allocation_failure();
    }

    std::vector<std::size_t> output_coordinates(rank);
    std::vector<std::size_t> kernel_coordinates(kernel_rank);
    bool has_padding_gap=false;
    for(std::size_t linear=0;linear<expanded_count;++linear){
        const auto kernel_linear=kernel_count==0?0:linear%kernel_count;
        auto remaining_kernel=kernel_linear;
        for(std::size_t axis=kernel_rank;axis-- >0;){
            const auto extent=static_cast<std::size_t>(kernel.shape[axis]);
            kernel_coordinates[axis]=remaining_kernel%extent;
            remaining_kernel/=extent;
        }

        auto remaining_output=kernel_count==0?0:linear/kernel_count;
        for(std::size_t axis=rank;axis-- >0;){
            const auto extent=static_cast<std::size_t>(output_shape[axis]);
            if(extent==0){
                output_coordinates[axis]=0;
            }else{
                output_coordinates[axis]=remaining_output%extent;
                remaining_output/=extent;
            }
        }

        std::size_t source_linear=0;
        bool valid=true;
        for(std::size_t axis=0;axis<rank;++axis){
            const auto input_extent=
                static_cast<std::size_t>(source.shape[axis]);
            std::size_t coordinate=output_coordinates[axis];
            if(axis>=leading_rank){
                const auto kernel_axis=axis-leading_rank;
                const auto kernel_extent=
                    static_cast<std::size_t>(kernel.shape[kernel_axis]);
                const auto reversed=
                    (kernel_extent-1-kernel_coordinates[kernel_axis])*dilation;
                const auto padded_coordinate=
                    output_coordinates[axis]*stride+reversed;
                if(padded_coordinate<padding){
                    valid=false;
                }else{
                    coordinate=padded_coordinate-padding;
                    if(coordinate>=input_extent) valid=false;
                }
            }
            if(!valid) break;
            source_linear=source_linear*input_extent+coordinate;
        }
        source_indices[linear]=valid?source_linear:0;
        kernel_indices[linear]=kernel_linear;
        if(!valid){
            valid_mask[linear]=0;
            has_padding_gap=true;
        }
    }

    auto* gathered_source=tensor_gather_logical_indices(
        source,std::move(source_indices),expanded_shape,line,column);
    auto* gathered_kernel=tensor_gather_logical_indices(
        kernel,std::move(kernel_indices),expanded_shape,line,column);
    auto* current=static_cast<TensorValue*>(
        quidra_tensor_binary(
            gathered_source,gathered_kernel,nullptr,0,3,line,column));
    release_managed_tensor(gathered_source);
    release_managed_tensor(gathered_kernel);

    if(has_padding_gap){
        const auto width=tensor_dtype_bytes(source.storage->dtype);
        if(expanded_count>std::numeric_limits<std::size_t>::max()/width){
            release_managed_tensor(current);
            tensor_fail("tensor.convolve mask size overflow",line,column);
        }
        std::vector<unsigned char> mask_bytes;
        try{
            mask_bytes.resize(expanded_count*width);
        }catch(...){
            release_managed_tensor(current);
            runtime_allocation_failure();
        }
        const auto store_mask_value=[&](std::size_t index,auto value){
            std::memcpy(
                mask_bytes.data()+index*width,&value,sizeof(value));
        };
        for(std::size_t i=0;i<expanded_count;++i){
            const bool valid=valid_mask[i]!=0;
            switch(source.storage->dtype){
                case 1: store_mask_value(i,std::int64_t(valid?1:0)); break;
                case 2: store_mask_value(i,std::int8_t(valid?1:0)); break;
                case 3: store_mask_value(i,std::int16_t(valid?1:0)); break;
                case 4: store_mask_value(i,std::int32_t(valid?1:0)); break;
                case 5: store_mask_value(i,std::uint8_t(valid?1:0)); break;
                case 6: store_mask_value(i,std::uint16_t(valid?1:0)); break;
                case 7: store_mask_value(i,std::uint32_t(valid?1:0)); break;
                case 8: store_mask_value(i,std::uint64_t(valid?1:0)); break;
                case 9: store_mask_value(i,valid?1.0:0.0); break;
                case 10: store_mask_value(i,valid?1.0f:0.0f); break;
                default:
                    release_managed_tensor(current);
                    tensor_fail(
                        "tensor.convolve requires a numeric tensor element type",
                        line,column);
            }
        }
        auto* mask_storage=tensor_storage_create(
            source.storage->dtype,expanded_count,1,source.storage->device,
            line,column);
        if(tensor_on_cpu(*source.storage)){
            if(!mask_bytes.empty())
                std::memcpy(
                    mask_storage->data.data(),mask_bytes.data(),mask_bytes.size());
        }else{
            std::string backend_error;
            if(!mask_bytes.empty()&&!quidra::device::copy_from_host(
                    mask_storage->gpu_buffer,0,mask_bytes.data(),
                    mask_bytes.size(),backend_error)){
                tensor_storage_release(mask_storage);
                release_managed_tensor(current);
                tensor_fail(backend_error.c_str(),line,column);
            }
        }
        auto* mask=tensor_descriptor(
            mask_storage,expanded_shape,
            tensor_contiguous_strides(expanded_shape),0);
        auto* masked=static_cast<TensorValue*>(
            quidra_tensor_binary(current,mask,nullptr,0,3,line,column));
        release_managed_tensor(current);
        release_managed_tensor(mask);
        current=masked;
    }

    auto reduction_shape=expanded_shape;
    for(std::size_t reduced=0;reduced<kernel_rank;++reduced){
        const auto width=static_cast<std::size_t>(reduction_shape.back());
        reduction_shape.pop_back();
        const auto collapsed_count=
            tensor_element_count(reduction_shape,line,column);
        if(source.storage->dtype==9||source.storage->dtype==10){
            auto* summed=static_cast<TensorValue*>(
                quidra_tensor_autograd_unary(current,5,line,column));
            release_managed_tensor(current);
            std::vector<std::size_t> collapsed_indices;
            try{
                collapsed_indices.resize(collapsed_count);
            }catch(...){
                release_managed_tensor(summed);
                runtime_allocation_failure();
            }
            for(std::size_t i=0;i<collapsed_count;++i)
                collapsed_indices[i]=i*width;
            current=tensor_gather_logical_indices(
                *summed,std::move(collapsed_indices),reduction_shape,line,column);
            release_managed_tensor(summed);
            continue;
        }

        TensorValue* summed=nullptr;
        for(std::size_t offset=0;offset<width;++offset){
            std::vector<std::size_t> indices;
            try{
                indices.resize(collapsed_count);
            }catch(...){
                if(summed) release_managed_tensor(summed);
                release_managed_tensor(current);
                runtime_allocation_failure();
            }
            for(std::size_t i=0;i<collapsed_count;++i)
                indices[i]=i*width+offset;
            auto* slice=tensor_gather_logical_indices(
                *current,std::move(indices),reduction_shape,line,column);
            if(!summed){
                summed=slice;
                continue;
            }
            auto* next=static_cast<TensorValue*>(
                quidra_tensor_binary(summed,slice,nullptr,0,1,line,column));
            release_managed_tensor(summed);
            release_managed_tensor(slice);
            summed=next;
        }
        release_managed_tensor(current);
        current=summed;
    }
    return current;
}

template <typename T>
TensorValue* tensor_max_last_cpu(
    const TensorValue& input,unsigned long long line,unsigned long long column) {
    if(input.shape.empty()||input.shape.back()<=0)
        tensor_fail("max_last requires a non-empty last axis",line,column);
    const auto count=tensor_logical_count(input);
    const auto width=static_cast<std::size_t>(input.shape.back());
    auto* storage=tensor_storage_create(input.storage->dtype,count,1,-1,line,column);
    for(std::size_t base=0;base<count;base+=width){
        const auto first_index=tensor_storage_index(input,base);
        T reduced{};
        std::memcpy(&reduced,input.storage->data.data()+first_index*sizeof(T),sizeof(T));
        for(std::size_t j=1;j<width;++j){
            const auto source_index=tensor_storage_index(input,base+j);
            T value{};
            std::memcpy(&value,input.storage->data.data()+source_index*sizeof(T),sizeof(T));
            if(value>reduced) reduced=value;
        }
        for(std::size_t j=0;j<width;++j)
            std::memcpy(storage->data.data()+(base+j)*sizeof(T),&reduced,sizeof(T));
    }
    return tensor_descriptor(
        storage,input.shape,tensor_contiguous_strides(input.shape),0);
}

TensorValue* tensor_numeric_max_last(
    const TensorValue& input,unsigned long long line,unsigned long long column) {
    tensor_require_initialized(input,line,column);
    if(input.graph)
        autograd_fail("tracked max_last requires a floating dtype",line,column);
    if(!tensor_on_cpu(*input.storage))
        return autograd_tensor_unary_raw_gpu(
            const_cast<TensorValue&>(input),6,line,column);
    switch(input.storage->dtype){
        case 1:return tensor_max_last_cpu<std::int64_t>(input,line,column);
        case 2:return tensor_max_last_cpu<std::int8_t>(input,line,column);
        case 3:return tensor_max_last_cpu<std::int16_t>(input,line,column);
        case 4:return tensor_max_last_cpu<std::int32_t>(input,line,column);
        case 5:return tensor_max_last_cpu<std::uint8_t>(input,line,column);
        case 6:return tensor_max_last_cpu<std::uint16_t>(input,line,column);
        case 7:return tensor_max_last_cpu<std::uint32_t>(input,line,column);
        case 8:return tensor_max_last_cpu<std::uint64_t>(input,line,column);
        default: autograd_fail("max_last requires a numeric tensor dtype",line,column);
    }
}

template <typename T>
TensorValue* tensor_min_last_cpu(
    const TensorValue& input,unsigned long long line,unsigned long long column) {
    if(input.shape.empty()||input.shape.back()<=0)
        tensor_fail("min_last requires a non-empty last axis",line,column);
    const auto count=tensor_logical_count(input);
    const auto width=static_cast<std::size_t>(input.shape.back());
    auto* storage=tensor_storage_create(input.storage->dtype,count,1,-1,line,column);
    for(std::size_t base=0;base<count;base+=width){
        const auto first_index=tensor_storage_index(input,base);
        T reduced{};
        std::memcpy(&reduced,input.storage->data.data()+first_index*sizeof(T),sizeof(T));
        for(std::size_t j=1;j<width;++j){
            const auto source_index=tensor_storage_index(input,base+j);
            T value{};
            std::memcpy(&value,input.storage->data.data()+source_index*sizeof(T),sizeof(T));
            if(value<reduced) reduced=value;
        }
        for(std::size_t j=0;j<width;++j)
            std::memcpy(storage->data.data()+(base+j)*sizeof(T),&reduced,sizeof(T));
    }
    return tensor_descriptor(
        storage,input.shape,tensor_contiguous_strides(input.shape),0);
}

TensorValue* tensor_numeric_min_last(
    const TensorValue& input,unsigned long long line,unsigned long long column) {
    tensor_require_initialized(input,line,column);
    if(input.graph)
        autograd_fail("tracked min_last is not supported; call untrack() explicitly",line,column);
    if(!tensor_on_cpu(*input.storage))
        return autograd_tensor_unary_raw_gpu(
            const_cast<TensorValue&>(input),7,line,column);
    switch(input.storage->dtype){
        case 1:return tensor_min_last_cpu<std::int64_t>(input,line,column);
        case 2:return tensor_min_last_cpu<std::int8_t>(input,line,column);
        case 3:return tensor_min_last_cpu<std::int16_t>(input,line,column);
        case 4:return tensor_min_last_cpu<std::int32_t>(input,line,column);
        case 5:return tensor_min_last_cpu<std::uint8_t>(input,line,column);
        case 6:return tensor_min_last_cpu<std::uint16_t>(input,line,column);
        case 7:return tensor_min_last_cpu<std::uint32_t>(input,line,column);
        case 8:return tensor_min_last_cpu<std::uint64_t>(input,line,column);
        default: autograd_fail("min_last requires a numeric tensor dtype",line,column);
    }
}

extern "C" void* quidra_tensor_autograd_unary(void* raw,int op,unsigned long long line,unsigned long long column) {
    if(!raw)autograd_fail("null tensor",line,column);
    auto& input=*static_cast<TensorValue*>(raw);
    if(input.storage->dtype!=9&&input.storage->dtype!=10){
        if(op==6) return tensor_numeric_max_last(input,line,column);
        if(op==7) return tensor_numeric_min_last(input,line,column);
        autograd_fail("autograd tensor unary operation requires a floating dtype",line,column);
    }
    const auto base=input.graph?input.graph:autograd_constant_node(input,line,column);
    const auto transformed=autograd_unary_node(base,op,line,column);
    auto* result=autograd_tensor_from_node(*transformed);
    if(input.graph) result->graph=transformed;
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
        if(owned_) release_managed_tensor(owned_);
    }

    void reset(
        const TensorValue& value,unsigned long long line,unsigned long long column) {
        if(owned_) {
            release_managed_tensor(owned_);
            owned_=nullptr;
        }
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


TensorValue* autograd_device_reshape_view(
    TensorValue* source,const std::vector<long long>& shape,
    unsigned long long line,unsigned long long column) {
    if(!source||!source->storage)
        autograd_fail("invalid GPU matmul reshape operand",line,column);
    if(tensor_element_count(shape,line,column)!=tensor_logical_count(*source))
        autograd_fail("GPU matmul reshape cannot change element count",line,column);
    if(!tensor_is_contiguous_value(*source))
        autograd_fail("GPU matmul reshape requires contiguous storage",line,column);
    if(source->storage->owners==std::numeric_limits<std::size_t>::max())
        runtime_text_failure("tensor storage owner overflow");
    ++source->storage->owners;
    return tensor_descriptor(
        source->storage,shape,tensor_contiguous_strides(shape),source->offset);
}

TensorValue* autograd_device_transpose_view(
    TensorValue* source,unsigned long long line,unsigned long long column) {
    if(!source||source->shape.size()!=2)
        autograd_fail("GPU matmul transpose requires rank-2 tensor",line,column);
    return static_cast<TensorValue*>(
        quidra_tensor_transpose(source,0,1,line,column));
}

TensorValue* autograd_device_binary_tensor(
    TensorValue* left,TensorValue* right,int operation,
    unsigned long long line,unsigned long long column) {
    return static_cast<TensorValue*>(
        quidra_tensor_binary(left,right,nullptr,0,operation,line,column));
}

void autograd_device_binary_backward(
    TensorValue* gradient,TensorValue* left,TensorValue* right,int operation,
    bool shared_parent,TensorValue*& left_gradient,TensorValue*& right_gradient,
    unsigned long long line,unsigned long long column) {
    AutogradDeviceDenseInput gd(*gradient,line,column);
    AutogradDeviceDenseInput ad(*left,line,column);
    AutogradDeviceDenseInput bd(*right,line,column);
    if(gd->shape!=ad->shape||gd->shape!=bd->shape)
        autograd_fail("autograd binary backward shape mismatch",line,column);
    if(shared_parent&&left!=right)
        autograd_fail("shared autograd binary parent mismatch",line,column);
    const auto count=tensor_logical_count(*gd.get());
    auto* left_storage=tensor_storage_create(
        gd->storage->dtype,count,1,gd->storage->device,line,column);
    TensorStorage* right_storage=left_storage;
    left_gradient=tensor_descriptor(
        left_storage,left->shape,tensor_contiguous_strides(left->shape),0);
    right_gradient=nullptr;
    if(!shared_parent){
        right_storage=tensor_storage_create(
            gd->storage->dtype,count,1,gd->storage->device,line,column);
        right_gradient=tensor_descriptor(
            right_storage,right->shape,tensor_contiguous_strides(right->shape),0);
    }

    std::string backend_error;
    const bool ok=quidra::device::compute_binary_backward(
        left_storage->gpu_buffer,right_storage->gpu_buffer,
        gd->storage->gpu_buffer,ad->storage->gpu_buffer,bd->storage->gpu_buffer,
        gd->storage->dtype,operation,count,backend_error);
    if(!ok){
        release_managed_tensor(left_gradient);
        if(right_gradient) release_managed_tensor(right_gradient);
        left_gradient=nullptr;
        right_gradient=nullptr;
        autograd_fail(backend_error.c_str(),line,column);
    }
}

TensorValue* autograd_device_negate_tensor(
    TensorValue* input,unsigned long long line,unsigned long long column) {
    return static_cast<TensorValue*>(
        quidra_tensor_unary(input,1,line,column));
}

bool autograd_accumulate_device_gradient_in_place(
    TensorValue* destination,const TensorValue* source,
    unsigned long long line,unsigned long long column) {
    if(!destination||!source||destination==source||
       !destination->storage||!source->storage)
        return false;
    if(tensor_on_cpu(*destination->storage)||tensor_on_cpu(*source->storage)||
       destination->storage->owners!=1||
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
            nullptr,0,destination->storage->dtype,1,count,backend_error))
        autograd_fail(backend_error.c_str(),line,column);
    return true;
}

void autograd_add_device_gradient(
    std::unordered_map<const AutogradNode*,TensorValue*>& gradients,
    const std::shared_ptr<AutogradNode>& node,TensorValue* value,
    unsigned long long line,unsigned long long column) {
    if(!value) autograd_fail("null GPU autograd gradient",line,column);
    const auto found=gradients.find(node.get());
    if(found==gradients.end()){
        gradients.emplace(node.get(),value);
        return;
    }
    if(autograd_accumulate_device_gradient_in_place(
            found->second,value,line,column)){
        release_managed_tensor(value);
        return;
    }
    auto* combined=autograd_device_binary_tensor(
        found->second,value,1,line,column);
    release_managed_tensor(found->second);
    release_managed_tensor(value);
    found->second=combined;
}

TensorValue* autograd_device_filled_like(
    const AutogradNode& node,bool ones,
    unsigned long long line,unsigned long long column) {
    if(!node.device_tensor)
        autograd_fail("internal autograd GPU fill requires a device tensor",line,column);
    const auto count=tensor_logical_count(*node.device_tensor);
    auto* storage=tensor_storage_create(
        node.dtype,count,ones?2:1,node.device_tensor->storage->device,line,column);
    auto strides=tensor_contiguous_strides(node.shape);
    return tensor_descriptor(storage,node.shape,std::move(strides),0);
}

void autograd_grad_device(
    const std::shared_ptr<AutogradNode>& loss,
    const std::vector<std::shared_ptr<AutogradSlot>>& selected,
    unsigned long long line,unsigned long long column) {
    std::unordered_set<const AutogradNode*> seen;
    std::vector<std::shared_ptr<AutogradNode>> order;
    seen.reserve(64);
    order.reserve(64);
    autograd_topological(loss,seen,order);

    if(!loss->device_tensor||tensor_logical_count(*loss->device_tensor)!=1)
        autograd_fail("grad requires a scalar GPU loss",line,column);

    std::unordered_map<const AutogradNode*,TensorValue*> gradients;
    gradients.reserve(order.size());
    auto* initial=autograd_device_filled_like(*loss,true,line,column);
    gradients.emplace(loss.get(),initial);

    for(auto it=order.rbegin();it!=order.rend();++it){
        const auto& node=*it;
        if(node->dtype!=loss->dtype)
            autograd_fail("autograd graph contains mixed dtypes",line,column);
        const auto found=gradients.find(node.get());
        if(found==gradients.end()) continue;
        auto* g=found->second;

        if(node->target_identity){
            for(const auto& slot:selected){
                if(!slot || slot->identity!=node->target_identity) continue;
                autograd_accumulate_slot(
                    slot,static_cast<TensorValue*>(quidra_tensor_clone(g)),line,column);
            }
        }
        if(node->parents.empty()){
            release_managed_tensor(g);
            gradients.erase(node.get());
            continue;
        }

        if(node->op==AutogradOp::Add||node->op==AutogradOp::Sub||
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
            if(shared_parent){
                // Produce the sum of both partial derivatives directly. This
                // turns x+x / x*x style backward from two gradient tensors plus
                // an accumulation kernel into one gradient tensor and one kernel.
                autograd_device_binary_backward(
                    g,a,b,operation,true,left_gradient,right_gradient,line,column);
                autograd_add_device_gradient(
                    gradients,node->parents[0],left_gradient,line,column);
            }else if(node->op==AutogradOp::Add){
                right_gradient=static_cast<TensorValue*>(quidra_tensor_clone(g));
                left_gradient=g;
                g=nullptr;
                autograd_add_device_gradient(
                    gradients,node->parents[0],left_gradient,line,column);
                autograd_add_device_gradient(
                    gradients,node->parents[1],right_gradient,line,column);
            }else if(node->op==AutogradOp::Sub){
                right_gradient=autograd_device_negate_tensor(g,line,column);
                left_gradient=g;
                g=nullptr;
                autograd_add_device_gradient(
                    gradients,node->parents[0],left_gradient,line,column);
                autograd_add_device_gradient(
                    gradients,node->parents[1],right_gradient,line,column);
            }else{
                autograd_device_binary_backward(
                    g,a,b,operation,false,
                    left_gradient,right_gradient,line,column);
                autograd_add_device_gradient(
                    gradients,node->parents[0],left_gradient,line,column);
                autograd_add_device_gradient(
                    gradients,node->parents[1],right_gradient,line,column);
            }
        }else if(node->op==AutogradOp::ScalarBinary){
            if(node->parents.size()!=1||node->aux.size()!=1||
               node->aux_index.size()!=2)
                autograd_fail("invalid GPU autograd scalar graph",line,column);
            const auto& input=node->parents[0];
            const auto operation=static_cast<int>(node->aux_index[0]);
            const bool scalar_left=node->aux_index[1]!=0;
            TensorValue* result=nullptr;
            if(operation==1||(operation==2&&!scalar_left)){
                result=g;
                g=nullptr;
            }else if(operation==2){
                result=autograd_device_negate_tensor(g,line,column);
            }else{
                AutogradDeviceDenseInput gd(*g,line,column);
                AutogradDeviceDenseInput xd;
                const TensorValue* input_dense=gd.get();
                if(operation==4&&scalar_left){
                    xd.reset(*input->device_tensor,line,column);
                    input_dense=xd.get();
                    if(gd->shape!=xd->shape)
                        autograd_fail("autograd scalar backward shape mismatch",line,column);
                }
                const auto count=tensor_logical_count(*gd.get());
                auto* storage=tensor_storage_create(
                    node->dtype,count,1,gd->storage->device,line,column);
                result=tensor_descriptor(
                    storage,input->shape,tensor_contiguous_strides(input->shape),0);
                std::string backend_error;
                const bool ok=quidra::device::compute_scalar_backward(
                    storage->gpu_buffer,gd->storage->gpu_buffer,
                    input_dense->storage->gpu_buffer,node->dtype,operation,
                    scalar_left,node->aux.scalar_as_double(0),count,backend_error);
                if(!ok){
                    release_managed_tensor(result);
                    autograd_fail(backend_error.c_str(),line,column);
                }
            }
            autograd_add_device_gradient(
                gradients,input,result,line,column);
        }else if(node->op==AutogradOp::Absolute){
            const auto& input=node->parents[0];
            AutogradDeviceDenseInput gd(*g,line,column);
            AutogradDeviceDenseInput xd(*input->device_tensor,line,column);
            const auto count=tensor_logical_count(*xd.get());
            auto* storage=tensor_storage_create(
                node->dtype,count,1,xd->storage->device,line,column);
            auto strides=tensor_contiguous_strides(input->shape);
            auto* result=tensor_descriptor(storage,input->shape,std::move(strides),0);
            std::string backend_error;
            const bool ok=quidra::device::compute_abs_backward(
                storage->gpu_buffer,gd->storage->gpu_buffer,xd->storage->gpu_buffer,
                node->dtype,count,backend_error);
            if(!ok){
                release_managed_tensor(result);
                autograd_fail(backend_error.c_str(),line,column);
            }
            autograd_add_device_gradient(
                gradients,input,result,line,column);
        }else if(node->op==AutogradOp::Exponential){
            auto* result=autograd_device_binary_tensor(
                g,node->device_tensor,3,line,column);
            autograd_add_device_gradient(
                gradients,node->parents[0],result,line,column);
        }else if(node->op==AutogradOp::Logarithm){
            auto* result=autograd_device_binary_tensor(
                g,node->parents[0]->device_tensor,4,line,column);
            autograd_add_device_gradient(
                gradients,node->parents[0],result,line,column);
        }else if(node->op==AutogradOp::Mean){
            const auto& input=node->parents[0];
            const auto count=autograd_node_count(*input);
            AutogradDeviceDenseInput gd(*g,line,column);
            auto* storage=tensor_storage_create(
                node->dtype,count,1,input->device_tensor->storage->device,line,column);
            auto strides=tensor_contiguous_strides(input->shape);
            auto* result=tensor_descriptor(storage,input->shape,std::move(strides),0);
            std::string backend_error;
            const bool ok=quidra::device::compute_mean_backward(
                storage->gpu_buffer,gd->storage->gpu_buffer,node->dtype,count,backend_error);
            if(!ok){
                release_managed_tensor(result);
                autograd_fail(backend_error.c_str(),line,column);
            }
            autograd_add_device_gradient(
                gradients,input,result,line,column);
        }else if(node->op==AutogradOp::SumLast){
            const auto& input=node->parents[0];
            const auto count=autograd_node_count(*input);
            const auto width=static_cast<std::size_t>(input->shape.back());
            AutogradDeviceDenseInput gd(*g,line,column);
            auto* storage=tensor_storage_create(
                node->dtype,count,1,input->device_tensor->storage->device,line,column);
            auto strides=tensor_contiguous_strides(input->shape);
            auto* result=tensor_descriptor(storage,input->shape,std::move(strides),0);
            std::string backend_error;
            const bool ok=quidra::device::compute_last_reduce_broadcast(
                storage->gpu_buffer,gd->storage->gpu_buffer,node->dtype,
                count,width,1,backend_error);
            if(!ok){
                release_managed_tensor(result);
                autograd_fail(backend_error.c_str(),line,column);
            }
            autograd_add_device_gradient(
                gradients,input,result,line,column);
        }else if(node->op==AutogradOp::MaxLast || node->op==AutogradOp::MinLast){
            const auto& input=node->parents[0];
            const auto count=autograd_node_count(*input);
            const auto width=static_cast<std::size_t>(input->shape.back());
            AutogradDeviceDenseInput gd(*g,line,column);
            AutogradDeviceDenseInput xd(*input->device_tensor,line,column);
            auto* storage=tensor_storage_create(
                node->dtype,count,1,input->device_tensor->storage->device,line,column);
            auto strides=tensor_contiguous_strides(input->shape);
            auto* result=tensor_descriptor(storage,input->shape,std::move(strides),0);
            std::string backend_error;
            const bool ok=node->op==AutogradOp::MaxLast
                ? quidra::device::compute_max_last_backward(
                    storage->gpu_buffer,gd->storage->gpu_buffer,xd->storage->gpu_buffer,
                    node->dtype,count,width,backend_error)
                : quidra::device::compute_min_last_backward(
                    storage->gpu_buffer,gd->storage->gpu_buffer,xd->storage->gpu_buffer,
                    node->dtype,count,width,backend_error);
            if(!ok){
                release_managed_tensor(result);
                autograd_fail(backend_error.c_str(),line,column);
            }
            autograd_add_device_gradient(
                gradients,input,result,line,column);
        }else if(node->op==AutogradOp::Matmul){
            if(node->parents.size()!=2||
               !node->parents[0]->device_tensor||!node->parents[1]->device_tensor)
                autograd_fail("invalid GPU matmul graph",line,column);
            auto* left=node->parents[0]->device_tensor;
            auto* right=node->parents[1]->device_tensor;
            if(left->shape.empty()||(right->shape.size()!=1&&right->shape.size()!=2))
                autograd_fail("invalid GPU matmul shape",line,column);
            std::vector<long long> leading(left->shape.begin(),left->shape.end()-1);
            const auto rows=tensor_element_count(leading,line,column);
            const auto inner=left->shape.back();
            const auto columns=right->shape.size()==1?1:right->shape[1];

            AutogradDeviceDenseInput ld(*left,line,column);
            AutogradDeviceDenseInput rd(*right,line,column);
            AutogradDeviceDenseInput gd(*g,line,column);
            auto* l2=autograd_device_reshape_view(
                const_cast<TensorValue*>(ld.get()),
                {static_cast<long long>(rows),inner},line,column);
            auto* r2=autograd_device_reshape_view(
                const_cast<TensorValue*>(rd.get()),{inner,columns},line,column);
            auto* g2=autograd_device_reshape_view(
                const_cast<TensorValue*>(gd.get()),
                {static_cast<long long>(rows),columns},line,column);

            auto* rt=autograd_device_transpose_view(r2,line,column);
            auto* da2=static_cast<TensorValue*>(
                quidra_linear_matmul(g2,rt,line,column));
            auto* da=autograd_device_reshape_view(da2,left->shape,line,column);
            auto* lt=autograd_device_transpose_view(l2,line,column);
            auto* db2=static_cast<TensorValue*>(
                quidra_linear_matmul(lt,g2,line,column));
            auto* db=autograd_device_reshape_view(db2,right->shape,line,column);

            release_managed_tensor(l2);
            release_managed_tensor(r2);
            release_managed_tensor(g2);
            release_managed_tensor(rt);
            release_managed_tensor(lt);
            release_managed_tensor(da2);
            release_managed_tensor(db2);
            autograd_add_device_gradient(
                gradients,node->parents[0],da,line,column);
            autograd_add_device_gradient(
                gradients,node->parents[1],db,line,column);
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
            auto* result=tensor_descriptor(
                gd->storage,parent->shape,tensor_contiguous_strides(parent->shape),gd->offset);
            autograd_add_device_gradient(gradients,node->parents[0],result,line,column);
        }else if(node->op==AutogradOp::Transpose){
            if(node->parents.size()!=1||node->aux_index.size()!=2||
               !node->parents[0]->device_tensor)
                autograd_fail("invalid GPU tensor transpose graph",line,column);
            auto* parent=node->parents[0]->device_tensor;
            if(g->storage->owners==std::numeric_limits<std::size_t>::max())
                runtime_text_failure("tensor storage owner overflow");
            ++g->storage->owners;
            auto shape=g->shape;
            auto strides=g->strides;
            const auto axis0=node->aux_index[0],axis1=node->aux_index[1];
            if(axis0>=shape.size()||axis1>=shape.size())
                autograd_fail("invalid GPU tensor transpose axis",line,column);
            std::swap(shape[axis0],shape[axis1]);
            std::swap(strides[axis0],strides[axis1]);
            auto* result=tensor_descriptor(g->storage,std::move(shape),std::move(strides),g->offset);
            if(result->shape!=parent->shape){
                release_managed_tensor(result);
                autograd_fail("GPU tensor transpose backward shape mismatch",line,column);
            }
            autograd_add_device_gradient(gradients,node->parents[0],result,line,column);
        }else if(node->op==AutogradOp::Gather){
            if(node->parents.size()!=1||!node->parents[0]->device_tensor)
                autograd_fail("invalid GPU tensor gather graph",line,column);
            const auto& input=node->parents[0];
            const auto input_count=autograd_node_count(*input);
            AutogradDeviceDenseInput gd(*g,line,column);
            auto* storage=tensor_storage_create(
                node->dtype,input_count,1,input->device_tensor->storage->device,
                line,column);
            auto* result=tensor_descriptor(
                storage,input->shape,tensor_contiguous_strides(input->shape),0);
            std::vector<std::uint64_t> indices(node->aux_index.size());
            for(std::size_t i=0;i<indices.size();++i)
                indices[i]=static_cast<std::uint64_t>(node->aux_index[i]);
            std::string backend_error;
            const bool ok=quidra::device::compute_gather_backward(
                storage->gpu_buffer,gd->storage->gpu_buffer,node->dtype,
                indices.data(),input_count,indices.size(),backend_error);
            if(!ok){
                release_managed_tensor(result);
                autograd_fail(backend_error.c_str(),line,column);
            }
            autograd_add_device_gradient(gradients,input,result,line,column);
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
                    autograd_fail("GPU tensor scatter index is outside gradient",line,column);
                indices[i]=static_cast<std::uint64_t>(node->aux_index[i]);
            }
            auto* storage=tensor_storage_create(
                node->dtype,input_count,1,input->device_tensor->storage->device,
                line,column);
            auto* result=tensor_descriptor(
                storage,input->shape,tensor_contiguous_strides(input->shape),0);
            std::string backend_error;
            const bool ok=quidra::device::compute_gather(
                storage->gpu_buffer,gd->storage->gpu_buffer,node->dtype,
                indices.data(),input_count,backend_error);
            if(!ok){
                release_managed_tensor(result);
                autograd_fail(backend_error.c_str(),line,column);
            }
            autograd_add_device_gradient(gradients,input,result,line,column);
        }

        // Every child has already contributed in reverse-topological order.
        // Release this gradient now instead of retaining the entire backward pass.
        if(g) release_managed_tensor(g);
        gradients.erase(node.get());
    }

    for(auto& [_,value]:gradients)
        release_managed_tensor(value);
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
    autograd_topological(loss,seen,order);

    std::unordered_map<const AutogradNode*,std::vector<T>> gradients;
    gradients.reserve(order.size());
    gradients[loss.get()]={T{1}};

    for(auto it=order.rbegin();it!=order.rend();++it){
        const auto& node=*it;
        if(node->dtype!=loss->dtype)
            autograd_fail("autograd graph contains mixed dtypes",line,column);
        const auto found=gradients.find(node.get());
        if(found==gradients.end()) continue;
        const auto& g=found->second;
        const auto& node_values=node->data.typed<T>();

        if(node->target_identity){
            for(const auto& slot:selected){
                if(!slot || slot->identity!=node->target_identity) continue;
                std::vector<T> slot_values(g.begin(),g.end());
                autograd_accumulate_slot(
                    slot,
                    autograd_tensor_from_values(
                        node->dtype,node->shape,AutogradBuffer(std::move(slot_values))),
                    line,column);
            }
        }

        if(node->parents.empty()){
            gradients.erase(node.get());
            continue;
        }

        if(node->op==AutogradOp::Add||node->op==AutogradOp::Sub||
           node->op==AutogradOp::Mul||node->op==AutogradOp::Div){
            const auto& a=node->parents[0]->data.typed<T>();
            const auto& b=node->parents[1]->data.typed<T>();
            const bool shared_parent=node->parents[0].get()==node->parents[1].get();
            if(shared_parent){
                std::vector<T> combined=std::move(found->second);
                for(std::size_t i=0;i<combined.size();++i){
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
                    gradients,node->parents[0],std::move(combined));
            }else{
                std::vector<T> left_gradient,right_gradient;
                if(node->op==AutogradOp::Add){
                    left_gradient=std::move(found->second);
                    right_gradient=left_gradient;
                }else if(node->op==AutogradOp::Sub){
                    left_gradient=std::move(found->second);
                    right_gradient.resize(left_gradient.size());
                    for(std::size_t i=0;i<left_gradient.size();++i)
                        right_gradient[i]=static_cast<T>(-left_gradient[i]);
                }else{
                    left_gradient.resize(g.size());
                    right_gradient.resize(g.size());
                    if(node->op==AutogradOp::Mul){
                        for(std::size_t i=0;i<g.size();++i){
                            left_gradient[i]=static_cast<T>(g[i]*b[i]);
                            right_gradient[i]=static_cast<T>(g[i]*a[i]);
                        }
                    }else{
                        for(std::size_t i=0;i<g.size();++i){
                            left_gradient[i]=static_cast<T>(g[i]/b[i]);
                            right_gradient[i]=static_cast<T>(
                                -static_cast<T>(g[i]*a[i])/
                                static_cast<T>(b[i]*b[i]));
                        }
                    }
                }
                autograd_add_gradient(gradients,node->parents[0],std::move(left_gradient));
                autograd_add_gradient(gradients,node->parents[1],std::move(right_gradient));
            }
        }else if(node->op==AutogradOp::ScalarBinary){
            if(node->parents.size()!=1||node->aux.size()!=1||
               node->aux_index.size()!=2)
                autograd_fail("invalid autograd scalar graph",line,column);
            const auto& input=node->parents[0]->data.typed<T>();
            const auto operation=static_cast<int>(node->aux_index[0]);
            const bool scalar_left=node->aux_index[1]!=0;
            const T scalar_value=static_cast<T>(node->aux.scalar_as_double(0));
            // Scalar backward is one-to-one, so transform the completed gradient
            // buffer in place instead of allocating an equally sized temporary.
            std::vector<T> input_gradient=std::move(found->second);
            for(std::size_t i=0;i<input_gradient.size();++i){
                const T gradient=input_gradient[i];
                if(operation==1)input_gradient[i]=gradient;
                else if(operation==2)
                    input_gradient[i]=scalar_left?static_cast<T>(-gradient):gradient;
                else if(operation==3)
                    input_gradient[i]=static_cast<T>(gradient*scalar_value);
                else if(scalar_left){
                    const T gs=static_cast<T>(gradient*scalar_value);
                    const T xx=static_cast<T>(input[i]*input[i]);
                    input_gradient[i]=static_cast<T>(-static_cast<T>(gs/xx));
                }else{
                    input_gradient[i]=static_cast<T>(gradient/scalar_value);
                }
            }
            autograd_add_gradient(
                gradients,node->parents[0],std::move(input_gradient));
        }else if(node->op==AutogradOp::Absolute||
                 node->op==AutogradOp::Exponential||
                 node->op==AutogradOp::Logarithm){
            const auto& input=node->parents[0]->data.typed<T>();
            std::vector<T> input_gradient=std::move(found->second);
            for(std::size_t i=0;i<input_gradient.size();++i){
                const T gradient=input_gradient[i];
                if(node->op==AutogradOp::Absolute)
                    input_gradient[i]=input[i]>T{0}?gradient:
                        input[i]<T{0}?static_cast<T>(-gradient):T{0};
                else if(node->op==AutogradOp::Exponential)
                    input_gradient[i]=static_cast<T>(gradient*node_values[i]);
                else
                    input_gradient[i]=static_cast<T>(gradient/input[i]);
            }
            autograd_add_gradient(gradients,node->parents[0],std::move(input_gradient));
        }else if(node->op==AutogradOp::Mean){
            const auto count=node->parents[0]->data.size();
            if(count==0) autograd_fail("mean gradient requires at least one element",line,column);
            std::vector<T> input_gradient(count,static_cast<T>(g[0]/static_cast<T>(count)));
            autograd_add_gradient(gradients,node->parents[0],std::move(input_gradient));
        }else if(node->op==AutogradOp::MeanBackward){
            if(g.empty()) autograd_fail("mean backward higher-order gradient is empty",line,column);
            T total=T{0};
            for(const auto value:g) total=static_cast<T>(total+value);
            autograd_add_gradient(
                gradients,node->parents[0],
                std::vector<T>{static_cast<T>(total/static_cast<T>(g.size()))});
        }else if(node->op==AutogradOp::SumLast||
                  node->op==AutogradOp::SumLastBackward||
                  node->op==AutogradOp::MaxLast||
                  node->op==AutogradOp::MinLast){
            const auto& input=node->parents[0]->data.typed<T>();
            const auto width=static_cast<std::size_t>(node->shape.back());
            std::vector<T> input_gradient=std::move(found->second);
            for(std::size_t base=0;base<input_gradient.size();base+=width){
                T total=T{0};
                for(std::size_t j=0;j<width;++j)
                    total=static_cast<T>(total+input_gradient[base+j]);
                if(node->op==AutogradOp::SumLast ||
                   node->op==AutogradOp::SumLastBackward){
                    for(std::size_t j=0;j<width;++j) input_gradient[base+j]=total;
                }else{
                    std::size_t selected_index=0;
                    for(std::size_t j=1;j<width;++j)
                        if(node->op==AutogradOp::MaxLast
                               ? input[base+j]>input[base+selected_index]
                               : input[base+j]<input[base+selected_index])
                            selected_index=j;
                    for(std::size_t j=0;j<width;++j)
                        input_gradient[base+j]=j==selected_index?total:T{0};
                }
            }
            autograd_add_gradient(gradients,node->parents[0],std::move(input_gradient));
        }else if(node->op==AutogradOp::Matmul){
            if(node->parents.size()!=2)
                autograd_fail("invalid matmul graph",line,column);
            const auto& left=node->parents[0];
            const auto& right=node->parents[1];
            if(left->shape.empty()||(right->shape.size()!=1&&right->shape.size()!=2))
                autograd_fail("invalid matmul shape",line,column);
            const auto& a=left->data.typed<T>();
            const auto& b=right->data.typed<T>();
            std::vector<long long> leading(left->shape.begin(),left->shape.end()-1);
            const auto rows=tensor_element_count(leading,line,column);
            const auto inner=static_cast<std::size_t>(left->shape.back());
            const auto columns=right->shape.size()==1
                ?std::size_t{1}:static_cast<std::size_t>(right->shape[1]);
            if(g.size()!=rows*columns||a.size()!=rows*inner||b.size()!=inner*columns)
                autograd_fail("matmul backward size mismatch",line,column);
            std::vector<T> da(a.size(),T{0}),db(b.size(),T{0});
            for(std::size_t i=0;i<rows;++i)
                for(std::size_t j=0;j<columns;++j){
                    const T grad=g[i*columns+j];
                    for(std::size_t k=0;k<inner;++k){
                        da[i*inner+k]=static_cast<T>(
                            da[i*inner+k]+static_cast<T>(grad*b[k*columns+j]));
                        db[k*columns+j]=static_cast<T>(
                            db[k*columns+j]+static_cast<T>(a[i*inner+k]*grad));
                    }
                }
            autograd_add_gradient(gradients,left,std::move(da));
            autograd_add_gradient(gradients,right,std::move(db));
        }else if(node->op==AutogradOp::Reshape){
            if(node->parents.size()!=1)
                autograd_fail("invalid tensor reshape graph",line,column);
            if(g.size()!=node->parents[0]->data.size())
                autograd_fail("tensor reshape backward size mismatch",line,column);
            autograd_add_gradient(
                gradients,node->parents[0],std::vector<T>(g.begin(),g.end()));
        }else if(node->op==AutogradOp::Transpose){
            if(node->parents.size()!=1||node->aux_index.size()!=2)
                autograd_fail("invalid tensor transpose graph",line,column);
            AutogradBuffer gb(std::vector<T>(g.begin(),g.end()));
            auto restored=autograd_transpose_values<T>(
                gb,node->shape,node->aux_index[0],node->aux_index[1],line,column);
            autograd_add_gradient(
                gradients,node->parents[0],
                std::vector<T>(
                    restored.template typed<T>().begin(),
                    restored.template typed<T>().end()));
        }else if(node->op==AutogradOp::Gather){
            if(node->parents.size()!=1)
                autograd_fail("invalid tensor gather graph",line,column);
            auto input_gradient=autograd_gather_backward_values<T>(
                g,autograd_node_count(*node->parents[0]),node->aux_index,
                line,column);
            autograd_add_gradient(
                gradients,node->parents[0],std::move(input_gradient));
        }else if(node->op==AutogradOp::GatherBackward){
            if(node->parents.size()!=1)
                autograd_fail("invalid tensor gather backward graph",line,column);
            auto input_gradient=autograd_gather_values<T>(
                g,node->aux_index,line,column);
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
    if(autograd_node_count(*tensor->graph)!=1)
        autograd_fail("backward() requires a scalar tensor",line,column);
    return tensor;
}

void autograd_backward_selected(
    TensorValue* tensor,const std::vector<std::shared_ptr<AutogradSlot>>& selected,bool track,
    unsigned long long line,unsigned long long column) {
    if(track){
        autograd_backward_tracked(tensor->graph,selected,line,column);
        return;
    }
    if(tensor->graph->device_tensor)
        autograd_grad_device(tensor->graph,selected,line,column);
    else if(tensor->graph->dtype==10)
        autograd_grad_t<float>(tensor->graph,selected,line,column);
    else
        autograd_grad_t<double>(tensor->graph,selected,line,column);
}

extern "C" void quidra_tensor_backward_many(
    void* raw,void** target_raws,const unsigned char* target_kinds,
    unsigned long long target_count,bool track,
    unsigned long long line,unsigned long long column) {
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

std::vector<long long> tensor_broadcast_shape(const TensorValue& left,
                                              const TensorValue& right,
                                              unsigned long long line,
                                              unsigned long long column) {
    if (left.shape.size() != right.shape.size()) {
        tensor_fail("tensor broadcasting requires identical ranks", line, column);
    }
    std::vector<long long> shape(left.shape.size());
    for (std::size_t i = 0; i < shape.size(); ++i) {
        const auto a = left.shape[i];
        const auto b = right.shape[i];
        if (a != b && a != 1 && b != 1) {
            tensor_fail("tensor shapes are not broadcast-compatible", line, column);
        }
        shape[i] = std::max(a, b);
    }
    return shape;
}

std::vector<std::size_t> tensor_broadcast_logical_indices(
    const TensorValue& source,const std::vector<long long>& output_shape,
    unsigned long long line,unsigned long long column) {
    if(source.shape.size()!=output_shape.size())
        tensor_fail("tensor broadcasting requires identical ranks",line,column);
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
        source.storage->dtype, count, 0, source.storage->device, line, column);
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
        tracker_set(output->initialization, logical);
    }
    std::string backend_error;
    if (!quidra::device::compute_gather(
            output->gpu_buffer, source.storage->gpu_buffer,
            source.storage->dtype, indices.data(), count, backend_error)) {
        tensor_storage_release(output);
        tensor_fail(backend_error.c_str(), line, column);
    }
    return output;
}

template <typename T>
bool tensor_add_checked(T a, T b, T& out) {
    if constexpr (std::is_floating_point_v<T>) {
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
    if constexpr (std::is_floating_point_v<T>) {
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
    if constexpr (std::is_floating_point_v<T>) {
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
bool tensor_apply_operator(T left, T right, int operation, T& out) {
    switch (operation) {
        case 1: return tensor_add_checked(left, right, out);
        case 2: return tensor_sub_checked(left, right, out);
        case 3: return tensor_mul_checked(left, right, out);
        case 4:
            if constexpr (std::is_integral_v<T>) {
                if (right == 0) return false;
                if constexpr (std::is_signed_v<T>) {
                    if (left == std::numeric_limits<T>::min() && right == -1) return false;
                }
            }
            out = static_cast<T>(left / right);
            return true;
        case 5:
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
        default:
            return false;
    }
}

template <typename T>
void tensor_binary_typed(const TensorValue& primary, const TensorValue* other,
                         const void* scalar, int scalar_side, int operation,
                         TensorStorage& output,
                         const std::vector<long long>& output_shape,
                         unsigned long long line, unsigned long long column) {
    const auto count = tensor_element_count(output_shape, line, column);
    T scalar_value{};
    if (scalar_side != 0) {
        if (!scalar) tensor_fail("missing tensor scalar operand", line, column);
        std::memcpy(&scalar_value, scalar, sizeof(T));
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
        } else if (scalar_side == 1) {
            left = scalar_value;
            right = primary_value;
        } else {
            left = primary_value;
            right = scalar_value;
        }

        T result{};
        if (!tensor_apply_operator(left, right, operation, result)) {
            tensor_fail(operation == 4 || operation == 5
                            ? "invalid tensor division/remainder or integer overflow"
                            : "tensor integer arithmetic overflow",
                        line, column);
        }
        std::memcpy(output.data.data() + logical * sizeof(T), &result, sizeof(T));
    }
}

} // namespace


extern "C" void* quidra_tensor_unary(void* raw, int operation,
                                      unsigned long long line,
                                      unsigned long long column) {
    if (!raw || operation != 1) {
        tensor_fail("invalid tensor unary operation", line, column);
    }
    auto& source = *static_cast<TensorValue*>(raw);
    const auto dtype = source.storage->dtype;
    if (dtype == 5 || dtype == 6 || dtype == 7 || dtype == 8) {
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
            dtype, count, 1, source.storage->device, line, column);
        std::string backend_error;
        const bool ok = quidra::device::compute_unary(
            output->gpu_buffer, input->gpu_buffer, input_offset,
            dtype, operation, count, backend_error);
        if (materialized) tensor_storage_release(materialized);
        if (!ok) {
            tensor_storage_release(output);
            tensor_fail(backend_error.c_str(), line, column);
        }
        return tensor_descriptor(
            output, source.shape, tensor_contiguous_strides(source.shape), 0);
    }

    auto* output = tensor_storage_create(dtype, count, 1);
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
        case 1: run(std::int64_t{}); break;
        case 2: run(std::int8_t{}); break;
        case 3: run(std::int16_t{}); break;
        case 4: run(std::int32_t{}); break;
        case 9: run(double{}); break;
        case 10: run(float{}); break;
        default:
            tensor_storage_release(output);
            tensor_fail("invalid tensor element type for negation", line, column);
    }
    return tensor_descriptor(
        output, source.shape, tensor_contiguous_strides(source.shape), 0);
}

extern "C" void* quidra_tensor_compare(
    void* primary_raw, void* other_raw, void* scalar, int scalar_side, int operation,
    unsigned long long line, unsigned long long column) {
    if (!primary_raw) tensor_fail("null tensor comparison operand", line, column);
    auto* primary = static_cast<TensorValue*>(primary_raw);
    auto* other = static_cast<TensorValue*>(other_raw);
    if (other && scalar_side != 0) tensor_fail("invalid tensor comparison operands", line, column);
    if (!other && scalar_side != 1 && scalar_side != 2)
        tensor_fail("invalid tensor scalar comparison side", line, column);
    if (other && primary->storage->dtype != other->storage->dtype)
        tensor_fail("tensor comparison requires identical element types", line, column);
    if (other && primary->storage->device != other->storage->device)
        tensor_fail("tensor operands are on different devices; use an explicit .gpu(n) or .cpu() transfer",
                    line, column);
    if (other && primary->shape != other->shape)
        tensor_fail("tensor comparison requires identical shape", line, column);
    if (operation < 1 || operation > 6)
        tensor_fail("invalid tensor comparison operation", line, column);
    tensor_require_initialized(*primary, line, column);
    if (other) tensor_require_initialized(*other, line, column);

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
    auto* output = tensor_storage_create(11, count, 1);
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
            } else if (scalar_side == 1) {
                a = scalar_value; b = primary_value;
            } else {
                a = primary_value; b = scalar_value;
            }
            bool match = false;
            switch (operation) {
                case 1: match = a == b; break;
                case 2: match = a != b; break;
                case 3: match = a < b; break;
                case 4: match = a <= b; break;
                case 5: match = a > b; break;
                case 6: match = a >= b; break;
            }
            output->data[i] = static_cast<unsigned char>(match ? 1 : 0);
        }
    };
    switch (dtype) {
        case 1: run(std::int64_t{}); break;
        case 2: run(std::int8_t{}); break;
        case 3: run(std::int16_t{}); break;
        case 4: run(std::int32_t{}); break;
        case 5: run(std::uint8_t{}); break;
        case 6: run(std::uint16_t{}); break;
        case 7: run(std::uint32_t{}); break;
        case 8: run(std::uint64_t{}); break;
        case 9: run(double{}); break;
        case 10: run(float{}); break;
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

extern "C" bool quidra_tensor_bool_reduce(
    void* raw, bool all, unsigned long long line, unsigned long long column) {
    if (!raw) tensor_fail("null boolean tensor", line, column);
    auto* tensor = static_cast<TensorValue*>(raw);
    if (tensor->storage->dtype != 11)
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

void tensor_attach_binary_graph(
    TensorValue* result,TensorValue* primary,TensorValue* other,
    void* scalar,int scalar_side,int operation,
    unsigned long long line,unsigned long long column) {
    if(!result||!primary) return;
    if(!primary->graph&&(!other||!other->graph)) return;
    if(primary->storage->dtype!=9&&primary->storage->dtype!=10)
        autograd_fail("tracked tensor arithmetic requires a floating dtype",line,column);

    auto graph_operand=[&](TensorValue* operand)->std::shared_ptr<AutogradNode>{
        if(operand->shape==result->shape)
            return operand->graph
                ? operand->graph : autograd_constant_node(*operand,line,column);

        auto indices=tensor_broadcast_logical_indices(
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
        ? (operation==1?AutogradOp::Add:operation==2?AutogradOp::Sub:
           operation==3?AutogradOp::Mul:AutogradOp::Div)
        : AutogradOp::ScalarBinary;
    const auto primary_node=graph_operand(primary);
    if(other){
        const auto other_node=graph_operand(other);
        node->parents={primary_node,other_node};
    }else{
        node->parents={primary_node};
        if(primary->storage->dtype==10){
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
            scalar_side==1?std::size_t{1}:std::size_t{0}
        };
    }
    if(tensor_on_cpu(*result->storage))
        node->data=tensor_float_values(*result,line,column);
    else
        node->device_tensor=static_cast<TensorValue*>(quidra_tensor_clone(result));
    result->graph=std::move(node);
}

extern "C" void* quidra_tensor_binary(void* primary_raw, void* other_raw,
                                        void* scalar, int scalar_side,
                                        int operation,
                                        unsigned long long line,
                                        unsigned long long column) {
    if (!primary_raw) tensor_fail("null tensor operand", line, column);
    auto* primary = static_cast<TensorValue*>(primary_raw);
    auto* other = static_cast<TensorValue*>(other_raw);
    if (other && scalar_side != 0) {
        tensor_fail("invalid tensor binary operands", line, column);
    }
    if (!other && scalar_side != 1 && scalar_side != 2) {
        tensor_fail("invalid tensor scalar operand side", line, column);
    }
    if (other && primary->storage->dtype != other->storage->dtype) {
        tensor_fail("tensor operands must have identical element types", line, column);
    }
    if (other && primary->storage->device != other->storage->device) {
        tensor_fail("tensor operands are on different devices; use an explicit .gpu(n) or .cpu() transfer",
                    line, column);
    }
    const auto output_shape =
        other ? tensor_broadcast_shape(*primary, *other, line, column) : primary->shape;
    const auto count = tensor_element_count(output_shape, line, column);

    if (!tensor_on_cpu(*primary->storage)) {
        tensor_require_initialized(*primary, line, column);
        if (other) tensor_require_initialized(*other, line, column);

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
            primary->storage->dtype, count, 1,
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
        tensor_attach_binary_graph(
            result,primary,other,scalar,scalar_side,operation,line,column);
        return result;
    }

    auto* output = tensor_storage_create(primary->storage->dtype, count, 1);

    switch (primary->storage->dtype) {
        case 1: tensor_binary_typed<std::int64_t>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case 2: tensor_binary_typed<std::int8_t>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case 3: tensor_binary_typed<std::int16_t>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case 4: tensor_binary_typed<std::int32_t>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case 5: tensor_binary_typed<std::uint8_t>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case 6: tensor_binary_typed<std::uint16_t>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case 7: tensor_binary_typed<std::uint32_t>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case 8: tensor_binary_typed<std::uint64_t>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case 9: tensor_binary_typed<double>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        case 10:tensor_binary_typed<float>(*primary,other,scalar,scalar_side,operation,*output,output_shape,line,column); break;
        default:
            delete output;
            tensor_fail("invalid tensor element type", line, column);
    }
    auto* result=tensor_descriptor(
        output, output_shape, tensor_contiguous_strides(output_shape), 0);
    tensor_attach_binary_graph(
        result,primary,other,scalar,scalar_side,operation,line,column);
    return result;
}



namespace {

template <typename T>
long double tensor_sum_typed(const TensorValue& value,
                             unsigned long long line,
                             unsigned long long column) {
    const auto count = tensor_logical_count(value);
    long double sum = 0.0L;
    for (std::size_t i = 0; i < count; ++i) {
        const auto storage_index = tensor_storage_index(value, i);
        if (storage_index >= value.storage->count ||
            !tracker_bit(value.storage->initialization, storage_index)) {
            runtime_uninitialized_failure(line, column);
        }
        T element{};
        std::memcpy(&element,
                    value.storage->data.data() + storage_index * sizeof(T),
                    sizeof(T));
        sum += static_cast<long double>(element);
    }
    return sum;
}

template <typename T>
bool tensor_dense_initialized_bytes(
    const TensorValue& value, const unsigned char*& output) {
    if (!value.storage || !tensor_is_contiguous_value(value) ||
        !value.storage->initialization.fully_initialized ||
        tensor_dtype_bytes(value.storage->dtype) != sizeof(T)) {
        return false;
    }
    const auto count = tensor_logical_count(value);
    if (value.offset > value.storage->count ||
        count > value.storage->count - value.offset) {
        return false;
    }
    output = value.storage->data.data();
    if (count != 0) output += value.offset * sizeof(T);
    return true;
}

template <typename T>
T tensor_dense_load(const unsigned char* data, std::size_t index) {
    T value{};
    std::memcpy(&value, data + index * sizeof(T), sizeof(T));
    return value;
}

template <typename T>
void tensor_matmul_typed(const TensorValue& left, const TensorValue& right,
                         TensorStorage& output,
                         std::size_t m,std::size_t k,std::size_t n,
                         unsigned long long line,
                         unsigned long long column) {
    if constexpr (std::is_floating_point_v<T>) {
        const unsigned char* left_dense{};
        const unsigned char* right_dense{};
        if (tensor_dense_initialized_bytes<T>(left, left_dense) &&
            tensor_dense_initialized_bytes<T>(right, right_dense)) {
            std::vector<T> dense_output(output.count,T{});
            for (std::size_t row=0;row<m;++row)
                for (std::size_t inner=0;inner<k;++inner) {
                    const T a=tensor_dense_load<T>(left_dense,row*k+inner);
                    for(std::size_t column_index=0;column_index<n;++column_index){
                        const auto oi=row*n+column_index;
                        T product{},next{};
                        (void)tensor_mul_checked(
                            a,tensor_dense_load<T>(
                                right_dense,inner*n+column_index),product);
                        (void)tensor_add_checked(dense_output[oi],product,next);
                        dense_output[oi]=next;
                    }
                }
            if(!dense_output.empty())
                std::memcpy(output.data.data(),dense_output.data(),
                            dense_output.size()*sizeof(T));
            return;
        }
    }
    for(std::size_t row=0;row<m;++row)
        for(std::size_t column_index=0;column_index<n;++column_index){
            T accumulator{};
            for(std::size_t inner=0;inner<k;++inner){
                const auto li=tensor_storage_index(left,row*k+inner);
                const auto rlogical=right.shape.size()==1
                    ?inner:inner*n+column_index;
                const auto ri=tensor_storage_index(right,rlogical);
                if(li>=left.storage->count||ri>=right.storage->count||
                   !tracker_bit(left.storage->initialization,li)||
                   !tracker_bit(right.storage->initialization,ri))
                    runtime_uninitialized_failure(line,column);
                T a{},b{},product{},next{};
                std::memcpy(&a,left.storage->data.data()+li*sizeof(T),sizeof(T));
                std::memcpy(&b,right.storage->data.data()+ri*sizeof(T),sizeof(T));
                if(!tensor_mul_checked(a,b,product)||
                   !tensor_add_checked(accumulator,product,next))
                    tensor_fail("linear.matmul integer arithmetic overflow",line,column);
                accumulator=next;
            }
            std::memcpy(output.data.data()+(row*n+column_index)*sizeof(T),
                        &accumulator,sizeof(T));
        }
}


template <typename T>
T tensor_dot_typed(const TensorValue& left, const TensorValue& right,
                   unsigned long long line, unsigned long long column) {
    const auto count = static_cast<std::size_t>(left.shape[0]);

    if constexpr (std::is_floating_point_v<T>) {
        const unsigned char* left_dense{};
        const unsigned char* right_dense{};
        if (tensor_dense_initialized_bytes<T>(left, left_dense) &&
            tensor_dense_initialized_bytes<T>(right, right_dense)) {
            T accumulator{};
            for (std::size_t logical = 0; logical < count; ++logical) {
                T product{};
                T next{};
                (void)tensor_mul_checked(
                    tensor_dense_load<T>(left_dense, logical),
                    tensor_dense_load<T>(right_dense, logical), product);
                (void)tensor_add_checked(accumulator, product, next);
                accumulator = next;
            }
            return accumulator;
        }
    }

    T accumulator{};
    for (std::size_t logical = 0; logical < count; ++logical) {
        const auto left_index = tensor_storage_index(left, logical);
        const auto right_index = tensor_storage_index(right, logical);
        if (left_index >= left.storage->count ||
            right_index >= right.storage->count ||
            !tracker_bit(left.storage->initialization, left_index) ||
            !tracker_bit(right.storage->initialization, right_index)) {
            runtime_uninitialized_failure(line, column);
        }
        T a{};
        T b{};
        std::memcpy(&a, left.storage->data.data() + left_index * sizeof(T), sizeof(T));
        std::memcpy(&b, right.storage->data.data() + right_index * sizeof(T), sizeof(T));
        T product{};
        T next{};
        if (!tensor_mul_checked(a, b, product) ||
            !tensor_add_checked(accumulator, product, next)) {
            tensor_fail("linear.dot integer arithmetic overflow", line, column);
        }
        accumulator = next;
    }
    return accumulator;
}

void validate_linear_dot(const TensorValue& left, const TensorValue& right,
                         int dtype, unsigned long long line,
                         unsigned long long column) {
    if (left.storage->dtype != dtype || right.storage->dtype != dtype) {
        tensor_fail("linear.dot requires identical expected element types", line, column);
    }
    if (left.storage->device != right.storage->device) {
        tensor_fail("linear.dot operands are on different devices; transfer them explicitly",
                    line, column);
    }
    if (left.shape.size() != 1 || right.shape.size() != 1) {
        tensor_fail("linear.dot requires rank-1 tensors", line, column);
    }
    if (left.shape[0] != right.shape[0]) {
        tensor_fail("linear.dot requires equal vector lengths", line, column);
    }
}

} // namespace


extern "C" unsigned long long quidra_linear_dot_integer(
    void* left_raw, void* right_raw, int dtype,
    unsigned long long line, unsigned long long column) {
    if (!left_raw || !right_raw) tensor_fail("linear.dot received a null tensor", line, column);
    auto& left = *static_cast<TensorValue*>(left_raw);
    auto& right = *static_cast<TensorValue*>(right_raw);
    validate_linear_dot(left, right, dtype, line, column);
    if (!tensor_on_cpu(*left.storage)) {
        tensor_require_initialized(left, line, column);
        tensor_require_initialized(right, line, column);
        TensorStorage* left_materialized = nullptr;
        TensorStorage* right_materialized = nullptr;
        const TensorStorage* left_storage = left.storage;
        const TensorStorage* right_storage = right.storage;
        if (!tensor_is_contiguous_value(left) || left.offset != 0) {
            left_materialized = tensor_gpu_materialize_storage(left, line, column);
            left_storage = left_materialized;
        }
        if (!tensor_is_contiguous_value(right) || right.offset != 0) {
            right_materialized = tensor_gpu_materialize_storage(right, line, column);
            right_storage = right_materialized;
        }
        std::array<unsigned char, 8> scalar{};
        std::string backend_error;
        const bool ok = quidra::device::compute_dot(
            left_storage->gpu_buffer, right_storage->gpu_buffer,
            dtype, static_cast<std::size_t>(left.shape[0]),
            scalar.data(), backend_error);
        if (left_materialized) tensor_storage_release(left_materialized);
        if (right_materialized) tensor_storage_release(right_materialized);
        if (!ok) tensor_fail(backend_error.c_str(), line, column);
        switch (dtype) {
            case 1: { std::int64_t v{}; std::memcpy(&v, scalar.data(), 8);
                      return static_cast<unsigned long long>(v); }
            case 2: { std::int8_t v{}; std::memcpy(&v, scalar.data(), 1);
                      return static_cast<unsigned long long>(v); }
            case 3: { std::int16_t v{}; std::memcpy(&v, scalar.data(), 2);
                      return static_cast<unsigned long long>(v); }
            case 4: { std::int32_t v{}; std::memcpy(&v, scalar.data(), 4);
                      return static_cast<unsigned long long>(v); }
            case 5: { std::uint8_t v{}; std::memcpy(&v, scalar.data(), 1);
                      return static_cast<unsigned long long>(v); }
            case 6: { std::uint16_t v{}; std::memcpy(&v, scalar.data(), 2);
                      return static_cast<unsigned long long>(v); }
            case 7: { std::uint32_t v{}; std::memcpy(&v, scalar.data(), 4);
                      return static_cast<unsigned long long>(v); }
            case 8: { std::uint64_t v{}; std::memcpy(&v, scalar.data(), 8); return v; }
            default:
                tensor_fail("linear.dot integer runtime received a non-integer dtype", line, column);
        }
    }
    switch (dtype) {
        case 1: return static_cast<unsigned long long>(
            tensor_dot_typed<std::int64_t>(left, right, line, column));
        case 2: return static_cast<unsigned long long>(
            tensor_dot_typed<std::int8_t>(left, right, line, column));
        case 3: return static_cast<unsigned long long>(
            tensor_dot_typed<std::int16_t>(left, right, line, column));
        case 4: return static_cast<unsigned long long>(
            tensor_dot_typed<std::int32_t>(left, right, line, column));
        case 5: return static_cast<unsigned long long>(
            tensor_dot_typed<std::uint8_t>(left, right, line, column));
        case 6: return static_cast<unsigned long long>(
            tensor_dot_typed<std::uint16_t>(left, right, line, column));
        case 7: return static_cast<unsigned long long>(
            tensor_dot_typed<std::uint32_t>(left, right, line, column));
        case 8: return tensor_dot_typed<std::uint64_t>(left, right, line, column);
        default:
            tensor_fail("linear.dot integer runtime received a non-integer dtype", line, column);
    }
}

extern "C" float quidra_linear_dot_float32(
    void* left_raw, void* right_raw,
    unsigned long long line, unsigned long long column) {
    if (!left_raw || !right_raw) tensor_fail("linear.dot received a null tensor", line, column);
    auto& left = *static_cast<TensorValue*>(left_raw);
    auto& right = *static_cast<TensorValue*>(right_raw);
    validate_linear_dot(left, right, 10, line, column);
    if (!tensor_on_cpu(*left.storage)) {
        tensor_require_initialized(left, line, column);
        tensor_require_initialized(right, line, column);
        TensorStorage* left_materialized = nullptr;
        TensorStorage* right_materialized = nullptr;
        const TensorStorage* left_storage = left.storage;
        const TensorStorage* right_storage = right.storage;
        if (!tensor_is_contiguous_value(left) || left.offset != 0) {
            left_materialized = tensor_gpu_materialize_storage(left, line, column);
            left_storage = left_materialized;
        }
        if (!tensor_is_contiguous_value(right) || right.offset != 0) {
            right_materialized = tensor_gpu_materialize_storage(right, line, column);
            right_storage = right_materialized;
        }
        float result{};
        std::string backend_error;
        const bool ok = quidra::device::compute_dot(
            left_storage->gpu_buffer, right_storage->gpu_buffer,
            10, static_cast<std::size_t>(left.shape[0]), &result, backend_error);
        if (left_materialized) tensor_storage_release(left_materialized);
        if (right_materialized) tensor_storage_release(right_materialized);
        if (!ok) tensor_fail(backend_error.c_str(), line, column);
        return result;
    }
    return tensor_dot_typed<float>(left, right, line, column);
}

extern "C" double quidra_linear_dot_float64(
    void* left_raw, void* right_raw,
    unsigned long long line, unsigned long long column) {
    if (!left_raw || !right_raw) tensor_fail("linear.dot received a null tensor", line, column);
    auto& left = *static_cast<TensorValue*>(left_raw);
    auto& right = *static_cast<TensorValue*>(right_raw);
    validate_linear_dot(left, right, 9, line, column);
    if (!tensor_on_cpu(*left.storage)) {
        tensor_require_initialized(left, line, column);
        tensor_require_initialized(right, line, column);
        TensorStorage* left_materialized = nullptr;
        TensorStorage* right_materialized = nullptr;
        const TensorStorage* left_storage = left.storage;
        const TensorStorage* right_storage = right.storage;
        if (!tensor_is_contiguous_value(left) || left.offset != 0) {
            left_materialized = tensor_gpu_materialize_storage(left, line, column);
            left_storage = left_materialized;
        }
        if (!tensor_is_contiguous_value(right) || right.offset != 0) {
            right_materialized = tensor_gpu_materialize_storage(right, line, column);
            right_storage = right_materialized;
        }
        double result{};
        std::string backend_error;
        const bool ok = quidra::device::compute_dot(
            left_storage->gpu_buffer, right_storage->gpu_buffer,
            9, static_cast<std::size_t>(left.shape[0]), &result, backend_error);
        if (left_materialized) tensor_storage_release(left_materialized);
        if (right_materialized) tensor_storage_release(right_materialized);
        if (!ok) tensor_fail(backend_error.c_str(), line, column);
        return result;
    }
    return tensor_dot_typed<double>(left, right, line, column);
}


extern "C" void* quidra_stats_reduce_ptr(
    void* raw, int dtype, int operation,
    unsigned long long line, unsigned long long column) {
    if (!raw || operation < 1 || operation > 3) {
        tensor_fail("invalid stats reduction", line, column);
    }
    auto& value = *static_cast<TensorValue*>(raw);
    if (value.storage->dtype != dtype) {
        tensor_fail("stats reduction dtype mismatch", line, column);
    }
    const auto count = tensor_logical_count(value);
    if (count == 0 && operation != 1) {
        tensor_fail(
            operation == 2
                ? "stats.min is undefined for an empty tensor"
                : "stats.max is undefined for an empty tensor",
            line, column);
    }
    tensor_require_initialized(value, line, column);
    static thread_local std::array<unsigned char, 8> result{};
    std::fill(result.begin(), result.end(), 0);

    if (!tensor_on_cpu(*value.storage)) {
        TensorStorage* materialized = nullptr;
        const TensorStorage* input = value.storage;
        if (!tensor_is_contiguous_value(value) || value.offset != 0) {
            materialized = tensor_gpu_materialize_storage(value, line, column);
            input = materialized;
        }
        std::string backend_error;
        const bool ok = quidra::device::compute_reduce(
            input->gpu_buffer, dtype, operation, count,
            result.data(), backend_error);
        if (materialized) tensor_storage_release(materialized);
        if (!ok) tensor_fail(backend_error.c_str(), line, column);
        return result.data();
    }

    auto run = [&](auto tag) {
        using T = decltype(tag);
        T reduced{};
        if (operation != 1 && count != 0) {
            const auto first_index = tensor_storage_index(value, 0);
            std::memcpy(
                &reduced,
                value.storage->data.data() + first_index * sizeof(T),
                sizeof(T));
        }
        const std::size_t start = operation == 1 ? 0 : 1;
        for (std::size_t i = start; i < count; ++i) {
            const auto storage_index = tensor_storage_index(value, i);
            T element{};
            std::memcpy(
                &element,
                value.storage->data.data() + storage_index * sizeof(T),
                sizeof(T));
            if (operation == 1) {
                T next{};
                if (!tensor_add_checked(reduced, element, next)) {
                    tensor_fail("stats.sum integer arithmetic overflow", line, column);
                }
                reduced = next;
            } else if (operation == 2) {
                if (element < reduced) reduced = element;
            } else {
                if (element > reduced) reduced = element;
            }
        }
        std::memcpy(result.data(), &reduced, sizeof(T));
    };

    switch (dtype) {
        case 1: run(std::int64_t{}); break;
        case 2: run(std::int8_t{}); break;
        case 3: run(std::int16_t{}); break;
        case 4: run(std::int32_t{}); break;
        case 5: run(std::uint8_t{}); break;
        case 6: run(std::uint16_t{}); break;
        case 7: run(std::uint32_t{}); break;
        case 8: run(std::uint64_t{}); break;
        case 9: run(double{}); break;
        case 10:run(float{}); break;
        default:
            tensor_fail("stats reduction received an unsupported tensor dtype", line, column);
    }
    return result.data();
}

extern "C" double quidra_stats_mean(void* raw,
                                      unsigned long long line,
                                      unsigned long long column) {
    if (!raw) tensor_fail("stats.mean received a null tensor", line, column);
    auto& value = *static_cast<TensorValue*>(raw);
    const auto count = tensor_logical_count(value);
    if (count == 0) tensor_fail("stats.mean is undefined for an empty tensor", line, column);
    if (!tensor_on_cpu(*value.storage)) {
        tensor_require_initialized(value, line, column);
        TensorStorage* materialized = nullptr;
        const TensorStorage* input = value.storage;
        if (!tensor_is_contiguous_value(value) || value.offset != 0) {
            materialized = tensor_gpu_materialize_storage(value, line, column);
            input = materialized;
        }
        double result{};
        std::string backend_error;
        const bool ok = quidra::device::compute_mean(
            input->gpu_buffer, value.storage->dtype, count, result, backend_error);
        if (materialized) tensor_storage_release(materialized);
        if (!ok) tensor_fail(backend_error.c_str(), line, column);
        return result;
    }
    long double sum = 0.0L;
    switch (value.storage->dtype) {
        case 1: sum=tensor_sum_typed<std::int64_t>(value,line,column); break;
        case 2: sum=tensor_sum_typed<std::int8_t>(value,line,column); break;
        case 3: sum=tensor_sum_typed<std::int16_t>(value,line,column); break;
        case 4: sum=tensor_sum_typed<std::int32_t>(value,line,column); break;
        case 5: sum=tensor_sum_typed<std::uint8_t>(value,line,column); break;
        case 6: sum=tensor_sum_typed<std::uint16_t>(value,line,column); break;
        case 7: sum=tensor_sum_typed<std::uint32_t>(value,line,column); break;
        case 8: sum=tensor_sum_typed<std::uint64_t>(value,line,column); break;
        case 9: sum=tensor_sum_typed<double>(value,line,column); break;
        case 10:sum=tensor_sum_typed<float>(value,line,column); break;
        default: tensor_fail("stats.mean received an unsupported tensor dtype", line, column);
    }
    return static_cast<double>(sum / static_cast<long double>(count));
}


void tensor_attach_matmul_graph(
    TensorValue* result,TensorValue& left,TensorValue& right,
    unsigned long long line,unsigned long long column) {
    if(!result||(!left.graph&&!right.graph)) return;
    if(left.storage->dtype!=9&&left.storage->dtype!=10){
        release_managed_tensor(result);
        autograd_fail("tracked tensor matmul requires float32 or float tensors",line,column);
    }
    auto node=std::make_shared<AutogradNode>(left.storage->dtype);
    node->shape=result->shape;
    node->op=AutogradOp::Matmul;
    node->parents={
        left.graph?left.graph:autograd_constant_node(left,line,column),
        right.graph?right.graph:autograd_constant_node(right,line,column)};
    if(tensor_on_cpu(*result->storage))
        node->data=tensor_float_values(*result,line,column);
    else
        node->device_tensor=static_cast<TensorValue*>(quidra_tensor_clone(result));
    result->graph=std::move(node);
}

extern "C" void* quidra_linear_matmul(void* left_raw, void* right_raw,
                                       unsigned long long line,
                                       unsigned long long column) {
    if(!left_raw||!right_raw)
        tensor_fail("linear.matmul received a null tensor",line,column);
    auto& left=*static_cast<TensorValue*>(left_raw);
    auto& right=*static_cast<TensorValue*>(right_raw);
    if(left.storage->dtype!=right.storage->dtype)
        tensor_fail("linear.matmul requires identical element types",line,column);
    if(left.storage->device!=right.storage->device)
        tensor_fail("linear.matmul operands are on different devices; transfer them explicitly",
                    line,column);
    if(left.shape.empty()||(right.shape.size()!=1&&right.shape.size()!=2))
        tensor_fail(
            "linear.matmul requires left rank >= 1 and right rank 1 or 2",
            line,column);

    std::vector<long long> leading(left.shape.begin(),left.shape.end()-1);
    const auto rows=tensor_element_count(leading,line,column);
    const auto inner=static_cast<std::size_t>(left.shape.back());
    const auto right_inner=static_cast<std::size_t>(right.shape[0]);
    const bool right_vector=right.shape.size()==1;
    const auto columns=right_vector?std::size_t{1}:
        static_cast<std::size_t>(right.shape[1]);
    if(inner!=right_inner)
        tensor_fail("linear.matmul inner dimensions do not match",line,column);
    auto shape=leading;
    if(!right_vector) shape.push_back(static_cast<long long>(columns));
    const auto count=tensor_element_count(shape,line,column);

    if(!tensor_on_cpu(*left.storage)){
        tensor_require_initialized(left,line,column);
        tensor_require_initialized(right,line,column);
        TensorStorage* lm=nullptr;
        TensorStorage* rm=nullptr;
        const TensorStorage* ls=left.storage;
        const TensorStorage* rs=right.storage;
        if(!tensor_is_contiguous_value(left)||left.offset!=0){
            lm=tensor_gpu_materialize_storage(left,line,column);ls=lm;
        }
        if(!tensor_is_contiguous_value(right)||right.offset!=0){
            rm=tensor_gpu_materialize_storage(right,line,column);rs=rm;
        }
        auto* output=tensor_storage_create(
            left.storage->dtype,count,1,left.storage->device,line,column);
        std::string backend_error;
        const bool ok=quidra::device::compute_matmul(
            output->gpu_buffer,ls->gpu_buffer,rs->gpu_buffer,
            left.storage->dtype,rows,inner,columns,backend_error);
        if(lm) tensor_storage_release(lm);
        if(rm) tensor_storage_release(rm);
        if(!ok){tensor_storage_release(output);tensor_fail(backend_error.c_str(),line,column);}
        auto* result=tensor_descriptor(
            output,shape,tensor_contiguous_strides(shape),0);
        tensor_attach_matmul_graph(result,left,right,line,column);
        return result;
    }

    auto* output=tensor_storage_create(left.storage->dtype,count,1);
    switch(left.storage->dtype){
        case 1:tensor_matmul_typed<std::int64_t>(left,right,*output,rows,inner,columns,line,column);break;
        case 2:tensor_matmul_typed<std::int8_t>(left,right,*output,rows,inner,columns,line,column);break;
        case 3:tensor_matmul_typed<std::int16_t>(left,right,*output,rows,inner,columns,line,column);break;
        case 4:tensor_matmul_typed<std::int32_t>(left,right,*output,rows,inner,columns,line,column);break;
        case 5:tensor_matmul_typed<std::uint8_t>(left,right,*output,rows,inner,columns,line,column);break;
        case 6:tensor_matmul_typed<std::uint16_t>(left,right,*output,rows,inner,columns,line,column);break;
        case 7:tensor_matmul_typed<std::uint32_t>(left,right,*output,rows,inner,columns,line,column);break;
        case 8:tensor_matmul_typed<std::uint64_t>(left,right,*output,rows,inner,columns,line,column);break;
        case 9:tensor_matmul_typed<double>(left,right,*output,rows,inner,columns,line,column);break;
        case 10:tensor_matmul_typed<float>(left,right,*output,rows,inner,columns,line,column);break;
        default:delete output;tensor_fail("linear.matmul received an unsupported tensor dtype",line,column);
    }
    auto* result=tensor_descriptor(
        output,shape,tensor_contiguous_strides(shape),0);
    tensor_attach_matmul_graph(result,left,right,line,column);
    return result;
}

namespace {

constexpr long long tensor_slice_missing = std::numeric_limits<long long>::min();

TensorStorage* tensor_materialize_storage(const TensorValue& source) {
    const auto count = tensor_logical_count(source);
    auto* output = tensor_storage_create(source.storage->dtype, count, 0);
    const auto width = tensor_dtype_bytes(source.storage->dtype);
    for (std::size_t i = 0; i < count; ++i) {
        const auto source_index = tensor_storage_index(source, i);
        if (source_index >= source.storage->count) {
            delete output;
            runtime_text_failure("tensor view exceeds storage");
        }
        std::memcpy(output->data.data() + i * width,
                    source.storage->data.data() + source_index * width, width);
        if (tracker_bit(source.storage->initialization, source_index)) {
            tracker_set(output->initialization, i);
        }
    }
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
    if (tensor.storage->owners == 1 && owns_full_contiguous_storage) return;

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
        const auto kind = specs[axis * 4];
        const auto first = specs[axis * 4 + 1];
        const auto second = specs[axis * 4 + 2];
        const auto third = specs[axis * 4 + 3];
        const auto dimension = source->shape[axis];
        const auto stride = source->strides[axis];

        if (kind == 0) {
            if (first < 0 || first >= dimension) {
                tensor_fail("tensor index is outside the dimension", line, column);
            }
            offset += static_cast<std::size_t>(first) *
                      static_cast<std::size_t>(stride);
            continue;
        }
        if (kind != 1) tensor_fail("invalid tensor index kind", line, column);

        const auto start = first == tensor_slice_missing ? 0 : first;
        const auto stop = second == tensor_slice_missing ? dimension : second;
        const auto step = third == tensor_slice_missing ? 1 : third;
        if (step <= 0) {
            tensor_fail("tensor slices currently require a positive step", line, column);
        }
        if (start < 0 || start > dimension || stop < 0 || stop > dimension) {
            tensor_fail("tensor slice is outside the dimension", line, column);
        }
        const auto length = stop <= start ? 0 : (stop - start + step - 1) / step;
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
    return tensor_descriptor(source->storage, std::move(shape), std::move(strides), offset);
}

extern "C" void quidra_tensor_set(void* raw, const long long* indices,
                                    unsigned long long count, const void* value,
                                    unsigned long long line,
                                    unsigned long long column) {
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
            tensor_fail("tensor index is outside the dimension", line, column);
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


extern "C" void* quidra_tensor_from_chw(const void* data,
                                          int dtype,
                                          unsigned long long channels,
                                          unsigned long long height,
                                          unsigned long long width) {
    if (dtype < 1 || dtype > 10 ||
        (channels != 1 && channels != 3 && channels != 4) ||
        channels > static_cast<unsigned long long>(std::numeric_limits<long long>::max()) ||
        height > static_cast<unsigned long long>(std::numeric_limits<long long>::max()) ||
        width > static_cast<unsigned long long>(std::numeric_limits<long long>::max())) {
        return nullptr;
    }
    std::vector<long long> shape{
        static_cast<long long>(channels),
        static_cast<long long>(height),
        static_cast<long long>(width)};
    std::size_t count = 0;
    try {
        count = tensor_element_count(shape, 0, 0);
    } catch (...) {
        return nullptr;
    }
    if (count != 0 && !data) return nullptr;
    auto* storage = tensor_storage_create(dtype, count, 1);
    const auto sample_bytes = tensor_dtype_bytes(dtype);
    if (count != 0) {
        std::memcpy(storage->data.data(), data, count * sample_bytes);
    }
    auto strides = tensor_contiguous_strides(shape);
    return tensor_descriptor(storage, std::move(shape), std::move(strides), 0);
}

extern "C" long long quidra_tensor_device_index(void* raw) {
    if (!raw) return -2;
    const auto* tensor = static_cast<TensorValue*>(raw);
    if (!tensor->storage) return -2;
    return static_cast<long long>(tensor->storage->device);
}

extern "C" int quidra_tensor_chw_info(void* raw,
                                        unsigned long long* channels,
                                        unsigned long long* height,
                                        unsigned long long* width) {
    if (!raw || !channels || !height || !width) return 0;
    const auto& tensor = *static_cast<TensorValue*>(raw);
    if (!tensor.storage || tensor.storage->dtype < 1 || tensor.storage->dtype > 10 ||
        tensor.shape.size() != 3 ||
        (tensor.shape[0] != 1 && tensor.shape[0] != 3 && tensor.shape[0] != 4) ||
        tensor.shape[1] < 0 || tensor.shape[2] < 0) {
        return 0;
    }
    *channels = static_cast<unsigned long long>(tensor.shape[0]);
    *height = static_cast<unsigned long long>(tensor.shape[1]);
    *width = static_cast<unsigned long long>(tensor.shape[2]);
    return tensor.storage->dtype;
}

extern "C" bool quidra_tensor_chw_copy(void* raw,
                                        void* output,
                                        unsigned long long count) {
    if (!raw) return false;
    const auto& tensor = *static_cast<TensorValue*>(raw);
    if (!tensor.storage || !tensor_on_cpu(*tensor.storage) ||
        tensor.storage->dtype < 1 || tensor.storage->dtype > 10 ||
        tensor.shape.size() != 3) {
        return false;
    }
    const auto logical = tensor_logical_count(tensor);
    if (logical != static_cast<std::size_t>(count) || (logical != 0 && !output)) {
        return false;
    }
    const auto sample_bytes = tensor_dtype_bytes(tensor.storage->dtype);
    auto* destination = static_cast<unsigned char*>(output);
    for (std::size_t i = 0; i < logical; ++i) {
        const auto storage_index = tensor_storage_index(tensor, i);
        if (storage_index >= tensor.storage->count ||
            !tracker_bit(tensor.storage->initialization, storage_index)) {
            return false;
        }
        std::memcpy(destination + i * sample_bytes,
                    tensor.storage->data.data() + storage_index * sample_bytes,
                    sample_bytes);
    }
    return true;
}

extern "C" void* quidra_tensor_from_u8_chw(const unsigned char* data,
                                             unsigned long long channels,
                                             unsigned long long height,
                                             unsigned long long width) {
    if ((channels != 1 && channels != 3 && channels != 4) ||
        channels > static_cast<unsigned long long>(std::numeric_limits<long long>::max()) ||
        height > static_cast<unsigned long long>(std::numeric_limits<long long>::max()) ||
        width > static_cast<unsigned long long>(std::numeric_limits<long long>::max())) {
        return nullptr;
    }
    std::vector<long long> shape{
        static_cast<long long>(channels),
        static_cast<long long>(height),
        static_cast<long long>(width)};
    std::size_t count = 0;
    try {
        count = tensor_element_count(shape, 0, 0);
    } catch (...) {
        return nullptr;
    }
    if (count != 0 && !data) return nullptr;
    auto* storage = tensor_storage_create(5, count, 1);
    if (count != 0) std::memcpy(storage->data.data(), data, count);
    auto strides = tensor_contiguous_strides(shape);
    return tensor_descriptor(storage, std::move(shape), std::move(strides), 0);
}

extern "C" bool quidra_tensor_u8_chw_info(void* raw,
                                            unsigned long long* channels,
                                            unsigned long long* height,
                                            unsigned long long* width) {
    if (!raw || !channels || !height || !width) return false;
    const auto& tensor = *static_cast<TensorValue*>(raw);
    if (!tensor.storage || tensor.storage->dtype != 5 || tensor.shape.size() != 3) return false;
    if (tensor.shape[0] != 1 && tensor.shape[0] != 3 && tensor.shape[0] != 4) return false;
    if (tensor.shape[1] < 0 || tensor.shape[2] < 0) return false;
    *channels = static_cast<unsigned long long>(tensor.shape[0]);
    *height = static_cast<unsigned long long>(tensor.shape[1]);
    *width = static_cast<unsigned long long>(tensor.shape[2]);
    return true;
}

extern "C" bool quidra_tensor_u8_chw_copy(void* raw,
                                            unsigned char* output,
                                            unsigned long long count) {
    if (!raw) return false;
    const auto& tensor = *static_cast<TensorValue*>(raw);
    if (!tensor.storage || !tensor_on_cpu(*tensor.storage) ||
        tensor.storage->dtype != 5 || tensor.shape.size() != 3) return false;
    std::size_t logical = 0;
    try {
        logical = tensor_logical_count(tensor);
    } catch (...) {
        return false;
    }
    if (logical != static_cast<std::size_t>(count) || (logical != 0 && !output)) return false;
    for (std::size_t i = 0; i < logical; ++i) {
        const auto storage_index = tensor_storage_index(tensor, i);
        if (storage_index >= tensor.storage->count ||
            !tracker_bit(tensor.storage->initialization, storage_index)) {
            return false;
        }
        output[i] = tensor.storage->data[storage_index];
    }
    return true;
}

extern "C" char* quidra_string_index(const char* text, long long index,
                                      unsigned long long line,
                                      unsigned long long column) {
    if (!text) runtime_text_failure("null string");
    if (index < 0) {
        std::fprintf(stderr,
                     "Quidra runtime error[INDEX_BOUNDS] at %llu:%llu: string index %lld is negative\n",
                     line, column, index);
        std::exit(101);
    }

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
            std::fprintf(stderr,
                         "Quidra runtime error[INDEX_BOUNDS] at %llu:%llu: string index %lld outside length %zu\n",
                         line, column, index, length);
            std::exit(101);
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
        std::fprintf(stderr,
                     "Quidra runtime error[INDEX_BOUNDS] at %llu:%llu: string index %lld outside length %zu\n",
                     line, column, index, length);
        std::exit(101);
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
    if (index < 0) {
        std::fprintf(stderr,
                     "Quidra runtime error[INDEX_BOUNDS] at %llu:%llu: string index %lld is negative\n",
                     line, column, index);
        std::exit(101);
    }

    if (auto* allocation = exact_managed_string(text);
        allocation && allocation->string_ascii_known &&
        allocation->string_ascii) {
        const auto position = static_cast<std::size_t>(index);
        if (position >= allocation->string_byte_length) {
            std::fprintf(stderr,
                         "Quidra runtime error[INDEX_BOUNDS] at %llu:%llu: string index %lld outside length %zu\n",
                         line, column, index, allocation->string_byte_length);
            std::exit(101);
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
        std::fprintf(stderr,
                     "Quidra runtime error[INDEX_BOUNDS] at %llu:%llu: string index %lld outside length %zu\n",
                     line, column, index, length);
        std::exit(101);
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
        std::fprintf(
            stderr,
            "Quidra runtime error[INTEGER_OVERFLOW] at %llu:%llu: integer overflow\n",
            overflow_line, overflow_column);
        std::exit(101);
    };
    auto bounds = [&](long long index, std::size_t length) -> void {
        std::fprintf(
            stderr,
            "Quidra runtime error[INDEX_BOUNDS] at %llu:%llu: string index %lld outside length %zu\n",
            index_line, index_column, index, length);
        std::exit(101);
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

extern "C" char* quidra_string_slice(
    const char* text, long long start, long long end) {
    if (!text) runtime_text_failure("null string");
    if (start < 0 || end < start)
        runtime_text_failure("string slice is outside [0, len]");

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
    if (codepoint != target_start) {
        runtime_text_failure("string slice is outside [0, len]");
    }
    const auto byte_start = byte;

    while (codepoint < target_end && byte < source.size()) {
        (void)utf8_next(source, byte);
        ++codepoint;
    }
    if (codepoint != target_end) {
        runtime_text_failure("string slice is outside [0, len]");
    }

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
        source.substr(byte_start, byte - byte_start), target_end - target_start);
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
            managed_ranges.emplace(key, allocation.size);
            allocation.interior_range_tracked = true;
        }
        if (allocation.owners == std::numeric_limits<std::size_t>::max()) {
            delete iterator;
            runtime_text_failure("managed owner count overflow");
        }
        ++allocation.owners;
        allocation.shared_string_slab = true;
        iterator->slab = const_cast<char*>(text);
        iterator->allocation = source_allocation;
        iterator->slab[source.size()] = '\0';
        return iterator;
    }

    auto* slab = static_cast<char*>(
        managed_allocate_impl(source.size() + 1, true));
    if (!source.empty()) std::memcpy(slab, source.data(), source.size());
    slab[source.size()] = '\0';

    const auto slab_key = reinterpret_cast<std::uintptr_t>(slab);
    auto slab_it = managed_allocations.find(slab_key);
    if (slab_it == managed_allocations.end()) {
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
        cached_shared_string_text = iterator.slab + start;
        cached_shared_string_length = iterator.size - start;
        cached_shared_string_allocation = iterator.allocation;
        return iterator.slab + start;
    }

    const auto position = start + relative;
    iterator.slab[position] = '\0';
    iterator.next = position + iterator.separator.size();
    cached_shared_string_text = iterator.slab + start;
    cached_shared_string_length = position - start;
    cached_shared_string_allocation = iterator.allocation;
    return iterator.slab + start;
}

extern "C" void quidra_string_split_iter_end(void* raw) {
    if (!raw) return;
    auto* iterator = static_cast<QuidraStringSplitIterator*>(raw);
    if (cached_shared_string_allocation == iterator->allocation) {
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

    if (piece_count > (std::numeric_limits<std::size_t>::max() - 8) / sizeof(char*) ||
        source.size() == std::numeric_limits<std::size_t>::max()) {
        runtime_allocation_failure();
    }

    auto* slab = static_cast<char*>(
        managed_allocate_impl(source.size() + 1, true));
    if (!source.empty()) std::memcpy(slab, source.data(), source.size());
    slab[source.size()] = '\0';

    const auto slab_key = reinterpret_cast<std::uintptr_t>(slab);
    auto slab_it = managed_allocations.find(slab_key);
    if (slab_it == managed_allocations.end())
        runtime_text_failure("split backing storage disappeared");
    slab_it->second.shared_string_slab = true;
    slab_it->second.owners = piece_count;

    const auto bytes = 8 + piece_count * sizeof(char*);
    auto* result = static_cast<unsigned char*>(managed_allocate(bytes));
    const auto count = static_cast<long long>(piece_count);
    std::memcpy(result, &count, sizeof(count));

    std::size_t start = 0;
    for (std::size_t i = 0; i < piece_count; ++i) {
        const auto pos = source.find(delimiter, start);
        const auto end = pos == std::string_view::npos ? source.size() : pos;
        slab[end] = '\0';
        auto* item = slab + start;
        std::memcpy(result + 8 + i * sizeof(char*), &item, sizeof(item));
        start = pos == std::string_view::npos
            ? source.size() : pos + delimiter.size();
    }
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
            case 0: {
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
            case 4: {
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
            case 1: {
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
            case 2: {
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
            case 3: {
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
    // Re-looking it up in managed_allocations on every append is pure overhead
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
            auto node = managed_allocations.extract(old_key);
            if (node.empty())
                runtime_text_failure("string build-append storage disappeared");
            node.key() = new_key;
            node.mapped().base = result;
            node.mapped().size = new_bytes;
            node.mapped().small_pool_class = 0;
            const auto inserted = managed_allocations.insert(std::move(node));
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
            case 0:
            case 4: {
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
            case 1:
            case 2: {
                if (lengths[i] != 0) {
                    std::memcpy(
                        result + offset, numeric_parts[i].data(), lengths[i]);
                    offset += lengths[i];
                }
                break;
            }
            case 3: {
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
    string_build_append_last_codepoints = added_length;
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
    return string_build_append_last_codepoints;
}

extern "C" char* quidra_string_concat_many(const char* const* values,
                                                 unsigned long long raw_count);

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
    auto it = managed_allocations.find(old_key);
    if (it == managed_allocations.end()) {
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
            auto node = managed_allocations.extract(old_key);
            node.key() = new_key;
            node.mapped().base = result;
            node.mapped().size = new_bytes;
            node.mapped().small_pool_class = 0;
            managed_allocations.insert(std::move(node));
        } else {
            allocation.base = result;
            allocation.size = new_bytes;
            allocation.small_pool_class = 0;
        }
        it = managed_allocations.find(new_key);
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
            case 0: {
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
            case 1: {
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
            case 2: {
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
            case 3: {
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
            case 0: {
                const auto* text = reinterpret_cast<const char*>(
                    static_cast<std::uintptr_t>(raw_values[i]));
                ManagedAllocation* allocation = nullptr;
                const auto piece = cached_string_view(text, allocation);
                if (!piece.empty())
                    std::memcpy(result + offset, piece.data(), piece.size());
                offset += piece.size();
                break;
            }
            case 1: {
                const auto value = std::bit_cast<long long>(raw_values[i]);
                const auto converted = std::to_chars(
                    result + offset, result + total, value, 10);
                if (converted.ec != std::errc{})
                    runtime_text_failure("integer formatting failed");
                offset = static_cast<std::size_t>(converted.ptr - result);
                break;
            }
            case 2: {
                const auto converted = std::to_chars(
                    result + offset, result + total, raw_values[i], 10);
                if (converted.ec != std::errc{})
                    runtime_text_failure("integer formatting failed");
                offset = static_cast<std::size_t>(converted.ptr - result);
                break;
            }
            case 3: {
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
    const auto* data = static_cast<const unsigned char*>(raw) + 8;
    const auto byte_index = static_cast<std::size_t>(index / 8);
    const auto shift = static_cast<unsigned>(7 - (index % 8));
    return ((data[byte_index] >> shift) & 1U) != 0;
}

void bin_set_bit(void* raw, long long index, bool value) {
    const auto length = bin_length(raw);
    if (index < 0 || index >= length) runtime_text_failure("bin index out of bounds");
    auto* data = static_cast<unsigned char*>(raw) + 8;
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
    if (bytes > std::numeric_limits<std::size_t>::max() - 8) runtime_allocation_failure();
    auto* result = static_cast<unsigned char*>(managed_allocate(8 + bytes));
    std::memcpy(result, &bit_count, sizeof(bit_count));
    if (bytes != 0) std::memset(result + 8, fill ? 0xff : 0x00, bytes);
    if (fill && bit_count % 8 != 0 && bytes != 0) {
        const auto used = static_cast<unsigned>(bit_count % 8);
        result[8 + bytes - 1] &= static_cast<unsigned char>(0xffU << (8U - used));
    }
    return result;
}

extern "C" void* quidra_bin_index(void* raw, long long index) {
    auto* result = quidra_bin_alloc(1, 0);
    bin_set_bit(result, 0, bin_bit_at(raw, index));
    return result;
}

extern "C" void quidra_bin_set(void* raw, long long index, void* bit) {
    if (bin_length(bit) != 1) runtime_text_failure("bin element assignment requires exactly one bit");
    bin_set_bit(raw, index, bin_bit_at(bit, 0));
}

extern "C" void* quidra_bin_slice(void* raw, long long start, long long end) {
    const auto length = bin_length(raw);
    if (start < 0 || end < start || end > length) runtime_text_failure("bin slice out of bounds");
    auto* result = quidra_bin_alloc(end - start, 0);
    for (long long i = start; i < end; ++i) bin_set_bit(result, i - start, bin_bit_at(raw, i));
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
    auto* result = static_cast<unsigned char*>(managed_allocate(8 + bytes));
    std::memcpy(result, raw, 8 + bytes);
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
        static_cast<unsigned char*>(left) + 8,
        static_cast<unsigned char*>(right) + 8,
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
        static_cast<const char*>(raw) + 8, count);
}

extern "C" char* quidra_bin_try_utf8(void* raw) {
    if (!raw) return nullptr;
    const auto bits = bin_length(raw);
    if (bits % 8 != 0) return nullptr;
    return quidra_runtime_try_copy_text_bytes(
        static_cast<const char*>(raw) + 8,
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
    const auto* data = static_cast<const unsigned char*>(raw) + 8;
    if (width == 8 && stride == 1) {
        if (count != 0)
            std::memcpy(static_cast<unsigned char*>(result) + 8, data,
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
             (std::numeric_limits<std::size_t>::max() - 8) / count_size))
        runtime_allocation_failure();
    auto* result = static_cast<unsigned char*>(
        managed_allocate(8 + count_size * static_cast<std::size_t>(stride)));
    std::memcpy(result, &count, sizeof(count));
    auto* data = result + 8;
    if (width == 8 && stride == 1) {
        if (count_size != 0)
            std::memcpy(data, static_cast<const unsigned char*>(raw) + 8,
                        count_size);
        quidra_init_create(result, static_cast<unsigned long long>(count_size),
                           1, 8, 1);
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
                       static_cast<unsigned long long>(stride), 8, 1);
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
    if (source.size() > (std::numeric_limits<std::size_t>::max() - 8) ||
        source.size() > static_cast<std::size_t>(std::numeric_limits<long long>::max() / 8)) {
        runtime_allocation_failure();
    }
    auto* result = static_cast<unsigned char*>(managed_allocate(8 + source.size()));
    const auto count = static_cast<long long>(source.size() * 8);
    std::memcpy(result, &count, sizeof(count));
    if (!source.empty()) std::memcpy(result + 8, source.data(), source.size());
    return result;
}

extern "C" void* quidra_string_codepoints(const char* text) {
    if (!text) runtime_text_failure("null string");
    ManagedAllocation* allocation = nullptr;
    std::size_t count_size = 0;
    const auto source =
        validated_string_view(text, allocation, &count_size);
    if (count_size > (std::numeric_limits<std::size_t>::max() - 8) / sizeof(long long)) {
        runtime_allocation_failure();
    }
    auto* result = static_cast<unsigned char*>(
        managed_allocate(8 + count_size * sizeof(long long)));
    const auto count = static_cast<long long>(count_size);
    std::memcpy(result, &count, sizeof(count));
    std::size_t byte_index = 0;
    std::size_t out_index = 0;
    while (byte_index < source.size()) {
        const auto codepoint = static_cast<long long>(utf8_next(source, byte_index));
        std::memcpy(result + 8 + out_index * sizeof(long long),
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
    if (count > (std::numeric_limits<std::size_t>::max() - 8) / sizeof(char*)) {
        runtime_allocation_failure();
    }
    if (count != 0) {
        quidra_init_require_range(
            static_cast<unsigned char*>(raw) + 8,
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
                        static_cast<unsigned char*>(raw) + 8 + i * sizeof(char*),
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
                            static_cast<unsigned char*>(raw) + 8 + i * sizeof(char*),
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
                    static_cast<unsigned char*>(raw) + 8 + i * sizeof(char*),
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
    if (!out) return -1;
    *out = nullptr;

    std::string text;
    if (!std::getline(std::cin, text)) {
        if (std::cin.bad()) return -1;
        if (std::cin.eof()) return 0;
        return -1;
    }

    std::size_t codepoints = 0;
    bool contains_nul = false;
    if (!valid_utf8(text, &codepoints, &contains_nul) || contains_nul)
        return -1;
    *out = copy_validated_runtime_text(text, codepoints);
    return 1;
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

extern "C" bool quidra_parse_signed(const char* text, long long* out) {
    if (!text || !out || !*text) return false;

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
    if (errno == ERANGE || end == text || !end || *end != '\0') return false;
    *out = value;
    return true;
}

extern "C" bool quidra_parse_unsigned(const char* text, unsigned long long* out) {
    if (!text || !out || !*text || *text == '-') return false;

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
    if (errno == ERANGE || end == text || !end || *end != '\0') return false;
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

extern "C" bool quidra_parse_float32(const char* text, float* out) {
    if (!text || !out || !*text) return false;
    errno = 0;
    char* end = nullptr;
    const auto value = std::strtof(text, &end);
    if (errno == ERANGE || end == text || !end || *end != '\0' || !std::isfinite(value)) return false;
    *out = value;
    return true;
}

extern "C" bool quidra_parse_float64(const char* text, double* out) {
    if (!text || !out || !*text) return false;
    errno = 0;
    char* end = nullptr;
    const auto value = std::strtod(text, &end);
    if (errno == ERANGE || end == text || !end || *end != '\0' || !std::isfinite(value)) return false;
    *out = value;
    return true;
}

extern "C" long long quidra_cli_parse_int(const char* text) {
    if (!text || !*text) cli_fail("invalid int value");
    errno = 0;
    char* end = nullptr;
    const auto value = std::strtoll(text, &end, 10);
    if (errno == ERANGE || end == text || !end || *end != '\0') cli_fail("invalid int value");
    return value;
}

extern "C" double quidra_cli_parse_float(const char* text) {
    if (!text || !*text) cli_fail("invalid float value");
    errno = 0;
    char* end = nullptr;
    const auto value = std::strtod(text, &end);
    if (errno == ERANGE || end == text || !end || *end != '\0') cli_fail("invalid float value");
    return value;
}

extern "C" void* quidra_cli_parse_bigint(const char* text) {
    auto* value = quidra_bigint_parse(text);
    if (!value) cli_fail("invalid bigint value");
    return value;
}

extern "C" void* quidra_cli_parse_bigreal(const char* text) {
    auto* value = quidra_bigreal_parse(text);
    if (!value) cli_fail("invalid bigreal value");
    return value;
}

extern "C" bool quidra_cli_parse_bool(const char* text) {
    if (text && std::strcmp(text, "true") == 0) return true;
    if (text && std::strcmp(text, "false") == 0) return false;
    cli_fail("bool values must be true or false");
}
