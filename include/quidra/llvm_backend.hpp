#pragma once
#include "quidra/ir.hpp"
#include <string>

namespace quidra {
std::string emit_llvm(const ir::Module& module, bool debug_info = false);
}
