#include <cstdio>
#include <vector>
bool g_escaped = false;
std::vector<int>* g_alias = nullptr;
void f(std::vector<int>& v) { g_alias = &v; g_escaped = true; v[0] = 77; }
int probe() {
// BEGIN PROBE F05.P1
std::vector<int> x{1, 2, 3};
f(x);
return x[0];
// END PROBE F05.P1
}
int main() { int r = probe(); std::printf("%d escaped=%d\n", r, g_escaped); }
