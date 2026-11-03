#pragma once
#include "llvm_backend/node_owned_type.hpp"
#include "llvm_backend/inline_scope_plan.hpp"

// SymbolTable: the names and types of one function's storage.
//
// Owns: the type of every IR value (%vN), the alloca type of every local
// (%local.X) and reference, which parameters are passed by address (%arg.X),
// and which parameters and locals hold borrowed values that the function
// does not release.
//
// The verbs follow the container idioms the output depends on. set_type,
// declare_parameter and record_storage assign, so the last write wins: the
// last store to a local decides the type of its entry-frame alloca. type_at
// reads with .at and throws on an unknown value. Emitters keep the reference
// type_at returns while set_type inserts another value; the unordered_map is
// node-based, so a rehash leaves the reference valid, and it must never move
// to dense storage. locals and references are std::map: the entry frame and
// the cleanup at function exits walk them in name order.

#include "quidra/ir/module.hpp"
#include "llvm_backend/node_ownership_hooks.hpp"
#include "llvm_text/escape.hpp"

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace quidra::llvm_backend {

struct NodeOwnedSlot {
    std::string name;
    Type type;
    bool pin{};
    std::uint64_t offset{};
    std::string drop_symbol;
};

class SymbolTable {
public:
    void inline_scopes(const InlineScopePlan& scopes) {
        inline_scopes_=&scopes;
        for(const auto& [id,scope]:scopes.scopes())for(const auto& slot:scope->slots) {
            switch(slot.kind) {
                case ir::InlineSlotKind::OwnedLocal:locals_[slot.name]=slot.type;break;
                case ir::InlineSlotKind::BorrowedLocal:locals_[slot.name]=slot.type;borrowed_locals_.insert(slot.name);break;
                case ir::InlineSlotKind::PinnedReference:references_[slot.name]=slot.type;break;
                case ir::InlineSlotKind::RawAddress:locals_[slot.name]=slot.type;writable_params_.insert(slot.name);break;
                default:throw std::logic_error("unsupported inline ABI slot kind");
            }
            (void)id;
        }
    }
    void select_scope(std::uint64_t scope){scope_=scope;}
    void select_block(const std::string& label){scope_=inline_scopes_?inline_scopes_->block_scope(label):0;}
    std::uint64_t selected_scope()const{return scope_;}
    std::uint64_t storage_scope(const std::string& name)const{return inline_scopes_?inline_scopes_->slot_scope(name):0;}
    std::string frame(std::uint64_t scope)const{return "%quidra.node.frame"+scope_suffix(scope);}
    std::string frame_active(std::uint64_t scope)const{return frame(scope)+".active";}
    static std::string scope_suffix(std::uint64_t scope){return scope?".inline."+std::to_string(scope):"";}
    void node_ownership(NodeOwnershipHooks* hooks){ownership_=hooks;}
    bool has_node_ownership()const{return ownership_ && ownership_->active();}
    NodeOwnershipHooks* ownership_hooks()const{return ownership_;}
    void acquire_owned(ir::ValueId value,const Type& type,const std::string& raw){
        if(ownership_ && node_owns_value(type))ownership_->acquire(value,type,raw);
    }
    void acquire_custom(ir::ValueId value,const Type& type,const std::string& raw,const std::string& release){
        if(ownership_)ownership_->acquire_custom(value,type,raw,release);
    }
    void acquire_custom_temporary(const std::string& raw,const Type& type,const std::string& release){
        if(ownership_)ownership_->acquire_custom_temporary(raw,type,release);
    }
    void tuple_projection(ir::ValueId value,ir::ValueId tuple,std::size_t index,const Type& type,bool take){
        if(ownership_ && node_owns_value(type))ownership_->tuple_projection(value,tuple,index,type,take);
    }
    void storage_origin(ir::ValueId value,const std::string& address,const Type& type){
        if(ownership_ && node_owns_value(type))ownership_->storage_origin(value,address,type);
    }
    void alias_owned(ir::ValueId value,ir::ValueId source,const Type& type){
        if(ownership_ && node_owns_value(type))ownership_->alias_value(value,source,type);
    }
    void borrowed_value(ir::ValueId value){if(ownership_)ownership_->borrowed_value(value);}
    void acquire_temporary(const std::string& raw,const Type& type){
        if(ownership_ && node_owns_value(type))ownership_->acquire_temporary(raw,type);
    }
    void transfer_owned(ir::ValueId value,const Type& type){
        if(ownership_ && node_owns_value(type))ownership_->transfer(value,type);
    }
    void transfer_temporary(const std::string& raw,const Type& type){
        if(ownership_ && node_owns_value(type))ownership_->transfer_temporary(raw,type);
    }
    void bind_borrowed_owner(const std::string& name,ir::ValueId value,const Type& type){
        if(ownership_ && node_owns_value(type))ownership_->bind_borrowed_local(name,value,type);
    }
    void load_borrowed_owner(ir::ValueId value,const std::string& name,const Type& type){
        if(ownership_ && node_owns_value(type))ownership_->load_borrowed_local(value,name,type);
    }
    std::string value(ir::ValueId id)const{return "%v"+std::to_string(id);}
    std::string local(const std::string&n)const{return "%local."+llvm_text::sanitize_identifier(n);}
    std::string arg(const std::string&n)const{return "%arg."+llvm_text::sanitize_identifier(n);}
    std::string storage(const std::string&n)const{return writable_params_.contains(n)?arg(n):local(n);}

    std::string reference_slot(const std::string& name)const{return captured_reference_slots_.contains(name)?arg(name):local(name);}
    void capture_reference_slot(const std::string& name){captured_reference_slots_.insert(name);}
    bool is_captured_reference_slot(const std::string& name)const{return captured_reference_slots_.contains(name);}
    void set_type(ir::ValueId id,const Type& type){values_[id]=type;}
    void set_type(ir::ValueId id,Type&& type){values_[id]=std::move(type);}
    const Type& type_at(ir::ValueId id)const{return values_.at(id);}

    // The storage a parameter or an instruction (StoreLocal, DeclareLocal,
    // DeclareReference) gives the function; called in function order before
    // any instruction is emitted. Defined in the class, like the rest of the
    // table: the pre-pass calls record_storage for every instruction.
    void declare_parameter(const ir::Parameter& p){
        locals_[p.name]=p.type;
        if(p.writable) writable_params_.insert(p.name);
        if(p.borrowed) borrowed_params_.insert(p.name);
    }
    void record_storage(const ir::Instruction& i){
        if(const auto* s=std::get_if<ir::StoreLocal>(&i)){
            locals_[s->name]=s->type;
            if(s->borrowed) borrowed_locals_.insert(s->name);
        }
        if(const auto* d=std::get_if<ir::DeclareLocal>(&i)) locals_[d->name]=d->type;
        if(const auto* r=std::get_if<ir::DeclareReference>(&i)) references_[r->name]=r->type;
    }

    void plan_node_cleanup(const std::string& function,std::uint64_t scope=0) {
        auto& plan=cleanup_plans_[scope];
        plan.symbol=".quidra.node.cleanup."+llvm_text::sanitize_identifier(function)+scope_suffix(scope);
        const auto append=[&](const std::string& name,const Type& type,bool pin) {
            plan.bytes=(plan.bytes+7)&~std::uint64_t{7};
            const auto index=plan.slots.size();
            plan.slots.push_back(NodeOwnedSlot{name,type,pin,plan.bytes,plan.symbol+".slot."+std::to_string(index)});
            plan.bytes+=pin?8:runtime_storage_bytes(type);
        };
        for(const auto& [name,type]:locals_)
            if(storage_scope(name)==scope && !is_writable_parameter(name) && !is_borrowed_parameter(name) &&
               !is_borrowed_local(name) && node_owns_value(type))append(name,type,false);
        for(const auto& [name,type]:references_)
            if(storage_scope(name)==scope && !is_captured_reference_slot(name))append(name,type,true);
    }
    const std::vector<NodeOwnedSlot>& cleanup_slots(std::uint64_t scope=0)const {
        static const std::vector<NodeOwnedSlot> empty;
        const auto found=cleanup_plans_.find(scope);return found==cleanup_plans_.end()?empty:found->second.slots;
    }
    const NodeOwnedSlot* cleanup_slot(const std::string& name)const {
        for(const auto& slot:cleanup_slots(storage_scope(name)))if(slot.name==name)return &slot;
        return nullptr;
    }
    const std::string& cleanup_symbol(std::uint64_t scope=0)const{return cleanup_plans_.at(scope).symbol;}
    std::uint64_t cleanup_bytes(std::uint64_t scope=0)const{return cleanup_plans_.at(scope).bytes;}
    bool node_cleanup()const{return node_cleanup(scope_);}
    bool node_cleanup(std::uint64_t scope)const{return !cleanup_slots(scope).empty();}
    const auto& cleanup_plans()const{return cleanup_plans_;}
    const std::map<std::string,Type>& locals()const{return locals_;}
    const std::map<std::string,Type>& references()const{return references_;}
    bool is_writable_parameter(const std::string& name)const{return writable_params_.contains(name);}
    bool is_borrowed_parameter(const std::string& name)const{return borrowed_params_.contains(name);}
    bool is_borrowed_local(const std::string& name)const{return borrowed_locals_.contains(name);}

private:
    NodeOwnershipHooks* ownership_{};
    struct CleanupPlan {std::vector<NodeOwnedSlot> slots;std::uint64_t bytes{};std::string symbol;};
    std::map<std::uint64_t,CleanupPlan> cleanup_plans_;
    const InlineScopePlan* inline_scopes_{};
    std::uint64_t scope_{};
    std::unordered_map<ir::ValueId,Type> values_;
    std::map<std::string,Type> locals_;
    std::map<std::string,Type> references_;
    std::unordered_set<std::string> writable_params_;
    std::unordered_set<std::string> captured_reference_slots_;
    std::unordered_set<std::string> borrowed_params_;
    std::unordered_set<std::string> borrowed_locals_;
};

} // namespace quidra::llvm_backend
