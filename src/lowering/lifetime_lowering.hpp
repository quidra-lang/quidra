#pragma once

// LifetimeLowering: when a value is copied (Clone or Retain), whether an
// expression's result is an owned temporary, and releasing one. Defined in
// lifetime_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/ir_names.hpp"
#include <string>
#include <algorithm>
#include <utility>
#include <vector>

namespace quidra::lowering {

class LocalScope;

class LifetimeLowering {
public:
    LifetimeLowering(
        const CheckedProgram& checked, ir::FunctionBuilder& builder, const LocalScope& scope)
        : checked_(checked), builder_(builder),scope_(scope) {}

    // copies, owned temporaries, releases
    ValueId copy_value(ValueId v,const Type& t);
    bool expression_owns_result(const Expr& expression) const;
    // A borrowed indexed value must survive code in later index operands,
    // including writes that replace its storage or mutate its elements.
    bool index_base_needs_snapshot(const IndexExpr& index) const;
    void release_temporary(const Expr& expression, ValueId value);
    void enter_tuple_move(std::string subject){moving_tuples_.push_back(std::move(subject));}
    void leave_tuple_move(){moving_tuples_.pop_back();}
    bool tuple_moves(const std::string& subject) const {return std::find(moving_tuples_.begin(),moving_tuples_.end(),subject)!=moving_tuples_.end();}

    // The hidden source of a value collection loop, which holds the iterated
    // array and, once `owned` is set, a reference or copy of it that the loop
    // owns (control_flow_lowering.cpp). The loop's own exits release it; a
    // return or a `try` propagation inside the loop releases the sources of
    // every loop it leaves.
    struct LoopSource {
        std::string source;
        std::string owned;
        Type type;
    };
    void enter_loop_source(LoopSource source) { loop_sources_.push_back(std::move(source)); }
    void leave_loop_source() { loop_sources_.pop_back(); }
    void release_loop_source(const LoopSource& source);
    // The arrays that reference loops over a reference binding pin at entry
    // (control_flow_lowering.cpp): the loop's own exits unpin them, and a
    // return or a `try` propagation inside the loop unpins the arrays of
    // every loop it leaves, with the sources (release_loop_sources).
    void enter_loop_reference(std::string name) { loop_references_.push_back(std::move(name)); }
    void leave_loop_reference() { loop_references_.pop_back(); }
    void enter_loop_value(ValueId value, Type type) { loop_values_.emplace_back(value,std::move(type)); }
    void leave_loop_value() { loop_values_.pop_back(); }
    void enter_loop_pin(ValueId array) { loop_pins_.push_back(array); }
    void leave_loop_pin() { loop_pins_.pop_back(); }
    // Releases the sources and unpins the arrays of every loop being lowered.
    void release_loop_sources();

private:
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    const LocalScope& scope_;
    std::vector<LoopSource> loop_sources_;
    std::vector<std::string> moving_tuples_;
    std::vector<std::pair<ValueId,Type>> loop_values_;
    std::vector<ValueId> loop_pins_;
    std::vector<std::string> loop_references_;
};

} // namespace quidra::lowering
