#include <cstdint>
#include <cstdio>
#include <string>

// BEGIN PROBE F13.P1
std::int32_t parse_twice(const std::string& s) {
    return 2 * std::stoi(s);
}
// END PROBE F13.P1

int main() {
    std::printf("%d\n", parse_twice("21"));
}
