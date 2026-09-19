#include <cstdio>
#include <cstdint>
#include <string>
std::int32_t parse_twice(const std::string& s) { return 2 * std::stoi(s); }
int main() { std::printf("%d\n", parse_twice("zz")); }
