#include <cstdint>
#include <cstdio>
#define cond (std::puts("sub"), true)
std::int32_t probe() {
// BEGIN PROBE F02.P1
std::int32_t v;
if (cond) v = 5;
else v = 9;
return v;
// END PROBE F02.P1
}
int main() { std::printf("%d\n", probe()); }
