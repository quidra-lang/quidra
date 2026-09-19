// MB-02 - Factorial: recompute k! mod M from scratch for every k.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static std::int64_t workload() {
    constexpr std::int64_t M = 1000003;
    constexpr std::int64_t N = 20000;

    std::int64_t total = 0;
    for (std::int64_t k = 1; k <= N; ++k) {
        std::int64_t f = 1;
        for (std::int64_t j = 2; j <= k; ++j) {
            f = (f * j) % M;
        }
        total = (total + f) % M;
    }
    return total;
}

// Steady-mode anti-elimination sink. The workload body is a pure function of no
// arguments, so without an observable use of its result on every iteration clang
// treats the call as loop-invariant and sinks it out of the steady loop: verified
// on the frozen recipe, mb01 then reported 42 ns for six of seven iterations while
// the work ran once after the loop. One volatile store per iteration, outside
// every pinned computation, keeps each iteration doing the whole workload.
static volatile std::int64_t g_steady_sink;

int main(int argc, char** argv) {
    // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
    const char* mode = argc > 1 ? argv[1] : "once";
    std::int64_t total = 0;
    if (std::strcmp(mode, "steady") == 0) {
        int K = argc > 2 ? std::atoi(argv[2]) : 7;
        if (K < 1) K = 7;
        for (int k = 0; k < K; ++k) {
            const auto t0 = std::chrono::steady_clock::now();
            total = workload();
            g_steady_sink = total;
            const auto t1 = std::chrono::steady_clock::now();
            std::printf("ITER %d %lld\n", k,
                        static_cast<long long>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
            std::fflush(stdout);
        }
    } else {
        total = workload();
    }
    std::printf("MB02 %lld\n", static_cast<long long>(total));
    return 0;
}
