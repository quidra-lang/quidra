#include "util.h"
#include <cstdio>
namespace util { std::int32_t secret(); }
int main() { std::printf("%d\n", util::secret()); }
