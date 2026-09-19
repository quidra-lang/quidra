#include <cstdio>
#include <regex>
#include <string>
int main(){ std::string s = "date 2026-09-17"; std::regex re(R"((\d{4})-(\d{2}))"); std::smatch m;
  if (std::regex_search(s, m, re)) std::printf("X21 %s %s\n", m[1].str().c_str(), m[2].str().c_str()); }
