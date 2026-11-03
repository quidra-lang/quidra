#pragma once

// Block, Parameter and Function: one function of the typed IR, its blocks of
// instructions and its tensor regions.

#include "quidra/ir/instruction.hpp"
#include "quidra/ir/region.hpp"
#include "quidra/types.hpp"
#include "quidra/declaration_identity.hpp"

#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace quidra::ir {
namespace facts { struct ValidatedEntryFacts; }

struct Block { std::string label; std::vector<Instruction> instructions; };
struct Parameter {
    std::string name;
    Type type;
    bool writable{};
    bool borrowed{};
    bool is_const{};
    bool target_escapes{};
};
struct ParallelNodeMetadata {
    std::string wrapper;
    std::vector<std::string> reference_slots;
    bool fallible{};
};
struct Function {
    std::string name;
    std::string source_file;
    std::uint32_t source_line{1};
    std::uint32_t source_column{1};
    std::vector<Parameter> parameters;
    Type result{Type::simple(TypeKind::Void)};
    std::vector<Block> blocks;
    std::vector<TensorRegion> tensor_regions;
    bool entrypoint{};
    std::optional<ParallelNodeMetadata> parallel_node;
    std::optional<std::string> external_symbol;
    // An exported function (export "C"): its C symbol, the function's bare
    // declared name. The backend gives it a C entry point under that symbol;
    // the function itself is lowered and called as without it.
    std::optional<std::string> c_export_symbol;
    // The public source function represented by a checked shape entry.
    std::string source_function;
    bool shape_wrapper{};
    // Allocation metadata, copied with the function and retained after value
    // removal. UINT32_MAX+1 represents exhaustion without a wrapped ValueId.
    std::uint64_t next_value{1};
    std::optional<DeclarationIdentity> declaration{};
    bool dispatch_stub{};
    std::shared_ptr<const facts::ValidatedEntryFacts> validated_entry{};
    std::vector<InlineScope> inline_scopes{};
};

} // namespace quidra::ir
