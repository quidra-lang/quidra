#pragma once

// BinLowering: the bin type's own methods. Defined in bin_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/ir_names.hpp"
#include <optional>

namespace quidra::lowering {

class Lowerer;

class BinLowering {
public:
    BinLowering(Lowerer& lowerer, ir::FunctionBuilder& builder)
        : lowerer_(lowerer), builder_(builder) {}

    // the bin type
    std::optional<ValueId> try_bin_type_method(const MethodCallExpr& n);

private:
    Lowerer& lowerer_;
    ir::FunctionBuilder& builder_;
};

} // namespace quidra::lowering
