#include "llvm_backend/aggregate_emitter.hpp"

#include "llvm_backend/copy_idioms.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/storage_layout.hpp"
#include "llvm_backend/type_helpers.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "quidra/abi/layout.hpp"
#include "quidra/ir/dtype.hpp"
#include "quidra/ir/initialization_mask.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void AggregateEmitter::emit(const ir::ArrayMake& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    const bool fixed=is_fixed_array(n.type);
    const auto stride=array_element_stride(n.type,array_layout_);
    const auto bytes=fixed
        ? fixed_array_storage_bytes(n.type,array_layout_)
        : abi::array_layout::payload_offset+stride*n.elements.size();
    builder_.call(symbols_.value(n.out),runtime_abi::prelude::alloc,{{i64,bytes}});
    if(!fixed) builder_.store({i64,n.elements.size()},symbols_.value(n.out),Align::one);
    if(fixed){
        const auto storage=fixed_array_storage(n.type,array_layout_);
        builder_.call(runtime_abi::memory::init_create,{{ptr,symbols_.value(n.out)},{i64,storage.count},{i64,runtime_storage_bytes(storage.leaf)},{i64,0},{i32,1}});
    }else{
        builder_.call(runtime_abi::memory::init_create,{{ptr,symbols_.value(n.out)},{i64,n.elements.size()},{i64,stride},{i64,abi::array_layout::payload_offset},{i32,1}});
    }
    for(std::size_t i=0;i<n.elements.size();++i){
        auto slot="%arr.slot."+std::to_string(n.out)+"."+std::to_string(i);
        const auto offset=(fixed?0:abi::array_layout::payload_offset)+stride*i;
        builder_.getelementptr(slot,Inbounds::yes,i8,symbols_.value(n.out),{{i64,offset}});
        if(fixed&&array_layout_.inline_fixed_child(n.type)){
            builder_.call(runtime_abi::c_library::memcpy,{{ptr,slot},{ptr,symbols_.value(n.elements[i])},{i64,stride}});
            builder_.call(runtime_abi::memory::managed_release,{{ptr,symbols_.value(n.elements[i])},{ptr,"null"}});
        }else{
            builder_.store({llvm_type(*n.type.first),symbols_.value(n.elements[i])},slot,Align::one);
        }
    }
}

void AggregateEmitter::emit(const ir::ArrayAlloc& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    if(is_fixed_array(n.type)){
        const auto storage=fixed_array_storage(n.type,array_layout_);
        const auto bytes=fixed_array_storage_bytes(n.type,array_layout_);
        builder_.call(symbols_.value(n.out),runtime_abi::prelude::alloc,{{i64,bytes}});
        builder_.call(runtime_abi::c_library::memset,{{ptr,symbols_.value(n.out)},{i32,0},{i64,bytes}});
        builder_.call(runtime_abi::memory::init_create,{{ptr,symbols_.value(n.out)},{i64,storage.count},{i64,runtime_storage_bytes(storage.leaf)},{i64,0},{i32,n.fully_initialized?1:0}});
    }else{
        const auto stride=array_element_stride(n.type,array_layout_);
        builder_.call(symbols_.value(n.out),runtime_abi::prelude::array_alloc,{{i64,symbols_.value(n.length)},{i64,stride},{i32,n.fully_initialized?1:0}});
    }
}

