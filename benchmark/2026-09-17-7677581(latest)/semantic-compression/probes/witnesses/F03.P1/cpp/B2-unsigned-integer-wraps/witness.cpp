#include <cstdint>
#include <cstdio>
int main() {
std::uint32_t x = 4294967295u;
// BEGIN PROBE F03.P1
x++;
// END PROBE F03.P1
std::printf("%u\n", x); }
