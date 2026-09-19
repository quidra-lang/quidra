#include <cstdio>
#include <string>

int main() {
    std::string base = "ab";
    std::string s1 = base + "cd";
    std::string s2 = base + "cd";
// BEGIN PROBE F09.P1
    bool eq = s1 == s2;
// END PROBE F09.P1
    std::printf("%d\n", eq);
}
