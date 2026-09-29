#include "device_backend.hpp"

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <string>

namespace {
struct SourceProvenance {
    const char* file{};
    const char* revision{};
    const char* node_id{};
    const char* node_kind{};
    unsigned long long line{};
    unsigned long long column{};
};

thread_local const SourceProvenance* source_provenance{};

void print_source_provenance(std::FILE* stream) {
    const auto* provenance = source_provenance;
    if (!provenance || !provenance->node_id || provenance->node_id[0] == '\0')
        return;
    std::fprintf(
        stream,
        " [source_revision=%s node_id=%s node_kind=%s source_file=%s]",
        provenance->revision ? provenance->revision : "",
        provenance->node_id,
        provenance->node_kind ? provenance->node_kind : "",
        provenance->file ? provenance->file : "");
}

std::uint64_t random_next(void* generator) {
    auto* state=static_cast<std::uint64_t*>(generator);
    *state+=0x9e3779b97f4a7c15ULL;
    std::uint64_t value=*state;
    value=(value^(value>>30U))*0xbf58476d1ce4e5b9ULL;
    value=(value^(value>>27U))*0x94d049bb133111ebULL;
    return value^(value>>31U);
}
}

extern "C" void quidra_runtime_set_source_provenance(
    const void* provenance) {
    source_provenance = static_cast<const SourceProvenance*>(provenance);
}

extern "C" void quidra_runtime_fail_at(
    const char* code, const char* message, unsigned long long line,
    unsigned long long column) {
    std::fprintf(
        stderr, "Quidra runtime error[%s] at %llu:%llu: %s",
        code ? code : "RUNTIME", line, column,
        message ? message : "runtime failure");
    print_source_provenance(stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    std::exit(101);
}

extern "C" void quidra_runtime_bounds_fail(
    long long index, long long length, unsigned long long line,
    unsigned long long column) {
    std::fprintf(
        stderr,
        "Quidra runtime error[INDEX_BOUNDS] at %llu:%llu: "
        "index %lld outside length %lld",
        line, column, index, length);
    print_source_provenance(stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    std::exit(101);
}

extern "C" void quidra_test_assert(bool condition) {
    if(condition) return;
    const auto line = source_provenance ? source_provenance->line : 0;
    const auto column = source_provenance ? source_provenance->column : 0;
    std::fprintf(
        stderr, "Quidra test assertion failed at %llu:%llu",
        line, column);
    print_source_provenance(stderr);
    std::fputc('\n', stderr);
    std::exit(1);
}
// io.flush() returns void | error: 0 here is success, anything else becomes
// the error alternative, which fails fast when the statement discards it.
extern "C" int quidra_io_flush() {
    if (std::fflush(stdout) == 0 && !std::ferror(stdout)) return 0;
    return 1;
}
// print and write report the standard output stream's error state after
// writing. The flag is sticky, so a failed write is never silently lost: the
// next output statement, or io.flush(), reports it.
extern "C" int quidra_output_status() {
    return std::ferror(stdout) ? 1 : 0;
}
extern "C" double quidra_time_now(bool sync) {
    if (sync) {
        std::string error;
        if (!quidra::device::synchronize_all(error)) {
            std::fprintf(stderr, "Quidra runtime error[GPU_SYNC]: %s\n", error.c_str());
            std::exit(101);
        }
    }
    using clock=std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}
extern "C" void quidra_gpu_sync(long long index,
                                unsigned long long line,
                                unsigned long long column) {
    if (index < 0) {
        quidra_runtime_fail_at(
            "GPU_SYNC", "GPU index must be non-negative", line, column);
    }
    std::string error;
    if (!quidra::device::synchronize(static_cast<int>(index), error)) {
        quidra_runtime_fail_at("GPU_SYNC", error.c_str(), line, column);
    }
}
extern "C" bool quidra_time_sleep(double seconds) {
    if(!std::isfinite(seconds)||seconds<0.0) return false;
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    return true;
}
extern "C" long long quidra_random_int(void* generator,long long start,long long end) {
    const auto span=static_cast<std::uint64_t>(end)-static_cast<std::uint64_t>(start);
    const auto threshold=(std::uint64_t{0}-span)%span;
    std::uint64_t sample=0;
    do{sample=random_next(generator);}while(sample<threshold);
    const auto result_bits=static_cast<std::uint64_t>(start)+(sample%span);
    return std::bit_cast<long long>(result_bits);
}
extern "C" double quidra_random_float(void* generator) {
    const auto sample=random_next(generator)>>11U;
    return static_cast<double>(sample)*(1.0/9007199254740992.0);
}
extern "C" bool quidra_random_bool(void* generator) {
    return (random_next(generator)&1ULL)!=0;
}
