// Deterministic CPU parallelism for package-native kernels (qcore_parallel_for).
//
// The partition of [begin, end) into chunks is a pure function of begin, end
// and grain. Threads only decide which chunk runs where and when, so a body
// that writes only the outputs of its own range produces the same bits for
// every thread count. Failure reporting follows the same rule: the result is
// that of the failing chunk with the lowest index, which every schedule runs.
//
// Apple platforms schedule through Grand Central Dispatch (dispatch_apply_f).
// Other platforms use one persistent pool whose threads are created on first
// use and never torn down, so a body that ends the process (for example a
// runtime error inside package code) cannot deadlock a destructor join.
//
// Bodies on threads other than the caller run under the caller's
// floating-point environment (rounding mode, flush-to-zero and denormal
// controls where fenv_t carries them), and the exception flags they raise are
// raised on the caller afterwards, so the environment a body observes and
// leaves behind is the one a serial loop on the caller would have.
//
// task.all workers in runtime.cpp hold a SerialScope. That reference also pulls
// this object out of the static runtime archive whenever runtime.o is linked,
// which matters on GNU ld: the generated link line names the archive before the
// package objects that call qcore_parallel_for.

#include "runtime_parallel.hpp"
#include "runtime_counters.hpp"

#include "platform/environment.hpp"
#include "quidra/abi/process_status.hpp"
#include "quidra/native_extension.h"

#include <algorithm>
#include <atomic>
#include <cfenv>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif

