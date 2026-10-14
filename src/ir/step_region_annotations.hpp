#pragma once

// Common IR annotation ownership only. The complete report is immutable and
// forward-owned; lowering never includes its analysis or CFG definitions.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace quidra::ir::analysis {
struct StepRegionReport;
struct StepRegionAnnotationAccess;
}
namespace quidra::ir {
struct StepRegionAnnotationRow {
    std::size_t function{};
    std::string lookup;
    std::string function_fields, report_fields;
    std::shared_ptr<const analysis::StepRegionReport> report;
};
struct StepRegionAnnotationData {
    std::uint32_t version{1};
    std::string environment_fields;
    std::vector<StepRegionAnnotationRow> rows;
    std::uint64_t operations{};
    std::size_t encoded_bytes{};
    // Formation's reported work excludes its upstream analyses. Neither
    // this owner nor its byte/work counters claim an end-to-end bound.
    bool upstream_budget_complete{};
};
class StepRegionAnnotations {
public:
    const StepRegionAnnotationData& data() const noexcept { return data_; }
private:
    explicit StepRegionAnnotations(StepRegionAnnotationData data)
        : data_(std::move(data)) {}
    StepRegionAnnotationData data_;
    friend struct analysis::StepRegionAnnotationAccess;
};
} // namespace quidra::ir
