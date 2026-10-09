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

    require(builtin_scalar_type("int") == t(TypeKind::Int) &&
                builtin_scalar_type("int") != builtin_scalar_type("int64"),
            "int is the arbitrary-precision integer, distinct from int64");
    require(builtin_scalar_type("nat") == t(TypeKind::Nat), "nat is a builtin scalar type");
    for (const auto* removed : {"float", "float32", "float64", "uint8", "uint16", "uint32",
                                "uint64", "bigreal"}) {
        require(!builtin_scalar_type(removed), "a removed numeric spelling names no type");
    }
    require(type_name(t(TypeKind::Bin)) == "bin", "bin name");
    require(is_pointer_runtime_type(t(TypeKind::Bin)), "bin uses managed runtime storage");

    const auto tensor_plain = Type::tensor(t(TypeKind::Real32));
    const auto tensor_rank2 = Type::tensor(t(TypeKind::Real32), 2);
    const auto tensor_first3 = Type::tensor(t(TypeKind::Real32), 1, {3});
    const auto tensor_3x4 = Type::tensor(t(TypeKind::Real32), 2, {3, 4});
    const auto tensor_3_any = Type::tensor(t(TypeKind::Real32), 2, {3, -1});
    const auto inferred_3x4 = Type::tensor(t(TypeKind::Real32), 2, {}, {3, 4});
    require(type_name(tensor_plain) == "tensor<real32>", "plain tensor name");
    require(type_name(tensor_rank2) == "tensor<real32>",
            "internal rank must not appear in source type name");
    require(type_name(tensor_first3) == "tensor<real32><3>",
            "rank-1 tensor shape pattern name");
    require(type_name(tensor_3x4) == "tensor<real32><3, 4>",
            "exact tensor shape pattern name");
    require(type_name(tensor_3_any) == "tensor<real32><3, _>",
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
    require(!assignable(t(TypeKind::Nat8), t(TypeKind::Int16)),
            "typed signedness/width changes are not implicit");
    require(!assignable(t(TypeKind::Int16), t(TypeKind::Real32)),
            "typed integer-to-float conversion is not implicit");
    require(!assignable(t(TypeKind::Real32), t(TypeKind::Real64)),
            "typed float widening is not implicit");

    require(numeric_conversion_policy(t(TypeKind::Int64), t(TypeKind::Int8)) ==
                NumericConversionPolicy::ExplicitRangeCheck,
            "integer narrowing is range-checked explicit conversion");
    require(numeric_conversion_policy(t(TypeKind::Real64), t(TypeKind::Int64)) ==
                NumericConversionPolicy::Forbidden,
            "float -> int requires an explicit rounding operation");
    require(numeric_conversion_policy(t(TypeKind::Real64), t(TypeKind::Real32)) ==
                NumericConversionPolicy::ExplicitRangeCheck,
            "float64 -> float32 permits rounding but rejects finite range overflow");

    require(integer_value_fits(127, t(TypeKind::Int8)), "127 fits int8");
    require(!integer_value_fits(128, t(TypeKind::Int8)), "128 does not fit int8");
    require(integer_value_fits(255, t(TypeKind::Nat8)), "255 fits uint8");
    require(!integer_value_fits(-1, t(TypeKind::Nat8)), "-1 does not fit uint8");

    require(float_value_fits_exactly(1.5, t(TypeKind::Real32)), "1.5 is exact float32");
    require(!float_value_fits_exactly(0.1, t(TypeKind::Real32)), "0.1 is not exact float32");
    require(float_value_fits_range(0.1, t(TypeKind::Real32)),
            "0.1 may round when materialized as float32");
    require(!float_value_fits_range(1.0e100, t(TypeKind::Real32)),
            "finite values outside float32 range are rejected");
    require(float_value_fits_range(0.1, t(TypeKind::Real32)),
            "0.1 may round when materialized as float32");
    require(!float_value_fits_range(1.0e100, t(TypeKind::Real32)),
            "finite values outside float32 range are rejected");
    require(integer_value_fits_exactly_in_float(9007199254740992LL, t(TypeKind::Real64)),
            "2^53 is exactly representable by float64");
    require(!integer_value_fits_exactly_in_float(9007199254740993LL, t(TypeKind::Real64)),
            "2^53+1 is not exactly representable by float64");
    require(!integer_value_fits_exactly_in_float(std::numeric_limits<long long>::max(), t(TypeKind::Real64)),
            "int64 max is not exactly representable by float64");
    require(integer_value_fits_exactly_in_float(std::numeric_limits<long long>::min(), t(TypeKind::Real64)),
            "int64 min is exactly representable by float64");

    require(builtin_scalar_type("real") == t(TypeKind::Real),
            "real is a builtin scalar type");
    require(type_name(t(TypeKind::Int)) == "int", "int name");
    require(type_name(t(TypeKind::Nat)) == "nat", "nat name");
    require(type_name(t(TypeKind::Real)) == "real", "real name");
    require(is_integer_family_type(t(TypeKind::Int)) && is_integer_family_type(t(TypeKind::Nat)),
            "int and nat belong to the integer family");
    require(is_bare_integer(t(TypeKind::Int)) && is_bare_integer(t(TypeKind::Nat)) &&
                !is_fixed_integer(t(TypeKind::Int)) && !is_fixed_integer(t(TypeKind::Nat)),
            "int and nat are bare integers, not fixed-width");
    require(is_real(t(TypeKind::Real)),
            "bigreal belongs to the real family");
    require(!is_tensor_numeric(t(TypeKind::Int)) && !is_tensor_numeric(t(TypeKind::Nat)) &&
            !is_tensor_numeric(t(TypeKind::Real)),
            "exact scalars are excluded from tensor native dtypes");
    require(is_pointer_runtime_type(t(TypeKind::Int)) &&
            is_pointer_runtime_type(t(TypeKind::Real)),
            "exact scalars use managed runtime storage");
    require(uses_shared_immutable_storage(t(TypeKind::Int)) &&
            uses_shared_immutable_storage(t(TypeKind::Real)),
            "exact scalars use immutable shared value storage");
    require(runtime_storage_bytes(t(TypeKind::Int)) == 8 &&
                runtime_storage_bytes(t(TypeKind::Nat)) == 8,
            "int and nat runtime storage is one word");
    require(runtime_storage_bytes(t(TypeKind::Real)) == 16,
            "real runtime storage is two words, a small rational or a managed node");
    require(numeric_conversion_policy(t(TypeKind::Int64), t(TypeKind::Int)) ==
                NumericConversionPolicy::ExplicitDeterministic,
            "fixed integer -> int is explicit and deterministic");
    require(numeric_conversion_policy(t(TypeKind::Int), t(TypeKind::Nat)) ==
                    NumericConversionPolicy::ExplicitRangeCheck &&
                numeric_conversion_policy(t(TypeKind::Int64), t(TypeKind::Nat)) ==
                    NumericConversionPolicy::ExplicitRangeCheck,
            "a signed integer -> nat is checked");
    require(numeric_conversion_policy(t(TypeKind::Nat), t(TypeKind::Int)) ==
                    NumericConversionPolicy::ExplicitDeterministic &&
                numeric_conversion_policy(t(TypeKind::Nat64), t(TypeKind::Nat)) ==
                    NumericConversionPolicy::ExplicitDeterministic,
            "nat -> int and an unsigned integer -> nat are exact");
    require(numeric_conversion_policy(t(TypeKind::Nat), t(TypeKind::Nat32)) ==
                NumericConversionPolicy::ExplicitRangeCheck,
            "nat -> a fixed-width integer is range-checked");
    require(numeric_conversion_policy(t(TypeKind::Int), t(TypeKind::Real)) ==
                NumericConversionPolicy::ExplicitDeterministic,
            "int -> real is exact explicit conversion");
    require(numeric_conversion_policy(t(TypeKind::Real64), t(TypeKind::Real)) ==
                NumericConversionPolicy::ExplicitDeterministic,
            "float -> bigreal preserves the exact IEEE value");
    require(numeric_conversion_policy(t(TypeKind::Real), t(TypeKind::Int)) ==
                NumericConversionPolicy::ExplicitRangeCheck,
            "real -> int requires runtime integer proof");
    require(numeric_conversion_policy(t(TypeKind::Real64), t(TypeKind::Int)) ==
                NumericConversionPolicy::Forbidden,
            "IEEE float -> int requires an explicit rounding operation");
    require(!assignable(t(TypeKind::Int), t(TypeKind::Real)),
            "int -> real is never implicit");

    return 0;
}
