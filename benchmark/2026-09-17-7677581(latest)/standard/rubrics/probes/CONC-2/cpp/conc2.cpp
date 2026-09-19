// CONC-2: two concurrent workers increment a shared counter with NO synchronization.
#include <cstdio>
#include <thread>

static const int ITERS = 200000;
static long counter = 0;

static void bump() { for (int i = 0; i < ITERS; i++) counter = counter + 1; }

int main() {
    std::thread t1(bump), t2(bump);
    t1.join(); t2.join();
    std::printf("counter=%ld expected=%d\n", counter, 2 * ITERS);
    return 0;
}
