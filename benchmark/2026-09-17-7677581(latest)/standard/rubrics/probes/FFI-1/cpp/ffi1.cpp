#include <cstdio>
extern "C" double cos(double);
int main() { std::printf("cos(1.0)=%.10f\n", cos(1.0)); return 0; }
