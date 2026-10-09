#pragma once

// FunctionBuilder: builds the IR of one function.
//
// It holds the Function being built, the block that emit() appends
// instructions to, and the three name counters of the function: values (%N),
// labels (prefix.N) and hidden locals ($prefix.N). Every fresh, label and
// hidden call takes the next number of its counter, so the order of the
// calls is part of the output; a caller never puts two of them in one
// unsequenced expression.
//
// Owns: nothing. The Function belongs to its Module, and a block appended by
// add_block lives in the Function's block vector: appending another block may
// move it, so a Block& from add_block is used only until the next add_block,
// and the current block is re-entered from the reference add_block returns.
//
// The constructor takes the counters to start from, so that a pass can
// resume a function it did not build. finish() is called once the function
// is complete; it has nothing to record yet.

#include "quidra/ir/function.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace quidra::ir {

class FunctionBuilder {
public:
    FunctionBuilder() = default;
    FunctionBuilder(Function& function, ValueId next_value, std::uint32_t next_label,
                    std::uint32_t next_hidden)
        : function_(&function), next_value_(next_value), next_label_(next_label),
          next_hidden_(next_hidden) {}

    // The function being built; null before the first function.
    Function* function() const { return function_; }
    // The block instructions are appended to; null until a block is entered.
    Block* block() const { return block_; }

    ValueId fresh() { return next_value_++; }
    std::string label(std::string_view p) { return std::string(p)+"."+std::to_string(next_label_++); }
    std::string hidden(std::string_view p) { return "$"+std::string(p)+"."+std::to_string(next_hidden_++); }

    // Appends an empty block named `name` to the function, without entering it.
    Block& add_block(std::string name) { function_->blocks.push_back(Block{std::move(name),{}}); return function_->blocks.back(); }
    // Makes `block` the block instructions are appended to.
    void enter(Block& block) { block_ = &block; }

    // Appends `instruction` to the current block, constructing the variant in
    // place from it.
    template <class T>
    void emit(T&& instruction) { block_->instructions.emplace_back(std::forward<T>(instruction)); }

    ValueId const_int(long long n) { auto v=fresh(); emit(ConstantInt{v,std::to_string(n),Type::simple(TypeKind::Int64)}); return v; }
    ValueId const_float(double x) { auto v=fresh(); emit(ConstantFloat{v,x,Type::simple(TypeKind::Real64),{}}); return v; }
    ValueId const_bool(bool b) { auto v=fresh(); emit(ConstantBool{v,b}); return v; }

    void finish() {}

private:
    Function* function_{};
    Block* block_{};
    ValueId next_value_{1};
    std::uint32_t next_label_{};
    std::uint32_t next_hidden_{};
};

} // namespace quidra::ir
