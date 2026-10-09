#include "llvm_backend/storage_layout.hpp"

#include "quidra/abi/layout.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <variant>

namespace quidra::llvm_backend {

std::size_t checked_storage_product(std::size_t left, std::size_t right) {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        throw std::overflow_error("fixed array storage size overflow");
    }
    return left * right;
}

FixedArrayStorage fixed_array_storage(const Type& type, const ArrayLayoutPolicy& policy) {
    if (!is_fixed_array(type)) throw std::logic_error("fixed array layout requested for dynamic type");
    std::size_t count = 1;
    const Type* current = &type;
    while (true) {
        count = checked_storage_product(
            count, static_cast<std::size_t>(current->length));
        const auto& element = *current->first;
        if (is_fixed_array(element) && policy.inline_fixed_child(*current)) {
            current = &element;
            continue;
        }
        return FixedArrayStorage{element, count};
    }
}

std::size_t fixed_array_storage_bytes(const Type& type, const ArrayLayoutPolicy& policy) {
    const auto storage = fixed_array_storage(type, policy);
    const auto stride = runtime_storage_bytes(storage.leaf);
    if (stride == 0) throw std::logic_error("fixed array leaf has no runtime storage");
    return checked_storage_product(storage.count, stride);
}

std::size_t array_element_stride(const Type& array, const ArrayLayoutPolicy& policy) {
    if (policy.inline_fixed_child(array)) {
        return fixed_array_storage_bytes(*array.first, policy);
    }
    return runtime_storage_bytes(*array.first);
}

ArrayLayoutPolicy collect_array_layout_policy(const ir::Module& module) {
    ArrayLayoutPolicy policy;
    for (const auto& function : module.functions) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (const auto* address = std::get_if<ir::AddressElement>(&instruction);
                    address && is_fixed_array(address->array_type) &&
                    is_fixed_array(address->element_type)) {
                    // A writable reference to a logical fixed-array element needs a stable
                    // pointer slot. Keep exactly this fixed edge boxed; other fixed edges
                    // remain eligible for contiguous inlining.
                    policy.boxed_fixed_edges.insert(type_identifier(address->array_type));
                }
            }
        }
    }
    return policy;
}

std::size_t class_field_offset(const ir::ClassLayout& layout, std::size_t index) {
    std::size_t offset = 0;
    for (std::size_t i = 0; i < index; ++i) offset += runtime_storage_bytes(layout.fields[i]);
    return offset;
}

std::size_t class_storage_bytes(const ir::ClassLayout& layout) {
    std::size_t bytes = 0;
    for (const auto& field : layout.fields) bytes += runtime_storage_bytes(field);
    return std::max<std::size_t>(1, bytes);
}

std::size_t union_box_bytes(const Type& type) {
    std::size_t payload = static_cast<std::size_t>(abi::union_box_layout::bytes -
                                                   abi::union_box_layout::payload_offset);
    for (const auto& current : type.cases) payload = std::max(payload, runtime_storage_bytes(current));
    return static_cast<std::size_t>(abi::union_box_layout::payload_offset) + payload;
}

} // namespace quidra::llvm_backend
