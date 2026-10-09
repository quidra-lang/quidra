#pragma once

// Whether the current block has ended, by the lowering's rule.
//
// A block ends at a terminator (Return, ReturnVoid, Exit, FailError, Jump,
// Branch) or at a direct Call that never returns (a Never result, or
// no_normal_return). The lowering appends nothing after that point: a body
// stops lowering its statements, and a construct that continues adds a new
// block first. The LLVM backend has its own rule (an IndirectCall with a
// Never result also ends a block there); the two are kept apart by name.

#include "ir/function_builder.hpp"
#include <variant>

namespace quidra::lowering {

inline bool current_block_ended(const ir::FunctionBuilder& builder) {
    const ir::Block* block = builder.block();
    if (!block || block->instructions.empty()) return false;
    const auto& i=block->instructions.back();
    if (std::holds_alternative<ir::Return>(i)||std::holds_alternative<ir::ReturnVoid>(i)||std::holds_alternative<ir::Exit>(i)||std::holds_alternative<ir::FailError>(i)||std::holds_alternative<ir::Jump>(i)||std::holds_alternative<ir::Branch>(i)) return true;
    if (const auto* c=std::get_if<ir::Call>(&i))
        return c->result.kind==TypeKind::Never || c->no_normal_return;
    return false;
}

} // namespace quidra::lowering
