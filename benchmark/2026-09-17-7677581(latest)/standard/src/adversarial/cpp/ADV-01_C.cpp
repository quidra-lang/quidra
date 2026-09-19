#include <cstdio>
#include <cstdint>

int main() {
    printf("ADV-START\n"); fflush(stdout);
    std::int64_t x = 9223372036854775807;
    std::int32_t v = x;
    printf("OBS=V:%d\n", (int)v); fflush(stdout);
    printf("ADV-END\n"); fflush(stdout);
    return 0;
}
