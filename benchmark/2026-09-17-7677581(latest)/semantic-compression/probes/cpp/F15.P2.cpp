#include <concepts>
#include <cstdint>
#include <cstdio>

std::int32_t biggest() {
// BEGIN PROBE F15.P2
    auto max_of = []<std::totally_ordered T>(T x, T y) { return x < y ? y : x; };
    std::int32_t m = max_of(3, 5);
    return m;
// END PROBE F15.P2
}

int main() {
    std::printf("%d\n", biggest());
}
