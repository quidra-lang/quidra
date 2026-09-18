#pragma once
#include "quidra/language.hpp"
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace quidra {

enum class TypeKind {
    Int,
    Int8,
    Int16,
    Int32,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Float,
    Float32,
    Bool,
    String,
    Bytes,
    Void,
    Never,
    Error,
    None,
    Array,
    Tensor,
    Neural,
    Gradients,
    Union,
    Class,
    Auto,
    Range,
    Invalid
};

struct Type {
    TypeKind kind{TypeKind::Void};
    std::shared_ptr<Type> first;
    std::vector<Type> cases;
    // Arrays use length as their static length. Tensor/neural values use it
    // for compiler-known rank; a source shape pattern fixes rank exactly.
    long long length{-1};
    std::vector<long long> tensor_shape_prefix;
    std::vector<long long> tensor_known_shape_prefix;
    std::string class_name;

    static Type simple(TypeKind kind) {
        Type type;
        type.kind = kind;
        return type;
    }

    static Type class_type(std::string name) {
        auto type = simple(TypeKind::Class);
        type.class_name = std::move(name);
        return type;
    }

    static Type array(Type element, long long length = -1) {
        auto type = simple(TypeKind::Array);
        type.first = std::make_shared<Type>(std::move(element));
        type.length = length;
        return type;
    }

    static Type tensor(Type element, long long rank = -1,
                       std::vector<long long> shape_prefix = {},
                       std::vector<long long> known_shape_prefix = {}) {
        auto type = simple(TypeKind::Tensor);
        type.first = std::make_shared<Type>(std::move(element));
        type.length = rank;
        type.tensor_shape_prefix = std::move(shape_prefix);
        type.tensor_known_shape_prefix = std::move(known_shape_prefix);
        return type;
    }

    static Type neural(Type element = simple(TypeKind::Float32), long long rank = -1,
                       std::vector<long long> shape_pattern = {},
                       std::vector<long long> known_shape = {}) {
        auto type = simple(TypeKind::Neural);
        type.first = std::make_shared<Type>(std::move(element));
        type.length = rank;
        type.tensor_shape_prefix = std::move(shape_pattern);
        type.tensor_known_shape_prefix = std::move(known_shape);
        return type;
    }

    static Type union_of(std::vector<Type>);

    bool operator==(const Type& other) const {
        if (kind != other.kind || class_name != other.class_name ||
            cases != other.cases || bool(first) != bool(other.first) ||
            (first && *first != *other.first)) {
            return false;
        }
        if (kind == TypeKind::Array) return length == other.length;
        if (kind == TypeKind::Tensor || kind == TypeKind::Neural) {
            // Inferred rank/shape facts are flow facts. Only an explicit source
            // shape pattern participates in static type identity.
            return tensor_shape_prefix == other.tensor_shape_prefix;
        }
        return true;
    }

    bool operator!=(const Type& other) const { return !(*this == other); }
};

inline std::string type_name(const Type& type) {
    switch (type.kind) {
        case TypeKind::Int: return "int";
        case TypeKind::Int8: return "int8";
        case TypeKind::Int16: return "int16";
        case TypeKind::Int32: return "int32";
        case TypeKind::UInt8: return "uint8";
        case TypeKind::UInt16: return "uint16";
        case TypeKind::UInt32: return "uint32";
        case TypeKind::UInt64: return "uint64";
        case TypeKind::Float: return "float";
        case TypeKind::Float32: return "float32";
        case TypeKind::Bool: return "bool";
        case TypeKind::String: return "string";
        case TypeKind::Bytes: return "bytes";
        case TypeKind::Void: return "void";
        case TypeKind::Never: return "never";
        case TypeKind::Error: return "error";
        case TypeKind::None: return "none";
        case TypeKind::Auto: return "auto";
        case TypeKind::Range: return "range";
        case TypeKind::Invalid: return "<invalid>";
        case TypeKind::Class: return type.class_name;
        case TypeKind::Tensor: {
            std::string result = "tensor<" + type_name(*type.first) + ">";
            if (!type.tensor_shape_prefix.empty()) {
                result += "<";
                for (std::size_t i = 0; i < type.tensor_shape_prefix.size(); ++i) {
                    if (i) result += ", ";
                    result += type.tensor_shape_prefix[i] < 0
                        ? "_" : std::to_string(type.tensor_shape_prefix[i]);
                }
                result += ">";
            }
            return result;
        }
        case TypeKind::Neural: {
            std::string result = *type.first == Type::simple(TypeKind::Float32)
                ? "neural"
                : "neural<" + type_name(*type.first) + ">";
            if (!type.tensor_shape_prefix.empty()) {
                result += "<";
                for (std::size_t i = 0; i < type.tensor_shape_prefix.size(); ++i) {
                    if (i) result += ", ";
                    result += type.tensor_shape_prefix[i] < 0
                        ? "_" : std::to_string(type.tensor_shape_prefix[i]);
                }
                result += ">";
            }
            return result;
        }
        case TypeKind::Gradients:
            return "neural.Gradients";
        case TypeKind::Array: {
            std::string dimensions;
            const Type* element = &type;
            while (element->kind == TypeKind::Array) {
                dimensions += "[" +
                              (element->length < 0 ? std::string{} : std::to_string(element->length)) +
                              "]";
                element = element->first.get();
            }
            return type_name(*element) + dimensions;
        }
        case TypeKind::Union: {
            std::string result;
            for (const auto& current : type.cases) {
                if (!result.empty()) result += " | ";
                result += type_name(current);
            }
            return result;
        }
    }
    return "?";
}

inline Type Type::union_of(std::vector<Type> input) {
    std::vector<Type> cases;
    for (auto& current : input) {
        if (current.kind == TypeKind::Union) {
            cases.insert(cases.end(), current.cases.begin(), current.cases.end());
        } else {
            cases.push_back(current);
        }
    }
    std::sort(cases.begin(), cases.end(),
              [](const auto& left, const auto& right) { return type_name(left) < type_name(right); });
    cases.erase(std::unique(cases.begin(), cases.end()), cases.end());
    if (cases.size() == 1) return cases.front();

    auto type = simple(TypeKind::Union);
    type.cases = std::move(cases);
    return type;
}

inline bool is_storable(const Type& type) {
    return type.kind != TypeKind::Void && type.kind != TypeKind::None &&
           type.kind != TypeKind::Never && type.kind != TypeKind::Auto &&
           type.kind != TypeKind::Range;
}

inline bool is_integer(const Type& type) {
    switch (type.kind) {
        case TypeKind::Int:
        case TypeKind::Int8:
        case TypeKind::Int16:
        case TypeKind::Int32:
        case TypeKind::UInt8:
        case TypeKind::UInt16:
        case TypeKind::UInt32:
        case TypeKind::UInt64:
            return true;
        default:
            return false;
    }
}

inline bool is_signed_integer(const Type& type) {
    return type.kind == TypeKind::Int || type.kind == TypeKind::Int8 ||
           type.kind == TypeKind::Int16 || type.kind == TypeKind::Int32;
}

inline bool is_float(const Type& type) {
    return type.kind == TypeKind::Float || type.kind == TypeKind::Float32;
}

inline bool is_numeric(const Type& type) {
    return is_integer(type) || is_float(type);
}

inline int integer_width(const Type& type) {
    switch (type.kind) {
        case TypeKind::Int8:
        case TypeKind::UInt8:
            return 8;
        case TypeKind::Int16:
        case TypeKind::UInt16:
            return 16;
        case TypeKind::Int32:
        case TypeKind::UInt32:
            return 32;
        case TypeKind::Int:
        case TypeKind::UInt64:
            return 64;
        default:
            return 0;
    }
}

inline int float_precision_bits(const Type& type) {
    switch (type.kind) {
        case TypeKind::Float32: return 24;
        case TypeKind::Float: return 53;
        default: return 0;
    }
}

inline bool integer_value_fits(long long value, const Type& type) {
    switch (type.kind) {
        case TypeKind::Int8:
            return value >= std::numeric_limits<std::int8_t>::min() &&
                   value <= std::numeric_limits<std::int8_t>::max();
        case TypeKind::Int16:
            return value >= std::numeric_limits<std::int16_t>::min() &&
                   value <= std::numeric_limits<std::int16_t>::max();
        case TypeKind::Int32:
            return value >= std::numeric_limits<std::int32_t>::min() &&
                   value <= std::numeric_limits<std::int32_t>::max();
        case TypeKind::Int:
            return true;
        case TypeKind::UInt8:
            return value >= 0 &&
                   static_cast<unsigned long long>(value) <= std::numeric_limits<std::uint8_t>::max();
        case TypeKind::UInt16:
            return value >= 0 &&
                   static_cast<unsigned long long>(value) <= std::numeric_limits<std::uint16_t>::max();
        case TypeKind::UInt32:
            return value >= 0 &&
                   static_cast<unsigned long long>(value) <= std::numeric_limits<std::uint32_t>::max();
        case TypeKind::UInt64:
            return value >= 0;
        default:
            return false;
    }
}

inline bool integer_literal_value_fits(unsigned long long value, const Type& type) {
    switch (type.kind) {
        case TypeKind::Int8:
            return value <= static_cast<unsigned long long>(std::numeric_limits<std::int8_t>::max());
        case TypeKind::Int16:
            return value <= static_cast<unsigned long long>(std::numeric_limits<std::int16_t>::max());
        case TypeKind::Int32:
            return value <= static_cast<unsigned long long>(std::numeric_limits<std::int32_t>::max());
        case TypeKind::Int:
            return value <= static_cast<unsigned long long>(std::numeric_limits<std::int64_t>::max());
        case TypeKind::UInt8:
            return value <= std::numeric_limits<std::uint8_t>::max();
        case TypeKind::UInt16:
            return value <= std::numeric_limits<std::uint16_t>::max();
        case TypeKind::UInt32:
            return value <= std::numeric_limits<std::uint32_t>::max();
        case TypeKind::UInt64:
            return true;
        default:
            return false;
    }
}

inline bool float_value_fits_exactly(double value, const Type& type) {
    if (type.kind == TypeKind::Float) return true;
    if (type.kind != TypeKind::Float32) return false;
    const auto narrowed = static_cast<float>(value);
    return static_cast<double>(narrowed) == value;
}

inline bool float_value_fits_exactly_in_integer(double value, const Type& type) {
    if (!is_integer(type) || !std::isfinite(value) || std::trunc(value) != value) return false;
    const long double exact = static_cast<long double>(value);
    switch (type.kind) {
        case TypeKind::Int8:
            return exact >= std::numeric_limits<std::int8_t>::min() &&
                   exact <= std::numeric_limits<std::int8_t>::max();
        case TypeKind::Int16:
            return exact >= std::numeric_limits<std::int16_t>::min() &&
                   exact <= std::numeric_limits<std::int16_t>::max();
        case TypeKind::Int32:
            return exact >= std::numeric_limits<std::int32_t>::min() &&
                   exact <= std::numeric_limits<std::int32_t>::max();
        case TypeKind::Int:
            return exact >= static_cast<long double>(std::numeric_limits<std::int64_t>::min()) &&
                   exact <= static_cast<long double>(std::numeric_limits<std::int64_t>::max());
        case TypeKind::UInt8:
            return exact >= 0.0L && exact <= std::numeric_limits<std::uint8_t>::max();
        case TypeKind::UInt16:
            return exact >= 0.0L && exact <= std::numeric_limits<std::uint16_t>::max();
        case TypeKind::UInt32:
            return exact >= 0.0L && exact <= std::numeric_limits<std::uint32_t>::max();
        case TypeKind::UInt64:
            return exact >= 0.0L &&
                   exact <= static_cast<long double>(std::numeric_limits<std::uint64_t>::max());
        default:
            return false;
    }
}

inline bool integer_value_fits_exactly_in_float(long long value, const Type& type) {
    if (!is_float(type)) return false;
    auto magnitude = value < 0
        ? static_cast<unsigned long long>(-(value + 1)) + 1ULL
        : static_cast<unsigned long long>(value);
    if (magnitude == 0) return true;
    while ((magnitude & 1ULL) == 0) magnitude >>= 1;
    int significant_bits = 0;
    while (magnitude != 0) {
        ++significant_bits;
        magnitude >>= 1;
    }
    return significant_bits <= float_precision_bits(type);
}

inline bool integer_literal_fits_exactly_in_float(unsigned long long value, const Type& type) {
    if (!is_float(type)) return false;
    auto magnitude = value;
    if (magnitude == 0) return true;
    while ((magnitude & 1ULL) == 0) magnitude >>= 1;
    int significant_bits = 0;
    while (magnitude != 0) {
        ++significant_bits;
        magnitude >>= 1;
    }
    return significant_bits <= float_precision_bits(type);
}

inline int scalar_storage_bits(const Type& type) {
    if (is_integer(type)) return integer_width(type);
    if (type.kind == TypeKind::Float32) return 32;
    if (type.kind == TypeKind::Float) return 64;
    if (type.kind == TypeKind::Bool) return 1;
    return 0;
}

inline bool integer_range_contained(const Type& from, const Type& to) {
    if (!is_integer(from) || !is_integer(to)) return false;

    const auto from_width = integer_width(from);
    const auto to_width = integer_width(to);
    const auto from_signed = is_signed_integer(from);
    const auto to_signed = is_signed_integer(to);

    if (from_signed && to_signed) return to_width >= from_width;
    if (!from_signed && !to_signed) return to_width >= from_width;
    if (from_signed && !to_signed) return false;
    return to_width > from_width;
}

inline bool integer_range_exact_in_float(const Type& from, const Type& to) {
    if (!is_integer(from) || !is_float(to)) return false;

    const auto required_bits =
        integer_width(from) - (is_signed_integer(from) ? 1 : 0);
    return float_precision_bits(to) >= required_bits;
}


inline bool explicit_numeric_cast_supported(const Type& from, const Type& to) {
    if (!is_numeric(from) || !is_numeric(to)) return false;
    if (is_float(from) && is_integer(to)) return false;
    return true;
}

enum class NumericConversionPolicy {
    Identity,
    ExplicitRangeCheck,
    ExplicitDeterministic,
    Forbidden
};

inline NumericConversionPolicy numeric_conversion_policy(const Type& from, const Type& to) {
    if (from == to) return NumericConversionPolicy::Identity;
    if (!explicit_numeric_cast_supported(from, to)) return NumericConversionPolicy::Forbidden;
    if (is_integer(from) && is_integer(to)) return NumericConversionPolicy::ExplicitRangeCheck;
    return NumericConversionPolicy::ExplicitDeterministic;
}

inline std::optional<Type> builtin_scalar_type(std::string_view name) {
    if (const auto canonical = canonical_builtin_type_name(name)) name = *canonical;
    if (name == "int") return Type::simple(TypeKind::Int);
    if (name == "int8") return Type::simple(TypeKind::Int8);
    if (name == "int16") return Type::simple(TypeKind::Int16);
    if (name == "int32") return Type::simple(TypeKind::Int32);
    if (name == "uint8") return Type::simple(TypeKind::UInt8);
    if (name == "uint16") return Type::simple(TypeKind::UInt16);
    if (name == "uint32") return Type::simple(TypeKind::UInt32);
    if (name == "uint64") return Type::simple(TypeKind::UInt64);
    if (name == "float") return Type::simple(TypeKind::Float);
    if (name == "float32") return Type::simple(TypeKind::Float32);
    if (name == "bool") return Type::simple(TypeKind::Bool);
    if (name == "string") return Type::simple(TypeKind::String);
    if (name == "bytes") return Type::simple(TypeKind::Bytes);
    return std::nullopt;
}

inline bool is_pointer_runtime_type(const Type& type) {
    return type.kind == TypeKind::String || type.kind == TypeKind::Bytes ||
           type.kind == TypeKind::Error || type.kind == TypeKind::Array ||
           type.kind == TypeKind::Tensor || type.kind == TypeKind::Neural ||
           type.kind == TypeKind::Gradients || type.kind == TypeKind::Union ||
           type.kind == TypeKind::Class;
}

enum class ValueStoragePolicy {
    Direct,
    ImmutableShared,
    IndependentStorage
};

inline ValueStoragePolicy value_storage_policy(const Type& type) {
    if (type.kind == TypeKind::String || type.kind == TypeKind::Error ||
        (type.kind == TypeKind::Class && type.class_name == "$std.json.Value")) {
        return ValueStoragePolicy::ImmutableShared;
    }
    if (type.kind == TypeKind::Array || type.kind == TypeKind::Tensor ||
        type.kind == TypeKind::Neural || type.kind == TypeKind::Gradients ||
        type.kind == TypeKind::Bytes || type.kind == TypeKind::Class ||
        type.kind == TypeKind::Union) {
        return ValueStoragePolicy::IndependentStorage;
    }
    return ValueStoragePolicy::Direct;
}

inline bool requires_value_clone(const Type& type) {
    return value_storage_policy(type) == ValueStoragePolicy::IndependentStorage;
}

inline bool requires_lifetime_management(const Type& type) {
    return is_pointer_runtime_type(type);
}

inline bool uses_shared_immutable_storage(const Type& type) {
    return value_storage_policy(type) == ValueStoragePolicy::ImmutableShared;
}

inline std::size_t runtime_storage_bytes(const Type& type) {
    if (is_integer(type)) return static_cast<std::size_t>(integer_width(type) / 8);
    if (type.kind == TypeKind::Float32) return 4;
    if (type.kind == TypeKind::Float) return 8;
    if (type.kind == TypeKind::Bool) return 1;
    if (is_pointer_runtime_type(type)) return 8;
    return 0;
}

inline int case_index(const Type& type, const Type& current) {
    const auto it = std::find(type.cases.begin(), type.cases.end(), current);
    return it == type.cases.end() ? -1 : static_cast<int>(it - type.cases.begin());
}

inline std::optional<long long> tensor_known_extent(const Type& type, std::size_t axis) {
    if (axis < type.tensor_known_shape_prefix.size()) {
        return type.tensor_known_shape_prefix[axis];
    }
    if (axis < type.tensor_shape_prefix.size() && type.tensor_shape_prefix[axis] >= 0) {
        return type.tensor_shape_prefix[axis];
    }
    return std::nullopt;
}

inline bool tensor_satisfies_shape_prefix(const Type& from, const Type& to) {
    if (to.tensor_shape_prefix.empty()) return true;
    const auto required_rank = static_cast<long long>(to.tensor_shape_prefix.size());
    if (from.length < 0 || from.length != required_rank) return false;
    for (std::size_t axis = 0; axis < to.tensor_shape_prefix.size(); ++axis) {
        const auto required = to.tensor_shape_prefix[axis];
        if (required < 0) continue;
        const auto extent = tensor_known_extent(from, axis);
        if (!extent || *extent != required) return false;
    }
    return true;
}

inline bool representation_erasure_compatible(const Type& from, const Type& to) {
    if (from == to) return true;
    if ((from.kind == TypeKind::Tensor && to.kind == TypeKind::Tensor) ||
        (from.kind == TypeKind::Neural && to.kind == TypeKind::Neural)) {
        return from.first && to.first && *from.first == *to.first &&
               tensor_satisfies_shape_prefix(from, to);
    }
    return false;
}

inline int compatible_case_index(const Type& type, const Type& current) {
    if (const int exact = case_index(type, current); exact >= 0) return exact;

    int match = -1;
    for (std::size_t i = 0; i < type.cases.size(); ++i) {
        if (!representation_erasure_compatible(current, type.cases[i])) continue;
        if (match >= 0) return -1;
        match = static_cast<int>(i);
    }
    return match;
}

inline bool assignable(const Type& from, const Type& to) {
    if (from.kind == TypeKind::Invalid || to.kind == TypeKind::Invalid || from == to ||
        from.kind == TypeKind::Never) {
        return true;
    }
    if (to.kind == TypeKind::Union) {
        if (from.kind == TypeKind::Union) {
            return std::all_of(
                from.cases.begin(), from.cases.end(),
                [&](const auto& current) { return compatible_case_index(to, current) >= 0; });
        }
        return compatible_case_index(to, from) >= 0;
    }
    if (from.kind == TypeKind::Array && to.kind == TypeKind::Array) {
        return (to.length < 0 || to.length == from.length) &&
               assignable(*from.first, *to.first);
    }
    if (from.kind == TypeKind::Tensor && to.kind == TypeKind::Tensor) {
        return *from.first == *to.first && tensor_satisfies_shape_prefix(from, to);
    }
    if (from.kind == TypeKind::Neural && to.kind == TypeKind::Neural) {
        return *from.first == *to.first && tensor_satisfies_shape_prefix(from, to);
    }
    return false;
}

} // namespace quidra
