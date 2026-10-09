#pragma once

// The runtime codes of IR element types: the dtype of a tensor element type
// and the element kind of a sorted array (quidra/abi/dtype.hpp).
//
// Owns: the mapping from Type to the codes. The backend passes the codes to
// the runtime; later stages that plan tensor storage use the same mapping.

#include "quidra/abi/dtype.hpp"
#include "quidra/types.hpp"

#include <stdexcept>

namespace quidra::ir {

// The dtype of a tensor element type; throws std::logic_error for a type
// that is not one.
inline abi::Dtype dtype_of(const Type& type) {
    if (const auto* numeric = numeric_kind_info(type.kind); numeric && numeric->dtype) {
        return *numeric->dtype;
    }
    if (type.kind == TypeKind::Bool) return abi::Dtype::boolean;
    throw std::logic_error("unsupported tensor element type");
}

// The element kind of a sorted array of `type`; throws std::logic_error for
// an element type the runtime cannot sort.
inline int sort_element_kind_of(const Type& type) {
    if (const auto* numeric = numeric_kind_info(type.kind); numeric && numeric->sort_kind) {
        return abi::sort_element_kind_code(*numeric->sort_kind);
    }
    if (type.kind == TypeKind::Bool) return abi::sort_element_kind_code(abi::SortElementKind::boolean);
    if (type.kind == TypeKind::String) return abi::sort_element_kind_code(abi::SortElementKind::string);
    throw std::logic_error("unsupported sorted array element type");
}

} // namespace quidra::ir
