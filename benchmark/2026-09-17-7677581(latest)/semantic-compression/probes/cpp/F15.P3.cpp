#include <cstdio>
#include <string>
#include <vector>

std::string first_tag() {
// BEGIN PROBE F15.P3
    struct Named { virtual std::string tag() const = 0; };
    struct A : Named { std::string tag() const override { return "a"; } };
    struct B : Named { std::string tag() const override { return "b"; } };
    A a;
    B b;
    std::vector<Named*> items{&a, &b};
    return items[0]->tag();
// END PROBE F15.P3
}

int main() {
    std::printf("%s\n", first_tag().c_str());
}
