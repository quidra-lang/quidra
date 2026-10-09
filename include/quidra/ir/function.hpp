#pragma once

// Block, Parameter and Function: one function of the typed IR, its blocks of
// instructions and its tensor regions.

#include "quidra/ir/instruction.hpp"
#include "quidra/ir/region.hpp"
#include "quidra/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace quidra::ir {

struct Block { std::string label; std::vector<Instruction> instructions; };
struct Parameter {
    std::string name;
    Type type;
    bool writable{};
    bool borrowed{};
    bool is_const{};
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
    std::optional<std::string> external_symbol;
    // An exported function (export "C"): its C symbol, the function's bare
    // declared name. The backend gives it a C entry point under that symbol;
    // the function itself is lowered and called as without it.
    std::optional<std::string> c_export_symbol;
};

} // namespace quidra::ir
