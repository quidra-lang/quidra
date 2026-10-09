#pragma once

// LlvmOperand: an operand as textual IR spells it, without its type: a
// local or global name (%v3, @.str.1), a constant keyword (null, true,
// false), a float literal (0x3FF0000000000000) or an integer literal. An
// integer is written in decimal (append_decimal), so an operand built from an
// integer spells it exactly as writing the integer with << does; a bool or
// a character is not an integer operand.
//
// LlvmValue: a typed operand (i64 %v3, ptr null), as an argument, a stored
// value or a returned value is written.
//
// Operands do not own their text: a name refers to a string that must
// outlive the operation it is passed to, such as a name the caller holds or
// a temporary built in the same call. Operations take operands by value and
// write them at once.

#include "llvm_text/appendable_text.hpp"
#include "llvm_text/llvm_type.hpp"

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace quidra::llvm_text {

// The integer types an integer operand is built from: not bool and not the
// character types, which a stream writes as characters.
template <class T>
concept IntegerLiteral =
    std::integral<T> && !std::same_as<std::remove_cv_t<T>, bool> &&
    !std::same_as<std::remove_cv_t<T>, char> && !std::same_as<std::remove_cv_t<T>, signed char> &&
    !std::same_as<std::remove_cv_t<T>, unsigned char> && !std::same_as<std::remove_cv_t<T>, wchar_t> &&
    !std::same_as<std::remove_cv_t<T>, char8_t> && !std::same_as<std::remove_cv_t<T>, char16_t> &&
    !std::same_as<std::remove_cv_t<T>, char32_t>;

class LlvmOperand {
public:
    LlvmOperand(const char* text) : kind_(Kind::text), text_(text) {}
    LlvmOperand(const std::string& text) : kind_(Kind::text), text_(text) {}
    LlvmOperand(std::string_view text) : kind_(Kind::text), text_(text) {}
    template <IntegerLiteral T>
    LlvmOperand(T value) {
        if constexpr (std::is_signed_v<T>) {
            kind_ = Kind::signed_integer;
            signed_ = static_cast<long long>(value);
        } else {
            kind_ = Kind::unsigned_integer;
            unsigned_ = static_cast<unsigned long long>(value);
        }
    }

    // @name: a global named by its symbol (an LLVM global or function).
    static LlvmOperand global(std::string_view name) {
        LlvmOperand operand(name);
        operand.kind_ = Kind::global;
        return operand;
    }

    // The symbol of a global name (@name or global(name)), without the '@';
    // empty for every other operand.
    std::string_view global_symbol() const {
        if (kind_ == Kind::global) return text_;
        if (kind_ == Kind::text && !text_.empty() && text_.front() == '@') return text_.substr(1);
        return {};
    }

    // Appends the operand to a text.
    template <AppendableText Text>
    void print(Text& out) const {
        switch (kind_) {
            case Kind::text:
                out.append(text_);
                return;
            case Kind::global:
                out.append("@");
                out.append(text_);
                return;
            case Kind::signed_integer:
                append_decimal(out, signed_);
                return;
            case Kind::unsigned_integer:
                append_decimal(out, unsigned_);
                return;
        }
    }

private:
    enum class Kind : std::uint8_t { text, global, signed_integer, unsigned_integer };

    Kind kind_{Kind::text};
    std::string_view text_;
    long long signed_{};
    unsigned long long unsigned_{};
};

struct LlvmValue {
    LlvmType type;
    LlvmOperand operand;
};

} // namespace quidra::llvm_text
