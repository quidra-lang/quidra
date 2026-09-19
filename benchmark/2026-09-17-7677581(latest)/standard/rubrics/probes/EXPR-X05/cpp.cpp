#include <cstdio>
#include <functional>
std::function<int(int)> make_adder(int n){ return [n](int x){ return x + n; }; }
int main(){ auto f = make_adder(10); std::printf("X05 %d\n", f(5)); }
