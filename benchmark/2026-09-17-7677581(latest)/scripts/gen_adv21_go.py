#!/usr/bin/env python3
"""Frozen generator for ADV-21 (Go): DEPTH nested parentheses around the literal 1.

methodology/08_adversarial_cases.json -> frozen_generators.parser_nesting
"""
import os
D = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(D, "..", "standard", "src", "adversarial", "go")
PRO = ('package main\n'
       '\n'
       'import "fmt"\n'
       '\n'
       'func main() {\n'
       '\tfmt.Println("ADV-START")\n'
       '\n'
       '\tvar v int64 = ')
EPI = ('\n'
       '\n'
       '\tfmt.Printf("OBS=V:%d\\n", v)\n'
       '\tfmt.Println("ADV-END")\n'
       '}\n')
for depth, name in ((100000, "ADV-21.go"),
                    (1000, "ADV-21_depth1000.go"),
                    (10000, "ADV-21_depth10000.go")):
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(PRO.encode() + b"(" * depth + b"1" + b")" * depth + EPI.encode())
    print(name, depth)
