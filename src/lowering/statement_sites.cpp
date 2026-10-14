#include "lowering/statement_sites.hpp"

#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace quidra::lowering {
namespace {
template<class Visit>
void walk_tree(std::span<const StmtPtr> body, Visit&& visit) {
    std::vector<const Stmt*> pending;
    const auto append = [&](const auto& statements) {
        for (auto it = statements.rbegin(); it != statements.rend(); ++it)
            if (*it) pending.push_back(it->get());
    };
    append(body);
    while (!pending.empty()) {
        const auto* statement = pending.back();
        pending.pop_back();
        if (!visit(statement)) continue;
        std::visit([&](const auto& node) {
            using T = std::remove_cvref_t<decltype(node)>;
            if constexpr (std::is_same_v<T, IfStmt>) {
                append(node.else_body);
                append(node.then_body);
            } else if constexpr (std::is_same_v<T, MainGuardStmt> ||
                                 std::is_same_v<T, ParallelStmt> ||
                                 std::is_same_v<T, WhileStmt> ||
                                 std::is_same_v<T, ForStmt>) {
                append(node.body);
            } else if constexpr (std::is_same_v<T, MatchStmt>) {
                for (auto it = node.cases.rbegin(); it != node.cases.rend(); ++it)
                    append(it->body);
            }
        }, statement->data);
    }
}
} // namespace

std::uint64_t StatementSites::allocate(FunctionSites& function) {
    if (function.exhausted) throw std::overflow_error("IR statement identities exhausted");
    const auto identity = function.next;
    if (identity == std::numeric_limits<std::uint64_t>::max()) function.exhausted = true;
    else ++function.next;
    return identity;
}

void StatementSites::register_tree(std::string_view name, std::span<const StmtPtr> body) {
    auto& function = functions_[std::string(name)];
    walk_tree(body, [&](const Stmt* statement) {
        if (function.source.contains(statement)) return false;
        function.source.emplace(statement, allocate(function));
        return true;
    });
}

void StatementSites::register_expansion(std::string_view name, const Stmt& source,
                                       std::span<const StmtPtr> expansion) {
    const auto parent = identity(name, source);
    register_expansion(name, parent, expansion);
}

void StatementSites::register_expansion(std::string_view name, std::uint64_t parent,
                                       std::span<const StmtPtr> expansion) {
    if (parent == 0) throw std::invalid_argument("statement expansion has no source identity");
    auto& function = functions_[std::string(name)];
    walk_tree(expansion, [&](const Stmt* statement) {
        function.source.try_emplace(statement, parent);
        return true;
    });
}

std::uint64_t StatementSites::identity(std::string_view name, const Stmt& statement) {
    auto& function = functions_[std::string(name)];
    const auto known = function.source.find(&statement);
    if (known != function.source.end()) return function.current = known->second;
    // Synthetic statements may live on a reused stack address. Give each
    // emission a fresh identity rather than treating that address as a site.
    return function.current = allocate(function);
}

std::uint64_t StatementSites::current_identity(std::string_view name) const {
    const auto function = functions_.find(name);
    return function == functions_.end() ? 0 : function->second.current;
}

} // namespace quidra::lowering
