#include <cstdio>
#include <vector>
long long i64 = 9007199254740993LL;
int main() {
    long long i = 9007199254740993LL;
    double d = 0.0;
// BEGIN PROBE F10.P2
    double sum = i + d;
// END PROBE F10.P2
    std::printf("%.17g  exact=%lld\n", sum, i);
}
