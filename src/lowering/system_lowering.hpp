#pragma once

// SystemLowering: the command line, the environment, the clock and
// processes. Defined in system_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/ir_names.hpp"

namespace quidra::lowering {

class Lowerer;
class LifetimeLowering;

class SystemLowering {
public:
    SystemLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, LifetimeLowering& lifetime)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          lifetime_(lifetime) {}

    // command line, environment, clock, processes
    ValueId lower_cli_argument(const Expr& e, const CallExpr& n);
    ValueId lower_cli_argument_optional(const Expr& e, const CallExpr& n);
    ValueId lower_cli_option(const Expr& e, const CallExpr& n);
    ValueId lower_cli_flag(const CallExpr& n);
    ValueId lower_cli_finish();
    ValueId lower_environment_get(const Expr& e, const CallExpr& n);
    ValueId lower_environment_has(const CallExpr& n);
    ValueId lower_time_now(const CallExpr& n);
    ValueId lower_time_since(const CallExpr& n);
    ValueId lower_time_seconds(const CallExpr& n);
    ValueId lower_time_sleep(const Expr& e, const CallExpr& n);
    ValueId lower_process_run(const CallExpr& n);
    ValueId lower_process_shell(const CallExpr& n);

private:
    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LifetimeLowering& lifetime_;
};

} // namespace quidra::lowering
