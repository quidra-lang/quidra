#include "llvm_backend/file_emitter.hpp"

#include "llvm_backend/runtime_abi.hpp"
#include "quidra/abi/io_status.hpp"
#include "quidra/standard_classes.hpp"

#include <string>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void FileEmitter::reserve(const ir::FileHandleReadLine&,const ir::Instruction& ins){
    scratch_.reserve(ins,ptr,Align::none);
}

namespace {

// The runtime function that opens a file in a mode.
const LlvmCallee& file_open_mode_function(FileOpenMode mode){
    switch(mode){
        case FileOpenMode::open: return runtime_abi::file::open_raw;
        case FileOpenMode::create: return runtime_abi::file::create_raw;
        case FileOpenMode::append: break;
    }
    return runtime_abi::file::append_raw;
}

// The word that names a mode in the names.
const char* file_open_mode_word(FileOpenMode mode){
    switch(mode){
        case FileOpenMode::open: return "open";
        case FileOpenMode::create: return "create";
        case FileOpenMode::append: break;
    }
    return "append";
}

} // namespace

void FileEmitter::emit_open(FileOpenMode mode,ir::ValueId result_value,ir::ValueId path,const Type& result_type){
    symbols_.set_type(result_value,result_type);
    const char* const word=file_open_mode_word(mode);
    const std::string prefix=std::string("file.")+word;
    const auto raw=names_.value(prefix+".raw"),ok=names_.value(prefix+".ok"),result=symbols_.value(result_value);
    builder_.call(raw,file_open_mode_function(mode),{{ptr,symbols_.value(path)}});
    union_box_.allocate(result,result_type);
    builder_.icmp(ok,IntPredicate::ne,ptr,raw,"null");
    const auto yes=names_.label(prefix+".ok"),bad=names_.label(prefix+".error"),done=names_.label(prefix+".done");
    builder_.br(ok,yes,bad);
    builder_.block(yes);
    union_box_.store_tag(result,case_index(result_type,Type::class_type(standard_class::file_handle)));
    const auto payload=names_.value(prefix+".handle");
    union_box_.payload_slot(payload,result);
    builder_.store({ptr,raw},payload,Align::none);
    builder_.br(done);
    builder_.block(bad);
    union_box_.store_tag(result,case_index(result_type,Type::simple(TypeKind::Error)));
    const auto error_payload=names_.value(prefix+".error.payload");
    union_box_.payload_slot(error_payload,result);
    builder_.store({ptr,"@.err.file"},error_payload,Align::none);
    builder_.br(done);
    builder_.block(done);
}

void FileEmitter::emit(const ir::FileOpen& n,const ir::Instruction&){
    emit_open(FileOpenMode::open,n.out,n.path,n.result_type);
}

void FileEmitter::emit(const ir::FileCreate& n,const ir::Instruction&){
    emit_open(FileOpenMode::create,n.out,n.path,n.result_type);
}

void FileEmitter::emit(const ir::FileAppend& n,const ir::Instruction&){
    emit_open(FileOpenMode::append,n.out,n.path,n.result_type);
}

void FileEmitter::emit(const ir::FileHandleRead& n,const ir::Instruction&){
    const auto raw=names_.value("file.handle.read.raw");
    builder_.call(raw,runtime_abi::file::handle_read_raw,{{ptr,symbols_.value(n.handle)}});
    errors_.string_result(n.out,n.result_type,raw,"file.handle.read");
}

