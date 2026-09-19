"""I1 round-trip and submission validator, language_id = typescript.

Subcommands
  roundtrip FILE                 R1 byte identity, R4 non-triviality, R5 domain containment
  submission ANON.ts EXPECTED    gate -> inverse -> build -> run -> byte-compare
  gate ANON.ts                   the methodology 10 section 4.2a conformance gate alone
  leakscan FILE                  identity-leak scan of a Reference Pack
  selftest                       every positive AND every negative case, with output

The selftest is the evidence for methodology 10 section 10.4 requirement (c): a
validator that cannot fail measures nothing, so each check is exercised with an
input it must accept and a mutated input it must reject.
"""

import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import forward as FWD          # noqa: E402
import inverse as INV          # noqa: E402
import gen_mapping as GEN      # noqa: E402

BUILD = ["tsc", "solution.ts"]
RUN = ["node", "solution.js"]

# Word tokens the Reference Pack documents untransformed (V-roles; methodology 10
# section 7.1 keeps standard-library names real in I1). None of them is a reserved
# word of this language, so the gate surface below is unaffected -- the set is
# written out anyway so the exemption is auditable rather than implicit.
PACK_UNTRANSFORMED = frozenset(["console", "log", "push", "join", "String", "length"])


def _load(name):
    with open(os.path.join(HERE, name)) as f:
        return f.read()


def reserved_words():
    with open(os.path.join(HERE, "wordlists", "reserved_typescript.txt")) as f:
        return set(x.strip() for x in f if x.strip())


def mapping_pairs():
    doc = json.loads(_load("mapping.json"))
    fwd = {real: e["pseudo_word"] for real, e in doc["mapping"].items()}
    inv = {v: k for k, v in fwd.items()}
    return fwd, inv


# --------------------------------------------------------------- 4.2a gate


def gate(src):
    """Return the sorted list of H_REAL events: real reserved words in a submission."""
    fwd, _ = mapping_pairs()
    surface = (reserved_words() | set(fwd)) - PACK_UNTRANSFORMED
    hits = set()
    for kind, text in INV.tokenize(src, keyword_words=frozenset(fwd.values())):
        if kind == "WORD" and text in surface:
            hits.add(text)
    return sorted(hits)


# --------------------------------------------------------------- R1/R4/R5


def roundtrip(path):
    """R1 byte identity, R4 non-triviality, R5 domain containment. Returns (ok, notes)."""
    fwd, inv = mapping_pairs()
    src = _load(path)
    anon, inserted = FWD.forward_text(src, fwd)
    back = INV.inverse_text(anon, inv, inserted=inserted)
    notes = []
    ok = True
    if back != src:
        ok = False
        notes.append("R1_BYTE_IDENTITY_FAILED")
    else:
        notes.append("R1_ok")
    if anon == src:
        ok = False
        notes.append("R4_TRANSFORM_WAS_A_NO_OP")
    else:
        notes.append("R4_ok")
    changed = _changed_words(src, anon)
    outside = sorted(w for w in changed if w not in fwd)
    if outside:
        ok = False
        notes.append("R5_DOMAIN_ESCAPE:" + ",".join(outside))
    else:
        notes.append("R5_ok(%d tokens changed)" % len(changed))
    return ok, notes


def _changed_words(src, anon):
    """The multiset (as a set) of real word tokens the forward pass rewrote."""
    a = [t for t in FWD.tokenize(src) if t[0] not in ("WS", "NEWLINE")]
    b = [t for t in FWD.tokenize(anon) if t[0] not in ("WS", "NEWLINE")]
    changed = set()
    for (ka, ta), (kb, tb) in zip(a, b):
        if ta != tb:
            changed.add(ta)
    if len(a) != len(b):
        changed.add("<TOKEN_COUNT_CHANGED>")
    return changed


# --------------------------------------------------------------- full pipeline


def submission(anon_path, expected_path):
    """gate -> inverse -> build -> run -> byte-compare. Returns (ok, reason)."""
    src = _load(anon_path) if not os.path.isabs(anon_path) else open(anon_path).read()
    expected = open(os.path.join(HERE, expected_path)).read() if not os.path.isabs(
        expected_path) else open(expected_path).read()
    hits = gate(src)
    if hits:
        return False, "GATE_H_REAL:" + ",".join(hits)
    real = INV.inverse_text(src)
    with tempfile.TemporaryDirectory() as td:
        srcf = os.path.join(td, "solution.ts")
        with open(srcf, "w") as f:
            f.write(real)
        b = subprocess.run(BUILD, cwd=td, capture_output=True, text=True)
        if b.returncode != 0:
            first = (b.stdout + b.stderr).strip().splitlines()
            return False, "COMPILE_FAILED:rc=%d:%s" % (
                b.returncode, first[0][:110] if first else "")
        if not os.path.exists(os.path.join(td, "solution.js")):
            return False, "NO_EMITTED_ARTIFACT"
        r = subprocess.run(RUN, cwd=td, capture_output=True, text=True, timeout=10)
        if r.returncode != 0:
            return False, "RUN_FAILED:rc=%d" % r.returncode
        if r.stderr != "":
            return False, "STDERR_NOT_EMPTY"
        if r.stdout != expected:
            return False, "OUTPUT_MISMATCH"
    return True, "ACCEPTED"