void AggregateEmitter::emit(const ir::ClassMake& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    const auto&layout=layouts_.at(n.type.class_name);
    const auto bytes=class_storage_bytes(layout);
    builder_.call(symbols_.value(n.out),runtime_abi::prelude::alloc,{{i64,bytes}});
    for(std::size_t i=0;i<n.fields.size();++i){
        auto slot="%class.slot."+std::to_string(n.out)+"."+std::to_string(i);
        builder_.getelementptr(slot,Inbounds::yes,i8,symbols_.value(n.out),{{i64,class_field_offset(layout,i)}});
        if(n.fields[i]) builder_.store({llvm_type(layout.fields[i]),symbols_.value(*n.fields[i])},slot,Align::one);
        else{
            const std::string_view zero=layout.fields[i].kind==TypeKind::Real?exact_real_none:is_bare_integer(layout.fields[i])?"0":is_pointer_runtime_type(layout.fields[i])?"null":is_fixed_real(layout.fields[i])?float_zero:layout.fields[i].kind==TypeKind::Bool?"false":"0";
            builder_.store({llvm_type(layout.fields[i]),zero},slot,Align::one);
        }
    }
    if(ir::has_initialization_mask(layout) && n.fields.size()<layout.fields.size()){
        // The bits of the fields the value is made with (L13).
        std::uint64_t bits=0;
        for(std::size_t i=0;i<n.fields.size();++i) if(n.fields[i]) bits|=std::uint64_t{1}<<i;
        auto slot="%class.slot."+std::to_string(n.out)+".init";
        builder_.getelementptr(slot,Inbounds::yes,i8,symbols_.value(n.out),{{i64,class_field_offset(layout,ir::initialization_mask_index(layout))}});
        builder_.store({i64,std::to_string(bits)},slot,Align::one);
    }
}

void AggregateEmitter::emit(const ir::FieldGet& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.field_type);
    const auto& layout=layouts_.at(symbols_.type_at(n.object).class_name);
    auto slot="%field.get."+std::to_string(n.out);
    builder_.getelementptr(slot,Inbounds::yes,i8,symbols_.value(n.object),{{i64,class_field_offset(layout,n.index)}});
    builder_.load(symbols_.value(n.out),llvm_type(n.field_type),slot,Align::one);
}

void AggregateEmitter::emit(const ir::FieldSet& n,const ir::Instruction&){
    const auto& layout=layouts_.at(symbols_.type_at(n.object).class_name);
    auto slot=names_.value("field.set");
    builder_.getelementptr(slot,Inbounds::yes,i8,symbols_.value(n.object),{{i64,class_field_offset(layout,n.index)}});
    if(requires_lifetime_management(n.field_type) && !n.replace_without_release){
        auto old=names_.value("field.old");
        builder_.load(old,llvm_type(n.field_type),slot,Align::one);
        lifetime_.release_value(n.field_type,old);
    }
    builder_.store({llvm_type(n.field_type),symbols_.value(n.value)},slot,Align::one);
    if(ir::has_initialization_mask(layout) && n.index<ir::initialization_mask_index(layout)){
        // A store of the field sets its initialization bit (L13).
        auto mask_slot=names_.value("field.init.slot"),mask=names_.value("field.init"),
             updated=names_.value("field.init.set");
        builder_.getelementptr(mask_slot,Inbounds::yes,i8,symbols_.value(n.object),{{i64,class_field_offset(layout,ir::initialization_mask_index(layout))}});
        builder_.load(mask,i64,mask_slot,Align::one);
        builder_.binary(updated,BinaryOp::or_,i64,mask,std::to_string(std::uint64_t{1}<<n.index));
        builder_.store({i64,updated},mask_slot,Align::one);
    }
}

void AggregateEmitter::emit(const ir::ArrayLength& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Int64));
    const auto& array_type=symbols_.type_at(n.array);
    if(is_fixed_array(array_type)){
        copy_by_leading_zero_add(builder_,symbols_.value(n.out),i64,array_type.length);
    }else{
        builder_.load(symbols_.value(n.out),i64,symbols_.value(n.array),Align::none);
    }
}

void AggregateEmitter::emit(const ir::ArrayCanAppendMove& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bool));
    builder_.call(symbols_.value(n.out),runtime_abi::array::can_append_move,{{ptr,symbols_.value(n.array)}});
}

