#include "profile_cli.hpp"
#include <cstdio>
#include <exception>
#include <string>

int main() {
    const std::string document = R"JSON({"schema":2,"clock_units":"ns",
"gpu_aggregation":"typed_activity_container","bytes_scope":"inclusive_events",
"complete":true,"dropped_records":{"invalid":0,"storage":0},"open_ranges":0,
"configuration":{"split_requested":false,"capture_step":0,"device_instrumentation":"available"},
"capture":{"requested_step":0,"state":"stopped","output_path":"capture.gputrace",
"snapshot_complete":true,"start_succeeded":true,"stopped":true,
"exact_core_step":true,"external_queues_verified":false,"artifact_exists":true,"extra_commits":2},
"traceEvents":[],"aggregate":[]})JSON";
    try {
        const auto summary = quidra::cli::read_profile_report(document);
        if (!summary.capture || summary.capture->state != "stopped" ||
            summary.capture->step != 0 || summary.capture->extra_commits != 2 ||
            summary.capture->exact_core_step != true || summary.capture->artifact_exists != true)
            return 1;
        auto unknown = document;
        for (const auto* key : {"start_succeeded", "stopped", "exact_core_step", "external_queues_verified", "artifact_exists"}) {
            const std::string prefix = std::string("\"") + key + "\":";
            const auto position = unknown.find(prefix) + prefix.size();
            unknown.replace(position, unknown[position] == 't' ? 4 : 5, "null");
        }
        unknown.replace(unknown.find("\"extra_commits\":2"), 17, "\"extra_commits\":null");
        unknown.replace(unknown.find("\"snapshot_complete\":true"), 24, "\"snapshot_complete\":false");
        const auto partial = quidra::cli::read_profile_report(unknown);
        if (!partial.capture || partial.capture->snapshot_complete ||
            partial.capture->start_succeeded || partial.capture->extra_commits ||
            partial.capture->exact_core_step || partial.capture->artifact_exists)
            return 1;
        auto wrong_step = document;
        wrong_step.replace(wrong_step.find("\"requested_step\":0"), 18, "\"requested_step\":1");
        try { (void)quidra::cli::read_profile_report(wrong_step); return 1; }
        catch (const std::exception&) {}
        const auto changed = [&](const std::string& from, const std::string& to) {
            auto value = document;
            value.replace(value.find(from), from.size(), to);
            return value;
        };
        const auto rejects = [](const std::string& value) {
            try { (void)quidra::cli::read_profile_report(value); return false; }
            catch (const std::exception&) { return true; }
        };
        for (const auto& contradictory : {
                changed("\"start_succeeded\":true", "\"start_succeeded\":false"),
                changed("\"start_succeeded\":true", "\"start_succeeded\":null"),
                changed("\"stopped\":true", "\"stopped\":false"),
                changed("\"stopped\":true", "\"stopped\":null"),
                changed("\"state\":\"stopped\"", "\"state\":\"active\""),
                changed("\"state\":\"stopped\"", "\"state\":\"invented\"")}) {
            if (!rejects(contradictory)) {
                std::fputs("contradictory complete capture status accepted\n", stderr);
                return 1;
            }
        }
        auto retained = changed("\"start_succeeded\":true", "\"start_succeeded\":false");
        retained.replace(retained.find("\"snapshot_complete\":true"), 24, "\"snapshot_complete\":false");
        if (quidra::cli::read_profile_report(retained).capture->snapshot_complete) return 1;
        auto maximum = changed("\"capture_step\":0", "\"capture_step\":18446744073709551615");
        maximum.replace(maximum.find("\"requested_step\":0"), 18,
                        "\"requested_step\":18446744073709551615");
        if (quidra::cli::read_profile_report(maximum).capture->step != UINT64_MAX) return 1;
        std::puts("capture CLI status preserves exact, unavailable and partial fields: ok");
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
