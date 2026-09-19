#include <cstdio>
#include <vector>

int probe() {
    std::vector<int> xs = {7, 8, 9};
// BEGIN PROBE F03.P2
xs[1] = 42;
// END PROBE F03.P2
    return xs[1];
}

int main() {
    std::printf("%d\n", probe());
}
