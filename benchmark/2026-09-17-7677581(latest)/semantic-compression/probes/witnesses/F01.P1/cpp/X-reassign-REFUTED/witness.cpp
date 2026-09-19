#include <cstdio>
int seven() { const int n = 7; n = 8; return n; }
int main(){ std::printf("%d\n", seven()); }
