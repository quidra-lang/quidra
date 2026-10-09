#include "llvm_backend/memory_emitter.hpp"

#include "llvm_backend/copy_idioms.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/storage_layout.hpp"
#include "llvm_backend/type_lowering.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void MemoryEmitter::emit(const ir::DeclareLocal& n,const ir::Instruction&){
    debug_.declare_variable(
        n.name,n.source_name,n.type,
        std::max<std::uint32_t>(1,n.source_line));
}

void MemoryEmitter::emit(const ir::DeclareReference&,const ir::Instruction&){}

void MemoryEmitter::emit(const ir::AddressLocal& n,const ir::Instruction&){
    copy_by_zero_gep(builder_,symbols_.value(n.out),Inbounds::yes,symbols_.storage(n.name));
}

void MemoryEmitter::emit(const ir::AddressField& n,const ir::Instruction&){
    const auto object_type=symbols_.type_at(n.object);
    const auto& layout=layouts_.at(object_type.class_name);
    builder_.getelementptr(symbols_.value(n.out),Inbounds::yes,i8,symbols_.value(n.object),{{i64,class_field_offset(layout,n.index)}});
}

void MemoryEmitter::emit(const ir::AddressElement& n,const ir::Instruction&){
    const auto stride=n.bin_element?1:array_element_stride(n.array_type,array_layout_);
    if(n.bin_element||!is_fixed_array(n.array_type)){
        builder_.call(symbols_.value(n.out),runtime_abi::prelude::array_slot,{{ptr,symbols_.value(n.array)},{i64,symbols_.value(n.index)},{i64,stride},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }else{
        if(array_layout_.inline_fixed_child(n.array_type)){
            throw std::logic_error("address-taken fixed-array edge was not boxed");
        }
        builder_.call(symbols_.value(n.out),runtime_abi::prelude::fixed_array_slot,{{ptr,symbols_.value(n.array)},{i64,symbols_.value(n.index)},{i64,n.array_type.length},{i64,stride},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
    }
}

void MemoryEmitter::emit(const ir::LoadAddress& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    if(n.type.kind!=TypeKind::Array)
        builder_.call(runtime_abi::memory::init_check,{{ptr,symbols_.value(n.address)},{i64,sites_.statement_line()},{i64,sites_.statement_column()}});
    builder_.load(symbols_.value(n.out),llvm_type(n.type),symbols_.value(n.address),Align::one);
}

void MemoryEmitter::emit(const ir::StoreAddress& n,const ir::Instruction&){
    if(requires_lifetime_management(n.type)){
        auto old=names_.value("address.old");
        builder_.load(old,llvm_type(n.type),symbols_.value(n.address),Align::one);
        lifetime_.release_value(n.type,old);
    }
    builder_.store({llvm_type(n.type),symbols_.value(n.value)},symbols_.value(n.address),Align::one);
    builder_.call(runtime_abi::memory::init_mark_range,{{ptr,symbols_.value(n.address)},{i64,runtime_storage_bytes(n.type)}});
}

void MemoryEmitter::emit(const ir::BindReference& n,const ir::Instruction&){
    const auto old=names_.value("ref.old");
    builder_.load(old,ptr,symbols_.local(n.name),Align::none);
    builder_.call(runtime_abi::memory::managed_pin,{{ptr,symbols_.value(n.address)}});
    builder_.call(runtime_abi::memory::managed_unpin,{{ptr,old}});
    builder_.store({ptr,symbols_.value(n.address)},symbols_.local(n.name),Align::none);
}

void MemoryEmitter::emit(const ir::ReferenceAddress& n,const ir::Instruction&){
    builder_.load(symbols_.value(n.out),ptr,symbols_.local(n.name),Align::none);
}

void MemoryEmitter::emit(const ir::LoadReference& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    auto a="%ref.load.addr."+std::to_string(n.out);
    builder_.load(a,ptr,symbols_.local(n.name),Align::none);
    if(n.type.kind!=TypeKind::Array)builder_.call(runtime_abi::memory::init_check,{{ptr,a},{i64,sites_.statement_line()},{i64,sites_.statement_column()}});
    builder_.load(symbols_.value(n.out),llvm_type(n.type),a,Align::one);
}

void MemoryEmitter::emit(const ir::StoreReference& n,const ir::Instruction&){
    auto a=names_.value("ref.store.addr");
    builder_.load(a,ptr,symbols_.local(n.name),Align::none);
    if(requires_lifetime_management(n.type)){
        auto old=names_.value("ref.store.old");
        builder_.load(old,llvm_type(n.type),a,Align::one);
        lifetime_.release_value(n.type,old);
    }
    builder_.store({llvm_type(n.type),symbols_.value(n.value)},a,Align::one);
    builder_.call(runtime_abi::memory::init_mark_range,{{ptr,a},{i64,runtime_storage_bytes(n.type)}});
}

void MemoryEmitter::emit(const ir::LoadLocal& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    builder_.load(symbols_.value(n.out),llvm_type(n.type),symbols_.storage(n.name),Align::none);
}

void MemoryEmitter::emit(const ir::StoreLocal& n,const ir::Instruction&){
    if(requires_lifetime_management(n.type) && !n.borrowed &&
       !n.replace_without_release){
        auto old=names_.value("local.old");
        builder_.load(old,llvm_type(n.type),symbols_.storage(n.name),Align::none);
        lifetime_.release_value(n.type,old);
    }
    builder_.store({llvm_type(n.type),symbols_.value(n.value)},symbols_.storage(n.name),Align::none);
}

} // namespace quidra::llvm_backend
