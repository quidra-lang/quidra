#pragma once

// LifetimeLowering: when a value is copied (Clone or Retain), whether an
// expression's result is an owned temporary, and releasing one. Defined in
// lifetime_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/ir_names.hpp"
#include <string>
#include <utility>
#include <vector>

namespace quidra::lowering {

class LifetimeLowering {
public:
    LifetimeLowering(
        const CheckedProgram& checked, ir::FunctionBuilder& builder)
        : checked_(checked), builder_(builder) {}

    // copies, owned temporaries, releases
    ValueId copy_value(ValueId v,const Type& t);
    bool expression_owns_result(const Expr& expression) const;
    void release_temporary(const Expr& expression, ValueId value);

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
    void enter_loop_pin(ValueId array) { loop_pins_.push_back(array); }
    void leave_loop_pin() { loop_pins_.pop_back(); }
    // Releases the sources and unpins the arrays of every loop being lowered.
    void release_loop_sources();

private:
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    std::vector<LoopSource> loop_sources_;
    std::vector<ValueId> loop_pins_;
};

} // namespace quidra::lowering
