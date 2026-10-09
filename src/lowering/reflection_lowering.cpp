// Reflection lowering: walks a value's type to collect the paths and the
// values of every field of a target type into hidden array locals,
// recursing through public class fields and array elements; a class already
// on the walk is not entered again (autograd_lowering.cpp collects the
// autograd targets reached from a value the same way). Also the reflect
// builtins (reflect.type_name, reflect.collect, reflect.paths).

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include <stdexcept>

namespace quidra::lowering {

std::string ReflectionLowering::collected_array_local(
    const Type& element_type,const std::string& prefix) {
    const auto array_type=Type::array(element_type);
    const auto name=builder_.hidden(prefix);
    scope_.local_type(name)=array_type;
    auto empty=builder_.fresh();
    builder_.emit(ArrayMake{empty,{},array_type});
    // The hidden accumulator transfers ownership to its expression result
    // (or is released explicitly after backward), so ordinary function
    // cleanup must not release the local a second time.
    builder_.emit(
        StoreLocal{name,empty,array_type,true,true});
    return name;
}

void ReflectionLowering::append_collected_value(
    const std::string& destination,const Type& element_type,ValueId value) {
    const auto array_type=Type::array(element_type);
    auto current=builder_.fresh();
    builder_.emit(
        LoadLocal{current,destination,array_type});
    auto length=builder_.fresh();
    builder_.emit(ArrayLength{length,current});
    auto grown=builder_.fresh();
    builder_.emit(
        ArrayGrowMove{grown,current,array_type});
    auto owned=lifetime_.copy_value(value,element_type);
    builder_.emit(ArraySet{
        grown,length,owned,element_type,0,0,true,true});
    builder_.emit(
        StoreLocal{destination,grown,array_type,true,true});
}

void ReflectionLowering::for_each_collected_array_element(
    ValueId array,const Type& array_type,const std::string& prefix,
    const std::function<void(ValueId,const Type&,ValueId)>& visit) {
    if(array_type.kind!=TypeKind::Array||!array_type.first) return;
    auto count=builder_.fresh();
    builder_.emit(ArrayLength{count,array});
    const auto index_name=builder_.hidden(prefix+".index");
    scope_.local_type(index_name)=Type::simple(TypeKind::Int64);
    builder_.emit(StoreLocal{
        index_name,builder_.const_int(0),Type::simple(TypeKind::Int64)});

    const auto cond=builder_.label(prefix+".cond");
    const auto body=builder_.label(prefix+".body");
    const auto done=builder_.label(prefix+".done");
    builder_.emit(Jump{cond});

    builder_.enter(builder_.add_block(cond));
    auto index=builder_.fresh();
    builder_.emit(
        LoadLocal{index,index_name,Type::simple(TypeKind::Int64)});
    auto more=builder_.fresh();
    builder_.emit(Binary{
        more,"<",index,count,Type::simple(TypeKind::Int64),
        Type::simple(TypeKind::Bool)});
    builder_.emit(Branch{more,body,done});

    builder_.enter(builder_.add_block(body));
    auto child=builder_.fresh();
    builder_.emit(ArrayGet{
        child,array,index,*array_type.first,0,0,
        array_type.length>=0,true});
    visit(child,*array_type.first,index);
    auto next=builder_.fresh();
    builder_.emit(Binary{
        next,"+",index,builder_.const_int(1),Type::simple(TypeKind::Int64),
        Type::simple(TypeKind::Int64)});
    builder_.emit(StoreLocal{
        index_name,next,Type::simple(TypeKind::Int64)});
    builder_.emit(Jump{cond});

    builder_.enter(builder_.add_block(done));
}

bool ReflectionLowering::reflected_type_contains(
    const Type& type,const Type& target,
    std::unordered_set<std::string>& active) const {
    if(type==target) return true;
    if(type.kind==TypeKind::Array){
        return type.first &&
            reflected_type_contains(*type.first,target,active);
    }
    if(type.kind!=TypeKind::Class) return false;
    if(!active.insert(type.class_name).second) return false;
    const auto ci=checked_.classes.find(type.class_name);
    if(ci==checked_.classes.end()){
        active.erase(type.class_name);
        return false;
    }
    for(const auto& field:ci->second.fields){
        if(field.is_private) continue;
        if(reflected_type_contains(field.type,target,active)){
            active.erase(type.class_name);
            return true;
        }
    }
    active.erase(type.class_name);
    return false;
}

ValueId ReflectionLowering::reflected_index_path(ValueId prefix,ValueId index) {
    const auto string_type=Type::simple(TypeKind::String);
    auto open=builder_.fresh();
    auto close=builder_.fresh();
    auto separator=builder_.fresh();
    builder_.emit(ConstantString{open,"["});
    builder_.emit(ConstantString{close,"]"});
    builder_.emit(ConstantString{separator,""});
    auto out=builder_.fresh();
    builder_.emit(StringBuild{
        out,
        {
            StringBuildPart{prefix,string_type,false},
            StringBuildPart{open,string_type,true},
            StringBuildPart{index,Type::simple(TypeKind::Int64),false},
            StringBuildPart{close,string_type,true},
        },
        separator});
    return out;
}

ValueId ReflectionLowering::reflected_field_path(
    ValueId prefix,bool prefix_empty,const std::string& field_name) {
    if(prefix_empty){
        auto out=builder_.fresh();
        builder_.emit(ConstantString{out,field_name});
        return out;
    }
    auto suffix=builder_.fresh();
    builder_.emit(ConstantString{suffix,"."+field_name});
    auto out=builder_.fresh();
    builder_.emit(StringConcat{out,{prefix,suffix}});
    return out;
}

void ReflectionLowering::collect_reflected_paths(
    ValueId object,const Type& type,const Type& target,
    ValueId prefix,bool prefix_empty,const std::string& destination,
    std::unordered_set<std::string>& active) {
    if(type==target){
        append_collected_value(
            destination,Type::simple(TypeKind::String),prefix);
        return;
    }
    if(type.kind==TypeKind::Array){
        for_each_collected_array_element(
            object,type,"reflect.paths",
            [&](ValueId child,const Type& child_type,ValueId index) {
                auto child_prefix=reflected_index_path(prefix,index);
                collect_reflected_paths(
                    child,child_type,target,child_prefix,false,destination,active);
                builder_.emit(
                    Release{child_prefix,Type::simple(TypeKind::String)});
            });
        return;
    }
    if(type.kind!=TypeKind::Class) return;
    if(!active.insert(type.class_name).second) return;
    const auto ci=checked_.classes.find(type.class_name);
    if(ci==checked_.classes.end()){
        active.erase(type.class_name);
        return;
    }
    for(const auto& field:ci->second.fields){
        if(field.is_private) continue;
        std::unordered_set<std::string> probe=active;
        if(!reflected_type_contains(field.type,target,probe)) continue;
        auto child=builder_.fresh();
        builder_.emit(
            FieldGet{child,object,field.index,field.type});
        auto child_prefix=reflected_field_path(
            prefix,prefix_empty,field.name);
        collect_reflected_paths(
            child,field.type,target,child_prefix,false,destination,active);
        if(!prefix_empty){
            builder_.emit(
                Release{child_prefix,Type::simple(TypeKind::String)});
        }
    }
    active.erase(type.class_name);
}

void ReflectionLowering::collect_reflected_values(
    ValueId object,const Type& type,const Type& target,
    const std::string& destination,
    std::unordered_set<std::string>& active) {
    if(type==target){
        append_collected_value(destination,target,object);
        return;
    }
    if(type.kind==TypeKind::Array){
        for_each_collected_array_element(
            object,type,"reflect.collect",
            [&](ValueId child,const Type& child_type,ValueId) {
                collect_reflected_values(
                    child,child_type,target,destination,active);
            });
        return;
    }
    if(type.kind!=TypeKind::Class) return;
    if(!active.insert(type.class_name).second) return;
    const auto ci=checked_.classes.find(type.class_name);
    if(ci==checked_.classes.end()){
        active.erase(type.class_name);
        return;
    }
    for(const auto& field:ci->second.fields){
        if(field.is_private) continue;
        std::unordered_set<std::string> probe=active;
        if(!reflected_type_contains(field.type,target,probe)) continue;
        auto child=builder_.fresh();
        builder_.emit(
            FieldGet{child,object,field.index,field.type});
        collect_reflected_values(
            child,field.type,target,destination,active);
    }
    active.erase(type.class_name);
}

ValueId ReflectionLowering::lower_reflect_type_name(const CallExpr& n) {
    auto object=lowerer_.lower(*n.args[0].value);
    auto out=builder_.fresh();
    builder_.emit(
        ConstantString{out,type_name(type_of(checked_,*n.args[0].value))});
    lifetime_.release_temporary(*n.args[0].value,object);
    return out;
}

ValueId ReflectionLowering::lower_reflect_collection(
    const Expr& e, const CallExpr& n, const CallResolution& resolution) {
    auto object=lowerer_.lower(*n.args[0].value);
    const auto result_type=checked_.raw_types.at(&e);
    if(result_type.kind!=TypeKind::Array||!result_type.first)
        throw std::logic_error("reflection collection result is not an array");
    const bool path_query=
        *resolution.builtin==BuiltinCallable::ReflectPaths;
    const auto destination=collected_array_local(
        *result_type.first,
        path_query?"reflect.paths.result":"reflect.collect.result");
    std::unordered_set<std::string> active;
    if(path_query){
        if(!resolution.reflected_target)
            throw std::logic_error("reflect.paths missing target type");
        auto root_prefix=builder_.fresh();
        builder_.emit(
            ConstantString{root_prefix,""});
        collect_reflected_paths(
            object,type_of(checked_,*n.args[0].value),
            *resolution.reflected_target,root_prefix,true,
            destination,active);
    }else{
        collect_reflected_values(
            object,type_of(checked_,*n.args[0].value),*result_type.first,
            destination,active);
    }
    auto out=builder_.fresh();
    builder_.emit(
        LoadLocal{out,destination,result_type});
    lifetime_.release_temporary(*n.args[0].value,object);
    return out;
}
} // namespace quidra::lowering
