#include <algorithm>
#include <cstdio>
#include <functional>
#include <vector>
static int calls = 0;
struct C { int v; };
bool operator<(C x, C y) { ++calls; std::printf("user op< ran\n"); return x.v > y.v; }
int main() {
    std::vector<int> xs = {3, 1, 2};
    C a{1}, b{2};
// BEGIN PROBE F09.P2
    std::ranges::sort(xs, std::greater{});
    bool lt = a < b;
// END PROBE F09.P2
    std::printf("%d %d %d %d calls=%d\n", xs[0], xs[1], xs[2], lt, calls);
}
