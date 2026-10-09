#include "llvm_backend/scratch_planner.hpp"

#include <stdexcept>

namespace quidra::llvm_backend {

std::size_t ScratchPlanner::append_slot(llvm_text::LlvmType type,llvm_text::Align alignment){
    const auto index=frame_scratch_slots_.size();
    frame_scratch_slots_.push_back(
        FrameScratchSlot{"%scratch."+std::to_string(index),type,alignment});
    return index;
}

void ScratchPlanner::reserve(const ir::Instruction& instruction,llvm_text::LlvmType type,llvm_text::Align alignment){
    const auto index=append_slot(type,alignment);
    instruction_scratch_slots_[&instruction].push_back(index);
}

void ScratchPlanner::reserve_shared(SharedSlot shared,llvm_text::LlvmType type,llvm_text::Align alignment){
    auto& slot=shared_slots_[static_cast<std::size_t>(shared)];
    if(!slot.empty()) return;
    const auto index=append_slot(type,alignment);
    slot=frame_scratch_slots_[index].name;
}

const std::string& ScratchPlanner::instruction_slot(const ir::Instruction& instruction,std::size_t index)const{
    const auto found=instruction_scratch_slots_.find(&instruction);
    if(found==instruction_scratch_slots_.end()||index>=found->second.size())
        throw std::logic_error("missing planned function scratch slot");
    return frame_scratch_slots_.at(found->second[index]).name;
}

void ScratchPlanner::emit_allocas(llvm_text::LlvmBuilder& builder) const{
    for(const auto& slot:frame_scratch_slots_)
        builder.alloca_slot(slot.name,slot.type,slot.alignment);
}

} // namespace quidra::llvm_backend
