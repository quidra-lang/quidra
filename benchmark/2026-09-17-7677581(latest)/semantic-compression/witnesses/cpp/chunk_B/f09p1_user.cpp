#include <cstdio>
#include <string>
static int calls = 0;
struct S { std::string v; };
bool operator==(const S& x, const S& y) { ++calls; std::printf("user op== ran\n"); return x.v.size() == y.v.size(); }
int main() {
    S s1{"abcd"}, s2{"wxyz"};
// BEGIN PROBE F09.P1
    bool eq = s1 == s2;
// END PROBE F09.P1
    std::printf("%d calls=%d\n", eq, calls);
}
