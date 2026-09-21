#include <cstdint>
#include <iostream>
#include <string>

std::int64_t scale(std::int64_t v) { return v * 3; }

int main() {
    std::cout << "ADV-START" << std::endl;
    std::string t;
    std::getline(std::cin, t);
    std::int64_t r = scale(t);
    std::cout << "OBS=R:" << r << std::endl;
    std::cout << "ADV-END" << std::endl;
    return 0;
}
