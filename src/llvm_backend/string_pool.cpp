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
        location.position.line,
        location.position.column});
    return name;
}

namespace {

// The LLVM type of a record with these slots: the abi::ErrorRecord header
// (ptr message, ptr code, i32 schema, i32 column, ptr cause, i64 line,
// ptr statement, ptr hops), then one ptr or i64 per detail slot.
std::string record_type(const ErrorRecordConstant& record) {
    std::string type = "{ ptr, ptr, i32, i32, ptr, i64, ptr, ptr";
    for (const auto& slot : record.slots) type += slot.integer ? ", i64" : ", ptr";
    return type + " }";
}

std::string global_or_null(const std::string& global) {
    return global.empty() ? "null" : "@" + global;
}

std::string record_value(const ErrorRecordConstant& record) {
    std::string value = "{ ptr " + global_or_null(record.message) + ", ptr " +
                        global_or_null(record.code) + ", i32 " + std::to_string(record.schema) +
                        ", i32 " + std::to_string(record.column) + ", ptr null, i64 " +
                        std::to_string(record.line) + ", ptr null, ptr null";
    for (const auto& slot : record.slots)
        value += slot.integer ? ", i64 " + std::to_string(slot.value)
                              : ", ptr " + global_or_null(slot.global);
    return value + " }";
}

} // namespace

std::string StringPool::intern_error_records(std::vector<ErrorRecordConstant> records, bool array) {
    std::string text;
    if (array) {
        const auto type = record_type(records.front());
        text = "[" + std::to_string(records.size()) + " x " + type + "] [";
        for (std::size_t i = 0; i < records.size(); ++i) {
            if (i != 0) text += ", ";
            text += type + " " + record_value(records[i]);
        }
        text += "]";
    } else {
        text = record_type(records.front()) + " " + record_value(records.front());
    }
    if (auto it = error_record_names.find(text); it != error_record_names.end()) return it->second;
    auto name = ".quidra.err." + std::to_string(error_symbols++);
    error_records.emplace_back(name, text);
    error_record_names.emplace(std::move(text), name);
    return name;
}

std::string StringPool::intern_hop_site(const std::string& name, unsigned long long line,
                                        unsigned long long column, unsigned kind) {
    const auto text = "{ ptr, i64, i64, i64 } { ptr @" + intern_error_text(name) + ", i64 " +
                      std::to_string(line) + ", i64 " + std::to_string(column) + ", i64 " +
                      std::to_string(kind) + " }";
    if (auto it = hop_site_names.find(text); it != hop_site_names.end()) return it->second;
    auto site = ".quidra.hop." + std::to_string(hop_sites.size());
    hop_sites.emplace_back(site, text);
    hop_site_names.emplace(text, site);
    return site;
}

} // namespace quidra::llvm_backend
