#include <cstdint>
#include <cstdio>
#include <numeric>
#include <vector>
#define total 99
std::int32_t sum_sequence() {
std::vector<std::int32_t> xs{1, 2, 3};
std::int32_t total = std::reduce(xs.begin(), xs.end());
return total;
}
int main() { std::printf("%d\n", sum_sequence()); }