void AggregateEmitter::emit(const ir::ArrayGrowMove& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.array_type);
    builder_.call(symbols_.value(n.out),runtime_abi::array::grow_move,{{ptr,symbols_.value(n.array)},{i64,array_element_stride(n.array_type,array_layout_)}});
}

void AggregateEmitter::emit(const ir::ArraySorted& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.array_type);
    builder_.call(symbols_.value(n.out),runtime_abi::array::sorted,{{ptr,symbols_.value(n.array)},{i32,ir::sort_element_kind_of(*n.array_type.first)},{i64,array_element_stride(n.array_type,array_layout_)},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
}

void AggregateEmitter::emit(const ir::ArrayInitializationComplete& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bool));
    builder_.call(symbols_.value(n.out),runtime_abi::array::initialization_complete,{{ptr,symbols_.value(n.array)}});
}

// The element of an array that an ArrayGet or an ArraySet reads or writes
// (ArraySlot), and the source position its bounds check reports.
struct ArraySlot {
    ir::ValueId array;
    ir::ValueId index;
    const Type& array_type;
    std::size_t stride;
    std::uint32_t line;
    std::uint32_t column;
};

void AggregateEmitter::emit_slot_address(const ArraySlot& element,const std::string& target,bool proven){
    if(is_fixed_array(element.array_type)){
        if(proven)
            builder_.call(target,runtime_abi::prelude::fixed_array_slot_proven,{{ptr,symbols_.value(element.array)},{i64,symbols_.value(element.index)},{i64,element.stride}});
        else
            builder_.call(target,runtime_abi::prelude::fixed_array_slot,{{ptr,symbols_.value(element.array)},{i64,symbols_.value(element.index)},{i64,element.array_type.length},{i64,element.stride},{i64,sites_.line(element.line)},{i64,sites_.column(element.column)}});
    }else{
        if(proven)
            builder_.call(target,runtime_abi::prelude::array_slot_proven,{{ptr,symbols_.value(element.array)},{i64,symbols_.value(element.index)},{i64,element.stride}});
        else
            builder_.call(target,runtime_abi::prelude::array_slot,{{ptr,symbols_.value(element.array)},{i64,symbols_.value(element.index)},{i64,element.stride},{i64,sites_.line(element.line)},{i64,sites_.column(element.column)}});
    }
}

void AggregateEmitter::emit_slot(const ArraySlot& element,const std::string& target,bool bounds_proven,
                                 const std::optional<ir::ValueId>& bounds_guard){
    if(bounds_proven){
        emit_slot_address(element,target,true);
    }else if(bounds_guard){
        const auto proven_label=names_.label("array.bounds.proven");
        const auto checked_label=names_.label("array.bounds.checked");
        const auto ready_label=names_.label("array.bounds.ready");
        const auto proven_slot=names_.value("array.bounds.proven.slot");
        const auto checked_slot=names_.value("array.bounds.checked.slot");
        builder_.br(symbols_.value(*bounds_guard),proven_label,checked_label);
        builder_.block(proven_label);
        emit_slot_address(element,proven_slot,true);
        builder_.br(ready_label);
        builder_.block(checked_label);
        emit_slot_address(element,checked_slot,false);
        builder_.br(ready_label);
        builder_.block(ready_label);
        builder_.phi(target,ptr,{{proven_slot,proven_label},{checked_slot,checked_label}});
    }else{
        emit_slot_address(element,target,false);
    }
}

