#include <cstdio>

// BEGIN PROBE F01.P2
long long counter = 0;
constexpr long long LIMIT = 100;
// END PROBE F01.P2

int main() {
    counter += LIMIT;
    std::printf("%lld\n", counter);
}
