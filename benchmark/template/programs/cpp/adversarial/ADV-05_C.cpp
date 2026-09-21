#include <cstdio>
#include <cstdint>

int main() {
    printf("ADV-START\n"); fflush(stdout);
    double d = 1e30;
    std::int32_t v = d;
    printf("OBS=V:%d\n", (int)v); fflush(stdout);
    printf("ADV-END\n"); fflush(stdout);
    return 0;
}
