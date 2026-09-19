#include <cstdio>
#include <string>
#include <vector>

std::string combine() {
// BEGIN PROBE F15.P1
    auto head = []<typename T>(const std::vector<T>& xs) -> T { return xs[0]; };
    auto a = head(std::vector<int>{4, 5, 6});
    auto b = head(std::vector<std::string>{"p", "q"});
    return std::to_string(a) + b;
// END PROBE F15.P1
}

int main() {
    std::printf("%s\n", combine().c_str());
}
