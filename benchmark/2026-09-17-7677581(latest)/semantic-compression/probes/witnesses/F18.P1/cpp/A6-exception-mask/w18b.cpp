#include <iostream>
#include <unistd.h>
int main() {
    std::cout.exceptions(std::ios::failbit | std::ios::badbit);
    ::close(1);
    try {
        std::cout << "x\n";
        std::cout.flush();
    } catch (const std::ios_base::failure& e) {
        std::cerr << "THREW ios_base::failure\n";
        return 0;
    }
    std::cerr << "no throw\n";
}
