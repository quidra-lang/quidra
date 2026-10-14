#include "llvm_backend/memory_emitter.hpp"

#include "llvm_backend/copy_idioms.hpp"
#include "llvm_backend/type_helpers.hpp"
#include "llvm_backend/small_rational.hpp"
#include "llvm_backend/bare_integer.hpp"
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

namespace {
std::string empty_owned_value(const Type& type) {
    if(type.kind==TypeKind::Real||type.kind==TypeKind::Tuple) return "zeroinitializer";
    if(is_bare_integer(type)) return "0";
    return is_pointer_runtime_type(type) ? "null" : "zeroinitializer";
}
}


void MemoryEmitter::emit(const ir::DeclareLocal& n,const ir::Instruction&){
    debug_.declare_variable(
        n.name,n.source_name,n.type,
        std::max<std::uint32_t>(1,n.source_line));
}

void MemoryEmitter::emit(const ir::DeclareReference&,const ir::Instruction&){}

void MemoryEmitter::emit(const ir::AddressReferenceSlot& n,const ir::Instruction&){
    copy_by_zero_gep(builder_,symbols_.value(n.out),Inbounds::yes,symbols_.reference_slot(n.name));
}

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
    if(n.bounds_proven) {
        if(is_fixed_array(n.array_type)) {
            if(array_layout_.inline_fixed_child(n.array_type))
                throw std::logic_error("address-taken fixed-array edge was not boxed");
            builder_.call(symbols_.value(n.out),runtime_abi::prelude::fixed_array_slot_proven,
                {{ptr,symbols_.value(n.array)},{i64,symbols_.value(n.index)},{i64,stride}});
        } else builder_.call(symbols_.value(n.out),runtime_abi::prelude::array_slot_proven,
                {{ptr,symbols_.value(n.array)},{i64,symbols_.value(n.index)},{i64,stride}});
    }else if(n.bin_element||!is_fixed_array(n.array_type)){
        builder_.call(symbols_.value(n.out),runtime_abi::prelude::array_slot,{{ptr,symbols_.value(n.array)},{i64,symbols_.value(n.index)},{i64,stride},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
    }else{
        if(array_layout_.inline_fixed_child(n.array_type)){
            throw std::logic_error("address-taken fixed-array edge was not boxed");
        }
        builder_.call(symbols_.value(n.out),runtime_abi::prelude::fixed_array_slot,{{ptr,symbols_.value(n.array)},{i64,symbols_.value(n.index)},{i64,n.array_type.length},{i64,stride},{i64,sites_.line(n.position.line)},{i64,sites_.column(n.position.column)}});
    }
}

void MemoryEmitter::emit(const ir::LoadAddress& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    if(n.type.kind!=TypeKind::Array)
        builder_.call(runtime_abi::memory::init_check,{{ptr,symbols_.value(n.address)},{i64,sites_.statement_line()},{i64,sites_.statement_column()}});
    builder_.load(symbols_.value(n.out),llvm_type(n.type),symbols_.value(n.address),Align::one);
    symbols_.storage_origin(n.out,symbols_.value(n.address),n.type);
}

void MemoryEmitter::emit(const ir::StoreAddress& n,const ir::Instruction&){
    if(owns_managed(n.type)){
        if(n.type.kind==TypeKind::Tuple && (symbols_.node_cleanup() || symbols_.has_node_ownership()))
            lifetime_.release_tuple_slot(n.type,symbols_.value(n.address),Align::one);
        else {
            auto old=names_.value("address.old");
            builder_.load(old,llvm_type(n.type),symbols_.value(n.address),Align::one);
            if(symbols_.node_cleanup() || symbols_.has_node_ownership())
                builder_.store({llvm_type(n.type),empty_owned_value(n.type)},symbols_.value(n.address),Align::one);
            lifetime_.release_value(n.type,old);
        }
    }
    builder_.store({llvm_type(n.type),symbols_.value(n.value)},symbols_.value(n.address),Align::one);
    symbols_.transfer_owned(n.value,n.type);
    builder_.call(runtime_abi::memory::init_mark_range,{{ptr,symbols_.value(n.address)},{i64,runtime_storage_bytes(n.type)}});
}

void MemoryEmitter::emit(const ir::BindReference& n,const ir::Instruction&){
    const auto old=names_.value("ref.old");
    builder_.load(old,ptr,symbols_.reference_slot(n.name),Align::none);
    builder_.call(runtime_abi::memory::managed_pin,{{ptr,n.address?symbols_.value(n.address):"null"}});
    if(symbols_.node_cleanup() || symbols_.is_captured_reference_slot(n.name))
        builder_.store({ptr,n.address?symbols_.value(n.address):"null"},symbols_.reference_slot(n.name),Align::none);
    builder_.call(runtime_abi::memory::managed_unpin,{{ptr,old}});
    if(!symbols_.node_cleanup() && !symbols_.is_captured_reference_slot(n.name))
        builder_.store({ptr,n.address?symbols_.value(n.address):"null"},symbols_.reference_slot(n.name),Align::none);
}

void MemoryEmitter::emit(const ir::ReferenceAddress& n,const ir::Instruction&){
    builder_.load(symbols_.value(n.out),ptr,symbols_.reference_slot(n.name),Align::none);
}

void MemoryEmitter::emit(const ir::LoadReference& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    auto a="%ref.load.addr."+std::to_string(n.out);
    builder_.load(a,ptr,symbols_.reference_slot(n.name),Align::none);
    if(n.type.kind!=TypeKind::Array)builder_.call(runtime_abi::memory::init_check,{{ptr,a},{i64,sites_.statement_line()},{i64,sites_.statement_column()}});
    builder_.load(symbols_.value(n.out),llvm_type(n.type),a,Align::one);
    symbols_.storage_origin(n.out,a,n.type);
}

void MemoryEmitter::emit(const ir::StoreReference& n,const ir::Instruction&){
    auto a=names_.value("ref.store.addr");
    builder_.load(a,ptr,symbols_.reference_slot(n.name),Align::none);
    if(owns_managed(n.type)){
        if(n.type.kind==TypeKind::Tuple && (symbols_.node_cleanup() || symbols_.has_node_ownership()))
            lifetime_.release_tuple_slot(n.type,a,Align::one);
        else {
            auto old=names_.value("ref.store.old");
            builder_.load(old,llvm_type(n.type),a,Align::one);
            if(symbols_.node_cleanup() || symbols_.has_node_ownership())
                builder_.store({llvm_type(n.type),empty_owned_value(n.type)},a,Align::one);
            lifetime_.release_value(n.type,old);
        }
    }
    builder_.store({llvm_type(n.type),symbols_.value(n.value)},a,Align::one);
    symbols_.transfer_owned(n.value,n.type);
    builder_.call(runtime_abi::memory::init_mark_range,{{ptr,a},{i64,runtime_storage_bytes(n.type)}});
}

void MemoryEmitter::emit(const ir::LoadLocal& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    builder_.load(symbols_.value(n.out),llvm_type(n.type),symbols_.storage(n.name),Align::none);
    if (n.move_out) {
        if (symbols_.is_writable_parameter(n.name) || symbols_.is_borrowed_parameter(n.name) ||
            symbols_.is_borrowed_local(n.name) || !owns_managed(n.type))
            throw std::logic_error("only an owned managed local may be moved out");
        symbols_.acquire_owned(n.out,n.type,symbols_.value(n.out));
        builder_.store({llvm_type(n.type),empty_owned_value(n.type)},symbols_.storage(n.name),Align::none);
    } else {
        symbols_.load_borrowed_owner(n.out,n.name,n.type);
    }
}

void MemoryEmitter::emit(const ir::StoreLocal& n,const ir::Instruction&){
    if(owns_managed(n.type) && !n.borrowed &&
       !n.replace_without_release){
        if(n.type.kind==TypeKind::Tuple && (symbols_.node_cleanup() || symbols_.has_node_ownership()))
            lifetime_.release_tuple_slot(n.type,symbols_.storage(n.name),Align::none);
        else {
            auto old=names_.value("local.old");
            builder_.load(old,llvm_type(n.type),symbols_.storage(n.name),Align::none);
            if(symbols_.node_cleanup() || symbols_.has_node_ownership())
                builder_.store({llvm_type(n.type),empty_owned_value(n.type)},symbols_.storage(n.name),Align::none);
            lifetime_.release_value(n.type,old);
        }
    }
    builder_.store({llvm_type(n.type),symbols_.value(n.value)},symbols_.storage(n.name),Align::none);
    if(n.borrowed)symbols_.bind_borrowed_owner(n.name,n.value,n.type);
    else symbols_.transfer_owned(n.value,n.type);
}

} // namespace quidra::llvm_backend
