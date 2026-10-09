#pragma once

// Spellings of LLVM textual IR that depend on the text alone: the bytes of a
// string constant, a metadata string, and an identifier for local names.
//
// The LLVM text layer includes no Quidra header: it knows nothing of the
// typed IR, so spellings that read a Quidra Type (llvm_type, the literal of
// a float constant, type_identifier) stay in the LLVM backend.

#include "llvm_text/appendable_text.hpp"

#include <cctype>
#include <string>
#include <string_view>

namespace quidra::llvm_text {

// Appends s escaped, as the body of an i8 array constant (c"...", before
// its terminating \00) and a metadata string spell their bytes: printable
// ASCII other than '"' and '\' verbatim, in runs, every other byte as \XX
// (two uppercase hexadecimal digits). A template, so that a constant
// expression can escape (FixedText).
template <AppendableText Text>
constexpr void append_escaped(Text& out, std::string_view s) {
    constexpr std::string_view digits = "0123456789ABCDEF";
    std::size_t run = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c >= 32 && c <= 126 && c != '"' && c != '\\') continue;
        out.append(s.substr(run, i - run));
        const char escape[3] = {'\\', digits[c >> 4], digits[c & 15]};
        out.append(std::string_view(escape, 3));
        run = i + 1;
    }
    out.append(s.substr(run));
}
// A metadata string ("..."): s escaped.
std::string escape_metadata(std::string_view s);
// The name with every character outside [A-Za-z0-9_] replaced by '_'. Inline:
// the emitter spells every local and argument name through it.
inline std::string sanitize_identifier(std::string name){std::string out;for(char c:name)out+=(std::isalnum(static_cast<unsigned char>(c))||c=='_')?c:'_';return out;}

} // namespace quidra::llvm_text
