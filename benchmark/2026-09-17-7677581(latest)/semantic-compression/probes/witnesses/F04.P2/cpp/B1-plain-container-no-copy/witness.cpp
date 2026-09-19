#include <cstdio>
#include <vector>
std::vector<int> xs = {4, 5, 6};
const void* seen = nullptr;
int probe() {
// BEGIN PROBE F04.P2
const auto& window = xs;
seen = &window;
return window[0];
// END PROBE F04.P2
}
int main() { int r = probe(); std::printf("%d same=%d\n", r, (const void*)&xs == seen); }
