#include <cstdio>
#include <cmath>
#include <limits>
int main(){ static_assert(std::numeric_limits<double>::is_iec559, "IEEE-754");
  std::printf("X17 %.0f %.0f %.0f %.0f %.0f\n", std::sqrt(4.0), std::exp(0.0), std::log(1.0), std::sin(0.0), std::cos(0.0)); }
