#pragma once

// SourceLocator: a span of the typed AST to the SourceLocation of the public
// syntax node that identifies it (node id and kind, and the revision of the
// source file). It inspects every source text of the program once, when it
// is constructed; locate() only reads (source_locator.cpp).

#include "quidra/ir/module.hpp"
#include "quidra/source_tools.hpp"
#include <string>
#include <unordered_map>

namespace quidra::lowering {

class SourceLocator {
public:
    explicit SourceLocator(const CheckedProgram& checked);
    // Moved, never copied: a copy of an unordered container may iterate in
    // another order.
    SourceLocator(const SourceLocator&) = delete;
    SourceLocator& operator=(const SourceLocator&) = delete;
    SourceLocator(SourceLocator&&) = default;

    // The location of `span` in the source file of `function`: line and
    // column always, the node when the file was inspected and a node fits.
    ir::SourceLocation locate(SourceSpan span, const ir::Function* function) const;

private:
    std::unordered_map<std::string, SourceInspection> inspections_;
};

} // namespace quidra::lowering
