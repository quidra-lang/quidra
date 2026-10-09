#pragma once

// FileEmitter: files and file handles of a function, through the runtime's
// raw file functions: opening, creating and appending (a File.Handle),
// reading, writing, flushing, seeking and closing a handle, reading and
// writing whole files as text or bins, path queries, and removing, copying,
// moving, creating and listing paths. Each operation that can fail gives a
// recoverable result (ErrorResultEmitter, or a union box built in place):
// void | error from a success flag, string | error from a string that is
// null on failure, or T | error.
//
// A file is opened in one of three modes (FileOpenMode): to read it (open),
// to write it from empty (create) or to write at its end (append); each mode
// has its runtime function, quidra_file_<mode>_raw, which returns null on
// failure, and the result is a File.Handle | error. A path is queried with
// a predicate (PathPredicate): whether it exists or is a directory; its
// runtime function, quidra_file_<predicate>_raw, returns 1 or 0, or a
// negative status on failure, and the result is a bool | error. A whole
// bin is read (BinRead) from a path or from an open handle into a
// bin | error.
//
// Owns no state. FileHandleReadLine reads through the entry-frame scratch
// slot the function's pre-pass reserved for it.

#include "quidra/ir/module.hpp"
#include "llvm_backend/error_result_emitter.hpp"
#include "llvm_backend/scratch_planner.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_backend/union_box.hpp"
#include "llvm_text/llvm_builder.hpp"

namespace quidra::llvm_backend {

enum class FileOpenMode { open, create, append };
enum class PathPredicate { exists, is_directory };
struct BinRead;

class FileEmitter {
public:
    FileEmitter(llvm_text::LlvmBuilder& builder,SymbolTable& symbols,ScratchPlanner& scratch,TemporaryNames& names,
                ErrorResultEmitter& errors,UnionBox& union_box)
        :builder_(builder),symbols_(symbols),scratch_(scratch),names_(names),errors_(errors),union_box_(union_box){}

    // Before any text of the function is written (FunctionPrepass): what
    // the instruction needs reserved.
    void reserve(const ir::FileHandleReadLine& n,const ir::Instruction& ins);

    void emit(const ir::FileOpen& n,const ir::Instruction& ins);
    void emit(const ir::FileCreate& n,const ir::Instruction& ins);
    void emit(const ir::FileAppend& n,const ir::Instruction& ins);
    void emit(const ir::FileHandleRead& n,const ir::Instruction& ins);
    void emit(const ir::FileHandleReadLine& n,const ir::Instruction& ins);
    void emit(const ir::FileHandleReadBin& n,const ir::Instruction& ins);
    void emit(const ir::FileHandleWrite& n,const ir::Instruction& ins);
    void emit(const ir::FileHandleFlush& n,const ir::Instruction& ins);
    void emit(const ir::FileHandleSeek& n,const ir::Instruction& ins);
    void emit(const ir::FileHandleClose& n,const ir::Instruction& ins);
    void emit(const ir::FileRead& n,const ir::Instruction& ins);
    void emit(const ir::FileReadBin& n,const ir::Instruction& ins);
    void emit(const ir::FileWrite& n,const ir::Instruction& ins);
    void emit(const ir::FileWriteBin& n,const ir::Instruction& ins);
    void emit(const ir::FileExists& n,const ir::Instruction& ins);
    void emit(const ir::FileIsDirectory& n,const ir::Instruction& ins);
    void emit(const ir::FileRemove& n,const ir::Instruction& ins);
    void emit(const ir::FileCopy& n,const ir::Instruction& ins);
    void emit(const ir::FileMove& n,const ir::Instruction& ins);
    void emit(const ir::FileMkdir& n,const ir::Instruction& ins);
    void emit(const ir::FileList& n,const ir::Instruction& ins);

private:
    void emit_open(FileOpenMode mode,ir::ValueId result_value,ir::ValueId path,const Type& result_type);
    void emit_path_query(PathPredicate predicate,ir::ValueId result_value,ir::ValueId path,const Type& result_type);
    void emit_bin_read(const BinRead& source,ir::ValueId result_value,ir::ValueId from,const Type& result_type);

    llvm_text::LlvmBuilder& builder_;
    SymbolTable& symbols_;
    ScratchPlanner& scratch_;
    TemporaryNames& names_;
    ErrorResultEmitter& errors_;
    UnionBox& union_box_;
};

} // namespace quidra::llvm_backend
