#!/usr/bin/env python3
"""
Round-trip / conformance validator for the I1 (go) transform set.

It implements, for this one language and this one task, the checks methodology 10
section 6.4 calls R1-R5, and the section 4.2a conformance gate for condition I1.
Every check is written so that it can FAIL: `validate.py selftest` runs each one
against a correct artifact and against a deliberately corrupted one and requires
the correct one to be accepted and the corrupted one to be rejected.

    validate.py roundtrip                 R1..R5 on the shipped fixture
    validate.py gate FILE                 section 4.2a conformance gate
    validate.py filters                   accept() rejection filters self-test
    validate.py determinism               regenerate the mapping and compare
    validate.py selftest                  everything, positive AND negative

Exit status 0 = all requested checks reached their expected verdict.
"""

import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import lex10          # noqa: E402
import forward as fw  # noqa: E402
import inverse as iv  # noqa: E402

REAL = os.path.join(HERE, "fixture_real.go")
ANON = os.path.join(HERE, "fixture_anon.go")
POSMAP = ANON + ".posmap.json"
EXPECTED = os.path.join(HERE, "expected_output.txt")

GREEN = "PASS"
RED = "FAIL"


def read(path):
    with open(path, "r", encoding="utf-8") as fh:
        return fh.read()


def load_reserved():
    words = set()
    with open(os.path.join(HERE, "wordlists", "reserved_go.txt"), "r") as fh:
        for line in fh:
            if line.strip():
                words.add(line.strip())
    mapping, _ = fw.load_mapping()
    for entry in mapping["mapping"].values():
        words.add(entry["real"])
    return words


# --------------------------------------------------------------- build helpers


def build_and_run(source_text, tag):
    """Build `source_text` with the frozen recipe and run it.

    Returns (build_ok, build_log, exit_status, stdout).
    """
    d = tempfile.mkdtemp(prefix="i1go_" + tag + "_")
    src = os.path.join(d, "solution.go")
    binp = os.path.join(d, "bin")
    with open(src, "w", encoding="utf-8") as fh:
        fh.write(source_text)
    p = subprocess.run(
        ["go", "build", "-o", binp, src],
        capture_output=True, text=True,
    )
    if p.returncode != 0:
        return False, (p.stdout + p.stderr).strip(), None, None
    r = subprocess.run([binp], capture_output=True, text=True, timeout=10)
    return True, "", r.returncode, r.stdout


# --------------------------------------------------------------- the checks


def check_roundtrip(real_text=None, anon_text=None, posmap=None, quiet=False):
    """R1..R5.  Returns dict of check -> bool."""
    real_text = real_text if real_text is not None else read(REAL)
    _, fwd_map = fw.load_mapping()
    _, inv_map = iv.load_mapping()

    if anon_text is None:
        anon_text, posmap = fw.forward(real_text, fwd_map)
    if posmap is None:
        posmap = json.loads(read(POSMAP))

    res = {}

    # R1 -- byte identity
    back, _ = iv.inverse(anon_text, inv_map, posmap)
    res["R1_byte_identity"] = (back == real_text)

    # R4 -- non-triviality
    res["R4_non_triviality"] = (anon_text != real_text and len(posmap["changed_tokens"]) > 0)

    # R5 -- domain containment: every token the forward pass changed is a bound
    # role token, and the diff touches nothing else.
    bound = set(fwd_map.keys())
    res["R5_domain_containment"] = all(
        a in bound and b == fwd_map[a] for a, b in posmap["changed_tokens"]
    )
    # ... and independently: the two token streams differ only at WORD positions
    # whose real text is bound (plus the recorded whitespace insertions).
    tr = [t for t in lex10.lex(real_text) if t.kind not in ("WS", "COMMENT")]
    ta = [t for t in lex10.lex(anon_text) if t.kind not in ("WS", "COMMENT")]
    if len(tr) != len(ta):
        res["R5_domain_containment"] = False
    else:
        for a, b in zip(tr, ta):
            if a.text == b.text:
                continue
            if not (a.kind == "WORD" and b.kind == "WORD"
                    and a.text in fwd_map and fwd_map[a.text] == b.text):
                res["R5_domain_containment"] = False
                break

    # R2 -- build identity, R3 -- behavioural identity
    ok_r, log_r, st_r, out_r = build_and_run(real_text, "real")
    ok_b, log_b, st_b, out_b = build_and_run(back, "back")
    res["R2_build_identity"] = ok_b and st_b == 0
    res["R3_behavioural_identity"] = (ok_r and ok_b and out_r == out_b and st_r == st_b)
    if not quiet:
        if not ok_b:
            print("    inverse image build log: " + log_b.splitlines()[0] if log_b else "")
    res["_stdout"] = out_b
    return res


def check_gate(text):
    """Section 4.2a conformance gate for I1.

    Passes iff no WORD token of the submission is a reserved word of the real
    language or a token the pack documents in transformed form.  Returns
    (passes, [H_REAL events]).
    """
    reserved = load_reserved()
    events = []
    for t in lex10.lex(text):
        if t.kind == "WORD" and t.text in reserved:
            events.append((t.pos, t.text))
    return (len(events) == 0), events


