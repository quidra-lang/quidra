#pragma once

// The initialization mask of a class layout (L13): a class whose field
// initialization some read checks at run time keeps a hidden trailing nat64
// field, $init, with one bit per declared field. Every store of a field
// (FieldSet) sets its bit and ClassMake sets the bits of the fields it is
// given; a checked read tests the bit. It is not a field of the source
// class: equality and display skip it, copies keep it.

#include "quidra/ir/module.hpp"

#include <cstddef>
#include <string_view>

namespace quidra::ir {

inline constexpr std::string_view initialization_mask_field = "$init";

// Whether the last field of `layout` is its initialization mask.
inline bool has_initialization_mask(const ClassLayout& layout) {
    return !layout.field_names.empty() && layout.field_names.back() == initialization_mask_field;
}

// The index of the mask field, which is the number of declared fields.
inline std::size_t initialization_mask_index(const ClassLayout& layout) {
    return layout.field_names.size() - 1;
}

// The number of declared (source) fields of `layout`.
inline std::size_t source_field_count(const ClassLayout& layout) {
    return layout.fields.size() - (has_initialization_mask(layout) ? 1 : 0);
}

} // namespace quidra::ir
