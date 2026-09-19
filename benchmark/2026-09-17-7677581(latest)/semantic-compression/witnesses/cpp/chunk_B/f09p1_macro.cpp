#include <cstdio>
#include <string>
int main() {
    std::string s1 = "ab";
    std::string s2 = "zz";
#define s2 s1
// BEGIN PROBE F09.P1
    bool eq = s1 == s2;
// END PROBE F09.P1
#undef s2
    std::printf("%d\n", eq);
}
