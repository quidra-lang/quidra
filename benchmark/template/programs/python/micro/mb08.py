#!/usr/bin/env python3
"""MB-08 -- Strings: build NW = 200000 words, then 5 character passes, R = 20.

Frozen workload definition: methodology/06_micro_workloads.md section 4, MB-08.
Every per-round pass is the explicit character loop the pseudocode writes: no
`upper()`, no slice reversal, no `count()`/`find()`/regex for `cnt_ab`, no
`split()` for `cnt_w`, no caching of the hashes, fresh buffers each round.
Representation, pinned for the `python` configuration by the binding table in
section 4.12(b) and not the implementer's choice: passes 1-4 operate on a
`bytearray` working buffer; pass 5 operates on a native `str` value built fresh
from that buffer inside every timed round and read with `s[i]`, never as a byte
view. Every pass is written as the explicit indexed character loop of the
pinned pseudocode.
"""

import sys
import time

NW = 200000
R = 20
Q = 1000000007
SEED = 20268917

SPACE = ord(" ")
LOWER_A = ord("a")
LOWER_B = ord("b")
LOWER_Z = ord("z")
LOWER_X = ord("x")


class Lehmer:
    """Park-Miller minimal-standard LCG: state = (48271 * state) mod 2147483647."""

    __slots__ = ("state",)

    def __init__(self, seed):
        self.state = seed

    def next_int(self):
        self.state = (48271 * self.state) % 2147483647
        return self.state


def rhash(seq, length):
    """Order-sensitive rolling hash over a sequence of character codes."""
    h = 0
    for i in range(length):
        h = (h * 131 + seq[i]) % Q
    return h


def workload():
    g = Lehmer(SEED)
    words = []
    for _ in range(NW):
        length = 4 + g.next_int() % 13
        words.append(bytes(LOWER_A + g.next_int() % 26 for _ in range(length)))
    text = bytearray(b" ".join(words))
    n = len(text)

    acc = 0
    cnt_ab = 0
    cnt_w = 0
    for r in range(R):
        p = 7 * r + 11
        if text[p] == SPACE:
            text[p] = LOWER_X
        else:
            text[p] = LOWER_A + (text[p] - LOWER_A + 1) % 26

        h1 = rhash(text, n)

        upper = bytearray(n)
        for i in range(n):
            c = text[i]
            upper[i] = c - 32 if LOWER_A <= c <= LOWER_Z else c
        h2 = rhash(upper, n)

        rev = bytearray(n)
        for i in range(n):
            rev[i] = text[n - 1 - i]
        h3 = rhash(rev, n)

        cnt_ab = 0
        for i in range(n - 1):
            if text[i] == LOWER_A and text[i + 1] == LOWER_B:
                cnt_ab = cnt_ab + 1

        # Pass 5 runs on the language's own string type, constructed fresh from
        # the working buffer inside the timed round (section 4.12(b) pins `str`
        # and `s[i]` for python); it is never a second scan of the byte buffer.
        text_str = text.decode("ascii")
        cnt_w = 1
        for i in range(n):
            if text_str[i] == " ":
                cnt_w = cnt_w + 1

        for v in (h1, h2, h3, cnt_ab, cnt_w):
            acc = (acc * 31 + v) % Q
    return acc, n, cnt_ab, cnt_w


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "once"
    if mode == "steady":
        k_count = int(sys.argv[2]) if len(sys.argv) > 2 else 7
        for k in range(k_count):
            start = time.perf_counter_ns()
            acc, n, cnt_ab, cnt_w = workload()
            elapsed = time.perf_counter_ns() - start
            print("ITER %d %d" % (k, elapsed), flush=True)
    else:
        acc, n, cnt_ab, cnt_w = workload()
    print("MB08 %d %d %d %d" % (acc, n, cnt_ab, cnt_w))


if __name__ == "__main__":
    main()
