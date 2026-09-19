#include <cstdint>
#include <cstdio>
#include <optional>

std::int32_t bump(std::optional<std::int32_t> o) {
// BEGIN PROBE F12.P2
    auto h = [](std::int32_t x) { return x + 1; };
    std::optional<std::int32_t> p;
    if (o) p = h(*o);
    return p.value_or(0);
// END PROBE F12.P2
}

int main() {
    std::printf("%d %d\n", bump(std::nullopt), bump(41));
}
