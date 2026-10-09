#pragma once

// What the optimizer knows about a tensor value at compile time: its type,
// device, layout, tracking and whether the value owns its storage. Each fact
// is unknown unless an instruction proves it; an unknown fact never
// satisfies a constraint of a conditional rule.

#include "quidra/ir/module.hpp"
#include "quidra/types.hpp"

#include <optional>
#include <unordered_map>

namespace quidra::optimizer {

enum class StaticDevice { Unknown, Cpu, Gpu };
struct StaticTensorFacts {
    Type type{Type::simple(TypeKind::Invalid)};
    bool has_type{};
    StaticDevice device{StaticDevice::Unknown};
    std::optional<bool> contiguous;
    std::optional<bool> tracked;
    bool owns_storage{};
};

// The facts of the tensor values of one function. A value gets facts from
// the instruction that defines it: a tensor creation, transfer, view,
// gather or scatter, cast, track, clone or load of a local. Facts flow
// through locals only inside a block; crossing a block edge would need
// dominance, so a local's facts start unknown in every block.
class StaticTensorFactsAnalysis {
public:
    static std::unordered_map<ir::ValueId, StaticTensorFacts> run(const ir::Function& function);
};

} // namespace quidra::optimizer
