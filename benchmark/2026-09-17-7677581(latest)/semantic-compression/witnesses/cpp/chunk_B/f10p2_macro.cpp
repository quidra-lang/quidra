#include <cstdio>
int main() {
    int i = 3;
    double d = 0.5;
#define d 2
// BEGIN PROBE F10.P2
    double sum = i + d;
// END PROBE F10.P2
#undef d
    std::printf("%f\n", sum);
}
