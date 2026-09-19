#include <cstdio>
#include <vector>
#define xs (std::puts("sub"), zs)
int main() {
std::vector<int> zs = {7, 8, 9};
// BEGIN PROBE F03.P2
xs[1] = 42;
// END PROBE F03.P2
std::printf("%d\n", zs[1]); }
