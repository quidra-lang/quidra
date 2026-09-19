#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>
static int g_allocs = 0;
bool g_escaped = false;
std::vector<int>* g_alias = nullptr;
void* operator new(std::size_t n) { ++g_allocs; void* p = std::malloc(n); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void f(std::vector<int> v) { std::printf("in-callee allocs=%d\n", g_allocs); v[0] = 99; }
int probe() {
// BEGIN PROBE F05.P1
std::vector<int> x{1, 2, 3};
f(x);
return x[0];
// END PROBE F05.P1
}
int main() { int r = probe(); std::printf("%d allocs=%d escaped=%d\n", r, g_allocs, g_escaped); }
