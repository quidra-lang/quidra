// Refutation of the class "a write through `window` would be accepted".
#include <cstdio>
#include <vector>
int probe() {
    std::vector<int> xs = {4, 5, 6};
    const auto& window = xs;
    window[0] = 1;              // the only change from the frozen fragment
    return window[0];
}
int main() { std::printf("%d\n", probe()); }