namespace quidra::runtime_parallel {
namespace {

constexpr std::uint64_t max_thread_count = 256;
constexpr std::uint64_t no_failure = std::numeric_limits<std::uint64_t>::max();

// Depth of SerialScope objects alive on this thread.
thread_local std::uint32_t serial_depth = 0;

// Depth of qcore_parallel_for bodies running on this thread. Unlike
// serial_depth it ignores task.all scopes.
thread_local std::uint32_t body_depth = 0;

std::atomic<std::uint64_t> thread_count_for_testing{0};
std::atomic<Backend> backend_for_testing{Backend::Platform};

std::uint64_t hardware_thread_count() {
    const auto hardware =
        static_cast<std::uint64_t>(std::thread::hardware_concurrency());
    if (hardware == 0) return 1;
    return std::min(hardware, max_thread_count);
}

// QUIDRA_CPU_THREADS, read and validated once per process. A count above the
// hardware thread count is reduced to it, so the value reported by
// qcore_parallel_thread_count() is one both backends can actually use.
std::uint64_t environment_thread_count() {
    static const std::uint64_t configured = [] {
        const auto hardware = hardware_thread_count();
        const auto text = platform::environment_value("QUIDRA_CPU_THREADS");
        if (!text || text->empty()) return hardware;
        std::uint64_t count = 0;
        if (!parse_thread_count(text->c_str(), count)) {
            const auto message = "QUIDRA_CPU_THREADS must be an integer from 1 to " +
                                 std::to_string(max_thread_count) + ", got \"" + *text + "\"";
            counters::report_unlocated_failure(abi::FailureReason::invalid_thread_count,
                                               message, false);
        }
        return std::min(count, hardware);
    }();
    return configured;
}

// Always validates QUIDRA_CPU_THREADS first, so a bad value is reported by the
// first call wherever it is made (top level, nested, or inside task.all), and
// never depends on whether that call would have run serially anyway.
std::uint64_t configured_thread_count() {
    const auto environment = environment_thread_count();
    const auto forced = thread_count_for_testing.load(std::memory_order_relaxed);
    return forced != 0 ? forced : environment;
}

struct Job {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uint64_t grain = 0;
    std::uint64_t chunks = 0;
    qcore_parallel_body_fn body = nullptr;
    void* context = nullptr;
    std::atomic<std::uint64_t> next{0};
    std::atomic<std::uint64_t> failed_chunk{no_failure};
    std::mutex failure_mutex;
    int failure_status = QCORE_PARALLEL_OK;
    // Set only on the parallel path, by the calling thread.
    std::thread::id caller;
    std::fenv_t caller_environment{};
    std::atomic<int> helper_exceptions{0};
};

// Negative results belong to Core. A body that returns one gets a status of
// its own, so it can never pass for an argument error or an escaped exception.
int run_chunk(const Job& job, std::uint64_t chunk) noexcept {
    const auto first = job.begin + chunk * job.grain;
    const auto last = job.end - first > job.grain ? first + job.grain : job.end;
    struct BodyDepth {
        BodyDepth() noexcept { ++body_depth; }
        ~BodyDepth() { --body_depth; }
    } const depth;
    int status = QCORE_PARALLEL_OK;
    try {
        status = job.body(first, last, job.context);
    } catch (...) {
        return QCORE_PARALLEL_CALLBACK_EXCEPTION;
    }
    return status < 0 ? QCORE_PARALLEL_INVALID_BODY_RESULT : status;
}

void record_failure(Job& job, std::uint64_t chunk, int status) {
    std::lock_guard<std::mutex> lock(job.failure_mutex);
    if (chunk < job.failed_chunk.load(std::memory_order_relaxed)) {
        job.failure_status = status;
        job.failed_chunk.store(chunk, std::memory_order_release);
    }
}

// Runs chunks until none is left. Chunks are claimed in ascending order, so
// once a claimed chunk lies above a known failure every later claim does too.
// A chunk below the lowest failure is never skipped.
void drain(Job& job) noexcept {
    const SerialScope scope;
    while (true) {
        const auto chunk = job.next.fetch_add(1, std::memory_order_relaxed);
        if (chunk >= job.chunks) return;
        if (chunk > job.failed_chunk.load(std::memory_order_acquire)) return;
        const int status = run_chunk(job, chunk);
        if (status != QCORE_PARALLEL_OK) record_failure(job, chunk, status);
    }
}

// Runs drain() on a thread other than the caller. The bodies see the caller's
// floating-point environment instead of this thread's, and the exception flags
// they raise are collected for the caller; this thread's own environment,
// which GCD may share with unrelated work, is restored afterwards.
void drain_as_helper(Job& job) noexcept {
    std::fenv_t own;
    if (std::fegetenv(&own) != 0) {
        // Not expected on any supported platform; leave this thread untouched.
        drain(job);
        return;
    }
    std::fesetenv(&job.caller_environment);
    std::feclearexcept(FE_ALL_EXCEPT);
    drain(job);
    const int raised = std::fetestexcept(FE_ALL_EXCEPT);
    std::fesetenv(&own);
    if (raised != 0) job.helper_exceptions.fetch_or(raised, std::memory_order_relaxed);
}

int run_serial(const Job& job) noexcept {
    const SerialScope scope;
    for (std::uint64_t chunk = 0; chunk < job.chunks; ++chunk) {
        const int status = run_chunk(job, chunk);
        if (status != QCORE_PARALLEL_OK) return status;
    }
    return QCORE_PARALLEL_OK;
}

// Portable persistent pool. One job runs at a time; a second top-level caller
// that finds the pool busy runs its job serially, which yields the same bits.
// The caller drains chunks itself and waits only for helpers that actually
// joined the job, never for helpers that were still asleep when it ran out of
// work, so a loaded machine does not turn every call into a wake-up barrier.
class Pool {
public:
    // Up to helpers_wanted threads join the calling thread. Returns false,
    // without running anything, when the pool is busy or has no thread.
    bool run(Job& job, std::uint64_t helpers_wanted) {
        std::unique_lock<std::mutex> submit(submit_, std::try_to_lock);
        if (!submit.owns_lock()) return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            grow(helpers_wanted);
            const auto helpers = std::min(helpers_wanted, threads_);
            if (helpers == 0) return false;
            job_ = &job;
            helpers_ = helpers;
            joined_ = 0;
            open_ = true;
            ++generation_;
        }
        wake_.notify_all();
        drain(job);
        std::unique_lock<std::mutex> lock(mutex_);
        open_ = false;
        done_.wait(lock, [this] { return joined_ == 0; });
        job_ = nullptr;
        return true;
    }

private:
    // Requires mutex_. Thread creation failure leaves fewer helpers.
    void grow(std::uint64_t wanted) {
        while (threads_ < wanted) {
            try {
                std::thread(&Pool::worker, this, threads_, generation_).detach();
            } catch (...) {
                return;
            }
            ++threads_;
        }
    }

    void worker(std::uint64_t id, std::uint64_t seen) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            wake_.wait(lock, [&] { return generation_ != seen; });
            seen = generation_;
            // A helper joins only while the caller is still draining; the
            // caller then waits for it before the job leaves its stack.
            if (!open_ || id >= helpers_) continue;
            ++joined_;
            Job* job = job_;
            lock.unlock();
            drain_as_helper(*job);
            lock.lock();
            if (--joined_ == 0) done_.notify_one();
        }
    }

    std::mutex submit_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    std::uint64_t threads_ = 0;
    std::uint64_t generation_ = 0;
    std::uint64_t helpers_ = 0;
    std::uint64_t joined_ = 0;
    bool open_ = false;
    Job* job_ = nullptr;
};

