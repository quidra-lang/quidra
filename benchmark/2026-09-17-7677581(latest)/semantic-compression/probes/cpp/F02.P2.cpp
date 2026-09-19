#include <cstdint>
#include <cstdio>

std::uint8_t probe() {
// BEGIN PROBE F02.P2
std::uint8_t buf[16];
buf[0] = 1;
return buf[0];
// END PROBE F02.P2
}

int main() {
    std::printf("%u\n", probe());
}
