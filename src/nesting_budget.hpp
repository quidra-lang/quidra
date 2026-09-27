#pragma once
#include "quidra/diagnostic.hpp"
#include <cstddef>
#include <string>

// Shared nesting budgets for the recursive AST walks that run after parsing.
//
// Without these budgets a small source file exhausts the process stack and the
// compiler dies from SIGSEGV instead of reporting a diagnostic. The cause is
// recursion depth, not program size: 40,000 flat statements (737,780 bytes)
// check and lower successfully, while 600 chained method calls (4,284 bytes)
// crash. Every budget below is therefore a depth limit, never a node count.
//
// The limits are derived from the measured per-level stack cost of the most
// expensive pass that runs behind the guard, in the Debug configuration that
// the sanitizer CI job builds on, targeting at most 60% of an 8 MiB stack
// (5,023,334 bytes):
//
//   expression cycle, worst frame  checker check_expr + check_builtin_call_expr
//                                  72,224 B/level Debug
//                                  5,023,334 / 72,224 = 69 -> 64
//   statement cycle, worst frame   IR lowering Lowerer::stmt
//                                  9,696 B/level Debug
//                                  5,023,334 / 9,696 = 518 -> 256 (2x margin)
//   generic clone                  frontend GenericExpander::clone_expr
//                                  4,576 B/level Debug
//                                  deliberately looser than the checker so the
//                                  checker stays the pass users actually meet
//
// Raising a limit is not the way to accept deeper programs: shrink the
// pathological frames first (check_method_call_expr, check_builtin_call_expr
// and Lowerer::raw_expr dominate), then re-derive the numbers above.

namespace quidra::nesting {

inline constexpr std::size_t max_expression_depth = 64;
inline constexpr std::size_t max_statement_depth = 256;
inline constexpr std::size_t max_ast_clone_depth = 512;

// Counts one level of a recursive walk and reports NESTING_DEPTH when the walk
// runs past its budget. The counter is restored by the destructor, so an
// ordinary diagnostic thrown from deeper in the walk cannot leave it inflated.
class DepthGuard {
public:
    DepthGuard(std::size_t& depth, std::size_t limit, const SourceSpan& span, const char* what)
        : depth_(depth) {
        ++depth_;
        if (depth_ > limit) {
            // A constructor that throws does not run its own destructor.
            --depth_;
            throw CompileError(Diagnostic{
                "NESTING_DEPTH",
                std::string(what) + " nesting exceeds the compiler's safety budget.", span});
        }
    }
    ~DepthGuard() { --depth_; }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;

private:
    std::size_t& depth_;
};

} // namespace quidra::nesting
