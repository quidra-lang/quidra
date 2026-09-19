#include <cstdio>
int guarded(short a, short b) {
// BEGIN PROBE F08.P2
    int q = b == 0 ? 0 : a / b;
    return q;
// END PROBE F08.P2
}
int main() { std::printf("%d\n", guarded(-7, 2)); }
