#include <concepts>
#include <cstdio>
struct NotOrdered {};
int main() {
    auto max_of = []<std::totally_ordered T>(T x, T y) { return x < y ? y : x; };
    auto m = max_of(NotOrdered{}, NotOrdered{});
    (void)m;
}
