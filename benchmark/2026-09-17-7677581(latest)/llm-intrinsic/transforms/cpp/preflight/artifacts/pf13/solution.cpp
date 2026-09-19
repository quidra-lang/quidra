
#include <iostream>
#include <string>

int main() {
    long long state = 7;
    long long s = 0;
    long long m = 0;
    long long e = 0;
    std::string j;
    for (int k = 0; k < 50; k = k + 1) {
        state = (state * 48271) % 2147483647;
        long long t = state % 1000;
        s = s + t;
        if (t > m) {
            m = t;
        }
        if (t % 2 == 0) {
            e = e + 1;
        }
        if (k < 5) {
            if (k > 0) {
                j = j + "-";
            }
            j = j + std::to_string(t);
        }
    }
    std::cout << "SUM " << s << std::endl;
    std::cout << "MAX " << m << std::endl;
    std::cout << "EVENS " << e << std::endl;
    std::cout << "JOINED " << j << std::endl;
    return 0;
}
