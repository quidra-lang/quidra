#include "llvm_backend/owned_shadow_emitter.hpp"
#include "llvm_backend/node_owned_type.hpp"
#include "llvm_backend/bare_integer.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/type_helpers.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "llvm_text/escape.hpp"
#include "llvm_text/function_text.hpp"
#include <limits>
#include <stdexcept>
namespace quidra::llvm_backend {
using namespace llvm_text;
using namespace llvm_text::types;
void OwnedShadowEmitter::reserve(const std::string& name,const Type& type,bool pin,const std::string& custom_release,std::uint64_t scope) {
    if(!pin && !node_owns_value(type))return;
    if(entry_emitted_)throw std::logic_error("node shadow reserved after entry");
    if(indices_.contains(name))throw std::logic_error("duplicate node owning shadow");
    const auto payload=pin?std::uint64_t{8}:std::uint64_t{runtime_storage_bytes(type)};
    if(payload>std::numeric_limits<std::uint64_t>::max()-31)
        throw std::logic_error("node owning shadow frame overflow");
    const auto stride=(header_bytes+payload+15)&~std::uint64_t{15};
    auto& bytes=scope_bytes_[scope];
    if(bytes>std::numeric_limits<std::uint64_t>::max()-stride)
        throw std::logic_error("node owning shadow frame overflow");
    indices_[name]=slots_.size();
    const auto index=slots_.size();
    if(prefix_.empty())throw std::logic_error("node shadow function identity missing");
    const auto prefix=prefix_+suffix(scope);
    slots_.push_back({name,type,pin,bytes,prefix+".drop."+std::to_string(index),prefix+".clear."+std::to_string(index),custom_release,scope});
    bytes+=stride;
}
std::string OwnedShadowEmitter::header(const std::string& name)const {
    return "%quidra.shadow."+std::to_string(indices_.at(name));
}
std::string OwnedShadowEmitter::payload(const std::string& name)const {
    return header(name)+".value";
}
std::string OwnedShadowEmitter::cell(const std::string& name)const{return header(name);}
std::string OwnedShadowEmitter::empty_value(const OwnedShadowSlot& slot)const {
    if(slot.pin)return "null";
    if(slot.type.kind==TypeKind::Real)return std::string(exact_real_none);
    if(is_bare_integer(slot.type))return "0";
    return is_pointer_runtime_type(slot.type)?"null":"zeroinitializer";
}
void OwnedShadowEmitter::prepare(const std::string& function) {
    if(!prefix_.empty())throw std::logic_error("node shadow prepared twice");
    prefix_=".quidra.shadow."+sanitize_identifier(function);
}
std::string OwnedShadowEmitter::entry_code(const std::string& function,std::uint64_t scope)const {
    FunctionText text;LlvmBuilder out(text);
    OwnedShadowEmitter copy(out,names_,layouts_);
    copy.indices_=indices_;copy.slots_=slots_;copy.prefix_=prefix_;copy.scope_bytes_=scope_bytes_;
    copy.entry(function,scope);return text.str();
}
std::string OwnedShadowEmitter::scope_allocations()const {
    FunctionText text;LlvmBuilder out(text);
    for(const auto& [scope,bytes]:scope_bytes_)if(scope)
        out.alloca_slot("%quidra.shadow.stack"+suffix(scope),LlvmType::array(bytes,i8),Align::sixteen);
    return text.str();
}
void OwnedShadowEmitter::entry(const std::string& function,std::uint64_t scope) {
    if(!has_scope(scope))return;
    if(prefix_.empty())prepare(function);
    entry_emitted_=true;
    const auto bytes=scope_bytes_.at(scope);
    const auto array=LlvmType::array(bytes,i8);
    if(!scope)builder_.alloca_slot("%quidra.shadow.stack",array,Align::sixteen);
    builder_.call(frame(scope),runtime_abi::program::parallel_frame_create,
        {{i64,bytes},{i64,16},{ptr,LlvmOperand::global(cleanup_symbol(scope))}});
    builder_.icmp("%quidra.shadow.active"+suffix(scope),IntPredicate::ne,ptr,frame(scope),"null");
    builder_.select(base(scope),"%quidra.shadow.active"+suffix(scope),
        {ptr,frame(scope)},{ptr,"%quidra.shadow.stack"+suffix(scope)});
    for(std::size_t i=0;i<slots_.size();++i) {
        auto& slot=slots_[i];
        if(slot.scope!=scope)continue;
        builder_.getelementptr(header(slot.name),Inbounds::no,i8,base(scope),{{i64,slot.offset}});
        builder_.getelementptr(payload(slot.name),Inbounds::no,i8,header(slot.name),{{i64,16}});
        builder_.store({ptr,"null"},header(slot.name),Align::none);
        builder_.store({slot.pin?ptr:llvm_type(slot.type),empty_value(slot)},payload(slot.name),Align::none);
    }
}
void OwnedShadowEmitter::publish(const std::string& name,const std::string& value) {
    const auto& s=slot(name);
    builder_.store({s.pin?ptr:llvm_type(s.type),value},payload(name),Align::none);
    builder_.store({ptr,LlvmOperand::global(s.drop_symbol)},header(name),Align::none);
}
void OwnedShadowEmitter::consume(const std::string& name) {
    const auto& s=slot(name);
    builder_.store({ptr,"null"},header(name),Align::none);
    builder_.store({s.pin?ptr:llvm_type(s.type),empty_value(s)},payload(name),Align::none);
}
CallArgumentOwnerLocation OwnedShadowEmitter::argument(const std::string& name)const {
    return {header(name),slot(name).clear_symbol};
}
void OwnedShadowEmitter::link_slot(const std::string& address,const std::string& owner,const std::string& clear,const std::string& view) {
    builder_.store({ptr,owner},address,Align::none);
    const auto code=names_.value("ownership.link.code");
    builder_.getelementptr(code,Inbounds::no,ptr,address,{{i64,1}});
    builder_.store({ptr,LlvmOperand::global(clear)},code,Align::none);
    const auto payload=names_.value("ownership.link.view");
    builder_.getelementptr(payload,Inbounds::no,ptr,address,{{i64,2}});
    builder_.store({ptr,view.empty()?owner:view},payload,Align::none);
}
void OwnedShadowEmitter::link(const std::string& address,const std::string& owner) {
    link_slot(address,header(owner),slot(owner).clear_symbol,payload(owner));
}
CallArgumentOwnerLocation OwnedShadowEmitter::argument_link(const std::string& address) {
    const auto owner=names_.value("ownership.alias"),code=names_.value("ownership.alias.code"),clear=names_.value("ownership.alias.clear");
    builder_.load(owner,ptr,address,Align::none);
    builder_.getelementptr(code,Inbounds::no,ptr,address,{{i64,1}});
    builder_.load(clear,ptr,code,Align::none);return {owner,clear,true};
}
std::string OwnedShadowEmitter::link_value_address(const std::string& address) {
    const auto slot=names_.value("ownership.view.slot"),view=names_.value("ownership.view");
    builder_.getelementptr(slot,Inbounds::no,ptr,address,{{i64,2}});
    builder_.load(view,ptr,slot,Align::none);return view;
}
void OwnedShadowEmitter::copy_link(const std::string& destination,const std::string& source) {
    const auto receipt=argument_link(source);
    builder_.store({ptr,receipt.stable_cell},destination,Align::none);
    const auto code=names_.value("ownership.alias.copy");
    builder_.getelementptr(code,Inbounds::no,ptr,destination,{{i64,1}});
    builder_.store({ptr,receipt.clear_symbol},code,Align::none);
    const auto view=link_value_address(source),payload=names_.value("ownership.view.copy");
    builder_.getelementptr(payload,Inbounds::no,ptr,destination,{{i64,2}});
    builder_.store({ptr,view},payload,Align::none);
}
void OwnedShadowEmitter::clear_link(const std::string& address) {
    builder_.store({ptr,"null"},address,Align::none);
    const auto code=names_.value("ownership.alias.empty");
    builder_.getelementptr(code,Inbounds::no,ptr,address,{{i64,1}});
    builder_.store({ptr,"null"},code,Align::none);
    const auto view=names_.value("ownership.view.empty");
    builder_.getelementptr(view,Inbounds::no,ptr,address,{{i64,2}});
    builder_.store({ptr,"null"},view,Align::none);
}
std::string OwnedShadowEmitter::consume_link(const std::string& address) {
    const auto receipt=argument_link(address);clear_link(address);
    const auto present=names_.value("ownership.origin.present");
    const auto clear=names_.label("ownership.origin.clear"),done=names_.label("ownership.origin.done");
    builder_.icmp(present,IntPredicate::ne,ptr,receipt.stable_cell,"null");builder_.br(present,clear,done);
    builder_.block(clear);
    // The paired callback clears either a SSA header or a typed local slot;
    // both are non-fallible and perform no release.
    builder_.call(void_type,LlvmOperand(receipt.clear_symbol),{{ptr,receipt.stable_cell}});
    builder_.br(done);builder_.block(done);return receipt.stable_cell;
}
void OwnedShadowEmitter::cleanup(std::uint64_t scope) {
    if(!has_scope(scope))return;
    builder_.call(runtime_abi::program::parallel_frame_cleanup,
        {{ptr,LlvmOperand::global(cleanup_symbol(scope))},{ptr,base(scope)}});
    builder_.call(runtime_abi::program::parallel_frame_leave,{{ptr,frame(scope)}});
}
std::string OwnedShadowEmitter::helpers()const {
    if(empty())return {};
    FunctionText text;LlvmBuilder out(text);
    for(const auto& slot:slots_) {
        const auto type=slot.pin?ptr:llvm_type(slot.type);
        LlvmFunction clear(void_type,"@"+slot.clear_symbol);clear.parameter(ptr,"","%cell");
        out.define(clear);out.block("entry");
        out.store({ptr,"null"},"%cell",Align::none);
        out.getelementptr("%payload",Inbounds::no,i8,"%cell",{{i64,16}});
        out.store({type,empty_value(slot)},"%payload",Align::none);out.ret_void();out.end_function();
        struct Leaf {Type type;std::vector<std::size_t> path;};
        std::vector<Leaf> leaves;
        const auto collect=[&](auto&& self,const Type& current,std::vector<std::size_t> path)->void {
            if(!node_owns_value(current))return;
            if(is_pointer_runtime_type(current) || current.kind==TypeKind::Real || is_bare_integer(current)) {
                leaves.push_back({current,std::move(path)});return;
            }
            // Unboxed aggregates use LLVM's naturally aligned struct layout;
            // only owned leaves create drop obligations, never the tuple itself.
            if(current.parameters.empty())throw std::logic_error("unknown node aggregate ownership layout");
            for(std::size_t i=0;i<current.parameters.size();++i) {
                auto child=path;child.push_back(i);self(self,current.parameters[i],std::move(child));
            }
        };
        if(!slot.pin && slot.custom_release.empty())collect(collect,slot.type,{});
        const bool aggregate=!slot.pin && slot.custom_release.empty() &&
            !is_pointer_runtime_type(slot.type) && slot.type.kind!=TypeKind::Real && !is_bare_integer(slot.type);
        if(aggregate) {
            // Each leaf clears its cell before dropping. frame_cleanup catches
            // a user-drop failure independently so later siblings still drain.
            for(std::size_t i=0;i<leaves.size();++i) {
                const auto& leaf=leaves[i];const auto leaf_type=llvm_type(leaf.type);
                LlvmFunction drop_leaf(void_type,"@"+slot.drop_symbol+".leaf."+std::to_string(i));
                drop_leaf.parameter(ptr,"","%leaf");out.define(drop_leaf);out.block("entry");
                out.load("%owned",leaf_type,"%leaf",Align::none);
                OwnedShadowSlot empty;empty.type=leaf.type;
                out.store({leaf_type,empty_value(empty)},"%leaf",Align::none);
                if(leaf.type.kind==TypeKind::Real)
                    out.call(void_type,LlvmOperand::global(real_helper::release),{{exact_real_type,"%owned"}});
                else if(is_bare_integer(leaf.type))
                    out.call(void_type,LlvmOperand::global(int_helper::release),{{bare_integer_type,"%owned"}});
                else out.call(runtime_abi::memory::managed_release,{{ptr,"%owned"},{ptr,drop_callback_for(leaf.type,layouts_)}});
                out.ret_void();out.end_function();
            }
        }
        LlvmFunction drop(void_type,"@"+slot.drop_symbol);drop.parameter(ptr,"","%cell");
        out.define(drop);out.block("entry");
        out.getelementptr("%payload",Inbounds::no,i8,"%cell",{{i64,16}});
        if(aggregate) {
            // Disable the aggregate obligation first, retaining payload bytes
            // until every leaf's no-fail clearing operation has read its value.
            out.store({ptr,"null"},"%cell",Align::none);
            for(std::size_t i=leaves.size();i;--i) {
                const auto& leaf=leaves[i-1];const auto suffix=std::to_string(i-1);
                std::string address="%payload";
                Type parent=slot.type;
                for(std::size_t depth=0;depth<leaf.path.size();++depth) {
                    const auto index=leaf.path[depth];
                    const auto child="%leaf."+suffix+"."+std::to_string(depth);
                    out.getelementptr(child,Inbounds::no,llvm_type(parent),address,{{i32,0},{i32,index}});
                    address=child;parent=parent.parameters.at(index);
                }
                out.call(runtime_abi::program::parallel_frame_cleanup,
                    {{ptr,LlvmOperand::global(slot.drop_symbol+".leaf."+suffix)},{ptr,address}});
            }
            out.store({type,empty_value(slot)},"%payload",Align::none);
        }else {
            out.load("%owned",type,"%payload",Align::none);
            out.call(void_type,LlvmOperand::global(slot.clear_symbol),{{ptr,"%cell"}});
            if(!slot.custom_release.empty())out.call(void_type,LlvmOperand::global(slot.custom_release),{{ptr,"%owned"}});
            else if(slot.pin)out.call(runtime_abi::memory::managed_unpin,{{ptr,"%owned"}});
            else if(slot.type.kind==TypeKind::Real)
                out.call(void_type,LlvmOperand::global(real_helper::release),{{exact_real_type,"%owned"}});
            else if(is_bare_integer(slot.type))
                out.call(void_type,LlvmOperand::global(int_helper::release),{{bare_integer_type,"%owned"}});
            else out.call(runtime_abi::memory::managed_release,
                {{ptr,"%owned"},{ptr,drop_callback_for(slot.type,layouts_)}});
        }
        out.ret_void();out.end_function();
    }
    for(const auto& [scope,bytes]:scope_bytes_) {
    (void)bytes;
    LlvmFunction cleanup(void_type,"@"+cleanup_symbol(scope));cleanup.parameter(ptr,"","%frame");
    out.define(cleanup);out.block("entry");
    for(std::size_t j=slots_.size();j;--j) {
        const auto& slot=slots_[j-1];if(slot.scope!=scope)continue;const auto suffix=std::to_string(j-1);
        const auto cell="%cell."+suffix,drop="%drop.callback."+suffix,live="%live."+suffix;
        const auto release="drop."+suffix,next="next."+suffix;
        out.getelementptr(cell,Inbounds::no,i8,"%frame",{{i64,slot.offset}});
        out.load(drop,ptr,cell,Align::none);out.icmp(live,IntPredicate::ne,ptr,drop,"null");
        out.br(live,release,next);out.block(release);
        out.call(runtime_abi::program::parallel_frame_cleanup,{{ptr,drop},{ptr,cell}});
        out.br(next);out.block(next);
    }
    out.ret_void();out.end_function();
    }
    return text.str();
}
}
