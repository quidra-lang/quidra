#pragma once
#include "ir/inline_binding_origins.hpp"
#include "ir/facts/entry.hpp"
#include <functional>
#include <span>

namespace quidra::ir::analysis {
struct TrackingFoldReplacement {InstrRef at;ValueId out{},tensor{};};
// Only the actual typed closed-entry analysis constructs this proof. Plain
// replacement records alone cannot authorize a changed-body source snapshot.
class HelperTrackingProof {
public:
    const std::string& body_fields()const noexcept{return body_;}
    const std::shared_ptr<const facts::ValidatedEntryFacts>& entry()const noexcept{return entry_;}
    const std::vector<TrackingFoldReplacement>& replacements()const noexcept{return replacements_;}
private:
    std::string body_;
    std::shared_ptr<const facts::ValidatedEntryFacts> entry_;
    std::vector<TrackingFoldReplacement> replacements_;
    friend std::shared_ptr<const HelperTrackingProof> prove_helper_tracking_folds(
        const Module&,const Function&,std::shared_ptr<const facts::ValidatedEntryFacts>,const std::function<void()>&);
};
bool helper_descriptor_read_only(const Module&,const Call&);
std::shared_ptr<const HelperTrackingProof> prove_helper_tracking_folds(const Module&,
    const Function&,std::shared_ptr<const facts::ValidatedEntryFacts>,const std::function<void()>& tick);
struct HelperCloneOriginReceipt {
    std::string clone,source;
    std::shared_ptr<const HelperTrackingProof> proof;
};
struct HelperCallRetargetReceipt {
    std::string caller,source,clone;
    std::shared_ptr<const std::string> caller_body;
    std::string source_instance;
    std::shared_ptr<const facts::ValidatedEntryFacts> clone_entry;
    InstrRef at;
    Call before,after;
};
struct HelperOriginResult {
    std::shared_ptr<const BindingOriginSnapshots> snapshots;
    std::vector<BindingSourceIssue> unavailable;
};
// Prepares against immutable original Functions and staged clones, before
// mutation. The pass publishes only after exact clone semantic import.
HelperOriginResult prepare_helper_origins(const Module&,std::span<const Function> clones,
    std::span<const HelperCloneOriginReceipt>,std::span<const HelperCallRetargetReceipt>);
// Exact post-transaction source/clone ABI/declaration/gargs/BODY check; the
// caller may request PreserveUnchangedBody only when this is true.
bool helper_unchanged_body(const Function&,const Function&);
} // namespace quidra::ir::analysis
