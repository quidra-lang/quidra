#include "llvm_backend/foreign_abi.hpp"

namespace quidra::llvm_backend {

std::string_view c_abi_return_attribute(const Type& type) {
    switch (type.kind) {
        case TypeKind::Int8:
        case TypeKind::Int16:
            return "signext ";
        case TypeKind::Nat8:
        case TypeKind::Nat16:
        case TypeKind::Bool:
            return "zeroext ";
        default:
            return "";
    }
}

std::string_view c_abi_parameter_attribute(const Type& type, bool readonly_buffer) {
    switch (type.kind) {
        case TypeKind::Int8:
        case TypeKind::Int16:
            return " signext";
        case TypeKind::Nat8:
        case TypeKind::Nat16:
        case TypeKind::Bool:
            return " zeroext";
        case TypeKind::String:
            return " nocapture nonnull readonly";
        case TypeKind::Bin:
        case TypeKind::Tensor:
            return readonly_buffer
                ? " nocapture nonnull readonly"
                : " nocapture nonnull";
        default:
            return "";
    }
}

std::string_view export_return_attribute(const Type& type) {
    return c_abi_return_attribute(type);
}

std::string_view export_parameter_attribute(const Type&) {
    return "";
}

} // namespace quidra::llvm_backend
