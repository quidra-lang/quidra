#include <cstdint>
#include <cstdio>

// BEGIN PROBE F20.P1
extern "C" int abs(int);

int main() {
    std::int32_t magnitude = abs(-3);
// END PROBE F20.P1
    std::printf("%d\n", magnitude);
}
