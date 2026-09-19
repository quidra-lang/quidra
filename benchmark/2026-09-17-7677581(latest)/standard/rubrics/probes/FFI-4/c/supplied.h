/* FFI-4 supplied C header. Identical for all 10 languages.
   Header-only so that the frozen single-artifact build commands need no extra
   link step. Declarations must NOT be hand-transcribed by the probe. */
#ifndef SUPPLIED_H
#define SUPPLIED_H

#include <stdint.h>

#define SUPPLIED_SCALE 3

typedef struct SuppliedRecord {
    int32_t count;
    double  weight;
} SuppliedRecord;

static inline int32_t supplied_add(int32_t a, int32_t b) {
    return a + b;
}

static inline double supplied_weighted(const SuppliedRecord *r) {
    return (double)r->count * r->weight;
}

#endif /* SUPPLIED_H */
