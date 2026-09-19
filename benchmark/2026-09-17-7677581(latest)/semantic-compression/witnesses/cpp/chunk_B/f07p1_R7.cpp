#include <cstdio>
int main() {
    short a = 300;
    short b = 300;
    short c = 1;
// BEGIN PROBE F07.P1
    int r = a * b + c;
// END PROBE F07.P1
    std::printf("%d\n", r);
}
