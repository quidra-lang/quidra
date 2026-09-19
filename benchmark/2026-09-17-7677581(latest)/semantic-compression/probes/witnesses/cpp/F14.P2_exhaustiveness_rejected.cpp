#include <variant>
struct Circle { double r; };
struct Rect { double w, h; };
using Shape = std::variant<Circle, Rect>;
struct Area { double operator()(Circle c) const { return c.r; } };
int main() { Shape s = Circle{2.0}; return (int)std::visit(Area{}, s); }
