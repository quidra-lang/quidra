#include "semantics/binding_entry.hpp"
#include <set>

namespace quidra::semantics {
BindingEntryInitialization binding_value_parameter_entry(const FunctionType& signature,
    const std::unordered_map<std::string,ClassTypeInfo>& classes,std::size_t first) {
    BindingEntryInitialization result;result.complete=true;
    for(std::size_t i=first;i<signature.parameters.size();++i) {
        const auto& parameter=signature.parameters[i];
        if(parameter.writable)continue;
        std::set<std::string> visiting;
        auto& paths=result.parameters[i];
        const auto collect=[&](const auto& self,const Type& type,const std::vector<PathStep>& prefix)->void {
            if(type.kind==TypeKind::Tuple) {
                for(std::size_t n=0;n<type.parameters.size();++n) {
                    auto path=prefix;path.push_back(PathStep::tuple_element(n));paths.push_back(path);
                    self(self,type.parameters[n],path);
                }
                return;
            }
            if(type.kind!=TypeKind::Class)return;
            const auto found=classes.find(type.class_name);
            if(found==classes.end()){result.complete=false;return;}
            const bool recurse=visiting.insert(type.class_name).second;
            for(const auto& field:found->second.fields) {
                if(field.reference)continue;
                auto path=prefix;path.push_back(PathStep::field_named(field.name));paths.push_back(path);
                if(recurse)self(self,field.type,path);
            }
            if(recurse)visiting.erase(type.class_name);
        };
        collect(collect,parameter.type,{});
    }
    return result;
}
} // namespace quidra::semantics
