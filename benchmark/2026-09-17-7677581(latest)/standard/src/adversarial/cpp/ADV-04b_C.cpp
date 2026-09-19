#include <cstdio>
#include <cstdint>

int main() {
    printf("ADV-START\n"); fflush(stdout);
    std::int32_t a = -1;
    std::uint32_t b = 1;
    bool c = a < b;
    printf("OBS=CMP:%s\n", c ? "true" : "false"); fflush(stdout);
    printf("ADV-END\n"); fflush(stdout);
    return 0;
}
