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
#include <iterator>
#include <limits>
#include <map>
#include <memory>
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

namespace {
int runtime_argc = 0;
char** runtime_argv = nullptr;
std::vector<unsigned char> runtime_used;

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
    std::uint64_t identity{};
    std::size_t owners{1};
    std::size_t pins{};
    std::size_t array_capacity{};
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
    bool string_index_cursor_valid{};
    std::size_t string_index_cursor_codepoint{};
    std::size_t string_index_cursor_byte{};
    ManagedDrop drop{};
    std::unique_ptr<InitializationTracker> initialization;
};

struct ManagedFinalization {
    void* base{};
    ManagedDrop drop{};
};

using ManagedAllocations = std::unordered_map<std::uintptr_t, ManagedAllocation>;
thread_local ManagedAllocations managed_allocations;
thread_local std::uint64_t next_managed_identity = 1;
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
    std::free(finalization.base);
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

void* managed_allocate(std::size_t bytes) {
    if (bytes == 0) bytes = 1;
    auto* memory = std::malloc(bytes);
    if (!memory) runtime_allocation_failure();
    const auto key = reinterpret_cast<std::uintptr_t>(memory);
    // unordered_map insertion may rehash and invalidate the cached value pointer.
    clear_managed_range_cache();
    if (next_managed_identity == std::numeric_limits<std::uint64_t>::max()) {
        std::free(memory);
        runtime_allocation_failure();
    }
    ManagedAllocation allocation;
    allocation.base = memory;
    allocation.size = bytes;
    allocation.identity = next_managed_identity++;
    managed_allocations.emplace(key, std::move(allocation));
    managed_ranges.emplace(key, bytes);
    return memory;
}

std::uint64_t managed_identity(const void* value) {
    if (!value) return 0;
    const auto it =
        managed_allocations.find(reinterpret_cast<std::uintptr_t>(value));
    return it == managed_allocations.end() ? 0 : it->second.identity;
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

bool valid_utf8(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7fU) {
            ++index;
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
    }
    return true;
}

bool valid_runtime_text(std::string_view text) {
    return text.find('\0') == std::string_view::npos && valid_utf8(text);
}

void validate_utf8(std::string_view text) {
    if (!valid_utf8(text)) runtime_text_failure("invalid UTF-8 string");
}

void validate_runtime_text(std::string_view text) {
    if (!valid_utf8(text)) runtime_text_failure("string text is not valid UTF-8");
    if (text.find('\0') != std::string_view::npos) {
        runtime_text_failure("string text cannot contain NUL");
    }
}

