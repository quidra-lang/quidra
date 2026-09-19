#include <cstdint>
#include <cstdio>
#include <optional>

std::int32_t fallback() {
// BEGIN PROBE F12.P1
    std::optional<std::int32_t> o = std::nullopt;
    std::int32_t n = o.value_or(0);
    return n;
// END PROBE F12.P1
}

int main() {
    std::printf("%d\n", fallback());
}
