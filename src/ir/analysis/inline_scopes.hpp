#pragma once
#include "quidra/ir/function.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace quidra::ir::analysis {
// Typed destructor distinction shared by summaries and flow-sensitive facts.
// Tensor finalization drops buffers/graph nodes but does not call user VJPs.
inline bool unknown_cleanup_type(const Type& type) {
    if(type.kind==TypeKind::Class || type.kind==TypeKind::Auto || type.kind==TypeKind::Invalid)return true;
    if(type.kind==TypeKind::Function)return false;
    if(type.first && unknown_cleanup_type(*type.first))return true;
    return std::any_of(type.parameters.begin(),type.parameters.end(),unknown_cleanup_type) ||
        std::any_of(type.cases.begin(),type.cases.end(),unknown_cleanup_type);
}
inline const InlineScope& inline_scope(const Function& fn,std::uint64_t identity) {
    const InlineScope* result=nullptr;
    for(const auto& scope:fn.inline_scopes)if(scope.identity==identity){
        if(!identity || result)throw std::invalid_argument("IR inline scope identity is ambiguous");result=&scope;
    }
    if(!result)throw std::invalid_argument("IR inline scope identity is absent");return *result;
}
inline std::optional<Type> cleanup_result_type(const Instruction& ins) {
    return std::visit([](const auto& n)->std::optional<Type>{
        using T=std::decay_t<decltype(n)>;
        if constexpr(!requires{n.out;})return {};
        else if constexpr(requires{n.result_type.kind;})return n.result_type;
        else if constexpr(requires{n.type.kind;})return n.type;
        else if constexpr(requires{n.result.kind;})return n.result;
        else if constexpr(requires{n.field_type.kind;})return n.field_type;
        else if constexpr(requires{n.element_type.kind;})return n.element_type;
        else if constexpr(std::is_same_v<T,ConstantString>)return Type::simple(TypeKind::String);
        else if constexpr(std::is_same_v<T,ConstantBool>)return Type::simple(TypeKind::Bool);
        else return {};
    },ins);
}
inline Type cleanup_operand_type(const Function& fn,ValueId value) {
    Type result;std::size_t found=0;
    for(const auto& block:fn.blocks)for(const auto& ins:block.instructions)std::visit([&](const auto& n){
        if constexpr(requires{n.out;})if(n.out==value){++found;
            if(const auto type=cleanup_result_type(ins))result=*type;
            else if constexpr(std::is_same_v<std::decay_t<decltype(n)>,AddressLocal>){
                for(const auto& p:fn.parameters)if(p.name==n.name)result=p.type;
                for(const auto& s:fn.inline_scopes)for(const auto& slot:s.slots)if(slot.name==n.name)result=slot.type;
                for(const auto& b:fn.blocks)for(const auto& i:b.instructions){
                    if(const auto* d=std::get_if<DeclareLocal>(&i);d && d->name==n.name)result=d->type;
                    if(const auto* d=std::get_if<StoreLocal>(&i);d && d->name==n.name)result=d->type;
                }
            }
        }
    },ins);return found==1?result:Type{};
}
inline bool inline_cleanup_unknown(const Function& fn,const InlineScope& scope) {
    for(const auto& slot:scope.slots)if((slot.kind==InlineSlotKind::OwnedLocal || slot.kind==InlineSlotKind::PinnedReference) && unknown_cleanup_type(slot.type))return true;
    // Node shadows can hold owned temporaries beyond the named slots.
    if(scope.node_cleanup)for(const auto& block:fn.blocks)if(std::find(scope.blocks.begin(),scope.blocks.end(),block.label)!=scope.blocks.end())
        for(const auto& ins:block.instructions){bool unknown=false;std::visit([&](const auto& n){
            if constexpr(requires{n.out;})if(n.out)if(const auto type=cleanup_result_type(ins))unknown=unknown_cleanup_type(*type);
            if constexpr(std::is_same_v<std::decay_t<decltype(n)>,Pin>)unknown=unknown_cleanup_type(cleanup_operand_type(fn,n.value));
        },ins);if(unknown)return true;}
    return false;
}
} // namespace quidra::ir::analysis
