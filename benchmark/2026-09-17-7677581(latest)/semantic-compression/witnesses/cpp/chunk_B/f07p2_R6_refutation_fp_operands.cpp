// Refutation witness (methodology 03 Rule 1.3.1.e) for F07.P2:
// floating-point OPERANDS are not realizable, because statement (2)'s `%`
// requires integral or unscoped enumeration operands ([expr.mul]/2).
// Expected result: this file FAILS to build under the frozen recipe.
#include <cstdio>
int main() {
    double a = -7.5;
    double b = 2.0;
// BEGIN PROBE F07.P2
    int q = a / b;
    int m = a % b;
    double d = static_cast<double>(a) / b;
// END PROBE F07.P2
    std::printf("%d %d %f\n", q, m, d);
}
