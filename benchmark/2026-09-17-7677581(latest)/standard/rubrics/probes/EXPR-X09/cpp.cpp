#include <cstdio>
#include <stdexcept>
struct Res { ~Res(){ std::printf("X09 cleanup "); } };
void f(){ Res r; throw std::runtime_error("boom"); }
int main(){ try { f(); } catch (...) { std::printf("caught\n"); } }
