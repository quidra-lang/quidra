#include <cstdio>

int seven() {
// BEGIN PROBE F01.P1
const int n = 7;
return n;
// END PROBE F01.P1
}

int main() {
    std::printf("%d\n", seven());
}
