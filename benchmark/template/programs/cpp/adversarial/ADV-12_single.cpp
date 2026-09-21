#include <cstdio>
#include <cstdint>

int main() {
    printf("ADV-START\n"); fflush(stdout);
    std::int64_t x;
    printf("OBS=VAL:%lld\n", (long long)x); fflush(stdout);
    printf("ADV-END\n"); fflush(stdout);
    return 0;
}
