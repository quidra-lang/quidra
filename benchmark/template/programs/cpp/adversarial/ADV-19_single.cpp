#include <cstdint>
#include <iostream>
#include <string>

std::int64_t f(std::int64_t n) { return 1 + f(n + 1); }

int main() {
    std::cout << "ADV-START" << std::endl;
    std::string t;
    std::getline(std::cin, t);
    std::int64_t n = std::stoll(t);
    std::int64_t r = f(n);
    std::cout << "OBS=R:" << r << std::endl;
    std::cout << "ADV-END" << std::endl;
    return 0;
}
