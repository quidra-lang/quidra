#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

std::int64_t shared_total() {
// BEGIN PROBE F19.P2
std::atomic_int64_t counter{0};
auto bump = [&]{ for (int i = 0; i < 1000; ++i) ++counter; };
std::thread t1(bump);
std::thread t2(bump);
t1.join();
t2.join();
return counter;
// END PROBE F19.P2
}

int main() {
    std::printf("%lld\n", shared_total());
}
