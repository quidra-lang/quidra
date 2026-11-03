#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

#if __has_include("../src/profile_cli.hpp")
#include "profile_cli.hpp"
#include "platform/environment.hpp"
namespace fs = std::filesystem;
using namespace quidra::cli;
namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected, message);
}
void env(const char* name, const std::string& value) {
#ifdef _WIN32
    check(_putenv_s(name, value.c_str()) == 0, "environment setup");
#else
    check(setenv(name, value.c_str(), 1) == 0, "environment setup");
#endif
}
const std::string profile = R"JSON({"schema":1,"clock_units":"ns","complete":true,
"dropped_records":{"invalid":0,"storage":0},"open_ranges":0,
"configuration":{"split_requested":true,"capture_step":12,"device_instrumentation":"pending"},
"traceEvents":[],"aggregate":[
{"node_id":"b","owner":"core","kernel":"second","calls":2,"host_calls":2,"gpu_calls":2,
 "host_unknown_calls":0,"gpu_unknown_calls":0,"host_ns":5000,"gpu_ns":11000,"bytes":18446744073709551615,
 "host_us":5.000,"gpu_us":11.000,"overflow":false,"site":{"file":"unit.qui","node_id":"b","line":1,"column":1}},
{"node_id":"a","owner":"core","kernel":"first","calls":1,"host_calls":1,"gpu_calls":0,
 "host_unknown_calls":0,"gpu_unknown_calls":1,"host_ns":5000,"gpu_ns":0,"bytes":16,
 "host_us":5.000,"gpu_us":0.000,"overflow":false,"site":{"file":"unit.qui","node_id":"a","line":2,"column":1}}
]})JSON";
const std::string timeline = R"JSON({"schema":1,"clock_units":"ns","clock":"mach_absolute_ns","complete":true,
"dropped_records":{"invalid":0,"storage":0},"step_attribution":"boundary convention",
"steps":[{"point":"step_boundary","step":0,"host_ns":1000,"host_available":true},
 {"point":"step_boundary","step":1,"host_ns":11000,"host_available":true},
 {"point":"step_boundary","step":2,"host_ns":23000,"host_available":true}],
"waits":[{"step":1,"begin_ns":2000,"end_ns":4000,"timing_available":true},
 {"step":2,"begin_ns":12000,"end_ns":16000,"timing_available":true}],
"gpu":[{"device":0,"serial":0,"commit_step":0,"gpu_begin_ns":500,"gpu_end_ns":1000,"timing_available":true},
 {"device":0,"serial":1,"commit_step":1,"gpu_begin_ns":2000,"gpu_end_ns":5000,"timing_available":true},
 {"device":0,"serial":2,"commit_step":1,"gpu_begin_ns":6000,"gpu_end_ns":7000,"timing_available":true},
 {"device":0,"serial":3,"commit_step":2,"gpu_begin_ns":10000,"gpu_end_ns":16000,"timing_available":true}],
