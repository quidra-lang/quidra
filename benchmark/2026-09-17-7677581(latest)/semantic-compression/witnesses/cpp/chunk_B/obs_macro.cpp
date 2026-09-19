#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>
#include <new>
#define vector deque
static int allocs = 0;
void* operator new(std::size_t n) { ++allocs; void* p = std::malloc(n); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int mid(const std::vector<int>& v);

int caller() {
// BEGIN PROBE F06.P1
    std::vector<int> xs = {1, 2, 3};
    int y = mid(xs);
    return y;
}

int mid(const std::vector<int>& v) { return v[1]; }
// END PROBE F06.P1

int main() { int r = caller(); std::printf("result=%d allocs=%d\n", r, allocs); }
