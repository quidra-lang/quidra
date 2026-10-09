// ir::dump (quidra/ir/module.hpp): the text of `quidra ir`. The IR version,
// the compiler extensions and the classes, then per function its signature,
// its blocks (one instruction_text line per instruction, source locations
// left out) and its tensor-region metadata.
#include "quidra/ir/module.hpp"

#include "ir/instruction_text.hpp"

#include <sstream>

namespace quidra::ir {

std::string dump(const Module& module) {
    std::ostringstream out;
    const auto emit_names = [&](std::string_view label,
                                const std::vector<std::string>& names) {
        if (names.empty()) return;
        out << " " << label << "=";
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (index != 0) out << ",";
            out << names[index];
        }
    };

    out << "quidra-ir " << ir_version << "\n";
    for (const auto& extension : module.compiler_extensions)
        out << "compiler-extension " << extension.package << "."
            << extension.name << "\n";
    for (const auto& c : module.classes)
        out << "class " << c.name << "\n";

    for (const auto& fn : module.functions) {
        out << "function " << fn.name << "(";
        for (std::size_t index = 0; index < fn.parameters.size(); ++index) {
            if (index != 0) out << ", ";
            const auto& parameter = fn.parameters[index];
            if (parameter.is_const) out << "const ";
            out << type_name(parameter.type) << " "
                << (parameter.writable ? "&" : "") << parameter.name;
        }
        out << ") -> " << type_name(fn.result);
        if (fn.external_symbol) out << " = \"" << *fn.external_symbol << "\"";
        if (fn.c_export_symbol) out << " export \"C\" " << *fn.c_export_symbol;
        out << "\n";

        for (const auto& block : fn.blocks) {
            out << block.label << ":\n";
            for (const auto& instruction : block.instructions) {
                if (std::holds_alternative<SourceLocation>(instruction))
                    continue;
                out << "  " << instruction_text(instruction) << "\n";
            }
        }

        // Tensor-region metadata is compiler-owned introspection only. Package
        // operation IDs and tables remain opaque strings; exposing them here
        // lets package integration tests prove registration/matching without
        // teaching Core any domain semantics.
        for (const auto& region : fn.tensor_regions) {
            out << "  tensor-region";
            emit_names("extensions", region.compiler_extensions);
            emit_names("operations", region.compiler_operations);
            emit_names("tables", region.compiler_extension_tables);
            emit_names("fusion-candidates", region.compiler_fusion_candidates);
            if (region.reaches_backward) out << " reaches-backward";
            if (region.may_require_higher_order) out << " higher-order";
            out << "\n";
        }
        out << "end\n";
    }
    return out.str();
}

} // namespace quidra::ir
