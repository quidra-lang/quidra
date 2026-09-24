#include <cstdio>
#include <cstdint>
#include <iostream>
#include <string>

int main() {
    printf("ADV-START\n"); fflush(stdout);
    std::string l1, l2;
    std::getline(std::cin, l1);
    std::getline(std::cin, l2);
    std::int32_t a = std::stol(l1);
    std::uint32_t b = std::stoul(l2);
    bool c = a < b;
    printf("OBS=CMP:%s\n", c ? "true" : "false"); fflush(stdout);
    printf("ADV-END\n"); fflush(stdout);
    return 0;
}
