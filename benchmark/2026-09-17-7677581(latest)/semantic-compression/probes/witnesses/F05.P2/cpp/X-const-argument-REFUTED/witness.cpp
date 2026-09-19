// Refutation of "the callee might receive a copy, so the caller's object need
// not be modifiable": a const argument cannot bind to the writable-alias
// parameter at all, which shows the `&` is load-bearing.
#include <cstdint>
#include <cstdio>
std::int32_t probe() {
    auto put = [](std::int32_t& out) { out = 12; };
    const std::int32_t cell = 3;      // the only change from the frozen fragment
    put(cell);
    return cell;
}
int main() { std::printf("%d\n", probe()); }
