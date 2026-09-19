#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

// BEGIN PROBE F17.P1
std::size_t read_all() {
    std::ifstream f("data.txt");
    if (!f) throw std::runtime_error("open failed");
    std::string text{std::istreambuf_iterator<char>(f), {}};
    return text.size();
}
// END PROBE F17.P1

int main() {
    std::printf("%zu\n", read_all());
}
