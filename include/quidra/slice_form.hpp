#pragma once

// The written form of a slice item: how each side is written, the
// markers' directions, and the slice as text for messages. Shared by the
// parser, the checker, the slice planner (quidra/slice_bounds.hpp) and the
// runtime.
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace quidra {

// The written form of each side of a slice item `[start:end:step]`: a
// value without a marker (inclusive), a value with a marker that touches
// the first colon (`s<:` / `:<e` ascend, `s>:` / `:>e` descend; the marked
// value is excluded), or no value.
enum class SliceStart : std::uint8_t {
    inclusive, exclusive_ascending, exclusive_descending, omitted
};
enum class SliceEnd : std::uint8_t {
    inclusive, exclusive_ascending, exclusive_descending, omitted
};

// The direction the checker resolves for a slice item: from its markers,
// else from a constant step's sign; by_step_sign when a run-time step
// decides it, or by_bounds for two written unmarked endpoints without a
// step. Other omitted forms default to ascending. The enumerator values are part of the runtime
// ABI (quidra_slice_plan), like the forms'.
enum class SliceDirection : std::uint8_t { ascending, descending, by_step_sign, by_bounds };

// The direction a side's marker gives: +1 ascending, -1 descending, 0 none.
constexpr int slice_marker_direction(SliceStart form) {
    return form == SliceStart::exclusive_ascending ? 1
         : form == SliceStart::exclusive_descending ? -1 : 0;
}
constexpr int slice_marker_direction(SliceEnd form) {
    return form == SliceEnd::exclusive_ascending ? 1
         : form == SliceEnd::exclusive_descending ? -1 : 0;
}

// The marker as written next to the colon, for messages.
constexpr const char* slice_marker_text(SliceStart form) {
    return form == SliceStart::exclusive_ascending ? "<:"
         : form == SliceStart::exclusive_descending ? ">:" : ":";
}
constexpr const char* slice_marker_text(SliceEnd form) {
    return form == SliceEnd::exclusive_ascending ? ":<"
         : form == SliceEnd::exclusive_descending ? ":>" : ":";
}

// The written form of a slice for messages (`2:<7`, `10>:0:-2`, `:7`): the
// values its operands had, its markers as written, an omitted side empty.
struct SliceFormText {
    char data[96]{};
    std::size_t size{};
    constexpr std::string_view view() const { return std::string_view(data, size); }
    constexpr void append(std::string_view text) {
        for (const char c : text) data[size++] = c;
    }
    constexpr void append(std::int64_t value) {
        auto magnitude = value < 0 ? 0ULL - static_cast<unsigned long long>(value)
                                   : static_cast<unsigned long long>(value);
        char reversed[20]{};
        std::size_t count = 0;
        do {
            reversed[count++] = static_cast<char>('0' + magnitude % 10);
            magnitude /= 10;
        } while (magnitude != 0);
        if (value < 0) data[size++] = '-';
        while (count != 0) data[size++] = reversed[--count];
    }
};

constexpr SliceFormText slice_form_text(SliceStart start_form, std::int64_t start,
                                        SliceEnd end_form, std::int64_t end,
                                        bool has_step = false, std::int64_t step = 0) {
    SliceFormText text;
    if (start_form != SliceStart::omitted) text.append(start);
    if (start_form == SliceStart::exclusive_ascending) text.append("<");
    if (start_form == SliceStart::exclusive_descending) text.append(">");
    text.append(":");
    if (end_form == SliceEnd::exclusive_ascending) text.append("<");
    if (end_form == SliceEnd::exclusive_descending) text.append(">");
    if (end_form != SliceEnd::omitted) text.append(end);
    if (has_step) {
        text.append(":");
        text.append(step);
    }
    return text;
}

} // namespace quidra
