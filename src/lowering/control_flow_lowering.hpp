#pragma once

// ControlFlowLowering: if, while, for and match statements, and exit().
// Defined in control_flow_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/borrow_inference.hpp"
#include "lowering/check_elision_facts.hpp"
#include "lowering/ir_names.hpp"
#include "lowering/local_scope.hpp"
#include "lowering/lifetime_lowering.hpp"
#include "lowering/loop_targets.hpp"
#include "lowering/storage_writes.hpp"
#include <string>
#include <vector>

namespace quidra::lowering {

class Lowerer;
class ConversionLowering;
class TextIdioms;
class CollectionIdioms;

class ControlFlowLowering {
public:
    ControlFlowLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, LocalScope& scope,
        CheckElisionFacts& facts, LoopTargets& loop_targets,
        const BorrowInference& borrows, const StorageWrites& storage_writes,
        LifetimeLowering& lifetime, ConversionLowering& conversions,
        TextIdioms& text_idioms, CollectionIdioms& collection_idioms)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          scope_(scope), facts_(facts), loop_targets_(loop_targets),
          borrows_(borrows), storage_writes_(storage_writes),
          lifetime_(lifetime), conversions_(conversions),
          text_idioms_(text_idioms), collection_idioms_(collection_idioms) {}

    // if, while, for and match; exit
    void lower_if(const IfStmt& n);
    void lower_while(const WhileStmt& n);
    void lower_for(const ForStmt& n);
    void lower_match(const MatchStmt& n);
    ValueId lower_exit(const CallExpr& n);

private:
    // the kinds of for loop, which lower_for dispatches to
    void lower_for_loop(const ForStmt& n);
    void lower_range_for(const ForStmt& n, const CallExpr& call);
    void lower_sequence_for(const ForStmt& n);
    // The copy-on-write source of a value collection loop whose body may
    // write the iterated storage: before a statement that may only replace
    // the array, the loop retains it once; before one that may write its
    // elements in place, it copies it once (`copied` is set then).
    void emit_source_retain(const LifetimeLowering::LoopSource& source);
    void emit_source_copy(
        const LifetimeLowering::LoopSource& source, const std::string& copied);
    // The body of a reference loop over a reference binding, which pinned
    // `entry_array` (of length `entry_length`): after each run of statements
    // with one that may replace or resize the array (`reshapes`, by index),
    // and before a break or continue lowered with such a statement, the
    // check that the binding still designates that array.
    void lower_pinned_body(
        const ForStmt& n, ValueId entry_array, ValueId entry_length,
        const std::vector<bool>& reshapes);

    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LocalScope& scope_;
    CheckElisionFacts& facts_;
    LoopTargets& loop_targets_;
    const BorrowInference& borrows_;
    const StorageWrites& storage_writes_;
    LifetimeLowering& lifetime_;
    ConversionLowering& conversions_;
    TextIdioms& text_idioms_;
    CollectionIdioms& collection_idioms_;
};

} // namespace quidra::lowering
