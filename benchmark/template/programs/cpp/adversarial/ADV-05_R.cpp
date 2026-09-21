#include <cstdio>
#include <cstdint>
#include <iostream>
#include <string>

int main() {
    printf("ADV-START\n"); fflush(stdout);
    std::string line;
    std::getline(std::cin, line);
    double d = std::stod(line);
    std::int32_t v = d;
    printf("OBS=V:%d\n", (int)v); fflush(stdout);
    printf("ADV-END\n"); fflush(stdout);
    return 0;
}
