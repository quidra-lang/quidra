#pragma once

// Typed-IR instructions of the numeric domain (scalar arithmetic, comparison and exact-number atoms).
//
// Owns: the domain's instruction structs and their InstructionTraits
// specializations (quidra/ir/domain.hpp). The fields, their order and their
// default member initializers are the IR data model.

#include "quidra/ir/domain.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"

#include <cstdint>
#include <string>

namespace quidra::ir {

struct ExactAtom {
    ValueId out;
    std::string provider;
    std::uint32_t opcode{};
    Type type;
};
struct ExactUnary {
    ValueId out;
    std::string provider;
    std::uint32_t opcode{};
    ValueId input;
    Type type;
};
// inline_proven (Unary and Binary): the operation is on arbitrary-precision
// integers whose operands and result the lowering proved to be inline words,
// so it needs no tag test and no promotion (check_elision_facts.hpp).
struct Unary {
    ValueId out;
    std::string op;
    ValueId operand;
    Type type;
    std::uint32_t line{};
    std::uint32_t column{};
    bool inline_proven{};
};
struct Binary {
    ValueId out;
    std::string op;
    ValueId left;
    ValueId right;
    Type operand_type;
    Type result_type;
    std::uint32_t line{};
    std::uint32_t column{};
    bool overflow_proven{};
    bool inline_proven{};

    Binary(ValueId out_value, std::string operation, ValueId left_value,
           ValueId right_value, Type operand, Type result,
           std::uint32_t source_line = 0,
           std::uint32_t source_column = 0,
           bool proven_no_overflow = false,
           bool proven_inline = false)
        : out(out_value), op(operation), left(left_value), right(right_value),
          operand_type(operand), result_type(result), line(source_line),
          column(source_column), overflow_proven(proven_no_overflow),
          inline_proven(proven_inline) {}
};

template <> struct InstructionTraits<ExactAtom> : InDomain<Domain::numeric> {};
template <> struct InstructionTraits<ExactUnary> : InDomain<Domain::numeric> {};
template <> struct InstructionTraits<Unary> : InDomain<Domain::numeric> {};
template <> struct InstructionTraits<Binary> : InDomain<Domain::numeric> {};

} // namespace quidra::ir
