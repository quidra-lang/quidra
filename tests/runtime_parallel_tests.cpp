#include "runtime_parallel.hpp"

#include "platform/environment.hpp"
#include "quidra/native_extension.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(_WIN32)
#include <xmmintrin.h>
#endif

// qcore_parallel_for contract tests: the chunk set depends only on
// (begin, end, grain); output bits do not depend on the thread count or the
// scheduling backend; nesting and task-style scopes run serially; failures
// and exceptions report the lowest failing chunk for every thread count; and
// bodies on helper threads see the caller's floating-point environment.

namespace {

namespace parallel = quidra::runtime_parallel;
using parallel::Backend;

int failures = 0;

void expect(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "runtime parallel test failed: %s\n", what.c_str());
        ++failures;
    }
}

std::string label(Backend backend, std::uint64_t threads) {
    return std::string(backend == Backend::Platform ? "platform" : "pool") +
        " threads=" + std::to_string(threads);
}

std::vector<std::uint64_t> thread_counts() {
    std::vector<std::uint64_t> counts{1, 2, 3, 4, 5, 7, 8, 13, 16};
    const auto hardware = std::thread::hardware_concurrency();
    if (hardware > 0 && std::find(counts.begin(), counts.end(), hardware) == counts.end())
        counts.push_back(std::min<std::uint64_t>(hardware, 256));
    return counts;
}

const Backend backends[] = {Backend::Platform, Backend::Pool};

void configure(Backend backend, std::uint64_t threads) {
    parallel::set_backend_for_testing(backend);
    parallel::set_thread_count_for_testing(threads);
}

// ---------------------------------------------------------------- arguments

void test_parse_thread_count() {
    std::uint64_t count = 0;
    expect(parallel::parse_thread_count("1", count) && count == 1, "parse 1");
    expect(parallel::parse_thread_count("8", count) && count == 8, "parse 8");
    expect(parallel::parse_thread_count("256", count) && count == 256, "parse 256");
    expect(parallel::parse_thread_count("007", count) && count == 7, "parse 007");
    for (const char* invalid : {"", "0", "257", "-1", "+2", " 4", "4 ", "4x", "x",
                                "1.5", "99999999999999999999999"}) {
        count = 12345;
        expect(!parallel::parse_thread_count(invalid, count) && count == 12345,
               std::string("reject \"") + invalid + "\"");
    }
    expect(!parallel::parse_thread_count(nullptr, count), "reject null");
}

int never_called(std::uint64_t, std::uint64_t, void* context) {
    static_cast<std::atomic<int>*>(context)->fetch_add(1);
    return 0;
}

void test_invalid_arguments() {
    std::atomic<int> calls{0};
    expect(qcore_parallel_for(0, 10, 1, nullptr, &calls) == QCORE_PARALLEL_INVALID_ARGUMENT,
           "null body rejected");
    expect(qcore_parallel_for(0, 10, 0, never_called, &calls) == QCORE_PARALLEL_INVALID_ARGUMENT,
           "zero grain rejected");
    expect(qcore_parallel_for(10, 9, 1, never_called, &calls) == QCORE_PARALLEL_INVALID_ARGUMENT,
           "begin > end rejected");
    expect(qcore_parallel_for(5, 5, 1, never_called, &calls) == QCORE_PARALLEL_OK,
           "empty range succeeds");
    expect(calls.load() == 0, "no body runs for rejected or empty ranges");
}

// ------------------------------------------------------------ chunk coverage

struct Coverage {
    std::uint64_t begin;
    std::uint64_t end;
    std::uint64_t grain;
    std::vector<std::atomic<int>>* visits;
    std::vector<std::atomic<int>>* chunk_calls;
    std::atomic<int> bad_bounds{0};
};

int record_coverage(std::uint64_t first, std::uint64_t last, void* raw) {
    auto& coverage = *static_cast<Coverage*>(raw);
    const auto offset = first - coverage.begin;
    const auto chunk = offset / coverage.grain;
    const auto expected_last = std::min(first + coverage.grain, coverage.end);
    if (offset % coverage.grain != 0 || last != expected_last || first >= last)
        coverage.bad_bounds.fetch_add(1);
    (*coverage.chunk_calls)[chunk].fetch_add(1);
    for (auto index = first; index < last; ++index)
        (*coverage.visits)[index - coverage.begin].fetch_add(1);
    return 0;
}

