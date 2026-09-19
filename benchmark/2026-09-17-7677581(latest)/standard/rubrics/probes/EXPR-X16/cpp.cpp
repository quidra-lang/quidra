#include <cstdio>
#include <concepts>
template <class T> concept HasVal = requires(T t) { { t.val() } -> std::same_as<int>; };
template <HasVal T> int get(const T& t){ return t.val(); }
struct C { int val() const { return 7; } };
int main(){ std::printf("X16 %d\n", get(C{})); }
