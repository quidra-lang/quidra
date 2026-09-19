#include <climits>
#include <cstdio>

int overflow_at_max() {
// BEGIN PROBE F08.P1
    int m = INT_MAX;
    int o = m + 1;
    return o;
// END PROBE F08.P1
}

int main() { std::printf("%d\n", overflow_at_max()); }
