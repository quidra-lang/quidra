#pragma once

// AutogradEmitter: automatic differentiation in a function, through the
// runtime: whether a tensor is tracked or has a gradient, clearing a
// gradient, tracking, untracking and retracking a tensor (TensorTrack's
// mode; tracking into an autograd target), the backward pass to explicit
// gradient targets and to autograd targets, reading a gradient, and the
// autograd targets themselves ($std.autograd.Target): creation, and
// querying, clearing and reading their gradient.
//
// Owns no state. The backward pass passes its targets and their kinds
// through the entry-frame scratch slots the function's pre-pass reserved
// for it; a backward pass without any target is an internal error.

#include "llvm_backend/statement_attribution.hpp"
#include "quidra/ir/module.hpp"
#include "llvm_backend/scratch_planner.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_text/llvm_builder.hpp"

namespace quidra::llvm_backend {

class AutogradEmitter {
public:
    AutogradEmitter(llvm_text::LlvmBuilder& builder,SymbolTable& symbols,ScratchPlanner& scratch,TemporaryNames& names,const StatementAttribution& sites)
        :builder_(builder),symbols_(symbols),scratch_(scratch),names_(names),sites_(sites){}

    // Before any text of the function is written (FunctionPrepass): what
    // the instruction needs reserved.
    void reserve(const ir::TensorBackward& n,const ir::Instruction& ins);

    void emit(const ir::TensorIsTracked& n,const ir::Instruction& ins);
    void emit(const ir::TensorHasGrad& n,const ir::Instruction& ins);
    void emit(const ir::TensorClearGrad& n,const ir::Instruction& ins);
    void emit(const ir::TensorTrack& n,const ir::Instruction& ins);
    void emit(const ir::TensorBackward& n,const ir::Instruction& ins);
    void emit(const ir::TensorGrad& n,const ir::Instruction& ins);
    void emit(const ir::AutogradTargetCreate& n,const ir::Instruction& ins);
    void emit(const ir::AutogradTargetHasGrad& n,const ir::Instruction& ins);
    void emit(const ir::AutogradTargetClearGrad& n,const ir::Instruction& ins);
    void emit(const ir::AutogradTargetGradient& n,const ir::Instruction& ins);

private:
    llvm_text::LlvmBuilder& builder_;
    SymbolTable& symbols_;
    ScratchPlanner& scratch_;
    TemporaryNames& names_;
    const StatementAttribution& sites_;
};

} // namespace quidra::llvm_backend
