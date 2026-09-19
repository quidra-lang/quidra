#!/usr/bin/env python3
"""Preflight check (a): the pack's worked example IS the fixture, it builds, and the
output the pack claims is byte-identical to what it actually produces."""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from validate import build_and_run, gate  # noqa: E402
from inverse import inverse  # noqa: E402


def blocks(md):
    return re.findall(r"```\n(.*?)```", md, re.S)


def main():
    pack = open(os.path.join(HERE, "reference_pack.md"), encoding="utf-8").read()
    anon = open(os.path.join(HERE, "fixture_anon.java"), encoding="utf-8").read()
    real = open(os.path.join(HERE, "fixture_real.java"), encoding="utf-8").read()
    expected = open(os.path.join(HERE, "expected_output.txt"), encoding="utf-8").read()

    bs = blocks(pack)
    example = bs[-2]
    claimed = bs[-1]
    fails = []

    same = (example == anon)
    print("  a1 pack P12 example is byte-identical to fixture_anon.java : %s" % same)
    fails += [] if same else ["a1"]

    same2 = (claimed == expected)
    print("  a2 pack's claimed output is byte-identical to expected_output.txt : %s" % same2)
    fails += [] if same2 else ["a2"]

    ok, ev = gate(example)
    print("  a3 pack P12 example passes the I1 conformance gate : %s %s" % (ok, ev))
    fails += [] if ok else ["a3"]

    back = inverse(example)
    same3 = (back == real)
    print("  a4 inverse(pack P12 example) is byte-identical to fixture_real.java : %s" % same3)
    fails += [] if same3 else ["a4"]

    built, ran, out, diag = build_and_run(back)
    print("  a5 it builds : %s   exit 0 on run : %s   stderr : %r" % (built, ran, diag))
    fails += [] if (built and ran and not diag) else ["a5"]

    same4 = (out == expected)
    print("  a6 its stdout is byte-identical to the pack's claim : %s" % same4)
    fails += [] if same4 else ["a6"]
    print("     produced:")
    for line in out.splitlines():
        print("       | %s" % line)

    print()
    if fails:
        print("PACK CHECK FAILED:", fails)
        return 1
    print("PACK CHECK PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
