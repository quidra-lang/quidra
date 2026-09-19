#!/usr/bin/env python3
"""MB-06 -- Matrix multiplication: classical i-j-k, flat row-major, n = 512, R = 3.

Frozen workload definition: methodology/06_micro_workloads.md section 4, MB-06.
The i-j-k loop order is mandatory: no i-k-j, no tiling, no blocking, no
transposition of B, no library matmul. All three matrices are flat
`array('d')` buffers of length n*n -- the binary64 array pinned for the
`python` configuration by the binding table in section 4.12(a), not the
implementer's choice.
"""

import sys
import time

from array import array

N = 512
R = 3
SEED = 20266917


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
    a = array("d", [g.next_unit() for _ in range(N * N)])
    b = array("d", [g.next_unit() for _ in range(N * N)])
    c = array("d", bytes(8 * N * N))

    for r in range(R):
        a[r] = a[r] + 1.0e-9
        for i in range(N):
            for j in range(N):
                s = 0.0
                for k in range(N):
                    s = s + a[i * N + k] * b[k * N + j]
                c[i * N + j] = s

    sum_c = 0.0
    for i in range(N * N):
        sum_c = sum_c + c[i]
    return sum_c, c[0], c[N * N - 1]


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "once"
    if mode == "steady":
        k_count = int(sys.argv[2]) if len(sys.argv) > 2 else 7
        for k in range(k_count):
            start = time.perf_counter_ns()
            sum_c, c_first, c_last = workload()
            elapsed = time.perf_counter_ns() - start
            print("ITER %d %d" % (k, elapsed), flush=True)
    else:
        sum_c, c_first, c_last = workload()
    print("MB06 sumC=%.16e c_first=%.16e c_last=%.16e" % (sum_c, c_first, c_last))


if __name__ == "__main__":
    main()
