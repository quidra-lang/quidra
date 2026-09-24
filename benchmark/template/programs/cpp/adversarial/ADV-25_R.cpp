#include <cstdint>
#include <iostream>
#include <string>

int main() {
    std::cout << "ADV-START" << std::endl;
    std::string t;
    std::getline(std::cin, t);
    std::int64_t r = std::stoll(t) * 2;
    std::cout << "OBS=R:" << r << std::endl;
    std::cout << "ADV-END" << std::endl;
    return 0;
}
