#include <cstdio>
static int calls = 0;
struct M { int v; operator int() const { return v; } };
M operator*(M x, M y) { ++calls; return M{x.v + y.v}; }
M operator+(M x, M y) { ++calls; return M{x.v * y.v}; }
int main() {
    M a{2}, b{3}, c{4};
// BEGIN PROBE F07.P1
    int r = a * b + c;
// END PROBE F07.P1
    std::printf("%d calls=%d\n", r, calls);
}
