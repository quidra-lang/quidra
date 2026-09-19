// CONC-1, C++. Worker mechanism: std::thread (<thread>, C++ standard library).
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <thread>
#include <vector>

static const long long N = 500000000LL;
static const int CHUNKS = 4;
static const long long SPAN = N / CHUNKS;

static double chunk_sum(int c) {
    double s = 0.0;
    long long start = (long long)c * SPAN;
    long long end = start + SPAN;
    for (long long i = start; i < end; i++) { double x = std::sin((double)i); s += x * x; }
    return s;
}

int main(int argc, char** argv) {
    int W = (argc > 1) ? std::atoi(argv[1]) : 1;
    std::vector<double> partial(CHUNKS, 0.0);
    std::vector<std::thread> th;
    for (int w = 0; w < W; w++) {
        th.emplace_back([w, W, &partial]() {
            for (int c = 0; c < CHUNKS; c++) if (c % W == w) partial[c] = chunk_sum(c);
        });
    }
    for (auto& t : th) t.join();
    double total = 0.0;
    for (int c = 0; c < CHUNKS; c++) total = total + partial[c];
    std::printf("workers=%d result=%.10f\n", W, total);
    return 0;
}
