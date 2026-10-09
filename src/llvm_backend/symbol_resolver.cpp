#include "llvm_backend/symbol_resolver.hpp"

#include "quidra/abi/symbols.hpp"

namespace quidra::llvm_backend {

std::string SymbolResolver::call_symbol(const std::string& name) const {
    const auto found=external_symbols_.find(name);
    return found==external_symbols_.end()?abi::user_symbol(name):found->second;
}

std::string SymbolResolver::definition_symbol() const {
    if(fn_.entrypoint) return std::string(abi::symbol_namespace::entry);
    auto symbol=abi::user_symbol(fn_.name);
    if(self_depth_recursive_) symbol+=".depth";
    return symbol;
}

std::string SymbolResolver::direct_call_symbol(const std::string& name) const {
    if(self_depth_recursive_&&name==fn_.name) return abi::user_symbol(name)+".depth";
    return call_symbol(name);
}

} // namespace quidra::llvm_backend