void test_chunk_coverage() {
    struct Range { std::uint64_t begin, end, grain; };
    const Range ranges[] = {
        {0, 1, 1}, {0, 1000, 1}, {5, 1005, 7}, {0, 100000, 4096},
        {3, 10, 100}, {1u << 20, (1u << 20) + 333, 333}, {0, 64, 64}, {0, 65, 64},
    };
    for (const auto backend : backends) {
        for (const auto threads : thread_counts()) {
            configure(backend, threads);
            for (const auto& range : ranges) {
                const auto length = range.end - range.begin;
                const auto chunks = (length + range.grain - 1) / range.grain;
                std::vector<std::atomic<int>> visits(length);
                std::vector<std::atomic<int>> chunk_calls(chunks);
                Coverage coverage{range.begin, range.end, range.grain, &visits, &chunk_calls};
                const int status = qcore_parallel_for(
                    range.begin, range.end, range.grain, record_coverage, &coverage);
                const auto what = label(backend, threads) + " range [" +
                    std::to_string(range.begin) + "," + std::to_string(range.end) +
                    ") grain " + std::to_string(range.grain);
                expect(status == QCORE_PARALLEL_OK, what + ": status");
                expect(coverage.bad_bounds.load() == 0, what + ": chunk bounds");
                expect(std::all_of(chunk_calls.begin(), chunk_calls.end(),
                                   [](const std::atomic<int>& c) { return c.load() == 1; }),
                       what + ": every chunk exactly once");
                expect(std::all_of(visits.begin(), visits.end(),
                                   [](const std::atomic<int>& v) { return v.load() == 1; }),
                       what + ": every index exactly once");
            }
        }
    }
}

// --------------------------------------------------------------- determinism

// An order-sensitive float32 kernel over independent outputs: each output is a
// long fused multiply-add chain plus transcendental terms, so any change in
// how an output is computed would show up in its bits.
struct Kernel {
    const float* input;
    const float* weights;
    float* output;
    std::uint64_t size;
    std::uint64_t taps;
};

float kernel_output(const Kernel& kernel, std::uint64_t index) {
    float accumulator = 0.0F;
    for (std::uint64_t tap = 0; tap < kernel.taps; ++tap) {
        const float value = kernel.input[(index * 31 + tap * 7) % kernel.size];
        accumulator = std::fma(value, kernel.weights[tap], accumulator);
    }
    return accumulator + std::exp(-std::fabs(accumulator)) * 0.5F +
        std::tanh(kernel.input[index]);
}

int run_kernel(std::uint64_t first, std::uint64_t last, void* raw) {
    const auto& kernel = *static_cast<const Kernel*>(raw);
    for (auto index = first; index < last; ++index)
        kernel.output[index] = kernel_output(kernel, index);
    return 0;
}

void test_bitwise_determinism() {
    const std::uint64_t size = 1u << 15;
    const std::uint64_t taps = 37;
    std::vector<float> input(size);
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    for (auto& value : input) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        const auto bits = static_cast<std::uint32_t>(state >> 32);
        value = static_cast<float>(static_cast<std::int32_t>(bits)) / 2147483648.0F;
    }
    // Edge values: signed zeros, infinities, NaN, subnormals.
    input[1] = -0.0F;
    input[2] = std::numeric_limits<float>::infinity();
    input[3] = -std::numeric_limits<float>::infinity();
    input[4] = std::numeric_limits<float>::quiet_NaN();
    input[5] = std::numeric_limits<float>::denorm_min();
    input[6] = -std::numeric_limits<float>::denorm_min();
    std::vector<float> weights(taps);
    for (std::uint64_t tap = 0; tap < taps; ++tap)
        weights[tap] = 1.0F / static_cast<float>(tap + 3) * (tap % 2 ? -1.0F : 1.0F);

    std::vector<float> reference(size);
    Kernel serial{input.data(), weights.data(), reference.data(), size, taps};
    for (std::uint64_t index = 0; index < size; ++index)
        reference[index] = kernel_output(serial, index);

    for (const auto backend : backends) {
        for (const auto threads : thread_counts()) {
            configure(backend, threads);
            for (const std::uint64_t grain : std::array<std::uint64_t, 5>{1, 63, 1000, 4096, size}) {
                std::vector<float> output(size, 12345.0F);
                Kernel kernel{input.data(), weights.data(), output.data(), size, taps};
                const int status = qcore_parallel_for(0, size, grain, run_kernel, &kernel);
                const auto what = label(backend, threads) + " grain " + std::to_string(grain);
                expect(status == QCORE_PARALLEL_OK, what + ": status");
                expect(std::memcmp(output.data(), reference.data(), size * sizeof(float)) == 0,
                       what + ": output bits equal the serial loop");
            }
        }
    }
}

// ----------------------------------------------------------- real parallelism

struct Spread {
    bool wait_for_second_thread = true;
    std::mutex mutex;
    std::set<std::thread::id> threads;
    std::atomic<int> active{0};
    std::atomic<int> max_active{0};
};

