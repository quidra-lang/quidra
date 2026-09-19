#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

std::int32_t map_total() {
// BEGIN PROBE F16.P2
std::map<std::string, std::int32_t> mp{{"a", 1}};
std::int32_t total = 0;
for (const auto& e : mp) total += e.second;
std::int32_t miss = mp.contains("b") ? mp.at("b") : 0;
return total + miss;
// END PROBE F16.P2
}

int main() {
    std::printf("%d\n", map_total());
}
