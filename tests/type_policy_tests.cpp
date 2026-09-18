#include "quidra/types.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "type policy failure: " << message << "\n";
    std::exit(1);
}

quidra::Type t(quidra::TypeKind kind) {
    return quidra::Type::simple(kind);
}

} // namespace

int main() {
    using namespace quidra;

    require(builtin_scalar_type("int") == builtin_scalar_type("int64"),
            "int must alias int64");
    require(builtin_scalar_type("float") == builtin_scalar_type("float64"),
            "float must alias float64");
    require(type_name(t(TypeKind::Bin)) == "bin", "bin name");
    require(type_name(Type::neural()) == "neural", "neural defaults to float32");
    require(type_name(Type::neural(t(TypeKind::Float))) == "neural<float>", "explicit neural float64 name");
    require(is_pointer_runtime_type(t(TypeKind::Bin)), "bin uses managed runtime storage");

    const auto tensor_plain = Type::tensor(t(TypeKind::Float32));
    const auto tensor_rank2 = Type::tensor(t(TypeKind::Float32), 2);
    const auto tensor_first3 = Type::tensor(t(TypeKind::Float32), 1, {3});
    const auto tensor_3x4 = Type::tensor(t(TypeKind::Float32), 2, {3, 4});
    const auto tensor_3_any = Type::tensor(t(TypeKind::Float32), 2, {3, -1});
    const auto inferred_3x4 = Type::tensor(t(TypeKind::Float32), 2, {}, {3, 4});
    require(type_name(tensor_plain) == "tensor<float32>", "plain tensor name");
    require(type_name(tensor_rank2) == "tensor<float32>",
            "internal rank must not appear in source type name");
    require(type_name(tensor_first3) == "tensor<float32><3>",
            "rank-1 tensor shape pattern name");
    require(type_name(tensor_3x4) == "tensor<float32><3, 4>",
            "exact tensor shape pattern name");
    require(type_name(tensor_3_any) == "tensor<float32><3, _>",
            "tensor wildcard shape pattern name");
    require(!assignable(inferred_3x4, tensor_first3),
            "shape pattern rank is exact");
    require(assignable(inferred_3x4, tensor_3_any),
            "wildcard extent accepts an inferred matching rank");
    require(assignable(inferred_3x4, tensor_3x4),
            "inferred shape may satisfy an exact source pattern");
    require(assignable(tensor_plain, tensor_first3),
            "unknown tensor shape may defer a constrained axis check to runtime");
    require(runtime_storage_bytes(tensor_first3) == runtime_storage_bytes(tensor_plain),
            "tensor shape patterns must not change runtime ABI size");
    require(type_name(Type::neural(t(TypeKind::Float32), 3, {3, -1, -1})) ==
                "neural<3, _, _>",
            "default-float neural shape shorthand");
    require(type_name(Type::neural(t(TypeKind::Float), 2, {-1, 768})) ==
                "neural<float><_, 768>",
            "explicit neural dtype plus shape pattern");

    const auto tensor_or_error = Type::union_of({tensor_plain, t(TypeKind::Error)});
    const auto constrained_or_error =
        Type::union_of({tensor_first3, t(TypeKind::Error)});
    require(compatible_case_index(tensor_or_error, tensor_first3) >= 0,
            "constrained tensor maps to unconstrained union case");
    require(assignable(tensor_first3, tensor_or_error),
            "constrained tensor may enter an unconstrained union");
    require(assignable(constrained_or_error, tensor_or_error),
            "constrained tensor union may widen to unconstrained tensor union");

    require(assignable(t(TypeKind::Int8), t(TypeKind::Int8)),
            "identity numeric representation is assignable");
    require(!assignable(t(TypeKind::Int8), t(TypeKind::Int16)),
            "typed integer widening is not implicit");
    require(!assignable(t(TypeKind::UInt8), t(TypeKind::Int16)),
            "typed signedness/width changes are not implicit");
    require(!assignable(t(TypeKind::Int16), t(TypeKind::Float32)),
            "typed integer-to-float conversion is not implicit");
    require(!assignable(t(TypeKind::Float32), t(TypeKind::Float)),
            "typed float widening is not implicit");

    require(numeric_conversion_policy(t(TypeKind::Int), t(TypeKind::Int8)) ==
                NumericConversionPolicy::ExplicitRangeCheck,
            "integer narrowing is range-checked explicit conversion");
    require(numeric_conversion_policy(t(TypeKind::Float), t(TypeKind::Int)) ==
                NumericConversionPolicy::Forbidden,
            "float -> int requires an explicit rounding operation");
    require(numeric_conversion_policy(t(TypeKind::Float), t(TypeKind::Float32)) ==
                NumericConversionPolicy::ExplicitRangeCheck,
            "float64 -> float32 permits rounding but rejects finite range overflow");

    require(integer_value_fits(127, t(TypeKind::Int8)), "127 fits int8");
    require(!integer_value_fits(128, t(TypeKind::Int8)), "128 does not fit int8");
    require(integer_value_fits(255, t(TypeKind::UInt8)), "255 fits uint8");
    require(!integer_value_fits(-1, t(TypeKind::UInt8)), "-1 does not fit uint8");

    require(float_value_fits_exactly(1.5, t(TypeKind::Float32)), "1.5 is exact float32");
    require(!float_value_fits_exactly(0.1, t(TypeKind::Float32)), "0.1 is not exact float32");
    require(float_value_fits_range(0.1, t(TypeKind::Float32)),
            "0.1 may round when materialized as float32");
    require(!float_value_fits_range(1.0e100, t(TypeKind::Float32)),
            "finite values outside float32 range are rejected");
    require(integer_value_fits_exactly_in_float(9007199254740992LL, t(TypeKind::Float)),
            "2^53 is exactly representable by float64");
    require(!integer_value_fits_exactly_in_float(9007199254740993LL, t(TypeKind::Float)),
            "2^53+1 is not exactly representable by float64");
    require(!integer_value_fits_exactly_in_float(std::numeric_limits<long long>::max(), t(TypeKind::Float)),
            "int64 max is not exactly representable by float64");
    require(integer_value_fits_exactly_in_float(std::numeric_limits<long long>::min(), t(TypeKind::Float)),
            "int64 min is exactly representable by float64");

    return 0;
}
