#include <cstdio>
#include <vector>
#define xs (std::puts("sub"), zs)
std::vector<int> zs = {4, 5, 6};
int probe() {
// BEGIN PROBE F04.P2
const auto& window = xs;
return window[0];
// END PROBE F04.P2
}
int main() { std::printf("%d\n", probe()); }
