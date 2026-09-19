#include <cstdio>
#include <variant>
#include <string>
struct Circle { double r; };
struct Rect { double w, h; };
using Shape = std::variant<Circle, Rect>;
std::string name(const Shape& s){ return std::visit([](auto&& v)->std::string{
  using T = std::decay_t<decltype(v)>;
  if constexpr (std::is_same_v<T, Circle>) return "circle"; else return "rect"; }, s); }
int main(){ Shape a=Circle{1.0}, b=Rect{2.0,3.0};
  std::printf("X02 %s %s (discriminants %zu %zu)\n", name(a).c_str(), name(b).c_str(), a.index(), b.index()); }
