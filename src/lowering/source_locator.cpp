// SourceLocator: the lowering emits a SourceLocation before every statement;
// debug information and diagnostics read them. A span is matched against the
// public syntax nodes of its file, inspected once per program.

#include "lowering/source_locator.hpp"
#include <limits>

namespace quidra::lowering {

SourceLocator::SourceLocator(const CheckedProgram& checked) {
    for (const auto& [path, source] : checked.program.source_texts) {
        inspections_.emplace(path, inspect_syntax_source(source));
    }
}

ir::SourceLocation SourceLocator::locate(SourceSpan span, const ir::Function* function) const {
    ir::SourceLocation location{};
    location.line = static_cast<std::uint32_t>(span.start.line);
    location.column = static_cast<std::uint32_t>(span.start.column);
    if (!function || function->source_file.empty()) return location;
    // The file is known for every statement of a function with a file,
    // whether or not a public node is found for it.
    location.source_file = function->source_file;
    const auto inspection = inspections_.find(function->source_file);
    if (inspection == inspections_.end()) return location;

    const SourceNode* best = nullptr;
    for (const auto& node : inspection->second.nodes) {
        if (node.span.start.offset != span.start.offset ||
            node.span.end.offset != span.end.offset) continue;
        if (!best || node.depth < best->depth) best = &node;
    }
    if (!best) {
        for (const auto& node : inspection->second.nodes) {
            if (node.span.start.offset > span.start.offset ||
                node.span.end.offset < span.end.offset) continue;
            const auto width = node.span.end.offset - node.span.start.offset;
            const auto best_width = best
                ? best->span.end.offset - best->span.start.offset
                : std::numeric_limits<std::size_t>::max();
            if (!best || width < best_width ||
                (width == best_width && node.depth < best->depth)) {
                best = &node;
            }
        }
    }
    // Parser statement spans can include the terminating newline while the
    // public structural node deliberately ends at the last source token.
    // In that case no public node can contain the compiler span even though
    // both identify the same statement. Fall back to the widest public node
    // that starts at the same byte and is contained by the compiler span;
    // this selects the statement rather than a nested expression sharing
    // its start position.
    if (!best) {
        for (const auto& node : inspection->second.nodes) {
            if (node.span.start.offset != span.start.offset ||
                node.span.end.offset > span.end.offset) continue;
            const auto width = node.span.end.offset - node.span.start.offset;
            const auto best_width = best
                ? best->span.end.offset - best->span.start.offset
                : 0;
            if (!best || width > best_width ||
                (width == best_width && node.depth < best->depth)) {
                best = &node;
            }
        }
    }
    if (best) {
        location.source_revision = inspection->second.revision;
        location.node_id = best->node_id;
        location.node_kind = best->kind;
    }
    return location;
}

} // namespace quidra::lowering
