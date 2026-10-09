#include "llvm_text/escape.hpp"

namespace quidra::llvm_text {

std::string escape_metadata(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    append_escaped(out, s);
    return out;
}

} // namespace quidra::llvm_text
