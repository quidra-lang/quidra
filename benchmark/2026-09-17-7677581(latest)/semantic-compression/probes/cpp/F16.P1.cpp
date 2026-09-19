#include <cstdint>
#include <cstdio>
#include <numeric>
#include <vector>

std::int32_t sum_sequence() {
// BEGIN PROBE F16.P1
std::vector<std::int32_t> xs{1, 2, 3};
std::int32_t total = std::reduce(xs.begin(), xs.end());
return total;
// END PROBE F16.P1
}

int main() {
    std::printf("%d\n", sum_sequence());
}
