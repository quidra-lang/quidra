#include <cstdio>
#include <vector>
#include <unordered_map>
#include <set>
#include <string>
int main(){ std::vector<int> v{1,2,3}; std::unordered_map<std::string,int> m{{"a",1},{"b",2}}; std::set<int> s{1,2,3};
  std::printf("X14 %zu %d %zu\n", v.size(), m["b"], s.size()); }
