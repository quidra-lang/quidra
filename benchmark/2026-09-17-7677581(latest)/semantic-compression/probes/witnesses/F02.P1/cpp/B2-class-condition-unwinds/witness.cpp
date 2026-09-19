#include <cstdint>
#include <cstdio>
struct C { explicit operator bool() const { std::puts("convert"); throw 1; } };
std::int32_t probe(C cond) {
// BEGIN PROBE F02.P1
std::int32_t v;
if (cond) v = 5;
else v = 9;
return v;
// END PROBE F02.P1
}
int main() { try { std::printf("%d\n", probe(C{})); } catch (int) { std::puts("caught"); } }
