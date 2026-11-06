// File lowering for mode-typed file.read/file.write/file.append call operators and
// filesystem builtins. The legacy file.Handle paths remain internal lowering
// compatibility code; the corresponding public source API is unavailable.

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include "quidra/abi/error_record.hpp"
#include "quidra/standard_classes.hpp"
#include <optional>

namespace quidra::lowering {

std::optional<ValueId> FileLowering::try_file_handle_method(
    const Expr& e, const MethodCallExpr& n, const Type& receiver_type) {

    if (receiver_type.kind == TypeKind::Class &&
        (receiver_type.class_name == standard_class::file_read ||
         receiver_type.class_name == standard_class::file_write ||
         receiver_type.class_name == standard_class::file_append)) {
        const bool reader = receiver_type.class_name == standard_class::file_read;
        auto handle = lowerer_.lower(*n.receiver);
        const bool owned = lifetime_.expression_owns_result(*n.receiver);
        const auto line = static_cast<std::uint32_t>(e.span.start.line);
        const auto column = static_cast<std::uint32_t>(e.span.start.column);
        const auto result_type = checked_.raw_types.at(&e);
        if (!reader) {
            const auto newline = n.method == "line";
            const auto has_value = !n.args.empty();
            const auto string_type = Type::simple(TypeKind::String);
            Type value_type = string_type;
            ValueId text = 0;
            if (has_value) {
                value_type = type_of(checked_,*n.args[0].value);
                const auto value = lowerer_.lower(*n.args[0].value);
                text = value;
                if (value_type.kind != TypeKind::String) {
                    text = builder_.fresh();
                    builder_.emit(ToString{text,value,value_type});
                    lifetime_.release_temporary(*n.args[0].value,value);
                }
            } else {
                text = builder_.fresh();
                builder_.emit(ConstantString{text,""});
            }
            auto out = builder_.fresh();
            builder_.emit(FileHandleWrite{out,handle,text,result_type,newline,{line,column}});
            if (has_value && value_type.kind == TypeKind::String)
                lifetime_.release_temporary(*n.args[0].value,text);
            else if (has_value && value_type.kind != TypeKind::Bool)
                release_owned(builder_,text,string_type);
            if (owned) release_owned(builder_,handle,receiver_type);
            return out;
        }

        if (checked_.scan_formats.contains(&e)) {
            const auto raw_type=Type::union_of({
                Type::simple(TypeKind::String),Type::simple(TypeKind::None),
                Type::simple(TypeKind::Error)});
            auto raw=builder_.fresh();
            builder_.emit(FileHandleReadLine{raw,handle,raw_type,{line,column}});
            auto out=lowerer_.scan_.lower_scan(e,raw,true);
            if (owned) release_owned(builder_,handle,receiver_type);
            return out;
        }

        auto address = lowerer_.lower_address(*n.args[0].value,true);
        const auto target_type = checked_.raw_types.at(n.args[0].value.get());
        const auto string_type = Type::simple(TypeKind::String);
        const auto none_type = Type::simple(TypeKind::None);
        const auto error_type = Type::simple(TypeKind::Error);
        const auto bool_type = Type::simple(TypeKind::Bool);
        const auto int_type = Type::simple(TypeKind::Int64);
        const auto raw_type = Type::union_of({string_type,none_type,error_type});
        const auto result_name = builder_.hidden("file.operator.result");
        lowerer_.scope_.local_type(result_name) = result_type;
        const auto done = builder_.label("file.operator.done");
        const auto wrap_bool = [&](bool value) {
            auto flag = builder_.const_bool(value);
            auto result = builder_.fresh();
            builder_.emit(VariantMake{result,case_index(result_type,bool_type),flag,result_type,bool_type});
            builder_.emit(StoreLocal{result_name,result,result_type,true});
            builder_.emit(Jump{done});
        };
        const auto wrap_error = [&](ValueId error) {
            auto result = builder_.fresh();
            builder_.emit(VariantMake{result,case_index(result_type,error_type),error,result_type,error_type});
            builder_.emit(StoreLocal{result_name,result,result_type,true});
            builder_.emit(Jump{done});
        };
        const auto propagate_error = [&](ValueId source, const Type& source_type) {
            auto error = builder_.fresh();
            builder_.emit(VariantPayload{error,source,error_type});
            auto kept = lifetime_.copy_value(error,error_type);
            release_owned(builder_,source,source_type);
            wrap_error(kept);
        };
        const auto store_success = [&](ValueId value) {
            builder_.emit(StoreAddress{address,value,target_type});
            wrap_bool(true);
        };

        auto raw = builder_.fresh();
        builder_.emit(FileHandleReadLine{raw,handle,raw_type,{line,column}});
        auto tag = builder_.fresh();
        builder_.emit(VariantTag{tag,raw});
        auto err_tag = builder_.const_int(case_index(raw_type,error_type));
        auto is_error = builder_.fresh();
        builder_.emit(Binary{is_error,"==",tag,err_tag,int_type,bool_type});
        const auto bad = builder_.label("file.operator.error");
        const auto good = builder_.label("file.operator.non_error");
        builder_.emit(Branch{is_error,bad,good});
        builder_.enter(builder_.add_block(bad));
        propagate_error(raw,raw_type);
        builder_.enter(builder_.add_block(good));
        auto eof_tag = builder_.const_int(case_index(raw_type,none_type));
        auto is_eof = builder_.fresh();
        builder_.emit(Binary{is_eof,"==",tag,eof_tag,int_type,bool_type});
        const auto eof = builder_.label("file.operator.eof");
        const auto line_label = builder_.label("file.operator.line");
        builder_.emit(Branch{is_eof,eof,line_label});
        builder_.enter(builder_.add_block(eof));
        release_owned(builder_,raw,raw_type);
        wrap_bool(false);
        builder_.enter(builder_.add_block(line_label));
        auto text = builder_.fresh();
        builder_.emit(VariantPayload{text,raw,string_type});
        if (target_type.kind == TypeKind::String) {
            auto line_value = lifetime_.copy_value(text,string_type);
            release_owned(builder_,raw,raw_type);
            store_success(line_value);
        } else if (target_type.kind == TypeKind::Bool) {
            const auto equals = [&](const char* spelling) {
                auto constant = builder_.fresh();
                builder_.emit(ConstantString{constant,spelling});
                auto match = builder_.fresh();
                builder_.emit(Binary{match,"==",text,constant,string_type,bool_type});
                return match;
            };
            auto is_true = equals("true");
            const auto true_label = builder_.label("file.operator.true");
            const auto other_label = builder_.label("file.operator.not_true");
            builder_.emit(Branch{is_true,true_label,other_label});
            builder_.enter(builder_.add_block(true_label));
            release_owned(builder_,raw,raw_type);
            store_success(builder_.const_bool(true));
            builder_.enter(builder_.add_block(other_label));
            auto is_false = equals("false");
            const auto false_label = builder_.label("file.operator.false");
            const auto invalid_label = builder_.label("file.operator.invalid_bool");
            builder_.emit(Branch{is_false,false_label,invalid_label});
            builder_.enter(builder_.add_block(false_label));
            release_owned(builder_,raw,raw_type);
            store_success(builder_.const_bool(false));
            builder_.enter(builder_.add_block(invalid_label));
            release_owned(builder_,raw,raw_type);
            auto invalid = builder_.fresh();
            builder_.emit(ErrorMake{invalid,
                                    static_cast<std::uint32_t>(abi::ErrorValueCode::numeric_parse),
                                    {},
                                    {{std::nullopt,"bool",false},
                                     {std::nullopt,"INVALID_TEXT",false}},
                                    std::nullopt,{line,column}});
            wrap_error(invalid);
        } else {
            const auto parsed_type = Type::union_of({target_type,error_type});
            auto parsed = builder_.fresh();
            builder_.emit(ParseNumber{parsed,text,target_type,parsed_type,{line,column}});
            release_owned(builder_,raw,raw_type);
            auto parsed_tag = builder_.fresh();
            builder_.emit(VariantTag{parsed_tag,parsed});
            auto parsed_error_tag = builder_.const_int(case_index(parsed_type,error_type));
            auto failed = builder_.fresh();
            builder_.emit(Binary{failed,"==",parsed_tag,parsed_error_tag,int_type,bool_type});
            const auto invalid_label = builder_.label("file.operator.invalid_number");
            const auto valid_label = builder_.label("file.operator.valid_number");
            builder_.emit(Branch{failed,invalid_label,valid_label});
            builder_.enter(builder_.add_block(invalid_label));
            propagate_error(parsed,parsed_type);
            builder_.enter(builder_.add_block(valid_label));
            auto value = builder_.fresh();
            builder_.emit(VariantPayload{value,parsed,target_type});
            auto kept = lifetime_.copy_value(value,target_type);
            release_owned(builder_,parsed,parsed_type);
            store_success(kept);
        }
        builder_.enter(builder_.add_block(done));
        auto out = builder_.fresh();
        builder_.emit(LoadLocal{out,result_name,result_type});
        if (owned) release_owned(builder_,handle,receiver_type);
        return out;
    }
    if(receiver_type.kind==TypeKind::Class &&
       receiver_type.class_name==standard_class::file_handle){
        auto handle=lowerer_.lower(*n.receiver);
        const bool owned=lifetime_.expression_owns_result(*n.receiver);
        const auto finish=[&](ValueId result){
            if(owned) release_owned(builder_,handle,receiver_type);
            return result;
        };
        if(n.method=="read"){
            auto out=builder_.fresh();
            builder_.emit(FileHandleRead{out,handle,checked_.raw_types.at(&e),{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
            return finish(out);
        }
        if(n.method=="read_line"){
            auto out=builder_.fresh();
            builder_.emit(FileHandleReadLine{out,handle,checked_.raw_types.at(&e),{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
            return finish(out);
        }
        if(n.method=="read_bin"){
            auto out=builder_.fresh();
            builder_.emit(FileHandleReadBin{out,handle,checked_.raw_types.at(&e),{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
            return finish(out);
        }
        if(n.method=="write" || n.method=="write_line"){
            auto text=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
            builder_.emit(FileHandleWrite{out,handle,text,checked_.raw_types.at(&e),n.method=="write_line",{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
            lifetime_.release_temporary(*n.args[0].value,text);
            return finish(out);
        }
        if(n.method=="flush"){
            auto out=builder_.fresh();
            builder_.emit(FileHandleFlush{out,handle,checked_.raw_types.at(&e),{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
            return finish(out);
        }
        if(n.method=="seek"){
            auto position=lowerer_.lower_int64(*n.args[0].value),out=builder_.fresh();
            builder_.emit(FileHandleSeek{out,handle,position,checked_.raw_types.at(&e),{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
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
    if(*resolution.builtin==BuiltinCallable::FileOpen || *resolution.builtin==BuiltinCallable::FileRead)
        builder_.emit(FileOpen{out,path,checked_.raw_types.at(&e),{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
    else if(*resolution.builtin==BuiltinCallable::FileCreate || *resolution.builtin==BuiltinCallable::FileWrite)
        builder_.emit(FileCreate{out,path,checked_.raw_types.at(&e),{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
    else
        builder_.emit(FileAppend{out,path,checked_.raw_types.at(&e),{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
    lifetime_.release_temporary(*n.args[0].value,path);
    return out;
}

ValueId FileLowering::lower_file_read(const Expr& e, const CallExpr& n) {
    auto path=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(FileRead{out,path,checked_.raw_types.at(&e),{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
    lifetime_.release_temporary(*n.args[0].value,path);
    return out;
}

ValueId FileLowering::lower_file_read_bin(const Expr& e, const CallExpr& n) {
    auto path=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(FileReadBin{out,path,checked_.raw_types.at(&e),{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
    lifetime_.release_temporary(*n.args[0].value,path);
    return out;
}

ValueId FileLowering::lower_file_list(const Expr& e, const CallExpr& n) {
    auto path=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    ValueId recursive=n.args.size()==2?lowerer_.lower_argument(*n.args[1].value):builder_.const_bool(false);
    builder_.emit(FileList{out,path,recursive,checked_.raw_types.at(&e),{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
    lifetime_.release_temporary(*n.args[0].value,path);
    return out;
}

ValueId FileLowering::lower_file_write(const Expr& e, const CallExpr& n) {
    auto path=lowerer_.lower(*n.args[0].value),text=lowerer_.lower(*n.args[1].value),out=builder_.fresh();
    builder_.emit(FileWrite{out,path,text,checked_.raw_types.at(&e),{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
    lifetime_.release_temporary(*n.args[0].value,path);
    lifetime_.release_temporary(*n.args[1].value,text);
    return out;
}

ValueId FileLowering::lower_file_write_bin(const Expr& e, const CallExpr& n) {
    auto path=lowerer_.lower(*n.args[0].value),bin=lowerer_.lower(*n.args[1].value),out=builder_.fresh();
    builder_.emit(FileWriteBin{out,path,bin,checked_.raw_types.at(&e),{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
    lifetime_.release_temporary(*n.args[0].value,path);
    lifetime_.release_temporary(*n.args[1].value,bin);
    return out;
}

ValueId FileLowering::lower_file_exists(const Expr& e, const CallExpr& n) {
    auto path=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(FileExists{out,path,checked_.raw_types.at(&e),{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
    lifetime_.release_temporary(*n.args[0].value,path);
    return out;
}

ValueId FileLowering::lower_file_is_directory(const Expr& e, const CallExpr& n) {
    auto path=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(FileIsDirectory{out,path,checked_.raw_types.at(&e),{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
    lifetime_.release_temporary(*n.args[0].value,path);
    return out;
}

ValueId FileLowering::lower_file_remove(const Expr& e, const CallExpr& n) {
    auto path=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(FileRemove{out,path,checked_.raw_types.at(&e),{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
    lifetime_.release_temporary(*n.args[0].value,path);
    return out;
}

ValueId FileLowering::lower_file_copy(const Expr& e, const CallExpr& n) {
    auto source=lowerer_.lower(*n.args[0].value),destination=lowerer_.lower(*n.args[1].value),out=builder_.fresh();
    builder_.emit(FileCopy{out,source,destination,checked_.raw_types.at(&e),{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
    lifetime_.release_temporary(*n.args[0].value,source);
    lifetime_.release_temporary(*n.args[1].value,destination);
    return out;
}

ValueId FileLowering::lower_file_move(const Expr& e, const CallExpr& n) {
    auto source=lowerer_.lower(*n.args[0].value),destination=lowerer_.lower(*n.args[1].value),out=builder_.fresh();
    builder_.emit(FileMove{out,source,destination,checked_.raw_types.at(&e),{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
    lifetime_.release_temporary(*n.args[0].value,source);
    lifetime_.release_temporary(*n.args[1].value,destination);
    return out;
}

ValueId FileLowering::lower_file_mkdir(const Expr& e, const CallExpr& n) {
    auto path=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(FileMkdir{out,path,checked_.raw_types.at(&e),{static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)}});
    lifetime_.release_temporary(*n.args[0].value,path);
    return out;
}
} // namespace quidra::lowering
