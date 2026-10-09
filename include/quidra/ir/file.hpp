#pragma once

// Typed-IR instructions of the file domain (files and file handles).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

namespace quidra::ir {

struct FileOpen { ValueId out; ValueId path; Type result_type; };
struct FileCreate { ValueId out; ValueId path; Type result_type; };
struct FileAppend { ValueId out; ValueId path; Type result_type; };
struct FileHandleRead { ValueId out; ValueId handle; Type result_type; };
struct FileHandleReadLine { ValueId out; ValueId handle; Type result_type; };
struct FileHandleReadBin { ValueId out; ValueId handle; Type result_type; };
struct FileHandleWrite { ValueId out; ValueId handle; ValueId text; Type result_type; bool line{}; };
struct FileHandleFlush { ValueId out; ValueId handle; Type result_type; };
struct FileHandleSeek { ValueId out; ValueId handle; ValueId position; Type result_type; };
struct FileHandleClose { ValueId handle; };
struct FileRead { ValueId out; ValueId path; Type result_type; };
struct FileReadBin { ValueId out; ValueId path; Type result_type; };
struct FileWrite { ValueId out; ValueId path; ValueId text; Type result_type; };
struct FileWriteBin { ValueId out; ValueId path; ValueId bin; Type result_type; };
struct FileExists { ValueId out; ValueId path; Type result_type; };
struct FileIsDirectory { ValueId out; ValueId path; Type result_type; };
struct FileRemove { ValueId out; ValueId path; Type result_type; };
struct FileCopy { ValueId out; ValueId source; ValueId destination; Type result_type; };
struct FileMove { ValueId out; ValueId source; ValueId destination; Type result_type; };
struct FileMkdir { ValueId out; ValueId path; Type result_type; };
struct FileList { ValueId out; ValueId path; ValueId recursive; Type result_type; };

template <> struct InstructionTraits<FileOpen> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileCreate> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileAppend> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileHandleRead> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileHandleReadLine> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileHandleReadBin> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileHandleWrite> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileHandleFlush> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileHandleSeek> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileHandleClose> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileRead> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileReadBin> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileWrite> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileWriteBin> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileExists> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileIsDirectory> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileRemove> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileCopy> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileMove> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileMkdir> : InDomain<Domain::file> {};
template <> struct InstructionTraits<FileList> : InDomain<Domain::file> {};

} // namespace quidra::ir
