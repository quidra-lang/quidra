#include <cstdio>
struct V { int x, y; V operator+(const V& o) const { return V{x+o.x, y+o.y}; } };
int main(){ V v = V{1,2} + V{3,4}; std::printf("X10 %d %d\n", v.x, v.y); }
