#pragma once

// ConversionLowering: a value of one type to another (unions, numbers, array
// shapes), fail-fast consumption, explicit casts. Defined in
// conversion_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/error_lowering.hpp"
#include "lowering/ir_names.hpp"
#include "lowering/local_scope.hpp"
#include <optional>

namespace quidra::lowering {

class Lowerer;
class LifetimeLowering;

// Whether converting `from` to `to` builds a new array (the shape type
// changes).
bool array_shape_conversion(const Type& from, const Type& to);

class ConversionLowering {
public:
    ConversionLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, LocalScope& scope,
        LifetimeLowering& lifetime)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          scope_(scope), lifetime_(lifetime) {}

    // union, numeric and array shape conversion
    ValueId consume_fail_fast(ValueId container, const Type& source,
                              const Type& target, bool owned, SourceSpan span,
                              const ErrorHopCall* hop = nullptr);
    ValueId convert(ValueId v,const Type& from,const Type& to,bool copy=false);
    ValueId convert_numeric_argument(ValueId value, const Type& source,
                                     const Type& target, SourceSpan span);
    ValueId lower_numeric_cast(
        const Expr& e, const CallExpr& n, const CallResolution& resolution);

private:
    // an array to an array of another shape type, for convert
    ValueId convert_array_shape(ValueId source,const Type& from,const Type& to);

    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LocalScope& scope_;
    LifetimeLowering& lifetime_;
};

} // namespace quidra::lowering