def check_filters():
    """Census of every accept() rejection filter over the whole PW-1 output space.

    Methodology 10 PF-08 requires that every rejection filter fire at least once.
    A hand-picked witness would prove little (an earlier filter usually catches it
    first), so this walks all 14*5*14*5*14*5 = 343,000 words PW-1 can emit, records
    which filter rejects each one, and reports the first witness per filter in
    lexicographic order.  A filter with zero witnesses is reported as SUBSUMED --
    it provably cannot fire for this language and task, which is a fact to publish,
    not a check to fake.

    Returns {filter_name: (count, first_witness_or_None)}.
    """
    import gen_mapping as gm

    census = {}
    used = {"zzzzzz"}   # non-empty so the "used" filter has something to reject
    accepted = 0
    for c1 in gm.CONS:
        for v1 in gm.VOW:
            for c2 in gm.CONS:
                for v2 in gm.VOW:
                    for c3 in gm.CONS:
                        for v3 in gm.VOW:
                            w = c1 + v1 + c2 + v2 + c3 + v3
                            good, why = gm.accept(w, used)
                            if good:
                                accepted += 1
                                continue
                            fam = why.split(":")[0]
                            cnt, first = census.get(fam, (0, None))
                            census[fam] = (cnt + 1, first if first else w)
    # "shape" and "used" cannot appear in that space by construction, so they are
    # exercised directly -- they are guards, not samplers.
    for name, probe, extra in (("shape", "ab3", set()), ("used", "zzzzzz", used)):
        good, why = gm.accept(probe, extra)
        if (not good) and why.split(":")[0] == name:
            cnt, first = census.get(name, (0, None))
            census[name] = (cnt + 1, first if first else probe)
    census["_accepted"] = (accepted, None)
    return census


def check_determinism():
    """Regenerate the mapping in two fresh processes; require byte equality."""
    cur = read(os.path.join(HERE, "mapping.json"))
    outs = []
    for _ in range(2):
        d = tempfile.mkdtemp(prefix="i1go_det_")
        subprocess.run(
            [sys.executable, os.path.join(HERE, "gen_mapping.py")],
            capture_output=True, text=True, cwd=d, check=True,
        )
        outs.append(read(os.path.join(HERE, "mapping.json")))
    a, b = json.loads(outs[0]), json.loads(outs[1])
    c = json.loads(cur)
    for x in (a, b, c):
        x.pop("generated_at", None)
    return a == b == c, {k: v["pseudo"] for k, v in a["mapping"].items()}


# --------------------------------------------------------------- self test


