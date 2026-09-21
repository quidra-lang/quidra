#include <cstdio>
#include <cstdint>

int main() {
    printf("ADV-START\n"); fflush(stdout);
    std::uint32_t a = 0;
    std::uint32_t b = 1;
    std::uint32_t r = a - b;
    printf("OBS=SUB:%u\n", r); fflush(stdout);
    printf("ADV-END\n"); fflush(stdout);
    return 0;
}