void FileEmitter::emit(const ir::FileHandleReadLine& n,const ir::Instruction& ins){
    symbols_.set_type(n.out,n.result_type);
    const auto result=symbols_.value(n.out),text_slot=scratch_.instruction_slot(ins),status=names_.value("file.handle.read_line.status");
    union_box_.allocate(result,n.result_type);
    builder_.store({ptr,"null"},text_slot,Align::none);
    builder_.call(status,runtime_abi::file::handle_read_line_raw,{{ptr,symbols_.value(n.handle)},{ptr,text_slot}});
    const auto is_ok=names_.value("file.handle.read_line.is.ok"),ok=names_.label("file.handle.read_line.ok");
    const auto non_ok=names_.label("file.handle.read_line.non.ok"),done=names_.label("file.handle.read_line.done");
    builder_.icmp(is_ok,IntPredicate::eq,i32,status,abi::read_line_status::line);
    builder_.br(is_ok,ok,non_ok);
    builder_.block(ok);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::String)));
    const auto payload=names_.value("file.handle.read_line.payload"),text=names_.value("file.handle.read_line.text");
    union_box_.payload_slot(payload,result);
    builder_.load(text,ptr,text_slot,Align::none);
    builder_.store({ptr,text},payload,Align::none);
    builder_.br(done);
    builder_.block(non_ok);
    const auto is_eof=names_.value("file.handle.read_line.is.eof"),eof=names_.label("file.handle.read_line.eof"),bad=names_.label("file.handle.read_line.error");
    builder_.icmp(is_eof,IntPredicate::eq,i32,status,abi::read_line_status::end_of_input);
    builder_.br(is_eof,eof,bad);
    builder_.block(eof);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::None)));
    builder_.br(done);
    builder_.block(bad);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::Error)));
    const auto error_payload=names_.value("file.handle.read_line.error.payload");
    union_box_.payload_slot(error_payload,result);
    builder_.store({ptr,"@.err.file"},error_payload,Align::none);
    builder_.br(done);
    builder_.block(done);
}

// Where a whole bin is read from (BinRead): the runtime function, which
// returns null on failure, and the prefix of the names.
struct BinRead {
    const LlvmCallee* reader;
    const char* prefix;
};

namespace {

constexpr BinRead bin_read_from_path{&runtime_abi::file::read_bin_raw,"file.read_bin"};
constexpr BinRead bin_read_from_handle{&runtime_abi::file::handle_read_bin_raw,"file.handle.read_bin"};

} // namespace

void FileEmitter::emit_bin_read(const BinRead& source,ir::ValueId result_value,ir::ValueId from,const Type& result_type){
    symbols_.set_type(result_value,result_type);
    const std::string prefix=source.prefix;
    const auto raw=names_.value(prefix+".raw"),present=names_.value(prefix+".present");
    const auto result=symbols_.value(result_value);
    builder_.call(raw,*source.reader,{{ptr,symbols_.value(from)}});
    union_box_.allocate(result,result_type);
    builder_.icmp(present,IntPredicate::ne,ptr,raw,"null");
    const auto ok=names_.label(prefix+".ok"),bad=names_.label(prefix+".error"),done=names_.label(prefix+".done");
    builder_.br(present,ok,bad);
    builder_.block(ok);
    union_box_.store_tag(result,case_index(result_type,Type::simple(TypeKind::Bin)));
    const auto payload=names_.value(prefix+".payload");
    union_box_.payload_slot(payload,result);
    builder_.store({ptr,raw},payload,Align::none);
    builder_.br(done);
    builder_.block(bad);
    union_box_.store_tag(result,case_index(result_type,Type::simple(TypeKind::Error)));
    const auto error_payload=names_.value(prefix+".error.payload");
    union_box_.payload_slot(error_payload,result);
    builder_.store({ptr,"@.err.file"},error_payload,Align::none);
    builder_.br(done);
    builder_.block(done);
}

void FileEmitter::emit(const ir::FileHandleReadBin& n,const ir::Instruction&){
    emit_bin_read(bin_read_from_handle,n.out,n.handle,n.result_type);
}

void FileEmitter::emit(const ir::FileHandleWrite& n,const ir::Instruction&){
    const auto ok=names_.value("file.handle.write.ok");
    builder_.call(ok,runtime_abi::file::handle_write_raw,{{ptr,symbols_.value(n.handle)},{ptr,symbols_.value(n.text)},{i1,n.line?"true":"false"}});
    errors_.void_result(n.out,n.result_type,ok,n.line?"file.handle.write_line":"file.handle.write");
}

