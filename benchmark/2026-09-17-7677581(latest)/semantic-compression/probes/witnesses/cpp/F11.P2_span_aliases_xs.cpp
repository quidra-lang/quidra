#include <cstdio>
#include <span>
#include <vector>
int main() {
    std::vector<int> xs{10, 20, 30, 40, 50};
    std::span part{xs.begin() + 1, 3};
    xs[1] = 99;
    std::printf("%d\n", part[0]);
}
