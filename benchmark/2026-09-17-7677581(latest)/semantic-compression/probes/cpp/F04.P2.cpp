#include <cstdio>
#include <vector>

int probe() {
    std::vector<int> xs = {4, 5, 6};
// BEGIN PROBE F04.P2
const auto& window = xs;
return window[0];
// END PROBE F04.P2
}

int main() {
    std::printf("%d\n", probe());
}
