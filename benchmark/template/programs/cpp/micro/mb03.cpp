// MB-03 - Integer arithmetic: mixed add / xor / multiply-modulo / divide.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct Result {
    std::int64_t s_add;
    std::int64_t s_xor;
    std::int64_t s_mul;
    std::int64_t s_div;
};

static Result workload() {
    constexpr std::int64_t N = 120000000;

    std::int64_t x = 20263917;  // the seed itself is the initial generator state
    std::int64_t s_add = 0;
    std::int64_t s_xor = 0;
    std::int64_t s_mul = 1;
    std::int64_t s_div = 0;

    for (std::int64_t i = 0; i < N; ++i) {
        x = (48271 * x) % 2147483647;
        s_add = (s_add + x) % 2147483647;
        s_xor = s_xor ^ x;
        s_mul = (s_mul * 33 + (x % 97)) % 1000003;
        s_div = s_div + x / 1000;
    }

    return Result{s_add, s_xor, s_mul, s_div};
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
    Result res{};
    if (std::strcmp(mode, "steady") == 0) {
        int K = argc > 2 ? std::atoi(argv[2]) : 7;
        if (K < 1) K = 7;
        for (int k = 0; k < K; ++k) {
            const auto t0 = std::chrono::steady_clock::now();
            res = workload();
            g_steady_sink = res.s_add;
            const auto t1 = std::chrono::steady_clock::now();
            std::printf("ITER %d %lld\n", k,
                        static_cast<long long>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
            std::fflush(stdout);
        }
    } else {
        res = workload();
    }

    std::printf("MB03 %lld %lld %lld %lld\n",
                static_cast<long long>(res.s_add), static_cast<long long>(res.s_xor),
                static_cast<long long>(res.s_mul), static_cast<long long>(res.s_div));
    return 0;
}
