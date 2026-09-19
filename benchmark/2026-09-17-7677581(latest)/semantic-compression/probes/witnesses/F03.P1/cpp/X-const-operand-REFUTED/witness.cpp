// Refutation witness for the K02 row at `x++;`: a non-modifiable operand makes
// the same fragment ill-formed, so the form itself carries the mutability fact.
#include <cstdint>
#include <cstdio>
std::int32_t probe() {
    const std::int32_t x = 41;
// BEGIN PROBE F03.P1
x++;
// END PROBE F03.P1
    return x;
}
int main() { std::printf("%d\n", probe()); }
