#pragma once

// AggregateEmitter: arrays, class objects and variants of a function.
// Arrays are allocated with their initialization state (fixed arrays inline
// their nested fixed arrays unless the module's array layout policy keeps
// the edge boxed), read and written through element slots with bounds and
// initialization checks (none where the lowering proved them), grown, sorted
// and measured; class objects are allocated with their fields stored at the
// layout's offsets; a variant is a union box (UnionBox) holding its tag and
// payload.
//
// ArrayGet and ArraySet address their element the same way (ArraySlot,
// emit_slot): through the runtime's slot function of a fixed or a dynamic
// array, checked against the bounds at run time (reporting the source
// position) unless the lowering proved them, or, under a bounds guard, both
// slots selected at run time and joined by a phi.
//
// Owns no state. Some handlers keep the reference SymbolTable::type_at
// returns while they give their result a type, which the node-based value
// map allows (see SymbolTable).

#include "llvm_backend/statement_attribution.hpp"
#include "quidra/ir/module.hpp"
#include "llvm_backend/lifetime_emitter.hpp"
#include "llvm_backend/storage_layout.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_backend/union_box.hpp"
#include "llvm_text/llvm_builder.hpp"

#include <optional>
#include <string>
#include <unordered_map>

namespace quidra::llvm_backend {

struct ArraySlot;

class AggregateEmitter {
public:
    AggregateEmitter(const std::unordered_map<std::string,ir::ClassLayout>& layouts,
                     const ArrayLayoutPolicy& array_layout,llvm_text::LlvmBuilder& builder,SymbolTable& symbols,
                     TemporaryNames& names,LifetimeEmitter& lifetime,UnionBox& union_box,const StatementAttribution& sites)
        :layouts_(layouts),array_layout_(array_layout),builder_(builder),symbols_(symbols),names_(names),
         lifetime_(lifetime),union_box_(union_box),sites_(sites){}

    void emit(const ir::ArrayMake& n,const ir::Instruction& ins);
    void emit(const ir::ArrayAlloc& n,const ir::Instruction& ins);
    void emit(const ir::ClassMake& n,const ir::Instruction& ins);
    void emit(const ir::FieldGet& n,const ir::Instruction& ins);
    void emit(const ir::FieldSet& n,const ir::Instruction& ins);
    void emit(const ir::ArrayLength& n,const ir::Instruction& ins);
    void emit(const ir::ArrayCanAppendMove& n,const ir::Instruction& ins);
    void emit(const ir::ArrayGrowMove& n,const ir::Instruction& ins);
    void emit(const ir::ArraySorted& n,const ir::Instruction& ins);
    void emit(const ir::ArrayInitializationComplete& n,const ir::Instruction& ins);
    void emit(const ir::ArrayGet& n,const ir::Instruction& ins);
    void emit(const ir::ArraySet& n,const ir::Instruction& ins);
    void emit(const ir::VariantMake& n,const ir::Instruction& ins);
    void emit(const ir::VariantTag& n,const ir::Instruction& ins);
    void emit(const ir::VariantPayload& n,const ir::Instruction& ins);

private:
    // The element's address in target: through the slot function of a proven
    // or a checked access.
    void emit_slot_address(const ArraySlot& element,const std::string& target,bool proven);
    // The element's address in target: proven, checked, or both under a
    // bounds guard.
    void emit_slot(const ArraySlot& element,const std::string& target,bool bounds_proven,
                   const std::optional<ir::ValueId>& bounds_guard);

    const std::unordered_map<std::string,ir::ClassLayout>& layouts_;
    const ArrayLayoutPolicy& array_layout_;
    llvm_text::LlvmBuilder& builder_;
    SymbolTable& symbols_;
    TemporaryNames& names_;
    LifetimeEmitter& lifetime_;
    UnionBox& union_box_;
    const StatementAttribution& sites_;
};

} // namespace quidra::llvm_backend
