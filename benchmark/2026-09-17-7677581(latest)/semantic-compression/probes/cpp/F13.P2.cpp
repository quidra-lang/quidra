#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>

std::int32_t parse_or_zero(const std::string& s) {
// BEGIN PROBE F13.P2
    std::int32_t n = 0;
    try {
        n = std::stoi(s);
    } catch (const std::exception&) {
    }
    return n;
// END PROBE F13.P2
}

int main() {
    std::printf("%d %d\n", parse_or_zero("7"), parse_or_zero("x"));
}
