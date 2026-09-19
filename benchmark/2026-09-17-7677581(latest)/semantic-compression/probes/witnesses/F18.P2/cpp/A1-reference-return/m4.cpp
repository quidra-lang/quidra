#include <cstdint>
#include <cstdio>
#include "u4.h"
int main() { std::int32_t result = util::pub_add(2, 3); util::pub_add(10, 10) = 99; std::printf("%d %d\n", result, util::pub_add(0,0)); }
