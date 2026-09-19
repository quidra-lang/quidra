#include <cstdio>
static int calls = 0;
struct M { int v; operator double() const { return v; } };
M operator/(M x, M y) { ++calls; return M{x.v - y.v}; }
M operator%(M x, M y) { ++calls; return M{x.v + y.v}; }
int main() {
    M a{7}, b{2};
// BEGIN PROBE F07.P2
    int q = a / b;
    int m = a % b;
    double d = static_cast<double>(a) / b;
// END PROBE F07.P2
    std::printf("%d %d %f calls=%d\n", q, m, d, calls);
}
