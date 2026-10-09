#include "llvm_backend/system_emitter.hpp"

#include "llvm_backend/copy_idioms.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "quidra/standard_classes.hpp"

#include <string>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

// An optional command-line value: where it is looked up, and the prefix of
// the names its emission allocates.
struct OptionalCliValue {
    const char* prefix;
    // The runtime function that returns the value's text, or null when the
    // command line does not give the value.
    const LlvmCallee* lookup;
    // The LLVM type of the lookup's key: a position or an option name.
    LlvmType key_type;
};

namespace {

constexpr OptionalCliValue optional_cli_argument{"cli.argument.optional",&runtime_abi::system::cli_argument_optional,i64};
constexpr OptionalCliValue cli_option{"cli.option",&runtime_abi::system::cli_option,ptr};

} // namespace

void SystemEmitter::parse_cli_text(const std::string& result,const Type& type,const std::string& raw){
    if(type.kind==TypeKind::Int64) builder_.call(result,runtime_abi::system::cli_parse_int,{{ptr,raw}});
    else if(type.kind==TypeKind::Real64) builder_.call(result,runtime_abi::system::cli_parse_float,{{ptr,raw}});
    else if(is_bare_integer(type)){
        const auto big=names_.value("cli.int.big");
        builder_.call(big,runtime_abi::system::cli_parse_bigint,{{ptr,raw}});
        builder_.call(result,bare_integer_type,LlvmOperand::global(int_helper::adopt),{{ptr,big}});
    }
    else if(type.kind==TypeKind::Real){
        const auto node=names_.value("cli.real.node");
        builder_.call(node,runtime_abi::system::cli_parse_bigreal,{{ptr,raw}});
        builder_.call(result,exact_real_type,LlvmOperand::global(real_helper::adopt_node),{{ptr,node}});
    }
    else if(type.kind==TypeKind::Bool) builder_.call(result,runtime_abi::system::cli_parse_bool,{{ptr,raw}});
}

void SystemEmitter::emit_optional_value(const OptionalCliValue& value,ir::ValueId result,ir::ValueId key,
                                        ir::ValueId default_value,const Type& type){
    symbols_.set_type(result,type);
    const std::string prefix=value.prefix;
    const auto raw=names_.value(prefix+".raw"),missing=names_.value(prefix+".missing");
    builder_.call(raw,*value.lookup,{{value.key_type,symbols_.value(key)}});
    builder_.icmp(missing,IntPredicate::eq,ptr,raw,"null");
    const auto use_default=names_.label(prefix+".default"),parse=names_.label(prefix+".parse"),done=names_.label(prefix+".done");
    builder_.br(missing,use_default,parse);
    builder_.block(use_default);
    if(type.kind==TypeKind::Real)
        builder_.call(void_type,LlvmOperand::global(real_helper::retain),{{exact_real_type,symbols_.value(default_value)}});
    else if(is_bare_integer(type))
        builder_.call(void_type,LlvmOperand::global(int_helper::retain),{{bare_integer_type,symbols_.value(default_value)}});
    else if(type.kind==TypeKind::String)
        builder_.call(runtime_abi::memory::managed_retain,{{ptr,symbols_.value(default_value)}});
    builder_.br(done);
    builder_.block(parse);
    std::string parsed;
    if(type.kind==TypeKind::String) parsed=raw;
    else {
        parsed=names_.value(prefix+".value");
        parse_cli_text(parsed,type,raw);
    }
    builder_.br(done);
    builder_.block(done);
    builder_.phi(symbols_.value(result),llvm_type(type),{{symbols_.value(default_value),use_default},{parsed,parse}});
}

void SystemEmitter::emit(const ir::CliArgument& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    const auto raw=names_.value("cli.argument.raw");
    builder_.call(raw,runtime_abi::system::cli_argument,{{i64,symbols_.value(n.index)}});
    if(n.type.kind==TypeKind::String){
        copy_by_zero_gep(builder_,symbols_.value(n.out),Inbounds::no,raw);
        return;
    }
    parse_cli_text(symbols_.value(n.out),n.type,raw);
}

void SystemEmitter::emit(const ir::CliArgumentOptional& n,const ir::Instruction&){
    emit_optional_value(optional_cli_argument,n.out,n.index,n.default_value,n.type);
}

