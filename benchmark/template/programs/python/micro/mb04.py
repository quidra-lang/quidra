#!/usr/bin/env python3
"""MB-04 -- Floating-point arithmetic: 4-accumulator FP mix, M = 4000, R = 75000.

Frozen workload definition: methodology/06_micro_workloads.md section 4, MB-04.
Four separate scalar accumulators, accumulated in the pinned order; no Kahan
summation, no partial sums, no reciprocal or rsqrt substitution.
Data container: `array('d')` -- the binary64 array pinned for the `python`
configuration by the binding table in section 4.12(a). Not the implementer's
choice.
"""

import sys
import time

from array import array
from math import sqrt

M = 4000
R = 75000
SEED = 20264917


class Lehmer:
    """Park-Miller minimal-standard LCG: state = (48271 * state) mod 2147483647."""

    __slots__ = ("state",)

    def __init__(self, seed):
        self.state = seed

    def next_int(self):
        self.state = (48271 * self.state) % 2147483647
        return self.state

    def next_unit(self):
        return self.next_int() / 2147483647.0


def workload():
    g = Lehmer(SEED)
    a_vec = array("d", [0.5 + g.next_unit() for _ in range(M)])
    b_vec = array("d", [0.5 + g.next_unit() for _ in range(M)])

    s1 = 0.0
    s2 = 0.0
    s3 = 0.0
    s4 = 0.0
    for r in range(R):
        a_vec[r % M] = a_vec[r % M] + 1.0e-9
        for i in range(M):
            a = a_vec[i]
            b = b_vec[i]
            s1 = s1 + a * b
            s2 = s2 + a / (b + 2.0)
            s3 = s3 + sqrt(a * a + b * b)
            s4 = s4 + (a - b) * (a - b)
    return s1, s2, s3, s4


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "once"
    if mode == "steady":
        k_count = int(sys.argv[2]) if len(sys.argv) > 2 else 7
        for k in range(k_count):
            start = time.perf_counter_ns()
            s1, s2, s3, s4 = workload()
            elapsed = time.perf_counter_ns() - start
            print("ITER %d %d" % (k, elapsed), flush=True)
    else:
        s1, s2, s3, s4 = workload()
    print("MB04 s1=%.16e s2=%.16e s3=%.16e s4=%.16e" % (s1, s2, s3, s4))


if __name__ == "__main__":
    main()
