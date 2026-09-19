#include <cstdio>

int main() {
    int a = -7;
    int b = 2;
// BEGIN PROBE F07.P2
    int q = a / b;
    int m = a % b;
    double d = static_cast<double>(a) / b;
// END PROBE F07.P2
    std::printf("%d %d %f\n", q, m, d);
}
