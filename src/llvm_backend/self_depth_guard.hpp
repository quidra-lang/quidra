#pragma once

// SelfDepthGuard: the wrapper of a self-depth recursive function. Without
// debug info, a function that calls only itself recursively counts its own
// depth in an extra i64 parameter instead of the runtime's call-depth
// counter: its body is defined as <symbol>.depth, and the public symbol is an
// alwaysinline wrapper that calls it with depth 1.
//
// Owns no state. The wrapper follows the function's definition in the text.

#include "quidra/ir/module.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_text/llvm_builder.hpp"

namespace quidra::llvm_backend {

class SelfDepthGuard {
public:
    SelfDepthGuard(llvm_text::LlvmBuilder& builder,const ir::Function& fn,const SymbolTable& symbols,bool self_depth_recursive)
        :builder_(builder),fn_(fn),symbols_(symbols),self_depth_recursive_(self_depth_recursive){}

    void emit_wrapper();

private:
    llvm_text::LlvmBuilder& builder_;
    const ir::Function& fn_;
    const SymbolTable& symbols_;
    bool self_depth_recursive_{};
};

} // namespace quidra::llvm_backend
