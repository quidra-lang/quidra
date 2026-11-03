#pragma once

// Emits a logical tail record immediately before a selected user call,
// and the depth-conditional cleanup at normal returns of self-tail
// functions. The native backend instantiates this service only when its
// static analysis found recorded sites; interactive output uses none.

#include "llvm_backend/temporary_names.hpp"
#include "llvm_text/llvm_builder.hpp"

#include <cstdint>
#include <string>

namespace quidra::llvm_backend {

class TailRecordEmitter {
public:
    TailRecordEmitter(llvm_text::LlvmBuilder& builder, TemporaryNames& names,
                      bool self_depth, std::uint32_t owner)
        : builder_(builder), names_(names), self_depth_(self_depth), owner_(owner) {}

    void record(std::uint32_t site, bool self);
    void normal_return();
    // Resolves the owner function's site range after all its calls exist.
    static void resolve_range(std::string& text, std::uint32_t end);

private:
    std::string cfa();
    llvm_text::LlvmBuilder& builder_;
    TemporaryNames& names_;
    bool self_depth_;
    std::uint32_t owner_;
};

} // namespace quidra::llvm_backend
