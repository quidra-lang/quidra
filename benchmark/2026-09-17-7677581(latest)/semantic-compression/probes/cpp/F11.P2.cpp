#include <cstdio>
#include <span>
#include <vector>

int second(std::vector<int>& xs) {
// BEGIN PROBE F11.P2
    std::span part{xs.begin() + 1, 3};
    return part[0];
// END PROBE F11.P2
}

int main() {
    std::vector<int> xs{10, 20, 30, 40, 50};
    std::printf("%d\n", second(xs));
}
