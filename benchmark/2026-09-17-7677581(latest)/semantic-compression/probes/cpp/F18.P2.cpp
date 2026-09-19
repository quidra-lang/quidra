#include <cstdint>
#include <cstdio>

// BEGIN PROBE F18.P2
#include "util.h"

int main() {
    std::int32_t result = util::pub_add(2, 3);
// END PROBE F18.P2
    std::printf("%d\n", result);
}
