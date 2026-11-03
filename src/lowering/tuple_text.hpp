#pragma once
#include "lowering/ownership_expansion.hpp"
namespace quidra::lowering {
inline ir::ValueId value_text(ir::FunctionBuilder& builder,ir::ValueId value,const Type& type,bool quote=false) {
    using namespace ir;
    const auto text_type=Type::simple(TypeKind::String);
    if(type.kind==TypeKind::String){if(!quote)return copy_owned(builder,value,type);auto out=builder.fresh();builder.emit(StringQuote{out,value});return out;}
    if(type.kind==TypeKind::None){auto out=builder.fresh();builder.emit(ConstantString{out,"none"});return out;}
    if(type.kind==TypeKind::Union) {
        auto tag=builder.fresh();builder.emit(VariantTag{tag,value});
        const auto slot=builder.hidden("tuple.text");builder.emit(DeclareLocal{slot,text_type,{}});
        auto done=builder.label("tuple.text.done");
        for(std::size_t i=0;i<type.cases.size();++i){auto yes=builder.label("tuple.text.case"),no=builder.label("tuple.text.next");auto expected=builder.const_int(static_cast<long long>(i));auto matches=builder.fresh();builder.emit(Binary{matches,"==",tag,expected,Type::simple(TypeKind::Int64),Type::simple(TypeKind::Bool)});builder.emit(Branch{matches,yes,no});builder.enter(builder.add_block(yes));ValueId payload{};if(type.cases[i].kind!=TypeKind::None){payload=builder.fresh();builder.emit(VariantPayload{payload,value,type.cases[i]});}auto text=value_text(builder,payload,type.cases[i],quote);builder.emit(StoreLocal{slot,text,text_type});builder.emit(Jump{done});builder.enter(builder.add_block(no));}
        builder.emit(Jump{done});builder.enter(builder.add_block(done));auto out=builder.fresh();builder.emit(LoadLocal{out,slot,text_type,true});return out;
    }
    if(type.kind!=TypeKind::Tuple){auto out=builder.fresh();builder.emit(ToString{out,value,type,std::nullopt,false});return out;}
    std::vector<StringBuildPart> parts;std::vector<ValueId> temporary;
    auto punctuation=[&](const char* text){auto out=builder.fresh();builder.emit(ConstantString{out,text});parts.push_back({out,text_type});};
    punctuation("(");
    for(std::size_t i=0;i<type.parameters.size();++i){if(i)punctuation(", ");auto child=builder.fresh();const auto& element=type.parameters[i];builder.emit(TupleGet{child,value,i,element});{auto text=value_text(builder,child,element,true);parts.push_back({text,text_type});if(element.kind!=TypeKind::None)temporary.push_back(text);}}
    punctuation(")");auto separator=builder.fresh();builder.emit(ConstantString{separator,""});auto out=builder.fresh();builder.emit(StringBuild{out,std::move(parts),separator});for(auto text:temporary)release_owned(builder,text,text_type);return out;
}
} // namespace quidra::lowering
