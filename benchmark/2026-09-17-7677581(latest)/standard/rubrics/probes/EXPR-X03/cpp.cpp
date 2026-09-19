#include <cstdio>
#include <string>
template <class T> struct Box { T v; T get() const { return v; } };
int main(){ Box<int> a{5}; Box<std::string> b{"hi"};
  std::printf("X03 %d %s\n", a.get(), b.get().c_str()); }
