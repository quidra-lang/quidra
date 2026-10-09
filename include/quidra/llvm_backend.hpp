#pragma once
#include "quidra/ir/module.hpp"
#include <string>

namespace quidra {
std::string emit_llvm(const ir::Module& module, bool debug_info = false);
}