void FileEmitter::emit(const ir::FileHandleFlush& n,const ir::Instruction&){
    const auto ok=names_.value("file.handle.flush.ok");
    builder_.call(ok,runtime_abi::file::handle_flush_raw,{{ptr,symbols_.value(n.handle)}});
    errors_.void_result(n.out,n.result_type,ok,"file.handle.flush");
}

void FileEmitter::emit(const ir::FileHandleSeek& n,const ir::Instruction&){
    const auto ok=names_.value("file.handle.seek.ok");
    builder_.call(ok,runtime_abi::file::handle_seek_raw,{{ptr,symbols_.value(n.handle)},{i64,symbols_.value(n.position)}});
    errors_.void_result(n.out,n.result_type,ok,"file.handle.seek");
}

void FileEmitter::emit(const ir::FileHandleClose& n,const ir::Instruction&){
    builder_.call(runtime_abi::file::handle_close,{{ptr,symbols_.value(n.handle)}});
}

void FileEmitter::emit(const ir::FileRead& n,const ir::Instruction&){
    const auto raw=names_.value("file.read.raw");
    builder_.call(raw,runtime_abi::file::read_raw,{{ptr,symbols_.value(n.path)}});
    errors_.string_result(n.out,n.result_type,raw,"file.read");
}

void FileEmitter::emit(const ir::FileReadBin& n,const ir::Instruction&){
    emit_bin_read(bin_read_from_path,n.out,n.path,n.result_type);
}

void FileEmitter::emit(const ir::FileWrite& n,const ir::Instruction&){
    const auto ok=names_.value("file.write.ok");
    builder_.call(ok,runtime_abi::file::write_raw,{{ptr,symbols_.value(n.path)},{ptr,symbols_.value(n.text)}});
    errors_.void_result(n.out,n.result_type,ok,"file.write");
}

void FileEmitter::emit(const ir::FileWriteBin& n,const ir::Instruction&){
    const auto ok=names_.value("file.write_bin.ok");
    builder_.call(ok,runtime_abi::file::write_bin_raw,{{ptr,symbols_.value(n.path)},{ptr,symbols_.value(n.bin)}});
    errors_.void_result(n.out,n.result_type,ok,"file.write_bin");
}

namespace {

// The runtime function that answers a predicate about a path.
const LlvmCallee& path_predicate_function(PathPredicate predicate){
    switch(predicate){
        case PathPredicate::exists: return runtime_abi::file::exists_raw;
        case PathPredicate::is_directory: break;
    }
    return runtime_abi::file::is_directory_raw;
}

// The word that names a predicate in the names.
const char* path_predicate_word(PathPredicate predicate){
    switch(predicate){
        case PathPredicate::exists: return "exists";
        case PathPredicate::is_directory: break;
    }
    return "is_directory";
}

} // namespace

void FileEmitter::emit_path_query(PathPredicate predicate,ir::ValueId result_value,ir::ValueId path,const Type& result_type){
    symbols_.set_type(result_value,result_type);
    const char* const word=path_predicate_word(predicate);
    const std::string prefix=std::string("file.")+word;
    const auto raw=names_.value(prefix+".raw"),is_error=names_.value(prefix+".error"),is_true=names_.value(prefix+".true");
    const auto result=symbols_.value(result_value);
    builder_.call(raw,path_predicate_function(predicate),{{ptr,symbols_.value(path)}});
    union_box_.allocate(result,result_type);
    builder_.icmp(is_error,IntPredicate::slt,i32,raw,0);
    const auto bad=names_.label(prefix+".error"),ok=names_.label(prefix+".ok"),done=names_.label(prefix+".done");
    builder_.br(is_error,bad,ok);
    builder_.block(ok);
    union_box_.store_tag(result,case_index(result_type,Type::simple(TypeKind::Bool)));
    const auto bool_payload=names_.value(prefix+".bool.payload");
    union_box_.payload_slot(bool_payload,result);
    builder_.icmp(is_true,IntPredicate::eq,i32,raw,abi::path_query_status::yes);
    builder_.store({i1,is_true},bool_payload,Align::none);
    builder_.br(done);
    builder_.block(bad);
    union_box_.store_tag(result,case_index(result_type,Type::simple(TypeKind::Error)));
    const auto error_payload=names_.value(prefix+".error.payload");
    union_box_.payload_slot(error_payload,result);
    builder_.store({ptr,"@.err.file"},error_payload,Align::none);
    builder_.br(done);
    builder_.block(done);
}

