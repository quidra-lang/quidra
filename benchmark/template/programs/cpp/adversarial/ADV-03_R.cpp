#include <cstdio>
#include <iostream>
#include <string>

int main() {
    printf("ADV-START\n"); fflush(stdout);
    std::string line;
    std::getline(std::cin, line);
    long long x = std::stoll(line);
    long long d = x - 3;
    printf("OBS=V:%lld\n", d); fflush(stdout);
    printf("ADV-END\n"); fflush(stdout);
    return 0;
}
