#include <cstdio>
#include <cstddef>
struct M { mutable int hits = 0; int v = 4;
  const int& operator[](std::size_t) const { ++hits; return v; } };
M xs;
int probe() {
// BEGIN PROBE F04.P2
const auto& window = xs;
return window[0];
// END PROBE F04.P2
}
int main() { int r = probe(); std::printf("%d hits=%d\n", r, xs.hits); }
