#include <cstdint>
#include <cstdio>
#include <future>

std::int32_t concurrent_sum() {
// BEGIN PROBE F19.P1
auto a = std::async(std::launch::async, []{ return 20; });
auto b = std::async(std::launch::async, []{ return 22; });
std::int32_t sum = a.get() + b.get();
return sum;
// END PROBE F19.P1
}

int main() {
    std::printf("%d\n", concurrent_sum());
}
