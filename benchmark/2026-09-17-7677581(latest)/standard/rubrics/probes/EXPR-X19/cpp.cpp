#include <cstdio>
#include <thread>
int main(){ int box = 0; std::thread t([&]{ box = 42; }); t.join(); std::printf("X19 %d\n", box); }
