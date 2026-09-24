#include <cstdio>

int main() {
    printf("ADV-START\n"); fflush(stdout);
    long long x = 9223372036854775807;
    long long s = x + 3;
    printf("OBS=V:%lld\n", s); fflush(stdout);
    printf("ADV-END\n"); fflush(stdout);
    return 0;
}
