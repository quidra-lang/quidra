#pragma once

// LifetimeEmitter: the emitted lifetime operations of one function: the
// release of a managed value (quidra_managed_release with the drop callback
// of its type), and at every function exit the unpinning of the references
// and the release of the values the locals still own. It is also the
// emitter of the lifetime domain: Clone copies a value deeply through the
// clone helper of its type, Retain adds a reference to a managed value and
// Release releases one; Clone and Retain give their IR value its type. Pin
// and Unpin pin and unpin the managed allocation that holds a value.
//
// Owns no state. The cleanup at exits walks the references, then the locals,
// in name order (std::map), one temporary per entry; parameters passed by
// address and borrowed parameters and locals are not released.

#include "quidra/ir/module.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_text/llvm_builder.hpp"

#include <string>
#include <unordered_map>

namespace quidra::llvm_backend {

class LifetimeEmitter {
public:
    LifetimeEmitter(llvm_text::LlvmBuilder& builder,TemporaryNames& names,SymbolTable& symbols,
                    const std::unordered_map<std::string,ir::ClassLayout>& layouts)
        :builder_(builder),names_(names),symbols_(symbols),layouts_(layouts){}

    void release_value(const Type& type, const std::string& raw);

    void cleanup_owned_values();

    // The handlers of its domain's instructions.
    void emit(const ir::Clone& n,const ir::Instruction& ins);
    void emit(const ir::Retain& n,const ir::Instruction& ins);
    void emit(const ir::Release& n,const ir::Instruction& ins);
    void emit(const ir::Pin& n,const ir::Instruction& ins);
    void emit(const ir::Unpin& n,const ir::Instruction& ins);

private:
    std::string drop_callback(const Type& type) const;

    llvm_text::LlvmBuilder& builder_;
    TemporaryNames& names_;
    SymbolTable& symbols_;
    const std::unordered_map<std::string,ir::ClassLayout>& layouts_;
};

} // namespace quidra::llvm_backend
