#include <cstdio>
#include <utility>

std::pair<int, int> divmod2(int a, int b);

int caller() {
// BEGIN PROBE F06.P2
    auto [q, r] = divmod2(7, 3);
    return q + r;
}

std::pair<int, int> divmod2(int a, int b) { return {a / b, a % b}; }
// END PROBE F06.P2

int main() { std::printf("%d\n", caller()); }
