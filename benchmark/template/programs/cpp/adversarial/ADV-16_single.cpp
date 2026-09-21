#include <cstdint>
#include <iostream>

int main() {
    std::cout << "ADV-START" << std::endl;
    constexpr std::int64_t x = 10;
    x = 20;
    std::cout << "OBS=V:" << x << std::endl;
    std::cout << "ADV-END" << std::endl;
    return 0;
}
