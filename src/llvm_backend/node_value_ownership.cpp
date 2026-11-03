#include "llvm_backend/node_value_ownership.hpp"
#include "llvm_backend/node_owned_type.hpp"
#include "llvm_backend/owned_result_contracts.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_text/escape.hpp"
#include "llvm_text/function_text.hpp"
#include <stdexcept>
namespace quidra::llvm_backend {
using namespace llvm_text;
using namespace llvm_text::types;
NodeValueOwnership::NodeValueOwnership(const ir::Function& function,SymbolTable& symbols,
    LlvmBuilder& builder,FunctionText& function_text,TemporaryNames& names,const std::unordered_map<std::string,ir::ClassLayout>& layouts,
    const InlineScopePlan& scopes,bool root_cleanup)
    :function_(function),scopes_(scopes),root_cleanup_(root_cleanup),symbols_(symbols),builder_(builder),function_text_(function_text),names_(names),shadow_(builder,names,layouts),pin_plan_(function,scopes,root_cleanup) {
    shadow_.prepare(function.name);
    for(const auto& key:pin_plan_.acquisition_order)
        record(key,Type::simple(TypeKind::String),true,pin_plan_.scopes.at(key));
    // A borrowed aggregate holder may receive a foreign view on a backedge
    // after an earlier load has already been emitted. Until the final IR has
    // a whole-CFG origin proof, explicit tuple takes use owning SSA/local
    // storage instead; never infer proof from emitter traversal order.
    for(const auto& block:function.blocks)for(const auto& instruction:block.instructions)
        if(const auto* store=std::get_if<ir::StoreLocal>(&instruction))
            if(store->borrowed && node_owns_value(store->type) &&
               !is_pointer_runtime_type(store->type) && !is_bare_integer(store->type))
                unproven_holders_.insert(store->name);
}
void NodeValueOwnership::record(const std::string& raw,const Type& type,bool pin,std::optional<std::uint64_t> scope) {
    const auto owner=scope.value_or(symbols_.selected_scope());
    if(!scopes_.enabled(owner,root_cleanup_))return;
    if(!pin && !node_owns_value(type))return;
    const auto found=owning_.find(raw);
    if(found!=owning_.end()) {
        if(found->second!=type)throw std::logic_error("conflicting node temporary ownership types");
        return;
    }
    owning_.emplace(raw,type);shadow_.reserve(raw,type,pin,{},owner);
}
void NodeValueOwnership::acquire(ir::ValueId id,const Type& type,const std::string& raw) {
    if(!active())return;
    if(raw!=value(id))throw std::logic_error("node result ownership raw name mismatch");
    acquire_temporary(raw,type);
}
void NodeValueOwnership::acquire_custom(ir::ValueId id,const Type& type,const std::string& raw,const std::string& release) {
    if(!active())return;
    if(raw!=value(id) || owning_.contains(raw))throw std::logic_error("invalid custom node acquisition");
    owning_.emplace(raw,type);shadow_.reserve(raw,type,false,release,symbols_.selected_scope());shadow_.publish(raw,raw);
}
void NodeValueOwnership::acquire_custom_temporary(const std::string& raw,const Type& type,const std::string& release) {
    if(!active())return;
    if(owning_.contains(raw))throw std::logic_error("duplicate custom node temporary");
    owning_.emplace(raw,type);shadow_.reserve(raw,type,false,release,symbols_.selected_scope());shadow_.publish(raw,raw);
}
void NodeValueOwnership::tuple_projection(ir::ValueId id,ir::ValueId tuple,std::size_t index,const Type& type,bool take) {
    if(!active())return;
    const auto raw=value(tuple);
    if(unproven_origins_.contains(tuple)) {
        if(!take){borrowed_value(id);return;}
        throw std::logic_error("parallel tuple take has an unproven borrowed origin");
    }
    std::string parent;
    if(owning_.contains(raw))parent=shadow_.value_address(raw);
    else if(const auto found=aliases_.find(tuple);found!=aliases_.end())parent=shadow_.link_value_address(found->second);
    else {
        if(!take){borrowed_value(id);return;}
        throw std::logic_error("parallel tuple take lacks a stable owning payload origin");
    }
    const auto child=names_.value("ownership.tuple.child");
    builder_.getelementptr(child,Inbounds::no,llvm_type(symbols_.type_at(tuple)),parent,{{i32,0},{i32,index}});
    if(take) {
        acquire(id,type,value(id));
        const auto empty=type.kind==TypeKind::Real?std::string(exact_real_none):
            is_bare_integer(type)?"0":is_pointer_runtime_type(type)?"null":"zeroinitializer";
        // Publication and source clearing contain no call/fallible operation.
        // Whole nested subtree moves preserve all siblings in the parent slot.
        builder_.store({llvm_type(type),empty},child,Align::none);
    }else storage_origin(id,child,type);
}
void NodeValueOwnership::storage_origin(ir::ValueId id,const std::string& address,const Type& type) {
    if(!active())return;
    const auto clear=".quidra.origin.clear."+sanitize_identifier(function_.name)+"."+std::to_string(id);
    local_clears_.emplace(clear,type);shadow_.link_slot(alias_cell(id),address,clear);
}
void NodeValueOwnership::alias_value(ir::ValueId id,ir::ValueId source,const Type&) {
    if(!active())return;
    const auto destination=alias_cell(id),raw=value(source);
    if(unproven_origins_.contains(source))unproven_origins_.insert(id);
    if(owning_.contains(raw))shadow_.link(destination,raw);
    else if(const auto found=aliases_.find(source);found!=aliases_.end())shadow_.copy_link(destination,found->second);
    else {shadow_.clear_link(destination);borrowed_value(id);}
}
void NodeValueOwnership::acquire_temporary(const std::string& raw,const Type& type) {
    if(!active())return;
    if(!node_owns_value(type))return;
    record(raw,type);shadow_.publish(raw,raw);
}
void NodeValueOwnership::transfer_temporary(const std::string& raw,const Type& type) {
    if(!node_owns_value(type))return;
    if(!owning_.contains(raw)) {if(!active())return;throw std::logic_error("node transfer lacks an owning temporary contract");}
    shadow_.consume(raw);
}
void NodeValueOwnership::transfer(ir::ValueId id,const Type& type) {
    if(!node_owns_value(type) || borrowed_.contains(id))return;
    const auto raw=value(id);
    if(owning_.contains(raw)) {shadow_.consume(raw);return;}
    if(const auto found=aliases_.find(id);found!=aliases_.end()) {
        shadow_.consume_link(found->second);return;
    }
    // A value evaluated in an ordinary outer caller has no node shadow.
    // Its explicit owning StoreLocal into the callee adopts that existing
    // obligation; there is no foreign shadow receipt to consume.
    if(const auto owner=scopes_.value_scope(id);owner && !scopes_.enabled(*owner,root_cleanup_))return;
    if(!active())return;
    // Constants are immortal borrowed values. Managed loads of foreign or
    // shared storage are not proven moves and must not disable other owners.
    bool constant=false;
    for(const auto& block:function_.blocks)for(const auto& instruction:block.instructions)
        std::visit([&](const auto& n) {
            using N=std::decay_t<decltype(n)>;
            if constexpr(std::is_same_v<N,ir::ConstantString> || std::is_same_v<N,ir::ConstantExact> ||
                         std::is_same_v<N,ir::ConstantInt> || std::is_same_v<N,ir::ConstantFloat>) {
                if(n.out==id)constant=true;
            } else if constexpr(requires{n.out;}) {
                if(n.out==id) {
                    const auto contract=node_result_contract(instruction);
                    if(contract && contract->owner==ResultOwner::borrowed)constant=true;
                }
            }
        },instruction);
    if(!constant)throw std::logic_error("parallel managed move lacks an owning origin");
}
std::string NodeValueOwnership::local_link(const std::string& name) {
    const auto found=local_links_.find(name);if(found!=local_links_.end())return found->second;
    const auto slot="%quidra.owner.local."+std::to_string(local_links_.size());
    local_links_[name]=slot;link_slots_.push_back({slot,symbols_.storage_scope(name)});return slot;
}
std::string NodeValueOwnership::alias_cell(ir::ValueId id) {
    const auto found=aliases_.find(id);if(found!=aliases_.end())return found->second;
    const auto slot="%quidra.owner.value."+std::to_string(id);
    aliases_[id]=slot;link_slots_.push_back({slot,scopes_.value_scope(id).value_or(symbols_.selected_scope())});return slot;
}
void NodeValueOwnership::bind_borrowed_local(const std::string& name,ir::ValueId id,const Type&) {
    if(!active())return;
    const auto destination=local_link(name),raw=value(id);
    if(unproven_origins_.contains(id) || (!owning_.contains(raw) && !aliases_.contains(id)))
        unproven_holders_.insert(name);
    if(owning_.contains(raw))shadow_.link(destination,raw);
    else if(const auto source=aliases_.find(id);source!=aliases_.end())shadow_.copy_link(destination,source->second);
    else shadow_.clear_link(destination);
}
void NodeValueOwnership::load_borrowed_local(ir::ValueId id,const std::string& name,const Type& type) {
    if(!active())return;
    if(symbols_.is_borrowed_local(name)) {
        shadow_.copy_link(alias_cell(id),local_link(name));
        if(unproven_holders_.contains(name))unproven_origins_.insert(id);
    }
    else if(symbols_.is_writable_parameter(name)) {
        // A writable ABI binding is the actual caller storage, not a node
        // owned value. A consuming operation must clear that same address.
        storage_origin(id,symbols_.storage(name),type);
    }
    else if(const auto* slot=symbols_.cleanup_slot(name);slot && !slot->pin) {
        const auto clear=".quidra.local.clear."+sanitize_identifier(function_.name)+"."+sanitize_identifier(name);
        local_clears_.emplace(clear,type);shadow_.link_slot(alias_cell(id),symbols_.storage(name),clear);
    }
}
void NodeValueOwnership::pin(const ir::Instruction& instruction,ir::ValueId target) {
    if(!active())return;
    shadow_.publish(pin_plan_.acquisitions.at(&instruction),value(target));
}
void NodeValueOwnership::unpin(const ir::Instruction& instruction,ir::ValueId target) {
    if(!active())return;
    if(std::get<ir::Unpin>(instruction).value!=target)
        throw std::logic_error("parallel unpin instruction target mismatch");
    const auto found=pin_plan_.releases.find(&instruction);
    // An unreachable original block carries no executed acquisition. Tail
    // continuation cloning permits scalar loads/stores/returns only, so it
    // cannot introduce a Pin or Unpin outside this original-IR plan.
    if(found==pin_plan_.releases.end())return;
    for(const auto& key:found->second)if(owning_.contains(key))shadow_.consume(key);
}
CallArgumentOwnerLocation NodeValueOwnership::argument(ir::ValueId id,const Type& type) {
    if(!node_owns_value(type))return {};
    const auto raw=value(id);
    if(owning_.contains(raw))return shadow_.argument(raw);
    if(unproven_origins_.contains(id))throw std::logic_error("parallel consuming argument has an unproven borrowed origin");
    if(const auto found=aliases_.find(id);found!=aliases_.end())return shadow_.argument_link(found->second);
    throw std::logic_error("parallel consuming argument lacks a proven owning origin");
}
void NodeValueOwnership::certify_result(const ir::Instruction& instruction) {
    if(!active())return;
    std::visit([&](const auto& n) {
        if constexpr(requires{n.out;}) {
            if(!n.out)return;
            const auto contract=node_result_contract(instruction);
            if(contract && (contract->owner==ResultOwner::borrowed || contract->owner==ResultOwner::alias ||
                            contract->owner==ResultOwner::unmanaged))return;
            // Domain set_type has the emitted type; address-only operations
            // deliberately have none and cannot own a managed result.
            const Type* type=nullptr;
            try {type=&symbols_.type_at(n.out);}catch(const std::out_of_range&){return;}
            if(node_owns_value(*type) && !owning_.contains(value(n.out)))
                throw std::logic_error("parallel managed result lacks early publication hooks");
        }
    },instruction);
}
std::string NodeValueOwnership::scope_entry_marker(std::uint64_t scope) {
    return scope?"  ; quidra.node.ownership.inline."+std::to_string(scope)+".entry\n":entry_marker;
}
std::string NodeValueOwnership::scope_cleanup_marker(std::uint64_t scope) {
    return scope?"  ; quidra.node.ownership.inline."+std::to_string(scope)+".cleanup\n":cleanup_marker;
}
void NodeValueOwnership::cleanup(){cleanup_scope(symbols_.selected_scope());}
void NodeValueOwnership::cleanup_scope(std::uint64_t scope){function_text_.append(scope_cleanup_marker(scope));}
std::string NodeValueOwnership::finish_cleanup(std::uint64_t scope)const {
    FunctionText text;LlvmBuilder out(text);
    if(shadow_.has_scope(scope)) {
        out.call(runtime_abi::program::parallel_frame_cleanup,
            {{ptr,LlvmOperand::global(shadow_.cleanup_symbol(scope))},{ptr,shadow_.base(scope)}});
        out.call(runtime_abi::program::parallel_frame_leave,{{ptr,shadow_.frame(scope)}});
    }
    return text.str();
}
std::string NodeValueOwnership::finish_entry(std::uint64_t scope)const {
    FunctionText text;LlvmBuilder out(text);
    for(const auto& [slot,owner]:link_slots_) {
        if(!scope)out.alloca_slot(slot,LlvmType::array(3,ptr),Align::none);
        if(owner==scope)out.store({LlvmType::array(3,ptr),"zeroinitializer"},slot,Align::none);
    }
    return shadow_.entry_code(function_.name,scope)+text.str()+(scope?std::string{}:shadow_.scope_allocations());
}
std::string NodeValueOwnership::helpers()const {
    FunctionText text;LlvmBuilder out(text);
    for(const auto& [symbol,type]:local_clears_) {
        LlvmFunction clear(void_type,"@"+symbol);clear.parameter(ptr,"","%slot");
        out.define(clear);out.block("entry");
        const auto empty=type.kind==TypeKind::Real?std::string(exact_real_none):
            is_bare_integer(type)?"0":is_pointer_runtime_type(type)?"null":"zeroinitializer";
        out.store({llvm_type(type),empty},"%slot",Align::none);out.ret_void();out.end_function();
    }
    return shadow_.helpers()+text.str();
}

}
