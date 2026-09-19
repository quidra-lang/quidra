#include <cstdio>
#define DEFINE_REC(Name, A, B) \
  struct Name { int A; int B; static constexpr int field_count = 2; }
DEFINE_REC(Rec, a, b);
int main(){ Rec r{1,2}; (void)r; std::printf("X12 meta %d\n", Rec::field_count); }
