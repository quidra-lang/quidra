#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>

extern "C" {
struct Record { int32_t count; double weight; };

static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}
static int cmp_record(const void* a, const void* b) {
    int32_t x = ((const Record*)a)->count, y = ((const Record*)b)->count;
    return (x > y) - (x < y);
}
}

int main() {
    std::printf("sizeof=%zu offset=%zu\n", sizeof(Record), offsetof(Record, weight));
    double xs[5] = {3.5, 1.25, 4.75, 1.5, 2.25};
    std::qsort(xs, 5, sizeof(double), cmp_double);
    for (int i = 0; i < 5; i++) std::printf(i ? " %.2f" : "%.2f", xs[i]);
    std::printf("\n");
    Record rs[3] = {{3, 1.5}, {1, 4.0}, {2, 2.5}};
    std::qsort(rs, 3, sizeof(Record), cmp_record);
    for (int i = 0; i < 3; i++) std::printf(i ? " %d:%.2f" : "%d:%.2f", rs[i].count, rs[i].weight);
    std::printf("\n");
    return 0;
}
