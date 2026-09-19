#include <cstdio>
struct K { int v; K operator++(int) { std::puts("user-op"); v += 10; return *this; } };
int main() {
K x{41};
// BEGIN PROBE F03.P1
x++;
// END PROBE F03.P1
std::printf("%d\n", x.v); }
