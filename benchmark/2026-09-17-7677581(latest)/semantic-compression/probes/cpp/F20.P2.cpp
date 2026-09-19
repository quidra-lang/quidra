#include <cstdio>

// BEGIN PROBE F20.P2
extern "C" int add2(int a, int b) { return a + b; }
// END PROBE F20.P2

int main() {
    std::printf("%d\n", add2(2, 3));
}
