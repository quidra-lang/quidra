#include <cstdio>
constexpr int fact(int n){ return n <= 1 ? 1 : n * fact(n-1); }
static_assert(fact(5) == 120, "evaluated before run time");
int main(){ constexpr int v = fact(5); std::printf("X11 %d\n", v); }
