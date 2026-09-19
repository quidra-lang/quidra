#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
struct Node { std::int32_t id; Node(std::int32_t i) : id(i) { std::puts("ctor"); } ~Node() { std::puts("dtor"); } };
bool probe() {
// BEGIN PROBE F04.P3
auto first = std::make_shared<Node>(5);
auto second = std::array{first}[0];
bool same = first == second;
return same;
// END PROBE F04.P3
}
int main() { std::printf("%d\n", probe()); }
