#include <cstdio>
int narrow(double big) {
// BEGIN PROBE F10.P1
    int small = static_cast<int>(big);
    return small;
// END PROBE F10.P1
}
int main() { std::printf("%d\n", narrow(-3.9)); }
