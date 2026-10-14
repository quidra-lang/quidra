#pragma once
#include "optimizer/pass_context.hpp"

namespace quidra::optimizer {
bool run_check_elimination_first(ir::Module&, const PassContext&);
bool run_check_elimination_second(ir::Module&, const PassContext&);
bool run_step_region_annotations(ir::Module&, const PassContext&);
}