int record_spread(std::uint64_t first, std::uint64_t, void* raw) {
    auto& spread = *static_cast<Spread*>(raw);
    {
        std::lock_guard<std::mutex> lock(spread.mutex);
        spread.threads.insert(std::this_thread::get_id());
    }
    const int now = spread.active.fetch_add(1) + 1;
    int seen = spread.max_active.load();
    while (now > seen && !spread.max_active.compare_exchange_weak(seen, now)) {}
    if (first == 0 && spread.wait_for_second_thread) {
        // Hold the first chunk until another thread has taken work, so a
        // working thread pool is observable even on a loaded machine.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(spread.mutex);
                if (spread.threads.size() >= 2) break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    } else {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    spread.active.fetch_sub(1);
    return 0;
}

void test_threads_are_used_and_bounded() {
    if (std::thread::hardware_concurrency() < 2) return;
    for (const auto backend : backends) {
        for (const std::uint64_t threads : {2ull, 3ull}) {
            configure(backend, threads);
            Spread spread;
            const int status = qcore_parallel_for(0, 64, 1, record_spread, &spread);
            const auto what = label(backend, threads);
            expect(status == QCORE_PARALLEL_OK, what + ": status");
            expect(spread.threads.size() >= 2, what + ": work ran on more than one thread");
            expect(spread.threads.size() <= threads,
                   what + ": at most the configured number of threads run bodies");
            expect(spread.max_active.load() <= static_cast<int>(threads),
                   what + ": at most the configured number of bodies run at once");
        }
        configure(backend, 1);
        Spread serial;
        serial.wait_for_second_thread = false;
        expect(qcore_parallel_for(0, 64, 1, record_spread, &serial) == QCORE_PARALLEL_OK &&
                   serial.threads.size() == 1 &&
                   *serial.threads.begin() == std::this_thread::get_id() &&
                   serial.max_active.load() == 1,
               label(backend, 1) + ": one thread runs everything on the caller");
    }
}

// ---------------------------------------------------- nesting and task scopes

struct Order {
    std::thread::id owner;
    std::vector<std::uint64_t> chunks;
    bool wrong_thread = false;
    bool wrong_count = false;
};

int record_order(std::uint64_t first, std::uint64_t, void* raw) {
    auto& order = *static_cast<Order*>(raw);
    if (std::this_thread::get_id() != order.owner) order.wrong_thread = true;
    if (qcore_parallel_thread_count() != 1) order.wrong_count = true;
    order.chunks.push_back(first);
    return 0;
}

struct Outer {
    std::atomic<int> problems{0};
};

int run_nested(std::uint64_t first, std::uint64_t, void* raw) {
    auto& outer = *static_cast<Outer*>(raw);
    if (qcore_parallel_thread_count() != 1) outer.problems.fetch_add(1);
    Order order;
    order.owner = std::this_thread::get_id();
    const int status = qcore_parallel_for(first * 100, first * 100 + 50, 5, record_order, &order);
    bool ascending = order.chunks.size() == 10;
    for (std::size_t index = 0; ascending && index < order.chunks.size(); ++index)
        ascending = order.chunks[index] == first * 100 + index * 5;
    if (status != QCORE_PARALLEL_OK || order.wrong_thread || order.wrong_count || !ascending)
        outer.problems.fetch_add(1);
    return 0;
}

void test_nested_and_task_scopes_run_serially() {
    for (const auto backend : backends) {
        configure(backend, 4);
        expect(qcore_parallel_thread_count() == 4, label(backend, 4) + ": reported thread count");
        Outer outer;
        expect(qcore_parallel_for(0, 32, 1, run_nested, &outer) == QCORE_PARALLEL_OK &&
                   outer.problems.load() == 0,
               label(backend, 4) + ": nested calls run serially, in order, on the body's thread");

        // task.all operations hold a SerialScope for their whole lifetime.
        std::thread task([&] {
            const parallel::SerialScope task_scope;
            Order order;
            order.owner = std::this_thread::get_id();
            const bool reports_one = qcore_parallel_thread_count() == 1;
            const int status = qcore_parallel_for(0, 40, 4, record_order, &order);
            bool ascending = order.chunks.size() == 10;
            for (std::size_t index = 0; ascending && index < order.chunks.size(); ++index)
                ascending = order.chunks[index] == index * 4;
            expect(reports_one && status == QCORE_PARALLEL_OK && !order.wrong_thread &&
                       !order.wrong_count && ascending,
                   label(backend, 4) + ": a task scope runs serially on its own thread");
        });
        task.join();
        expect(qcore_parallel_thread_count() == 4,
               label(backend, 4) + ": scopes do not leak into other threads");
    }
    parallel::set_thread_count_for_testing(0);
    // QUIDRA_CPU_THREADS, reduced to the hardware thread count, or the
    // hardware thread count when it is unset or empty.
    const std::uint64_t hardware = std::min<std::uint64_t>(
        std::max(1u, std::thread::hardware_concurrency()), 256);
    std::uint64_t expected = hardware;
    const auto configured = quidra::platform::environment_value("QUIDRA_CPU_THREADS");
    if (configured && !configured->empty() &&
        parallel::parse_thread_count(configured->c_str(), expected))
        expected = std::min(expected, hardware);
    expect(qcore_parallel_thread_count() == expected,
           "default thread count follows QUIDRA_CPU_THREADS or the hardware");
}

// ---------------------------------------------------------------- body depth

struct DepthProbe {
    std::atomic<int> outside{0};      // bodies that did not see a body depth
    std::atomic<int> nested_lost{0};  // bodies whose depth a nested call broke
    bool nest = false;
};

int probe_depth(std::uint64_t first, std::uint64_t, void* raw) {
    auto& probe = *static_cast<DepthProbe*>(raw);
    if (!parallel::inside_parallel_body()) probe.outside.fetch_add(1);
    if (probe.nest) {
        DepthProbe inner;
        const int status = qcore_parallel_for(first * 10, first * 10 + 8, 2, probe_depth, &inner);
        if (status != QCORE_PARALLEL_OK || inner.outside.load() != 0 ||
            !parallel::inside_parallel_body())
            probe.nested_lost.fetch_add(1);
    }
    return 0;
}

int throw_from_body(std::uint64_t, std::uint64_t, void*) {
    throw std::runtime_error("body failed");
}

#ifdef __APPLE__
void count_workers_at_body_depth(void* raw, std::size_t) {
    if (parallel::inside_parallel_body()) static_cast<std::atomic<int>*>(raw)->fetch_add(1);
    std::this_thread::sleep_for(std::chrono::microseconds(100));
}
#endif

// The depth the guard against qcore_* calls from bodies uses: set in every
// body at every thread count, kept across nested calls, cleared when a body
// ends (also by an exception), and not set by a task.all scope alone.
void test_body_depth_marks_bodies_only() {
    expect(!parallel::inside_parallel_body(), "no body depth outside a call");
    const std::uint64_t depth_threads[] = {1, 2, 8};
    for (const auto backend : backends) {
        for (const auto threads : depth_threads) {
            configure(backend, threads);
            const auto what = label(backend, threads);
            DepthProbe probe;
            probe.nest = true;
            expect(qcore_parallel_for(0, 64, 1, probe_depth, &probe) == QCORE_PARALLEL_OK &&
                       probe.outside.load() == 0 && probe.nested_lost.load() == 0,
                   what + ": every body, nested or not, runs at body depth");
            expect(qcore_parallel_for(0, 64, 1, throw_from_body, nullptr) ==
                           QCORE_PARALLEL_CALLBACK_EXCEPTION &&
                       !parallel::inside_parallel_body(),
                   what + ": body depth ends with the body, also after an exception");
        }
        configure(backend, 4);
        std::thread task([&] {
            const parallel::SerialScope task_scope;
            const bool outside_before = !parallel::inside_parallel_body();
            DepthProbe probe;
            const int status = qcore_parallel_for(0, 8, 1, probe_depth, &probe);
            expect(outside_before && status == QCORE_PARALLEL_OK && probe.outside.load() == 0 &&
                       !parallel::inside_parallel_body(),
                   label(backend, 4) + ": a task scope is not a body; the bodies it runs are");
        });
        task.join();
    }
#ifdef __APPLE__
    // GCD workers that ran (and threw from) bodies above are back at depth 0.
    std::atomic<int> workers_at_depth{0};
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-extension"
    dispatch_queue_t const queue = DISPATCH_APPLY_AUTO;
#pragma clang diagnostic pop
    dispatch_apply_f(256, queue, &workers_at_depth, count_workers_at_body_depth);
    expect(workers_at_depth.load() == 0, "GCD workers leave body depth behind them");
#endif
}

// ------------------------------------------------- Core calls inside bodies

// Pure data-access calls are allowed in bodies at every thread count, nested
// or not. Called with no tensor they return their documented "no tensor"
// values, so no Core state is needed.
struct DataAccess {
    std::atomic<int> wrong{0};
    bool nest = false;
};

int call_data_access(std::uint64_t first, std::uint64_t, void* raw) {
    auto& access = *static_cast<DataAccess*>(raw);
    const bool expected =
        qcore_native_abi_version() == QUIDRA_NATIVE_ABI_VERSION &&
        qcore_parallel_thread_count() == 1 &&
        (qcore_execution_policy_get() == QCORE_EXECUTION_FAST ||
         qcore_execution_policy_get() == QCORE_EXECUTION_DETERMINISTIC) &&
        qcore_execution_is_deterministic() ==
            (qcore_execution_policy_get() == QCORE_EXECUTION_DETERMINISTIC ? 1 : 0) &&
        qcore_tensor_dtype(nullptr) == 0 && qcore_tensor_device(nullptr) == -2 &&
        qcore_tensor_rank(nullptr) == 0 && qcore_tensor_extent(nullptr, 0) == -1 &&
        qcore_tensor_element_count(nullptr) == 0 &&
        qcore_tensor_is_contiguous(nullptr) == 0 && qcore_tensor_backend(nullptr) == -1 &&
        qcore_tensor_backend_device_index(nullptr) == -1 &&
        qcore_tensor_device_offset_bytes(nullptr) == 0;
    if (!expected) access.wrong.fetch_add(1);
    if (access.nest) {
        DataAccess inner;
        if (qcore_parallel_for(first * 4, first * 4 + 4, 1, call_data_access, &inner) !=
                QCORE_PARALLEL_OK ||
            inner.wrong.load() != 0)
            access.wrong.fetch_add(1);
    }
    return 0;
}

void test_data_access_calls_inside_bodies() {
    // Outside a body the guard lets every call through.
    parallel::reject_inside_parallel_body("qcore_tensor_cpu_data");
    expect(qcore_tensor_cpu_data(nullptr) == nullptr &&
               qcore_tensor_device_handle(nullptr) == 0,
           "guarded calls work outside a body");
    for (const auto backend : backends) {
        for (const std::uint64_t threads : {1, 4}) {
            configure(backend, threads);
            DataAccess access;
            access.nest = true;
            expect(qcore_parallel_for(0, 32, 1, call_data_access, &access) == QCORE_PARALLEL_OK &&
                       access.wrong.load() == 0,
                   label(backend, threads) + ": data-access calls inside bodies");
        }
    }
}

// A call that is not pure data access is rejected in a child process: it
// ends the process with runtime error PARALLEL_BODY, at every thread count
// and also from a nested body.
int call_cpu_data(std::uint64_t, std::uint64_t, void*) {
    return qcore_tensor_cpu_data(nullptr) == nullptr ? 0 : 1;
}

int call_cpu_data_nested(std::uint64_t first, std::uint64_t, void*) {
    return qcore_parallel_for(first, first + 2, 1, call_cpu_data, nullptr);
}

int call_counters_mark(std::uint64_t, std::uint64_t, void*) {
    qcore_counters_mark("inside a body");
    return 0;
}

// Child side: never returns normally when the guard works.
int run_rejected_call(const std::string& body, const std::string& backend,
                      const std::string& threads) {
    configure(backend == "pool" ? Backend::Pool : Backend::Platform,
              static_cast<std::uint64_t>(std::stoull(threads)));
    qcore_parallel_body_fn function = call_cpu_data;
    if (body == "nested") function = call_cpu_data_nested;
    if (body == "counters") function = call_counters_mark;
    std::printf("before the call\n");
    const int status = qcore_parallel_for(0, 16, 1, function, nullptr);
    std::printf("returned %d\n", status);
    return 0;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void test_other_calls_inside_bodies_are_rejected(const char* self) {
    struct Case { const char* body; const char* function; };
    const Case cases[] = {
        {"cpu_data", "qcore_tensor_cpu_data"},
        {"nested", "qcore_tensor_cpu_data"},
        {"counters", "qcore_counters_mark"},
    };
    const auto directory = std::filesystem::temp_directory_path();
    const auto stem = "quidra-parallel-body-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto out = directory / (stem + ".out");
    const auto err = directory / (stem + ".err");
    for (const auto& item : cases) {
        for (const char* backend : {"platform", "pool"}) {
            for (const char* threads : {"1", "4"}) {
                const std::string what = std::string(item.body) + " " + backend +
                    " threads=" + threads;
                const std::string command = "\"" + std::string(self) +
                    "\" --reject-inside-body " + item.body + " " + backend + " " + threads +
                    " >\"" + out.string() + "\" 2>\"" + err.string() + "\"";
#ifdef _WIN32
                // cmd.exe /c drops the outer quotes of a command that starts
                // with one; quote the whole line so the inner quotes survive.
                int status = std::system(("\"" + command + "\"").c_str());
#else
                int status = std::system(command.c_str());
#endif
#ifndef _WIN32
                status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
                const auto output = read_file(out);
                const auto error = read_file(err);
                const std::string message = std::string("Quidra runtime error[PARALLEL_BODY]: ") +
                    item.function + " was called inside a qcore_parallel_for body";
                expect(status == 101, what + ": exit status " + std::to_string(status));
                expect(error.find(message) != std::string::npos,
                       what + ": message, got \"" + error + "\"");
                // Redirected Windows text streams use CRLF rather than LF.
                expect(output == "before the call\n" || output == "before the call\r\n",
                       what + ": output flushed before the error and nothing after it, got \"" +
                           output + "\"");
            }
        }
    }
    std::error_code ignored;
    std::filesystem::remove(out, ignored);
    std::filesystem::remove(err, ignored);
}

// ------------------------------------------------------- failure reporting

// Per-chunk outcomes other than these two are returned by the body as they are.
constexpr int throws_std = std::numeric_limits<int>::min();
constexpr int throws_other = std::numeric_limits<int>::min() + 1;

struct Failing {
    std::uint64_t grain;
    std::vector<int> outcome;
    std::vector<std::atomic<int>>* ran;
};

int run_failing(std::uint64_t first, std::uint64_t, void* raw) {
    auto& failing = *static_cast<Failing*>(raw);
    const auto chunk = first / failing.grain;
    (*failing.ran)[chunk].fetch_add(1);
    std::this_thread::sleep_for(std::chrono::microseconds(chunk % 3 == 0 ? 50 : 0));
    const int outcome = failing.outcome[chunk];
    if (outcome == throws_std) throw std::runtime_error("chunk failed");
    if (outcome == throws_other) throw 42;
    return outcome;
}

void test_failures_report_the_lowest_chunk() {
    struct Case {
        const char* name;
        std::vector<std::pair<std::uint64_t, int>> outcomes;
        int expected;
        std::uint64_t lowest;
    };
    const Case cases[] = {
        {"statuses", {{37, 370}, {12, 120}, {50, 500}}, 120, 12},
        {"status before exception", {{5, 9}, {20, throws_std}}, 9, 5},
        {"exception before status", {{20, throws_std}, {30, 7}}, QCORE_PARALLEL_CALLBACK_EXCEPTION, 20},
        {"non-standard exception", {{0, throws_other}, {63, 3}}, QCORE_PARALLEL_CALLBACK_EXCEPTION, 0},
        {"last chunk", {{63, 11}}, 11, 63},
        // Negative body results never pass for Core's own statuses.
        {"body returns -1", {{9, QCORE_PARALLEL_INVALID_ARGUMENT}, {40, 4}},
         QCORE_PARALLEL_INVALID_BODY_RESULT, 9},
        {"body returns -2", {{0, QCORE_PARALLEL_CALLBACK_EXCEPTION}}, QCORE_PARALLEL_INVALID_BODY_RESULT, 0},
        {"body returns -3", {{17, QCORE_PARALLEL_INVALID_BODY_RESULT}}, QCORE_PARALLEL_INVALID_BODY_RESULT, 17},
        {"body returns -7", {{31, -7}, {32, throws_std}}, QCORE_PARALLEL_INVALID_BODY_RESULT, 31},
        {"most negative return", {{63, std::numeric_limits<int>::min() + 2}},
         QCORE_PARALLEL_INVALID_BODY_RESULT, 63},
        {"status before negative", {{3, 8}, {5, -7}}, 8, 3},
        {"exception before negative", {{2, throws_other}, {3, -1}}, QCORE_PARALLEL_CALLBACK_EXCEPTION, 2},
    };
    const std::uint64_t grain = 3;
    const std::uint64_t chunks = 64;
    for (const auto backend : backends) {
        for (const auto threads : thread_counts()) {
            configure(backend, threads);
            for (const auto& test : cases) {
                std::vector<std::atomic<int>> ran(chunks);
                Failing failing{grain, std::vector<int>(chunks, 0), &ran};
                for (const auto& [chunk, outcome] : test.outcomes) failing.outcome[chunk] = outcome;
                const int status = qcore_parallel_for(0, chunks * grain - 1, grain, run_failing, &failing);
                const auto what = label(backend, threads) + " " + test.name;
                expect(status == test.expected,
                       what + ": returned " + std::to_string(status) + ", expected " +
                           std::to_string(test.expected));
                bool lower_ran = true;
                for (std::uint64_t chunk = 0; chunk <= test.lowest; ++chunk)
                    lower_ran = lower_ran && ran[chunk].load() == 1;
                expect(lower_ran, what + ": every chunk up to the failure ran once");
                bool at_most_once = true;
                for (const auto& count : ran) at_most_once = at_most_once && count.load() <= 1;
                expect(at_most_once, what + ": no chunk ran twice");
                if (threads == 1) {
                    bool later_skipped = true;
                    for (auto chunk = test.lowest + 1; chunk < chunks; ++chunk)
                        later_skipped = later_skipped && ran[chunk].load() == 0;
                    expect(later_skipped, what + ": serial execution stops at the failure");
                }
            }
        }
    }
}

// ------------------------------------------------ floating-point environment

// Makes helpers take work: a chunk on the calling thread waits, with
// integer-only code so it raises no floating-point flag, until a chunk has run
// on another thread. Disabled for the single-thread references.
struct HelperGate {
    std::thread::id caller = std::this_thread::get_id();
    bool wait = false;
    std::atomic<int> helper_chunks{0};

    // Returns true on a helper thread.
    bool enter() {
        if (std::this_thread::get_id() != caller) {
            helper_chunks.fetch_add(1);
            return true;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (wait && helper_chunks.load() == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        return false;
    }
};

// Every step rounds, so the rounding mode changes almost every output.
struct Division {
    const float* numerators;
    const float* denominators;
    float* output;
    HelperGate* gate;
};

int run_division(std::uint64_t first, std::uint64_t last, void* raw) {
    const auto& division = *static_cast<const Division*>(raw);
    division.gate->enter();
    for (auto index = first; index < last; ++index) {
        float value = division.numerators[index] / division.denominators[index];
        for (int step = 0; step < 64; ++step) value = value / 1.0001F + 0.1F;
        division.output[index] = value;
    }
    return 0;
}

#ifdef __APPLE__
void count_non_default_rounding(void* raw, std::size_t) {
    if (std::fegetround() != FE_TONEAREST)
        static_cast<std::atomic<int>*>(raw)->fetch_add(1);
    std::this_thread::sleep_for(std::chrono::microseconds(100));
}
#endif

void test_helpers_use_the_callers_rounding_mode() {
    const std::uint64_t size = 1u << 14;
    std::vector<float> numerators(size), denominators(size);
    for (std::uint64_t index = 0; index < size; ++index) {
        numerators[index] = 1.0F + static_cast<float>(index);
        denominators[index] = 3.0F + static_cast<float>(index % 97);
    }
    const auto compute = [&](std::vector<float>& output, bool wait_for_helpers) {
        HelperGate gate;
        gate.wait = wait_for_helpers;
        Division division{numerators.data(), denominators.data(), output.data(), &gate};
        const int status = qcore_parallel_for(0, size, 64, run_division, &division);
        return status == QCORE_PARALLEL_OK && (!wait_for_helpers || gate.helper_chunks.load() > 0);
    };
    // References: one thread runs every chunk on the caller itself.
    std::vector<float> nearest(size), upward(size);
    configure(Backend::Platform, 1);
    compute(nearest, false);
    std::fesetround(FE_UPWARD);
    compute(upward, false);
    std::fesetround(FE_TONEAREST);
    expect(std::memcmp(nearest.data(), upward.data(), size * sizeof(float)) != 0,
           "the rounding mode changes the division kernel");

    for (const auto backend : backends) {
        for (const std::uint64_t threads : {2ull, 4ull, 8ull, 16ull}) {
            configure(backend, threads);
            std::vector<float> output(size, 12345.0F);
            std::fesetround(FE_UPWARD);
            const bool helped = compute(output, true);
            const int mode_after = std::fegetround();
            std::fesetround(FE_TONEAREST);
            const auto what = label(backend, threads);
            expect(helped, what + ": helper threads ran chunks");
            expect(std::memcmp(output.data(), upward.data(), size * sizeof(float)) == 0,
                   what + ": helper threads compute under the caller's rounding mode");
            expect(mode_after == FE_UPWARD, what + ": the caller keeps its rounding mode");
            std::fill(output.begin(), output.end(), 12345.0F);
            expect(compute(output, true) &&
                       std::memcmp(output.data(), nearest.data(), size * sizeof(float)) == 0,
                   what + ": a later call under the default mode is unaffected");
        }
    }
#ifdef __APPLE__
    // GCD worker threads are shared with the rest of the process, so each
    // must get its own environment back once its chunks are done.
    std::atomic<int> changed{0};
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-extension"
    dispatch_queue_t const queue = DISPATCH_APPLY_AUTO;
#pragma clang diagnostic pop
    dispatch_apply_f(256, queue, &changed, count_non_default_rounding);
    expect(changed.load() == 0, "GCD worker threads get their own rounding mode back");
#endif
}

// Raises FE_OVERFLOW only on helper threads, or no flag at all.
struct Flags {
    bool overflow_on_helpers;
    HelperGate gate;
};

int raise_flags_on_helpers(std::uint64_t, std::uint64_t, void* raw) {
    auto& flags = *static_cast<Flags*>(raw);
    if (flags.gate.enter() && flags.overflow_on_helpers) {
        volatile float huge = std::numeric_limits<float>::max();
        volatile float product = huge * huge;
        (void)product;
    }
    return 0;
}

void test_helper_exception_flags_reach_the_caller() {
    if (std::thread::hardware_concurrency() < 2) return;
    for (const auto backend : backends) {
        for (const std::uint64_t threads : {2ull, 8ull}) {
            configure(backend, threads);
            const auto what = label(backend, threads);
            std::feclearexcept(FE_ALL_EXCEPT);
            Flags overflowing;
            overflowing.overflow_on_helpers = true;
            overflowing.gate.wait = true;
            const int status = qcore_parallel_for(0, 64, 1, raise_flags_on_helpers, &overflowing);
            const int raised = std::fetestexcept(FE_ALL_EXCEPT);
            std::feclearexcept(FE_ALL_EXCEPT);
            expect(status == QCORE_PARALLEL_OK && overflowing.gate.helper_chunks.load() > 0,
                   what + ": helpers ran overflowing chunks");
            expect((raised & FE_OVERFLOW) != 0,
                   what + ": an exception flag raised on a helper is raised on the caller");

            // The caller's own flags survive, and helpers add none of their own.
            std::feraiseexcept(FE_INEXACT);
            Flags quiet;
            quiet.overflow_on_helpers = false;
            quiet.gate.wait = true;
            const int quiet_status = qcore_parallel_for(0, 64, 1, raise_flags_on_helpers, &quiet);
            const int kept = std::fetestexcept(FE_ALL_EXCEPT);
            std::feclearexcept(FE_ALL_EXCEPT);
            expect(quiet_status == QCORE_PARALLEL_OK && quiet.gate.helper_chunks.load() > 0 &&
                       kept == FE_INEXACT,
                   what + ": the caller's flags are kept and no other flag appears");
        }
    }
}

// Flush-to-zero is part of fenv_t on Apple arm64 (FPCR.FZ) and in MXCSR on
// x86-64; the test probes that the platform's fenv_t really carries it.
#if defined(__APPLE__) && defined(__aarch64__)
#define QUIDRA_TEST_FLUSH_TO_ZERO 1
void set_flush_to_zero(bool enabled) {
    std::fenv_t environment;
    std::fegetenv(&environment);
    if (enabled) {
        environment.__fpcr |= __fpcr_flush_to_zero;
    } else {
        environment.__fpcr &= ~static_cast<unsigned long long>(__fpcr_flush_to_zero);
    }
    std::fesetenv(&environment);
}
bool flush_to_zero_enabled() {
    std::fenv_t environment;
    std::fegetenv(&environment);
    return (environment.__fpcr & __fpcr_flush_to_zero) != 0;
}
#elif (defined(__x86_64__) || defined(_M_X64)) && !defined(_WIN32)
#define QUIDRA_TEST_FLUSH_TO_ZERO 1
constexpr unsigned flush_bits = 0x8040U;  // FTZ and DAZ
void set_flush_to_zero(bool enabled) {
    _mm_setcsr(enabled ? (_mm_getcsr() | flush_bits) : (_mm_getcsr() & ~flush_bits));
}
bool flush_to_zero_enabled() { return (_mm_getcsr() & flush_bits) == flush_bits; }
#endif

#ifdef QUIDRA_TEST_FLUSH_TO_ZERO
struct Scale {
    const float* input;
    float* output;
    float factor;
    HelperGate* gate;
};

int run_scale(std::uint64_t first, std::uint64_t last, void* raw) {
    const auto& scale = *static_cast<const Scale*>(raw);
    scale.gate->enter();
    for (auto index = first; index < last; ++index)
        scale.output[index] = scale.input[index] * scale.factor;
    return 0;
}

void test_helpers_use_the_callers_flush_to_zero() {
    // The probe: does an environment saved with flush-to-zero restore it?
    set_flush_to_zero(true);
    std::fenv_t saved;
    std::fegetenv(&saved);
    set_flush_to_zero(false);
    std::fesetenv(&saved);
    const bool carried = flush_to_zero_enabled();
    set_flush_to_zero(false);
    if (!carried) {
        std::printf("note: fenv_t does not carry flush-to-zero here; test skipped\n");
        return;
    }
    const std::uint64_t size = 1u << 12;
    std::vector<float> input(size);
    for (std::uint64_t index = 0; index < size; ++index)
        input[index] = 1.0e-20F * static_cast<float>(1 + index % 7);
    const auto compute = [&](std::vector<float>& output, bool wait_for_helpers) {
        HelperGate gate;
        gate.wait = wait_for_helpers;
        Scale scale{input.data(), output.data(), 1.0e-20F, &gate};
        const int status = qcore_parallel_for(0, size, 16, run_scale, &scale);
        return status == QCORE_PARALLEL_OK && (!wait_for_helpers || gate.helper_chunks.load() > 0);
    };
    std::vector<float> gradual(size), flushed(size);
    configure(Backend::Platform, 1);
    compute(gradual, false);
    set_flush_to_zero(true);
    compute(flushed, false);
    set_flush_to_zero(false);
    expect(std::memcmp(gradual.data(), flushed.data(), size * sizeof(float)) != 0,
           "flush-to-zero changes the subnormal kernel");
    for (const auto backend : backends) {
        for (const std::uint64_t threads : {2ull, 8ull}) {
            configure(backend, threads);
            std::vector<float> output(size, 12345.0F);
            set_flush_to_zero(true);
            const bool helped = compute(output, true);
            set_flush_to_zero(false);
            expect(helped && std::memcmp(output.data(), flushed.data(), size * sizeof(float)) == 0,
                   label(backend, threads) + ": helper threads flush to zero like the caller");
        }
    }
}
#endif

// --------------------------------------------------- concurrent top-level use

void test_concurrent_callers() {
    const std::uint64_t size = 1u << 12;
    std::vector<float> input(size);
    for (std::uint64_t index = 0; index < size; ++index)
        input[index] = std::sin(static_cast<float>(index) * 0.37F);
    std::vector<float> weights(9, 0.125F);
    std::vector<float> reference(size);
    Kernel serial{input.data(), weights.data(), reference.data(), size, 9};
    for (std::uint64_t index = 0; index < size; ++index)
        reference[index] = kernel_output(serial, index);
    for (const auto backend : backends) {
        configure(backend, 4);
        std::atomic<int> mismatches{0};
        std::vector<std::thread> callers;
        for (int caller = 0; caller < 3; ++caller) {
            callers.emplace_back([&] {
                for (int round = 0; round < 20; ++round) {
                    std::vector<float> output(size);
                    Kernel kernel{input.data(), weights.data(), output.data(), size, 9};
                    if (qcore_parallel_for(0, size, 64, run_kernel, &kernel) != QCORE_PARALLEL_OK ||
                        std::memcmp(output.data(), reference.data(), size * sizeof(float)) != 0)
                        mismatches.fetch_add(1);
                }
            });
        }
        for (auto& caller : callers) caller.join();
        expect(mismatches.load() == 0, label(backend, 4) + ": concurrent callers get exact results");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--reject-inside-body")
        return run_rejected_call(argv[2], argv[3], argv[4]);
    test_parse_thread_count();
    test_invalid_arguments();
    test_chunk_coverage();
    test_bitwise_determinism();
    test_threads_are_used_and_bounded();
    test_nested_and_task_scopes_run_serially();
    test_body_depth_marks_bodies_only();
    test_data_access_calls_inside_bodies();
    test_other_calls_inside_bodies_are_rejected(argv[0]);
    test_failures_report_the_lowest_chunk();
    test_helpers_use_the_callers_rounding_mode();
    test_helper_exception_flags_reach_the_caller();
#ifdef QUIDRA_TEST_FLUSH_TO_ZERO
    test_helpers_use_the_callers_flush_to_zero();
#endif
    test_concurrent_callers();
    if (failures != 0) {
        std::fprintf(stderr, "%d runtime parallel test(s) failed\n", failures);
        return 1;
    }
    std::printf("runtime parallel tests passed\n");
    return 0;
}
