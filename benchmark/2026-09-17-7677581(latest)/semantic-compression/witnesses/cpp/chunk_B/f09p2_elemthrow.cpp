#include <algorithm>
#include <cstdio>
#include <functional>
#include <vector>
#include <stdexcept>
struct E { int v; };
bool operator>(E x, E y) { if (x.v == 1) throw std::runtime_error("boom"); return x.v > y.v; }
int main() {
    std::vector<E> xs = {E{3}, E{1}, E{2}};
    int a = 1;
    int b = 2;
// BEGIN PROBE F09.P2
    std::ranges::sort(xs, std::greater{});
    bool lt = a < b;
// END PROBE F09.P2
    std::printf("%d %d %d %d\n", xs[0].v, xs[1].v, xs[2].v, lt);
}
