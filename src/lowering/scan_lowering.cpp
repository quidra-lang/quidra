// Scan lowering: the stdlib scan() intrinsic. One input line is walked
// against the checked format; each field is parsed and stored into its
// target, and the first failure leaves an error result.

#include "lowering/lowerer.hpp"

namespace quidra::lowering {

// scan(...): read one line, walk it against the format, and store each
// parsed field into its target. The result is void | error; on error no
// target is written after the failing point, and the ones before it hold
// what was parsed.
ValueId ScanLowering::lower_scan(const Expr& e){
    const auto& format=checked_.scan_formats.at(&e);
    const auto result_type=checked_.raw_types.at(&e);
    const auto error_type=Type::simple(TypeKind::Error);
    const auto string_type=Type::simple(TypeKind::String);
    const auto int_type=Type::simple(TypeKind::Int64);
    const auto bool_type=Type::simple(TypeKind::Bool);
    const auto none_type=Type::simple(TypeKind::None);
    const auto line_type=Type::union_of({string_type,none_type,error_type});
    const auto found_type=Type::union_of({int_type,none_type});
    const auto result_name=builder_.hidden("scan.result");scope_.local_type(result_name)=result_type;
    const auto rest_name=builder_.hidden("scan.rest");scope_.local_type(rest_name)=string_type;
    const auto done=builder_.label("scan.done");
    const auto codepoints=[](const std::string& text){
        long long count=0;
        for(const unsigned char c:text) if((c&0xC0)!=0x80) ++count;
        return count;
    };
    const auto quoted=[](const std::string& text){
        std::string out="\"";
        for(const char c:text){ if(c=='"') out+="\"\""; else out.push_back(c); }
        return out+"\"";
    };
    // Stores an error result and leaves for the join. The current block
    // must not be used afterwards.
    const auto fail_with=[&](ValueId message){
        auto wrapped=builder_.fresh();
        builder_.emit(VariantMake{wrapped,case_index(result_type,error_type),message,result_type,error_type});
        builder_.emit(StoreLocal{result_name,wrapped,result_type,true});
        builder_.emit(Jump{done});
    };
    const auto fail=[&](const std::string& message){
        auto text=builder_.fresh();
        builder_.emit(ConstantString{text,message});
        fail_with(text);
    };
    const auto load_rest=[&](){
        auto out=builder_.fresh();
        builder_.emit(LoadLocal{out,rest_name,string_type});
        return out;
    };
    const auto rest_length=[&](ValueId rest){
        auto out=builder_.fresh();
        builder_.emit(StringLength{out,rest});
        return out;
    };

    // 1. One line of input.
    auto raw=builder_.fresh();
    builder_.emit(Input{raw,line_type});
    auto tag=builder_.fresh();
    builder_.emit(VariantTag{tag,raw});
    {
        auto error_index=builder_.const_int(case_index(line_type,error_type)),is_error=builder_.fresh();
        builder_.emit(Binary{is_error,"==",tag,error_index,int_type,bool_type});
        const auto on_error=builder_.label("scan.input_error"),check_none=builder_.label("scan.check_none");
        builder_.emit(Branch{is_error,on_error,check_none});
        builder_.enter(builder_.add_block(on_error));
        auto problem=builder_.fresh();
        builder_.emit(VariantPayload{problem,raw,error_type});
        auto kept=lifetime_.copy_value(problem,error_type);
        builder_.emit(Release{raw,line_type});
        fail_with(kept);
        builder_.enter(builder_.add_block(check_none));
    }
    {
        auto none_index=builder_.const_int(case_index(line_type,none_type)),is_none=builder_.fresh();
        builder_.emit(Binary{is_none,"==",tag,none_index,int_type,bool_type});
        const auto on_eof=builder_.label("scan.eof"),on_text=builder_.label("scan.text");
        builder_.emit(Branch{is_none,on_eof,on_text});
        builder_.enter(builder_.add_block(on_eof));
        builder_.emit(Release{raw,line_type});
        fail("scan reached the end of input");
        builder_.enter(builder_.add_block(on_text));
        auto text=builder_.fresh();
        builder_.emit(VariantPayload{text,raw,string_type});
        auto owned=lifetime_.copy_value(text,string_type);
        builder_.emit(Release{raw,line_type});
        builder_.emit(StoreLocal{rest_name,owned,string_type});
    }

    // 2. Literal text before the first target must begin the line.
    const auto expect_literal=[&](const std::string& literal){
        if(literal.empty()) return;
        auto rest=load_rest(),needle=builder_.fresh(),ok=builder_.fresh();
        builder_.emit(ConstantString{needle,literal});
        builder_.emit(StringStartsWith{ok,rest,needle});
        const auto matched=builder_.label("scan.literal.ok"),missing=builder_.label("scan.literal.missing");
        builder_.emit(Branch{ok,matched,missing});
        builder_.enter(builder_.add_block(missing));
        fail("scan expected "+quoted(literal)+" in the input");
        builder_.enter(builder_.add_block(matched));
        auto start=builder_.const_int(codepoints(literal)),end=rest_length(rest),sliced=builder_.fresh();
        builder_.emit(StringSlice{sliced,rest,start,end});
        builder_.emit(StoreLocal{rest_name,sliced,string_type});
    };
    expect_literal(format.literals.front());

    // 3. Each target takes the text up to the next literal (or the end).
    for(std::size_t i=0;i<format.targets.size();++i){
        const auto& delimiter=format.literals[i+1];
        const auto& target_type=format.target_types[i];
        auto rest=load_rest();
        ValueId field=0;
        if(delimiter.empty()){
            auto zero=builder_.const_int(0),end=rest_length(rest);
            field=builder_.fresh();
            builder_.emit(StringSlice{field,rest,zero,end});
            auto empty=builder_.fresh();
            builder_.emit(ConstantString{empty,""});
            builder_.emit(StoreLocal{rest_name,empty,string_type});
        }else{
            auto needle=builder_.fresh(),found=builder_.fresh();
            builder_.emit(ConstantString{needle,delimiter});
            builder_.emit(StringFind{found,rest,needle,found_type});
            auto found_tag=builder_.fresh();
            builder_.emit(VariantTag{found_tag,found});
            auto none_index=builder_.const_int(case_index(found_type,none_type)),missing=builder_.fresh();
            builder_.emit(Binary{missing,"==",found_tag,none_index,int_type,bool_type});
            const auto absent=builder_.label("scan.delimiter.missing"),present=builder_.label("scan.delimiter.found");
            builder_.emit(Branch{missing,absent,present});
            builder_.enter(builder_.add_block(absent));
            builder_.emit(Release{found,found_type});
            fail("scan expected "+quoted(delimiter)+" in the input");
            builder_.enter(builder_.add_block(present));
            auto index=builder_.fresh();
            builder_.emit(VariantPayload{index,found,int_type});
            builder_.emit(Release{found,found_type});
            auto zero=builder_.const_int(0);
            field=builder_.fresh();
            builder_.emit(StringSlice{field,rest,zero,index});
            auto width=builder_.const_int(codepoints(delimiter)),after=builder_.fresh();
            builder_.emit(Binary{after,"+",index,width,int_type,int_type});
            auto end=rest_length(rest),remaining=builder_.fresh();
            builder_.emit(StringSlice{remaining,rest,after,end});
            builder_.emit(StoreLocal{rest_name,remaining,string_type});
        }
        auto address=lowerer_.lower_address(*format.targets[i],true);
        if(target_type.kind==TypeKind::String){
            builder_.emit(StoreAddress{address,field,string_type});
            continue;
        }
        const auto parsed_type=Type::union_of({target_type,error_type});
        auto parsed=builder_.fresh();
        builder_.emit(ParseNumber{parsed,field,target_type,parsed_type});
        auto parsed_tag=builder_.fresh();
        builder_.emit(VariantTag{parsed_tag,parsed});
        auto error_index=builder_.const_int(case_index(parsed_type,error_type)),is_error=builder_.fresh();
        builder_.emit(Binary{is_error,"==",parsed_tag,error_index,int_type,bool_type});
        const auto bad=builder_.label("scan.parse.error"),good=builder_.label("scan.parse.ok");
        builder_.emit(Branch{is_error,bad,good});
        builder_.enter(builder_.add_block(bad));
        builder_.emit(Release{parsed,parsed_type});
        {
            auto head=builder_.fresh(),tail=builder_.fresh(),message=builder_.fresh();
            builder_.emit(ConstantString{head,"scan could not read "+type_name(target_type)+" from \""});
            builder_.emit(ConstantString{tail,"\""});
            builder_.emit(StringConcat{message,{head,field,tail}});
            builder_.emit(Release{field,string_type});
            fail_with(message);
        }
        builder_.enter(builder_.add_block(good));
        builder_.emit(Release{field,string_type});
        auto value=builder_.fresh();
        builder_.emit(VariantPayload{value,parsed,target_type});
        builder_.emit(Release{parsed,parsed_type});
        builder_.emit(StoreAddress{address,value,target_type});
    }

    // 4. Nothing may remain after the format.
    {
        auto rest=load_rest(),empty=builder_.fresh();
        builder_.emit(StringEmpty{empty,rest});
        const auto complete=builder_.label("scan.complete"),trailing=builder_.label("scan.trailing");
        builder_.emit(Branch{empty,complete,trailing});
        builder_.enter(builder_.add_block(trailing));
        auto head=builder_.fresh(),tail=builder_.fresh(),message=builder_.fresh();
        builder_.emit(ConstantString{head,"scan found unexpected input after the format: \""});
        builder_.emit(ConstantString{tail,"\""});
        builder_.emit(StringConcat{message,{head,rest,tail}});
        fail_with(message);
        builder_.enter(builder_.add_block(complete));
        auto ok=builder_.fresh();
        builder_.emit(VariantMake{ok,case_index(result_type,Type::simple(TypeKind::Void)),0,result_type,Type::simple(TypeKind::Void)});
        builder_.emit(StoreLocal{result_name,ok,result_type,true});
        builder_.emit(Jump{done});
    }
    builder_.enter(builder_.add_block(done));
    auto out=builder_.fresh();
    builder_.emit(LoadLocal{out,result_name,result_type});
    return out;
}

} // namespace quidra::lowering
