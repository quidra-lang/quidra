// Refutation of the class "the storage is zero- or value-initialized".
// Same declaration but WITH the value-initializer, printed in full: the written
// form of the probe is demonstrably not this one.
#include <cstdint>
#include <cstdio>
int main() {
    std::uint8_t zeroed[16] = {};
    int sum = 0;
    for (int i = 0; i < 16; ++i) sum += zeroed[i];
    std::printf("value-initialized sum=%d (the probe's form performs no initialization: [dcl.init]/7)\n", sum);
}