void AggregateEmitter::emit(const ir::ArrayGet& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.element_type);
    const auto& array_type=symbols_.type_at(n.array);
    const auto stride=array_element_stride(array_type,array_layout_);
    auto slot="%arr.get.slot."+std::to_string(n.out);
    emit_slot(ArraySlot{n.array,n.index,array_type,stride,n.line,n.column},slot,n.bounds_proven,n.bounds_guard);
    if(is_fixed_array(array_type)&&array_layout_.inline_fixed_child(array_type)){
        copy_by_zero_gep(builder_,symbols_.value(n.out),Inbounds::yes,slot);
    }else{
        if(!n.initialization_proven){
            if(n.initialization_guard){
                const auto check_label=names_.label("array.init.check");
                const auto ready_label=names_.label("array.init.ready");
                builder_.br(symbols_.value(*n.initialization_guard),ready_label,check_label);
                builder_.block(check_label);
                builder_.call(runtime_abi::memory::init_check,{{ptr,slot},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
                builder_.br(ready_label);
                builder_.block(ready_label);
            }else{
                builder_.call(runtime_abi::memory::init_check,{{ptr,slot},{i64,sites_.line(n.line)},{i64,sites_.column(n.column)}});
            }
        }
        builder_.load(symbols_.value(n.out),llvm_type(n.element_type),slot,Align::one);
    }
}

void AggregateEmitter::emit(const ir::ArraySet& n,const ir::Instruction&){
    const auto& array_type=symbols_.type_at(n.array);
    const auto stride=array_element_stride(array_type,array_layout_);
    auto slot="%arr.set.slot."+std::to_string(n.index)+"."+std::to_string(n.value);
    emit_slot(ArraySlot{n.array,n.index,array_type,stride,n.line,n.column},slot,n.bounds_proven,n.bounds_guard);
    if(is_fixed_array(array_type)&&array_layout_.inline_fixed_child(array_type)){
        builder_.call(void_type,drop_name(n.element_type),{{ptr,slot}});
        builder_.call(runtime_abi::c_library::memcpy,{{ptr,slot},{ptr,symbols_.value(n.value)},{i64,stride}});
        builder_.call(runtime_abi::memory::managed_release,{{ptr,symbols_.value(n.value)},{ptr,"null"}});
    }else{
        if(requires_lifetime_management(n.element_type)){
            auto old=names_.value("array.old");
            builder_.load(old,llvm_type(n.element_type),slot,Align::one);
            lifetime_.release_value(n.element_type,old);
        }
        builder_.store({llvm_type(n.element_type),symbols_.value(n.value)},slot,Align::one);
    }
    if(!n.initialization_proven){
        if(n.initialization_guard){
            const auto mark_label=names_.label("array.init.mark");
            const auto ready_label=names_.label("array.init.mark.ready");
            builder_.br(symbols_.value(*n.initialization_guard),ready_label,mark_label);
            builder_.block(mark_label);
            builder_.call(runtime_abi::memory::init_mark_range,{{ptr,slot},{i64,stride}});
            builder_.br(ready_label);
            builder_.block(ready_label);
        }else{
            builder_.call(runtime_abi::memory::init_mark_range,{{ptr,slot},{i64,stride}});
        }
    }
}

void AggregateEmitter::emit(const ir::VariantMake& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.container_type);
    union_box_.allocate(symbols_.value(n.out),n.container_type);
    union_box_.store_tag(symbols_.value(n.out),n.tag);
    if(n.payload_type.kind!=TypeKind::Void&&n.payload_type.kind!=TypeKind::None){
        auto p="%variant.payload.ptr."+std::to_string(n.out);
        union_box_.payload_slot(p,symbols_.value(n.out));
        builder_.store({llvm_type(n.payload_type),symbols_.value(n.payload)},p,Align::none);
    }
}

void AggregateEmitter::emit(const ir::VariantTag& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Int64));
    builder_.load(symbols_.value(n.out),i64,symbols_.value(n.container),Align::none);
}

void AggregateEmitter::emit(const ir::VariantPayload& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.payload_type);
    if(n.payload_type.kind!=TypeKind::Void&&n.payload_type.kind!=TypeKind::None){
        auto p="%variant.read.ptr."+std::to_string(n.out);
        union_box_.payload_slot(p,symbols_.value(n.container));
        builder_.load(symbols_.value(n.out),llvm_type(n.payload_type),p,Align::none);
    }
}

} // namespace quidra::llvm_backend
