#include <cstdio>
int main() {
    double a = 0.1;
    double b = 3.0;
    double c = 0.0;
// BEGIN PROBE F07.P1
    int r = a * b + c;
// END PROBE F07.P1
    std::printf("%d %.17g\n", r, a * b + c);
}
