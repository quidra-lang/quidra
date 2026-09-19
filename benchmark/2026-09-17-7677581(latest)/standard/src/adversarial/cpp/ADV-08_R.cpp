#include <cstdio>
#include <cstdint>
#include <iostream>
#include <string>

int main() {
    printf("ADV-START\n"); fflush(stdout);
    std::string l1, l2;
    std::getline(std::cin, l1);
    std::getline(std::cin, l2);
    std::int64_t a = std::stoll(l1);
    std::int64_t b = std::stoll(l2);
    std::int64_t q = a / b;
    printf("OBS=QUOT:%lld\n", (long long)q); fflush(stdout);
    printf("ADV-END\n"); fflush(stdout);
    return 0;
}
