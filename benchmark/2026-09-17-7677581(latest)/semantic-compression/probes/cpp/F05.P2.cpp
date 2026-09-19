#include <cstdint>
#include <cstdio>

std::int32_t probe() {
// BEGIN PROBE F05.P2
auto put = [](std::int32_t& out) { out = 12; };
std::int32_t cell = 3;
put(cell);
return cell;
// END PROBE F05.P2
}

int main() {
    std::printf("%d\n", probe());
}
