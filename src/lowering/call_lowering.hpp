#pragma once

// CallLowering: arguments, class values, calls by resolution and method
// calls by receiver. Defined in call_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/borrow_inference.hpp"
#include "lowering/enclosing_class.hpp"
#include "lowering/ir_names.hpp"
#include "lowering/local_scope.hpp"
#include "semantics/argument_isolation.hpp"
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace quidra::lowering {

class Lowerer;
class LifetimeLowering;
class ConversionLowering;
class BuiltinLowering;
class TextLowering;
class BinLowering;
class AggregateLowering;
class TensorLowering;
class AutogradLowering;
class FileLowering;
class ConcurrencyLowering;

class CallLowering {
public:
    CallLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, const LocalScope& scope,
        const EnclosingClass& enclosing_class, const BorrowInference& borrows,
        const semantics::ArgumentIsolation& isolation, LifetimeLowering& lifetime, ConversionLowering& conversions,
        BuiltinLowering& builtins, TextLowering& text, BinLowering& bin,
        AggregateLowering& aggregates, TensorLowering& tensors,
        AutogradLowering& autograd, FileLowering& files,
        ConcurrencyLowering& concurrency)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          scope_(scope), enclosing_class_(enclosing_class), borrows_(borrows),
          isolation_(isolation), lifetime_(lifetime), conversions_(conversions), builtins_(builtins),
          text_(text), bin_(bin), aggregates_(aggregates), tensors_(tensors),
          autograd_(autograd), files_(files), concurrency_(concurrency) {}

    // class values
    ValueId make_class_with_defaults(const std::string& class_name);

    // a call, and a method call
    ValueId lower_call(const Expr& e, const CallExpr& n);
    ValueId lower_method_call(const Expr& e, const MethodCallExpr& n);

private:
    // Call arguments in parameter order, and the borrowed temporaries to
    // release after the call.
    struct LoweredCallArguments {
        std::vector<CallArgument> args;
        std::vector<std::pair<ValueId, Type>> borrowed_temporaries;
    };

    // arguments
    LoweredCallArguments lower_call_arguments(const std::vector<CallArg>& source_args,
                                               const FunctionType& sig,
                                               std::size_t offset,
                                               const std::string& callee);
    void release_borrowed_temporaries(const LoweredCallArguments& lowered);

    // calls, by resolution, which lower_call dispatches to
    ValueId lower_function_value_call(
        const Expr& e, const CallExpr& n, const CallResolution& resolution);
    ValueId lower_enclosing_method_call(
        const Expr& e, const CallExpr& n, const CallResolution& resolution);
    ValueId lower_constructor_call(
        const Expr& e, const CallExpr& n, const CallResolution& resolution);
    ValueId lower_function_call(
        const Expr& e, const CallExpr& n, const CallResolution& resolution);

    // a user method, the last receiver lower_method_call tries
    ValueId lower_user_method_call(const Expr& e, const MethodCallExpr& n);

    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    const LocalScope& scope_;
    const EnclosingClass& enclosing_class_;
    const BorrowInference& borrows_;
    const semantics::ArgumentIsolation& isolation_;
    LifetimeLowering& lifetime_;
    ConversionLowering& conversions_;
    BuiltinLowering& builtins_;
    TextLowering& text_;
    BinLowering& bin_;
    AggregateLowering& aggregates_;
    TensorLowering& tensors_;
    AutogradLowering& autograd_;
    FileLowering& files_;
    ConcurrencyLowering& concurrency_;
};

} // namespace quidra::lowering
