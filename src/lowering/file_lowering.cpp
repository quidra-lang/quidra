// File lowering: the methods of a file.Handle (read, read_line, read_bin,
// write, write_line, flush, seek, close). The handle is lowered once,
// before the method is chosen, and an owned handle is released after the
// call. Also the file builtins (file.open, file.read, file.write,
// file.list, file.exists, file.copy, ...).

#include "lowering/lowerer.hpp"
#include "quidra/standard_classes.hpp"
#include <optional>

namespace quidra::lowering {

std::optional<ValueId> FileLowering::try_file_handle_method(
    const Expr& e, const MethodCallExpr& n, const Type& receiver_type) {
    if(receiver_type.kind==TypeKind::Class &&
       receiver_type.class_name==standard_class::file_handle){
        auto handle=lowerer_.lower(*n.receiver);
        const bool owned=lifetime_.expression_owns_result(*n.receiver);
        const auto finish=[&](ValueId result){
            if(owned) builder_.emit(Release{handle,receiver_type});
            return result;
        };
        if(n.method=="read"){
            auto out=builder_.fresh();
            builder_.emit(FileHandleRead{out,handle,checked_.raw_types.at(&e)});
            return finish(out);
        }
        if(n.method=="read_line"){
            auto out=builder_.fresh();
            builder_.emit(FileHandleReadLine{out,handle,checked_.raw_types.at(&e)});
            return finish(out);
        }
        if(n.method=="read_bin"){
            auto out=builder_.fresh();
            builder_.emit(FileHandleReadBin{out,handle,checked_.raw_types.at(&e)});
            return finish(out);
        }
        if(n.method=="write" || n.method=="write_line"){
            auto text=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
            builder_.emit(FileHandleWrite{out,handle,text,checked_.raw_types.at(&e),n.method=="write_line"});
            lifetime_.release_temporary(*n.args[0].value,text);
            return finish(out);
        }
        if(n.method=="flush"){
            auto out=builder_.fresh();
            builder_.emit(FileHandleFlush{out,handle,checked_.raw_types.at(&e)});
            return finish(out);
        }
        if(n.method=="seek"){
            auto position=lowerer_.lower_int64(*n.args[0].value),out=builder_.fresh();
            builder_.emit(FileHandleSeek{out,handle,position,checked_.raw_types.at(&e)});
            return finish(out);
        }
        if(n.method=="close"){
            builder_.emit(FileHandleClose{handle});
            return finish(0);
        }
    }
    return std::nullopt;
}

ValueId FileLowering::lower_file_open(
    const Expr& e, const CallExpr& n, const CallResolution& resolution) {
    auto path=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    if(*resolution.builtin==BuiltinCallable::FileOpen)
        builder_.emit(FileOpen{out,path,checked_.raw_types.at(&e)});
    else if(*resolution.builtin==BuiltinCallable::FileCreate)
        builder_.emit(FileCreate{out,path,checked_.raw_types.at(&e)});
    else
        builder_.emit(FileAppend{out,path,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,path);
    return out;
}

ValueId FileLowering::lower_file_read(const Expr& e, const CallExpr& n) {
    auto path=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(FileRead{out,path,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,path);
    return out;
}

ValueId FileLowering::lower_file_read_bin(const Expr& e, const CallExpr& n) {
    auto path=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(FileReadBin{out,path,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,path);
    return out;
}

ValueId FileLowering::lower_file_list(const Expr& e, const CallExpr& n) {
    auto path=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    ValueId recursive=n.args.size()==2?lowerer_.lower(*n.args[1].value):builder_.const_bool(false);
    builder_.emit(FileList{out,path,recursive,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,path);
    return out;
}

ValueId FileLowering::lower_file_write(const Expr& e, const CallExpr& n) {
    auto path=lowerer_.lower(*n.args[0].value),text=lowerer_.lower(*n.args[1].value),out=builder_.fresh();
    builder_.emit(FileWrite{out,path,text,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,path);
    lifetime_.release_temporary(*n.args[1].value,text);
    return out;
}

ValueId FileLowering::lower_file_write_bin(const Expr& e, const CallExpr& n) {
    auto path=lowerer_.lower(*n.args[0].value),bin=lowerer_.lower(*n.args[1].value),out=builder_.fresh();
    builder_.emit(FileWriteBin{out,path,bin,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,path);
    lifetime_.release_temporary(*n.args[1].value,bin);
    return out;
}

ValueId FileLowering::lower_file_exists(const Expr& e, const CallExpr& n) {
    auto path=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(FileExists{out,path,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,path);
    return out;
}

ValueId FileLowering::lower_file_is_directory(const Expr& e, const CallExpr& n) {
    auto path=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(FileIsDirectory{out,path,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,path);
    return out;
}

ValueId FileLowering::lower_file_remove(const Expr& e, const CallExpr& n) {
    auto path=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(FileRemove{out,path,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,path);
    return out;
}

ValueId FileLowering::lower_file_copy(const Expr& e, const CallExpr& n) {
    auto source=lowerer_.lower(*n.args[0].value),destination=lowerer_.lower(*n.args[1].value),out=builder_.fresh();
    builder_.emit(FileCopy{out,source,destination,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,source);
    lifetime_.release_temporary(*n.args[1].value,destination);
    return out;
}

ValueId FileLowering::lower_file_move(const Expr& e, const CallExpr& n) {
    auto source=lowerer_.lower(*n.args[0].value),destination=lowerer_.lower(*n.args[1].value),out=builder_.fresh();
    builder_.emit(FileMove{out,source,destination,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,source);
    lifetime_.release_temporary(*n.args[1].value,destination);
    return out;
}

ValueId FileLowering::lower_file_mkdir(const Expr& e, const CallExpr& n) {
    auto path=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(FileMkdir{out,path,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,path);
    return out;
}
} // namespace quidra::lowering
