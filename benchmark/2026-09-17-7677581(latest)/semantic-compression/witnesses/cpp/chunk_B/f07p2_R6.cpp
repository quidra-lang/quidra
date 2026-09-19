#include <cstdio>
int main() {
    int a = 1;
    int b = 3;
// BEGIN PROBE F07.P2
    int q = a / b;
    int m = a % b;
    double d = static_cast<double>(a) / b;
// END PROBE F07.P2
    std::printf("%d %d %.17g\n", q, m, d);
}
