#pragma once
#include "quidra/ir/debug.hpp"
#include "quidra/ir/value_id.hpp"
#include "quidra/types.hpp"
#include <memory>
#include <optional>
#include <vector>

namespace quidra::ir {
namespace facts { struct ValidatedEntryFacts; }
enum class InlineSlotKind : std::uint8_t { OwnedLocal, BorrowedLocal, PinnedReference, RawAddress };
struct InlineSlot {
    std::string name;
    Type type;
    InlineSlotKind kind{InlineSlotKind::OwnedLocal};
    // Original source name is debug-only, separate from the renamed storage.
    std::string source_name;
    bool is_const{}, target_escapes{};
};
struct InlineStatementIdentity { std::uint64_t original{}, remapped{}; };
// Common typed metadata, with no CFG/analysis dependencies. Block membership
// selects the innermost scope by parent IDs, never by debug locations.
struct InlineScope {
    std::uint64_t identity{}, parent{}, caller_statement{};
    std::string callee;
    std::string source_file;
    SourcePosition position{1,1};
    std::vector<std::string> blocks;
    std::vector<InlineSlot> slots;
    std::vector<InlineStatementIdentity> statements;
    bool node_cleanup{};
    // Only a specialized-call obligation, never an old whole-call-site meet.
    std::shared_ptr<const facts::ValidatedEntryFacts> entry_obligation{};
    // Opaque caller attribution to restore at the continuation; never a decision key.
    std::optional<SourceLocation> caller_location{};
};
struct InlineScopeBegin { std::uint64_t scope{}; };
struct InlineScopeCleanup { std::uint64_t scope{}; };
struct InlineScopeEnd { std::uint64_t scope{}; };
// Raw function &-parameter binding: no added pin/unpin or initialization read.
struct InlineBindAddress { std::string name; ValueId address{}; Type type; };
// Cleanup is an explicit preceding InlineScopeCleanup. Unlike function Exit,
// this terminator must not drain unrelated outer/caller cells.
struct InlineExit { std::uint64_t scope{}; ValueId status{}; };

template<> struct InstructionTraits<InlineScopeBegin> : WithOperands<Domain::debug,InlineScopeBegin> {};
template<> struct InstructionTraits<InlineScopeCleanup> : WithOperands<Domain::lifetime,InlineScopeCleanup> {};
template<> struct InstructionTraits<InlineScopeEnd> : WithOperands<Domain::debug,InlineScopeEnd> {};
template<> struct InstructionTraits<InlineBindAddress> : WithOperands<Domain::memory,InlineBindAddress> {};
template<> struct InstructionTraits<InlineExit> : WithOperands<Domain::control_flow,InlineExit> {};
} // namespace quidra::ir
