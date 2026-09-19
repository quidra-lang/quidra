#include <cstdio>
#include <variant>

int main() {
// BEGIN PROBE F14.P1
    struct Circle { double r; };
    struct Rect { double w, h; };
    using Shape = std::variant<Circle, Rect>;
    Shape s = Circle{2.0};
// END PROBE F14.P1
    std::printf("%zu\n", s.index());
}
