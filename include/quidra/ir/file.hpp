#pragma once

// Typed-IR instructions of the file domain (files and file handles).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/debug.hpp"
#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstdint>

namespace quidra::ir {

struct FileOpen { ValueId out; ValueId path; Type result_type; SourcePosition position{}; };
struct FileCreate { ValueId out; ValueId path; Type result_type; SourcePosition position{}; };
struct FileAppend { ValueId out; ValueId path; Type result_type; SourcePosition position{}; };
struct FileHandleRead { ValueId out; ValueId handle; Type result_type; SourcePosition position{}; };
struct FileHandleReadLine { ValueId out; ValueId handle; Type result_type; SourcePosition position{}; };
struct FileHandleReadBin { ValueId out; ValueId handle; Type result_type; SourcePosition position{}; };
struct FileHandleWrite { ValueId out; ValueId handle; ValueId text; Type result_type; bool newline{}; SourcePosition position{}; };
struct FileHandleFlush { ValueId out; ValueId handle; Type result_type; SourcePosition position{}; };
struct FileHandleSeek { ValueId out; ValueId handle; ValueId offset; Type result_type; SourcePosition position{}; };
struct FileHandleClose { ValueId handle; };
struct FileRead { ValueId out; ValueId path; Type result_type; SourcePosition position{}; };
struct FileReadBin { ValueId out; ValueId path; Type result_type; SourcePosition position{}; };
struct FileWrite { ValueId out; ValueId path; ValueId text; Type result_type; SourcePosition position{}; };
struct FileWriteBin { ValueId out; ValueId path; ValueId bin; Type result_type; SourcePosition position{}; };
struct FileExists { ValueId out; ValueId path; Type result_type; SourcePosition position{}; };
struct FileIsDirectory { ValueId out; ValueId path; Type result_type; SourcePosition position{}; };
struct FileRemove { ValueId out; ValueId path; Type result_type; SourcePosition position{}; };
struct FileCopy { ValueId out; ValueId source; ValueId destination; Type result_type; SourcePosition position{}; };
struct FileMove { ValueId out; ValueId source; ValueId destination; Type result_type; SourcePosition position{}; };
struct FileMkdir { ValueId out; ValueId path; Type result_type; SourcePosition position{}; };
struct FileList { ValueId out; ValueId path; ValueId recursive; Type result_type; SourcePosition position{}; };

template <> struct InstructionTraits<FileOpen> : WithOperands<Domain::file, FileOpen> {};
template <> struct InstructionTraits<FileCreate> : WithOperands<Domain::file, FileCreate> {};
template <> struct InstructionTraits<FileAppend> : WithOperands<Domain::file, FileAppend> {};
template <> struct InstructionTraits<FileHandleRead> : WithOperands<Domain::file, FileHandleRead> {};
template <> struct InstructionTraits<FileHandleReadLine> : WithOperands<Domain::file, FileHandleReadLine> {};
template <> struct InstructionTraits<FileHandleReadBin> : WithOperands<Domain::file, FileHandleReadBin> {};
template <> struct InstructionTraits<FileHandleWrite> : WithOperands<Domain::file, FileHandleWrite> {};
template <> struct InstructionTraits<FileHandleFlush> : WithOperands<Domain::file, FileHandleFlush> {};
template <> struct InstructionTraits<FileHandleSeek> : WithOperands<Domain::file, FileHandleSeek> {};
template <> struct InstructionTraits<FileHandleClose> : WithOperands<Domain::file, FileHandleClose> {};
template <> struct InstructionTraits<FileRead> : WithOperands<Domain::file, FileRead> {};
template <> struct InstructionTraits<FileReadBin> : WithOperands<Domain::file, FileReadBin> {};
template <> struct InstructionTraits<FileWrite> : WithOperands<Domain::file, FileWrite> {};
template <> struct InstructionTraits<FileWriteBin> : WithOperands<Domain::file, FileWriteBin> {};
template <> struct InstructionTraits<FileExists> : WithOperands<Domain::file, FileExists> {};
template <> struct InstructionTraits<FileIsDirectory> : WithOperands<Domain::file, FileIsDirectory> {};
template <> struct InstructionTraits<FileRemove> : WithOperands<Domain::file, FileRemove> {};
template <> struct InstructionTraits<FileCopy> : WithOperands<Domain::file, FileCopy> {};
template <> struct InstructionTraits<FileMove> : WithOperands<Domain::file, FileMove> {};
template <> struct InstructionTraits<FileMkdir> : WithOperands<Domain::file, FileMkdir> {};
template <> struct InstructionTraits<FileList> : WithOperands<Domain::file, FileList> {};

} // namespace quidra::ir
