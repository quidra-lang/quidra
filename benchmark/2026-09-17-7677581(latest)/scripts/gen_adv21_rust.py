#!/usr/bin/env python3
"""Frozen generator for ADV-21 (Rust): DEPTH nested parentheses around the literal 1.

methodology/08_adversarial_cases.json -> frozen_generators.parser_nesting
"""
import os
D = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(D, "..", "standard", "src", "adversarial", "rust")
PRO = ('use std::io::Write;\n'
       '\n'
       'fn main() {\n'
       '    let mut out = std::io::stdout();\n'
       '    writeln!(out, "ADV-START").unwrap();\n'
       '    out.flush().unwrap();\n'
       '\n'
       '    let v: i64 = ')
EPI = (';\n'
       '\n'
       '    writeln!(out, "OBS=V:{}", v).unwrap();\n'
       '    out.flush().unwrap();\n'
       '    writeln!(out, "ADV-END").unwrap();\n'
       '    out.flush().unwrap();\n'
       '}\n')
for depth, name in ((100000, "ADV-21.rs"),
                    (1000, "ADV-21_depth1000.rs"),
                    (10000, "ADV-21_depth10000.rs")):
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(PRO.encode() + b"(" * depth + b"1" + b")" * depth + EPI.encode())
    print(name, depth)
