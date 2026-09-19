#include <optional>
#include <cstdint>
int main() {
    std::optional<std::int32_t> o = std::nullopt;
    auto h = [](std::int32_t x) { return x + 1; };
    auto p = o.and_then(h);
    return p.value_or(0);
}
