#include "quidra/traversal_plan.hpp"
#include "quidra/checker.hpp"
#include "quidra/standard_classes.hpp"
#include <unordered_set>

namespace quidra {
namespace {
template<class Field> bool reference_field(const Field& field) {
    if constexpr(requires {field.reference;}) return field.reference;
    else return false;
}
class Planner {
public:
    Planner(const Type& element,const std::unordered_map<std::string,ClassTypeInfo>& classes,
            const TraversalPolicy& policy):element_(element),classes_(classes),policy_(policy) {}
    bool contains(const Type& type,std::unordered_set<std::string>& seen) const {
        if(type==element_) return true;
        if(type.kind==TypeKind::Array) return type.first&&contains(*type.first,seen);
        if(type.kind==TypeKind::Tuple) {
            for(const auto& part:type.parameters) if(contains(part,seen)) return true;
            return false;
        }
        if(type.kind==TypeKind::Union) {
            for(const auto& part:type.cases) if(contains(part,seen)) return true;
            return false;
        }
        if(type.kind!=TypeKind::Class || !seen.insert(type.class_name).second) return false;
        const auto found=classes_.find(type.class_name);
        if(found==classes_.end()) {seen.erase(type.class_name);return false;}
        const auto& info=found->second;
        const bool container=standard_class::is_map_instance(info.name,info.standard_library)||
                             standard_class::is_distinct_instance(info.name,info.standard_library);
        if(info.standard_library&&!container) {seen.erase(type.class_name);return false;}
        for(const auto& field:info.fields) {
            if(reference_field(field) || (!container&&field.name.starts_with('$'))) continue;
            if(contains(field.type,seen)) {seen.erase(type.class_name);return true;}
        }
        seen.erase(type.class_name);return false;
    }
    bool contains(const Type& type) const {std::unordered_set<std::string> seen;return contains(type,seen);}
    TraversalNode build(const Type& type,const std::string& path,std::unordered_set<std::string>& active) {
        TraversalNode node;node.type=type;
        if(type==element_) {node.kind=TraversalNode::Kind::Visit;visited_=true;return node;}
        if(!contains(type)) return node;
        const auto subject=path.empty()?type_name(type):path;
        if(type.kind==TypeKind::Union)
            throw TraversalError("'"+subject+"' may hold "+type_name(element_)+" inside '"+
                type_name(type)+"', which the traversal does not enter; give the optional value its own field or array.");
        if(type.kind==TypeKind::Array&&type.first) {
            node.kind=TraversalNode::Kind::Array;
            node.children.push_back(build(*type.first,subject+"[]",active));return node;
        }
        if(type.kind==TypeKind::Tuple) {
            for(std::size_t i=0;i<type.parameters.size();++i) if(contains(type.parameters[i])) {
                TraversalNode child;child.kind=TraversalNode::Kind::TupleElement;
                child.type=type.parameters[i];child.index=i;
                child.children.push_back(build(child.type,path+"."+std::to_string(i),active));
                node.children.push_back(std::move(child));
            }
            return node;
        }
        if(type.kind!=TypeKind::Class) return node;
        const auto& info=classes_.at(type.class_name);
        if(standard_class::is_map_instance(info.name,info.standard_library)||
           standard_class::is_distinct_instance(info.name,info.standard_library))
            throw TraversalError("'"+subject+"' holds "+type_name(element_)+" inside '"+
                type_name(type)+"', which the traversal does not enter.");
        if(info.standard_library) return node;
        if(!active.insert(type.class_name).second)
            throw TraversalError("'"+subject+"' makes '"+type_name(type)+"' contain itself; "
                "the traversal visits finite structures and cannot visit "+type_name(element_)+" below it.");
        for(const auto& field:info.fields) {
            if(reference_field(field)||field.name.starts_with('$')||!contains(field.type)) continue;
            const auto name=path.empty()?field.name:path+"."+field.name;
            if(policy_.writable&&field.is_const)
                throw TraversalError("'"+name+"' is const, so the in-place traversal cannot yield writable "+
                    type_name(element_)+" inside it; traverse a value copy or make the field writable.");
            if(field.is_private&&!policy_.internal&&info.module_namespace!=policy_.module)
                throw TraversalError("'"+name+"' is private to module '"+info.module_namespace+
                    "', so a traversal written in this module cannot enter it; use the package's traversal.");
            TraversalNode child;child.kind=TraversalNode::Kind::Field;child.type=field.type;
            child.name=field.name;child.index=field.index;
            child.children.push_back(build(field.type,name,active));node.children.push_back(std::move(child));
        }
        active.erase(type.class_name);return node;
    }
    bool visited_{};
private:
    const Type& element_;
    const std::unordered_map<std::string,ClassTypeInfo>& classes_;
    const TraversalPolicy& policy_;
};
}
TraversalPlan make_traversal_plan(const Type& root,const Type& element,
    const std::unordered_map<std::string,ClassTypeInfo>& classes,const TraversalPolicy& policy) {
    Planner planner(element,classes,policy);std::unordered_set<std::string> active;
    auto tree=planner.build(root,"",active);
    return {root,element,std::move(tree),policy.writable,!planner.visited_};
}
}
