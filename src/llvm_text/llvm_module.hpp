#pragma once

// LlvmModule: the text of one LLVM module, written by section. The module
// owns its sections and their order: the header (the generator line, then
// the source file name when there is one), the prelude (the declarations
// and globals that come before the constants), the constants, the helpers
// (functions defined once per module), the function bodies and the
// metadata. Each section is a string of its own, so a section can be
// written after a later one (a module's constants are known only once its
// functions are written); str() joins the sections in their order, with a
// blank line after the constants.
//
// The generator line ("; Quidra 0.1 generated LLVM IR") is literal text of
// every module, written first. Text appended to a section is written as it
// is; the lines of the module's globals and constants are written by the
// module, with integers in decimal (append_decimal).

#include "llvm_text/appendable_text.hpp"
#include "llvm_text/escape.hpp"
#include "llvm_text/llvm_value.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace quidra::llvm_text {

// The line of a string constant, appended to a text:
// @name = private unnamed_addr constant [N x i8] c"..."
// The bytes are escaped (append_escaped), then the terminating zero that N
// counts. A template, so that the line of a constant can be written at
// compile time (FixedText).
template <AppendableText Text>
constexpr void append_string_constant(Text& out, std::string_view name, std::string_view bytes) {
    out.append("@");
    out.append(name);
    out.append(" = private unnamed_addr constant [");
    append_decimal(out, bytes.size() + 1);
    out.append(" x i8] c\"");
    append_escaped(out, bytes);
    out.append("\\00\"\n");
}

class LlvmModule {
public:
    enum class Section : std::uint8_t { header, prelude, constants, helpers, bodies, metadata };

    LlvmModule();

    // Appends text to a section as it is: lines, or the text of a function.
    void append(Section section, std::string_view text) { sections_[index(section)].append(text); }

    // In the prelude: @name = internal global T value
    void internal_global(std::string_view name, LlvmValue value);
    // In the constants: the line of a string constant (append_string_constant).
    void string_constant(std::string_view name, std::string_view bytes) {
        append_string_constant(sections_[index(Section::constants)], name, bytes);
    }
    // In the constants: @name = private constant { T, ... } { T v, ... }
    void private_constant(std::string_view name, std::initializer_list<LlvmValue> fields);

    // Whether a section's text contains `text`.
    bool contains(Section section, std::string_view text) const {
        return sections_[index(section)].find(text) != std::string::npos;
    }

    // The text of the module: its sections in their order.
    std::string str() const;

private:
    static constexpr std::size_t section_count = 6;
    static constexpr std::size_t index(Section section) { return static_cast<std::size_t>(section); }

    std::array<std::string, section_count> sections_;
};

} // namespace quidra::llvm_text
