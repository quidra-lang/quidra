#include <cstdio>
#include <vector>
int element(std::vector<int>& xs, int i) { int e = xs[i]; return e; }
int main() { std::vector<int> xs{1,2,3}; std::printf("%d\n", element(xs, 7)); }
