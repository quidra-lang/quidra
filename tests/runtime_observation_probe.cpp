// Exercises the generated-program entry and native observation ABI together.
#include "quidra/abi/layout.hpp"
#include "quidra/abi/runtime_entry_points.hpp"
#include "quidra/native_extension.h"
#include "runtime_report.hpp"
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>

extern "C" void quidra_parallel_call_require_accept(bool accepted);

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string_view mode = argv[1];
    if (mode == "--compiler") {
        (void)quidra_time_now(false);
        return 0;
    }
    // Linked C hosts may register callbacks before their first Quidra entry.
    // Observer finalization must leave these callbacks in the process totals.
    if (std::atexit([] { qcore_counter_note_refusal("test", "host exit callback", 1); }) != 0)
        return 5;
    quidra_runtime_set_args(argc, argv);
    const quidra::abi::SourceProvenanceRecord first{
        "observation.qui", "", "first", "statement", 3, 1};
    const quidra::abi::SourceProvenanceRecord second{
        "observation.qui", "", "second", "statement", 4, 1};
    quidra_runtime_set_source_provenance(&first);
    quidra_runtime_set_source_provenance(&first);
    const auto token = qcore_profile_range_begin("test", "native range", -1);
    const double before = quidra_time_now(false);
    const double after = quidra_time_now(false);
    if (after < before) return 3;
    qcore_profile_range_end(token);
    const quidra::abi::SourceProvenanceRecord body{
        "observation.qui", "", "worker body", "statement", 7, 1};
    std::thread worker([&] {
        const auto saved = quidra::runtime::save_node_source_state();
        quidra_runtime_set_source_provenance(&body);
        quidra::runtime::restore_node_source_state(saved);
    });
    worker.join();
    if (qcore_parallel_for(0, 4, 1,
        [](std::uint64_t, std::uint64_t, void*) -> int { return QCORE_PARALLEL_OK; }, nullptr)
        != QCORE_PARALLEL_OK) return 4;
    qcore_counters_mark("explicit");
    quidra_runtime_set_source_provenance(&second);
    if (mode == "--failure") quidra_parallel_call_require_accept(false);
    if (mode != "--normal") return 2;
    quidra_runtime_set_source_provenance(nullptr);
    std::puts("observation result");
    return 0;
}
