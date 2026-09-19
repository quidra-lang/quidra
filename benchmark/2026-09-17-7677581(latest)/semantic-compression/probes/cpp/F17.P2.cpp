#include <cstdint>
#include <cstdio>

std::int32_t released = 0;

std::int32_t cleanup_count() {
// BEGIN PROBE F17.P2
struct Handle {
    std::int32_t id;
    ~Handle() { ++released; }
};
{
    Handle h{1};
}
return released;
// END PROBE F17.P2
}

int main() {
    std::printf("%d\n", cleanup_count());
}
