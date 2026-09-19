#include <cstdio>
static int calls = 0;
struct B { long long v; operator int() const { ++calls; std::printf("user conversion ran\n"); return static_cast<int>(v) + 1; } };
int narrow(B big) {
// BEGIN PROBE F10.P1
    int small = static_cast<int>(big);
    return small;
// END PROBE F10.P1
}
int main() { std::printf("%d calls=%d\n", narrow(B{41}), calls); }
