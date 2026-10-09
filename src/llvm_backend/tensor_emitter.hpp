#pragma once

// TensorEmitter: the tensors of a function, written as runtime calls with
// the dtype codes and operation codes of quidra/abi: creation, transfer
// between devices, views (reshape, transpose, gather, scatter, contiguous
// copies), shape, device and layout queries, element reads, casts,
// elementwise arithmetic and comparison with a tensor or scalar operand,
// reductions of bool tensors, indexing, element writes, and the
// synchronization of the GPU device.
//
// Owns no state. A scalar operand, the index items of TensorIndex and the
// indices and value of TensorSet pass through the entry-frame scratch slots
// the function's pre-pass reserved for the instruction.

#include "llvm_backend/statement_attribution.hpp"
#include "quidra/ir/module.hpp"
#include "llvm_backend/scratch_planner.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_backend/union_box.hpp"
#include "llvm_text/llvm_builder.hpp"

namespace quidra::llvm_backend {

class TensorEmitter {
public:
    TensorEmitter(llvm_text::LlvmBuilder& builder,SymbolTable& symbols,ScratchPlanner& scratch,TemporaryNames& names,UnionBox& union_box,const StatementAttribution& sites)
        :builder_(builder),symbols_(symbols),scratch_(scratch),names_(names),union_box_(union_box),sites_(sites){}

    // Before any text of the function is written (FunctionPrepass): what
    // the instruction needs reserved.
    void reserve(const ir::TensorBinary& n,const ir::Instruction& ins);
    void reserve(const ir::TensorCompare& n,const ir::Instruction& ins);
    void reserve(const ir::TensorIndex& n,const ir::Instruction& ins);
    void reserve(const ir::TensorSet& n,const ir::Instruction& ins);

    void emit(const ir::TensorCreate& n,const ir::Instruction& ins);
    void emit(const ir::TensorTransfer& n,const ir::Instruction& ins);
    void emit(const ir::TensorReshape& n,const ir::Instruction& ins);
    void emit(const ir::TensorGather& n,const ir::Instruction& ins);
    void emit(const ir::TensorScatter& n,const ir::Instruction& ins);
    void emit(const ir::TensorTranspose& n,const ir::Instruction& ins);
    void emit(const ir::TensorContiguous& n,const ir::Instruction& ins);
    void emit(const ir::TensorShape& n,const ir::Instruction& ins);
    void emit(const ir::TensorDevice& n,const ir::Instruction& ins);
    void emit(const ir::TensorIsContiguous& n,const ir::Instruction& ins);
    void emit(const ir::TensorItem& n,const ir::Instruction& ins);
    void emit(const ir::TensorCast& n,const ir::Instruction& ins);
    void emit(const ir::TensorCompare& n,const ir::Instruction& ins);
    void emit(const ir::TensorBoolReduce& n,const ir::Instruction& ins);
    void emit(const ir::TensorBinary& n,const ir::Instruction& ins);
    void emit(const ir::TensorIndex& n,const ir::Instruction& ins);
    void emit(const ir::TensorSet& n,const ir::Instruction& ins);
    void emit(const ir::GpuSync& n,const ir::Instruction& ins);

private:
    llvm_text::LlvmBuilder& builder_;
    SymbolTable& symbols_;
    ScratchPlanner& scratch_;
    TemporaryNames& names_;
    UnionBox& union_box_;
    const StatementAttribution& sites_;
};

} // namespace quidra::llvm_backend
