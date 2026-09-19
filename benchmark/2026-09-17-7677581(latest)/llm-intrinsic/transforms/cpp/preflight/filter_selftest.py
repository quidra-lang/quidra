#!/usr/bin/env python3
"""
PF-08c: every rejection filter of accept() (methodology 10 §5.3) can fire.

Each candidate below is chosen so that the NAMED filter is the first one it trips.
Where the frozen lists subsume a filter -- nothing can reach filter 7 without
tripping 3-6 first -- the filter is exercised by planting the candidate in that
filter's own set. That is a test of the filter, not of the list.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gen_mapping as g  # noqa: E402

LABEL = {
    "1": "shape ^[a-z]{6}$",
    "2": "already assigned in this mapping",
    "3": "natural-language word list",
    "4": "programming-term list",
    "5": "reserved words of the ten languages",
    "6": "contains a list entry of length >= 4 as a substring",
    "7": "occurs in this column's task material",
}


def fire(expect, w, used=None, plant=None):
    if plant:
        g.TASK_WORDS.add(plant)
    before = dict(g.FILTER_HITS)
    r = g.accept(w, used or set())
    fired = [k for k in sorted(g.FILTER_HITS) if g.FILTER_HITS[k] > before[k]]
    ok = (fired == [expect]) and (r is False)
    print("  filter %s  %-45s candidate %-8s accepted=%-5s fired=%s  %s"
          % (expect, LABEL[expect], w, r, fired, "OK" if ok else "UNEXPECTED"))
    return ok


def main():
    results = [
        fire("1", "toolong"),
        fire("2", "riguma", used={"riguma"}),
        fire("3", "better"),
        fire("4", "buffer"),
        fire("5", "unsafe"),
        fire("6", "datave"),
        fire("7", "vizeka", plant="vizeka"),
    ]
    print("  all seven filters fired as expected:", all(results))
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
