// TEST-1 probe, C++. Exactly 3 tests registered with CTest: two assert a true
// condition, one asserts a false condition.
#include <cstring>
#include <iostream>
#include "probe_lib.hpp"

static int check(const char* name, bool condition) {
  if (condition) {
    std::cout << name << ": assertion held\n";
    return 0;
  }
  std::cout << name << ": assertion FAILED\n";
  return 1;
}

int main(int argc, char** argv) {
  if (argc != 2) { std::cerr << "usage: probe_tests <test-name>\n"; return 2; }
  if (std::strcmp(argv[1], "pass_one") == 0) return check("pass_one", add(1, 1) == 2);
  if (std::strcmp(argv[1], "pass_two") == 0) return check("pass_two", add(2, 3) == 5);
  if (std::strcmp(argv[1], "fail_one") == 0) return check("fail_one", add(1, 1) == 3);
  std::cerr << "unknown test: " << argv[1] << "\n";
  return 2;
}
