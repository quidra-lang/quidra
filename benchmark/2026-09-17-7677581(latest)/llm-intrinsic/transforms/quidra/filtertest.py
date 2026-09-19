#!/usr/bin/env python3
"""
Self-test for the seven acceptance filters of genmap.accept (methodology 10
section 5.3). Each filter is shown firing on a candidate that only that filter
rejects, and a clean candidate is shown being accepted. Without this, filters
that never fire for one seed would be untested code.
"""

import genmap

CASES = [
    ("1 shape (too short)", "abc", set()),
    ("1 shape (uppercase)", "Bakodi", set()),
    ("2 already used", "kimeke", {"kimeke"}),
    ("3 natural-language word", "banana", set()),
    ("4 programming term", "lambda", set()),
    ("5 reserved word of some language", "sizeof", set()),
    ("6 contains a listed word", "zonevu", set()),
]

ok = True
for label, cand, used in CASES:
    rejected = not genmap.accept(cand, used)
    print("%-36s %-8s %s" % (label, cand, "REJECTED" if rejected else "ACCEPTED (BAD)"))
    ok = ok and rejected

# filter 7: the candidate occurs somewhere in the task material
saved = genmap.MATERIAL
genmap.MATERIAL = saved + "\nvodumi\n"
rejected = not genmap.accept("vodumi", set())
print("%-36s %-8s %s" % ("7 occurs in task material", "vodumi", "REJECTED" if rejected else "ACCEPTED (BAD)"))
ok = ok and rejected
genmap.MATERIAL = saved

clean = genmap.accept("vodumi", set())
print("%-36s %-8s %s" % ("clean candidate", "vodumi", "ACCEPTED" if clean else "REJECTED (BAD)"))
ok = ok and clean

print("FILTER SELF-TEST:", "PASS" if ok else "FAIL")
raise SystemExit(0 if ok else 1)
