#include "llvm_backend/repl_value_printer.hpp"

#include "llvm_backend/copy_idioms.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "quidra/abi/exact_codes.hpp"
#include "quidra/abi/layout.hpp"
#include "quidra/ir/initialization_mask.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void ReplValuePrinter::print_text(const std::string& text) {
    const auto literal = pool_.intern(text);
    builder_.call(runtime_abi::c_library::printf,
                  {{ptr, "@.fmt.string.raw"}, {ptr, LlvmOperand::global(literal)}});
}

bool ReplValuePrinter::path_initialized(const std::vector<std::string>& initialized_paths,
                                        const std::string& path) const {
    return std::find(initialized_paths.begin(), initialized_paths.end(), path) !=
           initialized_paths.end();
}

void ReplValuePrinter::print_value(const Type& type, const std::string& raw_value,
                                   const std::vector<std::string>& initialized_paths,
                                   const std::string& path_prefix) {
    if (is_fixed_integer(type)) {
        std::string widened = raw_value;
        if (integer_width(type) < 64) {
            widened = names_.value("repl.int");
            builder_.cast(widened, is_signed_integer(type) ? CastOp::sext : CastOp::zext,
                          {llvm_type(type), raw_value}, i64);
        }
        builder_.call(runtime_abi::c_library::printf,
                      {{ptr, is_signed_integer(type) ? "@.fmt.int.raw" : "@.fmt.uint.raw"}, {i64, widened}});
        return;
    }
    if (is_bare_integer(type) || type.kind == TypeKind::Real) {
        const auto text = names_.value("repl.exact.text");
        if (is_bare_integer(type))
            builder_.call(text,ptr,LlvmOperand::global(int_helper::text),{{bare_integer_type,raw_value}});
        else
            builder_.call(text,ptr,LlvmOperand::global(real_helper::text),{{exact_real_type,raw_value},{i32,abi::exact_real_display_digits}});
        builder_.call(runtime_abi::c_library::printf, {{ptr, "@.fmt.string.raw"}, {ptr, text}});
        builder_.call(runtime_abi::memory::managed_release,{{ptr,text},{ptr,"null"}});
        return;
    }
    if (is_fixed_real(type)) {
        std::string widened = raw_value;
        if (type.kind == TypeKind::Real32) {
            widened = names_.value("repl.float");
            builder_.cast(widened,CastOp::fpext,{float_type,raw_value},double_type);
        }
        const auto text = names_.value("repl.float.text");
        builder_.call(text,runtime_abi::prelude::float_text,{{double_type,widened}});
        builder_.call(runtime_abi::c_library::printf, {{ptr, "@.fmt.string.raw"}, {ptr, text}});
        builder_.call(runtime_abi::memory::managed_release,{{ptr,text},{ptr,"null"}});
        return;
    }
    if (type.kind == TypeKind::Bool) {
        const auto text = names_.value("repl.bool");
        builder_.select(text,raw_value,{ptr,"@.bool.true"},{ptr,"@.bool.false"});
        builder_.call(runtime_abi::c_library::printf, {{ptr, "@.fmt.string.raw"}, {ptr, text}});
        return;
    }
    if (type.kind == TypeKind::Address) {
        builder_.call(runtime_abi::c_library::printf, {{ptr, "@.fmt.address.raw"}, {ptr, raw_value}});
        return;
    }
    if (type.kind == TypeKind::String) {
        builder_.call(runtime_abi::c_library::printf, {{ptr, "@.fmt.repl.string"}, {ptr, raw_value}});
        return;
    }
    if (type.kind == TypeKind::Error) {
        builder_.call(runtime_abi::c_library::printf, {{ptr, "@.fmt.repl.error"}, {ptr, raw_value}});
        return;
    }
    if (type.kind == TypeKind::None) {
        print_text("none");
        return;
    }
    if (type.kind == TypeKind::Bin) {
        const auto text = names_.value("repl.bin");
        builder_.call(text,runtime_abi::bin::string,{{ptr,raw_value}});
        builder_.call(runtime_abi::c_library::printf, {{ptr, "@.fmt.string.raw"}, {ptr, text}});
        builder_.call(runtime_abi::memory::managed_release,{{ptr,text},{ptr,"null"}});
        return;
    }
    if (type.kind == TypeKind::Array) {
        print_text("[");
        const bool fixed=is_fixed_array(type);
        const auto len = names_.value("repl.array.len");
        if(array_index_slot_.empty())
            throw std::logic_error("REPL array display requires planned scratch storage");
        const auto& index_slot = array_index_slot_;
        const auto cond = names_.label("repl.array.cond");
        const auto body = names_.label("repl.array.body");
        const auto done = names_.label("repl.array.done");
        const auto comma = names_.label("repl.array.comma");
        const auto item = names_.label("repl.array.item");
        if(fixed) copy_by_leading_zero_add(builder_,len,i64,type.length);
        else builder_.load(len,i64,raw_value,Align::one);
        builder_.store({i64,0},index_slot,Align::none);
        builder_.br(cond);
        builder_.block(cond);
        const auto index = names_.value("repl.array.index");
        const auto more = names_.value("repl.array.more");
        builder_.load(index,i64,index_slot,Align::none);
        builder_.icmp(more,IntPredicate::slt,i64,index,len);
        builder_.br(more,body,done);
        builder_.block(body);
        const auto first = names_.value("repl.array.first");
        builder_.icmp(first,IntPredicate::eq,i64,index,0);
        builder_.br(first,item,comma);
        builder_.block(comma);
        print_text(", ");
        builder_.br(item);
        builder_.block(item);
        const auto stride = array_element_stride(type,array_layout_);
        const auto byte_index = names_.value("repl.array.byte.index");
        const auto offset = names_.value("repl.array.offset");
        const auto element_ptr = names_.value("repl.array.ptr");
        const auto element = names_.value("repl.array.value");
        builder_.binary(byte_index,BinaryOp::mul,i64,index,stride);
        if(fixed) copy_by_add_zero(builder_,offset,i64,byte_index);
        else builder_.binary(offset,BinaryOp::add,i64,byte_index,abi::array_layout::payload_offset);
        builder_.getelementptr(element_ptr,Inbounds::yes,i8,raw_value,{{i64,offset}});
        if(fixed&&array_layout_.inline_fixed_child(type)){
            copy_by_zero_gep(builder_,element,Inbounds::yes,element_ptr);
        }else{
            builder_.load(element,llvm_type(*type.first),element_ptr,Align::one);
        }
        print_value(*type.first, element);
        const auto next = names_.value("repl.array.next");
        builder_.binary(next,BinaryOp::add,i64,index,1);
        builder_.store({i64,next},index_slot,Align::none);
        builder_.br(cond);
        builder_.block(done);
        print_text("]");
        return;
    }
    if (type.kind == TypeKind::Class) {
        const auto& layout = layouts_.at(type.class_name);
        print_text(type.class_name + "(");
        const auto shown = ir::source_field_count(layout);
        for (std::size_t i = 0; i < shown; ++i) {
            if (i) print_text(", ");
            print_text(layout.field_names[i] + " = ");
            const auto path = path_prefix.empty()
                ? layout.field_names[i]
                : path_prefix + "." + layout.field_names[i];
            if (!path_initialized(initialized_paths, path)) {
                print_text("<uninitialized>");
                continue;
            }
            const auto field_ptr = names_.value("repl.class.field.ptr");
            const auto field_value = names_.value("repl.class.field.value");
            builder_.getelementptr(field_ptr,Inbounds::yes,i8,raw_value,{{i64,class_field_offset(layout, i)}});
            builder_.load(field_value,llvm_type(layout.fields[i]),field_ptr,Align::one);
            print_value(layout.fields[i], field_value, initialized_paths, path);
        }
        print_text(")");
        return;
    }
    if (type.kind == TypeKind::Union) {
        const auto tag = names_.value("repl.union.tag");
        const auto done = names_.label("repl.union.done");
        const auto invalid = names_.label("repl.union.invalid");
        std::vector<std::string> cases;
        cases.reserve(type.cases.size());
        builder_.load(tag,i64,raw_value,Align::one);
        for (std::size_t i = 0; i < type.cases.size(); ++i)
            cases.push_back(names_.label("repl.union.case"));
        std::vector<LlvmCase> switch_cases;
        switch_cases.reserve(type.cases.size());
        for (std::size_t i = 0; i < type.cases.size(); ++i)
            switch_cases.push_back(LlvmCase{i, cases[i]});
        builder_.switch_on({i64, tag}, invalid, switch_cases);
        for (std::size_t i = 0; i < type.cases.size(); ++i) {
            builder_.block(cases[i]);
            const auto& item_type = type.cases[i];
            if (item_type.kind == TypeKind::None || item_type.kind == TypeKind::Void) {
                print_value(item_type, {});
            } else {
                const auto payload_ptr = names_.value("repl.union.payload.ptr");
                const auto payload = names_.value("repl.union.payload");
                union_box_.payload_slot(payload_ptr, raw_value);
                builder_.load(payload,llvm_type(item_type),payload_ptr,Align::one);
                print_value(item_type, payload);
            }
            builder_.br(done);
        }
        builder_.block(invalid);
        print_text("<invalid union>");
        builder_.br(done);
        builder_.block(done);
        return;
    }

    print_text("<" + type_name(type) + ">");
}

} // namespace quidra::llvm_backend
