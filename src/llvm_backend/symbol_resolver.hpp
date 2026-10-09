#pragma once

// SymbolResolver: the LLVM symbol of a function: its definition (the entry
// symbol main, abi::symbol_namespace::entry, for the entry function,
// otherwise the user symbol), and the callee of a call, which is the C
// symbol of an extern or the user symbol. A self-depth
// recursive function is defined as <symbol>.depth, and its calls to itself
// go there directly; other callers reach it through the wrapper
// (SelfDepthGuard).
//
// Owns no state.

#include "quidra/ir/module.hpp"

#include <string>
#include <unordered_map>

namespace quidra::llvm_backend {

class SymbolResolver {
public:
    SymbolResolver(const ir::Function& fn,
                   const std::unordered_map<std::string,std::string>& external_symbols,
                   bool self_depth_recursive)
        :fn_(fn),external_symbols_(external_symbols),self_depth_recursive_(self_depth_recursive){}

    std::string call_symbol(const std::string& name) const;
    std::string definition_symbol() const;
    std::string direct_call_symbol(const std::string& name) const;

private:
    const ir::Function& fn_;
    const std::unordered_map<std::string,std::string>& external_symbols_;
    bool self_depth_recursive_{};
};

} // namespace quidra::llvm_backend
