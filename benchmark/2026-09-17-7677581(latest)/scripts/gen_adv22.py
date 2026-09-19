#!/usr/bin/env python3
"""Frozen generator for ADV-22 (malformed source text).

methodology/08_adversarial_cases.json -> frozen_generators.malformed_source
  ADV-22a: first floor(0.60 * L) bytes of the valid file.
  ADV-22b: the three ASCII bytes 0x40 0x23 0x24 inserted at byte offset floor(L/2).
Both are generated unconditionally for every language.
"""
import sys, os

valid = sys.argv[1]
a_out = sys.argv[2]
b_out = sys.argv[3]
b = open(valid, "rb").read()
L = len(b)
open(a_out, "wb").write(b[: int(0.60 * L)])
mid = L // 2
open(b_out, "wb").write(b[:mid] + b"\x40\x23\x24" + b[mid:])
print("L=%d  cut=%d  mid=%d" % (L, int(0.60 * L), mid))
