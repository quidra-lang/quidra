#include <cstdio>

int main() {
    printf("ADV-START\n"); fflush(stdout);
    long long x = -9223372036854775808;
    long long d = x - 3;
    printf("OBS=V:%lld\n", d); fflush(stdout);
    printf("ADV-END\n"); fflush(stdout);
    return 0;
}
