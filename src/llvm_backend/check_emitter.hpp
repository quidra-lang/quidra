#pragma once

// CheckEmitter: the run-time checks of a function: the rank and the known
// extents of a shaped tensor (through the runtime), the equality of two
// extents and the nonzero step of a range (failing fast with the shape or
// range-step diagnostic), the array of a reference loop over a reference
// binding (failing fast with FOR_ITERATION, its code and message interned
// from quidra/abi/runtime_failure.hpp where the check occurs), the flag of a
// read that may be uninitialized (failing fast with UNINITIALIZED, its
// message naming the storage, from the same table), and the user's test
// assertions.
//
// Owns no state. The extent and iteration checks allocate their labels
// through FailFastEmitter; the range-step check names its labels after the
// step's IR value, so it allocates no name.

#include "llvm_backend/statement_attribution.hpp"
#include "quidra/ir/module.hpp"
#include "llvm_backend/fail_fast_emitter.hpp"
#include "llvm_backend/string_pool.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_text/llvm_builder.hpp"

namespace quidra::llvm_backend {

class CheckEmitter {
public:
    CheckEmitter(StringPool& pool,llvm_text::LlvmBuilder& builder,const SymbolTable& symbols,
                 TemporaryNames& names,FailFastEmitter& fail_fast,const StatementAttribution& sites)
        :pool_(pool),builder_(builder),symbols_(symbols),names_(names),fail_fast_(fail_fast),sites_(sites){}

    void emit(const ir::ShapedConstraintCheck& n,const ir::Instruction& ins);
    void emit(const ir::ExtentEqualCheck& n,const ir::Instruction& ins);
    void emit(const ir::TestAssert& n,const ir::Instruction& ins);
    void emit(const ir::RangeCheckStep& n,const ir::Instruction& ins);
    void emit(const ir::IterationShapeCheck& n,const ir::Instruction& ins);
    void emit(const ir::InitializedCheck& n,const ir::Instruction& ins);

private:
    StringPool& pool_;
    llvm_text::LlvmBuilder& builder_;
    const SymbolTable& symbols_;
    TemporaryNames& names_;
    FailFastEmitter& fail_fast_;
    const StatementAttribution& sites_;
};

} // namespace quidra::llvm_backend
