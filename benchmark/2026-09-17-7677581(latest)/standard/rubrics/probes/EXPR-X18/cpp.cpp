#include <cstdio>
#include <fstream>
#include <filesystem>
#include <string>
int main(){ { std::ofstream o("x18.txt"); o << "hello"; }
  std::string d; { std::ifstream i("x18.txt"); std::getline(i, d); }
  std::printf("X18 %s %s\n", d.c_str(), std::filesystem::exists("x18.txt") ? "true" : "false"); }
