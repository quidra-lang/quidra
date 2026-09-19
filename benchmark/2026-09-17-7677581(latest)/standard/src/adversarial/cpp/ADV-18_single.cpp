#include <cstdint>
#include <iostream>
#include <string>

std::int64_t pick(bool c) {
    if (c) return 1;
}

int main() {
    std::cout << "ADV-START" << std::endl;
    std::string t;
    std::getline(std::cin, t);
    bool c = std::stoll(t);
    std::int64_t r = pick(c) * 2;
    std::cout << "OBS=R:" << r << std::endl;
    std::cout << "ADV-END" << std::endl;
    return 0;
}
