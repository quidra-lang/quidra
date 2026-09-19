#include <cstdio>

int narrow(long long big) {
// BEGIN PROBE F10.P1
    int small = static_cast<int>(big);
    return small;
// END PROBE F10.P1
}

int main() { std::printf("%d\n", narrow(2147483648LL)); }
