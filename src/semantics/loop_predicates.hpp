#pragma once

// Loop predicates consume the same E.1 summaries as argument isolation and
// mutation markers. A whole-place write may replace an iterated buffer; a
// write below it changes its elements and only a value loop needs a snapshot.
#include "semantics/effect_summary.hpp"
#include "constant_integer_eval.hpp"
#include <functional>
#include <optional>
#include <unordered_map>

namespace quidra::semantics {

enum class LoopWrite { none, replaces, in_place };

inline std::optional<long long> loop_element_offset(
    const Expr& index, const Expr& base, const std::unordered_map<const Expr*, Type>& types) {
    const auto value = constant_eval::integer(index);
    if (!value || *value >= 0) return value;
    const auto type = types.find(&base);
    // A negative index can alias a nonnegative index. Without a static
    // length the effect path must allow any element.
    if (type == types.end() || type->second.kind != TypeKind::Array || type->second.length < 0)
        return std::nullopt;
    return type->second.length + *value;
}

inline std::optional<StoragePath> loop_storage_path(const CheckedProgram& checked,const Expr& expression) {
    if(const auto* name=std::get_if<NameExpr>(&expression.data)) {
        if(name->this_qualifier || checked.field_accesses.contains(&expression))
            return StoragePath{StorageRoot::local("$this"),{PathStep::field_named(name->name)}};
        return StoragePath{StorageRoot::local(name->name),{}};
    }
    if(const auto* member=std::get_if<MemberExpr>(&expression.data)) {
        auto path=loop_storage_path(checked,*member->base);
        if(path) return path->child(PathStep::field_named(member->name));
    }
    if(const auto* index=std::get_if<IndexExpr>(&expression.data);
       index && index->items.size()==1 && !index->items[0].slice) {
        auto path=loop_storage_path(checked,*index->base);
        if(path) {
            const auto value = loop_element_offset(*index->items[0].index, *index->base,
                                                   checked.expr_types);
            return path->child(value?PathStep::element(*value):PathStep::any_element());
        }
    }
    return std::nullopt;
}

inline bool loop_path_prefix(const StoragePath& prefix,const StoragePath& target) {
    return prefix.root==target.root && prefix.steps.size()<=target.steps.size() && overlaps(prefix,target);
}

inline bool loop_definite_place_prefix(const Expr& prefix, const Expr& path,
                                       const std::unordered_map<const Expr*, Type>& types) {
    const auto equal=[&](const auto& self,const Expr& a,const Expr& b)->bool {
        if(const auto* x=std::get_if<NameExpr>(&a.data)) {
            const auto* y=std::get_if<NameExpr>(&b.data);
            return y && x->name==y->name && x->this_qualifier.has_value()==y->this_qualifier.has_value();
        }
        if(const auto* x=std::get_if<MemberExpr>(&a.data)) {
            const auto* y=std::get_if<MemberExpr>(&b.data);
            return y && x->name==y->name && self(self,*x->base,*y->base);
        }
        if(const auto* x=std::get_if<IndexExpr>(&a.data)) {
            const auto* y=std::get_if<IndexExpr>(&b.data);
            if (!y || x->items.size() != 1 || y->items.size() != 1 ||
                x->items[0].slice || y->items[0].slice || !self(self, *x->base, *y->base))
                return false;
            if (self(self, *x->items[0].index, *y->items[0].index)) return true;
            // The borrowed path is already typed when checking a new write.
            const auto left = loop_element_offset(*x->items[0].index, *y->base, types);
            const auto right = loop_element_offset(*y->items[0].index, *y->base, types);
            return left && left == right;
        }
        const auto x=constant_eval::integer(a,nullptr),y=constant_eval::integer(b,nullptr);
        return x && y && *x==*y;
    };
    const auto visit=[&](const auto& self,const Expr& current)->bool {
        if(equal(equal,prefix,current)) return true;
        if(const auto* field=std::get_if<MemberExpr>(&current.data)) return self(self,*field->base);
        if(const auto* element=std::get_if<IndexExpr>(&current.data)) return self(self,*element->base);
        return false;
    };
    return visit(visit,path);
}

class LoopPredicates {
public:
    using Aliases=std::function<bool(const Expr&)>;
    LoopPredicates(const CheckedProgram& checked,Aliases aliases)
        :checked_(checked),aliases_(std::move(aliases)) {}

