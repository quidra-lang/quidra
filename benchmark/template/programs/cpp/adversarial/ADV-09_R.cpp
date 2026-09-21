#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <iostream>
#include <string>

int main() {
    printf("ADV-START\n"); fflush(stdout);
    std::string line;
    std::getline(std::cin, line);
    std::size_t z = std::stoull(line);
    std::vector<std::int64_t> v = {10, 20, 30, 40, 50};
    std::size_t i = z - 1;
    std::int64_t e = v[i];
    printf("OBS=ELEM:%lld\n", (long long)e); fflush(stdout);
    printf("ADV-END\n"); fflush(stdout);
    return 0;
}
