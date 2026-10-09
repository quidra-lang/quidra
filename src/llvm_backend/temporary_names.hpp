#pragma once

// TemporaryNames: the names of one function's temporary LLVM values
// (%prefix.N) and labels (prefix.N).
//
// Owns: the one counter that numbers both. Values and labels share it, so the
// order in which a function allocates names is part of the output: one more,
// one fewer or one reordered allocation renumbers every name allocated after
// it. Never split values and labels onto separate counters.

#include <cstddef>
#include <string>

namespace quidra::llvm_backend {

class TemporaryNames {
public:
    std::string value(const std::string&prefix){return "%"+prefix+"."+std::to_string(next_++);}
    std::string label(const std::string&prefix){return prefix+"."+std::to_string(next_++);}

private:
    std::size_t next_{0};
};

} // namespace quidra::llvm_backend
