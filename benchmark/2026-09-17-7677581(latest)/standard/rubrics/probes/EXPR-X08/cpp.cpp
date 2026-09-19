#include <cstdio>
#include <stdexcept>
void inner(){ throw std::runtime_error("boom"); }
void outer(){ inner(); }
int main(){ try { outer(); } catch (const std::exception& e) { std::printf("X08 caught %s\n", e.what()); } }
