#include <cstdio>
#include <span>
#include <array>
int main(){ std::array<int,4> buf{0,0,0,0}; std::span<int> view(buf.data()+1, 2);
  view[0] = 99; view[1] = 99; std::printf("X23 %d %d\n", buf[1], buf[2]); }