# --------------------------------------------------------------- leak scan

LEAK_TERMS = """
typescript type-script javascript ecmascript es2015 es5 es6 node nodejs node.js deno bun
tsc tsx ts js jsx npm npx yarn pnpm webpack vite esbuild v8 chrome browser dom
python python3 cpython pypy rust rustc cargo golang go gcc clang llvm cpp c++ java javac
jvm jdk kotlin kotlinc swift swiftc zig quidra dotnet csharp ruby perl php lua haskell
.ts .js .py .rs .go .java .kt .swift .zig .cpp .qui
mozilla developer.mozilla.org stackoverflow github docs.python.org
"""


def leakscan(path):
    text = _load(path) if not os.path.isabs(path) else open(path).read()
    low = text.lower()
    fwd, _ = mapping_pairs()
    hits = []
    for term in LEAK_TERMS.split():
        pat = re.escape(term)
        if term.startswith("."):
            rx = re.compile(pat + r"(?![A-Za-z0-9])")
        else:
            rx = re.compile(r"(?<![A-Za-z0-9_.+#-])" + pat + r"(?![A-Za-z0-9_+#-])")
        if rx.search(low):
            hits.append("IDENTITY_TERM:" + term)
    for real in sorted(fwd):
        bounded = r"(?<![A-Za-z0-9_$])" + re.escape(real) + r"(?![A-Za-z0-9_$])"
        if re.search(bounded, text):
            hits.append("REAL_KEYWORD:" + real)
            continue
        # Second, stricter pass: the same word in any capitalisation. This language
        # is case-sensitive, so `String` is not the type keyword `string` -- the
        # V-role names the pack documents untransformed are therefore exempt, and
        # the exemption is named rather than implied.
        for m in re.finditer(bounded, text, re.IGNORECASE):
            if m.group() not in PACK_UNTRANSFORMED:
                hits.append("REAL_KEYWORD_ANYCASE:" + m.group())
                break
    return hits, len(text.splitlines()), len(text)


# --------------------------------------------------------------- selftest

def _sub_inline(text, expected_path="expected_output.txt"):
    with tempfile.NamedTemporaryFile("w", suffix=".ts", delete=False) as f:
        f.write(text)
        p = f.name
    try:
        return submission(p, expected_path)
    finally:
        os.unlink(p)


