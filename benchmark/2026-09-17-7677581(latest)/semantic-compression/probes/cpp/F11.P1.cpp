#include <cstdio>
#include <vector>

int element(int i) {
    std::vector<int> xs{1, 2, 3};
// BEGIN PROBE F11.P1
    int e = xs[i];
    return e;
// END PROBE F11.P1
}

int main() {
    std::printf("%d\n", element(2));
}
