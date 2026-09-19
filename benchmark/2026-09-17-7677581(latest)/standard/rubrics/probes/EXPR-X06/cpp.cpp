#include <cstdio>
#include <tuple>
int main(){ std::tuple<int,int> pair{1,2}; auto [a,b] = pair; std::printf("X06 %d %d\n", a, b); }
