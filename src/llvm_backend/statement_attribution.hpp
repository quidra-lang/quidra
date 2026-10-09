#pragma once

// StatementAttribution: where the failures of one function's code are
// reported, and the calls that keep the runtime's user statement current
// (docs/spec/architecture.md, "User code and package code").
//
// A statement is user code when its source location names one of the
// module's user files (ir::Module::user_sources), and package code otherwise
// (an installed package, the files it imports, compiler-synthesized bodies).
// An instruction belongs to the statement of the nearest source location
// written before it; before the first, to the function's own file. While
// every location of a function names the function's file, as the lowering
// writes them, this is the per-function decision.
//
// - A statement starts with the runtime call that names it: today's setter
//   for user code (the innermost statement and the user statement), the
//   package setter for package code (the innermost statement only), and for
//   a user statement without a public node, a restore of the user statement
//   with an empty-node record of its own (no provenance suffix). A package
//   statement without a public node calls nothing.
// - Failure immediates: in package code both are 0 ("no exact site; report
//   the user's statement"); in user code they are the instruction's own,
//   the line encoded with the file's index in the source table.
// - The calls of a function (LlvmCallObserver): in package code a call that
//   may enter user code (an indirect call, a call to a user function, a C
//   function given an `fn` argument) is wrapped in a save and a restore of
//   the user statement; in user code a call that may read the user
//   statement (a call into package code, an indirect or C call, a runtime
//   entry, a prelude helper without location parameters) is preceded by a
//   restore of the statement's own record when a call that may enter user
//   code was written before it in the same statement.
//
// Owns: the current statement (user or package, its record and position),
// whether a call that may enter user code was written in it since the user
// statement was last set, and the name of a saved user statement whose
// restore is pending.

#include "quidra/ir/module.hpp"
#include "llvm_backend/module_context.hpp"
#include "llvm_backend/string_pool.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_text/llvm_builder.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace quidra::llvm_backend {

class StatementAttribution final : public llvm_text::LlvmCallObserver {
public:
    StatementAttribution(const ir::Function& fn, const ModuleContext& context,
                         llvm_text::LlvmBuilder& builder, TemporaryNames& names, StringPool& pool);

    // The statement a source location opens: writes the call that names it.
    void begin_statement(const ir::SourceLocation& location);

    // The failure immediates of an instruction of the current statement: in
    // user code the line carries the file's index in its upper 32 bits
    // ((index << 32) | line; the root's index is 0, so a single-file
    // program's immediates are its lines).
    unsigned long long line(std::uint32_t line) const { return user_ ? encode(line) : 0; }
    unsigned long long column(std::uint32_t column) const { return user_ ? column : 0; }
    // The position of the current statement in user code, for the failure
    // sites that have none of their own (the initialization checks of loads
    // through addresses and references); 0, 0 in package code.
    unsigned long long statement_line() const { return user_ ? encode(statement_line_) : 0; }
    unsigned long long statement_column() const { return user_ ? statement_column_ : 0; }

    void before_call(std::string_view symbol) override;
    void after_call(std::string_view symbol) override;

private:
    enum Effect : unsigned { none = 0, reads = 1, enters = 2 };
    unsigned long long encode(std::uint32_t line) const {
        return (static_cast<unsigned long long>(file_index_) << 32) | line;
    }
    void enter_file(const std::string& file);
    unsigned effect_of(std::string_view symbol) const;

    const ModuleContext& context_;
    llvm_text::LlvmBuilder& builder_;
    TemporaryNames& names_;
    StringPool& pool_;
    bool user_{};
    std::size_t file_index_{};
    std::uint32_t statement_line_{};
    std::uint32_t statement_column_{};
    // The record of the current user statement (empty before the first).
    std::string record_;
    bool entered_user_code_{};
    std::string saved_user_statement_;
};

} // namespace quidra::llvm_backend
