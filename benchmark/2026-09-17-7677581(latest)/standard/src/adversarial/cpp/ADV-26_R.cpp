#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

int main() {
    std::cout << "ADV-START" << std::endl;
    std::vector<std::int64_t> a;
    std::string t;
    for (int i = 0; i < 7; i++) {
        std::getline(std::cin, t);
        a.push_back(std::stoll(t));
    }
    std::getline(std::cin, t);
    std::int64_t target = std::stoll(t);
    std::int64_t lo = 0, hi = 6, result = -1;
    while (lo <= hi) {
        std::int64_t mid = (lo + hi) / 2;
        if (a[mid] == target) { result = mid; break; }
        if (a[mid] < target) lo = mid + 1; else hi = mid - 1;
    }
    std::cout << "OBS=IDX:" << result << std::endl;
    std::cout << "ADV-END" << std::endl;
    return 0;
}
