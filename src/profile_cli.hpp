#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quidra::cli {

struct ProfileOptions {
    std::uint64_t top{10};
    bool split{}, roofline{};
    std::optional<std::filesystem::path> trace, timeline;
    std::vector<std::string> program_arguments;
};
ProfileOptions parse_profile_options(const std::vector<std::string>& arguments);

struct ProfileRow {
    std::string node, owner, kernel, file;
    std::uint64_t line{}, column{}, calls{}, host_calls{}, gpu_calls{};
    std::uint64_t host_unknown_calls{}, gpu_unknown_calls{}, host_ns{}, gpu_ns{}, bytes{};
    bool overflow{};
    std::uint64_t activity_calls{}, activity_gpu_known_calls{}, activity_gpu_unknown_calls{};
    std::uint64_t activity_gpu_ns{}, activity_bytes{};
    std::uint64_t container_calls{}, container_gpu_known_calls{}, container_gpu_unknown_calls{};
    std::uint64_t container_gpu_ns{}, container_bytes{};
    bool activity_overflow{}, container_overflow{};
};
struct ProfileSummary {
    struct Capture {
        std::uint64_t step{};
        std::string state, output_path;
        bool snapshot_complete{};
        std::optional<bool> start_succeeded, stopped, exact_core_step;
        std::optional<bool> external_queues_verified, artifact_exists;
        std::optional<std::uint64_t> extra_commits;
    };
    bool complete{}, split_requested{};
    bool typed_gpu{};
    std::uint64_t invalid_drops{}, storage_drops{}, open_ranges{};
    std::optional<std::uint64_t> capture_step;
    std::optional<Capture> capture;
    std::string device_instrumentation;
    std::vector<ProfileRow> rows;
};
struct TimelineSummary {
    bool complete{}, approximate{}, sampled{};
    std::uint64_t invalid_drops{}, storage_drops{};
    std::vector<std::uint64_t> host_ns, wait_ns, gpu_ns, idle_ns;
    struct SamplingGap {
        std::uint64_t device{}, serial{}, step{};
        std::string reason;
        bool compute{}, additional_encoder{}, timing_incomplete{true};
    };
    bool sampling_attempted{}, unavailable_samples{};
    std::optional<bool> gpu_attribution_complete, gpu_leaf_attribution_complete;
    std::vector<SamplingGap> sampling_gaps;
};
// These readers accept only the runtime's versioned, owned report schemas.
// Integer counters are parsed exactly; Chrome display numbers never round them.
ProfileSummary read_profile_report(std::string_view document);
TimelineSummary read_timeline_report(std::string_view document);

// Applied to the program child only: nullopt means unset. The runner must use
// the existing cached run path and leave its compiler/cache-key environment alone.
using RuntimeEnvironment = std::vector<std::pair<std::string, std::optional<std::string>>>;
using CachedProfileRunner = std::function<int(const std::filesystem::path&,
    const std::vector<std::string>&, const RuntimeEnvironment&)>;

// Parse/path errors return 2 without invoking the runner. After invocation its
// status is preserved, including runtime failures and unavailable reports.
int run_profile_cli(const std::filesystem::path& source,
    const std::vector<std::string>& arguments, const CachedProfileRunner& runner,
    std::ostream& output, std::ostream& errors);

} // namespace quidra::cli