void SystemEmitter::emit(const ir::CliOption& n,const ir::Instruction&){
    emit_optional_value(cli_option,n.out,n.name,n.default_value,n.type);
}

void SystemEmitter::emit(const ir::CliFlag& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bool));
    builder_.call(symbols_.value(n.out),runtime_abi::system::cli_flag,{{ptr,symbols_.value(n.name)}});
}

void SystemEmitter::emit(const ir::CliFinish&,const ir::Instruction&){
    builder_.call(runtime_abi::system::cli_finish,{});
}

void SystemEmitter::emit(const ir::EnvironmentGet& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.result_type);
    const auto raw=names_.value("environment.get.raw");
    const auto missing=names_.value("environment.get.missing");
    const auto result=symbols_.value(n.out);
    builder_.call(raw,runtime_abi::system::environment_get,{{ptr,symbols_.value(n.name)}});
    union_box_.allocate(result,n.result_type);
    builder_.icmp(missing,IntPredicate::eq,ptr,raw,"null");
    const auto none_label=names_.label("environment.get.none");
    const auto string_label=names_.label("environment.get.string");
    const auto done=names_.label("environment.get.done");
    builder_.br(missing,none_label,string_label);
    builder_.block(none_label);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::None)));
    builder_.br(done);
    builder_.block(string_label);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::String)));
    const auto payload=names_.value("environment.get.payload");
    union_box_.payload_slot(payload,result);
    builder_.store({ptr,raw},payload,Align::none);
    builder_.br(done);
    builder_.block(done);
}

void SystemEmitter::emit(const ir::EnvironmentHas& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bool));
    builder_.call(symbols_.value(n.out),runtime_abi::system::environment_has,{{ptr,symbols_.value(n.name)}});
}

void SystemEmitter::emit(const ir::TimeNow& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::class_type(standard_class::time_instant));
    const auto seconds=names_.value("time.now.seconds");
    builder_.call(seconds,runtime_abi::system::time_now,{{i1,symbols_.value(n.sync)}});
    builder_.call(symbols_.value(n.out),runtime_abi::prelude::alloc,{{i64,8}});
    builder_.store({double_type,seconds},symbols_.value(n.out),Align::one);
}

void SystemEmitter::emit(const ir::TimeSince& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::class_type(standard_class::time_duration));
    const auto start=names_.value("time.since.start"),now=names_.value("time.since.now"),elapsed=names_.value("time.since.elapsed");
    builder_.load(start,double_type,symbols_.value(n.start),Align::one);
    builder_.call(now,runtime_abi::system::time_now,{{i1,symbols_.value(n.sync)}});
    builder_.binary(elapsed,BinaryOp::fsub,double_type,now,start);
    builder_.call(symbols_.value(n.out),runtime_abi::prelude::alloc,{{i64,8}});
    builder_.store({double_type,elapsed},symbols_.value(n.out),Align::one);
}

void SystemEmitter::emit(const ir::TimeSeconds& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::class_type(standard_class::time_duration));
    builder_.call(symbols_.value(n.out),runtime_abi::prelude::alloc,{{i64,8}});
    builder_.store({double_type,symbols_.value(n.seconds)},symbols_.value(n.out),Align::one);
}

void SystemEmitter::emit(const ir::TimeSleep& n,const ir::Instruction&){
    const auto seconds=names_.value("time.sleep.seconds"),ok=names_.value("time.sleep.ok"),bad=names_.value("time.sleep.bad");
    builder_.load(seconds,double_type,symbols_.value(n.duration),Align::one);
    builder_.call(ok,runtime_abi::system::time_sleep,{{double_type,seconds}});
    builder_.binary(bad,BinaryOp::xor_,i1,ok,"true");
    fail_fast_.fail_if(bad,"@.code.time.sleep","@.msg.time.sleep","time.sleep",n.line,n.column);
}

void SystemEmitter::emit(const ir::ProcessRun& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::class_type(standard_class::process_result));
    builder_.call(symbols_.value(n.out),runtime_abi::process::run,{{ptr,symbols_.value(n.program)},{ptr,symbols_.value(n.args)}});
}

void SystemEmitter::emit(const ir::ProcessShell& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::class_type(standard_class::process_result));
    builder_.call(symbols_.value(n.out),runtime_abi::process::shell,{{ptr,symbols_.value(n.command)}});
}

} // namespace quidra::llvm_backend
