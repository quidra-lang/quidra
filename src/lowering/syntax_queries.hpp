#pragma once

// Pure questions about the AST. They read the AST only: no checker facts, no
// lowering state, and they emit nothing (syntax_queries.cpp).

#include "quidra/ast.hpp"
#include <string>
#include <unordered_set>
#include <vector>

namespace quidra::lowering {

// The key of a name expression in the lowering's name-keyed facts and
// comparisons: a local's name, or "this.NAME" for a receiver field written
// `this.NAME`, which never equals a local's name (a parameter or local may
// share a field's name).
std::string source_key(const NameExpr& name);
// The name expression when it can denote a local: not `this.NAME`.
const NameExpr* local_name_expr(const Expr& expression);

// Whether `name` is the variable an expression's storage belongs to: the
// name itself, or the base of a member or index chain over it.
bool storage_root_is(const Expr& expression, const std::string& name);
bool expression_contains_writable_argument(const Expr& expression);
bool expression_mentions_name(const Expr& expression, const std::string& name);
bool statement_mentions_name(const Stmt& statement, const std::string& name);
// Whether an expression or block may replace the binding of one of
// `parameters` (rebind, assign, pass as a writable argument) rather than
// only write through it.
bool expression_may_replace_array_reference(
    const Expr& expression, const std::unordered_set<std::string>& parameters);
bool block_may_replace_array_reference(
    const std::vector<StmtPtr>& body, const std::unordered_set<std::string>& parameters);
bool statement_may_return(const Stmt& statement);
bool block_may_return(const std::vector<StmtPtr>& statements);

} // namespace quidra::lowering
