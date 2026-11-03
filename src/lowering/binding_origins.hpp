#pragma once
#include "ir/binding_origins.hpp"
#include "ir/summaries.hpp"
#include "semantics/binding_entry.hpp"
#include "ir/function_builder.hpp"

namespace quidra::lowering {
class BindingOriginRecorder {
public:
    void header(const ir::Function&,const ir::analysis::SemanticBinding&);
    void local(const ir::Function&,const std::string& storage,const std::string& source);
    void local_unknown(const ir::Function&,const std::string& storage);
    void collection(const ir::Function&,const Type& root_type,ir::analysis::CollectionOrigin);
    void rebind(const ir::Function&,ir::analysis::BindingRebindEvent);
    static ir::analysis::InstrRef site(const ir::FunctionBuilder&);
    void traversal(const ir::FunctionBuilder&,const TraversalPlan&,ir::analysis::BindingTraversalOrigin::Kind,
        ir::ValueId root,std::optional<ir::ValueId> output={},std::optional<ir::ValueId> address={});
    void split_shape_entry(const std::string& wrapper,const std::string& body,
        std::size_t first_block,std::size_t first_instruction);
    void remap_reference_storage(const std::string&,const std::vector<std::vector<std::size_t>>&,
        const std::set<std::string>& removed_locals);
    std::shared_ptr<const ir::analysis::BindingOriginSnapshots> finish(const ir::Module&,
        const ir::analysis::SummaryBindings&,const semantics::BindingEntryInitializations*) const;
private:
    std::map<std::string,ir::analysis::FunctionBindingSource,std::less<>> functions_;
};
} // namespace quidra::lowering