void FileEmitter::emit(const ir::FileExists& n,const ir::Instruction&){
    emit_path_query(PathPredicate::exists,n.out,n.path,n.result_type);
}

void FileEmitter::emit(const ir::FileIsDirectory& n,const ir::Instruction&){
    emit_path_query(PathPredicate::is_directory,n.out,n.path,n.result_type);
}

void FileEmitter::emit(const ir::FileRemove& n,const ir::Instruction&){
    const auto ok=names_.value("file.remove.ok");
    builder_.call(ok,runtime_abi::file::remove_raw,{{ptr,symbols_.value(n.path)}});
    errors_.void_result(n.out,n.result_type,ok,"file.remove");
}

void FileEmitter::emit(const ir::FileCopy& n,const ir::Instruction&){
    const auto ok=names_.value("file.copy.ok");
    builder_.call(ok,runtime_abi::file::copy_raw,{{ptr,symbols_.value(n.source)},{ptr,symbols_.value(n.destination)}});
    errors_.void_result(n.out,n.result_type,ok,"file.copy");
}

void FileEmitter::emit(const ir::FileMove& n,const ir::Instruction&){
    const auto ok=names_.value("file.move.ok");
    builder_.call(ok,runtime_abi::file::move_raw,{{ptr,symbols_.value(n.source)},{ptr,symbols_.value(n.destination)}});
    errors_.void_result(n.out,n.result_type,ok,"file.move");
}

void FileEmitter::emit(const ir::FileMkdir& n,const ir::Instruction&){
    const auto ok=names_.value("file.mkdir.ok");
    builder_.call(ok,runtime_abi::file::mkdir_raw,{{ptr,symbols_.value(n.path)}});
    errors_.void_result(n.out,n.result_type,ok,"file.mkdir");
}

void FileEmitter::emit(const ir::FileList& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.result_type);
    const auto raw=names_.value("file.list.raw"),oktest=names_.value("file.list.ok");
    const auto result=symbols_.value(n.out);
    const auto list_type=Type::array(Type::simple(TypeKind::String));
    builder_.call(raw,runtime_abi::file::list_raw,{{ptr,symbols_.value(n.path)},{i1,symbols_.value(n.recursive)}});
    union_box_.allocate(result,n.result_type);
    builder_.icmp(oktest,IntPredicate::ne,ptr,raw,"null");
    const auto ok=names_.label("file.list.ok"),bad=names_.label("file.list.error"),done=names_.label("file.list.done");
    builder_.br(oktest,ok,bad);
    builder_.block(ok);
    union_box_.store_tag(result,case_index(n.result_type,list_type));
    const auto list_payload=names_.value("file.list.payload");
    union_box_.payload_slot(list_payload,result);
    builder_.store({ptr,raw},list_payload,Align::none);
    builder_.br(done);
    builder_.block(bad);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::Error)));
    const auto error_payload=names_.value("file.list.error.payload");
    union_box_.payload_slot(error_payload,result);
    builder_.store({ptr,"@.err.file"},error_payload,Align::none);
    builder_.br(done);
    builder_.block(done);
}

} // namespace quidra::llvm_backend
