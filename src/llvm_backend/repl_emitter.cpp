#include "llvm_backend/repl_emitter.hpp"

#include "llvm_backend/runtime_abi.hpp"

#include <string>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void ReplEmitter::reserve(const ir::ReplDisplay&,const ir::Instruction& ins){
    scratch_.reserve(ins,i64,Align::none);
}

void ReplEmitter::emit(const ir::ReplReplayMode& n,const ir::Instruction&){
    builder_.store({i1,n.active?"true":"false"},"@.quidra.repl.replaying",Align::none);
}

void ReplEmitter::emit(const ir::ReplDisplay& n,const ir::Instruction& ins){
    const auto begin=pool_.intern("__QUIDRA_REPL_RESULT_BEGIN_6D8F2C__");
    const auto end=pool_.intern("__QUIDRA_REPL_RESULT_END_6D8F2C__");
    repl_printer_.set_array_index_slot(scratch_.instruction_slot(ins));
    builder_.call(runtime_abi::c_library::puts,{{ptr,LlvmOperand::global(begin)}});
    repl_printer_.print_value(n.type, n.type.kind == TypeKind::None ? std::string{} : symbols_.value(n.value),
                              n.initialized_paths);
    repl_printer_.print_text("\n");
    builder_.call(runtime_abi::c_library::puts,{{ptr,LlvmOperand::global(end)}});
    repl_printer_.clear_array_index_slot();
}

} // namespace quidra::llvm_backend
