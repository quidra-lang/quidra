#include "u3.h"
#include <cstdio>
namespace util { R::operator std::int32_t() const { std::printf("user-defined conversion ran\n"); return v * 10; }
R pub_add(std::int32_t a, std::int32_t b) { return R{a + b + 1}; } }