"gpu_steps":[],"marks":[],"warmups":[],"compiles":[]})JSON";
const std::string typed_profile = R"JSON({"schema":2,"clock_units":"ns","complete":true,
"gpu_aggregation":"typed_activity_container","bytes_scope":"inclusive_events",
"dropped_records":{"invalid":0,"storage":0},"open_ranges":0,
"configuration":{"split_requested":false,"capture_step":null,"device_instrumentation":"producer"},
"traceEvents":[],"aggregate":[
{"node_id":"mixed","owner":"core","kernel":"shared","calls":7,"host_calls":7,"gpu_calls":2,
 "host_unknown_calls":0,"gpu_unknown_calls":1,"host_ns":700,"gpu_ns":3000,"bytes":18446744073709551615,
 "activity_calls":3,"activity_gpu_known_calls":2,"activity_gpu_unknown_calls":1,"activity_gpu_ns":3000,"activity_bytes":56,"activity_overflow":false,
 "container_calls":3,"container_gpu_known_calls":2,"container_gpu_unknown_calls":1,"container_gpu_ns":28000,"container_bytes":18446744073709551615,"container_overflow":true,
 "host_us":0.700,"gpu_us":3.000,"overflow":true,"site":{"file":"unit.qui","node_id":"mixed","line":1,"column":1}},
{"node_id":"container","owner":"core","kernel":"large-container","calls":1,"host_calls":1,"gpu_calls":0,
 "host_unknown_calls":0,"gpu_unknown_calls":0,"host_ns":100,"gpu_ns":0,"bytes":1,
 "activity_calls":0,"activity_gpu_known_calls":0,"activity_gpu_unknown_calls":0,"activity_gpu_ns":0,"activity_bytes":0,"activity_overflow":false,
 "container_calls":1,"container_gpu_known_calls":1,"container_gpu_unknown_calls":0,"container_gpu_ns":999000,"container_bytes":1,"container_overflow":false,
 "host_us":0.100,"gpu_us":0.000,"overflow":false,"site":{"file":"container.qui","node_id":"container","line":2,"column":1}}
]})JSON";
std::string replace(std::string input, const std::string& old, const std::string& value) {
    const auto position = input.find(old); check(position != std::string::npos, "fixture replacement");
    return input.replace(position, old.size(), value);
}
void write(const fs::path& path, const std::string& text) {
    std::ofstream out(path); out << text; check(out.good(), "fixture write");
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 3) {
            std::ifstream input(argv[2], std::ios::binary);
            check(input.good(), "producer report unreadable");
            std::ostringstream document; document << input.rdbuf();
            const std::string mode = argv[1];
            if (mode == "--read-profile") (void)read_profile_report(document.str());
            else if (mode == "--read-timeline") (void)read_timeline_report(document.str());
            else throw std::runtime_error("unknown producer reader mode");
            std::cout << "runtime producer schema: ok\n";
            return 0;
        }
        check(argc == 1, "unexpected test arguments");
        const auto options = parse_profile_options({"--top", "1", "--split", "--roofline", "--", "--top", "two words"});
        check(options.top == 1 && options.split && options.roofline &&
              options.program_arguments == std::vector<std::string>{"--top", "two words"}, "options or program args changed");
        for (const auto& bad : {std::vector<std::string>{"--top", "0"}, {"--top", "-1"},
                {"--top", "18446744073709551616"}, {"--top"}, {"--split", "--split"}, {"--no-cache"}})
            rejects([&] { (void)parse_profile_options(bad); }, "bad option accepted");
        const auto summary = read_profile_report(profile);
        check(summary.rows[0].bytes == std::numeric_limits<std::uint64_t>::max(), "uint64 rounded");
        for (const auto& bad : {replace(profile, "18446744073709551615", "18446744073709551616"),
                replace(profile, "\"schema\":1", "\"schema\":2"),
                replace(profile, "\"schema\":1", "\"schema\":1,\"schema\":1"),
                replace(profile, "\"calls\":2", "\"calls\":2.0"),
                replace(profile, "\"host_unknown_calls\":0", "\"host_unknown_calls\":1")})
            rejects([&] { (void)read_profile_report(bad); }, "invalid report accepted");
        const auto steps = read_timeline_report(timeline);
        check(steps.host_ns == std::vector<std::uint64_t>{8000, 8000}, "host decomposition");
        check(steps.wait_ns == std::vector<std::uint64_t>{2000, 4000}, "wait decomposition");
        check(steps.gpu_ns == std::vector<std::uint64_t>{4000, 6000}, "GPU decomposition");
        check(steps.idle_ns == std::vector<std::uint64_t>{2000, 3000}, "GPU idle decomposition");
        check(steps.approximate && !steps.sampled, "commit attribution understated");
        const auto partial = read_timeline_report(replace(timeline, "\"complete\":true", "\"complete\":false"));
        check(partial.host_ns.empty() && partial.gpu_ns.empty(), "partial timeline fabricated complete metrics");
        const auto unknown = read_timeline_report(replace(timeline,
            "\"gpu_begin_ns\":2000,\"gpu_end_ns\":5000,\"timing_available\":true",
            "\"gpu_begin_ns\":0,\"gpu_end_ns\":0,\"timing_available\":false"));
        check(unknown.gpu_ns == std::vector<std::uint64_t>{6000} && unknown.idle_ns.empty(),
              "unknown GPU timing treated as zero or idle continuity invented");
        const auto sampled = read_timeline_report(replace(timeline, "\"gpu_steps\":[]", R"JSON("gpu_steps":[
            {"device":0,"serial":0,"step":0,"gpu_begin_ns":500,"gpu_end_ns":1000,"timing_available":true},
            {"device":0,"serial":1,"step":1,"gpu_begin_ns":2000,"gpu_end_ns":5000,"timing_available":true},
            {"device":0,"serial":2,"step":1,"gpu_begin_ns":6000,"gpu_end_ns":7000,"timing_available":true},
            {"device":0,"serial":3,"step":2,"gpu_begin_ns":10000,"gpu_end_ns":16000,"timing_available":true}])JSON"));
        check(sampled.sampled && !sampled.approximate && sampled.gpu_ns == steps.gpu_ns,
              "sampled attribution replaced with commit attribution");
        const auto encoder_timeline = replace(replace(replace(timeline,
            "\"serial\":1,\"commit_step\":1,\"gpu_begin_ns\":2000,\"gpu_end_ns\":5000",
            "\"serial\":1,\"commit_step\":1,\"gpu_begin_ns\":2000,\"gpu_end_ns\":7000"),
            "{\"device\":0,\"serial\":2,\"commit_step\":1,\"gpu_begin_ns\":6000,\"gpu_end_ns\":7000,\"timing_available\":true},", ""),
            "\"gpu_steps\":[]", R"JSON("gpu_steps":[
            {"device":0,"serial":0,"step":0,"gpu_begin_ns":500,"gpu_end_ns":1000,"timing_available":true},
            {"device":0,"serial":1,"step":1,"gpu_begin_ns":2000,"gpu_end_ns":5000,"timing_available":true},
            {"device":0,"serial":1,"step":1,"gpu_begin_ns":6000,"gpu_end_ns":7000,"timing_available":true},
            {"device":0,"serial":3,"step":2,"gpu_begin_ns":10000,"gpu_end_ns":16000,"timing_available":true}])JSON");
        const auto encoder_steps = read_timeline_report(encoder_timeline);
        check(encoder_steps.sampled && !encoder_steps.approximate &&
              encoder_steps.gpu_ns == std::vector<std::uint64_t>{4000, 6000} &&
              encoder_steps.idle_ns == std::vector<std::uint64_t>{2000, 3000},
              "separate encoder samples in one command buffer lost busy time or idle gaps");
        const auto unavailable_timeline = replace(replace(timeline, "\"gpu_steps\":[]", R"JSON("gpu_steps":[
            {"device":0,"serial":0,"step":0,"gpu_begin_ns":0,"gpu_end_ns":0,"timing_available":false},
            {"device":0,"serial":1,"step":1,"gpu_begin_ns":0,"gpu_end_ns":0,"timing_available":false},
            {"device":0,"serial":2,"step":1,"gpu_begin_ns":0,"gpu_end_ns":0,"timing_available":false},
            {"device":0,"serial":3,"step":2,"gpu_begin_ns":0,"gpu_end_ns":0,"timing_available":false}])JSON"),
            "\"marks\":[]", R"JSON("gpu_attribution_complete":false,"gpu_sampling_gaps":[
                {"device":0,"serial":1,"step":1,"compute":true,"additional_encoder":false,
                 "timing_incomplete":true,"reason":"unsupported_stage_sampling"}],"marks":[])JSON");
        const auto unavailable = read_timeline_report(unavailable_timeline);
        check(!unavailable.sampled && unavailable.sampling_attempted && !unavailable.approximate &&
              unavailable.unavailable_samples && unavailable.gpu_ns.empty() &&
              unavailable.gpu_attribution_complete == false && unavailable.sampling_gaps[0].timing_incomplete,
              "unavailable stage records advertised as successful sampling");
        const auto legacy_gap = read_timeline_report(replace(unavailable_timeline, "\"timing_incomplete\":true,", ""));
        check(legacy_gap.sampling_gaps[0].timing_incomplete, "legacy gap assumed complete timing");
        const auto coarse_timeline = replace(encoder_timeline, "\"marks\":[]", R"JSON(
            "gpu_attribution_complete":true,"gpu_leaf_attribution_complete":false,"gpu_sampling_gaps":[
            {"device":0,"serial":1,"step":1,"compute":true,"additional_encoder":false,
             "timing_incomplete":false,"reason":"coarse_kernel_attribution"}],"marks":[])JSON");
        const auto coarse = read_timeline_report(coarse_timeline);
        check(coarse.sampled && coarse.gpu_attribution_complete == true && coarse.gpu_leaf_attribution_complete == false &&
              !coarse.sampling_gaps[0].timing_incomplete && coarse.gpu_ns == steps.gpu_ns,
              "coarse leaf attribution discarded valid stage timings");
        const auto partial_samples = read_timeline_report(replace(replace(coarse_timeline,
            "\"gpu_attribution_complete\":true", "\"gpu_attribution_complete\":false"),
            "\"step\":1,\"gpu_begin_ns\":2000,\"gpu_end_ns\":5000,\"timing_available\":true",
            "\"step\":1,\"gpu_begin_ns\":0,\"gpu_end_ns\":0,\"timing_available\":false"));
        check(partial_samples.sampled && partial_samples.unavailable_samples &&
              partial_samples.gpu_ns == std::vector<std::uint64_t>{6000}, "partial sampling fabricated step busy time");
        const auto held_timeline = replace(replace(replace(coarse_timeline,
            "\"gpu_attribution_complete\":true", "\"gpu_attribution_complete\":false"),
            "\"timing_incomplete\":false", "\"timing_incomplete\":true"),
            "coarse_kernel_attribution", "package_hold_at_boundary");
        const auto held = read_timeline_report(held_timeline);
        check(held.gpu_ns == std::vector<std::uint64_t>{6000} && held.idle_ns.empty(),
              "held encoder gap fabricated complete step GPU metrics");
        const std::string spanning_timeline = R"JSON({"schema":1,"clock_units":"ns","clock":"host","complete":true,
            "dropped_records":{"invalid":0,"storage":0},"step_attribution":"boundary convention",
            "gpu_attribution_complete":false,"gpu_leaf_attribution_complete":false,
            "gpu_sampling_gaps":[{"device":0,"serial":1,"step":1,"compute":true,"additional_encoder":false,
                "timing_incomplete":true,"reason":"package_hold_at_boundary"}],
            "steps":[{"point":"step_boundary","step":0,"host_ns":100,"host_available":true},
                {"point":"step_boundary","step":1,"host_ns":10000,"host_available":true},
                {"point":"step_boundary","step":2,"host_ns":20000,"host_available":true},
                {"point":"step_boundary","step":3,"host_ns":30000,"host_available":true},
                {"point":"step_boundary","step":4,"host_ns":40000,"host_available":true},
                {"point":"step_boundary","step":5,"host_ns":50000,"host_available":true}],
            "waits":[],"gpu":[{"device":0,"serial":1,"commit_step":4,"gpu_begin_ns":1000,"gpu_end_ns":39000,"timing_available":true}],
            "gpu_steps":[{"device":0,"serial":1,"step":1,"gpu_begin_ns":1000,"gpu_end_ns":39000,"timing_available":true},
                {"device":1,"serial":10,"step":0,"gpu_begin_ns":100,"gpu_end_ns":200,"timing_available":true},
                {"device":1,"serial":11,"step":1,"gpu_begin_ns":2000,"gpu_end_ns":3000,"timing_available":true},
                {"device":1,"serial":12,"step":2,"gpu_begin_ns":12000,"gpu_end_ns":13000,"timing_available":true},
                {"device":1,"serial":13,"step":3,"gpu_begin_ns":22000,"gpu_end_ns":23000,"timing_available":true},
                {"device":1,"serial":14,"step":4,"gpu_begin_ns":32000,"gpu_end_ns":33000,"timing_available":true},
                {"device":1,"serial":15,"step":5,"gpu_begin_ns":42000,"gpu_end_ns":43000,"timing_available":true}],
            "marks":[],"warmups":[],"compiles":[]})JSON";
        const auto spanning = read_timeline_report(spanning_timeline);
        check(spanning.gpu_ns == std::vector<std::uint64_t>{1000} && spanning.idle_ns.empty() &&
              spanning.host_ns.size() == 5 && spanning.wait_ns.size() == 5,
              "spanning held encoder omitted a participating device from later step totals");
        const auto resumed = read_timeline_report(replace(spanning_timeline, "\"gpu_steps\":[", R"JSON("gpu_steps":[
            {"device":0,"serial":1,"step":3,"gpu_begin_ns":24000,"gpu_end_ns":25000,"timing_available":true},)JSON"));
        check(resumed.gpu_ns == std::vector<std::uint64_t>({2000, 1000, 1000}) && resumed.host_ns.size() == 5,
              "next valid sampled span failed to end the incomplete coverage range");
        for (const auto& bad : {
                replace(unavailable_timeline, "\"gpu_attribution_complete\":false", "\"gpu_attribution_complete\":true"),
                replace(coarse_timeline, "\"gpu_leaf_attribution_complete\":false", "\"gpu_leaf_attribution_complete\":true"),
                replace(unavailable_timeline, "\"timing_incomplete\":true", "\"timing_incomplete\":\"true\""),
                replace(unavailable_timeline, "\"reason\":\"unsupported_stage_sampling\"", "\"reason\":\"\""),
                replace(unavailable_timeline, "\"serial\":1,\"step\":1,\"compute\":true", "\"serial\":18446744073709551616,\"step\":1,\"compute\":true"),
                replace(unavailable_timeline, "\"serial\":1,\"step\":1,\"compute\":true", "\"serial\":1,\"step\":4294967296,\"compute\":true")})
            rejects([&] { (void)read_timeline_report(bad); }, "invalid GPU attribution metadata accepted");
        rejects([&] { (void)read_timeline_report(replace(timeline,
            "\"serial\":2,\"commit_step\":1", "\"serial\":1,\"commit_step\":1")); },
            "duplicate whole command buffer accepted");
        const auto gap = read_timeline_report(replace(timeline,
            "{\"point\":\"step_boundary\",\"step\":1,\"host_ns\":11000,\"host_available\":true},", ""));
        check(gap.host_ns.empty() && gap.gpu_ns.empty(), "missing boundary joined into a fabricated step");
        for (const auto& bad : {replace(timeline, "\"host_ns\":23000", "\"host_ns\":10000"),
                replace(timeline, "\"step\":2,\"host_ns\":23000", "\"step\":1,\"host_ns\":23000"),
                replace(timeline, "\"gpu_begin_ns\":10000,\"gpu_end_ns\":16000",
                    "\"gpu_begin_ns\":16000,\"gpu_end_ns\":10000"),
                replace(timeline, "\"complete\":true", "\"complete\":true,\"complete\":true")})
            rejects([&] { (void)read_timeline_report(bad); }, "invalid timeline accepted");
        const auto root = fs::temp_directory_path() /
            ("quidra-profile-cli-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
        struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{root};
        env("QUIDRA_CACHE_DIR", root.string()); env("QUIDRA_PROFILE", "parent-compiler-marker");
        int calls = 0;
        CachedProfileRunner callback = [&](const fs::path& source, const auto& args, const auto& environment) {
            ++calls; check(source == fs::path("unit.qui"), "source changed");
            check(args == options.program_arguments, "child args changed");
            check(quidra::platform::environment_value("QUIDRA_PROFILE") == "parent-compiler-marker", "compiler environment polluted");
            check(environment.size() == 3, "unexpected child overrides");
            for (const auto& [name, value] : environment) {
                if (name == "QUIDRA_PROFILE") { check(value.has_value(), "missing trace path"); write(fs::path(*value), profile); }
                if (name == "QUIDRA_TIMELINE") { check(value.has_value(), "missing timeline path"); write(fs::path(*value), timeline); }
                if (name == "QUIDRA_PROFILE_SPLIT") check(value == "1", "split not passed to child");
            }
            return 101;
        };
        std::ostringstream output, errors;
        check(run_profile_cli("unit.qui", {"--top", "1", "--split", "--roofline", "--", "--top", "two words"},
                              callback, output, errors) == 101 && calls == 1, "program status/callback changed");
        const auto text = output.str();
        check(text.find("H=8.0000us W=3.0000us G=5.0000us I=2.5000us") != std::string::npos, "exact medians missing");
        check(text.find("first node=a") < text.find("second node=b"), "stable host tie ordering");
        check(text.find("legacy schema 1") != std::string::npos, "unclassified legacy GPU spans presented as activity");
        check(text.find("Roofline unavailable") != std::string::npos && text.find("capture step=12") != std::string::npos,
              "availability/config flags missing");
        check(errors.str().empty(), "valid reports emitted error");
        const auto typed = read_profile_report(typed_profile);
        check(typed.rows[0].gpu_ns == 3000 && typed.rows[0].gpu_unknown_calls == 1,
              "typed GPU alias/unknown denominator changed");
        for (const auto& bad : {
                replace(typed_profile, "\"activity_calls\":3", "\"activity_calls\":2"),
                replace(typed_profile, "\"container_calls\":3", "\"container_calls\":5"),
                replace(typed_profile, "\"gpu_ns\":3000", "\"gpu_ns\":3100"),
                replace(typed_profile, "\"activity_bytes\":56", "\"activity_bytes\":18446744073709551616"),
                replace(typed_profile, "\"gpu_aggregation\":\"typed_activity_container\"", "\"gpu_aggregation\":\"mixed\""),
                replace(typed_profile, "\"activity_overflow\":false", "\"activity_overflow\":true")})
            rejects([&] { (void)read_profile_report(bad); }, "inconsistent typed metrics accepted");
        CachedProfileRunner typed_runner = [&](const auto&, const auto&, const auto& environment) {
            for (const auto& [name, value] : environment) {
                if (name == "QUIDRA_PROFILE") write(fs::path(*value), typed_profile);
                if (name == "QUIDRA_TIMELINE") write(fs::path(*value), timeline);
            }
            return 0;
        };
        std::ostringstream typed_output, typed_errors;
        check(run_profile_cli("unit.qui", {"--top", "1"}, typed_runner, typed_output, typed_errors) == 0 &&
              typed_errors.str().empty(), "typed report extraction failed");
        const auto typed_text = typed_output.str();
        const auto gpu_top = typed_text.substr(typed_text.find("Top GPU"));
        check(gpu_top.find("shared node=mixed") != std::string::npos &&
              gpu_top.find("large-container node=") == std::string::npos &&
              gpu_top.find("time=3.0000us calls=3 timed=2 unknown=1 bytes=56") != std::string::npos &&
              gpu_top.find("saturated") == std::string::npos,
              "GPU Top N includes container time/bytes/overflow or uses all-event denominator");
        check(typed_text.find("GPU containers") != std::string::npos && typed_text.find("unclassified") == std::string::npos,
              "typed container availability missing");
        const auto labels = [&](const std::string& fixture) {
            CachedProfileRunner runner = [&](const auto&, const auto&, const auto& environment) {
                for (const auto& [name, value] : environment) {
                    if (name == "QUIDRA_PROFILE") write(fs::path(*value), typed_profile);
                    if (name == "QUIDRA_TIMELINE") write(fs::path(*value), fixture);
                }
                return 101;
            };
            std::ostringstream output, errors;
            check(run_profile_cli("unit.qui", {}, runner, output, errors) == 101 && errors.str().empty(),
                  "sampling availability altered status/report extraction");
            return output.str();
        };
        const auto unavailable_text = labels(unavailable_timeline);
        check(unavailable_text.find("G=unavailable") != std::string::npos &&
              unavailable_text.find("GPU stage sampling: unavailable") != std::string::npos &&
              unavailable_text.find("sampled step spans") == std::string::npos &&
              unavailable_text.find("unsupported_stage_sampling count=1 (timing incomplete)") != std::string::npos,
              "unsupported sampling label overstated availability");
        const auto coarse_text = labels(coarse_timeline);
        check(coarse_text.find("G=5.0000us") != std::string::npos &&
              coarse_text.find("GPU timing attribution: complete") != std::string::npos &&
              coarse_text.find("GPU leaf attribution: coarse or unavailable") != std::string::npos &&
              coarse_text.find("coarse_kernel_attribution count=1 (coarse leaf attribution)") != std::string::npos &&
              coarse_text.find("partial timing coverage") == std::string::npos,
              "coarse source attribution mislabeled valid sampling as unavailable");
        std::ostringstream missing_output, missing_errors;
        CachedProfileRunner missing = [](const auto&, const auto&, const auto&) { return 7; };
        check(run_profile_cli("unit.qui", {}, missing, missing_output, missing_errors) == 7,
              "missing report changed program status");
        check(missing_errors.str().find("unavailable") != std::string::npos, "missing report hidden");
        check(calls == 1 && quidra::platform::environment_value("QUIDRA_PROFILE") == "parent-compiler-marker",
              "parent environment not preserved");
        std::ostringstream invalid_output, invalid_errors;
        check(run_profile_cli("unit.qui", {"--top", "0"}, callback, invalid_output, invalid_errors) == 2 && calls == 1,
              "invalid options ran program");
        const auto explicit_trace = root / "explicit-trace.json", explicit_timeline = root / "explicit-timeline.json";
        write(explicit_trace, profile); write(explicit_timeline, timeline);
        // Explicit paths work without resolving or creating a default cache.
        env("QUIDRA_CACHE_DIR", (root / "cache-must-not-be-created").string());
        int explicit_calls = 0;
        CachedProfileRunner explicit_runner = [&](const auto&, const auto& args, const auto& environment) {
            ++explicit_calls; check(args.empty(), "explicit args changed");
            check(!fs::exists(explicit_trace) && !fs::exists(explicit_timeline), "stale output retained");
            for (const auto& [name, value] : environment) {
                if (name == "QUIDRA_PROFILE") { check(fs::path(*value) == fs::weakly_canonical(explicit_trace), "explicit trace destination changed"); write(explicit_trace, "malformed"); }
                if (name == "QUIDRA_TIMELINE") { check(fs::path(*value) == fs::weakly_canonical(explicit_timeline), "explicit timeline destination changed"); write(explicit_timeline, timeline); }
                if (name == "QUIDRA_PROFILE_SPLIT") check(!value, "inherited split not unset for child");
            }
            return 9;
        };
        std::ostringstream explicit_output, explicit_errors;
        check(run_profile_cli("unit.qui", {"--trace", explicit_trace.string(), "--timeline", explicit_timeline.string()},
              explicit_runner, explicit_output, explicit_errors) == 9 && explicit_calls == 1,
              "malformed report changed program status");
        check(explicit_errors.str().find("Profile measurements unavailable") != std::string::npos &&
              explicit_output.str().find("H=8.0000us") != std::string::npos &&
              !fs::exists(root / "cache-must-not-be-created"), "explicit/malformed report handling");
        check(run_profile_cli("unit.qui", {"--trace", explicit_trace.string(), "--timeline", explicit_trace.string()},
              explicit_runner, invalid_output, invalid_errors) == 2 && explicit_calls == 1,
              "same report destinations ran program");
        check(run_profile_cli("unit.qui", {"--trace", "stderr", "--timeline", explicit_timeline.string()},
              explicit_runner, invalid_output, invalid_errors) == 2 && explicit_calls == 1,
              "stream destination ran program");
        const auto source_file = root / "source.qui";
        const auto preserved_trace = root / "preserved-trace.json", preserved_timeline = root / "preserved-timeline.json";
        int collision_calls = 0;
        CachedProfileRunner collision_runner = [&](const auto&, const auto&, const auto&) { ++collision_calls; return 7; };
        const auto collision = [&](const fs::path& trace, const fs::path& timeline) {
            write(source_file, "print(1)\n"); write(preserved_trace, "old trace"); write(preserved_timeline, "old timeline");
            std::ostringstream output, errors;
            check(run_profile_cli(source_file, {"--trace", trace.string(), "--timeline", timeline.string()},
                  collision_runner, output, errors) == 2 && collision_calls == 0,
                  "source/report collision ran the program");
            const auto contents = [](const fs::path& path) { std::ifstream input(path); std::ostringstream bytes; bytes << input.rdbuf(); return bytes.str(); };
            check(contents(source_file) == "print(1)\n" && contents(preserved_trace) == "old trace" &&
                  contents(preserved_timeline) == "old timeline", "collision validation removed source or existing reports");
        };
        collision(source_file, preserved_timeline);
        collision(preserved_trace, root / "." / "source.qui");
        const auto source_link = root / "source-hardlink.json";
        fs::create_hard_link(source_file, source_link);
        collision(source_link, preserved_timeline);
        const auto precise_profile = replace(replace(profile, "\"host_ns\":5000",
            "\"host_ns\":18446744073709551615"), "\"host_ns\":5000", "\"host_ns\":18446744073709551614");
        const auto precise_timeline = replace(replace(replace(timeline,
            "\"host_ns\":1000", "\"host_ns\":18446744073709551611"),
            "\"host_ns\":11000", "\"host_ns\":18446744073709551612"),
            "\"host_ns\":23000", "\"host_ns\":18446744073709551614");
        CachedProfileRunner precise = [&](const auto&, const auto&, const auto&) {
            write(explicit_trace, precise_profile); write(explicit_timeline, precise_timeline); return 0;
        };
        std::ostringstream precise_output, precise_errors;
        check(run_profile_cli("unit.qui", {"--top", "2", "--trace", explicit_trace.string(),
              "--timeline", explicit_timeline.string()}, precise, precise_output, precise_errors) == 0,
              "precise fixture failed");
        check(precise_output.str().find("second node=b") < precise_output.str().find("first node=a") &&
              precise_output.str().find("H=0.0015us") != std::string::npos && precise_errors.str().empty(),
              "uint64 sorting or half-nanosecond median rounded");
        std::cout << "profile CLI parsing, extraction and callback contracts: ok\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
#else
int main() { std::cerr << "FAIL: portable profile CLI helper is missing\n"; return 1; }
#endif
