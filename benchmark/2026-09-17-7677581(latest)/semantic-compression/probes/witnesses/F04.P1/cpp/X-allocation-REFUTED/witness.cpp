// Refutation of the class "allocation may occur at the alias fragment".
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
static int g_allocs = 0;
void* operator new(std::size_t n) { ++g_allocs; void* p = std::malloc(n); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
std::int32_t probe() {
// BEGIN PROBE F04.P1
std::int32_t slot = 4;
auto& port = slot;
port = 9;
return slot;
// END PROBE F04.P1
}
int main() { std::printf("%d allocs=%d\n", probe(), g_allocs); }
