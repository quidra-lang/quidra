#pragma once

// BorrowInference: the by-value parameters a function never mutates, so that
// callers pass them borrowed instead of cloned. A parameter is borrowed when
// its type needs a clone to be copied, it is not an atomic.Counter (a shared
// handle every task.all worker must own), and the body never mutates it:
// no assignment, rebind, writable argument, reference binding or mutating
// method reaches its storage. A const parameter whose address, or the
// address of a part of it, the body observes is never borrowed: it is a
// value with storage of its own (D12's identity rule, from the effect
// summaries). A call passes a borrowed argument that the call may write as
// a copy instead (semantics/argument_isolation.hpp) (borrow_inference.cpp).
//
// Owns: BorrowedParameters, decided for every function and method of the
// program when the inference is constructed, before any function is lowered.

#include "quidra/checker.hpp"
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace quidra::lowering {

// A callee's internal name to the indices of the parameters callers pass
// borrowed (a method's receiver is index 0).
using BorrowedParameters = std::unordered_map<std::string, std::unordered_set<std::size_t>>;

class BorrowInference {
public:
    explicit BorrowInference(const CheckedProgram& checked);
    // Moved, never copied: a copy of an unordered container may iterate in
    // another order.
    BorrowInference(const BorrowInference&) = delete;
    BorrowInference& operator=(const BorrowInference&) = delete;
    BorrowInference(BorrowInference&&) = default;

    bool parameter_is_borrowed(const std::string& callee, std::size_t index) const;
    // Whether `body`, or one statement of it, may mutate the storage of
    // `name`.
    bool block_mutates_parameter(const std::vector<StmtPtr>& body, const std::string& name) const;
    bool statement_mutates_parameter(const Stmt& statement, const std::string& name) const;
    // Whether `expression` may mutate the storage of `name`: a writable
    // argument or a mutating receiver rooted in it (storage_writes.hpp builds
    // on this query).
    bool expression_mutates_parameter(const Expr& expression, const std::string& name) const;

private:
    const CheckedProgram& checked_;
    BorrowedParameters borrowed_;
};

} // namespace quidra::lowering
