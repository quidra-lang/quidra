#pragma once

#include "quidra/ast.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace quidra::lowering {

// Semantic ordinals within one IR function. The source tree's authored order
// assigns identities; source coordinates, revisions and public debug node IDs
// are never consulted. AST pointers are lookup keys only and are not encoded.
class StatementSites {
public:
    void register_tree(std::string_view function, std::span<const StmtPtr> body);
    // Checker-owned pattern bindings represent the original user statement.
    void register_expansion(std::string_view function, const Stmt& source,
                            std::span<const StmtPtr> expansion);
    void register_expansion(std::string_view function, std::uint64_t source_identity,
                            std::span<const StmtPtr> expansion);
    std::uint64_t identity(std::string_view function, const Stmt& statement);
    std::uint64_t current_identity(std::string_view function) const;

private:
    struct FunctionSites {
        std::unordered_map<const Stmt*, std::uint64_t> source;
        std::uint64_t next{1};
        std::uint64_t current{};
        bool exhausted{};
    };
    std::map<std::string, FunctionSites, std::less<>> functions_;
    static std::uint64_t allocate(FunctionSites&);
};

} // namespace quidra::lowering
