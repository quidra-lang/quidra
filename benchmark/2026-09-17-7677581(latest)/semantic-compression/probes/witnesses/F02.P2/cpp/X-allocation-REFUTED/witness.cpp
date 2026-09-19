// Refutation of the class "allocation may occur": a replaced global allocation
// function counts every dynamic allocation the fragment performs.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
static int g_allocs = 0;
void* operator new(std::size_t n) { ++g_allocs; void* p = std::malloc(n); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
std::uint8_t probe() {
// BEGIN PROBE F02.P2
std::uint8_t buf[16];
buf[0] = 1;
return buf[0];
// END PROBE F02.P2
}
int main() { unsigned r = probe(); std::printf("%u allocs=%d\n", r, g_allocs); }