    // null means that E.1 has no summary for this call; the lowering retains
    // its conservative predicate. A known read-only call returns none even
    // when it receives a writable argument.
    std::optional<LoopWrite> call_write(const Expr& expression,const Expr& iterable,bool shapes_only) const {
        const auto* call=std::get_if<CallExpr>(&expression.data);
        const auto* method=std::get_if<MethodCallExpr>(&expression.data);
        if(!call && !method) return std::nullopt;
        if(!checked_.effects) return std::nullopt;
        std::string target;
        std::size_t offset=0;
        const Expr* receiver=nullptr;
        if(const auto found=checked_.method_calls.find(&expression);found!=checked_.method_calls.end()) {
            target=found->second.internal_name;
            offset=1;
            if(method) receiver=method->receiver.get();
        } else if(const auto found=checked_.call_resolutions.find(&expression);found!=checked_.call_resolutions.end()) {
            target=found->second.target;
            if(found->second.kind==CallKind::NumericCast) return LoopWrite::none;
        }
        const auto* summary=checked_.effects->find(target);
        const auto signature=checked_.functions.find(target);
        if(!summary || signature==checked_.functions.end()) return std::nullopt;
        const auto desired=loop_storage_path(checked_,iterable);
        if(!desired) return LoopWrite::in_place;
        if(summary->mutates_ambient) return LoopWrite::in_place;
        const auto& args=call?call->args:method->args;
        const auto& parameters=signature->second.parameters;
        std::vector<const Expr*> bound(parameters.size(),nullptr);
        if(offset && !bound.empty()) bound[0]=receiver;
        std::size_t positional=offset;
        for(const auto& arg:args) {
            std::size_t at=parameters.size();
            if(arg.name) {
                for(std::size_t i=offset;i<parameters.size();++i)
                    if(parameters[i].name==*arg.name) { at=i; break; }
            } else {
                while(positional<parameters.size() && bound[positional]) ++positional;
                at=positional++;
            }
            if(at<bound.size()) bound[at]=arg.value.get();
        }
        auto result=LoopWrite::none;
        const auto leaf=checked_.expr_types.find(&iterable);
        if(reference_call_writes(checked_,expression,iterable))return LoopWrite::replaces;
        if (aliases_(iterable)) {
            // A normal reference parameter can alias a copied holder's target,
            // even when it is not a stored-reference candidate itself.
            const auto compatible = [&](const Type& written) {
                return leaf == checked_.expr_types.end() ||
                    reference_target_compatible(checked_, leaf->second, written);
            };
            if (const auto* writes = checked_.effects->reference_call(expression)) {
                for (const auto& [_, written] : writes->shared)
                    if (compatible(written)) return LoopWrite::replaces;
                const auto* paths = checked_.effects->reference_target_paths(iterable);
                for (const auto& [path, written] : writes->resolved)
                    if (paths ? overlaps_any(*paths, path) : compatible(written))
                        return LoopWrite::replaces;
            } else {
                for (const auto& [_, written] : summary->reference_shared_writes)
                    if (compatible(written)) return LoopWrite::replaces;
            }
        }
        for(std::size_t i=0;i<summary->exposed_writes.size();++i) {
            if(summary->exposed_writes[i].empty()) continue;
            if(i>=bound.size() || !bound[i]) {
                // An implicit receiver is $this; its fields are exposed by
                // the same summary, but only a receiver-rooted place aliases.
                if(offset && i==0 && desired->root.name=="$this") return LoopWrite::in_place;
                continue;
            }
            const auto actual=loop_storage_path(checked_,*bound[i]);
            if(!actual) return LoopWrite::in_place;
            for(const auto& relative:summary->exposed_writes[i]) {
                const auto written=actual->extended(relative.steps);
                if(written.root==desired->root) {
                    if(loop_path_prefix(written,*desired)) return LoopWrite::replaces;
                    if(!shapes_only && overlaps(written,*desired)) result=LoopWrite::in_place;
                } else if(aliases_(*bound[i])) {
                    // Different references can designate the same storage.
                    // An element-only scalar write cannot replace an array;
                    // a write whose slot can hold the array remains recorded.
                    Type slot=checked_.expr_types.at(bound[i]);
                    bool unknown=false;
                    for(const auto& step:relative.steps) {
                        if(step.kind==StepKind::Field && slot.kind==TypeKind::Class) {
                            const auto cls=checked_.classes.find(slot.class_name);
                            bool found=false;
                            if(cls!=checked_.classes.end()) for(const auto& field:cls->second.fields)
                                if(field.name==step.field) { slot=field.type;found=true;break; }
                            if(!found) {unknown=true;break;}
                        } else if(step.kind!=StepKind::Field && slot.kind==TypeKind::Array) slot=*slot.first;
                        else {unknown=true;break;}
                    }
                    if(!shapes_only || unknown || (leaf!=checked_.expr_types.end() && may_contain(slot,leaf->second)))
                        return LoopWrite::in_place;
                }
            }
        }
        for(std::size_t i=0;i<summary->mutates_shared_of_parameter.size();++i)
            if(summary->mutates_shared_of_parameter[i] && leaf!=checked_.expr_types.end() &&
               contains_shared_region(checked_,leaf->second)) return LoopWrite::in_place;
        return result;
    }
private:
    bool may_contain(const Type& container,const Type& value) const {
        std::set<std::string> visiting;
        const auto visit=[&](const auto& self,const Type& type)->bool {
            if(type==value) return true;
            if(type.first && self(self,*type.first)) return true;
            for(const auto& child:type.parameters)if(self(self,child))return true;
            for(const auto& alternative:type.cases) if(self(self,alternative)) return true;
            if(type.kind==TypeKind::Class && visiting.insert(type.class_name).second) {
                if(const auto cls=checked_.classes.find(type.class_name);cls!=checked_.classes.end())
                    for(const auto& field:cls->second.fields) if(self(self,field.type)) return true;
            }
            return false;
        };
        return visit(visit,container);
    }
    const CheckedProgram& checked_;
    Aliases aliases_;
};

} // namespace quidra::semantics
