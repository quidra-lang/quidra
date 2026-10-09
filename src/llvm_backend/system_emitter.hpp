#pragma once

// SystemEmitter: the program's environment in a function, through the
// runtime: command-line arguments, optional arguments, options and flags
// (each parsed to its declared type) and the end of command-line parsing;
// environment variables; the clock (now, time since, seconds, sleep); and
// running a process or a shell command. An environment variable is a
// T | none union box. An optional argument and an option are optional
// command-line values (OptionalCliValue, system_emitter.cpp): the default
// when the command line does not give the value, else its text parsed to the
// declared type (parse_cli_text, which a required argument uses too).
//
// Owns no state.

#include "quidra/ir/module.hpp"
#include "llvm_backend/fail_fast_emitter.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_backend/union_box.hpp"
#include "llvm_text/llvm_builder.hpp"

#include <string>

namespace quidra::llvm_backend {

struct OptionalCliValue;

class SystemEmitter {
public:
    SystemEmitter(llvm_text::LlvmBuilder& builder,SymbolTable& symbols,TemporaryNames& names,FailFastEmitter& fail_fast,UnionBox& union_box)
        :builder_(builder),symbols_(symbols),names_(names),fail_fast_(fail_fast),union_box_(union_box){}

    void emit(const ir::CliArgument& n,const ir::Instruction& ins);
    void emit(const ir::CliArgumentOptional& n,const ir::Instruction& ins);
    void emit(const ir::CliOption& n,const ir::Instruction& ins);
    void emit(const ir::CliFlag& n,const ir::Instruction& ins);
    void emit(const ir::CliFinish& n,const ir::Instruction& ins);
    void emit(const ir::EnvironmentGet& n,const ir::Instruction& ins);
    void emit(const ir::EnvironmentHas& n,const ir::Instruction& ins);
    void emit(const ir::TimeNow& n,const ir::Instruction& ins);
    void emit(const ir::TimeSince& n,const ir::Instruction& ins);
    void emit(const ir::TimeSeconds& n,const ir::Instruction& ins);
    void emit(const ir::TimeSleep& n,const ir::Instruction& ins);
    void emit(const ir::ProcessRun& n,const ir::Instruction& ins);
    void emit(const ir::ProcessShell& n,const ir::Instruction& ins);

private:
    // The runtime's parse of a command-line text (raw) to a value of an int,
    // float, bigint, bigreal or bool type; nothing for any other type.
    void parse_cli_text(const std::string& result,const Type& type,const std::string& raw);
    void emit_optional_value(const OptionalCliValue& value,ir::ValueId result,ir::ValueId key,
                             ir::ValueId default_value,const Type& type);

    llvm_text::LlvmBuilder& builder_;
    SymbolTable& symbols_;
    TemporaryNames& names_;
    FailFastEmitter& fail_fast_;
    UnionBox& union_box_;
};

} // namespace quidra::llvm_backend
