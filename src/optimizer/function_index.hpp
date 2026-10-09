#pragma once

// FunctionIndex: the functions of a module by name, for the rewrites, which
// look up the definition a call names and the functions bound to a
// replacement operation. When two functions share a name, the first one
// counts. The index points into the module's function list, so the module
// must outlive it and keep its functions where they are.

#include "quidra/ir/module.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace quidra::optimizer {

class FunctionIndex {
public:
    explicit FunctionIndex(const ir::Module& module) : functions_(&module.functions) {
        for (std::size_t index = 0; index < module.functions.size(); ++index) {
            first_.emplace(module.functions[index].name, index);
        }
    }
    FunctionIndex(const FunctionIndex&) = delete;
    FunctionIndex& operator=(const FunctionIndex&) = delete;
    FunctionIndex(FunctionIndex&&) = default;
    FunctionIndex& operator=(FunctionIndex&&) = default;

    // The first function named name; null when there is none.
    const ir::Function* find(const std::string& name) const {
        const auto found = first_.find(name);
        return found == first_.end() ? nullptr : &(*functions_)[found->second];
    }

private:
    const std::vector<ir::Function>* functions_;
    std::unordered_map<std::string, std::size_t> first_;
};

} // namespace quidra::optimizer
