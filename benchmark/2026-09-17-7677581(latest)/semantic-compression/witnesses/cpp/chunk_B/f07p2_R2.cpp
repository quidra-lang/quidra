#include <cstdio>
int main() {
    long long a = 8589934592LL;
    long long b = 1LL;
// BEGIN PROBE F07.P2
    int q = a / b;
    int m = a % b;
    double d = static_cast<double>(a) / b;
// END PROBE F07.P2
    std::printf("%d %d %f\n", q, m, d);
}
