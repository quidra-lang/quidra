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
    require(type_name(t(TypeKind::Bytes)) == "bytes", "bytes name");
    require(type_name(Type::neural()) == "neural", "neural defaults to float32");
    require(type_name(Type::neural(t(TypeKind::Float))) == "neural<float>", "explicit neural float64 name");
    require(is_pointer_runtime_type(t(TypeKind::Bytes)), "bytes uses managed runtime storage");

    const auto tensor_unknown = Type::tensor(t(TypeKind::Float32));
    const auto tensor_rank2 = Type::tensor(t(TypeKind::Float32), 2);
    const auto tensor_rank3 = Type::tensor(t(TypeKind::Float32), 3);
    require(type_name(tensor_unknown) == "tensor<float32>", "unknown-rank tensor name");
    require(type_name(tensor_rank2) == "tensor<float32, 2>", "static-rank tensor name");
    require(assignable(tensor_rank2, tensor_unknown), "known tensor rank may erase to unknown rank");
    require(!assignable(tensor_unknown, tensor_rank2), "unknown tensor rank cannot assert a known rank");
    require(!assignable(tensor_rank2, tensor_rank3), "different known tensor ranks are incompatible");
    require(runtime_storage_bytes(tensor_rank2) == runtime_storage_bytes(tensor_unknown),
            "tensor rank metadata must not change runtime ABI size");

    require(lossless_implicit_numeric_conversion(t(TypeKind::Int8), t(TypeKind::Int16)),
            "int8 -> int16");
    require(lossless_implicit_numeric_conversion(t(TypeKind::UInt8), t(TypeKind::Int16)),
            "uint8 -> int16");
    require(lossless_implicit_numeric_conversion(t(TypeKind::UInt32), t(TypeKind::Int)),
            "uint32 -> int64");
    require(!lossless_implicit_numeric_conversion(t(TypeKind::Int8), t(TypeKind::UInt8)),
            "signed -> unsigned is not universally safe");
    require(!lossless_implicit_numeric_conversion(t(TypeKind::UInt64), t(TypeKind::Int)),
            "uint64 -> int64 is not universally safe");

    require(lossless_implicit_numeric_conversion(t(TypeKind::Int16), t(TypeKind::Float32)),
            "all int16 values are exactly representable by float32");
    require(!lossless_implicit_numeric_conversion(t(TypeKind::Int32), t(TypeKind::Float32)),
            "all int32 values are not exactly representable by float32");
    require(lossless_implicit_numeric_conversion(t(TypeKind::UInt32), t(TypeKind::Float)),
            "all uint32 values are exactly representable by float64");
    require(!lossless_implicit_numeric_conversion(t(TypeKind::Int), t(TypeKind::Float)),
            "all int64 values are not exactly representable by float64");
    require(lossless_implicit_numeric_conversion(t(TypeKind::Float32), t(TypeKind::Float)),
            "float32 -> float64");

    require(numeric_conversion_policy(t(TypeKind::Int), t(TypeKind::Int8)) ==
                NumericConversionPolicy::ExplicitRangeCheck,
            "integer narrowing is range-checked explicit conversion");
    require(numeric_conversion_policy(t(TypeKind::Float), t(TypeKind::Int)) ==
                NumericConversionPolicy::Forbidden,
            "float -> int requires an explicit rounding operation");
    require(numeric_conversion_policy(t(TypeKind::Float), t(TypeKind::Float32)) ==
                NumericConversionPolicy::ExplicitDeterministic,
            "float64 -> float32 is a deterministic explicit conversion");

    require(integer_value_fits(127, t(TypeKind::Int8)), "127 fits int8");
    require(!integer_value_fits(128, t(TypeKind::Int8)), "128 does not fit int8");
    require(integer_value_fits(255, t(TypeKind::UInt8)), "255 fits uint8");
    require(!integer_value_fits(-1, t(TypeKind::UInt8)), "-1 does not fit uint8");

    require(float_value_fits_exactly(1.5, t(TypeKind::Float32)), "1.5 is exact float32");
    require(!float_value_fits_exactly(0.1, t(TypeKind::Float32)), "0.1 is not exact float32");
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
