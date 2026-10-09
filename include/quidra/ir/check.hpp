#pragma once

// Typed-IR instructions of the check domain (shape, extent, range,
// iteration and initialization guards; test assertions).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace quidra::ir {

struct ShapedConstraintCheck {
    ValueId value;
    TypeKind kind{TypeKind::Tensor};
    std::vector<std::optional<ValueId>> extents;
    std::uint32_t line{};
    std::uint32_t column{};
};
struct ExtentEqualCheck {
    ValueId actual;
    ValueId expected;
    std::uint32_t line{};
    std::uint32_t column{};
};
struct TestAssert { ValueId condition; };
struct RangeCheckStep { ValueId step; std::uint32_t line{}; std::uint32_t column{}; };
// A reference loop over a reference binding: the array the binding
// designates now (`array`) is still the array the loop entered with
// (`entry_array`, of length `entry_length`); otherwise FOR_ITERATION at
// line:column.
struct IterationShapeCheck {
    ValueId array;
    ValueId entry_array;
    ValueId entry_length;
    std::uint32_t line{};
    std::uint32_t column{};
};

// What an initialization check reads: a binding, a class field or an
// argument (L13).
enum class InitializedSubject : std::uint8_t { binding, field, argument };
// A read whose initialization is known only at run time: `flag` (a bool)
// is true when the storage named by `path` (as the source writes it) is
// initialized; otherwise UNINITIALIZED at line:column.
struct InitializedCheck {
    ValueId flag;
    InitializedSubject subject{InitializedSubject::binding};
    std::string path;
    std::uint32_t line{};
    std::uint32_t column{};
};

template <> struct InstructionTraits<ShapedConstraintCheck> : InDomain<Domain::check> {};
template <> struct InstructionTraits<ExtentEqualCheck> : InDomain<Domain::check> {};
template <> struct InstructionTraits<TestAssert> : InDomain<Domain::check> {};
template <> struct InstructionTraits<RangeCheckStep> : InDomain<Domain::check> {};
template <> struct InstructionTraits<IterationShapeCheck> : InDomain<Domain::check> {};
template <> struct InstructionTraits<InitializedCheck> : InDomain<Domain::check> {};

} // namespace quidra::ir
