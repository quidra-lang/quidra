#include <cstdio>
static int calls = 0;
struct M { int v; operator int() const { return v; } };
bool operator==(M x, int y) { ++calls; return x.v == y; }
M operator/(M x, M y) { ++calls; return M{x.v + y.v}; }
int guarded(M a, M b) {
// BEGIN PROBE F08.P2
    int q = b == 0 ? 0 : a / b;
    return q;
// END PROBE F08.P2
}
int main() { std::printf("%d calls=%d\n", guarded(M{7}, M{2}), calls); }
