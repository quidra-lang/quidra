#include <cstdio>
#include <cstdint>

struct A { std::int64_t v; };
struct B { std::int64_t v; };

int main() {
    printf("ADV-START\n"); fflush(stdout);
    A a{42};
    void* p = &a;
    B* b = static_cast<B*>(p);
    printf("OBS=V:%lld\n", (long long)b->v); fflush(stdout);
    printf("ADV-END\n"); fflush(stdout);
    return 0;
}
