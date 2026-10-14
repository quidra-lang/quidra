// Verify the real writer owns a FILE lock throughout the document. The shim
// forwards formatting unchanged, then a foreign thread tries the real lock.
#ifndef _WIN32
#include "runtime/timeline.hpp"
#include "platform/clock.hpp"
#include "platform/environment.hpp"
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
std::atomic<unsigned> unlocked_writes{};
void check_stream_lock(std::FILE* file) {
    if (file != stderr) return;
    bool available = false;
    std::thread other([&] {
        if (::ftrylockfile(file) == 0) {
            available = true;
            ::funlockfile(file);
        }
    });
    other.join();
    if (available) unlocked_writes.fetch_add(1, std::memory_order_relaxed);
}
}
namespace stream_test_std {
using namespace std;
int fprintf(FILE* file, const char* format, ...) __attribute__((format(printf, 2, 3)));
int fprintf(FILE* file, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int result = std::vfprintf(file, format, arguments);
    va_end(arguments);
    check_stream_lock(file);
    return result;
}
}

// Alias standard names while substituting only the actual stdio call boundary.
#define std stream_test_std
#include "../src/runtime/timeline.cpp"
#undef std

namespace quidra::counters {
bool current_source_site(SourceSite&) { return false; }
}
namespace quidra::platform {
std::uint64_t monotonic_ns() { return 1; }
std::uint64_t process_cpu_ns() { return 1; }
std::uint64_t process_start_ns() { return 1; }
}

int main() {
    if (!quidra::timeline::enabled()) return 2;
    quidra::timeline::mark_program_started();
    quidra::timeline::note_host({quidra::timeline::Point::StepBoundary, 0, 100, 20, {}});
    if (!quidra::timeline::flush_report()) return 3;
    if (unlocked_writes.load(std::memory_order_relaxed) != 0) {
        std::puts("FAIL: timeline document releases stderr between fragments");
        return 1;
    }
    std::puts("runtime timeline stream locking: ok");
    return 0;
}
#else
#include <cstdio>
int main() {
    std::puts("runtime timeline stream locking: POSIX try-lock check unavailable");
    return 0;
}
#endif
