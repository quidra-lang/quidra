#include <cstdio>
int guarded(long long a, long long b) {
// BEGIN PROBE F08.P2
    int q = b == 0 ? 0 : a / b;
    return q;
// END PROBE F08.P2
}
int main() { std::printf("%d\n", guarded(8589934592LL, 1LL)); }
