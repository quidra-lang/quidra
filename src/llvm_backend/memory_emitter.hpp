#pragma once

// MemoryEmitter: the storage of a function: loads and stores of locals
// (%local.X, or %arg.X for a parameter passed by address), of references
// (a pinned address kept in an entry-frame slot) and through addresses of
// locals, class fields and array or bin elements. A store of a managed
// value releases the value it replaces, unless the lowering marked the
// store borrowed or as replacing without release; a store through an
// address marks the bytes initialized, a load through one checks them.
// DeclareLocal declares its variable to the debug information;
// DeclareReference writes nothing, because the entry frame allocates every
// reference slot.
//
// Owns no state. The entry-frame allocas come from the storage the
// SymbolTable recorded before emission; element addresses follow the
// module's array layout policy.

#include "llvm_backend/statement_attribution.hpp"
#include "quidra/ir/module.hpp"
#include "llvm_backend/debug_emitter.hpp"
#include "llvm_backend/lifetime_emitter.hpp"
#include "llvm_backend/storage_layout.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_text/llvm_builder.hpp"

#include <string>
#include <unordered_map>

namespace quidra::llvm_backend {

class MemoryEmitter {
public:
    MemoryEmitter(const std::unordered_map<std::string,ir::ClassLayout>& layouts,
                  const ArrayLayoutPolicy& array_layout,llvm_text::LlvmBuilder& builder,SymbolTable& symbols,
                  TemporaryNames& names,DebugEmitter& debug,LifetimeEmitter& lifetime,const StatementAttribution& sites)
        :layouts_(layouts),array_layout_(array_layout),builder_(builder),symbols_(symbols),names_(names),
         debug_(debug),lifetime_(lifetime),sites_(sites){}

    void emit(const ir::DeclareLocal& n,const ir::Instruction& ins);
    void emit(const ir::DeclareReference& n,const ir::Instruction& ins);
    void emit(const ir::AddressLocal& n,const ir::Instruction& ins);
    void emit(const ir::AddressField& n,const ir::Instruction& ins);
    void emit(const ir::AddressElement& n,const ir::Instruction& ins);
    void emit(const ir::LoadAddress& n,const ir::Instruction& ins);
    void emit(const ir::StoreAddress& n,const ir::Instruction& ins);
    void emit(const ir::BindReference& n,const ir::Instruction& ins);
    void emit(const ir::ReferenceAddress& n,const ir::Instruction& ins);
    void emit(const ir::LoadReference& n,const ir::Instruction& ins);
    void emit(const ir::StoreReference& n,const ir::Instruction& ins);
    void emit(const ir::LoadLocal& n,const ir::Instruction& ins);
    void emit(const ir::StoreLocal& n,const ir::Instruction& ins);

private:
    const std::unordered_map<std::string,ir::ClassLayout>& layouts_;
    const ArrayLayoutPolicy& array_layout_;
    llvm_text::LlvmBuilder& builder_;
    SymbolTable& symbols_;
    TemporaryNames& names_;
    DebugEmitter& debug_;
    LifetimeEmitter& lifetime_;
    const StatementAttribution& sites_;
};

} // namespace quidra::llvm_backend
