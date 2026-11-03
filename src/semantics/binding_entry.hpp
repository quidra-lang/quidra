#pragma once
#include "semantics/storage_path.hpp"
#include "quidra/checker.hpp"

namespace quidra::semantics {
struct BindingEntryInitialization {
    bool complete{};
    std::map<std::size_t,std::vector<std::vector<PathStep>>> parameters;
};
struct BindingEntryInitializations {
    std::map<std::string,BindingEntryInitialization,std::less<>> functions;
};
// Called from the checker at the exact entry-state establishment site. Only
// non-reference source value parameters are certified; receivers and L13
// required/initializes summaries are not an entry validation receipt.
BindingEntryInitialization binding_value_parameter_entry(const FunctionType&,
    const std::unordered_map<std::string,ClassTypeInfo>&,std::size_t first_parameter);
} // namespace quidra::semantics
