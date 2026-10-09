#include "llvm_backend/console_emitter.hpp"

#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "quidra/abi/exact_codes.hpp"
#include "quidra/abi/io_status.hpp"

#include <string>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void ConsoleEmitter::reserve(const ir::Input&,const ir::Instruction& ins){
    scratch_.reserve(ins,ptr,Align::none);
}

void ConsoleEmitter::emit(const ir::Flush& n,const ir::Instruction&){
    const auto status=names_.value("flush.status");
    builder_.call(status,runtime_abi::console::flush,{});
    errors_.output_result(n.out,n.result_type,status);
}

void ConsoleEmitter::emit(const ir::Print& n,const ir::Instruction&){
    const auto replay=names_.value("repl.output.replay");
    const auto emit_label=names_.label("repl.output.emit");
    const auto done_label=names_.label("repl.output.done");
    builder_.load(replay,i1,"@.quidra.repl.replaying",Align::none);
    builder_.br(replay,done_label,emit_label);
    builder_.block(emit_label);
    if(is_fixed_integer(n.type)){
        std::string widened=symbols_.value(n.value);
        if(integer_width(n.type)<64){
            widened=names_.value("print.int");
            builder_.cast(widened,is_signed_integer(n.type)?CastOp::sext:CastOp::zext,{llvm_type(n.type),symbols_.value(n.value)},i64);
        }
        builder_.call(runtime_abi::c_library::printf,{{ptr,is_signed_integer(n.type)?"@.fmt.int.raw":"@.fmt.uint.raw"},{i64,widened}});
    }else if(is_bare_integer(n.type)){
        builder_.call(void_type,LlvmOperand::global(int_helper::print),{{bare_integer_type,symbols_.value(n.value)}});
    }else if(n.type.kind==TypeKind::Real){
        auto text=names_.value("print.exact.text");
        builder_.call(text,ptr,LlvmOperand::global(real_helper::text),{{exact_real_type,symbols_.value(n.value)},{i32,abi::exact_real_display_digits}});
        builder_.call(runtime_abi::c_library::printf,{{ptr,"@.fmt.string.raw"},{ptr,text}});
        builder_.call(runtime_abi::memory::managed_release,{{ptr,text},{ptr,"null"}});
    }else if(is_fixed_real(n.type)){
        std::string widened=symbols_.value(n.value);
        if(n.type.kind==TypeKind::Real32){
            widened=names_.value("print.float");
            builder_.cast(widened,CastOp::fpext,{float_type,symbols_.value(n.value)},double_type);
        }
        auto text=names_.value("print.float.text");
        builder_.call(text,runtime_abi::prelude::float_text,{{double_type,widened}});
        builder_.call(runtime_abi::c_library::printf,{{ptr,"@.fmt.string.raw"},{ptr,text}});
        builder_.call(runtime_abi::memory::managed_release,{{ptr,text},{ptr,"null"}});
    }else if(n.type.kind==TypeKind::Bool){
        auto t=names_.value("print.bool");
        builder_.select(t,symbols_.value(n.value),{ptr,"@.bool.true"},{ptr,"@.bool.false"});
        builder_.call(runtime_abi::c_library::printf,{{ptr,"@.fmt.string.raw"},{ptr,t}});
    }else if(n.type.kind==TypeKind::Address){
        builder_.call(runtime_abi::c_library::printf,{{ptr,"@.fmt.address.raw"},{ptr,symbols_.value(n.value)}});
    }else if(n.type.kind==TypeKind::Bin){
        auto text=names_.value("print.bin");
        builder_.call(text,runtime_abi::bin::string,{{ptr,symbols_.value(n.value)}});
        builder_.call(runtime_abi::c_library::printf,{{ptr,"@.fmt.string.raw"},{ptr,text}});
        builder_.call(runtime_abi::memory::managed_release,{{ptr,text},{ptr,"null"}});
    }else{
        builder_.call(runtime_abi::c_library::printf,{{ptr,"@.fmt.string.raw"},{ptr,symbols_.value(n.value)}});
    }
    builder_.br(done_label);
    builder_.block(done_label);
    const auto status=names_.value("output.status");
    builder_.call(status,runtime_abi::console::output_status,{});
    errors_.output_result(n.out,n.result_type,status);
}

void ConsoleEmitter::emit(const ir::Input& n,const ir::Instruction& ins){
    symbols_.set_type(n.out,n.result_type);
    const auto result=symbols_.value(n.out),text_slot=scratch_.instruction_slot(ins),status=names_.value("input.status");
    union_box_.allocate(result,n.result_type);
    builder_.store({ptr,"null"},text_slot,Align::none);
    builder_.call(status,runtime_abi::console::input_read,{{ptr,text_slot}});

    const auto is_ok=names_.value("input.is.ok"),ok_label=names_.label("input.ok");
    const auto non_ok_label=names_.label("input.non.ok"),done_label=names_.label("input.done");
    builder_.icmp(is_ok,IntPredicate::eq,i32,status,abi::read_line_status::line);
    builder_.br(is_ok,ok_label,non_ok_label);

    builder_.block(ok_label);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::String)));
    const auto string_payload=names_.value("input.string.payload"),text=names_.value("input.text");
    union_box_.payload_slot(string_payload,result);
    builder_.load(text,ptr,text_slot,Align::none);
    builder_.store({ptr,text},string_payload,Align::none);
    builder_.br(done_label);

    builder_.block(non_ok_label);
    const auto is_eof=names_.value("input.is.eof"),eof_label=names_.label("input.eof"),error_label=names_.label("input.error");
    builder_.icmp(is_eof,IntPredicate::eq,i32,status,abi::read_line_status::end_of_input);
    builder_.br(is_eof,eof_label,error_label);

    builder_.block(eof_label);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::None)));
    builder_.br(done_label);

    builder_.block(error_label);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::Error)));
    const auto error_payload=names_.value("input.error.payload");
    union_box_.payload_slot(error_payload,result);
    builder_.store({ptr,"@.err.input"},error_payload,Align::none);
    builder_.br(done_label);

    builder_.block(done_label);
}

} // namespace quidra::llvm_backend
