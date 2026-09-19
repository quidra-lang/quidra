#include <cstdio>
#include <string>
int main(){ std::u32string s = U"héllo"; size_t n=0; for (char32_t c : s) { (void)c; ++n; }
  printf("X15 %zu\n", n); }
