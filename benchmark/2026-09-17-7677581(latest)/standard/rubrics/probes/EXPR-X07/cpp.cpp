#include <cstdio>
struct Upto { int n;
  struct It { int i; int operator*() const { return i; } void operator++(){ ++i; }
              bool operator!=(const It& o) const { return i != o.i; } };
  It begin() const { return It{0}; } It end() const { return It{n}; } };
int main(){ std::printf("X07"); for (int v : Upto{3}) std::printf(" %d", v); std::printf("\n"); }