def selftest():
    fwd, inv = mapping_pairs()
    fails = []

    def report(tag, ok, detail):
        print("[%s] %-28s %s" % ("PASS" if ok else "FAIL", tag, detail))
        if not ok:
            fails.append(tag)

    print("=== POSITIVES (must be accepted) ===")
    for tag, path in (("P1 fixture roundtrip", "fixture_real.ts"),
                      ("P2 solution roundtrip", "reference_solution.ts")):
        ok, notes = roundtrip(path)
        report(tag, ok, " ".join(notes))

    ok, reason = submission("fixture_anon.ts", "fixture_expected_output.txt")
    report("P3 anon fixture pipeline", ok, reason)

    anon, inserted = FWD.forward_text(_load("fixture_real.ts"), fwd)
    ok = anon == _load("fixture_anon.ts") and inserted == []
    report("P4 fixture_anon == fwd", ok,
           "byte-identical to forward(fixture_real.ts); 0 inserted whitespace")

    anon_sol, _ = FWD.forward_text(_load("reference_solution.ts"), fwd)
    ok, reason = _sub_inline(anon_sol)
    report("P5 anon solution pipeline", ok, reason)

    # P6/P7: what the pack SHOWS is what the fixture IS, and what it CLAIMS is what
    # the fixture PRINTS -- both compared as bytes, never by eye.
    blocks = re.findall(r"```\n([\s\S]*?)```", _load("reference_pack.md"))
    ok = any(b == _load("fixture_anon.ts") for b in blocks)
    report("P6 pack example == fixture_anon", ok, "worked example is byte-identical")
    ok = any(b == _load("fixture_expected_output.txt") for b in blocks)
    report("P7 pack claim == fixture output", ok, "claimed output is byte-identical")

    print()
    print("=== NEGATIVES (must be rejected) ===")

    # N1 one pseudo-word swapped back to the real keyword it stands for
    n1 = anon_sol.replace("memagu", "if", 1)
    ok, reason = _sub_inline(n1)
    report("N1 one real keyword", (not ok) and reason.startswith("GATE_H_REAL"), reason)

    # N2 correct real source that ignores the transformation entirely (4.2a / PF-05 b)
    ok, reason = _sub_inline(_load("reference_solution.ts"))
    report("N2 real source, transform ignored",
           (not ok) and reason.startswith("GATE_H_REAL"), reason)

    # N3 a pseudo-word mistyped into an undefined name
    n3 = anon_sol.replace("vudoru", "vudorx")
    ok, reason = _sub_inline(n3)
    report("N3 mistyped pseudo-word", (not ok) and reason.startswith("COMPILE_FAILED"), reason)

    # N4 source that already uses a pseudo-word as a user identifier (injectivity)
    n4 = _load("fixture_real.ts").replace("biggest", "gebamu")
    try:
        FWD.forward_text(n4, fwd)
        ok, reason = True, "accepted (WRONG)"
    except ValueError as exc:
        ok, reason = False, str(exc).split(";")[0]
    report("N4 pseudo-word as identifier", not ok, reason)

    # N4b the same collision seen from the submission side: INVERSE_AMBIGUOUS
    n4b = anon_sol.replace("busopo total", "busopo vudoru").replace("total =", "vudoru =")
    n4b = n4b.replace('String(total)', 'String(vudoru)')
    ok, reason = _sub_inline(n4b)
    report("N4b INVERSE_AMBIGUOUS", (not ok) and reason.startswith("COMPILE_FAILED"), reason)

    # N5 token-clean, gate-clean, compiles, runs -- arithmetic altered
    n5 = anon_sol.replace("48271", "48273")
    ok, reason = _sub_inline(n5)
    report("N5 correct-looking, wrong output", (not ok) and reason == "OUTPUT_MISMATCH", reason)

    # N6 a transform that reached inside a text literal
    probe = ('%s greet(): %s {\n    console.log("for the record");\n    %s "done";\n}\n'
             % ("function", "string", "return"))
    naive = probe
    for real, pseudo in fwd.items():
        naive = re.sub(r"(?<![A-Za-z0-9_$])" + re.escape(real) + r"(?![A-Za-z0-9_$])",
                       pseudo, naive)
    back = INV.inverse_text(naive, inv)
    ok = back != probe
    report("N6 transform inside a literal", ok,
           "R1_BYTE_IDENTITY_FAILED (the literal came back as %r)"
           % re.search(r'"(.*?)"', back).group(1))

    # N7 output correct but a trailing extra line
    n7 = anon_sol.replace("main();", 'console.log("EXTRA");\nmain();')
    ok, reason = _sub_inline(n7)
    report("N7 extra output line", (not ok) and reason == "OUTPUT_MISMATCH", reason)

    print()
    print("=== REJECTION-FILTER LIVENESS (gen_mapping.accept) ===")
    accept, _fired = GEN.make_accept()
    probes = [
        ("filter1 shape", "ab3de", False, "not ^[a-z]{6}$"),
        ("filter2 collision", "gebamu", False, "already used"),
        ("filter3 english", "banana", False, "in en_common.txt"),
        ("filter4 progterm", "buffer", False, "in prog_terms.txt"),
        ("filter5 reserved", "static", False, "in reserved_union.txt"),
        ("filter6 substring", "zovoid", False, "contains 'void'"),
        ("filter7 material", "bigger", False, "occurs in the fixture material"),
        ("control accepted", "gidopu", True, "trips no filter"),
    ]
    used = set(fwd.values())
    for tag, w, want, why in probes:
        got = accept(w, used)
        report(tag, got == want, "%-8s accept()=%-5s (%s)" % (w, got, why))

    print()
    print("=== IDENTITY-LEAK SCAN (reference_pack.md) ===")
    if os.path.exists(os.path.join(HERE, "reference_pack.md")):
        hits, lines, chars = leakscan("reference_pack.md")
        report("leakscan pack", not hits,
               "%d lines, %d characters, %d hits %s" % (lines, chars, len(hits), hits or ""))
    else:
        report("leakscan pack", False, "reference_pack.md missing")

    print()
    if fails:
        print("SELFTEST FAILED:", ", ".join(fails))
        return 1
    print("SELFTEST OK - every positive accepted, every negative rejected")
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "selftest":
        return selftest()
    if cmd == "roundtrip":
        ok, notes = roundtrip(argv[2])
        print(("OK " if ok else "FAIL ") + " ".join(notes))
        return 0 if ok else 1
    if cmd == "gate":
        hits = gate(_load(argv[2]) if not os.path.isabs(argv[2]) else open(argv[2]).read())
        print("CLEAN" if not hits else "GATE_H_REAL:" + ",".join(hits))
        return 0 if not hits else 1
    if cmd == "submission":
        ok, reason = submission(argv[2], argv[3])
        print(reason)
        return 0 if ok else 1
    if cmd == "leakscan":
        hits, lines, chars = leakscan(argv[2])
        print("%d lines, %d characters, %d hits" % (lines, chars, len(hits)))
        for h in hits:
            print("  " + h)
        return 0 if not hits else 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
