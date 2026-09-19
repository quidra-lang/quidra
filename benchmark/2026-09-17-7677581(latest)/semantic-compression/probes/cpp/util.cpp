#include "util.h"

namespace util {
namespace {
std::int32_t secret() { return 1; }
}
std::int32_t pub_add(std::int32_t a, std::int32_t b) { return a + b + secret(); }
}
