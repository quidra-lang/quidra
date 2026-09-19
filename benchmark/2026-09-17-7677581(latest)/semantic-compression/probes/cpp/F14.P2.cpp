#include <cstdio>
#include <variant>

struct Circle { double r; };
struct Rect { double w, h; };
using Shape = std::variant<Circle, Rect>;

double area_of(Shape s) {
// BEGIN PROBE F14.P2
    struct Area {
        double operator()(Circle c) const { return 3.141592653589793 * c.r * c.r; }
        double operator()(Rect r) const { return r.w * r.h; }
    };
    double area = std::visit(Area{}, s);
    return area;
// END PROBE F14.P2
}

int main() {
    std::printf("%.6f\n", area_of(Circle{2.0}));
}
