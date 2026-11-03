#include "llvm_backend/lifetime_emitter.hpp"

#include "llvm_backend/bare_integer.hpp"
#include "llvm_backend/copy_idioms.hpp"
#include "llvm_backend/small_rational.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/type_helpers.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "llvm_text/function_text.hpp"
#include <stdexcept>
#include <utility>
#include <vector>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

std::string LifetimeEmitter::drop_callback(const Type& type) const {
    return drop_callback_for(type, layouts_);
}

void LifetimeEmitter::release_value(const Type& type, const std::string& raw) {
    if(type.kind==TypeKind::Tuple) {
        for(std::size_t i=0;i<type.parameters.size();++i) if(owns_managed(type.parameters[i])) {
            const auto element=names_.value("tuple.release");
            builder_.extractvalue(element,llvm_type(type),raw,i);
            release_value(type.parameters[i],element);
        }
        return;
    }
    if (!requires_lifetime_management(type)) return;
    if (type.kind == TypeKind::Real) {
        builder_.call(void_type,LlvmOperand::global(real_helper::release),{{exact_real_type,raw}});
        return;
    }
    if (is_bare_integer(type)) {
        builder_.call(void_type,LlvmOperand::global(int_helper::release),{{bare_integer_type,raw}});
        return;
    }
    builder_.call(runtime_abi::memory::managed_release,{{ptr,raw},{ptr,drop_callback(type)}});
}

void LifetimeEmitter::release_tuple_slot(const Type& type,const std::string& address,Align alignment) {
    const auto active=names_.value("tuple.cleanup.active");
    const auto node=names_.label("tuple.cleanup.node");
    const auto ordinary=names_.label("tuple.cleanup.ordinary");
    const auto done=names_.label("tuple.cleanup.done");
    builder_.call(active,runtime_abi::program::parallel_cleanup_active,{});
    builder_.br(active,node,ordinary);
    builder_.block(node);
    // The helper clears actual leaf cells before individually captured drops.
    // Do not empty the whole tuple first: that would lose unprocessed siblings.
    builder_.call(void_type,drop_name(type),{{ptr,address}});
    builder_.call(runtime_abi::program::parallel_cleanup_rethrow,{});
    builder_.br(done);
    builder_.block(ordinary);
    const auto owned=names_.value("tuple.cleanup.value");
    builder_.load(owned,llvm_type(type),address,alignment);
    builder_.store({llvm_type(type),"zeroinitializer"},address,alignment);
    release_value(type,owned);
    builder_.br(done);
    builder_.block(done);
}

void LifetimeEmitter::cleanup_owned_values(){cleanup_scope(0);}
void LifetimeEmitter::cleanup_scope(std::uint64_t scope) {
    if(auto* hooks=symbols_.ownership_hooks())hooks->cleanup_scope(scope);
    for (const auto& [name, _] : symbols_.references()) {
        if(symbols_.storage_scope(name)!=scope || symbols_.is_captured_reference_slot(name))continue;
        const auto address = names_.value("cleanup.ref");
        builder_.load(address,ptr,symbols_.local(name),Align::none);
        if(symbols_.node_cleanup(scope)) builder_.store({ptr,"null"},symbols_.reference_slot(name),Align::none);
        builder_.call(runtime_abi::memory::managed_unpin,{{ptr,address}});
    }
    for (const auto& [name, type] : symbols_.locals()) {
        if (symbols_.storage_scope(name)!=scope || symbols_.is_writable_parameter(name) || symbols_.is_borrowed_parameter(name) ||
            symbols_.is_borrowed_local(name) || !owns_managed(type)) continue;
        if(type.kind==TypeKind::Tuple && symbols_.node_cleanup(scope)) {
            release_tuple_slot(type,symbols_.local(name),Align::none);
            continue;
        }
        const auto owned = names_.value("cleanup.value");
        builder_.load(owned,llvm_type(type),symbols_.local(name),Align::none);
        if(symbols_.node_cleanup(scope)) {
            const auto empty=type.kind==TypeKind::Tuple?std::string("zeroinitializer"):type.kind==TypeKind::Real?std::string(exact_real_none):
                is_bare_integer(type)?std::string("0"):std::string("null");
            builder_.store({llvm_type(type),empty},symbols_.local(name),Align::none);
        }
        release_value(type, owned);
    }
    if(symbols_.node_cleanup(scope))
        builder_.call(runtime_abi::program::parallel_frame_leave,{{ptr,symbols_.frame(scope)}});
}

