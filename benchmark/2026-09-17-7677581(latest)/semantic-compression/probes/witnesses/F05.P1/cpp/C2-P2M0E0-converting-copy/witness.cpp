#include <cstdio>
#include <vector>
bool g_escaped = false;
std::vector<int>* g_alias = nullptr;
struct W { W(const std::vector<int>&) { std::puts("converted"); } };
void f(W) { }
int probe() {
// BEGIN PROBE F05.P1
std::vector<int> x{1, 2, 3};
f(x);
return x[0];
// END PROBE F05.P1
}
int main() { int r = probe(); std::printf("%d escaped=%d\n", r, g_escaped); }
