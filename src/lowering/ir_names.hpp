#pragma once

// The IR's names in the lowering: the lowering writes IR, so its units and
// analyses name the IR's types unqualified. Its public header,
// quidra/lowering.hpp, brings both sides: the checked program it reads and
// the IR module it writes.

#include "quidra/lowering.hpp"

namespace quidra::lowering {

// The lowering writes IR: the IR's names are used unqualified here. ast.hpp
// also declares a quidra::Parameter (a source parameter); here Parameter is
// the IR's.
using namespace quidra::ir;
using ir::Parameter;

} // namespace quidra::lowering
