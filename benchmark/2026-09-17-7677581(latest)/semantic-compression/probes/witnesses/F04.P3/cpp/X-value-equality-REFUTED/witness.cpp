// Refutation of the class "the identity test might be a value comparison".
// Node declares no operator==, so `*first == *second` is ill-formed; and a
// field-by-field test succeeds for two DISTINCT objects, which the frozen
// fragment's `first == second` does not.
#include <cstdint>
#include <cstdio>
#include <memory>
struct Node { std::int32_t id; };
int main() {
    auto a = std::make_shared<Node>(5);
    auto b = std::make_shared<Node>(5);   // a DIFFERENT object with an equal field
    std::printf("identity=%d field_equality=%d\n", a == b, a->id == b->id);
    // The next line is the ill-formed one; uncomment to reproduce the error:
    // bool v = *a == *b;
}
