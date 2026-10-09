#include "llvm_text/function_text.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace quidra::llvm_text {

namespace {

// The first buffer of a function: a helper's text fits, a function's grows.
constexpr std::size_t initial_capacity = 1024;

} // namespace

FunctionText::FunctionText()
    : data_(std::make_unique_for_overwrite<char[]>(initial_capacity)), capacity_(initial_capacity) {}

void FunctionText::grow(std::size_t needed) {
    const auto capacity = std::max(capacity_ * 2, size_ + needed);
    auto data = std::make_unique_for_overwrite<char[]>(capacity);
    std::memcpy(data.get(), data_.get(), size_);
    data_ = std::move(data);
    capacity_ = capacity;
}

} // namespace quidra::llvm_text
