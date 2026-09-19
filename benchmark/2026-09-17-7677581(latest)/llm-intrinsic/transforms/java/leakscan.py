#!/usr/bin/env python3
"""Identity-leak scan of the Reference Pack (methodology 10, section 2.6).

CRITICAL  the real spelling of any token this condition anonymized, as a whole word,
          anywhere in the pack.  Any hit defeats the transformation and blocks the run.
IDENTITY  the name of any programming language, compiler, runtime, build tool or file
          extension in the fixed set of ten.  Any hit blocks the run.
ADVISORY  any other reserved word of the real language appearing in PROSE (outside a
          fenced code block).  These are ordinary English words; each hit is published
          as an anonymity residual rather than treated as a defect.
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PACK = os.path.join(HERE, "reference_pack.md")

IDENTITY = [
    "java", "jvm", "javac", "jdk", "openjdk", "oracle", "jar", "bytecode", "classpath",
    "python", "cpython", "pip", "py",
    "c++", "cpp", "clang", "gcc", "g++", "stl",
    "rust", "rustc", "cargo", "crate",
    "golang", "gofmt",
    "typescript", "javascript", "tsc", "node", "npm", "deno", "ecmascript",
    "kotlin", "kotlinc", "gradle",
    "swift", "swiftc", "xcode", "cocoa", "foundation",
    "zig", "ziglang",
    "quidra",
    "stdlib", "libc", "posix",
]


def main():
    text = open(PACK, encoding="utf-8").read()
    doc = json.load(open(os.path.join(HERE, "mapping.json"), encoding="ascii"))
    real_tokens = sorted(doc["mapping"].keys())

    # split prose from fenced code blocks
    parts = text.split("```")
    prose = "".join(parts[0::2])
    code = "".join(parts[1::2])

    failures = []

    print("CRITICAL — anonymized real spellings anywhere in the pack")
    for t in real_tokens:
        hits = re.findall(r"(?<![A-Za-z0-9_])" + re.escape(t) + r"(?![A-Za-z0-9_])", text)
        status = "clean" if not hits else "LEAK x%d" % len(hits)
        print("    %-10s %s" % (t, status))
        if hits:
            failures.append("CRITICAL:" + t)

    print()
    print("IDENTITY — language / toolchain / extension names anywhere in the pack")
    id_hits = []
    low = text.lower()
    for term in IDENTITY:
        for m in re.finditer(r"(?<![a-z0-9_+#])" + re.escape(term) + r"(?![a-z0-9_+#])", low):
            id_hits.append((term, m.start()))
    if id_hits:
        for term, pos in id_hits:
            print("    LEAK %-12s at offset %d" % (term, pos))
            failures.append("IDENTITY:" + term)
    else:
        print("    clean (0 hits across %d terms)" % len(IDENTITY))

    print()
    print("ADVISORY — other reserved words of the real language, in PROSE only")
    reserved = [ln.strip() for ln in
                open(os.path.join(HERE, "wordlists", "reserved_java.txt"), encoding="ascii")
                if ln.strip()]
    adv = {}
    for w in reserved:
        if w in real_tokens or w == "_":
            continue
        n = len(re.findall(r"(?<![A-Za-z0-9_])" + re.escape(w) + r"(?![A-Za-z0-9_])", prose))
        if n:
            adv[w] = n
    if adv:
        for w, n in sorted(adv.items(), key=lambda kv: -kv[1]):
            print("    %-12s x%d  (ordinary English usage; published residual)" % (w, n))
    else:
        print("    none")

    print()
    lines = text.count("\n") + (0 if text.endswith("\n") else 1)
    print("pack line count      : %d" % lines)
    print("pack character count : %d" % len(text))
    print("prose characters     : %d" % len(prose))
    print("code characters      : %d" % len(code))
    print()
    if failures:
        print("LEAK SCAN FAILED:", failures)
        return 1
    print("LEAK SCAN PASSED (critical and identity scans clean)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