std::string LifetimeEmitter::node_cleanup_helpers()const {
    std::string result;
    for(const auto& [scope,plan]:symbols_.cleanup_plans()){(void)plan;result+=node_cleanup_helpers(scope);}
    return result;
}
std::string LifetimeEmitter::node_cleanup_helpers(std::uint64_t scope)const {
    if(!symbols_.node_cleanup(scope))return {};
    FunctionText text;
    LlvmBuilder out(text);
    const auto emit_leaf=[&](const std::string& symbol,const Type& type,bool pin) {
        const auto lowered=pin?ptr:llvm_type(type);
        const auto empty=pin?std::string("null"):type.kind==TypeKind::Real?std::string(exact_real_none):
            is_bare_integer(type)?std::string("0"):std::string("null");
        LlvmFunction callback(void_type,"@"+symbol);
        callback.parameter(ptr,"","%slot");out.define(callback);out.block("entry");
        out.load("%owned",lowered,"%slot",Align::none);
        out.store({lowered,empty},"%slot",Align::none);
        if(pin) out.call(runtime_abi::memory::managed_unpin,{{ptr,"%owned"}});
        else if(type.kind==TypeKind::Real)
            out.call(void_type,LlvmOperand::global(real_helper::release),{{exact_real_type,"%owned"}});
        else if(is_bare_integer(type))
            out.call(void_type,LlvmOperand::global(int_helper::release),{{bare_integer_type,"%owned"}});
        else out.call(runtime_abi::memory::managed_release,{{ptr,"%owned"},{ptr,drop_callback(type)}});
        out.ret_void();out.end_function();
    };
    for(const auto& slot:symbols_.cleanup_slots(scope)) {
        if(slot.pin || slot.type.kind!=TypeKind::Tuple) {
            emit_leaf(slot.drop_symbol,slot.type,slot.pin);
            continue;
        }
        struct Leaf {Type type;std::vector<std::size_t> path;};
        std::vector<Leaf> leaves;
        const auto collect=[&](auto&& self,const Type& type,std::vector<std::size_t> path)->void {
            if(!owns_managed(type))return;
            if(type.kind!=TypeKind::Tuple){leaves.push_back({type,std::move(path)});return;}
            for(std::size_t i=0;i<type.parameters.size();++i) {
                auto child=path;child.push_back(i);self(self,type.parameters[i],std::move(child));
            }
        };
        collect(collect,slot.type,{});
        for(std::size_t i=0;i<leaves.size();++i)
            emit_leaf(slot.drop_symbol+".leaf."+std::to_string(i),leaves[i].type,false);
        LlvmFunction callback(void_type,"@"+slot.drop_symbol);
        callback.parameter(ptr,"","%slot");out.define(callback);out.block("entry");
        // A failing user drop is captured separately for each leaf so every
        // sibling drains. Each leaf clears its actual typed cell before drop.
        for(std::size_t i=0;i<leaves.size();++i) {
            std::string address="%slot";Type parent=slot.type;
            for(std::size_t depth=0;depth<leaves[i].path.size();++depth) {
                const auto index=leaves[i].path[depth];
                const auto child="%leaf."+std::to_string(i)+"."+std::to_string(depth);
                out.getelementptr(child,Inbounds::no,llvm_type(parent),address,{{i32,0},{i32,index}});
                address=child;parent=parent.parameters.at(index);
            }
            out.call(runtime_abi::program::parallel_frame_cleanup,
                {{ptr,LlvmOperand::global(slot.drop_symbol+".leaf."+std::to_string(i))},{ptr,address}});
        }
        out.ret_void();out.end_function();
    }
    LlvmFunction cleanup(void_type,"@"+symbols_.cleanup_symbol(scope));
    cleanup.parameter(ptr,"","%frame");out.define(cleanup);out.block("entry");
    for(std::size_t i=0;i<symbols_.cleanup_slots(scope).size();++i) {
        const auto& slot=symbols_.cleanup_slots(scope)[i];
        const auto address="%slot."+std::to_string(i);
        out.getelementptr(address,Inbounds::yes,i8,"%frame",{{i64,slot.offset}});
        out.call(runtime_abi::program::parallel_frame_cleanup,
            {{ptr,LlvmOperand::global(slot.drop_symbol)},{ptr,address}});
    }
    out.ret_void();out.end_function();return text.str();
}

void LifetimeEmitter::emit(const ir::Clone& n,const ir::Instruction&){
    if(n.type.kind==TypeKind::Tuple) throw std::logic_error("tuple clone must expand into elements");
    symbols_.set_type(n.out,n.type);
    builder_.call(symbols_.value(n.out),ptr,clone_name(n.type),{{ptr,symbols_.value(n.value)}});
    symbols_.acquire_owned(n.out,n.type,symbols_.value(n.out));
}

void LifetimeEmitter::emit(const ir::Retain& n,const ir::Instruction&){
    if(n.type.kind==TypeKind::Tuple) throw std::logic_error("tuple retain must expand into elements");
    symbols_.set_type(n.out,n.type);
    if(n.type.kind==TypeKind::Real){
        builder_.call(void_type,LlvmOperand::global(real_helper::retain),{{exact_real_type,symbols_.value(n.value)}});
        copy_by_select_true(builder_,symbols_.value(n.out),exact_real_type,symbols_.value(n.value));
        symbols_.acquire_owned(n.out,n.type,symbols_.value(n.out));
        return;
    }
    if(is_bare_integer(n.type)){
        builder_.call(void_type,LlvmOperand::global(int_helper::retain),{{bare_integer_type,symbols_.value(n.value)}});
        copy_by_add_zero(builder_,symbols_.value(n.out),bare_integer_type,symbols_.value(n.value));
        symbols_.acquire_owned(n.out,n.type,symbols_.value(n.out));
        return;
    }
    builder_.call(runtime_abi::memory::managed_retain,{{ptr,symbols_.value(n.value)}});
    copy_by_zero_gep(builder_,symbols_.value(n.out),Inbounds::yes,symbols_.value(n.value));
    symbols_.acquire_owned(n.out,n.type,symbols_.value(n.out));
}

void LifetimeEmitter::emit(const ir::Release& n,const ir::Instruction&){
    symbols_.transfer_owned(n.value,n.type);
    if(n.type.kind==TypeKind::Tuple) throw std::logic_error("tuple release must expand into elements");
    release_value(n.type,symbols_.value(n.value));
}

void LifetimeEmitter::emit(const ir::Pin& n,const ir::Instruction& ins){
    builder_.call(runtime_abi::memory::managed_pin,{{ptr,symbols_.value(n.value)}});
    if(auto* hooks=symbols_.ownership_hooks())hooks->pin(ins,n.value);
}

void LifetimeEmitter::emit(const ir::Unpin& n,const ir::Instruction& ins){
    if(auto* hooks=symbols_.ownership_hooks())hooks->unpin(ins,n.value);
    builder_.call(runtime_abi::memory::managed_unpin,{{ptr,symbols_.value(n.value)}});
}

} // namespace quidra::llvm_backend
