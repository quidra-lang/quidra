#!/usr/bin/env python3
"""Losslessness and correctness of ktlex.py on the adversarial inputs that a
Java-shaped scanner gets wrong.  Cited by preflight.md section 4.4."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ktlex import tokenize, check_lossless

CASES = [
    ("range operator",        'for (i in 0..49) {}\n',            ("OP", "..")),
    ("underscore/hex/float",  'val a = 1_000L; val b = 0xFF; val c = 1.5e-3f\n', ("NUMBER", "0xFF")),
    ("NESTED block comment",  '/* a /* b */ c */ val x = 1\n',    ("COMMENT", "/* a /* b */ c */")),
    ("back-quoted identifier",'val `if` = 3\n',                   ("BQWORD", "`if`")),
    ("keywords in a literal", 'println("if else fun val")\n',     ("STRING", '"if else fun val"')),
    ("raw triple-quoted",     'val s = """raw "fun" text"""\n',   ("STRING", '"""raw "fun" text"""')),
    ("negated membership",    'if (x !in y) {}\n',                ("WORD", "in")),
    ("character literal",     "val c = 'a'\n",                    ("CHAR", "'a'")),
    ("elvis/safe/identity",   'a ?: b; a?.c; a === b\n',          ("OP", "===")),
]

fails = []
for label, src, expect in CASES:
    toks = tokenize(src)
    lossless = check_lossless(src)
    present = expect in toks
    ok = lossless and present
    print("  %-24s lossless=%-5s expected token %-22s present=%-5s  %s"
          % (label, lossless, repr(expect), present, "PASS" if ok else "FAIL"))
    if not ok:
        fails.append(label)

src = open(os.path.join(HERE, "fixture_real.kt"), encoding="utf-8").read()
ok = check_lossless(src)
print("  %-24s lossless=%-5s %s" % ("fixture_real.kt", ok, "PASS" if ok else "FAIL"))
if not ok:
    fails.append("fixture")

print()
if fails:
    print("LEXER TEST FAILED:", fails); sys.exit(1)
print("LEXER TEST PASSED (%d adversarial inputs + the fixture)" % len(CASES))
