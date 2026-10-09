#pragma once

// ScratchPlanner: the entry-frame scratch slots of one function (%scratch.N).
//
// An instruction that needs temporary storage reserves its slots before the
// blocks are emitted, either for itself or shared by every instruction of a
// kind; emit_allocas then writes one alloca per slot into the entry frame, so
// a source loop never grows the stack with repeated allocas.
//
// Owns: the slots in the order of reservation, which numbers them (part of
// the output), and the slots of each instruction, found by the instruction's
// identity (its address in the function), never through a copy.

#include "quidra/ir/module.hpp"
#include "llvm_text/llvm_builder.hpp"
#include "llvm_text/llvm_type.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace quidra::llvm_backend {

class ScratchPlanner {
public:
    // Slots shared by every instruction of a kind, reserved by the first one
    // that needs it: the exact-number source of a fallible numeric conversion
    // to an integer, float32 or float64 target. Count is the number of shared
    // slots, not a slot.
    enum class SharedSlot { FallibleExactI64, FallibleExactFloat32, FallibleExactFloat64, Count };

    void reserve(const ir::Instruction& instruction,llvm_text::LlvmType type,llvm_text::Align alignment);
    void reserve_shared(SharedSlot shared,llvm_text::LlvmType type,llvm_text::Align alignment);
    // The index-th slot reserved by instruction; throws std::logic_error when
    // there is none.
    const std::string& instruction_slot(const ir::Instruction& instruction,std::size_t index=0)const;
    // The name of a shared slot, empty when no instruction reserved it.
    const std::string& shared_slot(SharedSlot shared) const {
        return shared_slots_[static_cast<std::size_t>(shared)];
    }
    void emit_allocas(llvm_text::LlvmBuilder& builder) const;

private:
    struct FrameScratchSlot {
        std::string name;
        llvm_text::LlvmType type;
        llvm_text::Align alignment;
    };
    // Appends the next slot (%scratch.<index>) and returns its index.
    std::size_t append_slot(llvm_text::LlvmType type,llvm_text::Align alignment);

    std::vector<FrameScratchSlot> frame_scratch_slots_;
    std::unordered_map<const ir::Instruction*,std::vector<std::size_t>> instruction_scratch_slots_;
    std::array<std::string, static_cast<std::size_t>(SharedSlot::Count)> shared_slots_;
};

} // namespace quidra::llvm_backend
