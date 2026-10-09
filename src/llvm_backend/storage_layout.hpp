#pragma once

// Storage layout of arrays and class objects in the generated program.
//
// Owns: which nested fixed-array edges stay boxed (ArrayLayoutPolicy, one per
// module; every other fixed array inside a fixed array is stored inline in
// its parent), the leaf type and element count of an inline fixed array
// (FixedArrayStorage), element strides, and the field offsets and byte size
// of a class object. Sizes come from runtime_storage_bytes (quidra/types.hpp).

#include "quidra/ir/module.hpp"
#include "llvm_backend/type_lowering.hpp"

#include <cstddef>
#include <string>
#include <unordered_set>

namespace quidra::llvm_backend {

inline bool is_fixed_array(const Type& type) {
    return type.kind == TypeKind::Array && type.length >= 0;
}

struct ArrayLayoutPolicy {
    std::unordered_set<std::string> boxed_fixed_edges;

    bool inline_fixed_child(const Type& parent) const {
        return is_fixed_array(parent) && parent.first && is_fixed_array(*parent.first) &&
               !boxed_fixed_edges.contains(type_identifier(parent));
    }
};

// left * right; throws std::overflow_error when the product overflows.
std::size_t checked_storage_product(std::size_t left, std::size_t right);

struct FixedArrayStorage {
    Type leaf;
    std::size_t count{};
};

FixedArrayStorage fixed_array_storage(const Type& type, const ArrayLayoutPolicy& policy);
std::size_t fixed_array_storage_bytes(const Type& type, const ArrayLayoutPolicy& policy);
std::size_t array_element_stride(const Type& array, const ArrayLayoutPolicy& policy);
ArrayLayoutPolicy collect_array_layout_policy(const ir::Module& module);
std::size_t class_field_offset(const ir::ClassLayout& layout, std::size_t index);
std::size_t class_storage_bytes(const ir::ClassLayout& layout);
// The bytes of a union box for a value of `type`: the tag and a payload of
// eight bytes, or of the largest case's size when that is larger (a
// `real`).
std::size_t union_box_bytes(const Type& type);

} // namespace quidra::llvm_backend
