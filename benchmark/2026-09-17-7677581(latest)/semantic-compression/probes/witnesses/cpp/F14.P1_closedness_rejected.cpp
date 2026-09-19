#include <variant>
struct Circle { double r; };
struct Rect { double w, h; };
struct Tri { double b, h; };
using Shape = std::variant<Circle, Rect>;
int main() { Shape s = Tri{1.0, 2.0}; (void)s; }
