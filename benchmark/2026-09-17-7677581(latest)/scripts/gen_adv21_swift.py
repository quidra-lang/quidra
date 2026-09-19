#!/usr/bin/env python3
"""Frozen generator for ADV-21 (Swift): DEPTH nested parentheses around the literal 1.

methodology/08_adversarial_cases.json -> frozen_generators.parser_nesting
"""
import os
D = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(D, "..", "standard", "src", "adversarial", "swift")
PRO = ('import Foundation\n'
       'print("ADV-START"); fflush(stdout)\n'
       'let v: Int64 = ')
EPI = ('\n'
       'print("OBS=V:\\(v)"); fflush(stdout)\n'
       'print("ADV-END"); fflush(stdout)\n')
for depth, name in ((100000, "ADV-21.swift"),
                    (1000, "ADV-21_depth1000.swift"),
                    (10000, "ADV-21_depth10000.swift")):
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(PRO.encode() + b"(" * depth + b"1" + b")" * depth + EPI.encode())
    print(name, depth)
