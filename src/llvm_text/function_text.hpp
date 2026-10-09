#pragma once

// FunctionText: the text of one LLVM function definition (or of a helper
// function) while it is written. It is one buffer, which LlvmBuilder
// appends every line to: the debug location passes find the lines of a
// statement by the byte offsets before and after it (offset), and attach
// locations to the finished text. Appending is the cost of every piece of
// every line, so it copies into spare capacity inline and grows the buffer
// out of line, doubling it.
//
// LlvmBlock: a basic block, named by its label (entry, cast.ok.12). Writing
// a block starts it ("label:"); a branch, a phi or a switch refers to it as
// %label.

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace quidra::llvm_text {

class FunctionText {
public:
    FunctionText();
    // The one buffer of a text: it is neither copied nor moved.
    FunctionText(const FunctionText&) = delete;
    FunctionText& operator=(const FunctionText&) = delete;

    // Appends a piece of text. An empty piece writes nothing: the data of
    // an empty std::string_view may be null, which memcpy must not get.
    void append(std::string_view piece) {
        if (piece.empty()) return;
        if (capacity_ - size_ < piece.size()) grow(piece.size());
        std::memcpy(data_.get() + size_, piece.data(), piece.size());
        size_ += piece.size();
    }
    // The offset of the next byte to be written.
    std::size_t offset() const { return size_; }
    // The text written so far.
    std::string str() const { return std::string(data_.get(), size_); }

private:
    // Makes room for `needed` more bytes.
    void grow(std::size_t needed);

    std::unique_ptr<char[]> data_;
    std::size_t size_{};
    std::size_t capacity_{};
};

struct LlvmBlock {
    LlvmBlock(const char* text) : label(text) {}
    LlvmBlock(const std::string& text) : label(text) {}
    LlvmBlock(std::string_view text) : label(text) {}

    std::string_view label;
};

} // namespace quidra::llvm_text
