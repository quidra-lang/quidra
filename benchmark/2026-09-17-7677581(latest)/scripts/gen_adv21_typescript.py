#!/usr/bin/env python3
"""Frozen generator for ADV-21 (TypeScript): DEPTH nested parentheses around the literal 1.

methodology/08_adversarial_cases.json -> frozen_generators.parser_nesting

Binding note: the generator spec fixes the inner token as the single ASCII byte
0x31 ('1').  TypeScript's fixed_width_i64 binding is `bigint` (TM3), whose
literal would have to be written `1n`, which the frozen byte specification
forbids.  The binding used here is therefore TypeScript's default_arithmetic
type `number`, which accepts the literal `1` exactly as specified.
"""
import os
D = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(D, "..", "standard", "src", "adversarial", "typescript")
PRO = ('console.log("ADV-START");\n'
       '\n'
       'const v: number = ')
EPI = (';\n'
       '\n'
       'console.log("OBS=V:" + String(v));\n'
       'console.log("ADV-END");\n')
for depth, name in ((100000, "ADV-21.ts"),
                    (1000, "ADV-21_depth1000.ts"),
                    (10000, "ADV-21_depth10000.ts")):
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(PRO.encode() + b"(" * depth + b"1" + b")" * depth + EPI.encode())
    print(name, depth)
