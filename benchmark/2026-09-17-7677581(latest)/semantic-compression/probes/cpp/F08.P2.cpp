#include <cstdio>

int guarded(int a, int b) {
// BEGIN PROBE F08.P2
    int q = b == 0 ? 0 : a / b;
    return q;
// END PROBE F08.P2
}

int main() { std::printf("%d\n", guarded(7, 0)); }
