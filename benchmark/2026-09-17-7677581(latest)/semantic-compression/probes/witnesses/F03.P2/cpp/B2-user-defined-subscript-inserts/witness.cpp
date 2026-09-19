#include <cstdio>
#include <map>
int main() {
std::map<int,int> xs;
// BEGIN PROBE F03.P2
xs[1] = 42;
// END PROBE F03.P2
std::printf("%d %zu\n", xs[1], xs.size()); }
