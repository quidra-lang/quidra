#pragma once

// LlvmType: the type of an LLVM value or memory slot, as textual IR spells
// it. The set of types is closed: void, the integers i1, i8, i16, i32 and
// i64, float, double and ptr (the scalars); an array of a scalar
// ([N x T]); and a structure of two scalars ({ T, U }), the result of an
// overflow intrinsic.
//
// A small value type. The scalars are also named constants in
// llvm_text::types (i64, ptr, ...), for code that writes many operations.
// A type is written by appending its spelling to a text; the element count
// of an array is written in decimal (append_decimal).

#include "llvm_text/appendable_text.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace quidra::llvm_text {

class LlvmType {
public:
    enum class Scalar : std::uint8_t { void_type, i1, i8, i16, i32, i64, float_type, double_type, ptr };

    constexpr LlvmType(Scalar scalar) : shape_(Shape::scalar), first_(scalar) {}

    // [count x element], element a scalar.
    static constexpr LlvmType array(std::uint64_t count, LlvmType element) {
        LlvmType type(element.first_);
        type.shape_ = Shape::array;
        type.count_ = count;
        return type;
    }
    // { first, second }, both scalars.
    static constexpr LlvmType structure(LlvmType first, LlvmType second) {
        LlvmType type(first.first_);
        type.shape_ = Shape::structure;
        type.second_ = second.first_;
        return type;
    }

    static constexpr std::string_view spelling(Scalar scalar) {
        constexpr std::array<std::string_view, 9> spellings{"void", "i1",    "i8",     "i16", "i32",
                                                            "i64",  "float", "double", "ptr"};
        return spellings[static_cast<std::size_t>(scalar)];
    }

    // Appends the spelling to a text (a FixedText at compile time).
    template <AppendableText Text>
    constexpr void print(Text& out) const;

    friend constexpr bool operator==(const LlvmType&, const LlvmType&) = default;

private:
    enum class Shape : std::uint8_t { scalar, array, structure };

    Shape shape_;
    Scalar first_;
    Scalar second_{Scalar::void_type};
    std::uint64_t count_{};
};

template <AppendableText Text>
constexpr void LlvmType::print(Text& out) const {
    switch (shape_) {
        case Shape::scalar:
            out.append(spelling(first_));
            return;
        case Shape::array:
            out.append("[");
            append_decimal(out, count_);
            out.append(" x ");
            out.append(spelling(first_));
            out.append("]");
            return;
        case Shape::structure:
            out.append("{ ");
            out.append(spelling(first_));
            out.append(", ");
            out.append(spelling(second_));
            out.append(" }");
            return;
    }
}

// The scalar types by their LLVM names.
namespace types {

inline constexpr LlvmType void_type{LlvmType::Scalar::void_type};
inline constexpr LlvmType i1{LlvmType::Scalar::i1};
inline constexpr LlvmType i8{LlvmType::Scalar::i8};
inline constexpr LlvmType i16{LlvmType::Scalar::i16};
inline constexpr LlvmType i32{LlvmType::Scalar::i32};
inline constexpr LlvmType i64{LlvmType::Scalar::i64};
inline constexpr LlvmType float_type{LlvmType::Scalar::float_type};
inline constexpr LlvmType double_type{LlvmType::Scalar::double_type};
inline constexpr LlvmType ptr{LlvmType::Scalar::ptr};

} // namespace types

} // namespace quidra::llvm_text
