#include <cstdint>
#include <cstdio>

std::int32_t probe() {
// BEGIN PROBE F04.P1
std::int32_t slot = 4;
auto& port = slot;
port = 9;
return slot;
// END PROBE F04.P1
}

int main() {
    std::printf("%d\n", probe());
}
