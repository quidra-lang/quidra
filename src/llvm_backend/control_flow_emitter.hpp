#pragma once

// ControlFlowEmitter: the terminators of a function's blocks. Return and
// ReturnVoid release the values the function still owns (LifetimeEmitter),
// leave the runtime's call-depth count when the function guards its depth,
// and clear the source provenance in the entry function; Exit releases the
// owned values and exits with the status; FailError stops the program with
// the unhandled error; Jump and Branch transfer control to their labels.
//
// Owns no state. The labels are the IR's block labels.

#include "llvm_backend/statement_attribution.hpp"
#include "quidra/ir/module.hpp"
#include "llvm_backend/lifetime_emitter.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_text/llvm_builder.hpp"

namespace quidra::llvm_backend {

class ControlFlowEmitter {
public:
    ControlFlowEmitter(const ir::Function& fn,bool guard_stack_depth,llvm_text::LlvmBuilder& builder,
                       const SymbolTable& symbols,LifetimeEmitter& lifetime,const StatementAttribution& sites)
        :fn_(fn),guard_stack_depth_(guard_stack_depth),builder_(builder),symbols_(symbols),lifetime_(lifetime),sites_(sites){}

    void emit(const ir::Exit& n,const ir::Instruction& ins);
    void emit(const ir::FailError& n,const ir::Instruction& ins);
    void emit(const ir::Return& n,const ir::Instruction& ins);
    void emit(const ir::ReturnVoid& n,const ir::Instruction& ins);
    void emit(const ir::Jump& n,const ir::Instruction& ins);
    void emit(const ir::Branch& n,const ir::Instruction& ins);

private:
    const ir::Function& fn_;
    bool guard_stack_depth_{};
    llvm_text::LlvmBuilder& builder_;
    const SymbolTable& symbols_;
    LifetimeEmitter& lifetime_;
    const StatementAttribution& sites_;
};

} // namespace quidra::llvm_backend
