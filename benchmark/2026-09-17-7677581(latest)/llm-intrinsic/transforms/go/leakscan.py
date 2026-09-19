#!/usr/bin/env python3
"""Identity-leak scan of the Reference Pack (methodology 10, section 2.6).

Rejects: the real language's name and toolchain names, its file extension, and any
whole-word occurrence of a reserved word of the real language or of a token this
condition transforms.  V-role spellings and the standard module paths are exempt in
I1 by the stated section 2.6 exemption; they are listed, not counted as hits.
"""
import json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
text = open(os.path.join(HERE, "reference_pack.md"), encoding="utf-8").read()

reserved = [w.strip() for w in open(os.path.join(HERE, "wordlists/reserved_go.txt")) if w.strip()]
mapping = json.load(open(os.path.join(HERE, "mapping.json")))
transformed = [e["real"] for e in mapping["mapping"].values()]

IDENTITY = ["golang", "gopher", "gofmt", "go build", "go run", "go vet",
            "gccgo", "goroutine", ".go", "google"]
EXEMPT = {"fmt", "strconv", "Println", "FormatInt"}

hits = []
for w in sorted(set(reserved) | set(transformed)):
    if w in EXEMPT:
        continue
    for m in re.finditer(r"(?<![A-Za-z0-9_])" + re.escape(w) + r"(?![A-Za-z0-9_])", text):
        line = text.count("\n", 0, m.start()) + 1
        hits.append((w, line, text.splitlines()[line - 1].strip()[:70]))
for term in IDENTITY:
    for m in re.finditer(re.escape(term), text, re.IGNORECASE):
        line = text.count("\n", 0, m.start()) + 1
        hits.append((term, line, text.splitlines()[line - 1].strip()[:70]))

lines = text.count("\n") + (0 if text.endswith("\n") else 1)
print("reference_pack.md: %d lines, %d characters" % (lines, len(text)))
print("exempt V-role spellings present by design (section 2.6 I1 exemption): %s"
      % ", ".join(sorted(s for s in EXEMPT if re.search(r"\b"+s+r"\b", text))))
if hits:
    print("LEAK SCAN: %d HIT(S)" % len(hits))
    for w, ln, ctx in hits:
        print("  line %-4d %-12s | %s" % (ln, w, ctx))
    sys.exit(1)
print("LEAK SCAN: 0 hits")
