#include "runtime/metal_capture.hpp"
#include <array>
#include <atomic>
#include <cstdlib>
#include <new>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <chrono>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

namespace allocation_test { std::atomic<bool> fail{}; }
void* operator new(std::size_t size) {
    if (allocation_test::fail.load(std::memory_order_relaxed)) throw std::bad_alloc();
    if (void* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
namespace mc = quidra::profile::metal;
namespace {
int failures = 0;
void expect(bool value, const char* why) {
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", why); }
}
struct Mock {
    bool available{true}, supported{true}, capturing{}, refuse_start{}, refuse_stop{}, artifact{true};
    unsigned acquired{}, released{}, queries{}, starts{}, stops{}, clocks{};
    std::array<char, 64> events{};
    std::size_t used{};
    bool long_error{}, reverse_clock{}, fail_support_query{}, fail_capture_query{}, query_known{true};
    std::string path;
    std::mutex mutex;
    std::condition_variable cv;
    bool block_start{}, block_stop{}, entered{}, resume{};
    void event(char e) { if (used < events.size()) events[used++] = e; }
};
bool available(void* p) noexcept { return static_cast<Mock*>(p)->available; }
bool acquire(void* p) noexcept { auto& m = *static_cast<Mock*>(p); ++m.acquired; m.event('a'); return true; }
bool supported(void* p) noexcept { auto& m = *static_cast<Mock*>(p); ++m.queries; m.event('d'); m.query_known = !m.fail_support_query; return m.supported; }
bool capturing(void* p) noexcept { auto& m = *static_cast<Mock*>(p); ++m.queries; m.event('q'); m.query_known = !m.fail_capture_query; return m.capturing; }
bool start(void* p, void* device, const char* path, mc::NativeError& error) noexcept {
    auto& m = *static_cast<Mock*>(p); ++m.starts; m.event('s');
    if (device != &m) return false;
    try {
        m.path = path;
        if (m.block_start) { std::unique_lock lock(m.mutex); m.entered = true; m.cv.notify_all(); m.cv.wait(lock, [&] { return m.resume; }); }
        if (m.refuse_start) { error.domain = "test.capture"; error.code = 17; error.message = m.long_error ? std::string(511, 'a') + "é-tail" : "refused"; return false; }
    } catch (...) { return false; }
    m.capturing = true; return true;
}
bool stop(void* p) noexcept {
    auto& m = *static_cast<Mock*>(p); ++m.stops; m.event('x');
    if (m.block_stop) {
        std::unique_lock lock(m.mutex); m.entered = true; m.cv.notify_all();
        m.cv.wait(lock, [&] { return m.resume; });
    }
    if (m.refuse_stop) return false;
    m.capturing = false;
    if (m.artifact) { try { std::ofstream output(m.path); output << "mock artifact"; } catch (...) {} }
    return true;
}
void release(void* p) noexcept { ++static_cast<Mock*>(p)->released; }
std::uint64_t clock(void* p) noexcept { auto& m = *static_cast<Mock*>(p); ++m.clocks; return m.reverse_clock ? 1000 - m.clocks : 1000 + m.clocks; }
bool query_succeeded(void* p) noexcept { return static_cast<Mock*>(p)->query_known; }
mc::NativeApi api(Mock& m) { return {&m, available, acquire, supported, capturing, start, stop, release, clock, nullptr, query_succeeded}; }
struct Directory {
    std::filesystem::path path;
    Directory() {
        const auto base = std::filesystem::temp_directory_path();
        for (unsigned i = 0; i != 1000; ++i) {
            auto candidate = base / ("quidra-capture-unit-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "-" + std::to_string(i));
            if (std::filesystem::create_directory(candidate)) { path = std::move(candidate); return; }
        }
        throw std::runtime_error("temporary directory unavailable");
    }
    ~Directory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    std::string file(const char* name) const { return (path / name).string(); }
};
mc::Creation create(Mock& m, const std::string& path, std::uint64_t step = 3) {
    const auto native = api(m);
    return mc::make_capture({true, step, path, {}}, &native);
}
mc::CommandInput input(Mock& m, std::uint64_t step = 3, std::uint64_t serial = 1, std::int32_t device = 2) {
    return {step, serial, device, &m, {}};
}
void record(mc::Capture& c, Mock& m, std::uint64_t serial = 1) {
    const auto token = c.before_command(input(m, 3, serial));
    expect(static_cast<bool>(token), "start occurs before target command creation");
    m.event('c'); expect(c.command_created(token), "actual creation is recorded");
    m.event('k'); expect(c.command_committed(token, 3, 3), "actual target commitment is recorded");
}
void off_and_paths(Directory& dir) {
    Mock m; auto native = api(m);
    allocation_test::fail.store(true);
    auto off = mc::make_capture({false, 0, "/dev/stdout", {}}, &native);
    allocation_test::fail.store(false);
    expect(!off.capture && off.status.state == mc::State::Off, "off request does not allocate controller");
    expect(m.acquired == 0 && m.queries == 0 && m.clocks == 0, "off makes no native or clock calls");
    expect(!create(m, "relative.gputrace").capture, "relative destination refused");
    expect(!create(m, dir.file("wrong.json")).capture, "non-trace destination refused");
    const auto existing = dir.file("existing.gputrace"); { std::ofstream out(existing); out << "preserve"; }
    expect(!create(m, existing).capture, "existing artifact is never overwritten");
    expect(!create(m, "/dev/stdout.gputrace").capture, "stream/device destination refused");
    auto bad = dir.file("bad.gputrace"); bad.insert(bad.size() - 9, 1, static_cast<char>(0xff));
    expect(!create(m, bad).capture, "invalid UTF-8 destination refused");
    auto nul = dir.file("nul.gputrace"); nul.insert(2, 1, '\0');
    expect(!create(m, nul).capture, "embedded NUL destination refused");
    const auto alias = dir.file("alias.gputrace"); std::array<std::string_view, 1> excluded{alias};
    expect(!mc::make_capture({true, 3, alias, excluded}, &native).capture, "report/source alias refused before native calls");
    expect(m.acquired == 0 && m.starts == 0, "path rejection never acquires manager");
    auto max = create(m, dir.file("max.gputrace"), std::numeric_limits<std::uint64_t>::max());
    expect(max.capture && max.capture->snapshot().requested_step == std::numeric_limits<std::uint64_t>::max(), "requested step remains full uint64");
    max.capture->stop(mc::Reason::NormalExit);
    expect(max.capture->snapshot().state == mc::State::NoMetalWork && m.acquired == 0, "CPU-only request does not acquire manager");
}
void ownership(Directory& dir) {
    { Mock m; m.fail_support_query = true; auto c = create(m, dir.file("query-start.gputrace"));
      expect(!c.capture->before_command(input(m)) && m.starts == 0 && c.capture->snapshot().state == mc::State::Unavailable, "failed destination query cannot claim support or start"); c.capture->stop(mc::Reason::NormalExit); }
    { Mock m; auto c = create(m, dir.file("query-stop.gputrace")); record(*c.capture, m); m.fail_capture_query = true; c.capture->boundary(3);
      expect(m.stops == 0 && m.released == 1 && c.capture->snapshot().state == mc::State::Unavailable, "failed ownership query cannot claim external stop or stop an uncertain manager"); }
    { Mock m; m.capturing = true; auto c = create(m, dir.file("foreign.gputrace"));
      expect(!c.capture->before_command(input(m)), "foreign capture refused"); c.capture->stop(mc::Reason::ImmediateFailure);
      expect(m.starts == 0 && m.stops == 0 && m.released == 1 && c.capture->snapshot().gaps[static_cast<std::size_t>(mc::Gap::AlreadyCapturing)] == 1, "foreign manager never stopped"); }
    { Mock m; m.refuse_start = true; auto c = create(m, dir.file("fail.gputrace"));
      expect(!c.capture->before_command(input(m)), "checked failed start refused"); c.capture->stop(mc::Reason::NormalExit);
      const auto s = c.capture->snapshot(); expect(m.starts == 1 && m.stops == 0 && m.released == 1 && s.error.code == 17 && s.error.domain == "test.capture", "failed start preserves error and never owns stop"); }
    { Mock m; m.supported = false; auto c = create(m, dir.file("unsupported.gputrace"));
      expect(!c.capture->before_command(input(m)), "unsupported destination refused");
      expect(m.starts == 0 && c.capture->snapshot().state == mc::State::Unsupported, "unsupported state is explicit"); }
    { Mock m; auto c = create(m, dir.file("exact.gputrace")); record(*c.capture, m); c.capture->boundary(3); c.capture->stop(mc::Reason::NormalExit);
      const auto s = c.capture->snapshot(); expect(m.stops == 1 && m.released == 1 && s.stopped && s.exact_core_step && s.artifact_exists_known && s.artifact_exists, "owned complete window stops once and separates artifact existence");
      expect(!s.external_queues_verified && s.first_serial == 1 && s.last_serial == 1 && s.created_commands == 1 && s.committed_commands == 1, "exact Core window does not claim external queues");
      expect(s.start_begin_ns < s.start_end_ns && s.stop_begin_ns < s.stop_end_ns && s.clock_queries == 4, "native operation elapsed endpoints are actual clock queries");
      expect(s.start_clock_known && s.stop_clock_known, "clock availability is explicit");
      const std::string events(m.events.data(), m.used); expect(events.find('s') < events.find('c') && events.find('k') < events.find('x'), "capture start precedes creation and commit precedes stop"); }
    { Mock m; auto c = create(m, dir.file("interrupted.gputrace")); record(*c.capture, m); m.capturing = false; c.capture->boundary(3);
      expect(m.stops == 0 && m.released == 1 && c.capture->snapshot().state == mc::State::Interrupted, "externally ended capture is never stopped again"); }
}
void storage_and_native_identity(Directory& dir) {
    { Mock m; auto c = create(m, dir.file("late-token.gputrace")); record(*c.capture, m);
      const auto delayed = c.capture->before_command(input(m, 3, 2)); c.capture->boundary(3);
      expect(c.capture->snapshot().exact_core_step, "initial completed window has exact Core coverage");
      expect(!c.capture->command_created(delayed), "creation outside stopped window is rejected");
      expect(!c.capture->snapshot().exact_core_step, "late coverage gap clears terminal exactness"); }
    { Mock m; const auto path = dir.file("late.gputrace"); auto c = create(m, path);
      { std::ofstream out(path); out << "keep"; }
      expect(!c.capture->before_command(input(m)) && m.starts == 0 && m.acquired == 0, "destination created after arming is rechecked before manager acquisition");
      std::ifstream file(path); std::string content; file >> content; expect(content == "keep", "late destination remains untouched"); }
    { Mock m; auto c = create(m, dir.file("identity.gputrace")); record(*c.capture, m); auto wrong = input(m, 3, 2); Mock other; wrong.native_device = &other;
      expect(!c.capture->before_command(wrong), "same ordinal with a different actual native device is refused"); c.capture->boundary(3);
      expect(!c.capture->snapshot().exact_core_step, "native device mismatch is a coverage gap"); }
    { Mock m; m.refuse_start = m.long_error = true; auto c = create(m, dir.file("long.gputrace")); c.capture->before_command(input(m));
      const auto e = c.capture->snapshot().error; expect(e.truncated && e.message.size() == 511 && e.message.back() == 'a', "bounded native error avoids splitting a UTF-8 code point"); }
    { Mock m; m.reverse_clock = true; auto c = create(m, dir.file("clock.gputrace")); record(*c.capture, m); c.capture->boundary(3);
      const auto s = c.capture->snapshot(); expect(!s.start_clock_known && !s.stop_clock_known, "reversed native operation clocks remain unavailable"); }
    { Mock m; const auto path = dir.file("oom-config.gputrace"); auto native = api(m);
      allocation_test::fail.store(true); auto c = mc::make_capture({true, 3, path, {}}, &native); allocation_test::fail.store(false);
      expect(!c.capture && c.status.state == mc::State::Unavailable && m.acquired == 0, "configuration allocation failure is unavailable with no native operation"); }
    { Mock m; auto c = create(m, dir.file("oom-record.gputrace")); const auto token = c.capture->before_command(input(m));
      allocation_test::fail.store(true); const bool made = c.capture->command_created(token); allocation_test::fail.store(false);
      expect(!made, "creation metadata allocation failure is reported"); c.capture->boundary(3);
      const auto s = c.capture->snapshot(); expect(m.stops == 1 && !s.exact_core_step && s.metadata_drops == 1, "metadata failure preserves owned stop but cannot claim coverage"); }
    { Mock m; auto c = create(m, dir.file("oom-snapshot.gputrace")); record(*c.capture, m);
      allocation_test::fail.store(true); const auto failed = c.capture->snapshot(); allocation_test::fail.store(false);
      expect(failed.state == mc::State::Unavailable, "snapshot failure is explicit"); c.capture->boundary(3);
      expect(c.capture->snapshot().stopped && m.stops == 1, "snapshot failure does not corrupt retained ownership state"); }
}
void coverage(Directory& dir) {
    { Mock m; auto c = create(m, dir.file("prior-extra.gputrace")); c.capture->note_edge({true, false, false, false, 1, 0});
      expect(m.acquired == 0 && c.capture->snapshot().extra_commits == 1, "actual prior extra commit is recorded without premature capture start");
      record(*c.capture, m); c.capture->boundary(3); expect(c.capture->snapshot().exact_core_step, "safe prior extra commit does not invalidate exact target window"); }
    { Mock m; auto c = create(m, dir.file("held.gputrace")); auto in = input(m); in.edge.held = true;
      expect(!c.capture->before_command(in) && m.acquired == 0, "held entry has no manager action or stream work"); }
    { Mock m; auto c = create(m, dir.file("prior.gputrace")); auto in = input(m); in.edge.prior_work_closed = false;
      expect(!c.capture->before_command(in) && m.starts == 0, "preexisting uncommitted buffer cannot become captured by encoder split"); }
    { Mock m; auto c = create(m, dir.file("partial.gputrace")); const auto token = c.capture->before_command(input(m)); c.capture->command_created(token); c.capture->boundary(3);
      expect(c.capture->snapshot().state == mc::State::Partial && !c.capture->snapshot().exact_core_step && m.stops == 1, "missing commitment stops safely and reports partial"); }
    { Mock m; auto c = create(m, dir.file("span.gputrace")); const auto token = c.capture->before_command(input(m)); c.capture->command_created(token); c.capture->command_committed(token, 2, 3, true); c.capture->boundary(3);
      expect(!c.capture->snapshot().exact_core_step && c.capture->snapshot().extra_commits == 1, "spanning work is not exact and extra physical commit is reported"); }
    { Mock m; auto c = create(m, dir.file("multi.gputrace")); record(*c.capture, m); expect(!c.capture->before_command(input(m, 3, 1, 4)), "second actual device omitted"); expect(!c.capture->before_command(input(m, 3, 2, 4)), "repeated omitted device remains omitted"); c.capture->boundary(3);
      expect(c.capture->snapshot().omitted_devices == 1 && !c.capture->snapshot().exact_core_step, "multi-device gap is explicit"); }
    { Mock m; m.artifact = false; auto c = create(m, dir.file("absent.gputrace")); record(*c.capture, m); c.capture->boundary(3);
      const auto s = c.capture->snapshot(); expect(s.stopped && s.artifact_exists_known && !s.artifact_exists && s.exact_core_step, "stop has no error return and absent artifact is not claimed successful"); }
    { Mock m; auto c = create(m, dir.file("duplicate.gputrace")); const auto token = c.capture->before_command(input(m)); c.capture->command_created(token);
      expect(!c.capture->command_created(token), "duplicate actual creation rejected"); c.capture->command_committed(token, 3, 3);
      expect(!c.capture->command_committed(token, 3, 3), "duplicate actual commitment rejected"); c.capture->boundary(3); expect(!c.capture->snapshot().exact_core_step, "invalid lifecycle prevents exact coverage claim"); }
    { Mock m; auto c = create(m, dir.file("failure.gputrace")); record(*c.capture, m); c.capture->stop(mc::Reason::ImmediateFailure);
      expect(m.stops == 1 && c.capture->snapshot().state == mc::State::Partial, "failure finalization stops only owned capture without commitment or wait"); }
    { Mock m; auto c = create(m, dir.file("busy-stop.gputrace")); record(*c.capture, m); m.block_stop = true;
      std::thread worker([&] { c.capture->boundary(3); });
      { std::unique_lock lock(m.mutex); m.cv.wait(lock, [&] { return m.entered; }); }
      expect(!c.capture->stop(mc::Reason::ImmediateFailure, {}, true), "failure finalization cannot wait for a native stop already in progress");
      { std::lock_guard lock(m.mutex); m.resume = true; } m.cv.notify_all(); worker.join();
      const auto s = c.capture->snapshot();
      expect(!s.exact_core_step && s.gaps[static_cast<std::size_t>(mc::Gap::StopBusy)] == 1, "concurrent refused stop invalidates exactness after native stop returns");
      expect(s.state == mc::State::Partial && s.stopped && m.stops == 1 && m.released == 1, "busy terminal stop is partial while preserving successful native stop and one release"); }
    { Mock m; auto c = create(m, dir.file("busy.gputrace")); m.block_start = true;
      std::thread worker([&] { c.capture->before_command(input(m)); });
      { std::unique_lock lock(m.mutex); m.cv.wait(lock, [&] { return m.entered; }); }
      expect(m.released == 0, "active native call retains its manager reference");
      expect(!c.capture->stop(mc::Reason::ImmediateFailure, {}, true), "immediate finalization does not block on active native operation");
      auto report = std::async(std::launch::async, [&] { return c.capture->snapshot(true); });
      const bool ready = report.wait_for(std::chrono::milliseconds(20)) == std::future_status::ready;
      expect(ready, "immediate failure snapshot does not block after refused try-stop");
      { std::lock_guard lock(m.mutex); m.resume = true; } m.cv.notify_all(); worker.join(); const auto fallback = report.get();
      expect(!fallback.snapshot_complete && fallback.requested_step == 3, "busy snapshot explicitly marks mutable native status unavailable"); c.capture->stop(mc::Reason::NormalExit);
      expect(m.released == 1 && c.capture->snapshot().gaps[static_cast<std::size_t>(mc::Gap::StopBusy)] == 1 && !c.capture->snapshot().exact_core_step, "busy stop remains explicit after lock becomes available"); }
}
}
int main() {
    try { Directory dir; off_and_paths(dir); ownership(dir); coverage(dir); storage_and_native_identity(dir); }
    catch (const std::exception& e) { std::fprintf(stderr, "test setup: %s\n", e.what()); return 2; }
    if (failures) return 1;
    std::puts("Metal capture ownership and lifecycle: ok"); return 0;
}
