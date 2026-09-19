#include <cstdio>
#include <cstdint>
#include <exception>
#include <string>
std::int32_t parse_or_zero(const char* s) {
    std::int32_t n = 0;
    try {
        n = std::stoi(s);
    } catch (const std::exception&) {
    }
    return n;
}
int main() { std::printf("%d\n", parse_or_zero("7")); }
