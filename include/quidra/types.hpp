#pragma once
#include "quidra/abi/layout.hpp"
#include "quidra/language.hpp"
#include "quidra/numeric_types.hpp"
#include "quidra/standard_classes.hpp"
#include "quidra/type_kind.hpp"
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

struct Type {
    TypeKind kind{TypeKind::Void};
    std::shared_ptr<Type> first;
    std::vector<Type> cases;
    std::vector<Type> parameters;
    // Arrays use length as their static length. Tensor values use it
    // for compiler-known rank; a source shape pattern fixes rank exactly.
    long long length{-1};
    std::vector<long long> tensor_shape_prefix;
    std::vector<long long> tensor_known_shape_prefix;
    std::string class_name;
    std::string union_name;
    std::vector<std::string> case_names;

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

    static Type function(Type result, std::vector<Type> parameters = {}) {
        auto type = simple(TypeKind::Function);
        type.first = std::make_shared<Type>(std::move(result));
        type.parameters = std::move(parameters);
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

    static Type union_of(std::vector<Type>);

    static Type enum_type(std::string name, std::vector<std::string> names,
                          std::vector<Type> payloads) {
        auto type = simple(TypeKind::Union);
        type.union_name = std::move(name);
        type.case_names = std::move(names);
        type.cases = std::move(payloads);
        return type;
    }

    bool operator==(const Type& other) const {
        if (kind != other.kind || class_name != other.class_name ||
            union_name != other.union_name || case_names != other.case_names ||
            cases != other.cases || parameters != other.parameters ||
            bool(first) != bool(other.first) ||
            (first && *first != *other.first)) {
            return false;
        }
        if (kind == TypeKind::Array) return length == other.length;
        if (kind == TypeKind::Tensor) {
            // Inferred rank/shape facts are flow facts. Only an explicit source
            // shape pattern participates in static type identity.
            return tensor_shape_prefix == other.tensor_shape_prefix;
        }
        return true;
    }

    bool operator!=(const Type& other) const { return !(*this == other); }
};

inline std::string type_name(const Type& type) {
    if (const auto* numeric = numeric_kind_info(type.kind)) return std::string(numeric->spelling);
    switch (type.kind) {
        case TypeKind::Bool: return "bool";
        case TypeKind::String: return "string";
        case TypeKind::Bin: return "bin";
        case TypeKind::Void: return "void";
        case TypeKind::Never: return "never";
        case TypeKind::Error: return "error";
        case TypeKind::None: return "none";
        case TypeKind::Auto: return "auto";
        case TypeKind::Range: return "range";
        case TypeKind::Invalid: return "<invalid>";
        case TypeKind::Class: return type.class_name;
        case TypeKind::Address: return "address";
        case TypeKind::Function: {
            std::string result = "fn<" + type_name(*type.first) + ">(";
            for (std::size_t i = 0; i < type.parameters.size(); ++i) {
                if (i) result += ", ";
                result += type_name(type.parameters[i]);
            }
            result += ")";
            return result;
        }
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
            if (!type.union_name.empty()) return type.union_name;
            std::string result;
            for (const auto& current : type.cases) {
                if (!result.empty()) result += " | ";
                result += type_name(current);
            }
            return result;
        }
        default:
            break;
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
           type.kind != TypeKind::Range && type.kind != TypeKind::Address;
}

// The table row of a numeric type, or nullptr (quidra/numeric_types.hpp).
inline const NumericKindInfo* numeric_info(const Type& type) {
    return numeric_kind_info(type.kind);
}

inline bool is_fixed_integer(const Type& type) {
    const auto* numeric = numeric_info(type);
    return numeric && numeric->width != 0 &&
           (numeric->family == NumericFamily::Integer || numeric->family == NumericFamily::Natural);
}

inline bool is_signed_integer(const Type& type) {
    const auto* numeric = numeric_info(type);
    return numeric && numeric->width != 0 && numeric->family == NumericFamily::Integer;
}

// int and nat: arbitrary-precision integers, held as words
// (abi::bare_integer_layout).
inline bool is_bare_integer(const Type& type) {
    const auto* numeric = numeric_info(type);
    return numeric && numeric->width == 0 &&
           (numeric->family == NumericFamily::Integer || numeric->family == NumericFamily::Natural);
}

inline bool is_integer_family_type(const Type& type) {
    const auto* numeric = numeric_info(type);
    return numeric &&
           (numeric->family == NumericFamily::Integer || numeric->family == NumericFamily::Natural);
}

inline bool is_fixed_real(const Type& type) {
    const auto* numeric = numeric_info(type);
    return numeric && numeric->width != 0 && numeric->family == NumericFamily::Real;
}

inline bool is_exact_real(const Type& type) {
    const auto* numeric = numeric_info(type);
    return numeric && numeric->width == 0 && numeric->family == NumericFamily::Real;
}

inline bool is_real(const Type& type) {
    const auto* numeric = numeric_info(type);
    return numeric && numeric->family == NumericFamily::Real;
}

inline bool is_numeric(const Type& type) {
    return numeric_info(type) != nullptr;
}

// A numeric kind without a fixed width: its values are boxed and shared.
inline bool is_arbitrary_precision(const Type& type) {
    const auto* numeric = numeric_info(type);
    return numeric && numeric->width == 0;
}

inline bool is_tensor_numeric(const Type& type) {
    const auto* numeric = numeric_info(type);
    return numeric && numeric->tensor_element;
}

inline int integer_width(const Type& type) {
    return is_fixed_integer(type) ? static_cast<int>(numeric_info(type)->width) : 0;
}

inline int float_precision_bits(const Type& type) {
    const auto* numeric = numeric_info(type);
    return numeric ? ieee_precision_bits(numeric->format) : 0;
}

// The smallest and largest value of a fixed-width integer type of `width`
// bits, as the two's complement or unsigned range of that width.
inline long long signed_integer_min(int width) {
    return width >= 64 ? std::numeric_limits<long long>::min() : -(1LL << (width - 1));
}

inline long long signed_integer_max(int width) {
    return width >= 64 ? std::numeric_limits<long long>::max() : (1LL << (width - 1)) - 1;
}

inline unsigned long long unsigned_integer_max(int width) {
    return width >= 64 ? std::numeric_limits<unsigned long long>::max() : (1ULL << width) - 1ULL;
}

inline bool integer_value_fits(long long value, const Type& type) {
    if (!is_fixed_integer(type)) return false;
    const auto width = integer_width(type);
    if (is_signed_integer(type)) {
        return value >= signed_integer_min(width) && value <= signed_integer_max(width);
    }
    return value >= 0 && static_cast<unsigned long long>(value) <= unsigned_integer_max(width);
}

inline bool integer_literal_value_fits(unsigned long long value, const Type& type) {
    if (!is_fixed_integer(type)) return false;
    const auto width = integer_width(type);
    if (is_signed_integer(type)) {
        return value <= static_cast<unsigned long long>(signed_integer_max(width));
    }
    return value <= unsigned_integer_max(width);
}

inline bool negative_integer_literal_value_fits(unsigned long long magnitude, const Type& type) {
    if (!is_signed_integer(type)) return false;
    const auto width = integer_width(type);
    if (width <= 0) return false;
    const auto limit = 1ULL << (width - 1);
    return magnitude <= limit;
}

inline bool float_value_fits_exactly(double value, const Type& type) {
    const auto* numeric = numeric_info(type);
    if (!numeric) return false;
    if (numeric->format == IeeeFormat::Binary64) return true;
    if (numeric->format != IeeeFormat::Binary32) return false;
    const auto narrowed = static_cast<float>(value);
    return static_cast<double>(narrowed) == value;
}

inline bool float_value_fits_range(double value, const Type& type) {
    if (!is_fixed_real(type) || !std::isfinite(value)) return false;
    const auto format = numeric_info(type)->format;
    if (format == IeeeFormat::Binary64) return true;
    if (format != IeeeFormat::Binary32) return false;
    return std::isfinite(static_cast<float>(value));
}

inline bool float_value_fits_exactly_in_integer(double value, const Type& type) {
    if (!is_fixed_integer(type) || !std::isfinite(value) || std::trunc(value) != value) return false;
    const long double exact = static_cast<long double>(value);
    const auto width = integer_width(type);
    if (is_signed_integer(type)) {
        return exact >= static_cast<long double>(signed_integer_min(width)) &&
               exact <= static_cast<long double>(signed_integer_max(width));
    }
    return exact >= 0.0L && exact <= static_cast<long double>(unsigned_integer_max(width));
}

inline bool integer_value_fits_exactly_in_float(long long value, const Type& type) {
    if (!is_fixed_real(type)) return false;
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
    if (!is_fixed_real(type)) return false;
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
    if (const auto* numeric = numeric_info(type); numeric && numeric->width != 0) {
        return static_cast<int>(numeric->width);
    }
    if (type.kind == TypeKind::Bool) return 1;
    return 0;
}

inline bool integer_range_contained(const Type& from, const Type& to) {
    if (!is_fixed_integer(from) || !is_fixed_integer(to)) return false;

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
    if (!is_fixed_integer(from) || !is_fixed_real(to)) return false;

    const auto required_bits =
        integer_width(from) - (is_signed_integer(from) ? 1 : 0);
    return float_precision_bits(to) >= required_bits;
}


inline bool explicit_numeric_cast_supported(const Type& from, const Type& to) {
    if (!is_numeric(from) || !is_numeric(to)) return false;
    // IEEE real -> integer is a rounding operation, not a representation
    // conversion. Exact bigreal -> integer is allowed only when runtime proof
    // establishes that the mathematical value is integral.
    if (is_fixed_real(from) && is_integer_family_type(to)) return false;
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
    if (is_fixed_integer(from) && is_fixed_integer(to)) {
        return integer_range_contained(from, to)
            ? NumericConversionPolicy::ExplicitDeterministic
            : NumericConversionPolicy::ExplicitRangeCheck;
    }
    // nat holds no negative value: a signed source is checked.
    if (to.kind == TypeKind::Nat && (from.kind == TypeKind::Int || is_signed_integer(from))) {
        return NumericConversionPolicy::ExplicitRangeCheck;
    }
    if ((is_bare_integer(from) && is_fixed_integer(to)) ||
        (is_exact_real(from) && is_integer_family_type(to)) ||
        ((is_bare_integer(from) || is_exact_real(from)) && is_fixed_real(to))) {
        return NumericConversionPolicy::ExplicitRangeCheck;
    }
    if (is_fixed_real(from) && is_fixed_real(to) &&
        numeric_info(to)->width < numeric_info(from)->width) {
        return NumericConversionPolicy::ExplicitRangeCheck;
    }
    return NumericConversionPolicy::ExplicitDeterministic;
}

inline std::optional<Type> builtin_scalar_type(std::string_view name) {
    if (const auto canonical = canonical_builtin_type_name(name)) name = *canonical;
    if (const auto* numeric = numeric_kind_info(name)) return Type::simple(numeric->kind);
    if (name == "bool") return Type::simple(TypeKind::Bool);
    if (name == "string") return Type::simple(TypeKind::String);
    if (name == "bin") return Type::simple(TypeKind::Bin);
    return std::nullopt;
}

inline bool is_pointer_runtime_type(const Type& type) {
    return is_arbitrary_precision(type) ||
           type.kind == TypeKind::String || type.kind == TypeKind::Bin ||
           type.kind == TypeKind::Error || type.kind == TypeKind::Array ||
           type.kind == TypeKind::Tensor || type.kind == TypeKind::Union ||
           type.kind == TypeKind::Class;
}

enum class ValueStoragePolicy {
    Direct,
    ImmutableShared,
    IndependentStorage
};

inline ValueStoragePolicy value_storage_policy(const Type& type) {
    if (is_arbitrary_precision(type) ||
        type.kind == TypeKind::String || type.kind == TypeKind::Error ||
        (type.kind == TypeKind::Class &&
         type.class_name == standard_class::json_value)) {
        return ValueStoragePolicy::ImmutableShared;
    }
    if (type.kind == TypeKind::Array || type.kind == TypeKind::Tensor ||
        type.kind == TypeKind::Bin || type.kind == TypeKind::Class ||
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
    if (const auto* numeric = numeric_info(type); numeric && numeric->width != 0) {
        return static_cast<std::size_t>(numeric->width / 8);
    }
    if (type.kind == TypeKind::Real) return abi::exact_real_layout::bytes;
    if (type.kind == TypeKind::Bool) return 1;
    if (type.kind == TypeKind::Function) return 8;
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
    // Unknown rank/extent is not a contradiction: constrained bindings and calls
    // validate it at runtime. Known conflicts are still rejected immediately.
    if (from.length >= 0 && from.length != required_rank) return false;
    for (std::size_t axis = 0; axis < to.tensor_shape_prefix.size(); ++axis) {
        const auto required = to.tensor_shape_prefix[axis];
        if (required < 0) continue;
        const auto extent = tensor_known_extent(from, axis);
        if (extent && *extent != required) return false;
    }
    return true;
}

inline bool representation_erasure_compatible(const Type& from, const Type& to) {
    if (from == to) return true;
    if (from.kind == TypeKind::Tensor && to.kind == TypeKind::Tensor) {
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
    if ((from.kind == TypeKind::Union && !from.union_name.empty()) ||
        (to.kind == TypeKind::Union && !to.union_name.empty())) {
        return false;
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
    return false;
}

} // namespace quidra
