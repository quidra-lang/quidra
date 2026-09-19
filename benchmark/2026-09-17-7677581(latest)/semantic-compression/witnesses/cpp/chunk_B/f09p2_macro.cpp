#include <algorithm>
#include <cstdio>
#include <functional>
#include <vector>
#define greater less
int main() {
    std::vector<int> xs = {3, 1, 2};
    int a = 1;
    int b = 2;
// BEGIN PROBE F09.P2
    std::ranges::sort(xs, std::greater{});
    bool lt = a < b;
// END PROBE F09.P2
    std::printf("%d %d %d %d\n", xs[0], xs[1], xs[2], lt);
}
