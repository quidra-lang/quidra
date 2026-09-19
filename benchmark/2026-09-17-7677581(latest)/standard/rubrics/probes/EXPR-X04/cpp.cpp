#include <cstdio>
#include <memory>
#include <string>
struct Speaker { virtual std::string speak() const = 0; virtual ~Speaker() = default; };
struct Dog : Speaker { std::string speak() const override { return "woof"; } };
struct Cat : Speaker { std::string speak() const override { return "meow"; } };
std::unique_ptr<Speaker> pick(int n){ if(n==0) return std::make_unique<Dog>(); return std::make_unique<Cat>(); }
int main(){ const char* s="01"; auto a=pick(s[0]-48); auto b=pick(s[1]-48);
  std::printf("X04 %s %s\n", a->speak().c_str(), b->speak().c_str()); }
