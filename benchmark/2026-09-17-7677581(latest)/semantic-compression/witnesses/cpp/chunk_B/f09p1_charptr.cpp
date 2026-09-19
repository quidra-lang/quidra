#include <cstdio>
#include <cstring>
int main() {
    char buf1[] = "abcd";
    char buf2[] = "abcd";
    const char* s1 = buf1;
    const char* s2 = buf2;
// BEGIN PROBE F09.P1
    bool eq = s1 == s2;
// END PROBE F09.P1
    std::printf("%d (strcmp=%d)\n", eq, std::strcmp(s1, s2));
}
