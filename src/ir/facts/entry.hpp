#pragma once
#include "ir/facts/tensor.hpp"
#include "quidra/declaration_identity.hpp"
#include <memory>
#include <string>

namespace quidra::ir::facts {
enum class EntryFactOrigin : std::uint8_t { AllDirectCallSites, SpecializedCalls };
// Owned formal parameter facts. Numeric extents are process-independent;
// caller-local symbols, SSA identities and storage ownership proofs are absent.
// AllDirectCallSites must be revalidated after a call-producing rewrite.
// SpecializedCalls is an obligation on every caller of that particular clone.
struct ValidatedEntryFacts {
    EntryFactOrigin origin{EntryFactOrigin::AllDirectCallSites};
    DeclarationIdentity declaration;
    std::string source_instance, fact_key;
    std::map<std::size_t, TensorFacts> tensors;
    std::map<std::size_t, ShapeExtent> scalars;
    PolicyFacts policies;
};
using EntryFactsMap = std::map<std::string, std::shared_ptr<const ValidatedEntryFacts>, std::less<>>;
} // namespace quidra::ir::facts
