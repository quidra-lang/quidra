#pragma once

// StatementLowering: bodies and statements: the statement recursion roots
// and one function per kind of statement. Defined in statement_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/check_elision_facts.hpp"
#include "lowering/enclosing_class.hpp"
#include "lowering/ir_names.hpp"
#include "lowering/local_scope.hpp"
#include "lowering/loop_targets.hpp"
#include "lowering/nesting_depth.hpp"
#include "lowering/repl_replay_state.hpp"
#include "lowering/shape_constraint_state.hpp"
#include "lowering/source_locator.hpp"
#include <cstddef>
#include <functional>
#include <vector>

namespace quidra::lowering {

class Lowerer;
class LifetimeLowering;
class ConversionLowering;
class ShapeConstraintLowering;
class CallLowering;
class AssignmentLowering;
class ControlFlowLowering;
class TextIdioms;
class FunctionLowering;

class StatementLowering {
public:
    StatementLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, LocalScope& scope,
        CheckElisionFacts& facts, NestingDepth& nesting_depth,
        const LoopTargets& loop_targets, const EnclosingClass& enclosing_class,
        const ReplReplayState& replay, ShapeConstraintState& shapes,
        const SourceLocator& locator, LifetimeLowering& lifetime,
        ConversionLowering& conversions,
        ShapeConstraintLowering& shape_constraints, CallLowering& calls,
        AssignmentLowering& assignments, ControlFlowLowering& control_flow,
        TextIdioms& text_idioms, FunctionLowering& functions)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          scope_(scope), facts_(facts), nesting_depth_(nesting_depth),
          loop_targets_(loop_targets), enclosing_class_(enclosing_class),
          replay_(replay), shapes_(shapes), locator_(locator),
          lifetime_(lifetime), conversions_(conversions),
          shape_constraints_(shape_constraints), calls_(calls),
          assignments_(assignments), control_flow_(control_flow),
          text_idioms_(text_idioms), functions_(functions) {}

    // the statement recursion roots: a body and a statement. A body may name
    // code to emit before each of its statements, by index; a text sequence
    // that starts at a statement is lowered after that statement's code.
    void lower_block(
        const std::vector<StmtPtr>& statements,
        const std::function<void(std::size_t)>& before_statement = {});
    void lower_statement(const Stmt& s);

private:
    // a statement without its SourceLocation and the initialization flags it
    // sets after it (lower_statement)
    void lower_statement_kind(const Stmt& s);
    // one function per kind of statement, which lower_statement
    // dispatches to
    void lower_binding(const Stmt& s, const BindingStmt& n);
    void lower_rebind(const RebindStmt& n);
    void lower_loop_control(const LoopControlStmt& n);
    void lower_return(const Stmt& s, const ReturnStmt& n);
    void lower_expression_statement(const Stmt& s, const ExprStmt& n);
    void lower_main_guard(const MainGuardStmt& n);

    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LocalScope& scope_;
    CheckElisionFacts& facts_;
    NestingDepth& nesting_depth_;
    const LoopTargets& loop_targets_;
    const EnclosingClass& enclosing_class_;
    const ReplReplayState& replay_;
    ShapeConstraintState& shapes_;
    const SourceLocator& locator_;
    LifetimeLowering& lifetime_;
    ConversionLowering& conversions_;
    ShapeConstraintLowering& shape_constraints_;
    CallLowering& calls_;
    AssignmentLowering& assignments_;
    ControlFlowLowering& control_flow_;
    TextIdioms& text_idioms_;
    FunctionLowering& functions_;
};

} // namespace quidra::lowering
