#include "llvm_text/llvm_module.hpp"

#include "llvm_text/appendable_text.hpp"

namespace quidra::llvm_text {

LlvmModule::LlvmModule() {
    append(Section::header, "; Quidra 0.1 generated LLVM IR\n");
}

void LlvmModule::internal_global(std::string_view name, LlvmValue value) {
    auto& out = sections_[index(Section::prelude)];
    out.append("@");
    out.append(name);
    out.append(" = internal global ");
    value.type.print(out);
    out.append(" ");
    value.operand.print(out);
    out.append("\n");
}

void LlvmModule::private_constant(std::string_view name, std::initializer_list<LlvmValue> fields) {
    auto& out = sections_[index(Section::constants)];
    out.append("@");
    out.append(name);
    out.append(" = private constant { ");
    bool first = true;
    for (const auto& field : fields) {
        if (!first) out.append(", ");
        first = false;
        field.type.print(out);
    }
    out.append(" } { ");
    first = true;
    for (const auto& field : fields) {
        if (!first) out.append(", ");
        first = false;
        field.type.print(out);
        out.append(" ");
        field.operand.print(out);
    }
    out.append(" }\n");
}

std::string LlvmModule::str() const {
    constexpr std::string_view constants_end = "\n";
    std::size_t size = constants_end.size();
    for (const auto& section : sections_) size += section.size();
    std::string text;
    text.reserve(size);
    for (std::size_t i = 0; i < section_count; ++i) {
        text.append(sections_[i]);
        if (i == index(Section::constants)) text.append(constants_end);
    }
    return text;
}

} // namespace quidra::llvm_text