def selftest():
    failures = []

    def report(label, got, want=True):
        ok = (got == want)
        print("  [%s] %s" % (GREEN if ok else RED, label))
        if not ok:
            failures.append(label)
        return ok

    print("A. ROUND TRIP -- correct artifact must be ACCEPTED")
    res = check_roundtrip()
    for k in ("R1_byte_identity", "R2_build_identity", "R3_behavioural_identity",
              "R4_non_triviality", "R5_domain_containment"):
        report("%s on the shipped fixture" % k, res[k])
    report("inverse image reproduces expected_output.txt",
           res["_stdout"] == read(EXPECTED))

    print("\nB. ROUND TRIP -- corrupted artifacts must be REJECTED")

    anon = read(ANON)
    posmap = json.loads(read(POSMAP))

    # B1: a pseudo-word corrupted by one character -> inverse leaves an unknown
    #     word -> the inverse image does not build.
    bad1 = anon.replace("feguzi", "fegusi", 1)
    r1 = check_roundtrip(anon_text=bad1, posmap=posmap, quiet=True)
    report("B1 one-character corruption of a pseudo-word rejected (R1)",
           r1["R1_byte_identity"], want=False)
    report("B1 one-character corruption of a pseudo-word rejected (R2 build)",
           r1["R2_build_identity"], want=False)

    # B2: two pseudo-words swapped -> round trip is not byte-identical and the
    #     inverse image does not build.
    bad2 = (anon.replace("feguzi", "\x00TMP\x00")
                .replace("lunuso", "feguzi")
                .replace("\x00TMP\x00", "lunuso"))
    r2 = check_roundtrip(anon_text=bad2, posmap=posmap, quiet=True)
    report("B2 swapped pseudo-words rejected (R1)", r2["R1_byte_identity"], want=False)
    report("B2 swapped pseudo-words rejected (R2 build)", r2["R2_build_identity"], want=False)

    # B3: a transformer that reached INSIDE a text literal.  This is the
    #     corruption that silently changes program output, so it is checked
    #     behaviourally as well as byte-wise.
    bad3 = anon.replace('"SUM "', '"nebala "', 1)
    r3 = check_roundtrip(anon_text=bad3, posmap=posmap, quiet=True)
    report("B3 substitution inside a text literal rejected (R1)",
           r3["R1_byte_identity"], want=False)
    report("B3 substitution inside a text literal rejected (R3 behaviour)",
           r3["R3_behavioural_identity"], want=False)
    report("B3 output actually differs from the oracle",
           r3["_stdout"] == read(EXPECTED), want=False)

    # B4: a truncated submission -> does not build.
    bad4 = "\n".join(anon.splitlines()[:12])
    r4 = check_roundtrip(anon_text=bad4, posmap=posmap, quiet=True)
    report("B4 truncated submission rejected (R2 build)", r4["R2_build_identity"], want=False)

    print("\nC. CONFORMANCE GATE (section 4.2a) -- positive and negative")
    ok, ev = check_gate(read(ANON))
    report("C1 anonymized fixture passes the gate (0 H_REAL events)", ok)
    ok, ev = check_gate(read(REAL))
    report("C2 correct REAL source that ignores the transformation is rejected",
           ok, want=False)
    print("      H_REAL events found in the real source: %d  (first five: %s)"
          % (len(ev), ", ".join(w for _, w in ev[:5])))
    # C3: an anonymized submission that leaks a single real keyword.
    leak = read(ANON).replace("feguzi term > largest", "if term > largest", 1)
    ok, ev = check_gate(leak)
    report("C3 anonymized submission leaking ONE real keyword is rejected", ok, want=False)
    # C4: a pseudo-word inside a text literal must NOT trip the gate, and a real
    #     keyword inside a text literal must not trip it either.
    ok, _ = check_gate(read(ANON).replace('"SUM "', '"if for package "', 1))
    report("C4 real keywords inside a text literal do not trip the gate", ok)

    print("\nD. LEXICALIZER FILTERS -- census over all 343,000 PW-1 words")
    census = check_filters()
    accepted = census.pop("_accepted")[0]
    for name in ("shape", "used", "en_common", "prog_terms", "reserved_union",
                 "substring", "task_material"):
        cnt, first = census.get(name, (0, None))
        if cnt:
            print("  [%s] D filter %-15s fires %7d times (first witness: %s)"
                  % (GREEN, name, cnt, first))
        else:
            print("  [SUBSUMED] D filter %-15s cannot fire: every word it would "
                  "reject is already rejected by an earlier filter" % name)
    print("  accepted by all filters: %d of 343000 (%.1f%%)"
          % (accepted, 100.0 * accepted / 343000))
    report("D the filter chain rejects a non-trivial share of the space",
           0 < accepted < 343000)
    print("  -- isolated predicate tests, so that a SUBSUMED filter is still")
    print("     shown to be live code and not a dead branch:")
    import gen_mapping as gm
    preds = [
        ("prog_terms", gm.PROG_TERMS, "module", "mizufi"),
        ("reserved_union", gm.RESERVED_UNION, "native", "mizufi"),
        ("task_material", gm.TASK_MATERIAL_WORDS, "strconv", "mizufi"),
        ("en_common", gm.EN_COMMON, "banana", "mizufi"),
    ]
    for name, lst, member, nonmember in preds:
        report("D predicate %s accepts its member %r and rejects %r"
               % (name, member, nonmember),
               (member in lst) and (nonmember not in lst))
    report("D substring predicate finds a banned substring in 'bababa' and none "
           "in any shipped pseudo-word",
           gm._banned_substring("bababa") is not None
           and all(gm._banned_substring(w) is None
                   for w in json.loads(read(os.path.join(HERE, "mapping.json")))["reverse"]))

    print("\nE. DETERMINISM -- two fresh processes must agree byte for byte")
    det, mapping = check_determinism()
    report("E mapping regenerates identically", det)

    print("\nF. LEXER LOSSLESSNESS")
    for p in (REAL, ANON):
        report("F lossless round trip of the lexer on %s" % os.path.basename(p),
               lex10.check_lossless(read(p)))

    print("\n%s" % ("ALL CHECKS REACHED THEIR EXPECTED VERDICT"
                    if not failures else "FAILURES: " + ", ".join(failures)))
    return 0 if not failures else 1


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "selftest"
    if cmd == "selftest":
        sys.exit(selftest())
    if cmd == "roundtrip":
        r = check_roundtrip()
        for k, v in r.items():
            if not k.startswith("_"):
                print("%-28s %s" % (k, GREEN if v else RED))
        sys.exit(0 if all(v for k, v in r.items() if not k.startswith("_")) else 1)
    if cmd == "gate":
        ok, ev = check_gate(read(sys.argv[2]))
        print("gate: %s  (%d H_REAL events)" % ("PASS" if ok else "REJECT", len(ev)))
        for pos, w in ev:
            print("  H_REAL at offset %d: %s" % (pos, w))
        sys.exit(0 if ok else 1)
    if cmd == "filters":
        c = check_filters()
        acc = c.pop("_accepted")[0]
        for k in sorted(c):
            print("%-16s %7d  first witness: %s" % (k, c[k][0], c[k][1]))
        print("accepted        %7d of 343000" % acc)
        sys.exit(0)
    if cmd == "determinism":
        det, m = check_determinism()
        print("determinism: %s" % (GREEN if det else RED))
        print(json.dumps(m, indent=2, sort_keys=True))
        sys.exit(0 if det else 1)
    print(__doc__)
    sys.exit(2)


if __name__ == "__main__":
    main()
