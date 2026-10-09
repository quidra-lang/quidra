#include "llvm_backend/lifetime_emitter.hpp"

#include "llvm_backend/bare_integer.hpp"
#include "llvm_backend/copy_idioms.hpp"
#include "llvm_backend/small_rational.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/type_helpers.hpp"
#include "llvm_backend/type_lowering.hpp"

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

std::string LifetimeEmitter::drop_callback(const Type& type) const {
    return drop_callback_for(type, layouts_);
}

void LifetimeEmitter::release_value(const Type& type, const std::string& raw) {
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

void LifetimeEmitter::cleanup_owned_values() {
    for (const auto& [name, _] : symbols_.references()) {
        const auto address = names_.value("cleanup.ref");
        builder_.load(address,ptr,symbols_.local(name),Align::none);
        builder_.call(runtime_abi::memory::managed_unpin,{{ptr,address}});
    }
    for (const auto& [name, type] : symbols_.locals()) {
        if (symbols_.is_writable_parameter(name) || symbols_.is_borrowed_parameter(name) ||
            symbols_.is_borrowed_local(name) || !requires_lifetime_management(type)) continue;
        const auto owned = names_.value("cleanup.value");
        builder_.load(owned,llvm_type(type),symbols_.local(name),Align::none);
        release_value(type, owned);
    }
}

void LifetimeEmitter::emit(const ir::Clone& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    builder_.call(symbols_.value(n.out),ptr,clone_name(n.type),{{ptr,symbols_.value(n.value)}});
}

void LifetimeEmitter::emit(const ir::Retain& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.type);
    if(n.type.kind==TypeKind::Real){
        builder_.call(void_type,LlvmOperand::global(real_helper::retain),{{exact_real_type,symbols_.value(n.value)}});
        copy_by_select_true(builder_,symbols_.value(n.out),exact_real_type,symbols_.value(n.value));
        return;
    }
    if(is_bare_integer(n.type)){
        builder_.call(void_type,LlvmOperand::global(int_helper::retain),{{bare_integer_type,symbols_.value(n.value)}});
        copy_by_add_zero(builder_,symbols_.value(n.out),bare_integer_type,symbols_.value(n.value));
        return;
    }
    builder_.call(runtime_abi::memory::managed_retain,{{ptr,symbols_.value(n.value)}});
    copy_by_zero_gep(builder_,symbols_.value(n.out),Inbounds::yes,symbols_.value(n.value));
}

void LifetimeEmitter::emit(const ir::Release& n,const ir::Instruction&){
    release_value(n.type,symbols_.value(n.value));
}

void LifetimeEmitter::emit(const ir::Pin& n,const ir::Instruction&){
    builder_.call(runtime_abi::memory::managed_pin,{{ptr,symbols_.value(n.value)}});
}

void LifetimeEmitter::emit(const ir::Unpin& n,const ir::Instruction&){
    builder_.call(runtime_abi::memory::managed_unpin,{{ptr,symbols_.value(n.value)}});
}

} // namespace quidra::llvm_backend
