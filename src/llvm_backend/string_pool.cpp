#include "llvm_backend/string_pool.hpp"

namespace quidra::llvm_backend {

std::string StringPool::intern_provenance(const ir::SourceLocation& location) {
    auto name = ".quidra.source." + std::to_string(provenance_entries.size());
    provenance_entries.push_back(SourceProvenanceConstant{
        name,
        intern(location.source_file),
        intern(location.source_revision),
        intern(location.node_id),
        intern(location.node_kind),
        location.line,
        location.column});
    return name;
}

} // namespace quidra::llvm_backend
