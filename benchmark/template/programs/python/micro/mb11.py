#!/usr/bin/env python3
"""MB-11 -- Collections: dict, set and list under insert/update/lookup/delete/iterate.

Frozen workload definition: methodology/06_micro_workloads.md section 4, MB-11.
Python's own standard containers, default-constructed: `dict`, `set`, `list`.
No capacity hint, no custom hash, no third-party primitive-collection library,
fresh containers every round. The containers are pinned for the `python`
configuration by the binding table in section 4.12(d) (`dict`, `set`, `list`),
and the `keys` input array by 4.12(a) (`array('q')`); neither is the
implementer's choice.
"""

import sys
import time

from array import array

N = 1000000
R = 3
KM = 500009
SM = 100003
Q = 1000000007
SEED = 20271917


class Lehmer:
    """Park-Miller minimal-standard LCG: state = (48271 * state) mod 2147483647."""

    __slots__ = ("state",)

    def __init__(self, seed):
        self.state = seed

    def next_int(self):
        self.state = (48271 * self.state) % 2147483647
        return self.state


def workload():
    g = Lehmer(SEED)
    keys = array("q", [g.next_int() for _ in range(N)])

    acc = 0
    size1 = found = vsum = mchk = size2 = size3 = lsum = 0
    for r in range(R):
        keys[r] = keys[r] + 1000000

        m = {}
        for i in range(N):
            k = keys[i] % KM
            m[k] = m.get(k, 0) + 1
        size1 = len(m)

        found = 0
        vsum = 0
        for i in range(N):
            k = (keys[i] + 7) % KM
            if k in m:
                found = found + 1
                vsum = vsum + m[k]

        mchk = 0
        for k, v in m.items():
            mchk = (mchk + (k % 1000003) * v) % 1000003

        for i in range(0, N, 2):
            k = keys[i] % KM
            if k in m:
                del m[k]
        size2 = len(m)

        st = set()
        for i in range(N):
            st.add(keys[i] % SM)
        size3 = len(st)

        lst = []
        for i in range(N):
            lst.append(keys[i] % 1000)
        lsum = 0
        for v in lst:
            lsum = lsum + v

        for v in (size1, found, vsum, mchk, size2, size3, lsum):
            acc = (acc * 31 + v) % Q
    return acc, size1, found, vsum, mchk, size2, size3, lsum


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "once"
    if mode == "steady":
        k_count = int(sys.argv[2]) if len(sys.argv) > 2 else 7
        for k in range(k_count):
            start = time.perf_counter_ns()
            result = workload()
            elapsed = time.perf_counter_ns() - start
            print("ITER %d %d" % (k, elapsed), flush=True)
    else:
        result = workload()
    print("MB11 %d %d %d %d %d %d %d %d" % result)


if __name__ == "__main__":
    main()
