#include <cstdio>
static int calls = 0;
struct U { int v; operator double() const { return v; } };
U operator+(U x, U y) { ++calls; std::printf("user op+ ran\n"); return U{x.v * y.v}; }
int main() {
    U i{3}, d{4};
// BEGIN PROBE F10.P2
    double sum = i + d;
// END PROBE F10.P2
    std::printf("%f calls=%d\n", sum, calls);
}
