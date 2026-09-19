#!/usr/bin/env python3
"""MB-10 -- File I/O: write and read back 3 text files of 1000000 lines each.

Frozen workload definition: methodology/06_micro_workloads.md section 4, MB-10.
Normal buffered standard-library text I/O only: `open()` in text mode, one
`write()` per line, iteration over the file object for the read-back. No mmap,
no raw write(2) loop, no single bulk write of the whole file, no skipping the
read-back. The files are left in the working directory for the harness.
Writer/reader and buffer size are pinned for the `python` configuration by the
binding table in section 4.12(c): `open(name, "w", buffering=65536)` + `write`,
and `open(name, "r", buffering=65536)` iterated as a file object. The
65536-byte buffer is frozen for every configuration and is not a tuning choice.
"""

import sys
import time

N = 1000000
R = 3
SEED = 20270917
BUFSIZE = 65536          # frozen for every configuration by section 4.12(c)


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
    sum_v = 0
    chk = 0
    nbytes = 0
    lines = 0

    for r in range(R):
        name = "mb10_round_%d.txt" % r

        with open(name, "w", encoding="ascii", buffering=BUFSIZE) as out:
            for i in range(N):
                v = g.next_int()
                line = "%d %d\n" % (i, v)
                out.write(line)
                nbytes = nbytes + len(line)

        idx = 0
        with open(name, "r", encoding="ascii", buffering=BUFSIZE) as inp:
            for line in inp:
                field0, field1 = line.split(" ")
                a = int(field0)
                v = int(field1)
                if a != idx:
                    sys.exit("MB10: line index mismatch: %d != %d" % (a, idx))
                idx = idx + 1
                lines = lines + 1
                sum_v = (sum_v + v) % 1000000007
                chk = (chk * 31 + v % 1000003) % 1000003
    return sum_v, chk, nbytes, lines


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "once"
    if mode == "steady":
        k_count = int(sys.argv[2]) if len(sys.argv) > 2 else 7
        for k in range(k_count):
            start = time.perf_counter_ns()
            sum_v, chk, nbytes, lines = workload()
            elapsed = time.perf_counter_ns() - start
            print("ITER %d %d" % (k, elapsed), flush=True)
    else:
        sum_v, chk, nbytes, lines = workload()
    print("MB10 %d %d %d %d" % (sum_v, chk, nbytes, lines))


if __name__ == "__main__":
    main()
