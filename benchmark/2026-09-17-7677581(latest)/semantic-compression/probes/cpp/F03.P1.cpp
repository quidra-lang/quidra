#include <cstdint>
#include <cstdio>

std::int32_t probe() {
    std::int32_t x = 41;
// BEGIN PROBE F03.P1
x++;
// END PROBE F03.P1
    return x;
}

int main() {
    std::printf("%d\n", probe());
}
