#include <iostream>
#include "mod_a.hpp"
#include "mod_b.hpp"
int main() {
  if (scale(2) != 6) { std::cout << "scale failed\n"; return 1; }
  if (scale_and_offset(2) != 7) { std::cout << "scale_and_offset failed\n"; return 1; }
  std::cout << "project tests passed\n";
  return 0;
}
