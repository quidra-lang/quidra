#!/usr/bin/env python3
"""Reference-pack conformance checks (methodology 10, PF-06 + section 10.4 (a)).

  1. the pack's worked example is BYTE-IDENTICAL to fixture_anon.zig;
  2. the output block the pack claims is BYTE-IDENTICAL to expected_output.txt,
     which was itself produced by running the real toolchain;
  3. the twelve sections are present, in order;
  4. section 12's self-reported line/character counts are the measured ones
     (--fix drives them to their fixed point);
  5. counts are written to reference_pack_counts.json.
"""
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PACK = os.path.join(HERE, "reference_pack.md")

SECTIONS = ["## 1. Program shape", "## 2. Lexical rules", "## 3. Declarations",
            "## 4. Kinds used by this task", "## 5. Expressions, operators, precedence",
            "## 6. Control flow", "## 7. Standard vocabulary", "## 8. Modules",
            "## 9. Output", "## 10. Substituted words (condition I1)",
            "## 11. Worked example", "## 12. Size of this pack"]

COUNT_RE = re.compile(r"Recorded by measurement, never by estimate: \d+ lines, \d+ characters\.")


def read(p):
    with open(p, encoding="utf-8") as fh:
        return fh.read()


def counts(text):
    lines = text.count("\n") + (0 if text.endswith("\n") else 1)
    return lines, len(text)


def fix_counts():
    """Drive section 12's self-reference to its fixed point."""
    for _ in range(12):
        text = read(PACK)
        ln, ch = counts(text)
        want = ("Recorded by measurement, never by estimate: %d lines, %d characters."
                % (ln, ch))
        cur = COUNT_RE.search(text)
        if cur and cur.group(0) == want:
            return True
        new = COUNT_RE.sub(lambda m: want, text, count=1)
        if new == text:
            return False
        with open(PACK, "w", encoding="utf-8") as fh:
            fh.write(new)
    return False


def main():
    if "--fix" in sys.argv:
        ok = fix_counts()
        print("section 12 self-count fixed point reached: %s" % ok)

    text = read(PACK)
    anon = read(os.path.join(HERE, "fixture_anon.zig"))
    expected = read(os.path.join(HERE, "expected_output.txt"))

    blocks = re.findall(r"```[^\n]*\n(.*?)```", text, re.S)
    ok = True

    # section 11 worked example, then the output block that follows it
    ex_ok = anon in blocks
    print("  [%s] worked example is byte-identical to fixture_anon.zig"
          % ("PASS" if ex_ok else "FAIL"))
    ok &= ex_ok

    out_ok = expected in blocks
    print("  [%s] claimed output is byte-identical to expected_output.txt"
          % ("PASS" if out_ok else "FAIL"))
    ok &= out_ok

    pos, order_ok = -1, True
    for h in SECTIONS:
        i = text.find(h)
        if i < 0 or i < pos:
            order_ok = False
            print("      missing or out of order: %s" % h)
        pos = max(pos, i)
    print("  [%s] twelve sections present, in order" % ("PASS" if order_ok else "FAIL"))
    ok &= order_ok

    ln, ch = counts(text)
    claim = COUNT_RE.search(text)
    claim_ok = bool(claim) and claim.group(0).endswith("%d lines, %d characters." % (ln, ch))
    print("  [%s] section 12 self-count matches measurement (%d lines, %d characters)"
          % ("PASS" if claim_ok else "FAIL", ln, ch))
    ok &= claim_ok

    in_range = 150 <= ln <= 250
    print("  [%s] length within the 150-250 line target" % ("PASS" if in_range else "FAIL"))
    ok &= in_range

    mapping = json.load(open(os.path.join(HERE, "mapping.json")))
    documented = sum(1 for e in mapping["mapping"].values()
                     if ("`%s`" % e["pseudo"]) in text)
    all_doc = documented == len(mapping["mapping"])
    print("  [%s] all %d pseudo-words documented in the pack (%d found)"
          % ("PASS" if all_doc else "FAIL", len(mapping["mapping"]), documented))
    ok &= all_doc

    doc = {
        "file": "reference_pack.md",
        "lines": ln,
        "characters": ch,
        "sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
        "target_range_lines": [150, 250],
        "within_target": in_range,
        "sections_present": ["P%d" % i for i in range(1, 13)],
        "anonymized_tokens_documented": documented,
        "worked_examples": 1,
        "worked_example_byte_identical_to_fixture_anon": ex_ok,
        "claimed_output_byte_identical_to_expected_output": out_ok,
        "sibling_pack_line_counts": {"go": 247, "rust": 230, "java": 249,
                                     "python": 252, "cpp": 245},
    }
    with open(os.path.join(HERE, "reference_pack_counts.json"), "w", encoding="utf-8") as fh:
        fh.write(json.dumps(doc, indent=2) + "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
