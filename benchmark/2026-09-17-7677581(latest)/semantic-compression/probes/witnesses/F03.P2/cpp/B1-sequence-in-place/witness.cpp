#include <cstdio>
#include <vector>
int main() {
std::vector<int> xs = {7, 8, 9};
// BEGIN PROBE F03.P2
xs[1] = 42;
// END PROBE F03.P2
std::printf("%d %zu\n", xs[1], xs.size()); }
