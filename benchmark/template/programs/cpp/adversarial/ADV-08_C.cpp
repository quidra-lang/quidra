#include <cstdio>
#include <cstdint>

int main() {
    printf("ADV-START\n"); fflush(stdout);
    std::int64_t a = 7;
    std::int64_t b = 0;
    std::int64_t q = a / b;
    printf("OBS=QUOT:%lld\n", (long long)q); fflush(stdout);
    printf("ADV-END\n"); fflush(stdout);
    return 0;
}