char* copy_runtime_text(std::string_view value) {
    validate_runtime_text(value);
    if (value.size() == std::numeric_limits<std::size_t>::max()) {
        runtime_allocation_failure();
    }
    auto* result = static_cast<char*>(managed_allocate(value.size() + 1));
    if (!value.empty()) std::memcpy(result, value.data(), value.size());
    result[value.size()] = '\0';
    return result;
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

std::vector<std::size_t> utf8_offsets(std::string_view text) {
    std::vector<std::size_t> offsets{0};
    std::size_t index = 0;
    while (index < text.size()) {
        (void)utf8_next(text,index);
        offsets.push_back(index);
    }
    return offsets;
}

struct StringIndexBounds {
    bool found{};
    std::size_t start{};
    std::size_t end{};
};

std::string_view cached_string_view(const char* text, ManagedAllocation*& allocation) {
    allocation = nullptr;
    if (!text) runtime_text_failure("null string");
    const auto it = managed_allocations.find(reinterpret_cast<std::uintptr_t>(text));
    if (it == managed_allocations.end()) return std::string_view(text);

    allocation = &it->second;
    if (!allocation->string_byte_length_known) {
        const auto* end = static_cast<const char*>(
            std::memchr(text, '\0', allocation->size));
        if (!end) runtime_text_failure("managed string is missing a terminator");
        allocation->string_byte_length = static_cast<std::size_t>(end - text);
        allocation->string_byte_length_known = true;
    }
    return std::string_view(text, allocation->string_byte_length);
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

extern "C" void quidra_managed_retain(void* value) {
    if (!value) return;
    const auto it = managed_allocations.find(reinterpret_cast<std::uintptr_t>(value));
    if (it != managed_allocations.end()) {
        if (it->second.owners == std::numeric_limits<std::size_t>::max()) {
            runtime_text_failure("managed owner count overflow");
        }
        ++it->second.owners;
    }
}

extern "C" void quidra_managed_release(void* value, void* drop_function) {
    if (!value) return;
    const auto it = managed_allocations.find(reinterpret_cast<std::uintptr_t>(value));
    if (it == managed_allocations.end()) return;
    auto& allocation = it->second;
    if (drop_function && !allocation.drop) {
        allocation.drop = reinterpret_cast<ManagedDrop>(drop_function);
    }
    if (allocation.owners == 0) runtime_text_failure("managed owner count underflow");
    --allocation.owners;
    if (allocation.owners == 0 && allocation.pins == 0) {
        const auto key = reinterpret_cast<std::uintptr_t>(allocation.base);
        const ManagedFinalization finalization{allocation.base, allocation.drop};
        clear_managed_range_cache(&allocation);
        managed_ranges.erase(key);
        managed_allocations.erase(it);
        finalize_managed(finalization);
    }
}

extern "C" void quidra_managed_pin(void* address) {
    if (!address) return;
    auto* allocation = managed_containing(address);
    if (!allocation) return;
    if (allocation->pins == std::numeric_limits<std::size_t>::max()) {
        runtime_text_failure("managed pin count overflow");
    }
    ++allocation->pins;
}

extern "C" void quidra_managed_unpin(void* address) {
    if (!address) return;
    auto* allocation = managed_containing(address);
    if (!allocation) return;
    if (allocation->pins == 0) runtime_text_failure("managed pin count underflow");
    --allocation->pins;
    if (allocation->owners == 0 && allocation->pins == 0) {
        const auto key = reinterpret_cast<std::uintptr_t>(allocation->base);
        const ManagedFinalization finalization{allocation->base, allocation->drop};
        clear_managed_range_cache(allocation);
        managed_ranges.erase(key);
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
        it->second.array_capacity = tracker->count;
    }
    it->second.initialization = std::move(tracker);
}

extern "C" void quidra_init_mark_range(void* address, unsigned long long bytes) {
    if (!address || bytes == 0) return;
    if (tracker_is_complete(address)) return;
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


extern "C" bool quidra_array_can_append_move(void* array) {
    if (!array) return false;
    const auto it = managed_allocations.find(reinterpret_cast<std::uintptr_t>(array));
    if (it == managed_allocations.end()) return false;
    const auto& allocation = it->second;
    if (allocation.owners != 1 || allocation.pins != 0 || !allocation.initialization) {
        return false;
    }
    const auto& tracker = *allocation.initialization;
    return tracker.data_offset == 8 && tracker.unit_bytes != 0 &&
           tracker.fully_initialized && tracker.count <= allocation.array_capacity;
}

extern "C" void* quidra_array_grow_move(void* array, unsigned long long raw_stride) {
    if (!quidra_array_can_append_move(array) || raw_stride == 0 ||
        raw_stride > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        runtime_text_failure("array append move requires unique initialized storage");
    }

    const auto old_key = reinterpret_cast<std::uintptr_t>(array);
    auto it = managed_allocations.find(old_key);
    auto& allocation = it->second;
    auto& tracker = *allocation.initialization;
    const auto stride = static_cast<std::size_t>(raw_stride);
    if (stride != tracker.unit_bytes) runtime_text_failure("array append stride mismatch");

    long long signed_length = 0;
    std::memcpy(&signed_length, array, sizeof(signed_length));
    if (signed_length < 0 ||
        static_cast<unsigned long long>(signed_length) != tracker.count ||
        signed_length == std::numeric_limits<long long>::max()) {
        runtime_text_failure("invalid array length during append");
    }

    const auto old_count = static_cast<std::size_t>(signed_length);
    const auto new_count = old_count + 1;
    void* result = array;

    if (allocation.array_capacity < new_count) {
        std::size_t new_capacity = allocation.array_capacity < 4
            ? 4
            : allocation.array_capacity;
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

        const auto new_bytes = static_cast<std::size_t>(8) + new_capacity * stride;
        const auto old_bytes = allocation.size;
        clear_managed_range_cache(&allocation);
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
            node.key() = new_key;
            node.mapped().base = result;
            node.mapped().size = new_bytes;
            node.mapped().array_capacity = new_capacity;
            managed_allocations.insert(std::move(node));
        } else {
            allocation.base = result;
            allocation.size = new_bytes;
            allocation.array_capacity = new_capacity;
        }
        managed_ranges.emplace(new_key, new_bytes);
        it = managed_allocations.find(new_key);
    }

    auto& updated_tracker = *it->second.initialization;
    updated_tracker.count = new_count;
    updated_tracker.initialized_count = new_count;
    updated_tracker.fully_initialized = true;
    updated_tracker.bits.clear();

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
            for (std::size_t i = 0; i < count; ++i) {
                if (!begin[i]) runtime_text_failure("null string in sorted array");
                validate_utf8(begin[i]);
                quidra_managed_retain(begin[i]);
            }
            std::stable_sort(begin, begin + count, [](const char* left, const char* right) {
                return std::string_view(left) < std::string_view(right);
            });
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

namespace {

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
        case 2: case 5: return 1;
        case 3: case 6: return 2;
        case 4: case 7: case 10: return 4;
        default: runtime_text_failure("invalid tensor dtype");
    }
}

std::vector<long long> tensor_shape_from_array(
    void* raw, unsigned long long line, unsigned long long column) {
    if (!raw) tensor_fail("shape array is null", line, column);
    long long rank = 0;
    std::memcpy(&rank, raw, sizeof(rank));
    if (rank < 0 || rank > 64) tensor_fail("tensor rank must be between 0 and 64", line, column);
    auto* data = static_cast<unsigned char*>(raw) + 8;
    quidra_init_require_range(
        data, static_cast<unsigned long long>(rank) * sizeof(long long), line, column);
    std::vector<long long> shape(static_cast<std::size_t>(rank));
    for (long long i = 0; i < rank; ++i) {
        std::memcpy(&shape[static_cast<std::size_t>(i)],
                    data + static_cast<std::size_t>(i) * sizeof(long long),
                    sizeof(long long));
        if (shape[static_cast<std::size_t>(i)] < 0) {
            tensor_fail("tensor dimensions cannot be negative", line, column);
        }
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
        storage, std::move(shape), std::move(strides), offset};
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
    std::array<unsigned char, 8> element{};
    std::string backend_error;

    if (target_device >= 0 && source.storage->device == target_device &&
        source.storage->initialization.fully_initialized &&
        tensor_is_contiguous_value(source)) {
        const auto source_offset = source.offset * width;
        const auto bytes = count * width;
        if (quidra::device::copy_device_to_device(
                output->gpu_buffer, 0, source.storage->gpu_buffer,
                source_offset, bytes, backend_error)) {
            output->initialization.fully_initialized = true;
            output->initialization.initialized_count = count;
            output->initialization.bits.clear();
            return output;
        }
        tensor_storage_release(output);
        tensor_fail(backend_error.c_str(), line, column);
    }

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
    auto* storage = tensor_transfer_storage(
        *source, static_cast<int>(gpu), line, column);
    return tensor_descriptor(
        storage, source->shape, tensor_contiguous_strides(source->shape), 0);
}

extern "C" void* quidra_tensor_to_cpu(
    void* raw, unsigned long long line, unsigned long long column) {
    if (!raw) tensor_fail("null tensor", line, column);
    auto* source = static_cast<TensorValue*>(raw);
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
    return tensor_descriptor(
        source->storage, source->shape, source->strides, source->offset);
}

extern "C" void quidra_tensor_drop(void* raw) {
    if (!raw) return;
    auto* tensor = static_cast<TensorValue*>(raw);
    auto* storage = tensor->storage;
    tensor->~TensorValue();
    tensor_storage_release(storage);
}

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
    return tensor_descriptor(source->storage, std::move(shape), std::move(strides), source->offset);
}

extern "C" void* quidra_tensor_contiguous(void* raw) {
    if (!raw) runtime_text_failure("null tensor");
    auto* source = static_cast<TensorValue*>(raw);
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
        return tensor_descriptor(output, source->shape,
                                 tensor_contiguous_strides(source->shape), 0);
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
    return tensor_descriptor(output, source->shape,
                             tensor_contiguous_strides(source->shape), 0);
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

enum class NeuralOp {
    Leaf, Add, Sub, Mul, Div, Affine, Convolution, Normalize, RandomMask,
    Absolute, Exponential, Logarithm, Mean, SumLast, MaxLast
};

extern "C" void* quidra_neural_tensor_unary(
    void* raw,int op,unsigned long long line,unsigned long long column);
extern "C" void* quidra_tensor_unary(
    void* raw,int operation,unsigned long long line,unsigned long long column);
extern "C" void* quidra_tensor_binary(
    void* primary_raw,void* other_raw,void* scalar,int scalar_side,
    int operation,unsigned long long line,unsigned long long column);


class NeuralBuffer {
public:
    explicit NeuralBuffer(int dtype=10) { set_dtype(dtype); }
    explicit NeuralBuffer(std::vector<float> values) : values_(std::move(values)) {}
    explicit NeuralBuffer(std::vector<double> values) : values_(std::move(values)) {}

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
            runtime_text_failure("invalid neural floating dtype");
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

struct NeuralNode {
    explicit NeuralNode(int element_dtype=10)
        : dtype(element_dtype), data(element_dtype), aux(element_dtype) {}

    int dtype{10};
    std::vector<long long> shape;
    NeuralBuffer data;
    NeuralOp op{NeuralOp::Leaf};
    std::vector<std::shared_ptr<NeuralNode>> parents;
    NeuralBuffer aux;
    std::vector<std::size_t> aux_index;
    unsigned long long parameter_id{};
    TensorValue* device_tensor{};

    ~NeuralNode() noexcept {
        if(device_tensor){
            quidra_tensor_drop(device_tensor);
            device_tensor=nullptr;
        }
        // shared_ptr parent chains can otherwise recurse through destructors and
        // exhaust the native stack even though graph traversal itself is iterative.
        std::vector<std::shared_ptr<NeuralNode>> pending;
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

struct NeuralValue {
    std::shared_ptr<NeuralNode> node;
};

struct NeuralGradient {
    explicit NeuralGradient(int element_dtype=10) : dtype(element_dtype), data(element_dtype) {}
    NeuralGradient(const NeuralGradient&)=delete;
    NeuralGradient& operator=(const NeuralGradient&)=delete;
    NeuralGradient(NeuralGradient&& other) noexcept
        : dtype(other.dtype),
          shape(std::move(other.shape)),
          data(std::move(other.data)),
          device_tensor(std::exchange(other.device_tensor,nullptr)) {}
    NeuralGradient& operator=(NeuralGradient&& other) noexcept {
        if(this==&other) return *this;
        if(device_tensor) quidra_tensor_drop(device_tensor);
        dtype=other.dtype;
        shape=std::move(other.shape);
        data=std::move(other.data);
        device_tensor=std::exchange(other.device_tensor,nullptr);
        return *this;
    }
    ~NeuralGradient() {
        if(device_tensor) quidra_tensor_drop(device_tensor);
    }

    int dtype{10};
    std::vector<long long> shape;
    NeuralBuffer data;
    TensorValue* device_tensor{};
};

std::size_t neural_gradient_count(const NeuralGradient& gradient) {
    return gradient.device_tensor
        ? tensor_logical_count(*gradient.device_tensor)
        : gradient.data.size();
}

struct NeuralGradientData {
    std::unordered_map<unsigned long long, NeuralGradient> values;
};

struct NeuralGradients {
    std::shared_ptr<NeuralGradientData> data;
};

[[noreturn]] void neural_fail(const char* message, unsigned long long line, unsigned long long column) {
    std::fprintf(stderr, "Quidra runtime error[NEURAL] at %llu:%llu: %s\n", line, column, message);
    std::exit(101);
}

NeuralBuffer tensor_float_values(const TensorValue& tensor,
                                 unsigned long long line,
                                 unsigned long long column) {
    tensor_require_cpu(*tensor.storage, "neural tensor conversion", line, column);
    tensor_require_initialized(tensor,line,column);
    if(tensor.storage->dtype!=9&&tensor.storage->dtype!=10)
        neural_fail("neural values require float32 or float tensors",line,column);
    const auto count=tensor_logical_count(tensor);
    NeuralBuffer result(tensor.storage->dtype);
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

std::shared_ptr<NeuralNode> neural_constant_node(const TensorValue& tensor,
                                                 unsigned long long line,
                                                 unsigned long long column) {
    tensor_require_initialized(tensor,line,column);
    if(tensor.storage->dtype!=9&&tensor.storage->dtype!=10)
        neural_fail("neural values require float32 or float tensors",line,column);
    auto node=std::make_shared<NeuralNode>(tensor.storage->dtype);
    node->shape=tensor.shape;
    if(tensor_on_cpu(*tensor.storage)){
        node->data=tensor_float_values(tensor,line,column);
    }else{
        node->device_tensor=static_cast<TensorValue*>(
            quidra_tensor_clone(const_cast<TensorValue*>(&tensor)));
        if(!node->device_tensor)
            neural_fail("failed to retain GPU tensor for neural graph",line,column);
    }
    return node;
}

NeuralValue* neural_descriptor(std::shared_ptr<NeuralNode> node) {
    auto* memory=managed_allocate(sizeof(NeuralValue));
    return new(memory) NeuralValue{std::move(node)};
}

NeuralGradients* neural_gradients_descriptor(std::shared_ptr<NeuralGradientData> data) {
    auto* memory=managed_allocate(sizeof(NeuralGradients));
    return new(memory) NeuralGradients{std::move(data)};
}

TensorValue* neural_tensor_from_values(
    int dtype,const std::vector<long long>& shape,const NeuralBuffer& data) {
    if(data.dtype()!=dtype) runtime_text_failure("neural buffer dtype mismatch");
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
        runtime_text_failure("invalid neural floating dtype");
    }
    return tensor_descriptor(storage,shape,tensor_contiguous_strides(shape),0);
}

TensorValue* neural_tensor_from_node(const NeuralNode& node) {
    if(node.device_tensor)
        return static_cast<TensorValue*>(quidra_tensor_clone(node.device_tensor));
    return neural_tensor_from_values(node.dtype,node.shape,node.data);
}

std::size_t neural_node_count(const NeuralNode& node) {
    return node.device_tensor
        ? tensor_logical_count(*node.device_tensor)
        : node.data.size();
}

void neural_require_same_node_device(
    const char* operation,const NeuralNode& left,const NeuralNode& right,
    unsigned long long line,unsigned long long column) {
    const bool left_gpu=left.device_tensor!=nullptr;
    const bool right_gpu=right.device_tensor!=nullptr;
    if(left_gpu!=right_gpu){
        const auto message=std::string(operation)+
            " operands must be on the same device; transfer them explicitly before neural.track";
        neural_fail(message.c_str(),line,column);
    }
    if(left_gpu &&
       left.device_tensor->storage->device!=right.device_tensor->storage->device){
        const auto message=std::string(operation)+
            " operands must use the same gpu(n)";
        neural_fail(message.c_str(),line,column);
    }
}

TensorValue* neural_parameter_tensor(void* parameter_raw) {
    if (!parameter_raw) return nullptr;
    void* tensor_raw=nullptr;
    std::memcpy(&tensor_raw,parameter_raw,sizeof(tensor_raw));
    return static_cast<TensorValue*>(tensor_raw);
}

void neural_require_same_tensor_device(
    const char* operation, const TensorValue& input,
    std::initializer_list<const TensorValue*> others,
    unsigned long long line, unsigned long long column) {
    const int device = input.storage->device;
    for (const auto* tensor : others) {
        if (!tensor) neural_fail("null neural tensor", line, column);
        if (tensor->storage->device != device) {
            const auto message = std::string(operation) +
                " input and Parameter/state tensors must be on the same device; transfer them explicitly";
            neural_fail(message.c_str(), line, column);
        }
    }
    (void)operation;
}

double neural_tensor_value(const TensorValue& tensor,std::size_t logical,
                           unsigned long long line,unsigned long long column) {
    tensor_require_cpu(*tensor.storage, "neural parameter access", line, column);
    const auto index=tensor_storage_index(tensor,logical);
    if(index>=tensor.storage->count ||
       !tracker_bit(tensor.storage->initialization,index)) {
        runtime_uninitialized_failure(line,column);
    }
    const auto* slot=tensor.storage->data.data()+index*tensor_dtype_bytes(tensor.storage->dtype);
    if(tensor.storage->dtype==9){double v{};std::memcpy(&v,slot,8);return v;}
    if(tensor.storage->dtype==10){float v{};std::memcpy(&v,slot,4);return static_cast<double>(v);}
    neural_fail("neural parameter requires float32 or float storage",line,column);
}

void neural_store_float(TensorStorage& storage,std::size_t logical,double value) {
    auto* slot=storage.data.data()+logical*tensor_dtype_bytes(storage.dtype);
    if(storage.dtype==9){const double v=value;std::memcpy(slot,&v,8);}
    else if(storage.dtype==10){const float v=static_cast<float>(value);std::memcpy(slot,&v,4);}
    else runtime_text_failure("invalid neural floating dtype");
}

std::uint64_t neural_splitmix64(std::uint64_t& state) {
    state += 0x9e3779b97f4a7c15ULL;
    auto z=state;
    z=(z^(z>>30U))*0xbf58476d1ce4e5b9ULL;
    z=(z^(z>>27U))*0x94d049bb133111ebULL;
    return z^(z>>31U);
}

unsigned long long neural_parameter_identity(
    void* tensor_raw,unsigned long long line,unsigned long long column) {
    const auto identity=managed_identity(tensor_raw);
    if(identity==0) neural_fail("invalid neural Parameter identity",line,column);
    return static_cast<unsigned long long>(identity);
}

std::shared_ptr<NeuralNode> neural_parameter_node(
    void* parameter_raw,unsigned long long line,unsigned long long column) {
    auto* tensor=neural_parameter_tensor(parameter_raw);
    if(!tensor)neural_fail("null neural Parameter",line,column);
    auto node=neural_constant_node(*tensor,line,column);
    node->parameter_id=neural_parameter_identity(tensor,line,column);
    return node;
}

template <typename T>
std::vector<T> neural_affine_values_t(
    const std::vector<T>& input,const std::vector<long long>& input_shape,
    const std::vector<T>& weight,const std::vector<long long>& weight_shape,
    const std::vector<T>& bias,
    unsigned long long line,unsigned long long column) {
    if(input_shape.empty()||weight_shape.size()!=2)
        neural_fail("affine requires input rank >= 1 and rank-2 weight",line,column);
    const auto in=static_cast<std::size_t>(weight_shape[1]);
    const auto out=static_cast<std::size_t>(weight_shape[0]);
    if(static_cast<std::size_t>(input_shape.back())!=in||bias.size()!=out)
        neural_fail("affine dimensions do not match",line,column);
    const auto batches=in==0?0:input.size()/in;
    std::vector<T> result(batches*out,T{0});
    for(std::size_t batch=0;batch<batches;++batch){
        for(std::size_t o=0;o<out;++o){
            T total=bias[o];
            for(std::size_t i=0;i<in;++i)
                total=static_cast<T>(total+static_cast<T>(
                    input[batch*in+i]*weight[o*in+i]));
            result[batch*out+o]=total;
        }
    }
    return result;
}

NeuralBuffer neural_affine_values(
    const NeuralBuffer& input,const std::vector<long long>& input_shape,
    const NeuralBuffer& weight,const std::vector<long long>& weight_shape,
    const NeuralBuffer& bias,
    unsigned long long line,unsigned long long column) {
    if(input.dtype()!=weight.dtype()||input.dtype()!=bias.dtype())
        neural_fail("affine input and Parameter dtypes must match",line,column);
    if(input.dtype()==10)
        return NeuralBuffer(neural_affine_values_t<float>(
            input.typed<float>(),input_shape,weight.typed<float>(),weight_shape,
            bias.typed<float>(),line,column));
    if(input.dtype()==9)
        return NeuralBuffer(neural_affine_values_t<double>(
            input.typed<double>(),input_shape,weight.typed<double>(),weight_shape,
            bias.typed<double>(),line,column));
    neural_fail("invalid affine dtype",line,column);
}

void* neural_object_pointer_field(void* object,std::size_t offset) {
    void* value=nullptr;
    std::memcpy(&value,static_cast<unsigned char*>(object)+offset,sizeof(value));
    return value;
}
double neural_object_double_field(void* object,std::size_t offset) {
    double value{};
    std::memcpy(&value,static_cast<unsigned char*>(object)+offset,sizeof(value));
    return value;
}
std::uint64_t neural_state_u64(void* state) {
    std::uint64_t value{};
    std::memcpy(&value,state,sizeof(value));
    return value;
}
void neural_set_state_u64(void* state,std::uint64_t value) {
    std::memcpy(state,&value,sizeof(value));
}

void neural_require_same_shape(const NeuralNode& a,const NeuralNode& b,
                               unsigned long long line,unsigned long long column) {
    if(a.shape!=b.shape || neural_node_count(a)!=neural_node_count(b))
        neural_fail("neural operand shapes must match",line,column);
    if(a.dtype!=b.dtype)
        neural_fail("neural operand element types must match",line,column);
    neural_require_same_node_device("neural operation",a,b,line,column);
}

template <typename T>
void neural_apply_unary_t(std::vector<T>& values,int op,const std::vector<long long>& shape,
                          unsigned long long line,unsigned long long column) {
    if(op==1){for(auto&v:values)v=std::abs(v);return;}
    if(op==2){for(auto&v:values)v=std::exp(v);return;}
    if(op==3){
        for(auto&v:values){
            if(!(v>T{0})||!std::isfinite(v))
                neural_fail("logarithm requires finite positive values",line,column);
            v=std::log(v);
        }
        return;
    }
    if(op==4){
        if(values.empty()) neural_fail("mean requires at least one element",line,column);
        T total=T{0};
        for(const auto value:values) total=static_cast<T>(total+value);
        const T average=static_cast<T>(total/static_cast<T>(values.size()));
        values.clear();
        values.push_back(average);
        return;
    }
    if(op==5||op==6){
        if(shape.empty()) neural_fail("last-axis reduction requires rank >= 1",line,column);
        if(shape.back()<=0) neural_fail("last-axis reduction requires a non-empty last axis",line,column);
        const auto width=static_cast<std::size_t>(shape.back());
        for(std::size_t base=0;base<values.size();base+=width){
            T reduced=op==5?T{0}:values[base];
            for(std::size_t j=0;j<width;++j){
                if(op==5) reduced=static_cast<T>(reduced+values[base+j]);
                else reduced=std::max(reduced,values[base+j]);
            }
            for(std::size_t j=0;j<width;++j) values[base+j]=reduced;
        }
        return;
    }
    neural_fail("unknown neural unary operation",line,column);
}



void neural_apply_unary(NeuralBuffer& values,int dtype,int op,const std::vector<long long>& shape,
                        unsigned long long line,unsigned long long column) {
    if(dtype==10) neural_apply_unary_t(values.typed<float>(),op,shape,line,column);
    else if(dtype==9) neural_apply_unary_t(values.typed<double>(),op,shape,line,column);
    else neural_fail("invalid neural dtype",line,column);
}
std::shared_ptr<NeuralNode> neural_unary_node(const std::shared_ptr<NeuralNode>& input,int op,
                                              unsigned long long line,unsigned long long column) {
    auto node=std::make_shared<NeuralNode>(input->dtype);
    node->dtype=input->dtype;
    node->shape=input->shape;
    node->parents={input};
    node->op=op==1?NeuralOp::Absolute:
        op==2?NeuralOp::Exponential:
        op==3?NeuralOp::Logarithm:
        op==4?NeuralOp::Mean:
        op==5?NeuralOp::SumLast:NeuralOp::MaxLast;
    if(input->device_tensor){
        node->device_tensor=static_cast<TensorValue*>(
            quidra_neural_tensor_unary(input->device_tensor,op,line,column));
        if(!node->device_tensor)
            neural_fail("GPU neural unary operation returned null",line,column);
    }else{
        node->data=input->data;
        neural_apply_unary(node->data,node->dtype,op,node->shape,line,column);
    }
    if(op==4) node->shape={};
    return node;
}

template <typename T>
void neural_add_gradient(std::unordered_map<const NeuralNode*,std::vector<T>>& gradients,
                         const std::shared_ptr<NeuralNode>& node,std::vector<T> value) {
    auto& current=gradients[node.get()];
    if(current.empty()) current=std::move(value);
    else{
        if(current.size()!=value.size()) runtime_text_failure("neural gradient size mismatch");
        for(std::size_t i=0;i<current.size();++i)
            current[i]=static_cast<T>(current[i]+value[i]);
    }
}

void neural_topological(const std::shared_ptr<NeuralNode>& node,
                        std::unordered_set<const NeuralNode*>& seen,
                        std::vector<std::shared_ptr<NeuralNode>>& order) {
    if(!node || !seen.insert(node.get()).second) return;
    struct Frame {
        std::shared_ptr<NeuralNode> node;
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

NeuralBuffer neural_cast_buffer(const NeuralBuffer& source,int target_dtype) {
    if(target_dtype!=9&&target_dtype!=10)
        runtime_text_failure("invalid neural cast dtype");
    NeuralBuffer result(target_dtype);
    result.resize(source.size());
    if(target_dtype==10){
        auto& values=result.typed<float>();
        for(std::size_t i=0;i<values.size();++i)
            values[i]=static_cast<float>(source.scalar_as_double(i));
    }else{
        auto& values=result.typed<double>();
        for(std::size_t i=0;i<values.size();++i)
            values[i]=source.scalar_as_double(i);
    }
    return result;
}

std::shared_ptr<NeuralNode> neural_cast_graph(
    const std::shared_ptr<NeuralNode>& root,int target_dtype) {
    if(!root) runtime_text_failure("null neural cast root");
    if(root->dtype==target_dtype) return root;
    std::unordered_set<const NeuralNode*> seen;
    std::vector<std::shared_ptr<NeuralNode>> order;
    neural_topological(root,seen,order);
    std::unordered_map<const NeuralNode*,std::shared_ptr<NeuralNode>> converted;
    converted.reserve(order.size());
    for(const auto& source:order){
        auto node=std::make_shared<NeuralNode>(target_dtype);
        node->dtype=target_dtype;
        node->shape=source->shape;
        node->data=neural_cast_buffer(source->data,target_dtype);
        node->op=source->op;
        node->aux=neural_cast_buffer(source->aux,target_dtype);
        node->aux_index=source->aux_index;
        node->parameter_id=source->parameter_id;
        node->parents.reserve(source->parents.size());
        for(const auto& parent:source->parents){
            const auto found=converted.find(parent.get());
            if(found==converted.end())
                runtime_text_failure("neural cast graph order is invalid");
            node->parents.push_back(found->second);
        }
        converted.emplace(source.get(),std::move(node));
    }
    return converted.at(root.get());
}

struct NeuralMomentRecord {
    std::string path;
    int dtype{10};
    std::uint64_t step{};
    std::vector<long long> shape;
    std::vector<double> first;
    std::vector<double> second;
};

constexpr std::uint64_t neural_moment_state_magic = 0x4e4f554144414d33ULL;

std::size_t neural_checked_add(std::size_t a,std::size_t b,
                               unsigned long long line,unsigned long long column) {
    if(b>std::numeric_limits<std::size_t>::max()-a)
        neural_fail("moment state size overflow",line,column);
    return a+b;
}

std::size_t neural_checked_mul(std::size_t a,std::size_t b,
                               unsigned long long line,unsigned long long column) {
    if(a!=0&&b>std::numeric_limits<std::size_t>::max()/a)
        neural_fail("moment state size overflow",line,column);
    return a*b;
}

std::vector<NeuralMomentRecord> neural_decode_moments(
    void* raw,unsigned long long line,unsigned long long column) {
    if(!raw) neural_fail("null moment state",line,column);
    const auto allocation=managed_allocations.find(reinterpret_cast<std::uintptr_t>(raw));
    if(allocation==managed_allocations.end()||allocation->second.size<8)
        neural_fail("invalid moment state",line,column);
    std::int64_t signed_length{};
    std::memcpy(&signed_length,raw,sizeof(signed_length));
    if(signed_length<0) neural_fail("invalid moment state",line,column);
    const auto length=static_cast<std::size_t>(signed_length);
    if(length>allocation->second.size-8)
        neural_fail("corrupt moment state",line,column);
    if(length==0) return {};

    const auto* bytes=static_cast<const unsigned char*>(raw)+8;
    std::size_t cursor=0;
    const auto need=[&](std::size_t count) {
        if(count>length-cursor) neural_fail("corrupt moment state",line,column);
    };
    auto read_u64=[&]() {
        need(8); std::uint64_t value{}; std::memcpy(&value,bytes+cursor,8); cursor+=8; return value;
    };
    auto read_i32=[&]() {
        need(4); std::int32_t value{}; std::memcpy(&value,bytes+cursor,4); cursor+=4; return value;
    };
    auto read_u32=[&]() {
        need(4); std::uint32_t value{}; std::memcpy(&value,bytes+cursor,4); cursor+=4; return value;
    };
    if(read_u64()!=neural_moment_state_magic)
        neural_fail("unsupported moment state version",line,column);
    const auto record_count=read_u64();
    if(record_count>static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        neural_fail("moment record count overflow",line,column);
    constexpr std::size_t minimum_record_bytes=29;
    if(record_count>static_cast<std::uint64_t>((length-cursor)/minimum_record_bytes))
        neural_fail("corrupt moment record count",line,column);
    std::vector<NeuralMomentRecord> records;
    records.reserve(static_cast<std::size_t>(record_count));
    for(std::size_t record_index=0;record_index<static_cast<std::size_t>(record_count);++record_index) {
        NeuralMomentRecord record;
        record.dtype=read_i32();
        const auto rank=read_u32();
        const auto count_u64=read_u64();
        record.step=read_u64();
        const auto path_length=read_u32();
        need(path_length);
        record.path.assign(
            reinterpret_cast<const char*>(bytes+cursor),
            static_cast<std::size_t>(path_length));
        cursor+=path_length;
        if(record.path.empty()) neural_fail("corrupt moment update Parameter path",line,column);
        if(rank>1024) neural_fail("corrupt moment rank",line,column);
        if(count_u64>static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            neural_fail("moment element count overflow",line,column);
        const auto count=static_cast<std::size_t>(count_u64);
        record.shape.resize(rank);
        std::size_t shape_count=1;
        for(std::size_t axis=0;axis<rank;++axis) {
            need(8);
            std::int64_t dimension{};
            std::memcpy(&dimension,bytes+cursor,8); cursor+=8;
            if(dimension<0) neural_fail("corrupt moment shape",line,column);
            record.shape[axis]=dimension;
            shape_count=neural_checked_mul(shape_count,static_cast<std::size_t>(dimension),line,column);
        }
        if(shape_count!=count) neural_fail("corrupt moment shape/count",line,column);
        const auto vector_bytes=neural_checked_mul(count,sizeof(double),line,column);
        need(neural_checked_mul(vector_bytes,2,line,column));
        record.first.resize(count);
        record.second.resize(count);
        if(vector_bytes){
            std::memcpy(record.first.data(),bytes+cursor,vector_bytes); cursor+=vector_bytes;
            std::memcpy(record.second.data(),bytes+cursor,vector_bytes); cursor+=vector_bytes;
        }
        records.push_back(std::move(record));
    }
    if(cursor!=length) neural_fail("trailing bytes in moment state",line,column);
    return records;
}

void* neural_encode_moments(
    const std::vector<NeuralMomentRecord>& records,
    unsigned long long line,unsigned long long column) {
    std::size_t payload=16;
    for(const auto& record:records){
        if(record.path.empty() ||
           record.path.size()>std::numeric_limits<std::uint32_t>::max())
            neural_fail("invalid moment update Parameter path",line,column);
        payload=neural_checked_add(payload,28,line,column);
        payload=neural_checked_add(payload,record.path.size(),line,column);
        payload=neural_checked_add(
            payload,neural_checked_mul(record.shape.size(),sizeof(std::int64_t),line,column),
            line,column);
        payload=neural_checked_add(
            payload,neural_checked_mul(
                neural_checked_mul(record.first.size(),sizeof(double),line,column),2,line,column),
            line,column);
        if(record.first.size()!=record.second.size())
            neural_fail("invalid moment vectors",line,column);
    }
    if(payload>static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
        neural_fail("moment state too large",line,column);
    auto* raw=static_cast<unsigned char*>(managed_allocate(neural_checked_add(8,payload,line,column)));
    const auto signed_length=static_cast<std::int64_t>(payload);
    std::memcpy(raw,&signed_length,8);
    auto* bytes=raw+8;
    std::size_t cursor=0;
    auto write=[&](const void* source,std::size_t count) {
        if(count){std::memcpy(bytes+cursor,source,count); cursor+=count;}
    };
    write(&neural_moment_state_magic,8);
    const auto record_count=static_cast<std::uint64_t>(records.size());
    write(&record_count,8);
    for(const auto& record:records){
        const auto dtype=static_cast<std::int32_t>(record.dtype);
        const auto rank=static_cast<std::uint32_t>(record.shape.size());
        const auto count=static_cast<std::uint64_t>(record.first.size());
        const auto path_length=static_cast<std::uint32_t>(record.path.size());
        write(&dtype,4); write(&rank,4); write(&count,8); write(&record.step,8);
        write(&path_length,4); write(record.path.data(),record.path.size());
        for(const auto dimension:record.shape) write(&dimension,8);
        write(record.first.data(),record.first.size()*sizeof(double));
        write(record.second.data(),record.second.size()*sizeof(double));
    }
    return raw;
}

void neural_replace_moments(void* optimizer,void* encoded) {
    auto* state=neural_object_pointer_field(optimizer,40);
    if(!state) runtime_text_failure("null moment State");
    auto* old=neural_object_pointer_field(state,0);
    std::memcpy(state,&encoded,sizeof(encoded));
    quidra_managed_release(old,nullptr);
}

const NeuralGradient* neural_gradient_for_parameter(
    void* parameter_raw,void* gradients_raw,
    unsigned long long line,unsigned long long column) {
    if(!parameter_raw||!gradients_raw) neural_fail("null neural update operand",line,column);
    auto* tensor=neural_parameter_tensor(parameter_raw);
    if(!tensor) neural_fail("invalid neural Parameter",line,column);
    auto* gradients=static_cast<NeuralGradients*>(gradients_raw);
    const auto id=neural_parameter_identity(tensor,line,column);
    const auto found=gradients->data->values.find(id);
    if(found==gradients->data->values.end()) return nullptr;
    const auto& gradient=found->second;
    if((gradient.dtype!=9&&gradient.dtype!=10)||
       gradient.shape!=tensor->shape||
       neural_gradient_count(gradient)!=tensor_logical_count(*tensor))
        neural_fail("gradient and Parameter shape mismatch",line,column);
    if((gradient.device_tensor!=nullptr)!=(!tensor_on_cpu(*tensor->storage)))
        neural_fail("gradient and Parameter must be on the same device",line,column);
    if(gradient.device_tensor &&
       gradient.device_tensor->storage->device!=tensor->storage->device)
        neural_fail("gradient and Parameter must use the same gpu(n)",line,column);
    return &gradient;
}


void neural_replace_tensor_value(
    TensorValue& target,TensorValue* source,
    unsigned long long line,unsigned long long column) {
    if(!source||!source->storage)
        neural_fail("invalid GPU optimizer result",line,column);
    if(source->storage->owners==std::numeric_limits<std::size_t>::max())
        neural_fail("GPU optimizer storage ownership overflow",line,column);
    ++source->storage->owners;
    auto* old=target.storage;
    target.storage=source->storage;
    target.shape=source->shape;
    target.strides=source->strides;
    target.offset=source->offset;
    tensor_storage_release(old);
    quidra_tensor_drop(source);
}

void neural_apply_parameter_delta(
    void* parameter_raw,const std::vector<double>& delta,
    unsigned long long line,unsigned long long column) {
    auto* tensor=neural_parameter_tensor(parameter_raw);
    if(!tensor) neural_fail("invalid neural Parameter",line,column);
    if(delta.size()!=tensor_logical_count(*tensor))
        neural_fail("optimizer update size mismatch",line,column);
    tensor_detach_for_write(*tensor, line, column);
    for(std::size_t i=0;i<delta.size();++i){
        const auto storage_index=tensor_storage_index(*tensor,i);
        const auto current=neural_tensor_value(*tensor,i,line,column);
        neural_store_float(*tensor->storage,storage_index,current-delta[i]);
        tracker_set(tensor->storage->initialization,storage_index);
    }
}

} // namespace

extern "C" void* quidra_neural_track(void* raw,unsigned long long line,unsigned long long column) {
    if(!raw)neural_fail("null tensor",line,column);
    return neural_descriptor(neural_constant_node(*static_cast<TensorValue*>(raw),line,column));
}
extern "C" void* quidra_neural_parameter_track(
    void* raw,unsigned long long line,unsigned long long column) {
    if(!raw)neural_fail("null parameter tensor",line,column);
    auto node=neural_constant_node(*static_cast<TensorValue*>(raw),line,column);
    node->parameter_id=neural_parameter_identity(raw,line,column);
    return neural_descriptor(std::move(node));
}

struct NeuralnormalizationLayout {
    std::size_t features{};
    std::size_t inner{};
    std::size_t samples{};
};

NeuralnormalizationLayout neural_normalize_layout(
    const std::vector<long long>& shape,std::size_t count,
    unsigned long long line,unsigned long long column) {
    if(shape.size()<2) neural_fail("normalization requires rank >= 2 with feature/channel axis 1",line,column);
    if(shape[1]<=0) neural_fail("normalization feature/channel dimension must be positive",line,column);
    std::size_t inner=1;
    for(std::size_t axis=2;axis<shape.size();++axis){
        if(shape[axis]<0) neural_fail("normalization shape contains a negative dimension",line,column);
        const auto dim=static_cast<std::size_t>(shape[axis]);
        if(inner!=0 && dim>std::numeric_limits<std::size_t>::max()/inner)
            neural_fail("normalization shape overflow",line,column);
        inner*=dim;
    }
    const auto features=static_cast<std::size_t>(shape[1]);
    if(features!=0 && inner>std::numeric_limits<std::size_t>::max()/features)
        neural_fail("normalization shape overflow",line,column);
    const auto block=features*inner;
    if(block==0 || count%block!=0)
        neural_fail("normalization tensor storage does not match shape",line,column);
    const auto outer=count/block;
    if(outer!=0 && inner>std::numeric_limits<std::size_t>::max()/outer)
        neural_fail("normalization sample count overflow",line,column);
    return NeuralnormalizationLayout{features,inner,outer*inner};
}

std::size_t neural_normalize_feature(
    std::size_t linear,const NeuralnormalizationLayout& layout) {
    return (linear/layout.inner)%layout.features;
}

template <typename T>
std::vector<T> neural_normalize_values_t(
    const std::vector<T>& input,const std::vector<long long>& shape,
    const std::vector<T>& scale,const std::vector<T>& bias,
    const std::vector<T>& mean,const std::vector<T>& variance,double epsilon_raw,
    unsigned long long line,unsigned long long column) {
    const auto layout=neural_normalize_layout(shape,input.size(),line,column);
    if(scale.size()!=layout.features||bias.size()!=layout.features||
       mean.size()!=layout.features||variance.size()!=layout.features)
        neural_fail("normalization feature dimensions do not match",line,column);
    const T epsilon=static_cast<T>(epsilon_raw);
    std::vector<T> output(input.size());
    for(std::size_t i=0;i<input.size();++i){
        const auto feature=neural_normalize_feature(i,layout);
        output[i]=static_cast<T>(
            static_cast<T>((input[i]-mean[feature])/
                           std::sqrt(static_cast<T>(variance[feature]+epsilon)))*
            scale[feature]+bias[feature]);
    }
    return output;
}

NeuralBuffer neural_normalize_values(
    const NeuralBuffer& input,const std::vector<long long>& shape,
    const NeuralBuffer& scale,const NeuralBuffer& bias,
    const NeuralBuffer& mean,const NeuralBuffer& variance,double epsilon,
    unsigned long long line,unsigned long long column) {
    const auto dtype=input.dtype();
    if(scale.dtype()!=dtype||bias.dtype()!=dtype||mean.dtype()!=dtype||variance.dtype()!=dtype)
        neural_fail("normalization input and state dtypes must match",line,column);
    if(dtype==10)
        return NeuralBuffer(neural_normalize_values_t<float>(
            input.typed<float>(),shape,scale.typed<float>(),bias.typed<float>(),
            mean.typed<float>(),variance.typed<float>(),epsilon,line,column));
    if(dtype==9)
        return NeuralBuffer(neural_normalize_values_t<double>(
            input.typed<double>(),shape,scale.typed<double>(),bias.typed<double>(),
            mean.typed<double>(),variance.typed<double>(),epsilon,line,column));
    neural_fail("invalid normalization dtype",line,column);
}

void* neural_normalize_inference(
    void* receiver,void* input_raw,unsigned long long line,unsigned long long column) {
    if(!receiver||!input_raw) neural_fail("null normalization input",line,column);
    auto& input=*static_cast<TensorValue*>(input_raw);
    tensor_require_initialized(input,line,column);
    auto* scale=neural_parameter_tensor(neural_object_pointer_field(receiver,0));
    auto* bias=neural_parameter_tensor(neural_object_pointer_field(receiver,8));
    auto* mean_state=neural_object_pointer_field(receiver,16);
    auto* variance_state=neural_object_pointer_field(receiver,24);
    auto* mean=static_cast<TensorValue*>(neural_object_pointer_field(mean_state,0));
    auto* variance=static_cast<TensorValue*>(neural_object_pointer_field(variance_state,0));
    const double epsilon=neural_object_double_field(receiver,40);
    if(!scale||!bias||!mean||!variance) neural_fail("invalid normalization state",line,column);
    if(input.storage->dtype!=scale->storage->dtype||
       input.storage->dtype!=bias->storage->dtype||
       input.storage->dtype!=mean->storage->dtype||
       input.storage->dtype!=variance->storage->dtype)
        neural_fail("normalization input and state dtypes must match",line,column);
    neural_require_same_tensor_device(
        "neural.normalize_inference", input,
        {scale, bias, mean, variance}, line, column);
    if(tensor_on_cpu(*input.storage)){
        const auto values=neural_normalize_values(
            tensor_float_values(input,line,column),input.shape,
            tensor_float_values(*scale,line,column),tensor_float_values(*bias,line,column),
            tensor_float_values(*mean,line,column),tensor_float_values(*variance,line,column),
            epsilon,line,column);
        return neural_tensor_from_values(input.storage->dtype,input.shape,values);
    }
    tensor_require_initialized(*scale,line,column);
    tensor_require_initialized(*bias,line,column);
    tensor_require_initialized(*mean,line,column);
    tensor_require_initialized(*variance,line,column);
    const auto count=tensor_logical_count(input);
    const auto layout=neural_normalize_layout(input.shape,count,line,column);
    if(tensor_logical_count(*scale)!=layout.features||
       tensor_logical_count(*bias)!=layout.features||
       tensor_logical_count(*mean)!=layout.features||
       tensor_logical_count(*variance)!=layout.features)
        neural_fail("normalization feature dimensions do not match",line,column);

    TensorStorage* in_mat=nullptr;TensorStorage* s_mat=nullptr;TensorStorage* b_mat=nullptr;TensorStorage* m_mat=nullptr;TensorStorage* v_mat=nullptr;
    const TensorStorage* in_store=input.storage;const TensorStorage* s_store=scale->storage;const TensorStorage* b_store=bias->storage;const TensorStorage* m_store=mean->storage;const TensorStorage* v_store=variance->storage;
    if(!tensor_is_contiguous_value(input)||input.offset!=0){in_mat=tensor_gpu_materialize_storage(input,line,column);in_store=in_mat;}
    if(!tensor_is_contiguous_value(*scale)||scale->offset!=0){s_mat=tensor_gpu_materialize_storage(*scale,line,column);s_store=s_mat;}
    if(!tensor_is_contiguous_value(*bias)||bias->offset!=0){b_mat=tensor_gpu_materialize_storage(*bias,line,column);b_store=b_mat;}
    if(!tensor_is_contiguous_value(*mean)||mean->offset!=0){m_mat=tensor_gpu_materialize_storage(*mean,line,column);m_store=m_mat;}
    if(!tensor_is_contiguous_value(*variance)||variance->offset!=0){v_mat=tensor_gpu_materialize_storage(*variance,line,column);v_store=v_mat;}
    auto* output=tensor_storage_create(input.storage->dtype,count,1,input.storage->device,line,column);
    std::string backend_error;
    const bool ok=quidra::device::compute_normalize_inference(
        output->gpu_buffer,in_store->gpu_buffer,s_store->gpu_buffer,b_store->gpu_buffer,
        m_store->gpu_buffer,v_store->gpu_buffer,input.storage->dtype,count,
        layout.features,layout.inner,epsilon,backend_error);
    if(in_mat) tensor_storage_release(in_mat);
    if(s_mat) tensor_storage_release(s_mat);
    if(b_mat) tensor_storage_release(b_mat);
    if(m_mat) tensor_storage_release(m_mat);
    if(v_mat) tensor_storage_release(v_mat);
    if(!ok){tensor_storage_release(output);neural_fail(backend_error.c_str(),line,column);}
    return tensor_descriptor(output,input.shape,tensor_contiguous_strides(input.shape),0);
}

template <typename T>
void* neural_normalize_forward_t(
    void* receiver,const std::shared_ptr<NeuralNode>& input,
    const std::shared_ptr<NeuralNode>& scale,const std::shared_ptr<NeuralNode>& bias,
    unsigned long long line,unsigned long long column) {
    const auto& input_values=input->data.typed<T>();
    const auto& scale_values=scale->data.typed<T>();
    const auto& bias_values=bias->data.typed<T>();
    const auto layout=neural_normalize_layout(input->shape,input_values.size(),line,column);
    const auto features=layout.features;
    const auto samples=layout.samples;
    if(scale_values.size()!=features||bias_values.size()!=features)
        neural_fail("normalization feature dimensions do not match",line,column);
    if(samples==0) neural_fail("normalization training requires at least one sample per feature",line,column);

    std::vector<T> mean(features,T{0}),variance(features,T{0});
    for(std::size_t i=0;i<input_values.size();++i)
        mean[neural_normalize_feature(i,layout)]=static_cast<T>(
            mean[neural_normalize_feature(i,layout)]+input_values[i]);
    const T sample_count=static_cast<T>(samples);
    for(auto& value:mean) value=static_cast<T>(value/sample_count);
    for(std::size_t i=0;i<input_values.size();++i){
        const auto feature=neural_normalize_feature(i,layout);
        const T difference=static_cast<T>(input_values[i]-mean[feature]);
        variance[feature]=static_cast<T>(
            variance[feature]+static_cast<T>(difference*difference));
    }
    for(auto& value:variance) value=static_cast<T>(value/sample_count);

    auto* mean_state=neural_object_pointer_field(receiver,16);
    auto* variance_state=neural_object_pointer_field(receiver,24);
    auto* running_mean=static_cast<TensorValue*>(neural_object_pointer_field(mean_state,0));
    auto* running_variance=static_cast<TensorValue*>(neural_object_pointer_field(variance_state,0));
    if(!running_mean||!running_variance) neural_fail("invalid normalization state",line,column);
    const T momentum=static_cast<T>(neural_object_double_field(receiver,32));
    const T epsilon=static_cast<T>(neural_object_double_field(receiver,40));
    tensor_detach_for_write(*running_mean, line, column);
    tensor_detach_for_write(*running_variance, line, column);
    for(std::size_t feature=0;feature<features;++feature){
        const T old_mean=static_cast<T>(neural_tensor_value(*running_mean,feature,line,column));
        const T old_variance=static_cast<T>(neural_tensor_value(*running_variance,feature,line,column));
        const T next_mean=static_cast<T>(
            static_cast<T>((T{1}-momentum)*old_mean)+static_cast<T>(momentum*mean[feature]));
        const T next_variance=static_cast<T>(
            static_cast<T>((T{1}-momentum)*old_variance)+
            static_cast<T>(momentum*variance[feature]));
        neural_store_float(*running_mean->storage,feature,static_cast<double>(next_mean));
        neural_store_float(*running_variance->storage,feature,static_cast<double>(next_variance));
        tracker_set(running_mean->storage->initialization,feature);
        tracker_set(running_variance->storage->initialization,feature);
    }

    auto node=std::make_shared<NeuralNode>(input->dtype);
    node->shape=input->shape;
    node->data=NeuralBuffer(neural_normalize_values_t<T>(
        input_values,input->shape,scale_values,bias_values,mean,variance,
        static_cast<double>(epsilon),line,column));
    node->op=NeuralOp::Normalize;
    node->parents={input,scale,bias};
    node->aux_index={samples};
    node->aux.resize(features*2);
    auto& backward_cache=node->aux.typed<T>();
    for(std::size_t feature=0;feature<features;++feature){
        backward_cache[feature]=mean[feature];
        backward_cache[features+feature]=static_cast<T>(
            T{1}/std::sqrt(static_cast<T>(variance[feature]+epsilon)));
    }
    return neural_descriptor(std::move(node));
}

void* neural_normalize_training(
    void* receiver,void* input_raw,unsigned long long line,unsigned long long column) {
    if(!receiver||!input_raw) neural_fail("null normalization input",line,column);
    const auto input=static_cast<NeuralValue*>(input_raw)->node;
    auto scale=neural_parameter_node(neural_object_pointer_field(receiver,0),line,column);
    auto bias=neural_parameter_node(neural_object_pointer_field(receiver,8),line,column);
    auto* mean_state=neural_object_pointer_field(receiver,16);
    auto* variance_state=neural_object_pointer_field(receiver,24);
    auto* running_mean=static_cast<TensorValue*>(neural_object_pointer_field(mean_state,0));
    auto* running_variance=static_cast<TensorValue*>(neural_object_pointer_field(variance_state,0));
    if(!running_mean||!running_variance) neural_fail("invalid normalization state",line,column);
    if(input->dtype!=scale->dtype||input->dtype!=bias->dtype||
       input->dtype!=running_mean->storage->dtype||
       input->dtype!=running_variance->storage->dtype)
        neural_fail("normalization input and state dtypes must match",line,column);
    if(input->dtype==10)
        return neural_normalize_forward_t<float>(receiver,input,scale,bias,line,column);
    if(input->dtype==9)
        return neural_normalize_forward_t<double>(receiver,input,scale,bias,line,column);
    neural_fail("invalid normalization dtype",line,column);
}

extern "C" void* quidra_neural_normalize(
    void* input,void* scale,void* bias,void* running_mean,void* running_variance,
    double momentum,double epsilon,bool training,
    unsigned long long line,unsigned long long column) {
    if(!(epsilon>0.0))
        neural_fail("neural.normalize epsilon must be positive",line,column);
    if(training&&!(momentum>=0.0&&momentum<=1.0))
        neural_fail("neural.normalize momentum must be in [0,1]",line,column);
    alignas(void*) unsigned char receiver[48]{};
    std::memcpy(receiver+0,&scale,sizeof(scale));
    std::memcpy(receiver+8,&bias,sizeof(bias));
    std::memcpy(receiver+16,&running_mean,sizeof(running_mean));
    std::memcpy(receiver+24,&running_variance,sizeof(running_variance));
    std::memcpy(receiver+32,&momentum,sizeof(momentum));
    std::memcpy(receiver+40,&epsilon,sizeof(epsilon));
    return training
        ? neural_normalize_training(receiver,input,line,column)
        : neural_normalize_inference(receiver,input,line,column);
}

void* neural_random_mask_apply(
    void* receiver,void* input_raw,unsigned long long line,unsigned long long column) {
    if(!receiver||!input_raw) neural_fail("null random mask input",line,column);
    const auto input=static_cast<NeuralValue*>(input_raw)->node;
    const double rate=neural_object_double_field(receiver,0);
    auto* rng_state=neural_object_pointer_field(receiver,8);
    auto state=neural_state_u64(rng_state);
    auto node=std::make_shared<NeuralNode>(input->dtype);
    node->shape=input->shape;
    node->parents={input};
    node->op=NeuralOp::RandomMask;
    node->data.resize(input->data.size());
    node->aux.resize(input->data.size());
    if(input->dtype==10){
        const float scale=rate==0.0?1.0F:static_cast<float>(1.0/(1.0-rate));
        const auto& source=input->data.typed<float>();
        auto& destination=node->data.typed<float>();
        auto& mask=node->aux.typed<float>();
        for(std::size_t i=0;i<source.size();++i){
            float multiplier=1.0F;
            if(rate>0.0){
                const auto bits=neural_splitmix64(state)>>11U;
                const double unit=static_cast<double>(bits)*(1.0/9007199254740992.0);
                multiplier=unit<rate?0.0F:scale;
            }
            mask[i]=multiplier;
            destination[i]=static_cast<float>(source[i]*multiplier);
        }
    }else if(input->dtype==9){
        const double scale=rate==0.0?1.0:1.0/(1.0-rate);
        const auto& source=input->data.typed<double>();
        auto& destination=node->data.typed<double>();
        auto& mask=node->aux.typed<double>();
        for(std::size_t i=0;i<source.size();++i){
            double multiplier=1.0;
            if(rate>0.0){
                const auto bits=neural_splitmix64(state)>>11U;
                const double unit=static_cast<double>(bits)*(1.0/9007199254740992.0);
                multiplier=unit<rate?0.0:scale;
            }
            mask[i]=multiplier;
            destination[i]=source[i]*multiplier;
        }
    }else{
        neural_fail("invalid random mask dtype",line,column);
    }
    neural_set_state_u64(rng_state,state);
    return neural_descriptor(std::move(node));
}

extern "C" void* quidra_neural_random_mask(
    void* input,void* state,double rate,
    unsigned long long line,unsigned long long column) {
    if(!(rate>=0.0&&rate<1.0))
        neural_fail("neural.random_mask rate must be in [0,1)",line,column);
    alignas(void*) unsigned char receiver[16]{};
    std::memcpy(receiver+0,&rate,sizeof(rate));
    std::memcpy(receiver+8,&state,sizeof(state));
    return neural_random_mask_apply(receiver,input,line,column);
}

template <typename T>
std::vector<T> neural_conv2d_values_t(
    const std::vector<T>& input,const std::vector<long long>& input_shape,
    const std::vector<T>& weight,const std::vector<long long>& weight_shape,
    const std::vector<T>& bias,long long stride,long long padding,
    std::vector<long long>& output_shape,
    unsigned long long line,unsigned long long column) {
    if(input_shape.size()!=4||weight_shape.size()!=4)
        neural_fail("convolution requires NCHW rank-4 input and OIHW rank-4 weight",line,column);
    if(stride<=0||padding<0)
        neural_fail("convolution requires stride > 0 and padding >= 0",line,column);
    const auto n=input_shape[0],in_c=input_shape[1],h=input_shape[2],w=input_shape[3];
    const auto out_c=weight_shape[0],weight_in=weight_shape[1],kh=weight_shape[2],kw=weight_shape[3];
    if(n<0||in_c<=0||h<0||w<0||out_c<=0||weight_in!=in_c||kh<=0||kw<=0||kh!=kw||
       static_cast<long long>(bias.size())!=out_c)
        neural_fail("convolution dimensions do not match",line,column);
    if(padding>(std::numeric_limits<long long>::max()-h)/2||
       padding>(std::numeric_limits<long long>::max()-w)/2)
        neural_fail("convolution padded shape overflow",line,column);
    const auto padded_h=h+2*padding,padded_w=w+2*padding;
    if(padded_h<kh||padded_w<kw)
        neural_fail("convolution kernel is larger than padded input",line,column);
    const auto out_h=(padded_h-kh)/stride+1;
    const auto out_w=(padded_w-kw)/stride+1;
    output_shape={n,out_c,out_h,out_w};
    const auto safe_mul=[&](std::size_t a,std::size_t b){
        if(a!=0&&b>std::numeric_limits<std::size_t>::max()/a)
            neural_fail("convolution output size overflow",line,column);
        return a*b;
    };
    auto count=safe_mul(static_cast<std::size_t>(n),static_cast<std::size_t>(out_c));
    count=safe_mul(count,static_cast<std::size_t>(out_h));
    count=safe_mul(count,static_cast<std::size_t>(out_w));
    std::vector<T> result(count,T{0});
    const auto in_index=[&](long long bn,long long ch,long long y,long long x){
        return static_cast<std::size_t>(((bn*in_c+ch)*h+y)*w+x);
    };
    const auto weight_index=[&](long long oc,long long ic,long long y,long long x){
        return static_cast<std::size_t>(((oc*in_c+ic)*kh+y)*kw+x);
    };
    const auto out_index=[&](long long bn,long long oc,long long y,long long x){
        return static_cast<std::size_t>(((bn*out_c+oc)*out_h+y)*out_w+x);
    };
    for(long long bn=0;bn<n;++bn) for(long long oc=0;oc<out_c;++oc)
        for(long long oy=0;oy<out_h;++oy) for(long long ox=0;ox<out_w;++ox){
            T total=bias[static_cast<std::size_t>(oc)];
            for(long long ic=0;ic<in_c;++ic) for(long long ky=0;ky<kh;++ky)
                for(long long kx=0;kx<kw;++kx){
                    const auto iy=oy*stride+ky-padding;
                    const auto ix=ox*stride+kx-padding;
                    if(iy<0||ix<0||iy>=h||ix>=w) continue;
                    total=static_cast<T>(total+static_cast<T>(
                        input[in_index(bn,ic,iy,ix)]*
                        weight[weight_index(oc,ic,ky,kx)]));
                }
            result[out_index(bn,oc,oy,ox)]=total;
        }
    return result;
}

NeuralBuffer neural_conv2d_values(
    const NeuralBuffer& input,const std::vector<long long>& input_shape,
    const NeuralBuffer& weight,const std::vector<long long>& weight_shape,
    const NeuralBuffer& bias,long long stride,long long padding,
    std::vector<long long>& output_shape,
    unsigned long long line,unsigned long long column) {
    if(input.dtype()!=weight.dtype()||input.dtype()!=bias.dtype())
        neural_fail("convolution input and Parameter dtypes must match",line,column);
    if(input.dtype()==10)
        return NeuralBuffer(neural_conv2d_values_t<float>(
            input.typed<float>(),input_shape,weight.typed<float>(),weight_shape,
            bias.typed<float>(),stride,padding,output_shape,line,column));
    if(input.dtype()==9)
        return NeuralBuffer(neural_conv2d_values_t<double>(
            input.typed<double>(),input_shape,weight.typed<double>(),weight_shape,
            bias.typed<double>(),stride,padding,output_shape,line,column));
    neural_fail("invalid convolution dtype",line,column);
}

extern "C" void* quidra_neural_tensor_convolve2d(
    void* input_raw,void* weight_raw,void* bias_raw,long long stride,long long padding,
    unsigned long long line,unsigned long long column) {
    if(!input_raw) neural_fail("null convolution input",line,column);
    auto& input=*static_cast<TensorValue*>(input_raw);
    tensor_require_initialized(input,line,column);
    auto* weight=neural_parameter_tensor(weight_raw);
    auto* bias=neural_parameter_tensor(bias_raw);
    if(!weight||!bias) neural_fail("null convolution Parameter",line,column);
    if(input.storage->dtype!=weight->storage->dtype||input.storage->dtype!=bias->storage->dtype)
        neural_fail("convolution input and Parameter dtypes must match",line,column);
    neural_require_same_tensor_device(
        "neural.convolve2d", input, {weight, bias}, line, column);
    if(tensor_on_cpu(*input.storage)){
        std::vector<long long> output_shape;
        const auto values=neural_conv2d_values(
            tensor_float_values(input,line,column),input.shape,
            tensor_float_values(*weight,line,column),weight->shape,
            tensor_float_values(*bias,line,column),stride,padding,output_shape,line,column);
        return neural_tensor_from_values(input.storage->dtype,output_shape,values);
    }
    tensor_require_initialized(*weight,line,column);
    tensor_require_initialized(*bias,line,column);
    if(input.shape.size()!=4||weight->shape.size()!=4)
        neural_fail("convolution requires NCHW rank-4 input and OIHW rank-4 weight",line,column);
    if(stride<=0||padding<0)
        neural_fail("convolution requires stride > 0 and padding >= 0",line,column);
    const auto n=input.shape[0],in_c=input.shape[1],height=input.shape[2],width=input.shape[3];
    const auto out_c=weight->shape[0],weight_in=weight->shape[1],kh=weight->shape[2],kw=weight->shape[3];
    if(n<0||in_c<=0||height<0||width<0||out_c<=0||weight_in!=in_c||kh<=0||kw<=0||kh!=kw||
       static_cast<long long>(tensor_logical_count(*bias))!=out_c)
        neural_fail("convolution dimensions do not match",line,column);
    if(padding>(std::numeric_limits<long long>::max()-height)/2||
       padding>(std::numeric_limits<long long>::max()-width)/2)
        neural_fail("convolution padded shape overflow",line,column);
    const auto padded_h=height+2*padding,padded_w=width+2*padding;
    if(padded_h<kh||padded_w<kw)
        neural_fail("convolution kernel is larger than padded input",line,column);
    const auto out_h=(padded_h-kh)/stride+1;
    const auto out_w=(padded_w-kw)/stride+1;
    std::vector<long long> output_shape{n,out_c,out_h,out_w};
    const auto safe_mul=[&](std::size_t a,std::size_t b){
        if(a!=0&&b>std::numeric_limits<std::size_t>::max()/a)
            neural_fail("convolution output size overflow",line,column);
        return a*b;
    };
    auto output_count=safe_mul(static_cast<std::size_t>(n),static_cast<std::size_t>(out_c));
    output_count=safe_mul(output_count,static_cast<std::size_t>(out_h));
    output_count=safe_mul(output_count,static_cast<std::size_t>(out_w));

    TensorStorage* in_mat=nullptr; TensorStorage* w_mat=nullptr; TensorStorage* b_mat=nullptr;
    const TensorStorage* in_store=input.storage; const TensorStorage* w_store=weight->storage; const TensorStorage* b_store=bias->storage;
    if(!tensor_is_contiguous_value(input)||input.offset!=0){in_mat=tensor_gpu_materialize_storage(input,line,column);in_store=in_mat;}
    if(!tensor_is_contiguous_value(*weight)||weight->offset!=0){w_mat=tensor_gpu_materialize_storage(*weight,line,column);w_store=w_mat;}
    if(!tensor_is_contiguous_value(*bias)||bias->offset!=0){b_mat=tensor_gpu_materialize_storage(*bias,line,column);b_store=b_mat;}

    auto* output=tensor_storage_create(input.storage->dtype,output_count,1,input.storage->device,line,column);
    std::string backend_error;
    const bool ok=quidra::device::compute_conv2d(
        output->gpu_buffer,in_store->gpu_buffer,w_store->gpu_buffer,b_store->gpu_buffer,
        input.storage->dtype,static_cast<std::size_t>(n),static_cast<std::size_t>(in_c),
        static_cast<std::size_t>(height),static_cast<std::size_t>(width),
        static_cast<std::size_t>(out_c),static_cast<std::size_t>(kh),static_cast<std::size_t>(kw),
        static_cast<std::size_t>(out_h),static_cast<std::size_t>(out_w),
        static_cast<std::size_t>(stride),static_cast<std::size_t>(padding),backend_error);
    if(in_mat) tensor_storage_release(in_mat);
    if(w_mat) tensor_storage_release(w_mat);
    if(b_mat) tensor_storage_release(b_mat);
    if(!ok){tensor_storage_release(output);neural_fail(backend_error.c_str(),line,column);}
    return tensor_descriptor(output,output_shape,tensor_contiguous_strides(output_shape),0);
}

extern "C" void* quidra_neural_convolve2d(
    void* input_raw,void* weight_raw,void* bias_raw,long long stride,long long padding,
    unsigned long long line,unsigned long long column) {
    if(!input_raw) neural_fail("null convolution input",line,column);
    const auto input=static_cast<NeuralValue*>(input_raw)->node;
    auto weight=neural_parameter_node(weight_raw,line,column);
    auto bias=neural_parameter_node(bias_raw,line,column);
    if(input->dtype!=weight->dtype||input->dtype!=bias->dtype)
        neural_fail("convolution input and Parameter dtypes must match",line,column);
    auto node=std::make_shared<NeuralNode>(input->dtype);
    node->dtype=input->dtype;
    if(input->device_tensor){
        node->device_tensor=static_cast<TensorValue*>(
            quidra_neural_tensor_convolve2d(
                input->device_tensor,weight_raw,bias_raw,stride,padding,line,column));
        if(!node->device_tensor)
            neural_fail("GPU neural convolution returned null",line,column);
        node->shape=node->device_tensor->shape;
    }else{
        node->data=neural_conv2d_values(
            input->data,input->shape,weight->data,weight->shape,bias->data,
            stride,padding,node->shape,line,column);
    }
    node->op=NeuralOp::Convolution;
    node->parents={input,weight,bias};
    node->aux_index={
        static_cast<std::size_t>(stride),
        static_cast<std::size_t>(padding)};
    return neural_descriptor(std::move(node));
}

extern "C" void* quidra_neural_tensor_affine(
    void* input_raw,void* weight_raw,void* bias_raw,
    unsigned long long line,unsigned long long column) {
    if(!input_raw)neural_fail("null affine input",line,column);
    auto& input=*static_cast<TensorValue*>(input_raw);
    tensor_require_initialized(input,line,column);
    auto* weight=neural_parameter_tensor(weight_raw);
    auto* bias=neural_parameter_tensor(bias_raw);
    if(!weight||!bias)neural_fail("null affine Parameter",line,column);
    if(input.storage->dtype!=weight->storage->dtype ||
       input.storage->dtype!=bias->storage->dtype) {
        neural_fail("affine input and Parameter dtypes must match",line,column);
    }
    neural_require_same_tensor_device(
        "neural.affine", input, {weight, bias}, line, column);
    if(tensor_on_cpu(*input.storage)){
        const auto input_values=tensor_float_values(input,line,column);
        const auto weight_values=tensor_float_values(*weight,line,column);
        const auto bias_values=tensor_float_values(*bias,line,column);
        auto output_values=neural_affine_values(
            input_values,input.shape,weight_values,weight->shape,bias_values,line,column);
        auto shape=input.shape;
        shape.back()=weight->shape[0];
        return neural_tensor_from_values(input.storage->dtype,shape,output_values);
    }
    tensor_require_initialized(*weight,line,column);
    tensor_require_initialized(*bias,line,column);
    if(input.shape.empty()||weight->shape.size()!=2)
        neural_fail("affine requires input rank >= 1 and rank-2 weight",line,column);
    const auto features_in=static_cast<std::size_t>(weight->shape[1]);
    const auto features_out=static_cast<std::size_t>(weight->shape[0]);
    if(input.shape.back()!=weight->shape[1]||
       tensor_logical_count(*bias)!=features_out)
        neural_fail("affine dimensions do not match",line,column);
    const auto input_count=tensor_logical_count(input);
    const auto batches=features_in==0?0:input_count/features_in;

    TensorStorage* in_mat=nullptr; TensorStorage* w_mat=nullptr; TensorStorage* b_mat=nullptr;
    const TensorStorage* in_store=input.storage; const TensorStorage* w_store=weight->storage; const TensorStorage* b_store=bias->storage;
    if(!tensor_is_contiguous_value(input)||input.offset!=0){in_mat=tensor_gpu_materialize_storage(input,line,column);in_store=in_mat;}
    if(!tensor_is_contiguous_value(*weight)||weight->offset!=0){w_mat=tensor_gpu_materialize_storage(*weight,line,column);w_store=w_mat;}
    if(!tensor_is_contiguous_value(*bias)||bias->offset!=0){b_mat=tensor_gpu_materialize_storage(*bias,line,column);b_store=b_mat;}

    auto shape=input.shape; shape.back()=weight->shape[0];
    const auto output_count=batches*features_out;
    auto* output=tensor_storage_create(input.storage->dtype,output_count,1,input.storage->device,line,column);
    std::string backend_error;
    const bool ok=quidra::device::compute_affine(
        output->gpu_buffer,in_store->gpu_buffer,w_store->gpu_buffer,b_store->gpu_buffer,
        input.storage->dtype,batches,features_in,features_out,backend_error);
    if(in_mat) tensor_storage_release(in_mat);
    if(w_mat) tensor_storage_release(w_mat);
    if(b_mat) tensor_storage_release(b_mat);
    if(!ok){tensor_storage_release(output);neural_fail(backend_error.c_str(),line,column);}
    return tensor_descriptor(output,shape,tensor_contiguous_strides(shape),0);
}

extern "C" void* quidra_neural_affine(
    void* input_raw,void* weight_raw,void* bias_raw,
    unsigned long long line,unsigned long long column) {
    if(!input_raw)neural_fail("null affine input",line,column);
    const auto input=static_cast<NeuralValue*>(input_raw)->node;
    auto weight=neural_parameter_node(weight_raw,line,column);
    auto bias=neural_parameter_node(bias_raw,line,column);
    if(input->dtype!=weight->dtype||input->dtype!=bias->dtype) {
        neural_fail("affine input and Parameter dtypes must match",line,column);
    }
    auto node=std::make_shared<NeuralNode>(input->dtype);
    node->dtype=input->dtype;
    node->shape=input->shape;
    if(node->shape.empty())neural_fail("affine requires input rank >= 1",line,column);
    node->shape.back()=weight->shape[0];
    if(input->device_tensor){
        node->device_tensor=static_cast<TensorValue*>(
            quidra_neural_tensor_affine(
                input->device_tensor,weight_raw,bias_raw,line,column));
        if(!node->device_tensor)
            neural_fail("GPU neural affine returned null",line,column);
    }else{
        node->data=neural_affine_values(
            input->data,input->shape,weight->data,weight->shape,bias->data,line,column);
    }
    node->op=NeuralOp::Affine;
    node->parents={input,weight,bias};
    return neural_descriptor(std::move(node));
}
extern "C" bool quidra_neural_parameter_has_gradient(
    void* parameter,void* gradients_raw,
    unsigned long long line,unsigned long long column) {
    return neural_gradient_for_parameter(parameter,gradients_raw,line,column)!=nullptr;
}

template <typename T>
bool neural_update_parameter_t(
    void* parameter,const NeuralGradient& gradient,double rate_raw,
    unsigned long long line,unsigned long long column) {
    auto* tensor=neural_parameter_tensor(parameter);
    if(!tensor) neural_fail("invalid neural Parameter",line,column);
    const auto& values=gradient.data.typed<T>();
    if(values.size()!=tensor_logical_count(*tensor))
        neural_fail("optimizer update size mismatch",line,column);
    const T rate=static_cast<T>(rate_raw);
    tensor_detach_for_write(*tensor, line, column);
    for(std::size_t i=0;i<values.size();++i){
        const auto storage_index=tensor_storage_index(*tensor,i);
        const T current=static_cast<T>(neural_tensor_value(*tensor,i,line,column));
        const T delta=static_cast<T>(rate*values[i]);
        const T next=static_cast<T>(current-delta);
        neural_store_float(*tensor->storage,storage_index,static_cast<double>(next));
        tracker_set(tensor->storage->initialization,storage_index);
    }
    return true;
}

extern "C" bool quidra_neural_update_parameter(
    void* parameter,void* gradients_raw,double rate,
    unsigned long long line,unsigned long long column) {
    if(!std::isfinite(rate)||rate<=0.0)
        neural_fail("neural.update rate must be finite and positive",line,column);
    const auto* gradient=neural_gradient_for_parameter(
        parameter,gradients_raw,line,column);
    if(!gradient) return false;
    if(gradient->device_tensor){
        auto* tensor=neural_parameter_tensor(parameter);
        if(!tensor) neural_fail("invalid neural Parameter",line,column);
        void* scaled_raw=nullptr;
        if(gradient->dtype==10){
            float scalar=static_cast<float>(rate);
            scaled_raw=quidra_tensor_binary(
                gradient->device_tensor,nullptr,&scalar,2,3,line,column);
        }else if(gradient->dtype==9){
            double scalar=rate;
            scaled_raw=quidra_tensor_binary(
                gradient->device_tensor,nullptr,&scalar,2,3,line,column);
        }else{
            neural_fail("invalid neural gradient dtype",line,column);
        }
        auto* scaled=static_cast<TensorValue*>(scaled_raw);
        auto* next=static_cast<TensorValue*>(
            quidra_tensor_binary(tensor,scaled,nullptr,0,2,line,column));
        quidra_tensor_drop(scaled);
        neural_replace_tensor_value(*tensor,next,line,column);
        return true;
    }
    if(gradient->dtype==10)
        return neural_update_parameter_t<float>(parameter,*gradient,rate,line,column);
    if(gradient->dtype==9)
        return neural_update_parameter_t<double>(parameter,*gradient,rate,line,column);
    neural_fail("invalid neural gradient dtype",line,column);
}

extern "C" long long quidra_neural_moment_begin(
    void* optimizer,unsigned long long parameter_count,
    unsigned long long line,unsigned long long column) {
    if(!optimizer) neural_fail("null moment update optimizer",line,column);
    const double rate=neural_object_double_field(optimizer,0);
    const double beta1=neural_object_double_field(optimizer,8);
    const double beta2=neural_object_double_field(optimizer,16);
    const double epsilon=neural_object_double_field(optimizer,24);
    if(!std::isfinite(rate)||rate<=0.0||!std::isfinite(beta1)||beta1<0.0||beta1>=1.0||
       !std::isfinite(beta2)||beta2<0.0||beta2>=1.0||
       !std::isfinite(epsilon)||epsilon<=0.0)
        neural_fail("invalid moment update optimizer state",line,column);
    auto* step_state=neural_object_pointer_field(optimizer,32);
    auto* moments_state=neural_object_pointer_field(optimizer,40);
    if(!step_state||!moments_state) neural_fail("invalid moment update State fields",line,column);
    const auto step=neural_state_u64(step_state);
    if(step>=static_cast<std::uint64_t>(std::numeric_limits<long long>::max()))
        neural_fail("moment update step counter overflow",line,column);
    const auto records=neural_decode_moments(
        neural_object_pointer_field(moments_state,0),line,column);
    if((step==0&&!records.empty())||
       (step>0&&records.size()!=parameter_count))
        neural_fail("moment update state does not match model Parameter structure",line,column);
    return static_cast<long long>(step+1);
}

extern "C" void quidra_neural_moment_validate_parameter(
    void* parameter,void* gradients_raw,void* optimizer,void* path_raw,
    unsigned long long index,
    unsigned long long line,unsigned long long column) {
    if(!optimizer) neural_fail("null moment update optimizer",line,column);
    const auto* parameter_path=static_cast<const char*>(path_raw);
    if(!parameter_path||!*parameter_path)
        neural_fail("invalid moment update Parameter path",line,column);
    auto* tensor=neural_parameter_tensor(parameter);
    if(!tensor) neural_fail("invalid neural Parameter",line,column);
    auto* moments_state=neural_object_pointer_field(optimizer,40);
    if(!moments_state) neural_fail("invalid moment update moments State",line,column);
    const auto records=neural_decode_moments(
        neural_object_pointer_field(moments_state,0),line,column);
    if(records.empty()) return;
    if(index>=records.size())
        neural_fail("moment update Parameter traversal changed",line,column);
    const auto& record=records[static_cast<std::size_t>(index)];
    const auto logical_count=tensor_logical_count(*tensor);
    if(record.path!=parameter_path)
        neural_fail("moment update state does not match Parameter structural path",line,column);
    if(record.dtype!=tensor->storage->dtype||record.shape!=tensor->shape||
       record.first.size()!=logical_count||record.second.size()!=logical_count)
        neural_fail("moment update state does not match Parameter dtype/shape",line,column);
    const auto* gradient=neural_gradient_for_parameter(
        parameter,gradients_raw,line,column);
    if(gradient&&record.step==std::numeric_limits<std::uint64_t>::max())
        neural_fail("moment update Parameter step counter overflow",line,column);
}

extern "C" void quidra_neural_moment_finish(
    void* optimizer,long long next_step,
    unsigned long long line,unsigned long long column) {
    if(!optimizer||next_step<=0) neural_fail("invalid moment update step",line,column);
    auto* step_state=neural_object_pointer_field(optimizer,32);
    if(!step_state) neural_fail("invalid moment update step State",line,column);
    const auto current=neural_state_u64(step_state);
    const auto expected=static_cast<std::uint64_t>(next_step);
    if(current==std::numeric_limits<std::uint64_t>::max()||current+1!=expected)
        neural_fail("moment update step State changed during update",line,column);
    neural_set_state_u64(step_state,expected);
}

extern "C" bool quidra_neural_moment_update_parameter(
    void* parameter,void* gradients_raw,void* optimizer,void* path_raw,
    unsigned long long index,long long step,
    unsigned long long line,unsigned long long column) {
    if(!optimizer||step<=0) neural_fail("invalid moment update step",line,column);
    const auto* parameter_path=static_cast<const char*>(path_raw);
    if(!parameter_path||!*parameter_path)
        neural_fail("invalid moment update Parameter path",line,column);
    auto* tensor=neural_parameter_tensor(parameter);
    if(!tensor) neural_fail("invalid neural Parameter",line,column);
    auto* moments_state=neural_object_pointer_field(optimizer,40);
    if(!moments_state) neural_fail("invalid moment update moments State",line,column);
    auto records=neural_decode_moments(
        neural_object_pointer_field(moments_state,0),line,column);
    if(index>records.size()) neural_fail("moment update Parameter traversal changed",line,column);
    const auto logical_count=tensor_logical_count(*tensor);
    if(index==records.size()){
        NeuralMomentRecord record;
        record.path=parameter_path;
        record.dtype=tensor->storage->dtype;
        record.shape=tensor->shape;
        record.first.assign(logical_count,0.0);
        record.second.assign(logical_count,0.0);
        records.push_back(std::move(record));
    }
    auto& record=records[static_cast<std::size_t>(index)];
    if(record.path!=parameter_path)
        neural_fail("moment update state does not match Parameter structural path",line,column);
    if(record.dtype!=tensor->storage->dtype||record.shape!=tensor->shape||
       record.first.size()!=logical_count||record.second.size()!=logical_count)
        neural_fail("moment update state does not match Parameter dtype/shape",line,column);

    const auto* gradient=neural_gradient_for_parameter(parameter,gradients_raw,line,column);
    bool matched=gradient!=nullptr;
    if(gradient&&gradient->device_tensor)
        neural_fail(
            "neural.moment_update GPU gradients require the DNN GPU moment backend",
            line,column);
    if(matched){
        if(record.step==std::numeric_limits<std::uint64_t>::max())
            neural_fail("moment update Parameter step counter overflow",line,column);
        ++record.step;
        const double rate=neural_object_double_field(optimizer,0);
        const double beta1=neural_object_double_field(optimizer,8);
        const double beta2=neural_object_double_field(optimizer,16);
        const double epsilon=neural_object_double_field(optimizer,24);
        const double correction1=1.0-std::pow(beta1,static_cast<double>(record.step));
        const double correction2=1.0-std::pow(beta2,static_cast<double>(record.step));
        if(correction1<=0.0||correction2<=0.0)
            neural_fail("invalid moment update bias correction",line,column);
        std::vector<double> delta(logical_count);
        for(std::size_t i=0;i<logical_count;++i){
            const double g=gradient->data.scalar_as_double(i);
            record.first[i]=beta1*record.first[i]+(1.0-beta1)*g;
            record.second[i]=beta2*record.second[i]+(1.0-beta2)*g*g;
            const double mhat=record.first[i]/correction1;
            const double vhat=record.second[i]/correction2;
            delta[i]=rate*mhat/(std::sqrt(vhat)+epsilon);
        }
        neural_apply_parameter_delta(parameter,delta,line,column);
    }
    auto* encoded=neural_encode_moments(records,line,column);
    neural_replace_moments(optimizer,encoded);
    return matched;
}

extern "C" void quidra_neural_validate_step(
    void* gradients_raw,unsigned long long matched,
    unsigned long long line,unsigned long long column) {
    if(!gradients_raw) neural_fail("null gradients",line,column);
    const auto count=static_cast<NeuralGradients*>(gradients_raw)->data->values.size();
    if(matched!=count)
        neural_fail("Gradients contain Parameters that do not belong to the supplied model",line,column);
}

namespace {

struct NeuralStateReadRequest {
    std::string path;
    int kind{};
    void* address{};
};

struct NeuralStateContext {
    std::string path;
    std::vector<unsigned char> data;
    std::size_t cursor{};
    std::size_t end{};
    std::size_t payload_start{};
    std::vector<NeuralStateReadRequest> reads;
};

[[noreturn]] void neural_state_fail(
    const char* message,unsigned long long line,unsigned long long column) {
    std::fprintf(stderr,
        "Quidra runtime error[NEURAL_STATE] at %llu:%llu: %s\n",
        line,column,message);
    std::exit(101);
}

bool neural_state_path_valid(const char* raw) {
    if(!raw) return false;
    const std::string_view path(raw);
    constexpr std::string_view suffix=".quistate";
    return path.size()>=suffix.size() &&
           path.substr(path.size()-suffix.size())==suffix;
}

void neural_state_append_u32(std::vector<unsigned char>& out,std::uint32_t value) {
    for(unsigned i=0;i<4;++i) out.push_back(static_cast<unsigned char>((value>>(i*8U))&0xffU));
}

void neural_state_append_u64(std::vector<unsigned char>& out,std::uint64_t value) {
    for(unsigned i=0;i<8;++i) out.push_back(static_cast<unsigned char>((value>>(i*8U))&0xffU));
}

std::uint32_t neural_state_read_u32(
    NeuralStateContext& context,unsigned long long line,unsigned long long column) {
    if(context.cursor>context.end || context.end-context.cursor<4)
        neural_state_fail("truncated .quistate file",line,column);
    std::uint32_t value=0;
    for(unsigned i=0;i<4;++i)
        value|=static_cast<std::uint32_t>(context.data[context.cursor++])<<(i*8U);
    return value;
}

std::uint64_t neural_state_read_u64(
    NeuralStateContext& context,unsigned long long line,unsigned long long column) {
    if(context.cursor>context.end || context.end-context.cursor<8)
        neural_state_fail("truncated .quistate file",line,column);
    std::uint64_t value=0;
    for(unsigned i=0;i<8;++i)
        value|=static_cast<std::uint64_t>(context.data[context.cursor++])<<(i*8U);
    return value;
}

void neural_state_append_raw(
    std::vector<unsigned char>& out,const void* source,std::size_t width) {
    if(!source) runtime_text_failure("null .quistate scalar");
    const auto* bytes=static_cast<const unsigned char*>(source);
    if constexpr(std::endian::native==std::endian::little) {
        out.insert(out.end(),bytes,bytes+width);
    } else {
        for(std::size_t i=0;i<width;++i) out.push_back(bytes[width-1-i]);
    }
}

void neural_state_read_raw(
    NeuralStateContext& context,void* destination,std::size_t width,
    unsigned long long line,unsigned long long column) {
    if(!destination) neural_state_fail("null .quistate load target",line,column);
    if(context.cursor>context.end || width>context.end-context.cursor)
        neural_state_fail("truncated .quistate file",line,column);
    auto* bytes=static_cast<unsigned char*>(destination);
    if constexpr(std::endian::native==std::endian::little) {
        std::memcpy(bytes,context.data.data()+context.cursor,width);
    } else {
        for(std::size_t i=0;i<width;++i)
            bytes[width-1-i]=context.data[context.cursor+i];
    }
    context.cursor+=width;
}

std::uint64_t neural_state_checksum(
    const unsigned char* data,std::size_t size) {
    std::uint64_t hash=1469598103934665603ULL;
    for(std::size_t i=0;i<size;++i){
        hash^=static_cast<std::uint64_t>(data[i]);
        hash*=1099511628211ULL;
    }
    return hash;
}

std::size_t neural_state_scalar_width(int kind) {
    if(kind>=1&&kind<=10) return tensor_dtype_bytes(kind);
    if(kind==11) return 1;
    return 0;
}

void neural_state_append_path(
    NeuralStateContext& context,const char* path,int kind,
    unsigned long long line,unsigned long long column) {
    if(!path) neural_state_fail("null .quistate field path",line,column);
    const auto length=std::strlen(path);
    if(length>std::numeric_limits<std::uint32_t>::max())
        neural_state_fail(".quistate field path is too long",line,column);
    neural_state_append_u32(context.data,static_cast<std::uint32_t>(length));
    context.data.insert(context.data.end(),path,path+length);
    context.data.push_back(static_cast<unsigned char>(kind));
}

void neural_state_expect_path(
    NeuralStateContext& context,const char* path,int kind,
    unsigned long long line,unsigned long long column) {
    if(!path) neural_state_fail("null .quistate field path",line,column);
    const auto length=neural_state_read_u32(context,line,column);
    if(context.cursor>=context.end ||
       static_cast<std::size_t>(length)>context.end-context.cursor-1)
        neural_state_fail("truncated .quistate field header",line,column);
    const std::string_view stored(
        reinterpret_cast<const char*>(context.data.data()+context.cursor),length);
    context.cursor+=length;
    const int stored_kind=context.data[context.cursor++];
    if(stored!=path || stored_kind!=kind)
        neural_state_fail(".quistate schema/field mismatch",line,column);
}

void neural_state_write_tensor(
    NeuralStateContext& context,TensorValue& tensor,
    unsigned long long line,unsigned long long column) {
    tensor_require_cpu(*tensor.storage, "neural.save", line, column);
    tensor_require_initialized(tensor,line,column);
    neural_state_append_u32(
        context.data,static_cast<std::uint32_t>(tensor.storage->dtype));
    if(tensor.shape.size()>std::numeric_limits<std::uint32_t>::max())
        neural_state_fail("tensor rank is too large for .quistate",line,column);
    neural_state_append_u32(
        context.data,static_cast<std::uint32_t>(tensor.shape.size()));
    for(const auto dimension:tensor.shape)
        neural_state_append_u64(context.data,static_cast<std::uint64_t>(dimension));
    const auto count=tensor_logical_count(tensor);
    neural_state_append_u64(context.data,static_cast<std::uint64_t>(count));
    const auto width=tensor_dtype_bytes(tensor.storage->dtype);
    for(std::size_t i=0;i<count;++i){
        const auto index=tensor_storage_index(tensor,i);
        neural_state_append_raw(
            context.data,tensor.storage->data.data()+index*width,width);
    }
}

void neural_state_read_tensor(
    NeuralStateContext& context,void* address,bool apply,
    unsigned long long line,unsigned long long column) {
    if(!address) neural_state_fail("null tensor load target",line,column);
    void* raw=nullptr;
    std::memcpy(&raw,address,sizeof(raw));
    auto* tensor=static_cast<TensorValue*>(raw);
    if(!tensor) neural_state_fail("null tensor load target",line,column);
    tensor_require_initialized(*tensor,line,column);

    const auto dtype=static_cast<int>(neural_state_read_u32(context,line,column));
    const auto rank=neural_state_read_u32(context,line,column);
    if(dtype!=tensor->storage->dtype ||
       static_cast<std::size_t>(rank)!=tensor->shape.size())
        neural_state_fail(".quistate tensor dtype/shape mismatch",line,column);
    if(static_cast<std::size_t>(rank)>(context.end-context.cursor)/8)
        neural_state_fail("truncated .quistate tensor shape",line,column);
    for(std::uint32_t i=0;i<rank;++i){
        const auto dimension=neural_state_read_u64(context,line,column);
        if(dimension>static_cast<std::uint64_t>(std::numeric_limits<long long>::max()))
            neural_state_fail(".quistate tensor dimension is too large",line,column);
        if(static_cast<long long>(dimension)!=tensor->shape[i])
            neural_state_fail(".quistate tensor dtype/shape mismatch",line,column);
    }
    const auto count=neural_state_read_u64(context,line,column);

    if(count!=tensor_logical_count(*tensor))
        neural_state_fail(".quistate tensor dtype/shape mismatch",line,column);

    const auto width=tensor_dtype_bytes(dtype);
    if(count>static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()/width))
        neural_state_fail(".quistate tensor payload is too large",line,column);
    const auto payload_bytes=static_cast<std::size_t>(count)*width;
    if(payload_bytes>context.end-context.cursor)
        neural_state_fail("truncated .quistate tensor",line,column);

    if(!apply){
        context.cursor+=payload_bytes;
        return;
    }

    tensor_detach_for_write(*tensor, line, column);
    for(std::size_t i=0;i<static_cast<std::size_t>(count);++i){
        const auto index=tensor_storage_index(*tensor,i);
        neural_state_read_raw(
            context,tensor->storage->data.data()+index*width,width,line,column);
        tracker_set(tensor->storage->initialization,index);
    }
}

void neural_state_process_read(
    NeuralStateContext& context,const char* path,int kind,void* address,bool apply,
    unsigned long long line,unsigned long long column) {
    if(!address) neural_state_fail("null .quistate load target",line,column);
    neural_state_expect_path(context,path,kind,line,column);

    const auto scalar_width=neural_state_scalar_width(kind);
    if(scalar_width){
        if(scalar_width>context.end-context.cursor)
            neural_state_fail("truncated .quistate scalar",line,column);
        if(apply) neural_state_read_raw(context,address,scalar_width,line,column);
        else context.cursor+=scalar_width;
        return;
    }
    if(kind==12){
        const auto length=neural_state_read_u64(context,line,column);
        if(length>context.end-context.cursor)
            neural_state_fail("truncated .quistate string",line,column);
        const auto text_size=static_cast<std::size_t>(length);
        const std::string_view text(
            reinterpret_cast<const char*>(context.data.data()+context.cursor),
            text_size);
        if(!valid_runtime_text(text))
            neural_state_fail(
                ".quistate string is not valid UTF-8 text without NUL",
                line,column);
        if(!apply){
            context.cursor+=text_size;
            return;
        }
        std::string value(text);
        context.cursor+=text_size;
        auto* replacement=runtime_copy_string(value);
        void* old=nullptr;
        std::memcpy(&old,address,sizeof(old));
        std::memcpy(address,&replacement,sizeof(replacement));
        quidra_managed_release(old,nullptr);
        return;
    }
    if(kind==13){
        const auto length=neural_state_read_u64(context,line,column);
        if(length>context.end-context.cursor ||
           length>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
           length>static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()-8))
            neural_state_fail("truncated or oversized .quistate bytes",line,column);
        if(!apply){
            context.cursor+=static_cast<std::size_t>(length);
            return;
        }
        const auto total=static_cast<std::size_t>(length)+8;
        auto* replacement=static_cast<unsigned char*>(managed_allocate(total));
        const auto signed_length=static_cast<std::int64_t>(length);
        std::memcpy(replacement,&signed_length,8);
        if(length)
            std::memcpy(replacement+8,context.data.data()+context.cursor,
                        static_cast<std::size_t>(length));
        context.cursor+=static_cast<std::size_t>(length);
        void* old=nullptr;
        std::memcpy(&old,address,sizeof(old));
        void* replacement_raw=replacement;
        std::memcpy(address,&replacement_raw,sizeof(replacement_raw));
        quidra_managed_release(old,nullptr);
        return;
    }
    if(kind==14){
        neural_state_read_tensor(context,address,apply,line,column);
        return;
    }
    neural_state_fail("unsupported .quistate field kind",line,column);
}

} // namespace

extern "C" void* quidra_neural_state_save_begin(
    void* path_raw,void* schema_raw,
    unsigned long long line,unsigned long long column) {
    const auto* path=static_cast<const char*>(path_raw);
    const auto* schema=static_cast<const char*>(schema_raw);
    if(!neural_state_path_valid(path))
        neural_state_fail("neural.save path must end with .quistate",line,column);
    if(!schema) neural_state_fail("null .quistate schema",line,column);
    auto* context=new NeuralStateContext();
    context->path=path;
    constexpr char magic[]="QUIDRASTATE";
    context->data.insert(context->data.end(),magic,magic+sizeof(magic)-1);
    neural_state_append_u32(context->data,1);
    const auto schema_length=std::strlen(schema);
    neural_state_append_u64(context->data,static_cast<std::uint64_t>(schema_length));
    context->data.insert(context->data.end(),schema,schema+schema_length);
    return context;
}

extern "C" void quidra_neural_state_write(
    void* raw_context,void* path_raw,int kind,void* raw,
    unsigned long long line,unsigned long long column) {
    if(!raw_context) neural_state_fail("null .quistate save context",line,column);
    auto& context=*static_cast<NeuralStateContext*>(raw_context);
    const auto* path=static_cast<const char*>(path_raw);
    neural_state_append_path(context,path,kind,line,column);

    const auto scalar_width=neural_state_scalar_width(kind);
    if(scalar_width){
        neural_state_append_raw(context.data,raw,scalar_width);
        return;
    }
    if(kind==12){
        const auto* text=static_cast<const char*>(raw);
        if(!text) neural_state_fail("null string in .quistate",line,column);
        if(!valid_runtime_text(text))
            neural_state_fail(
                "string in .quistate is not valid UTF-8 text without NUL",
                line,column);
        const auto length=std::strlen(text);
        neural_state_append_u64(context.data,static_cast<std::uint64_t>(length));
        context.data.insert(context.data.end(),text,text+length);
        return;
    }
    if(kind==13){
        if(!raw) neural_state_fail("null bytes in .quistate",line,column);
        std::int64_t signed_length{};
        std::memcpy(&signed_length,raw,8);
        if(signed_length<0) neural_state_fail("invalid bytes length in .quistate",line,column);
        const auto length=static_cast<std::size_t>(signed_length);
        neural_state_append_u64(context.data,static_cast<std::uint64_t>(length));
        const auto* data=static_cast<const unsigned char*>(raw)+8;
        context.data.insert(context.data.end(),data,data+length);
        return;
    }
    if(kind==14){
        if(!raw) neural_state_fail("null tensor in .quistate",line,column);
        neural_state_write_tensor(
            context,*static_cast<TensorValue*>(raw),line,column);
        return;
    }
    neural_state_fail("unsupported .quistate field kind",line,column);
}

extern "C" void quidra_neural_state_save_finish(
    void* raw_context,unsigned long long line,unsigned long long column) {
    if(!raw_context) neural_state_fail("null .quistate save context",line,column);
    std::unique_ptr<NeuralStateContext> context(
        static_cast<NeuralStateContext*>(raw_context));
    const auto checksum=neural_state_checksum(
        context->data.data(),context->data.size());
    neural_state_append_u64(context->data,checksum);

    const std::filesystem::path target(context->path);
    static std::atomic<std::uint64_t> temporary_sequence{0};
    const auto sequence=temporary_sequence.fetch_add(1,std::memory_order_relaxed);
    if(sequence==std::numeric_limits<std::uint64_t>::max())
        neural_state_fail("temporary .quistate sequence exhausted",line,column);
#ifdef _WIN32
    const auto process_id=static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    const auto process_id=static_cast<std::uint64_t>(getpid());
#endif
    auto temporary=target;
    temporary+=std::string(".tmp.")+std::to_string(process_id)+"."+
        std::to_string(sequence);
    std::error_code ignored;
    std::filesystem::remove(temporary,ignored);

    {
        std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
        if(!output)
            neural_state_fail("cannot open temporary .quistate file for writing",line,column);
        output.write(
            reinterpret_cast<const char*>(context->data.data()),
            static_cast<std::streamsize>(context->data.size()));
        output.flush();
        if(!output){
            output.close();
            std::filesystem::remove(temporary,ignored);
            neural_state_fail("failed to write .quistate file",line,column);
        }
    }

#ifdef _WIN32
    if(!MoveFileExW(
            temporary.c_str(),target.c_str(),
            MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        std::filesystem::remove(temporary,ignored);
        neural_state_fail("failed to replace .quistate file",line,column);
    }
#else
    std::error_code rename_error;
    std::filesystem::rename(temporary,target,rename_error);
    if(rename_error){
        std::filesystem::remove(temporary,ignored);
        neural_state_fail("failed to replace .quistate file",line,column);
    }
#endif
}

extern "C" void* quidra_neural_state_load_begin(
    void* path_raw,void* schema_raw,
    unsigned long long line,unsigned long long column) {
    const auto* path=static_cast<const char*>(path_raw);
    const auto* schema=static_cast<const char*>(schema_raw);
    if(!neural_state_path_valid(path))
        neural_state_fail("neural.load path must end with .quistate",line,column);
    if(!schema) neural_state_fail("null .quistate schema",line,column);

    std::ifstream input(path,std::ios::binary);
    if(!input) neural_state_fail("cannot open .quistate file for reading",line,column);
    std::vector<unsigned char> data{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    constexpr char magic[]="QUIDRASTATE";
    constexpr std::size_t minimum=(sizeof(magic)-1)+4+8+8;
    if(data.size()<minimum) neural_state_fail("truncated .quistate file",line,column);

    std::uint64_t stored_checksum=0;
    for(unsigned i=0;i<8;++i)
        stored_checksum|=static_cast<std::uint64_t>(data[data.size()-8+i])<<(i*8U);
    const auto computed=neural_state_checksum(data.data(),data.size()-8);
    if(stored_checksum!=computed)
        neural_state_fail(".quistate checksum mismatch",line,column);

    auto* context=new NeuralStateContext();
    context->path=path;
    context->data=std::move(data);
    context->end=context->data.size()-8;

    if(context->end<sizeof(magic)-1 ||
       std::memcmp(context->data.data(),magic,sizeof(magic)-1)!=0){
        delete context;
        neural_state_fail("invalid .quistate magic",line,column);
    }
    context->cursor=sizeof(magic)-1;
    const auto version=neural_state_read_u32(*context,line,column);
    if(version!=1){
        delete context;
        neural_state_fail("unsupported .quistate version",line,column);
    }
    const auto schema_length=neural_state_read_u64(*context,line,column);
    if(schema_length>context->end-context->cursor){
        delete context;
        neural_state_fail("truncated .quistate schema",line,column);
    }
    const std::string_view stored_schema(
        reinterpret_cast<const char*>(context->data.data()+context->cursor),
        static_cast<std::size_t>(schema_length));
    context->cursor+=static_cast<std::size_t>(schema_length);
    if(stored_schema!=schema){
        delete context;
        neural_state_fail(".quistate schema mismatch",line,column);
    }
    context->payload_start=context->cursor;
    return context;
}

extern "C" void quidra_neural_state_read(
    void* raw_context,void* path_raw,int kind,void* address,
    unsigned long long line,unsigned long long column) {
    if(!raw_context) neural_state_fail("null .quistate load context",line,column);
    auto& context=*static_cast<NeuralStateContext*>(raw_context);
    const auto* path=static_cast<const char*>(path_raw);
    if(!path) neural_state_fail("null .quistate field path",line,column);
    neural_state_process_read(context,path,kind,address,false,line,column);
    context.reads.push_back(NeuralStateReadRequest{path,kind,address});
}

extern "C" void quidra_neural_state_load_finish(
    void* raw_context,unsigned long long line,unsigned long long column) {
    if(!raw_context) neural_state_fail("null .quistate load context",line,column);
    std::unique_ptr<NeuralStateContext> context(
        static_cast<NeuralStateContext*>(raw_context));

    if(context->cursor!=context->end)
        neural_state_fail(".quistate contains unexpected trailing state",line,column);

    context->cursor=context->payload_start;
    for(const auto& request:context->reads){
        neural_state_process_read(
            *context,request.path.c_str(),request.kind,request.address,true,line,column);
    }
    if(context->cursor!=context->end)
        neural_state_fail(".quistate internal replay mismatch",line,column);
}

extern "C" void* quidra_neural_untrack(void* raw) {
    if(!raw)runtime_text_failure("null neural value");
    return neural_tensor_from_node(*static_cast<NeuralValue*>(raw)->node);
}
extern "C" void* quidra_neural_cast(
    void* raw,int target_dtype,
    unsigned long long line,unsigned long long column) {
    if(!raw) neural_fail("null neural value",line,column);
    if(target_dtype!=9&&target_dtype!=10)
        neural_fail("neural cast target must be float32 or float",line,column);
    auto root=static_cast<NeuralValue*>(raw)->node;
    return neural_descriptor(neural_cast_graph(root,target_dtype));
}
extern "C" void quidra_neural_rank_check(
    void* raw,long long expected_rank,
    unsigned long long line,unsigned long long column) {
    if(!raw) neural_fail("null neural value",line,column);
    const auto& shape=static_cast<NeuralValue*>(raw)->node->shape;
    if(expected_rank<0 || shape.size()!=static_cast<std::size_t>(expected_rank))
        neural_fail("neural rank does not satisfy captured shape constraint",line,column);
}
extern "C" void quidra_neural_extent_check(
    void* raw,long long axis,long long expected,
    unsigned long long line,unsigned long long column) {
    if(!raw) neural_fail("null neural value",line,column);
    const auto& shape=static_cast<NeuralValue*>(raw)->node->shape;
    if(axis<0 || static_cast<std::size_t>(axis)>=shape.size())
        neural_fail("neural shape axis is outside rank",line,column);
    if(expected<0)
        neural_fail("captured neural extent cannot be negative",line,column);
    if(shape[static_cast<std::size_t>(axis)]!=expected)
        neural_fail("neural extent does not satisfy captured shape constraint",line,column);
}
extern "C" void* quidra_neural_clone(void* raw) {
    if(!raw)return nullptr;
    return neural_descriptor(static_cast<NeuralValue*>(raw)->node);
}
extern "C" void quidra_neural_drop(void* raw) {
    if(raw)static_cast<NeuralValue*>(raw)->~NeuralValue();
}
extern "C" void* quidra_neural_gradients_clone(void* raw) {
    if(!raw)return nullptr;
    return neural_gradients_descriptor(static_cast<NeuralGradients*>(raw)->data);
}
extern "C" void quidra_neural_gradients_drop(void* raw) {
    if(raw)static_cast<NeuralGradients*>(raw)->~NeuralGradients();
}
extern "C" void* quidra_neural_unary(void* raw,int op,unsigned long long line,unsigned long long column) {
    if(!raw)neural_fail("null neural value",line,column);
    return neural_descriptor(neural_unary_node(static_cast<NeuralValue*>(raw)->node,op,line,column));
}
extern "C" void* quidra_neural_tensor_unary(void* raw,int op,unsigned long long line,unsigned long long column) {
    if(!raw)neural_fail("null tensor",line,column);
    auto& input=*static_cast<TensorValue*>(raw);
    if (tensor_on_cpu(*input.storage)) {
        auto node=neural_constant_node(input,line,column);
        auto transformed=neural_unary_node(node,op,line,column);
        return neural_tensor_from_node(*transformed);
    }
    tensor_require_initialized(input,line,column);
    if(input.storage->dtype!=9&&input.storage->dtype!=10)
        neural_fail("neural values require float32 or float tensors",line,column);

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
    if(op==4){
        output_shape.clear();
        output_count=1;
    }
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
    }else if(op==5||op==6){
        if(input.shape.empty()||input.shape.back()<=0){
            if(materialized)tensor_storage_release(materialized);
            tensor_storage_release(output);
            neural_fail("last-axis reduction requires a non-empty last axis",line,column);
        }
        ok=quidra::device::compute_last_reduce_broadcast(
            output->gpu_buffer,source->gpu_buffer,input.storage->dtype,
            tensor_logical_count(input),static_cast<std::size_t>(input.shape.back()),
            op==5?1:2,backend_error);
    }else{
        if(materialized)tensor_storage_release(materialized);
        tensor_storage_release(output);
        neural_fail("unknown neural unary operation",line,column);
    }
    if(materialized)tensor_storage_release(materialized);
    if(!ok){
        tensor_storage_release(output);
        neural_fail(backend_error.c_str(),line,column);
    }
    auto output_strides=tensor_contiguous_strides(output_shape);
    return tensor_descriptor(
        output,std::move(output_shape),std::move(output_strides),0);
}
template <typename T>
void* neural_binary_t(
    const std::shared_ptr<NeuralNode>& left,const std::shared_ptr<NeuralNode>& right,int op,
    unsigned long long line,unsigned long long column) {
    const auto& a=left->data.typed<T>();
    const auto& b=right->data.typed<T>();
    std::vector<T> values(a.size(),T{0});
    for(std::size_t i=0;i<values.size();++i){
        if(op==1) values[i]=static_cast<T>(a[i]+b[i]);
        else if(op==2) values[i]=static_cast<T>(a[i]-b[i]);
        else if(op==3) values[i]=static_cast<T>(a[i]*b[i]);
        else{
            if(b[i]==T{0}) neural_fail("division by zero",line,column);
            values[i]=static_cast<T>(a[i]/b[i]);
        }
    }
    auto node=std::make_shared<NeuralNode>(left->dtype);
    node->shape=left->shape;
    node->parents={left,right};
    node->op=op==1?NeuralOp::Add:op==2?NeuralOp::Sub:op==3?NeuralOp::Mul:NeuralOp::Div;
    node->data=NeuralBuffer(std::move(values));
    return neural_descriptor(std::move(node));
}

extern "C" void* quidra_neural_binary(void* left_raw,void* right_raw,int op,
                                       unsigned long long line,unsigned long long column) {
    if(!left_raw||!right_raw) neural_fail("null neural operand",line,column);
    const auto left=static_cast<NeuralValue*>(left_raw)->node;
    const auto right=static_cast<NeuralValue*>(right_raw)->node;
    neural_require_same_shape(*left,*right,line,column);
    if(left->device_tensor){
        auto node=std::make_shared<NeuralNode>(left->dtype);
        node->shape=left->shape;
        node->parents={left,right};
        node->op=op==1?NeuralOp::Add:op==2?NeuralOp::Sub:op==3?NeuralOp::Mul:NeuralOp::Div;
        node->device_tensor=static_cast<TensorValue*>(
            quidra_tensor_binary(
                left->device_tensor,right->device_tensor,nullptr,0,op,line,column));
        if(!node->device_tensor)
            neural_fail("GPU neural binary operation returned null",line,column);
        return neural_descriptor(std::move(node));
    }
    if(left->dtype==10) return neural_binary_t<float>(left,right,op,line,column);
    if(left->dtype==9) return neural_binary_t<double>(left,right,op,line,column);
    neural_fail("invalid neural binary dtype",line,column);
}

extern "C" void* quidra_neural_binary_scalar(
    void* raw,double scalar,int op,bool scalar_left,
    unsigned long long line,unsigned long long column) {
    if(!raw) neural_fail("null neural operand",line,column);
    auto input=static_cast<NeuralValue*>(raw)->node;
    auto constant=std::make_shared<NeuralNode>(input->dtype);
    constant->shape=input->shape;
    if(input->device_tensor){
        void* zero_tensor=nullptr;
        void* constant_tensor=nullptr;
        if(input->dtype==10){
            const float zero=0.0F;
            const float value=static_cast<float>(scalar);
            zero_tensor=quidra_tensor_binary(
                input->device_tensor,nullptr,const_cast<float*>(&zero),2,3,line,column);
            constant_tensor=quidra_tensor_binary(
                zero_tensor,nullptr,const_cast<float*>(&value),2,1,line,column);
        }else if(input->dtype==9){
            const double zero=0.0;
            const double value=scalar;
            zero_tensor=quidra_tensor_binary(
                input->device_tensor,nullptr,const_cast<double*>(&zero),2,3,line,column);
            constant_tensor=quidra_tensor_binary(
                zero_tensor,nullptr,const_cast<double*>(&value),2,1,line,column);
        }else{
            neural_fail("invalid neural scalar dtype",line,column);
        }
        quidra_tensor_drop(zero_tensor);
        constant->device_tensor=static_cast<TensorValue*>(constant_tensor);
        if(!constant->device_tensor)
            neural_fail("GPU neural scalar materialization returned null",line,column);
    }else{
        constant->data.assign(input->data.size(),scalar);
    }
    NeuralValue a{scalar_left?constant:input},b{scalar_left?input:constant};
    return quidra_neural_binary(&a,&b,op,line,column);
}


TensorValue* neural_device_dense_clone(
    const TensorValue& value,unsigned long long line,unsigned long long column) {
    if(tensor_on_cpu(*value.storage))
        neural_fail("internal neural GPU path received a CPU tensor",line,column);
    if(tensor_is_contiguous_value(value)&&value.offset==0)
        return static_cast<TensorValue*>(
            quidra_tensor_clone(const_cast<TensorValue*>(&value)));
    auto* storage=tensor_gpu_materialize_storage(value,line,column);
    auto strides=tensor_contiguous_strides(value.shape);
    return tensor_descriptor(storage,value.shape,std::move(strides),0);
}

TensorValue* neural_device_binary_tensor(
    TensorValue* left,TensorValue* right,int operation,
    unsigned long long line,unsigned long long column) {
    return static_cast<TensorValue*>(
        quidra_tensor_binary(left,right,nullptr,0,operation,line,column));
}

TensorValue* neural_device_negate_tensor(
    TensorValue* input,unsigned long long line,unsigned long long column) {
    return static_cast<TensorValue*>(
        quidra_tensor_unary(input,1,line,column));
}

void neural_add_device_gradient(
    std::unordered_map<const NeuralNode*,TensorValue*>& gradients,
    const std::shared_ptr<NeuralNode>& node,TensorValue* value,
    unsigned long long line,unsigned long long column) {
    if(!value) neural_fail("null GPU neural gradient",line,column);
    const auto found=gradients.find(node.get());
    if(found==gradients.end()){
        gradients.emplace(node.get(),value);
        return;
    }
    auto* combined=neural_device_binary_tensor(
        found->second,value,1,line,column);
    quidra_tensor_drop(found->second);
    quidra_tensor_drop(value);
    found->second=combined;
}

TensorValue* neural_device_filled_like(
    const NeuralNode& node,bool ones,
    unsigned long long line,unsigned long long column) {
    if(!node.device_tensor)
        neural_fail("internal neural GPU fill requires a device tensor",line,column);
    const auto count=tensor_logical_count(*node.device_tensor);
    auto* storage=tensor_storage_create(
        node.dtype,count,ones?2:1,node.device_tensor->storage->device,line,column);
    auto strides=tensor_contiguous_strides(node.shape);
    return tensor_descriptor(storage,node.shape,std::move(strides),0);
}

void neural_store_device_parameter_gradient(
    NeuralGradientData& output,const NeuralNode& node,TensorValue* gradient,
    unsigned long long line,unsigned long long column) {
    auto [it,inserted]=output.values.try_emplace(node.parameter_id,node.dtype);
    auto& destination=it->second;
    if(!inserted&&destination.dtype!=node.dtype)
        neural_fail("gradient dtype mismatch",line,column);
    destination.shape=node.shape;
    if(!destination.device_tensor){
        destination.device_tensor=static_cast<TensorValue*>(
            quidra_tensor_clone(gradient));
        return;
    }
    auto* combined=neural_device_binary_tensor(
        destination.device_tensor,gradient,1,line,column);
    quidra_tensor_drop(destination.device_tensor);
    destination.device_tensor=combined;
}

void* neural_grad_device(
    const std::shared_ptr<NeuralNode>& loss,
    unsigned long long line,unsigned long long column) {
    std::unordered_set<const NeuralNode*> seen;
    std::vector<std::shared_ptr<NeuralNode>> order;
    neural_topological(loss,seen,order);

    if(!loss->device_tensor||tensor_logical_count(*loss->device_tensor)!=1)
        neural_fail("grad requires a scalar GPU loss",line,column);

    std::unordered_map<const NeuralNode*,TensorValue*> gradients;
    auto* initial=neural_device_filled_like(*loss,true,line,column);
    gradients.emplace(loss.get(),initial);
    auto output=std::make_shared<NeuralGradientData>();

    for(auto it=order.rbegin();it!=order.rend();++it){
        const auto& node=*it;
        if(node->dtype!=loss->dtype)
            neural_fail("autograd graph contains mixed dtypes",line,column);
        const auto found=gradients.find(node.get());
        if(found==gradients.end()) continue;
        auto* g=found->second;

        if(node->parameter_id)
            neural_store_device_parameter_gradient(
                *output,*node,g,line,column);

        if(node->parents.empty()) continue;

        if(node->op==NeuralOp::Add||node->op==NeuralOp::Sub||
           node->op==NeuralOp::Mul||node->op==NeuralOp::Div){
            if(node->parents.size()!=2 ||
               !node->parents[0]->device_tensor ||
               !node->parents[1]->device_tensor)
                neural_fail("invalid GPU neural binary graph",line,column);
            auto* a=node->parents[0]->device_tensor;
            auto* b=node->parents[1]->device_tensor;
            TensorValue* left_gradient=nullptr;
            TensorValue* right_gradient=nullptr;
            if(node->op==NeuralOp::Add){
                left_gradient=static_cast<TensorValue*>(quidra_tensor_clone(g));
                right_gradient=static_cast<TensorValue*>(quidra_tensor_clone(g));
            }else if(node->op==NeuralOp::Sub){
                left_gradient=static_cast<TensorValue*>(quidra_tensor_clone(g));
                right_gradient=neural_device_negate_tensor(g,line,column);
            }else if(node->op==NeuralOp::Mul){
                left_gradient=neural_device_binary_tensor(g,b,3,line,column);
                right_gradient=neural_device_binary_tensor(g,a,3,line,column);
            }else{
                left_gradient=neural_device_binary_tensor(g,b,4,line,column);
                auto* ga=neural_device_binary_tensor(g,a,3,line,column);
                auto* bb=neural_device_binary_tensor(b,b,3,line,column);
                auto* quotient=neural_device_binary_tensor(ga,bb,4,line,column);
                right_gradient=neural_device_negate_tensor(quotient,line,column);
                quidra_tensor_drop(ga);
                quidra_tensor_drop(bb);
                quidra_tensor_drop(quotient);
            }
            neural_add_device_gradient(
                gradients,node->parents[0],left_gradient,line,column);
            neural_add_device_gradient(
                gradients,node->parents[1],right_gradient,line,column);
        }else if(node->op==NeuralOp::Absolute){
            const auto& input=node->parents[0];
            auto* gd=neural_device_dense_clone(*g,line,column);
            auto* xd=neural_device_dense_clone(*input->device_tensor,line,column);
            const auto count=tensor_logical_count(*xd);
            auto* storage=tensor_storage_create(
                node->dtype,count,1,xd->storage->device,line,column);
            auto strides=tensor_contiguous_strides(input->shape);
            auto* result=tensor_descriptor(storage,input->shape,std::move(strides),0);
            std::string backend_error;
            const bool ok=quidra::device::compute_abs_backward(
                storage->gpu_buffer,gd->storage->gpu_buffer,xd->storage->gpu_buffer,
                node->dtype,count,backend_error);
            quidra_tensor_drop(gd);quidra_tensor_drop(xd);
            if(!ok){
                quidra_tensor_drop(result);
                neural_fail(backend_error.c_str(),line,column);
            }
            neural_add_device_gradient(
                gradients,input,result,line,column);
        }else if(node->op==NeuralOp::Exponential){
            auto* result=neural_device_binary_tensor(
                g,node->device_tensor,3,line,column);
            neural_add_device_gradient(
                gradients,node->parents[0],result,line,column);
        }else if(node->op==NeuralOp::Logarithm){
            auto* result=neural_device_binary_tensor(
                g,node->parents[0]->device_tensor,4,line,column);
            neural_add_device_gradient(
                gradients,node->parents[0],result,line,column);
        }else if(node->op==NeuralOp::Mean){
            const auto& input=node->parents[0];
            const auto count=neural_node_count(*input);
            auto* gd=neural_device_dense_clone(*g,line,column);
            auto* storage=tensor_storage_create(
                node->dtype,count,1,input->device_tensor->storage->device,line,column);
            auto strides=tensor_contiguous_strides(input->shape);
            auto* result=tensor_descriptor(storage,input->shape,std::move(strides),0);
            std::string backend_error;
            const bool ok=quidra::device::compute_mean_backward(
                storage->gpu_buffer,gd->storage->gpu_buffer,node->dtype,count,backend_error);
            quidra_tensor_drop(gd);
            if(!ok){
                quidra_tensor_drop(result);
                neural_fail(backend_error.c_str(),line,column);
            }
            neural_add_device_gradient(
                gradients,input,result,line,column);
        }else if(node->op==NeuralOp::SumLast){
            const auto& input=node->parents[0];
            const auto count=neural_node_count(*input);
            const auto width=static_cast<std::size_t>(input->shape.back());
            auto* gd=neural_device_dense_clone(*g,line,column);
            auto* storage=tensor_storage_create(
                node->dtype,count,1,input->device_tensor->storage->device,line,column);
            auto strides=tensor_contiguous_strides(input->shape);
            auto* result=tensor_descriptor(storage,input->shape,std::move(strides),0);
            std::string backend_error;
            const bool ok=quidra::device::compute_last_reduce_broadcast(
                storage->gpu_buffer,gd->storage->gpu_buffer,node->dtype,
                count,width,1,backend_error);
            quidra_tensor_drop(gd);
            if(!ok){
                quidra_tensor_drop(result);
                neural_fail(backend_error.c_str(),line,column);
            }
            neural_add_device_gradient(
                gradients,input,result,line,column);
        }else if(node->op==NeuralOp::MaxLast){
            const auto& input=node->parents[0];
            const auto count=neural_node_count(*input);
            const auto width=static_cast<std::size_t>(input->shape.back());
            auto* gd=neural_device_dense_clone(*g,line,column);
            auto* xd=neural_device_dense_clone(*input->device_tensor,line,column);
            auto* storage=tensor_storage_create(
                node->dtype,count,1,input->device_tensor->storage->device,line,column);
            auto strides=tensor_contiguous_strides(input->shape);
            auto* result=tensor_descriptor(storage,input->shape,std::move(strides),0);
            std::string backend_error;
            const bool ok=quidra::device::compute_max_last_backward(
                storage->gpu_buffer,gd->storage->gpu_buffer,xd->storage->gpu_buffer,
                node->dtype,count,width,backend_error);
            quidra_tensor_drop(gd);quidra_tensor_drop(xd);
            if(!ok){
                quidra_tensor_drop(result);
                neural_fail(backend_error.c_str(),line,column);
            }
            neural_add_device_gradient(
                gradients,input,result,line,column);
        }else if(node->op==NeuralOp::Affine){
            if(node->parents.size()!=3)
                neural_fail("invalid GPU affine graph",line,column);
            const auto& input=node->parents[0];
            const auto& weight=node->parents[1];
            const auto& bias=node->parents[2];
            const auto features_in=static_cast<std::size_t>(weight->shape[1]);
            const auto features_out=static_cast<std::size_t>(weight->shape[0]);
            const auto batches=features_in==0?0:neural_node_count(*input)/features_in;
            auto* gd=neural_device_dense_clone(*g,line,column);
            auto* id=neural_device_dense_clone(*input->device_tensor,line,column);
            auto* wd=neural_device_dense_clone(*weight->device_tensor,line,column);
            auto* input_storage=tensor_storage_create(
                node->dtype,neural_node_count(*input),1,
                input->device_tensor->storage->device,line,column);
            auto* weight_storage=tensor_storage_create(
                node->dtype,neural_node_count(*weight),1,
                weight->device_tensor->storage->device,line,column);
            auto* bias_storage=tensor_storage_create(
                node->dtype,neural_node_count(*bias),1,
                bias->device_tensor->storage->device,line,column);
            auto input_strides=tensor_contiguous_strides(input->shape);
            auto weight_strides=tensor_contiguous_strides(weight->shape);
            auto bias_strides=tensor_contiguous_strides(bias->shape);
            auto* input_result=tensor_descriptor(
                input_storage,input->shape,std::move(input_strides),0);
            auto* weight_result=tensor_descriptor(
                weight_storage,weight->shape,std::move(weight_strides),0);
            auto* bias_result=tensor_descriptor(
                bias_storage,bias->shape,std::move(bias_strides),0);
            std::string backend_error;
            const bool ok=quidra::device::compute_affine_backward(
                input_storage->gpu_buffer,weight_storage->gpu_buffer,bias_storage->gpu_buffer,
                gd->storage->gpu_buffer,id->storage->gpu_buffer,wd->storage->gpu_buffer,
                node->dtype,batches,features_in,features_out,backend_error);
            quidra_tensor_drop(gd);quidra_tensor_drop(id);quidra_tensor_drop(wd);
            if(!ok){
                quidra_tensor_drop(input_result);
                quidra_tensor_drop(weight_result);
                quidra_tensor_drop(bias_result);
                neural_fail(backend_error.c_str(),line,column);
            }
            neural_add_device_gradient(
                gradients,input,input_result,line,column);
            neural_add_device_gradient(
                gradients,weight,weight_result,line,column);
            neural_add_device_gradient(
                gradients,bias,bias_result,line,column);
        }else if(node->op==NeuralOp::Convolution){
            neural_fail(
                "neural.convolve2d backward is not supported on GPU by the current DNN backend",
                line,column);
        }else if(node->op==NeuralOp::Normalize){
            neural_fail(
                "neural.normalize backward is not supported on GPU by the current DNN backend",
                line,column);
        }else if(node->op==NeuralOp::RandomMask){
            neural_fail(
                "neural.random_mask backward is not supported on GPU by the current DNN backend",
                line,column);
        }
    }

    for(auto& [_,value]:gradients)
        quidra_tensor_drop(value);
    return neural_gradients_descriptor(std::move(output));
}

template <typename T>
void* neural_grad_t(
    const std::shared_ptr<NeuralNode>& loss,
    unsigned long long line,unsigned long long column) {
    std::unordered_set<const NeuralNode*> seen;
    std::vector<std::shared_ptr<NeuralNode>> order;
    neural_topological(loss,seen,order);

    std::unordered_map<const NeuralNode*,std::vector<T>> gradients;
    gradients[loss.get()]={T{1}};
    auto output=std::make_shared<NeuralGradientData>();

    for(auto it=order.rbegin();it!=order.rend();++it){
        const auto& node=*it;
        if(node->dtype!=loss->dtype)
            neural_fail("autograd graph contains mixed dtypes",line,column);
        const auto found=gradients.find(node.get());
        if(found==gradients.end()) continue;
        const auto& g=found->second;
        const auto& node_values=node->data.typed<T>();

        if(node->parameter_id){
            auto [gradient_it,inserted]=output->values.try_emplace(node->parameter_id,node->dtype);
            auto& destination=gradient_it->second;
            if(!inserted&&destination.dtype!=node->dtype)
                neural_fail("gradient dtype mismatch",line,column);
            destination.shape=node->shape;
            auto& values=destination.data.typed<T>();
            if(values.empty()) values=g;
            else{
                if(values.size()!=g.size()) neural_fail("gradient size mismatch",line,column);
                for(std::size_t i=0;i<g.size();++i)
                    values[i]=static_cast<T>(values[i]+g[i]);
            }
        }

        if(node->parents.empty()) continue;

        if(node->op==NeuralOp::Add||node->op==NeuralOp::Sub||
           node->op==NeuralOp::Mul||node->op==NeuralOp::Div){
            const auto& a=node->parents[0]->data.typed<T>();
            const auto& b=node->parents[1]->data.typed<T>();
            std::vector<T> left_gradient(g.size()),right_gradient(g.size());
            for(std::size_t i=0;i<g.size();++i){
                if(node->op==NeuralOp::Add){
                    left_gradient[i]=g[i]; right_gradient[i]=g[i];
                }else if(node->op==NeuralOp::Sub){
                    left_gradient[i]=g[i]; right_gradient[i]=static_cast<T>(-g[i]);
                }else if(node->op==NeuralOp::Mul){
                    left_gradient[i]=static_cast<T>(g[i]*b[i]);
                    right_gradient[i]=static_cast<T>(g[i]*a[i]);
                }else{
                    left_gradient[i]=static_cast<T>(g[i]/b[i]);
                    right_gradient[i]=static_cast<T>(
                        -static_cast<T>(g[i]*a[i])/
                        static_cast<T>(b[i]*b[i]));
                }
            }
            neural_add_gradient(gradients,node->parents[0],std::move(left_gradient));
            neural_add_gradient(gradients,node->parents[1],std::move(right_gradient));
        }else if(node->op==NeuralOp::Absolute||
                 node->op==NeuralOp::Exponential||
                 node->op==NeuralOp::Logarithm){
            const auto& input=node->parents[0]->data.typed<T>();
            std::vector<T> input_gradient(g.size());
            for(std::size_t i=0;i<g.size();++i){
                if(node->op==NeuralOp::Absolute)
                    input_gradient[i]=input[i]>T{0}?g[i]:input[i]<T{0}?static_cast<T>(-g[i]):T{0};
                else if(node->op==NeuralOp::Exponential)
                    input_gradient[i]=static_cast<T>(g[i]*node_values[i]);
                else
                    input_gradient[i]=static_cast<T>(g[i]/input[i]);
            }
            neural_add_gradient(gradients,node->parents[0],std::move(input_gradient));
        }else if(node->op==NeuralOp::Mean){
            const auto count=node->parents[0]->data.size();
            if(count==0) neural_fail("mean gradient requires at least one element",line,column);
            std::vector<T> input_gradient(count,static_cast<T>(g[0]/static_cast<T>(count)));
            neural_add_gradient(gradients,node->parents[0],std::move(input_gradient));
        }else if(node->op==NeuralOp::SumLast||node->op==NeuralOp::MaxLast){
            const auto& input=node->parents[0]->data.typed<T>();
            const auto width=static_cast<std::size_t>(node->shape.back());
            std::vector<T> input_gradient(g.size(),T{0});
            for(std::size_t base=0;base<g.size();base+=width){
                T total=T{0};
                for(std::size_t j=0;j<width;++j)
                    total=static_cast<T>(total+g[base+j]);
                if(node->op==NeuralOp::SumLast){
                    for(std::size_t j=0;j<width;++j) input_gradient[base+j]=total;
                }else{
                    std::size_t selected=0;
                    for(std::size_t j=1;j<width;++j)
                        if(input[base+j]>input[base+selected]) selected=j;
                    input_gradient[base+selected]=total;
                }
            }
            neural_add_gradient(gradients,node->parents[0],std::move(input_gradient));
        }else if(node->op==NeuralOp::RandomMask){
            if(node->aux.size()!=g.size())
                neural_fail("random mask backward mask size mismatch",0,0);
            const auto& mask=node->aux.typed<T>();
            std::vector<T> input_gradient(g.size());
            for(std::size_t i=0;i<g.size();++i)
                input_gradient[i]=static_cast<T>(g[i]*mask[i]);
            neural_add_gradient(gradients,node->parents[0],std::move(input_gradient));
        }else if(node->op==NeuralOp::Normalize){
            const auto& input=node->parents[0];
            const auto& scale=node->parents[1];
            const auto& bias=node->parents[2];
            const auto& input_values=input->data.typed<T>();
            const auto& scale_values=scale->data.typed<T>();
            const auto layout=neural_normalize_layout(input->shape,input_values.size(),0,0);
            const auto features=layout.features;
            const auto samples=layout.samples;
            if(node->aux_index.size()!=1 || node->aux_index[0]!=samples ||
               node->aux.size()!=features*2)
                neural_fail("normalization backward cache layout mismatch",0,0);
            const auto& backward_cache=node->aux.typed<T>();
            std::vector<T> input_gradient(input_values.size(),T{0});
            std::vector<T> scale_gradient(features,T{0}),bias_gradient(features,T{0});
            for(std::size_t feature=0;feature<features;++feature){
                const T mean=backward_cache[feature];
                const T inverse=backward_cache[features+feature];
                T sum_gradient=T{0},sum_gradient_x=T{0};
                for(std::size_t i=0;i<input_values.size();++i){
                    if(neural_normalize_feature(i,layout)!=feature) continue;
                    const T xhat=static_cast<T>(
                        static_cast<T>(input_values[i]-mean)*inverse);
                    sum_gradient=static_cast<T>(sum_gradient+g[i]);
                    sum_gradient_x=static_cast<T>(
                        sum_gradient_x+static_cast<T>(g[i]*xhat));
                    scale_gradient[feature]=static_cast<T>(
                        scale_gradient[feature]+static_cast<T>(g[i]*xhat));
                    bias_gradient[feature]=static_cast<T>(bias_gradient[feature]+g[i]);
                }
                const T sample_count=static_cast<T>(samples);
                for(std::size_t i=0;i<input_values.size();++i){
                    if(neural_normalize_feature(i,layout)!=feature) continue;
                    const T xhat=static_cast<T>(
                        static_cast<T>(input_values[i]-mean)*inverse);
                    input_gradient[i]=static_cast<T>(
                        static_cast<T>(
                            static_cast<T>(scale_values[feature]*inverse)/sample_count)*
                        static_cast<T>(
                            static_cast<T>(sample_count*g[i])-sum_gradient-
                            static_cast<T>(xhat*sum_gradient_x)));
                }
            }
            neural_add_gradient(gradients,input,std::move(input_gradient));
            neural_add_gradient(gradients,scale,std::move(scale_gradient));
            neural_add_gradient(gradients,bias,std::move(bias_gradient));
        }else if(node->op==NeuralOp::Convolution){
            const auto& input=node->parents[0];
            const auto& weight=node->parents[1];
            const auto& bias=node->parents[2];
            const auto& input_values=input->data.typed<T>();
            const auto& weight_values=weight->data.typed<T>();
            if(node->aux_index.size()!=2)
                neural_fail("convolution backward metadata mismatch",0,0);
            const auto stride=static_cast<long long>(node->aux_index[0]);
            const auto padding=static_cast<long long>(node->aux_index[1]);
            const auto n=input->shape[0],in_c=input->shape[1],height=input->shape[2],width=input->shape[3];
            const auto out_c=weight->shape[0],kernel_h=weight->shape[2],kernel_w=weight->shape[3];
            const auto out_h=node->shape[2],out_w=node->shape[3];
            std::vector<T> input_gradient(input_values.size(),T{0});
            std::vector<T> weight_gradient(weight_values.size(),T{0});
            std::vector<T> bias_gradient(bias->data.size(),T{0});
            const auto input_index=[&](long long batch,long long channel,long long y,long long x){
                return static_cast<std::size_t>(((batch*in_c+channel)*height+y)*width+x);
            };
            const auto weight_index=[&](long long output_channel,long long input_channel,long long y,long long x){
                return static_cast<std::size_t>(((output_channel*in_c+input_channel)*kernel_h+y)*kernel_w+x);
            };
            const auto output_index=[&](long long batch,long long output_channel,long long y,long long x){
                return static_cast<std::size_t>(((batch*out_c+output_channel)*out_h+y)*out_w+x);
            };
            for(long long batch_index=0;batch_index<n;++batch_index)
                for(long long output_channel=0;output_channel<out_c;++output_channel)
                    for(long long oy=0;oy<out_h;++oy)
                        for(long long ox=0;ox<out_w;++ox){
                            const T gradient=g[output_index(batch_index,output_channel,oy,ox)];
                            bias_gradient[static_cast<std::size_t>(output_channel)]=static_cast<T>(
                                bias_gradient[static_cast<std::size_t>(output_channel)]+gradient);
                            for(long long input_channel=0;input_channel<in_c;++input_channel)
                                for(long long ky=0;ky<kernel_h;++ky)
                                    for(long long kx=0;kx<kernel_w;++kx){
                                        const auto iy=oy*stride+ky-padding;
                                        const auto ix=ox*stride+kx-padding;
                                        if(iy<0||ix<0||iy>=height||ix>=width) continue;
                                        const auto input_offset=input_index(batch_index,input_channel,iy,ix);
                                        const auto weight_offset=weight_index(output_channel,input_channel,ky,kx);
                                        input_gradient[input_offset]=static_cast<T>(
                                            input_gradient[input_offset]+static_cast<T>(
                                                gradient*weight_values[weight_offset]));
                                        weight_gradient[weight_offset]=static_cast<T>(
                                            weight_gradient[weight_offset]+static_cast<T>(
                                                gradient*input_values[input_offset]));
                                    }
                        }
            neural_add_gradient(gradients,input,std::move(input_gradient));
            neural_add_gradient(gradients,weight,std::move(weight_gradient));
            neural_add_gradient(gradients,bias,std::move(bias_gradient));
        }else if(node->op==NeuralOp::Affine){
            const auto& input=node->parents[0];
            const auto& weight=node->parents[1];
            const auto& bias=node->parents[2];
            const auto& input_values=input->data.typed<T>();
            const auto& weight_values=weight->data.typed<T>();
            const auto in=static_cast<std::size_t>(weight->shape[1]);
            const auto out=static_cast<std::size_t>(weight->shape[0]);
            const auto batches=in==0?0:input_values.size()/in;
            std::vector<T> input_gradient(input_values.size(),T{0});
            std::vector<T> weight_gradient(weight_values.size(),T{0});
            std::vector<T> bias_gradient(bias->data.size(),T{0});
            for(std::size_t batch=0;batch<batches;++batch){
                for(std::size_t output_index=0;output_index<out;++output_index){
                    const T gradient=g[batch*out+output_index];
                    bias_gradient[output_index]=static_cast<T>(
                        bias_gradient[output_index]+gradient);
                    for(std::size_t input_index=0;input_index<in;++input_index){
                        input_gradient[batch*in+input_index]=static_cast<T>(
                            input_gradient[batch*in+input_index]+static_cast<T>(
                                gradient*weight_values[output_index*in+input_index]));
                        weight_gradient[output_index*in+input_index]=static_cast<T>(
                            weight_gradient[output_index*in+input_index]+static_cast<T>(
                                gradient*input_values[batch*in+input_index]));
                    }
                }
            }
            neural_add_gradient(gradients,input,std::move(input_gradient));
            neural_add_gradient(gradients,weight,std::move(weight_gradient));
            neural_add_gradient(gradients,bias,std::move(bias_gradient));
        }
    }
    return neural_gradients_descriptor(std::move(output));
}

extern "C" void* quidra_neural_grad(
    void* raw,unsigned long long line,unsigned long long column) {
    if(!raw) neural_fail("null loss",line,column);
    auto loss=static_cast<NeuralValue*>(raw)->node;
    if(neural_node_count(*loss)!=1) neural_fail("grad requires a scalar loss",line,column);
    if(loss->device_tensor)
        return neural_grad_device(loss,line,column);
    if(loss->dtype==10) return neural_grad_t<float>(loss,line,column);
    if(loss->dtype==9) return neural_grad_t<double>(loss,line,column);
    neural_fail("invalid neural gradient dtype",line,column);
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
        return tensor_descriptor(
            output, output_shape, tensor_contiguous_strides(output_shape), 0);
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
    return tensor_descriptor(
        output, output_shape, tensor_contiguous_strides(output_shape), 0);
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
                         unsigned long long line,
                         unsigned long long column) {
    const auto m = static_cast<std::size_t>(left.shape[0]);
    const auto k = static_cast<std::size_t>(left.shape[1]);
    const auto n = static_cast<std::size_t>(right.shape[1]);

    if constexpr (std::is_floating_point_v<T>) {
        const unsigned char* left_dense{};
        const unsigned char* right_dense{};
        if (tensor_dense_initialized_bytes<T>(left, left_dense) &&
            tensor_dense_initialized_bytes<T>(right, right_dense)) {
            std::vector<T> dense_output;
            try {
                dense_output.assign(output.count, T{});
            } catch (...) {
                runtime_allocation_failure();
            }

            // Keep each output element's accumulation in the same k-order as
            // the scalar definition, while making adjacent columns the inner
            // loop so the compiler can SIMD the independent j dimension.
            for (std::size_t row = 0; row < m; ++row) {
                for (std::size_t inner = 0; inner < k; ++inner) {
                    const T a = tensor_dense_load<T>(left_dense, row * k + inner);
                    for (std::size_t column_index = 0; column_index < n; ++column_index) {
                        const auto output_index = row * n + column_index;
                        T product{};
                        T next{};
                        (void)tensor_mul_checked(
                            a, tensor_dense_load<T>(
                                   right_dense, inner * n + column_index),
                            product);
                        (void)tensor_add_checked(
                            dense_output[output_index], product, next);
                        dense_output[output_index] = next;
                    }
                }
            }
            if (!dense_output.empty()) {
                std::memcpy(output.data.data(), dense_output.data(),
                            dense_output.size() * sizeof(T));
            }
            return;
        }
    }

    for (std::size_t row = 0; row < m; ++row) {
        for (std::size_t column_index = 0; column_index < n; ++column_index) {
            T accumulator{};
            for (std::size_t inner = 0; inner < k; ++inner) {
                const auto left_index =
                    left.offset + row * static_cast<std::size_t>(left.strides[0]) +
                    inner * static_cast<std::size_t>(left.strides[1]);
                const auto right_index =
                    right.offset + inner * static_cast<std::size_t>(right.strides[0]) +
                    column_index * static_cast<std::size_t>(right.strides[1]);
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
                    tensor_fail("linear.matmul integer arithmetic overflow", line, column);
                }
                accumulator = next;
            }
            std::memcpy(output.data.data() + (row * n + column_index) * sizeof(T),
                        &accumulator, sizeof(T));
        }
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

extern "C" void* quidra_linear_matmul(void* left_raw, void* right_raw,
                                       unsigned long long line,
                                       unsigned long long column) {
    if (!left_raw || !right_raw) tensor_fail("linear.matmul received a null tensor", line, column);
    auto& left = *static_cast<TensorValue*>(left_raw);
    auto& right = *static_cast<TensorValue*>(right_raw);
    if (left.storage->dtype != right.storage->dtype) {
        tensor_fail("linear.matmul requires identical element types", line, column);
    }
    if (left.storage->device != right.storage->device) {
        tensor_fail("linear.matmul operands are on different devices; transfer them explicitly",
                    line, column);
    }
    if (left.shape.size() != 2 || right.shape.size() != 2) {
        tensor_fail("linear.matmul currently requires rank-2 tensors", line, column);
    }
    if (left.shape[1] != right.shape[0]) {
        tensor_fail("linear.matmul inner dimensions do not match", line, column);
    }
    std::vector<long long> shape{left.shape[0], right.shape[1]};
    const auto count = tensor_element_count(shape, line, column);
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
        auto* output = tensor_storage_create(
            left.storage->dtype, count, 1, left.storage->device, line, column);
        std::string backend_error;
        const bool ok = quidra::device::compute_matmul(
            output->gpu_buffer, left_storage->gpu_buffer, right_storage->gpu_buffer,
            left.storage->dtype,
            static_cast<std::size_t>(left.shape[0]),
            static_cast<std::size_t>(left.shape[1]),
            static_cast<std::size_t>(right.shape[1]), backend_error);
        if (left_materialized) tensor_storage_release(left_materialized);
        if (right_materialized) tensor_storage_release(right_materialized);
        if (!ok) {
            tensor_storage_release(output);
            tensor_fail(backend_error.c_str(), line, column);
        }
        auto strides = tensor_contiguous_strides(shape);
        return tensor_descriptor(output, std::move(shape), std::move(strides), 0);
    }
    auto* output = tensor_storage_create(left.storage->dtype, count, 1);
    switch (left.storage->dtype) {
        case 1: tensor_matmul_typed<std::int64_t>(left,right,*output,line,column); break;
        case 2: tensor_matmul_typed<std::int8_t>(left,right,*output,line,column); break;
        case 3: tensor_matmul_typed<std::int16_t>(left,right,*output,line,column); break;
        case 4: tensor_matmul_typed<std::int32_t>(left,right,*output,line,column); break;
        case 5: tensor_matmul_typed<std::uint8_t>(left,right,*output,line,column); break;
        case 6: tensor_matmul_typed<std::uint16_t>(left,right,*output,line,column); break;
        case 7: tensor_matmul_typed<std::uint32_t>(left,right,*output,line,column); break;
        case 8: tensor_matmul_typed<std::uint64_t>(left,right,*output,line,column); break;
        case 9: tensor_matmul_typed<double>(left,right,*output,line,column); break;
        case 10:tensor_matmul_typed<float>(left,right,*output,line,column); break;
        default:
            delete output;
            tensor_fail("linear.matmul received an unsupported tensor dtype", line, column);
    }
    auto strides = tensor_contiguous_strides(shape);
    return tensor_descriptor(output, std::move(shape), std::move(strides), 0);
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

    return runtime_copy_string(
        std::string(text + bounds.start, bounds.end - bounds.start));
}

extern "C" long long quidra_string_length(const char* text) {
    ManagedAllocation* allocation = nullptr;
    const auto source = cached_string_view(text, allocation);
    if (allocation && allocation->string_codepoint_length_known) {
        return static_cast<long long>(allocation->string_codepoint_length);
    }
    const auto length = utf8_length(source);
    if (allocation) {
        allocation->string_codepoint_length_known = true;
        allocation->string_codepoint_length = length;
    }
    return static_cast<long long>(length);
}

extern "C" bool quidra_string_contains(const char* text, const char* needle) {
    if (!text || !needle) runtime_text_failure("null string");
    const std::string_view source(text), query(needle);
    validate_utf8(source);
    validate_utf8(query);
    return source.find(query) != std::string_view::npos;
}

extern "C" bool quidra_string_starts_with(const char* text, const char* prefix) {
    if (!text || !prefix) runtime_text_failure("null string");
    const std::string_view source(text), query(prefix);
    validate_utf8(source);
    validate_utf8(query);
    return source.size() >= query.size() && source.substr(0,query.size()) == query;
}

extern "C" bool quidra_string_ends_with(const char* text, const char* suffix) {
    if (!text || !suffix) runtime_text_failure("null string");
    const std::string_view source(text), query(suffix);
    validate_utf8(source);
    validate_utf8(query);
    return source.size() >= query.size() &&
           source.substr(source.size()-query.size()) == query;
}
extern "C" long long quidra_string_find(const char* text, const char* needle) {
    if (!text || !needle) runtime_text_failure("null string");
    const std::string_view source(text), query(needle);
    validate_utf8(source);
    validate_utf8(query);
    const auto pos = source.find(query);
    if (pos == std::string_view::npos) return -1;
    // Valid UTF-8 is self-synchronizing: a valid query cannot begin at a
    // continuation byte in a valid source. Count code points only up to the match.
    return static_cast<long long>(utf8_prefix_length(source, pos));
}
extern "C" char* quidra_string_slice(const char* text,long long start,long long end){if(!text)runtime_text_failure("null string");const std::string_view a(text);const auto offsets=utf8_offsets(a);const auto length=static_cast<long long>(offsets.size()-1);if(start<0||end<start||end>length)runtime_text_failure("string slice is outside [0, len]");return runtime_copy_string(std::string(a.substr(offsets[static_cast<std::size_t>(start)],offsets[static_cast<std::size_t>(end)]-offsets[static_cast<std::size_t>(start)])));}
extern "C" char* quidra_string_trim(const char* text){if(!text)runtime_text_failure("null string");const std::string_view a(text);const auto offsets=utf8_offsets(a);std::size_t first=0,last=offsets.size()-1;while(first<last){std::size_t c=offsets[first];if(!unicode_space(utf8_next(a,c)))break;++first;}while(last>first){std::size_t c=offsets[last-1];if(!unicode_space(utf8_next(a,c)))break;--last;}return runtime_copy_string(std::string(a.substr(offsets[first],offsets[last]-offsets[first])));}
extern "C" void* quidra_string_split(const char* text, const char* separator) {
    if (!text || !separator) runtime_text_failure("null string");
    const std::string_view source(text), delimiter(separator);
    validate_utf8(source);
    validate_utf8(delimiter);
    if (delimiter.empty()) runtime_text_failure("string split separator cannot be empty");

    std::vector<std::string> pieces;
    std::size_t start = 0;
    while (true) {
        const auto pos = source.find(delimiter, start);
        if (pos == std::string_view::npos) {
            pieces.emplace_back(source.substr(start));
            break;
        }
        pieces.emplace_back(source.substr(start, pos - start));
        start = pos + delimiter.size();
    }

    if (pieces.size() > (std::numeric_limits<std::size_t>::max() - 8) / sizeof(char*)) {
        runtime_allocation_failure();
    }
    const auto bytes = 8 + pieces.size() * sizeof(char*);
    auto* result = static_cast<unsigned char*>(managed_allocate(bytes));
    const auto count = static_cast<long long>(pieces.size());
    std::memcpy(result, &count, sizeof(count));
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        auto* item = runtime_copy_string(pieces[i]);
        std::memcpy(result + 8 + i * sizeof(char*), &item, sizeof(item));
    }
    return result;
}
extern "C" bool quidra_string_can_append_move(void* raw) {
    if (!raw) return false;
    const auto it =
        managed_allocations.find(reinterpret_cast<std::uintptr_t>(raw));
    if (it == managed_allocations.end()) return false;
    const auto& allocation = it->second;
    return allocation.owners == 1 && allocation.pins == 0 &&
           !allocation.initialization && allocation.drop == nullptr &&
           allocation.size != 0;
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
    const auto original = cached_string_view(raw, receiver);
    if (!receiver || !receiver->string_utf8_validated) {
        validate_utf8(original);
        if (receiver) receiver->string_utf8_validated = true;
    }
    const auto old_length = original.size();

    std::vector<std::size_t> lengths(count);
    std::vector<unsigned char> aliases(count, 0);
    std::size_t added = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (!suffixes[i]) runtime_text_failure("null string in append");
        aliases[i] = suffixes[i] == raw ? 1 : 0;
        const std::string_view suffix(suffixes[i]);
        validate_utf8(suffix);
        lengths[i] = suffix.size();
        if (lengths[i] > std::numeric_limits<std::size_t>::max() - added) {
            runtime_allocation_failure();
        }
        added += lengths[i];
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
        clear_managed_range_cache(&allocation);
        managed_ranges.erase(old_key);
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
            managed_allocations.insert(std::move(node));
        } else {
            allocation.base = result;
            allocation.size = new_bytes;
        }
        managed_ranges.emplace(new_key, new_bytes);
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
    it->second.string_codepoint_length_known = false;
    it->second.string_utf8_validated = true;
    return result;
}

extern "C" char* quidra_string_concat_many(const char* const* values,
                                                 unsigned long long raw_count) {
    if (raw_count > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        runtime_allocation_failure();
    }
    const auto count = static_cast<std::size_t>(raw_count);
    if (count != 0 && !values) runtime_text_failure("null string concat values");

    std::size_t total = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (!values[i]) runtime_text_failure("null string in concatenation");
        const auto length = std::strlen(values[i]);
        if (length > std::numeric_limits<std::size_t>::max() - total - 1) {
            runtime_allocation_failure();
        }
        total += length;
    }

    auto* result = static_cast<char*>(managed_allocate(total + 1));
    std::size_t offset = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto length = std::strlen(values[i]);
        if (length != 0) std::memcpy(result + offset, values[i], length);
        offset += length;
    }
    result[total] = '\0';
    return result;
}

extern "C" void* quidra_string_utf8(const char* text) {
    if (!text) runtime_text_failure("null string");
    const std::string_view source(text);
    validate_utf8(source);
    if (source.size() > (std::numeric_limits<std::size_t>::max() - 8)) {
        runtime_allocation_failure();
    }
    auto* result = static_cast<unsigned char*>(managed_allocate(8 + source.size()));
    const auto count = static_cast<long long>(source.size());
    std::memcpy(result, &count, sizeof(count));
    if (!source.empty()) std::memcpy(result + 8, source.data(), source.size());
    return result;
}

extern "C" void* quidra_string_codepoints(const char* text) {
    if (!text) runtime_text_failure("null string");
    const std::string_view source(text);
    validate_utf8(source);
    const auto count_size = utf8_length(source);
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

    const std::string_view delimiter(separator);
    validate_utf8(delimiter);

    std::size_t total = 0;
    if (count > 1 && delimiter.size() != 0) {
        if (count - 1 > std::numeric_limits<std::size_t>::max() / delimiter.size()) {
            runtime_allocation_failure();
        }
        total = (count - 1) * delimiter.size();
    }
    for (std::size_t i = 0; i < count; ++i) {
        char* item = nullptr;
        std::memcpy(&item,
                    static_cast<unsigned char*>(raw) + 8 + i * sizeof(char*),
                    sizeof(item));
        if (!item) runtime_text_failure("null string in join");
        const std::string_view piece(item);
        validate_utf8(piece);
        if (piece.size() > std::numeric_limits<std::size_t>::max() - total) {
            runtime_allocation_failure();
        }
        total += piece.size();
    }
    if (total == std::numeric_limits<std::size_t>::max()) {
        runtime_allocation_failure();
    }

    auto* result = static_cast<char*>(managed_allocate(total + 1));
    std::size_t offset = 0;
    for (std::size_t i = 0; i < count; ++i) {
        char* item = nullptr;
        std::memcpy(&item,
                    static_cast<unsigned char*>(raw) + 8 + i * sizeof(char*),
                    sizeof(item));
        const std::string_view piece(item);
        if (i != 0 && !delimiter.empty()) {
            std::memcpy(result + offset, delimiter.data(), delimiter.size());
            offset += delimiter.size();
        }
        if (!piece.empty()) {
            std::memcpy(result + offset, piece.data(), piece.size());
            offset += piece.size();
        }
    }
    result[total] = '\0';
    return result;
}



extern "C" void quidra_runtime_set_args(int argc, char** argv) {
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
    while (true) {
        const int ch = std::fgetc(stdin);
        if (ch == EOF) {
            if (std::ferror(stdin)) return -1;
            if (text.empty()) return 0;
            break;
        }
        if (ch == '\n') break;
        text.push_back(static_cast<char>(ch));
    }

    if (!valid_runtime_text(text)) return -1;
    *out = runtime_copy_string(text);
    return 1;
}

extern "C" bool quidra_parse_signed(const char* text, long long* out) {
    if (!text || !out || !*text) return false;
    errno = 0;
    char* end = nullptr;
    const auto value = std::strtoll(text, &end, 10);
    if (errno == ERANGE || end == text || !end || *end != '\0') return false;
    *out = value;
    return true;
}

extern "C" bool quidra_parse_unsigned(const char* text, unsigned long long* out) {
    if (!text || !out || !*text || *text == '-') return false;
    errno = 0;
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || !end || *end != '\0') return false;
    *out = value;
    return true;
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

extern "C" bool quidra_cli_parse_bool(const char* text) {
    if (text && std::strcmp(text, "true") == 0) return true;
    if (text && std::strcmp(text, "false") == 0) return false;
    cli_fail("bool values must be true or false");
}


namespace {

void image_tensor_require_chw(
    const TensorValue& value,const char* operation,
    unsigned long long line,unsigned long long column) {
    if(value.shape.size()!=3)
        tensor_fail((std::string(operation)+" requires a rank-3 CHW tensor").c_str(),line,column);
    if(value.shape[0]<=0||value.shape[1]<=0||value.shape[2]<=0)
        tensor_fail((std::string(operation)+" requires positive CHW dimensions").c_str(),line,column);
    tensor_require_initialized(value,line,column);
}

TensorValue* image_tensor_output(
    int dtype,const std::vector<long long>& shape,int device,
    unsigned long long line,unsigned long long column) {
    const auto count=tensor_element_count(shape,line,column);
    auto* storage=tensor_storage_create(dtype,count,1,device,line,column);
    auto strides=tensor_contiguous_strides(shape);
    return tensor_descriptor(storage,shape,std::move(strides),0);
}

const TensorStorage* image_tensor_dense_gpu(
    const TensorValue& source,TensorStorage*& materialized,
    unsigned long long line,unsigned long long column) {
    materialized=nullptr;
    if(tensor_is_contiguous_value(source)&&source.offset==0)
        return source.storage;
    materialized=tensor_gpu_materialize_storage(source,line,column);
    return materialized;
}

template <typename T>
void image_morphology_cpu(
    const TensorValue& input,TensorStorage& output,std::size_t channels,
    std::size_t height,std::size_t width,std::size_t radius,bool dilate) {
    for(std::size_t c=0;c<channels;++c)
        for(std::size_t y=0;y<height;++y)
            for(std::size_t x=0;x<width;++x){
                const auto center=tensor_storage_index(
                    input,(c*height+y)*width+x);
                T best{};
                std::memcpy(&best,input.storage->data.data()+center*sizeof(T),sizeof(T));
                const auto y0=y>radius?y-radius:0;
                const auto y1=std::min(height-1,y+std::min(radius,height-1-y));
                const auto x0=x>radius?x-radius:0;
                const auto x1=std::min(width-1,x+std::min(radius,width-1-x));
                for(std::size_t sy=y0;sy<=y1;++sy)
                    for(std::size_t sx=x0;sx<=x1;++sx){
                        const auto source_index=tensor_storage_index(
                            input,(c*height+sy)*width+sx);
                        T candidate{};
                        std::memcpy(&candidate,
                            input.storage->data.data()+source_index*sizeof(T),sizeof(T));
                        if(dilate?candidate>best:candidate<best) best=candidate;
                    }
                const auto out_index=(c*height+y)*width+x;
                std::memcpy(output.data.data()+out_index*sizeof(T),&best,sizeof(T));
            }
}

} // namespace

extern "C" void* quidra_image_tensor_geometry(
    void* raw,int dtype,int operation,
    long long p0,long long p1,long long p2,long long p3,
    unsigned long long line,unsigned long long column) {
    if(!raw) tensor_fail("image tensor geometry received a null tensor",line,column);
    auto& input=*static_cast<TensorValue*>(raw);
    if(input.storage->dtype!=dtype)
        tensor_fail("image tensor geometry dtype mismatch",line,column);
    image_tensor_require_chw(input,"image tensor geometry",line,column);
    const auto channels=static_cast<std::size_t>(input.shape[0]);
    const auto ih=static_cast<std::size_t>(input.shape[1]);
    const auto iw=static_cast<std::size_t>(input.shape[2]);
    std::size_t oh=ih,ow=iw,param0=0,param1=0;

    if(operation==1){
        if(p0<0||p1<0||p2<=0||p3<=0)
            tensor_fail("image crop requires nonnegative origin and positive size",line,column);
        if(p0>=input.shape[1]||p1>=input.shape[2]||
           p2>input.shape[1]-p0||p3>input.shape[2]-p1)
            tensor_fail("image crop rectangle exceeds the image",line,column);
        param0=static_cast<std::size_t>(p0);
        param1=static_cast<std::size_t>(p1);
        oh=static_cast<std::size_t>(p2);
        ow=static_cast<std::size_t>(p3);
    }else if(operation==2){
        if(p0<=0||p1<=0)
            tensor_fail("image resize requires positive output dimensions",line,column);
        oh=static_cast<std::size_t>(p0);
        ow=static_cast<std::size_t>(p1);
    }else if(operation==3||operation==4){
    }else if(operation==5||operation==6){
        oh=iw; ow=ih;
    }else{
        tensor_fail("unknown image tensor geometry operation",line,column);
    }

    std::vector<long long> shape{
        static_cast<long long>(channels),
        static_cast<long long>(oh),
        static_cast<long long>(ow)};
    auto* result=image_tensor_output(dtype,shape,input.storage->device,line,column);

    if(!tensor_on_cpu(*input.storage)){
        TensorStorage* materialized=nullptr;
        const auto* source=image_tensor_dense_gpu(input,materialized,line,column);
        std::string backend_error;
        const bool ok=quidra::device::compute_image_geometry(
            result->storage->gpu_buffer,source->gpu_buffer,dtype,
            channels,ih,iw,oh,ow,operation,param0,param1,backend_error);
        if(materialized) tensor_storage_release(materialized);
        if(!ok){
            quidra_tensor_drop(result);
            tensor_fail(backend_error.c_str(),line,column);
        }
        return result;
    }

    const auto bytes=tensor_dtype_bytes(dtype);
    const auto output_count=channels*oh*ow;
    for(std::size_t linear=0;linear<output_count;++linear){
        const auto x=linear%ow;
        const auto y=(linear/ow)%oh;
        const auto c=linear/(ow*oh);
        std::size_t sy=0,sx=0;
        if(operation==1){sy=param0+y;sx=param1+x;}
        else if(operation==2){sy=y*ih/oh;sx=x*iw/ow;}
        else if(operation==3){sy=y;sx=iw-1-x;}
        else if(operation==4){sy=ih-1-y;sx=x;}
        else if(operation==5){sy=ih-1-x;sx=y;}
        else {sy=x;sx=iw-1-y;}
        const auto source_index=tensor_storage_index(input,(c*ih+sy)*iw+sx);
        std::memcpy(
            result->storage->data.data()+linear*bytes,
            input.storage->data.data()+source_index*bytes,bytes);
    }
    return result;
}

extern "C" void* quidra_image_tensor_grayscale(
    void* raw,unsigned long long line,unsigned long long column) {
    if(!raw) tensor_fail("image grayscale received a null tensor",line,column);
    auto& input=*static_cast<TensorValue*>(raw);
    image_tensor_require_chw(input,"image grayscale",line,column);
    if(input.storage->dtype!=5)
        tensor_fail("image grayscale requires tensor<uint8>",line,column);
    const auto channels=static_cast<std::size_t>(input.shape[0]);
    const auto height=static_cast<std::size_t>(input.shape[1]);
    const auto width=static_cast<std::size_t>(input.shape[2]);
    if(channels!=1&&channels!=3&&channels!=4)
        tensor_fail("image grayscale requires 1, 3, or 4 channels",line,column);
    std::vector<long long> shape{1,input.shape[1],input.shape[2]};
    auto* result=image_tensor_output(5,shape,input.storage->device,line,column);
    if(!tensor_on_cpu(*input.storage)){
        TensorStorage* materialized=nullptr;
        const auto* source=image_tensor_dense_gpu(input,materialized,line,column);
        std::string backend_error;
        const bool ok=quidra::device::compute_image_grayscale(
            result->storage->gpu_buffer,source->gpu_buffer,
            channels,height,width,backend_error);
        if(materialized) tensor_storage_release(materialized);
        if(!ok){
            quidra_tensor_drop(result);
            tensor_fail(backend_error.c_str(),line,column);
        }
        return result;
    }
    const auto pixels=height*width;
    for(std::size_t i=0;i<pixels;++i){
        std::uint8_t value{};
        if(channels==1){
            const auto index=tensor_storage_index(input,i);
            value=input.storage->data[index];
        }else{
            const auto ri=tensor_storage_index(input,i);
            const auto gi=tensor_storage_index(input,pixels+i);
            const auto bi=tensor_storage_index(input,2*pixels+i);
            const double luminance=
                0.299*input.storage->data[ri]+
                0.587*input.storage->data[gi]+
                0.114*input.storage->data[bi];
            value=static_cast<std::uint8_t>(
                std::clamp<long long>(std::llround(luminance),0,255));
        }
        result->storage->data[i]=value;
    }
    return result;
}

extern "C" void* quidra_image_tensor_threshold(
    void* raw,std::uint8_t cutoff,std::uint8_t low,std::uint8_t high,
    unsigned long long line,unsigned long long column) {
    if(!raw) tensor_fail("image threshold received a null tensor",line,column);
    auto& input=*static_cast<TensorValue*>(raw);
    image_tensor_require_chw(input,"image threshold",line,column);
    if(input.storage->dtype!=5)
        tensor_fail("image threshold requires tensor<uint8>",line,column);
    auto* result=image_tensor_output(5,input.shape,input.storage->device,line,column);
    const auto count=tensor_logical_count(input);
    if(!tensor_on_cpu(*input.storage)){
        TensorStorage* materialized=nullptr;
        const auto* source=image_tensor_dense_gpu(input,materialized,line,column);
        std::string backend_error;
        const bool ok=quidra::device::compute_image_threshold(
            result->storage->gpu_buffer,source->gpu_buffer,count,
            cutoff,low,high,backend_error);
        if(materialized) tensor_storage_release(materialized);
        if(!ok){
            quidra_tensor_drop(result);
            tensor_fail(backend_error.c_str(),line,column);
        }
        return result;
    }
    for(std::size_t i=0;i<count;++i){
        const auto source=tensor_storage_index(input,i);
        result->storage->data[i]=input.storage->data[source]>=cutoff?high:low;
    }
    return result;
}

extern "C" void* quidra_image_tensor_blur(
    void* raw,long long radius,
    unsigned long long line,unsigned long long column) {
    if(!raw) tensor_fail("image blur received a null tensor",line,column);
    auto& input=*static_cast<TensorValue*>(raw);
    image_tensor_require_chw(input,"image blur",line,column);
    if(input.storage->dtype!=5)
        tensor_fail("image blur requires tensor<uint8>",line,column);
    if(radius<0) tensor_fail("image blur radius must be nonnegative",line,column);
    const auto channels=static_cast<std::size_t>(input.shape[0]);
    const auto height=static_cast<std::size_t>(input.shape[1]);
    const auto width=static_cast<std::size_t>(input.shape[2]);
    const auto r=static_cast<std::size_t>(radius);
    auto* result=image_tensor_output(5,input.shape,input.storage->device,line,column);
    if(!tensor_on_cpu(*input.storage)){
        TensorStorage* materialized=nullptr;
        const auto* source=image_tensor_dense_gpu(input,materialized,line,column);
        std::string backend_error;
        const bool ok=quidra::device::compute_image_blur(
            result->storage->gpu_buffer,source->gpu_buffer,
            channels,height,width,r,backend_error);
        if(materialized) tensor_storage_release(materialized);
        if(!ok){
            quidra_tensor_drop(result);
            tensor_fail(backend_error.c_str(),line,column);
        }
        return result;
    }
    for(std::size_t c=0;c<channels;++c)
        for(std::size_t y=0;y<height;++y)
            for(std::size_t x=0;x<width;++x){
                std::uint64_t total=0,count=0;
                const auto y0=y>r?y-r:0;
                const auto y1=std::min(height-1,y+std::min(r,height-1-y));
                const auto x0=x>r?x-r:0;
                const auto x1=std::min(width-1,x+std::min(r,width-1-x));
                for(std::size_t sy=y0;sy<=y1;++sy)
                    for(std::size_t sx=x0;sx<=x1;++sx){
                        const auto source=tensor_storage_index(
                            input,(c*height+sy)*width+sx);
                        total+=input.storage->data[source];
                        ++count;
                    }
                result->storage->data[(c*height+y)*width+x]=
                    static_cast<std::uint8_t>(total/count);
            }
    return result;
}

extern "C" void* quidra_image_tensor_filter(
    void* raw,void* kernel_raw,long long divisor,long long offset,
    unsigned long long line,unsigned long long column) {
    if(!raw||!kernel_raw)
        tensor_fail("image filter received a null tensor",line,column);
    auto& input=*static_cast<TensorValue*>(raw);
    auto& kernel=*static_cast<TensorValue*>(kernel_raw);
    image_tensor_require_chw(input,"image filter",line,column);
    tensor_require_initialized(kernel,line,column);
    if(input.storage->dtype!=5||kernel.storage->dtype!=1||kernel.shape.size()!=2)
        tensor_fail("image filter requires tensor<uint8> pixels and a rank-2 tensor<int> kernel",line,column);
    if(kernel.shape[0]<=0||kernel.shape[1]<=0)
        tensor_fail("image filter requires positive kernel dimensions",line,column);
    if(divisor==0) tensor_fail("image filter divisor must not be zero",line,column);
    if(input.storage->device!=kernel.storage->device)
        tensor_fail("image filter tensors must be on the same device",line,column);
    const auto channels=static_cast<std::size_t>(input.shape[0]);
    const auto height=static_cast<std::size_t>(input.shape[1]);
    const auto width=static_cast<std::size_t>(input.shape[2]);
    const auto kh=static_cast<std::size_t>(kernel.shape[0]);
    const auto kw=static_cast<std::size_t>(kernel.shape[1]);
    auto* result=image_tensor_output(5,input.shape,input.storage->device,line,column);

    if(!tensor_on_cpu(*input.storage)){
        TensorStorage* input_mat=nullptr;
        TensorStorage* kernel_mat=nullptr;
        const auto* input_store=image_tensor_dense_gpu(input,input_mat,line,column);
        const auto* kernel_store=image_tensor_dense_gpu(kernel,kernel_mat,line,column);
        std::string backend_error;
        const bool ok=quidra::device::compute_image_filter(
            result->storage->gpu_buffer,input_store->gpu_buffer,kernel_store->gpu_buffer,
            channels,height,width,kh,kw,divisor,offset,backend_error);
        if(input_mat) tensor_storage_release(input_mat);
        if(kernel_mat) tensor_storage_release(kernel_mat);
        if(!ok){
            quidra_tensor_drop(result);
            tensor_fail(backend_error.c_str(),line,column);
        }
        return result;
    }

    const auto cy=kh/2,cx=kw/2;
    for(std::size_t c=0;c<channels;++c)
        for(std::size_t y=0;y<height;++y)
            for(std::size_t x=0;x<width;++x){
                std::int64_t total=0;
                for(std::size_t ky=0;ky<kh;++ky){
                    const auto sy=static_cast<long long>(y)+
                        static_cast<long long>(ky)-static_cast<long long>(cy);
                    if(sy<0||sy>=static_cast<long long>(height)) continue;
                    for(std::size_t kx=0;kx<kw;++kx){
                        const auto sx=static_cast<long long>(x)+
                            static_cast<long long>(kx)-static_cast<long long>(cx);
                        if(sx<0||sx>=static_cast<long long>(width)) continue;
                        const auto pi=tensor_storage_index(
                            input,(c*height+static_cast<std::size_t>(sy))*width+
                                  static_cast<std::size_t>(sx));
                        const auto ki=tensor_storage_index(kernel,ky*kw+kx);
                        std::int64_t weight{};
                        std::memcpy(&weight,kernel.storage->data.data()+ki*8,8);
                        std::int64_t product{},next{};
                        if(!tensor_mul_checked(
                               static_cast<std::int64_t>(input.storage->data[pi]),
                               weight,product)||
                           !tensor_add_checked(total,product,next)){
                            quidra_tensor_drop(result);
                            tensor_fail("image filter integer arithmetic overflow",line,column);
                        }
                        total=next;
                    }
                }
                if(total==std::numeric_limits<std::int64_t>::min()&&divisor==-1){
                    quidra_tensor_drop(result);
                    tensor_fail("image filter integer arithmetic overflow",line,column);
                }
                const auto divided=total/divisor;
                std::int64_t adjusted{};
                if(!tensor_add_checked(divided,offset,adjusted)){
                    quidra_tensor_drop(result);
                    tensor_fail("image filter integer arithmetic overflow",line,column);
                }
                const auto clamped=std::clamp<std::int64_t>(adjusted,0,255);
                result->storage->data[(c*height+y)*width+x]=
                    static_cast<std::uint8_t>(clamped);
            }
    return result;
}

extern "C" void* quidra_image_tensor_morphology(
    void* raw,int dtype,long long radius,bool dilate,
    unsigned long long line,unsigned long long column) {
    if(!raw) tensor_fail("image morphology received a null tensor",line,column);
    auto& input=*static_cast<TensorValue*>(raw);
    if(input.storage->dtype!=dtype)
        tensor_fail("image morphology dtype mismatch",line,column);
    image_tensor_require_chw(input,"image morphology",line,column);
    if(radius<0)
        tensor_fail("image morphology radius must be nonnegative",line,column);
    const auto channels=static_cast<std::size_t>(input.shape[0]);
    const auto height=static_cast<std::size_t>(input.shape[1]);
    const auto width=static_cast<std::size_t>(input.shape[2]);
    const auto r=static_cast<std::size_t>(radius);
    auto* result=image_tensor_output(dtype,input.shape,input.storage->device,line,column);
    if(!tensor_on_cpu(*input.storage)){
        TensorStorage* materialized=nullptr;
        const auto* source=image_tensor_dense_gpu(input,materialized,line,column);
        std::string backend_error;
        const bool ok=quidra::device::compute_image_morphology(
            result->storage->gpu_buffer,source->gpu_buffer,dtype,
            channels,height,width,r,dilate,backend_error);
        if(materialized) tensor_storage_release(materialized);
        if(!ok){
            quidra_tensor_drop(result);
            tensor_fail(backend_error.c_str(),line,column);
        }
        return result;
    }
    switch(dtype){
        case 1:image_morphology_cpu<std::int64_t>(input,*result->storage,channels,height,width,r,dilate);break;
        case 2:image_morphology_cpu<std::int8_t>(input,*result->storage,channels,height,width,r,dilate);break;
        case 3:image_morphology_cpu<std::int16_t>(input,*result->storage,channels,height,width,r,dilate);break;
        case 4:image_morphology_cpu<std::int32_t>(input,*result->storage,channels,height,width,r,dilate);break;
        case 5:image_morphology_cpu<std::uint8_t>(input,*result->storage,channels,height,width,r,dilate);break;
        case 6:image_morphology_cpu<std::uint16_t>(input,*result->storage,channels,height,width,r,dilate);break;
        case 7:image_morphology_cpu<std::uint32_t>(input,*result->storage,channels,height,width,r,dilate);break;
        case 8:image_morphology_cpu<std::uint64_t>(input,*result->storage,channels,height,width,r,dilate);break;
        case 9:image_morphology_cpu<double>(input,*result->storage,channels,height,width,r,dilate);break;
        case 10:image_morphology_cpu<float>(input,*result->storage,channels,height,width,r,dilate);break;
        default:
            quidra_tensor_drop(result);
            tensor_fail("image morphology received an unsupported dtype",line,column);
    }
    return result;
}
