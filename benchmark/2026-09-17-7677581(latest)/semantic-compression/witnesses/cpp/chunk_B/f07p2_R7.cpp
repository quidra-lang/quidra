#include <cstdio>
int main() {
    short a = -7;
    short b = 2;
// BEGIN PROBE F07.P2
    int q = a / b;
    int m = a % b;
    double d = static_cast<double>(a) / b;
// END PROBE F07.P2
    std::printf("%d %d %f\n", q, m, d);
}
