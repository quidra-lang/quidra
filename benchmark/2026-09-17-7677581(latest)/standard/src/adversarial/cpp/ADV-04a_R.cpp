#include <cstdio>
#include <cstdint>
#include <iostream>
#include <string>

int main() {
    printf("ADV-START\n"); fflush(stdout);
    std::string l1, l2;
    std::getline(std::cin, l1);
    std::getline(std::cin, l2);
    std::uint32_t a = std::stoul(l1);
    std::uint32_t b = std::stoul(l2);
    std::uint32_t r = a - b;
    printf("OBS=SUB:%u\n", r); fflush(stdout);
    printf("ADV-END\n"); fflush(stdout);
    return 0;
}
