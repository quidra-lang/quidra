#include <cstdio>
#include <vector>
bool g_escaped = false;
std::vector<int>* g_alias = nullptr;
void f(std::vector<int> v) { (void)v; }
// C++ has no by-name / lazy parameter form; [expr.call]/8 sequences argument
// initialisation before the body and the body always runs. Nothing can defer it.
int probe() {
// BEGIN PROBE F05.P1
std::vector<int> x{1, 2, 3};
f(x);
return x[0];
// END PROBE F05.P1
}
int main() { int r = probe(); std::printf("%d escaped=%d\n", r, g_escaped); }
