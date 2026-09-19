// Refutation of "the closure might capture `k` by reference" and of "the closure
// might be heap-allocated". `k` is mutated between the lambda's evaluation and the
// transform; a by-reference capture would change the result, and a replaced global
// operator new counts every dynamic allocation the fragment performs.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <new>
#include <vector>
static int g_allocs = 0;
void* operator new(std::size_t n) { ++g_allocs; void* p = std::malloc(n); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
std::int32_t probe() {
    std::vector<std::int32_t> xs = {1, 2, 3};
    std::int32_t k = 0;
    auto neg = [k](std::int32_t a) { return k - a; };
    k = 1000;                                   // invisible to `neg` if captured by copy
    std::vector<std::int32_t> ys;
    std::ranges::transform(xs, std::back_inserter(ys), neg);
    return ys[0];
}
int main() { std::printf("%d allocs=%d (xs=1, ys>=1; none for the closure)\n", probe(), g_allocs); }
