// MB-04 - Floating-point arithmetic: four accumulators over two arrays.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Lehmer / MINSTD generator, frozen for every language in the suite.
class Lcg {
public:
    explicit Lcg(std::int64_t seed) : state_(seed) {}

    std::int64_t next_int() {
        state_ = (48271 * state_) % 2147483647;
        return state_;
    }

    double next_unit() { return static_cast<double>(next_int()) / 2147483647.0; }

private:
    std::int64_t state_;
};

struct Result {
    double s1;
    double s2;
    double s3;
    double s4;
};

static Result workload() {
    constexpr int M = 4000;
    constexpr int R = 75000;

    Lcg gen(20264917);
    std::vector<double> a_vec(M);
    std::vector<double> b_vec(M);
    for (int i = 0; i < M; ++i) a_vec[i] = 0.5 + gen.next_unit();
    for (int i = 0; i < M; ++i) b_vec[i] = 0.5 + gen.next_unit();

    double s1 = 0.0, s2 = 0.0, s3 = 0.0, s4 = 0.0;
    for (int r = 0; r < R; ++r) {
        a_vec[r % M] = a_vec[r % M] + 1.0e-9;  // anti-elimination, part of the algorithm
        for (int i = 0; i < M; ++i) {
            const double a = a_vec[i];
            const double b = b_vec[i];
            s1 = s1 + a * b;
            s2 = s2 + a / (b + 2.0);
            s3 = s3 + std::sqrt(a * a + b * b);
            s4 = s4 + (a - b) * (a - b);
        }
    }

    return Result{s1, s2, s3, s4};
}

// Steady-mode anti-elimination sink. The workload body is a pure function of no
// arguments, so without an observable use of its result on every iteration clang
// treats the call as loop-invariant and sinks it out of the steady loop: verified
// on the frozen recipe, mb01 then reported 42 ns for six of seven iterations while
// the work ran once after the loop. One volatile store per iteration, outside
// every pinned computation, keeps each iteration doing the whole workload.
static volatile double g_steady_sink;

int main(int argc, char** argv) {
    // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
    // Each steady iteration re-seeds the generator and regenerates a_vec and b_vec.
    const char* mode = argc > 1 ? argv[1] : "once";
    Result res{};
    if (std::strcmp(mode, "steady") == 0) {
        int K = argc > 2 ? std::atoi(argv[2]) : 7;
        if (K < 1) K = 7;
        for (int k = 0; k < K; ++k) {
            const auto t0 = std::chrono::steady_clock::now();
            res = workload();
            g_steady_sink = res.s1;
            const auto t1 = std::chrono::steady_clock::now();
            std::printf("ITER %d %lld\n", k,
                        static_cast<long long>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
            std::fflush(stdout);
        }
    } else {
        res = workload();
    }

    std::printf("MB04 s1=%.16e s2=%.16e s3=%.16e s4=%.16e\n", res.s1, res.s2, res.s3, res.s4);
    return 0;
}