Pool& pool() {
    // Intentionally leaked: detached helpers keep using it until process exit.
    static Pool* instance = new Pool();
    return *instance;
}

#ifdef __APPLE__
// dispatch_apply_f runs some iterations on the calling thread itself.
void dispatch_worker(void* context, std::size_t) {
    auto& job = *static_cast<Job*>(context);
    if (std::this_thread::get_id() == job.caller) {
        drain(job);
    } else {
        drain_as_helper(job);
    }
}
#endif

// Runs job on `participants` threads including the caller. Returns false when
// nothing ran and the caller must fall back to the serial path.
bool run_parallel(Job& job, std::uint64_t participants) {
    job.caller = std::this_thread::get_id();
    if (std::fegetenv(&job.caller_environment) != 0) return false;
#ifdef __APPLE__
    if (backend_for_testing.load(std::memory_order_relaxed) == Backend::Platform) {
        // Each iteration drains chunks, so at most `participants` threads
        // execute bodies even though GCD sizes its own worker set.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-extension"
        dispatch_queue_t const queue = DISPATCH_APPLY_AUTO;
#pragma clang diagnostic pop
        dispatch_apply_f(
            static_cast<std::size_t>(participants), queue, &job, dispatch_worker);
        return true;
    }
#endif
    return pool().run(job, participants - 1);
}

} // namespace

SerialScope::SerialScope() noexcept { ++serial_depth; }
SerialScope::~SerialScope() { --serial_depth; }

bool inside_parallel_body() noexcept { return body_depth != 0; }

void reject_inside_parallel_body(const char* function) noexcept {
    if (body_depth == 0) return;
    // Bodies on several threads may break the rule at once: the reporter
    // prints the first report and the others end without printing. The
    // process ends at once (_Exit), as for deferred GPU errors: other bodies
    // may still be running, so static destructors and atexit handlers must
    // not run under them.
    char message[256];
    std::snprintf(message, sizeof message,
                  "%s was called inside a qcore_parallel_for body; a body may call only the "
                  "pure data-access functions listed in quidra/native_extension.h",
                  function ? function : "a Core function");
    counters::report_unlocated_failure(abi::FailureReason::core_call_in_parallel_body, message,
                                       true);
}

bool parse_thread_count(const char* text, std::uint64_t& count) noexcept {
    if (!text || !*text) return false;
    std::uint64_t value = 0;
    for (const char* cursor = text; *cursor; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return false;
        value = value * 10 + static_cast<std::uint64_t>(*cursor - '0');
        if (value > max_thread_count) return false;
    }
    if (value == 0) return false;
    count = value;
    return true;
}

void set_thread_count_for_testing(std::uint64_t count) noexcept {
    thread_count_for_testing.store(
        std::min(count, max_thread_count), std::memory_order_relaxed);
}

void set_backend_for_testing(Backend backend) noexcept {
    backend_for_testing.store(backend, std::memory_order_relaxed);
}

} // namespace quidra::runtime_parallel

extern "C" std::uint64_t qcore_parallel_thread_count() {
    using namespace quidra::runtime_parallel;
    const auto configured = configured_thread_count();
    return serial_depth != 0 ? 1 : configured;
}

extern "C" int qcore_parallel_for(
    std::uint64_t begin, std::uint64_t end, std::uint64_t grain,
    qcore_parallel_body_fn body, void* context) {
    using namespace quidra::runtime_parallel;
    const auto configured = configured_thread_count();
    if (!body || grain == 0 || begin > end) return QCORE_PARALLEL_INVALID_ARGUMENT;
    if (begin == end) return QCORE_PARALLEL_OK;

    Job job;
    job.begin = begin;
    job.end = end;
    job.grain = grain;
    const auto length = end - begin;
    job.chunks = length / grain + (length % grain != 0 ? 1 : 0);
    job.body = body;
    job.context = context;

    const auto threads = serial_depth != 0 ? 1 : configured;
    const auto participants = std::min(threads, job.chunks);
    if (participants <= 1 || !run_parallel(job, participants)) return run_serial(job);
    // Every helper has finished: dispatch_apply_f returned, or the pool caller
    // waited for each helper that joined.
    const int helper_exceptions = job.helper_exceptions.load(std::memory_order_relaxed);
    if (helper_exceptions != 0) std::feraiseexcept(helper_exceptions);
    return job.failed_chunk.load(std::memory_order_acquire) == no_failure
        ? QCORE_PARALLEL_OK
        : job.failure_status;
}
