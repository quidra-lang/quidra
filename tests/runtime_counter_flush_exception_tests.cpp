#include "runtime_counters.hpp"
#include "platform/environment.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <share.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
std::atomic<bool> reject_allocations{};

void require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "counter flush exception test failed: %s\n", message);
    std::exit(1);
}

void environment(const char* name, const char* value) {
#ifdef _WIN32
    require(_putenv_s(name, value ? value : "") == 0, "setting child environment");
#else
    require((value ? setenv(name, value, 1) : unsetenv(name)) == 0,
            "setting child environment");
#endif
}

int child_status(const std::string& executable, const char* mode) {
#ifdef _WIN32
    return static_cast<int>(_spawnl(_P_WAIT, executable.c_str(), executable.c_str(),
                                   mode, static_cast<char*>(nullptr)));
#else
    const auto pid = fork();
    require(pid >= 0, "starting child process");
    if (pid == 0) {
        execl(executable.c_str(), executable.c_str(), mode, static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    require(waitpid(pid, &status, 0) == pid, "waiting for child process");
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

std::uint64_t field(const std::string& row, const char* name) {
    const auto key = std::string("\"") + name + "\":";
    const auto position = row.find(key);
    require(position != std::string::npos, "missing JSON field");
    return std::stoull(row.substr(position + key.size()));
}

int run_child(const char* mode) {
    const bool capacity = std::string(mode) == "--capacity";
    const bool failure = std::string(mode) == "--failure";
    const bool empty = std::string(mode) == "--empty";
    const auto errors = quidra::platform::environment_value("QUIDRA_TEST_COUNTER_ERRORS");
    const auto report_path = quidra::platform::environment_value("QUIDRA_COUNTERS");
    require(errors && report_path, "child capture paths are configured");
#ifdef _WIN32
    std::FILE* error_stream = nullptr;
    require(::freopen_s(&error_stream, errors->c_str(), "w", stderr) == 0 && error_stream,
            "opening child error capture");
#else
    require(std::freopen(errors->c_str(), "w", stderr), "opening child error capture");
#endif
    quidra::counters::mark_program_started();
    for (int step = 0; step < (empty ? 0 : capacity ? 4096 : 1); ++step) {
        quidra::counters::add(quidra::counters::Id::CoreDispatches, capacity ? 1 : 3);
        quidra::counters::step_boundary(quidra::counters::StepEvent::Mark,
                                      "first \"quoted\"\t\\line\n");
    }
    if (!empty) {
        quidra::counters::add(quidra::counters::Id::CoreDispatches, 2);
        quidra::counters::note_wait(quidra::counters::WaitReason::Sync);
        quidra::counters::note_refusal("review", "failure", 7);
    }
    if (failure || empty) quidra::counters::set_unlocated_failure_reporter(nullptr);

    // A report must still be usable when the runtime is failing because it
    // cannot allocate. This rejects final map copies, vector growth and JSON
    // string building while retaining the real report code and file I/O.
    reject_allocations = true;
    if (failure || empty)
        quidra::counters::report_unlocated_failure(
            quidra::abi::FailureReason::tensor_failure, "forced counter failure", true);
    quidra::counters::flush_report();
    quidra::counters::flush_report();
#ifdef _WIN32
    auto* report = _fsopen(report_path->c_str(), "r", _SH_DENYNO);
#else
    auto* report = std::fopen(report_path->c_str(), "r");
#endif
    if (!report) {
        std::fprintf(stderr, "report open failed: errno=%d, enabled=%d, path=%s\n",
                     errno, quidra::counters::enabled() ? 1 : 0, report_path->c_str());
    }
    require(report != nullptr, "allocation failure lost the buffered report");
    char line[8192];
    int totals = 0;
    while (std::fgets(line, sizeof(line), report))
        if (std::strstr(line, "\"record\":\"total\"")) ++totals;
    std::fclose(report);
    require(totals == 1, "final total was unavailable before allocations recovered");
    reject_allocations = false;
    return 0;
}
} // namespace

void* operator new(std::size_t bytes) {
    if (reject_allocations.load(std::memory_order_relaxed)) throw std::bad_alloc{};
    if (auto* memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main(int argc, char** argv) {
    if (argc == 2) return run_child(argv[1]);
    require(argc == 1, "unexpected arguments");
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("quidra-counter-flush-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    const auto executable = std::filesystem::absolute(argv[0]).string();
    struct Scenario { const char* mode; bool step_flush; int steps; int total; };
    for (const auto scenario : {
             Scenario{"--buffered", false, 1, 5},
             Scenario{"--step", true, 1, 5},
             Scenario{"--capacity", false, 4096, 4098},
             Scenario{"--failure", false, 1, 5},
             Scenario{"--empty", false, 0, 0}}) {
        const auto report = root / (std::string(scenario.mode) + ".jsonl");
        const auto errors = root / (std::string(scenario.mode) + ".errors");
        environment("QUIDRA_COUNTERS", report.string().c_str());
        environment("QUIDRA_TEST_COUNTER_ERRORS", errors.string().c_str());
        environment("QUIDRA_COUNTERS_STEP", nullptr);
        environment("QUIDRA_COUNTERS_FLUSH", scenario.step_flush ? "step" : nullptr);
        const auto observed_status = child_status(executable, scenario.mode);
        const bool failure = std::string(scenario.mode) == "--failure" || scenario.steps == 0;
        if (observed_status != (failure ? 101 : 0)) {
            std::ifstream error_input(errors);
            const std::string text((std::istreambuf_iterator<char>(error_input)), {});
            std::fputs(text.c_str(), stderr);
            std::fprintf(stderr, "child mode %s: observed status %d, expected %d\n",
                         scenario.mode, observed_status, failure ? 101 : 0);
        }
        require(observed_status == (failure ? 101 : 0),
                "finalization changed the child's original result");
        std::ifstream input(report);
        require(input.good(), "allocation failure lost the buffered report");
        std::vector<std::string> rows;
        for (std::string row; std::getline(input, row);) rows.push_back(std::move(row));
        require(rows.size() == static_cast<std::size_t>(scenario.steps + 2),
                "missing or duplicate steps/total");
        for (int step = 0; step < scenario.steps; ++step) {
            const auto& row = rows[static_cast<std::size_t>(step)];
            require(row.find("\"record\":\"step\"") != std::string::npos &&
                        row.find("\"event\":\"mark\"") != std::string::npos &&
                        field(row, "step") == static_cast<std::uint64_t>(step),
                    "buffered step order changed");
            require(row.find(R"("label":"first \"quoted\"\u0009\\line\u000a")") !=
                        std::string::npos, "JSON label escaping changed");
            require(field(row, "core_dispatches") == (scenario.steps == 1 ? 3U : 1U),
                    "buffered counter delta changed");
        }
        const auto& tail = rows[static_cast<std::size_t>(scenario.steps)];
        const auto& total = rows.back();
        require(tail.find("\"event\":\"exit\"") != std::string::npos &&
                    field(tail, "core_dispatches") == (scenario.steps == 0 ? 0U : 2U),
                "final step was lost");
        require(total.find("\"record\":\"total\"") != std::string::npos &&
                    field(total, "core_dispatches") == static_cast<std::uint64_t>(scenario.total),
                "final total changed");
        for (const auto* row : {&tail, &total}) {
            if (scenario.steps == 0) {
                require(field(*row, "host_waits_sync") == 0 &&
                            field(*row, "package_refusals") == 0 &&
                            row->find("\"wait_sites\":{}") != std::string::npos &&
                            row->find("\"refusals\":{}") != std::string::npos,
                        "an empty report fabricated attribution");
                continue;
            }
            require(field(*row, "host_waits_sync") == 1 &&
                        field(*row, "package_refusals") == 1 &&
                        row->find("\"wait_sites\":{\"sync\":1}") != std::string::npos &&
                        row->find("\"refusals\":{\"review/failure:7\":1}") != std::string::npos,
                    "allocation failure lost attribution");
        }
        if (failure) {
            std::ifstream error_input(errors);
            const std::string text((std::istreambuf_iterator<char>(error_input)), {});
            require(text.find("forced counter failure") != std::string::npos,
                    "reporting replaced the original failure");
        }
    }
    std::puts("counter reports survive allocation failure: ok");
}
