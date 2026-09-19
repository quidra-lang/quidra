#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <vector>
struct Money { int c; };
std::int32_t operator-(const Money& m, std::int32_t a) { std::puts("user-op"); return m.c - a - 100; }
std::int32_t probe() {
std::vector<std::int32_t> xs = {1, 2, 3};
Money k{0};
// BEGIN PROBE F05.P3
auto neg = [k](std::int32_t a) { return k - a; };
std::vector<std::int32_t> ys;
std::ranges::transform(xs, std::back_inserter(ys), neg);
return ys[0];
// END PROBE F05.P3
}
int main() { std::printf("%d\n", probe()); }
