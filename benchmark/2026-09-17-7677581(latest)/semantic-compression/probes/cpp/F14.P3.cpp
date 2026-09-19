#include <cstdio>

#include "util.h"

double tri_area() {
// BEGIN PROBE F14.P3
    struct Tri : Shape {
        double b = 3.0, h = 4.0;
        double area() const override { return 0.5 * b * h; }
    };
    Tri t;
    const Shape& s = t;
    return s.area();
// END PROBE F14.P3
}

int main() {
    std::printf("%.6f\n", tri_area());
}
