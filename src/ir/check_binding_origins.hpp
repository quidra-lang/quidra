#pragma once

#include "ir/inline_binding_origins.hpp"
#include <functional>

namespace quidra::ir::analysis {
// These describe edits already authorized by the check proof owner. This
// adapter validates metadata fidelity; it never proves a check cannot fail.
enum class CheckOriginAction : std::uint8_t {
    Erase, BoundsFlag, InitializationFlag, OverflowFlag, InlineFlag,
    BroadcastFlag, ShapeFlag
};
struct CheckOriginEdit {InstrRef at;CheckOriginAction action;};
struct CheckOriginTransfer {
    std::string function;
    std::vector<CheckOriginEdit> edits;
    std::map<InstrRef,std::optional<InstrRef>,InlineSiteLess> instructions;
    std::map<InstrRef,InstrRef,InlineSiteLess> boundaries;
    // Derived candidate regions carry instruction indices. Their invalidation
    // must be explicit; every other Function field is retained exactly.
    bool invalidate_tensor_regions{};
};
enum class CheckOriginFailure : std::uint8_t {None, InvalidReceipt, Budget};
struct CheckOriginResult {
    bool accepted{};
    CheckOriginFailure failure{CheckOriginFailure::None};
    std::shared_ptr<const BindingOriginSnapshots> snapshots;
    std::vector<BindingSourceIssue> unavailable;
};
// Pure, transactional preparation against the actual original module. A
// malformed edit/map leaves its owner unchanged. Valid edits with absent or
// stale sources remain accepted, explicitly unavailable, without a new source
// proof. Publish the accepted Function and returned owner together. Each raw
// field and mapping visit debits tick; exceptions from tick propagate intact.
CheckOriginResult transfer_check_origins(const Module&,const Function& before,
    const Function& after,const CheckOriginTransfer&,const std::function<void()>& tick,
    std::size_t maximum_preimage_bytes=64*1024*1024,
    const std::function<void(std::size_t)>& bytes={});
} // namespace quidra::ir::analysis
