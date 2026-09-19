#!/usr/bin/env python3
"""Frozen generator for ADV-21 (C++): DEPTH nested parentheses around the literal 1."""
import sys, os
D = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(D, "..", "standard", "src", "adversarial", "cpp")
PRO = ('#include <cstdint>\n'
       '#include <iostream>\n'
       '\n'
       'int main() {\n'
       '    std::cout << "ADV-START" << std::endl;\n'
       '    std::int64_t v = ')
EPI = (';\n'
       '    std::cout << "OBS=V:" << v << std::endl;\n'
       '    std::cout << "ADV-END" << std::endl;\n'
       '    return 0;\n'
       '}\n')
for depth, name in ((100000, "ADV-21_single.cpp"),
                    (1000, "ADV-21_single_depth1000.cpp"),
                    (10000, "ADV-21_single_depth10000.cpp")):
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(PRO.encode() + b"(" * depth + b"1" + b")" * depth + EPI.encode())
    print(name, depth)
