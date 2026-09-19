#include <iostream>
#include <string>

int main() {
    const int steps = 50;
    long long state = 7;
    long long total = 0;
    long long biggest = -1;
    long long evens = 0;
    std::string joined;
    for (int i = 0; i < steps; i = i + 1) {
        state = (state * 48271) % 2147483647;
        long long term = state % 1000;
        total = total + term;
        if (term > biggest) {
            biggest = term;
        }
        if (term % 2 == 0) {
            evens = evens + 1;
        }
        if (i < 5) {
            if (i == 0) {
                joined = std::to_string(term);
            } else {
                joined = joined + "-" + std::to_string(term);
            }
        }
    }
    std::cout << "SUM " << total << std::endl;
    std::cout << "MAX " << biggest << std::endl;
    std::cout << "EVENS " << evens << std::endl;
    std::cout << "JOINED " << joined << std::endl;
    return 0;
}
