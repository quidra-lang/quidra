#include <cstdio>
int main() {
    unsigned a = 3000000000u;
    unsigned b = 3u;
    unsigned c = 0u;
// BEGIN PROBE F07.P1
    int r = a * b + c;
// END PROBE F07.P1
    std::printf("%d\n", r);
}
