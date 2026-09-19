#include <cstdio>
namespace my { template<class A, class B> struct pair { A first; B second;
  pair(A a, B b) : first(a), second(b) { std::printf("my::pair ctor ran\n"); } }; }
#define std my
std::pair<int, int> divmod2(int a, int b);
#undef std

int caller() {
#define std my
// BEGIN PROBE F06.P2
    auto [q, r] = divmod2(7, 3);
    return q + r;
}

std::pair<int, int> divmod2(int a, int b) { return {a / b, a % b}; }
// END PROBE F06.P2
#undef std

int main() { std::printf("%d\n", caller()); }
