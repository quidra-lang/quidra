#pragma once

// ReplReplayState: what the lowering of a REPL submission needs to know. The
// entry's statements are the accepted source followed by the new submission;
// the accepted prefix is replayed, and the submission's expression statement
// displays its value.

#include "quidra/ast.hpp"
#include <cstddef>

namespace quidra::lowering {

class ReplReplayState {
public:
    ReplReplayState(const Expr* displayed_expression, std::size_t replay_prefix_offset)
        : displayed_expression_(displayed_expression),
          replay_prefix_offset_(replay_prefix_offset) {}

    // The expression whose value the REPL displays; null outside the REPL.
    const Expr* displayed_expression() const { return displayed_expression_; }
    // The byte offset where the submission starts; 0 when nothing is replayed.
    std::size_t replay_prefix_offset() const { return replay_prefix_offset_; }
    // The top-level statement being replayed from the accepted REPL prefix.
    const Stmt* replayed_top_level_statement() const { return replayed_top_level_statement_; }
    void set_replayed_top_level_statement(const Stmt* statement) { replayed_top_level_statement_ = statement; }

private:
    const Expr* displayed_expression_{};
    std::size_t replay_prefix_offset_{};
    const Stmt* replayed_top_level_statement_{};
};

} // namespace quidra::lowering
