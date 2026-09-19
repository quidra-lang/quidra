#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <vector>

std::int32_t probe() {
std::vector<double> xs = {1.5, 2.5, 3.5};
double k = 0.25;
// BEGIN PROBE F05.P3
auto neg = [k](std::int32_t a) { return k - a; };
std::vector<std::int32_t> ys;
std::ranges::transform(xs, std::back_inserter(ys), neg);
return ys[0];
// END PROBE F05.P3
}
int main() { std::printf("%d\n", probe()); }
