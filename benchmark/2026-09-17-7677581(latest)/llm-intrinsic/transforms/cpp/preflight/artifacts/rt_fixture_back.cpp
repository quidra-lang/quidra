#include <iostream>
#include <string>

int main() {
    const int steps = 6;
    long long value = 3;
    long long total = 0;
    long long biggest = -1;
    long long odds = 0;
    std::string chain;
    for (int i = 0; i < steps; i = i + 1) {
        value = (value * 7) % 101;
        total = total + value;
        if (value > biggest) {
            biggest = value;
        }
        if (value % 2 == 1) {
            odds = odds + 1;
        }
        if (i == 0) {
            chain = std::to_string(value);
        } else {
            chain = chain + "/" + std::to_string(value);
        }
    }
    std::cout << "TOTAL " << total << std::endl;
    std::cout << "BIGGEST " << biggest << std::endl;
    std::cout << "ODDS " << odds << std::endl;
    std::cout << "CHAIN " << chain << std::endl;
    return 0;
}
