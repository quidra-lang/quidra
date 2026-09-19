#include <cstdio>
int guarded(double a, double b) {
// BEGIN PROBE F08.P2
    int q = b == 0 ? 0 : a / b;
    return q;
// END PROBE F08.P2
}
int main() { std::printf("%d %d\n", guarded(-7.5, 2.0), guarded(1.0, 0.0)); }
