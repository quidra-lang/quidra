#include <cstdio>
#include <cstdint>
#include <vector>
#include <iostream>
#include <string>

int main() {
    printf("ADV-START\n"); fflush(stdout);
    std::string line;
    std::getline(std::cin, line);
    std::int64_t n = std::stoll(line);
    std::vector<std::int64_t> v(n);
    v[0] = 1;
    printf("OBS=ALLOC:%lld|FIRST:%lld\n", (long long)v.size(), (long long)v[0]); fflush(stdout);
    printf("ADV-END\n"); fflush(stdout);
    return 0;
}
