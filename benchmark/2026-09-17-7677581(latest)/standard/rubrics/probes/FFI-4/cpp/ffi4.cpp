#include <cstdio>
#include "supplied.h"   /* declarations consumed mechanically; none hand-written */
int main() {
    SuppliedRecord r; r.count = 3; r.weight = 1.5;
    std::printf("supplied_add=%d\n", supplied_add(20, 22));
    std::printf("supplied_weighted=%.2f\n", supplied_weighted(&r));
    std::printf("SUPPLIED_SCALE=%d\n", SUPPLIED_SCALE);
    return 0;
}
