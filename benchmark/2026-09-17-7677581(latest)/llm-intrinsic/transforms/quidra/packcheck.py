#!/usr/bin/env python3
"""
Pre-flight check (a): the worked fixture the Reference Pack shows is byte-for-byte
the anonymized fixture on disk, its inverse image is byte-for-byte the real
fixture, it builds with the frozen recipe, and the output it actually produces is
byte-identical to the output the pack claims it produces.

Usage: python3 packcheck.py
"""

import os
import re
import subprocess
import sys
import tempfile

from inverse import inverse

HERE = os.path.dirname(os.path.abspath(__file__))
QUIDRA = os.environ.get("QUIDRA_BIN", "/Users/koba/Desktop/Quidra/quidra/build/quidra")

ok = True


def check(name, cond, detail=""):
    global ok
    ok = ok and bool(cond)
    print("%-22s %s%s" % (name, "PASS" if cond else "FAIL", ("  " + detail) if detail else ""))


pack = open(os.path.join(HERE, "reference_pack.md"), encoding="utf-8").read()
blocks = re.findall(r"```\n(.*?)```", pack, re.S)
check("PACK_BLOCKS", len(blocks) >= 2, "found %d fenced blocks" % len(blocks))

claimed_program = blocks[-2]
claimed_output = blocks[-1]

anon = open(os.path.join(HERE, "fixture_anon.qui"), encoding="utf-8").read()
real = open(os.path.join(HERE, "fixture_real.qui"), encoding="utf-8").read()

check("PACK_PROGRAM_EQ_ANON", claimed_program == anon,
      "" if claimed_program == anon else "pack's worked example differs from fixture_anon.qui")

back = inverse(anon)
check("ANON_INVERSE_EQ_REAL", back == real,
      "" if back == real else "inverse(fixture_anon) differs from fixture_real")

tmp = tempfile.mkdtemp(prefix="packcheck_")
src = os.path.join(tmp, "solution.qui")
binp = os.path.join(tmp, "solution")
open(src, "w", encoding="utf-8").write(back)
b = subprocess.run([QUIDRA, "build", src, "-o", binp], capture_output=True, text=True)
check("FIXTURE_BUILD", b.returncode == 0, "exit %d" % b.returncode)
if b.returncode == 0:
    r = subprocess.run([binp], capture_output=True, text=True)
    check("FIXTURE_EXIT_ZERO", r.returncode == 0, "exit %d" % r.returncode)
    check("FIXTURE_STDERR_EMPTY", r.stderr == "", repr(r.stderr[:60]))
    check("OUTPUT_EQ_PACK_CLAIM", r.stdout == claimed_output,
          "" if r.stdout == claimed_output else "actual %r vs claimed %r" % (r.stdout, claimed_output))
    print("--- actual stdout ---")
    sys.stdout.write(r.stdout)
    print("--- claimed by pack ---")
    sys.stdout.write(claimed_output)

print("PACK CHECK:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
