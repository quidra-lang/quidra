#pragma once
#include "quidra/ir/module.hpp"
#include "quidra/ir/visit.hpp"
#include <map>
#include <set>
#include <stdexcept>

namespace quidra::llvm_backend {
// Compile-time scope ownership comes only from typed block/slot metadata.
// The selection is independent of debug markers and emission visitation order.
class InlineScopePlan {
public:
    explicit InlineScopePlan(const ir::Function& function) {
        if(function.inline_scopes.empty())return;
        std::set<std::string> labels;
        for(const auto& block:function.blocks)
            if(!labels.insert(block.label).second)throw std::logic_error("duplicate inline-scope block label");
        for(const auto& scope:function.inline_scopes) {
            if(!scope.identity || !scopes_.emplace(scope.identity,&scope).second)
                throw std::logic_error("duplicate or zero inline scope identity");
            for(const auto& slot:scope.slots)
                if(!slots_.emplace(slot.name,scope.identity).second)
                    throw std::logic_error("inline storage belongs to more than one scope");
        }
        for(const auto& [identity,scope]:scopes_) {
            std::set<std::uint64_t> seen;
            auto current=identity;
            while(current) {
                if(!seen.insert(current).second)throw std::logic_error("cyclic inline scope parents");
                const auto found=scopes_.find(current);
                if(found==scopes_.end())throw std::logic_error("missing inline scope parent");
                current=found->second->parent;
            }
            for(const auto& block:scope->blocks) {
                if(!labels.contains(block))throw std::logic_error("inline scope names a missing block");
                const auto old=blocks_.find(block);
                if(old==blocks_.end())blocks_.emplace(block,identity);
                else if(ancestor(old->second,identity))old->second=identity;
                else if(!ancestor(identity,old->second))throw std::logic_error("overlapping unrelated inline scopes");
            }
        }
        struct Definitions final:ir::OperandVisitor {
            InlineScopePlan& plan;std::uint64_t scope;
            Definitions(InlineScopePlan& p,std::uint64_t s):plan(p),scope(s){}
            void definition(ir::ValueId id)override {
                if(id && !plan.values_.emplace(id,scope).second)
                    throw std::logic_error("inline scope value definition is duplicated");
            }
        };
        for(const auto& block:function.blocks) {
            const auto scope=block_scope(block.label);
            Definitions definitions(*this,scope);
            for(const auto& instruction:block.instructions) {
                instructions_.emplace(&instruction,scope);
                ir::visit_operands(instruction,definitions);
            }
        }
    }
    const ir::InlineScope& at(std::uint64_t id)const{return *scopes_.at(id);}
    const auto& scopes()const{return scopes_;}
    std::uint64_t block_scope(const std::string& label)const {
        const auto found=blocks_.find(label);return found==blocks_.end()?0:found->second;
    }
    std::uint64_t slot_scope(const std::string& name)const {
        const auto found=slots_.find(name);return found==slots_.end()?0:found->second;
    }
    std::uint64_t instruction_scope(const ir::Instruction& instruction)const {
        const auto found=instructions_.find(&instruction);return found==instructions_.end()?0:found->second;
    }
    std::optional<std::uint64_t> value_scope(ir::ValueId value)const {
        const auto found=values_.find(value);return found==values_.end()?std::nullopt:std::optional(found->second);
    }
    bool enabled(std::uint64_t id,bool root)const {
        if(root)return true;
        while(id) {const auto& scope=at(id);if(scope.node_cleanup)return true;id=scope.parent;}
        return false;
    }
private:
    bool ancestor(std::uint64_t parent,std::uint64_t child)const {
        while(child) {if(child==parent)return true;child=at(child).parent;}return false;
    }
    std::map<std::uint64_t,const ir::InlineScope*> scopes_;
    std::map<std::string,std::uint64_t> blocks_,slots_;
    std::map<const ir::Instruction*,std::uint64_t> instructions_;
    std::map<ir::ValueId,std::uint64_t> values_;
};
} // namespace quidra::llvm_backend
