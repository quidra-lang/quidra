#pragma once

// AppendableText: a text that the LLVM text layer appends pieces to: the
// text of a function (FunctionText), any std::string, or a FixedText that a
// constant expression builds. Types and operands write their spelling into
// one.
//
// append_decimal: an integer appended in decimal, with a leading minus sign
// when it is negative: the digits the stream's integer insertion writes in
// the classic locale, which the compiler never changes, without the
// stream's formatting (a sentry, the locale's num_put facet and its
// formatting into a buffer for every integer). In a constant expression,
// where std::to_chars is not constexpr in C++20, the same digits are
// written one at a time.
//
// FixedText: a text of at most N bytes that a constant expression can
// build, for constant text written at compile time (the runtime prelude,
// tens of kilobytes, among them: a piece is copied with
// std::char_traits<char>::copy, which a constant expression evaluates in far
// fewer steps than a loop over its characters). TextSize counts the bytes of
// a text without keeping them: the N its FixedText needs.

#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

namespace quidra::llvm_text {

template <class T>
concept AppendableText = requires(T& text, std::string_view piece) { text.append(piece); };

template <AppendableText Text, std::integral T>
constexpr void append_decimal(Text& out, T value) {
    std::array<char, 24> digits{};
    if (std::is_constant_evaluated()) {
        auto magnitude = static_cast<unsigned long long>(value);
        bool negative = false;
        if constexpr (std::is_signed_v<T>) {
            negative = value < 0;
            if (negative) magnitude = 0ULL - magnitude;
        }
        auto first = digits.size();
        do {
            digits[--first] = static_cast<char>('0' + magnitude % 10);
            magnitude /= 10;
        } while (magnitude != 0);
        if (negative) digits[--first] = '-';
        out.append(std::string_view(digits.data() + first, digits.size() - first));
        return;
    }
    const auto end = std::to_chars(digits.data(), digits.data() + digits.size(), value).ptr;
    out.append(std::string_view(digits.data(), static_cast<std::size_t>(end - digits.data())));
}

template <std::size_t N>
class FixedText {
public:
    constexpr void append(std::string_view piece) {
        std::char_traits<char>::copy(text_.data() + size_, piece.data(), piece.size());
        size_ += piece.size();
    }
    constexpr std::string_view view() const { return std::string_view(text_.data(), size_); }

private:
    std::array<char, N> text_{};
    std::size_t size_{};
};

class TextSize {
public:
    constexpr void append(std::string_view piece) { size_ += piece.size(); }
    constexpr std::size_t size() const { return size_; }

private:
    std::size_t size_{};
};

} // namespace quidra::llvm_text
