#include <concepts>
int main() {
    auto f = []<std::totally_ordered T>(T x) { return x.no_such_member(); };
    (void)f;
}
