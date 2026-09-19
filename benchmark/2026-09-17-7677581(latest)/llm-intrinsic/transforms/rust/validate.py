#!/usr/bin/env python3
"""Round-trip / conformance validator for the I1 'rust' column.

Checks implemented (methodology 10_intrinsic_design.md section 6.4 and 4.2a):

  M1  mapping integrity: one-to-one, exact length 6, ^[a-z]{6}$, no pseudo-word
      is a prefix of another, no pseudo-word equals a real token
  R1  byte identity          inverse(forward(F), posmap) == F
  R4  non-triviality         forward(F) differs from F in >= 1 token
  R5  domain containment     every changed token is a bound keyword
  R2  build identity         inverse(forward(F)) builds, exit 0
  R3  behavioural identity   its stdout is byte-identical to the oracle
  G1  conformance gate       the submission contains no real reserved word and
                             no real bound token (other than pack-documented
                             untransformed library names)
  G2  inverse-ambiguity      no pseudo-word is used in a declaration-name
                             position (that would be an identifier/keyword clash)

Exit status 0 iff every requested check passed.

  python3 validate.py --source fixture_anon.rs [--expect-pass|--expect-reject]
"""
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lex10          # noqa: E402
import forward as fwd  # noqa: E402
import inverse as inv  # noqa: E402

BUILD_CMD = ["rustc", "-O", "-C", "debug-assertions=on", "-C", "overflow-checks=on"]
ENTRY_FILENAME = "solution.rs"
RUN_TIMEOUT_S = 10

MAPPING = fwd.load_mapping(sys.argv[sys.argv.index("--mapping") + 1]
                           if "--mapping" in sys.argv else None)
REAL_TOKENS = set(MAPPING["forward"].keys())
PSEUDO = set(MAPPING["inverse"].keys())
DOCUMENTED_UNTRANSFORMED = set(MAPPING["untransformed_by_design"]["tokens"])
RESERVED_REAL = set(
    l.strip() for l in open(os.path.join(HERE, "wordlists", "reserved_rust.txt"))
    if l.strip())


class Result(object):
    def __init__(self):
        self.rows = []
        self.ok = True

    def add(self, cid, passed, detail):
        self.rows.append((cid, passed, detail))
        if not passed:
            self.ok = False

    def report(self):
        for cid, passed, detail in self.rows:
            print("  %-4s %-6s %s" % (cid, "PASS" if passed else "FAIL", detail))


# ------------------------------------------------------------------ M1

def check_mapping(res):
    pws = list(MAPPING["forward"].values())
    res.add("M1a", len(set(pws)) == len(pws),
            "one-to-one: %d pseudo-words, %d distinct" % (len(pws), len(set(pws))))
    bad = [w for w in pws if not re.match(r"^[a-z]{6}$", w)]
    res.add("M1b", not bad, "shape ^[a-z]{6}$: %s" % ("all conform" if not bad else bad))
    pref = [(a, b) for a in pws for b in pws if a != b and a.startswith(b)]
    res.add("M1c", not pref, "no prefix relation: %s" % ("none" if not pref else pref))
    clash = [w for w in pws if w in REAL_TOKENS or w in RESERVED_REAL]
    res.add("M1d", not clash, "no pseudo-word collides with a real token: %s"
            % ("none" if not clash else clash))
    lens = set(len(w) for w in pws)
    res.add("M1e", lens == {6}, "comparable length: character lengths = %s" % sorted(lens))


# ------------------------------------------------------------------ G1 gate

def conformance_gate(source):
    """Returns the list of H_REAL events. Empty list == gate passes."""
    forbidden = (RESERVED_REAL | REAL_TOKENS) - DOCUMENTED_UNTRANSFORMED
    return [w for w in lex10.word_tokens(source) if w in forbidden]


# ------------------------------------------------------------------ G2

DECL_INTRODUCERS = set()
for rk in ("K01", "K02", "K24"):
    if rk in MAPPING["roles"]:
        DECL_INTRODUCERS.add(MAPPING["roles"][rk]["pseudo_word"])


# the entry point's own name is a bound token that legitimately stands in a
# name position; it is the one documented exception to the G2 rule.
NAME_POSITION_ALLOWED = set()
if "K22" in MAPPING["roles"]:
    NAME_POSITION_ALLOWED.add(MAPPING["roles"]["K22"]["pseudo_word"])


def inverse_ambiguity(source):
    """A pseudo-word standing in a declaration-name position is a model naming a
    variable or function with a keyword; the lexer-based inverse cannot tell the
    two apart, so the trial must be recorded INVERSE_AMBIGUOUS rather than
    silently mis-mapped."""
    words = lex10.word_tokens(source)
    hits = []
    for i, w in enumerate(words):
        if (w in PSEUDO and i > 0 and words[i - 1] in DECL_INTRODUCERS
                and w not in DECL_INTRODUCERS and w not in NAME_POSITION_ALLOWED):
            # the token after a declaration introducer is a name unless it is
            # itself another introducer (e.g. the mutability qualifier)
            hits.append(w)
    return hits


# ------------------------------------------------------------------ build+run

def build_and_run(real_source, tag):
    d = tempfile.mkdtemp(prefix="i1rust_%s_" % tag)
    src = os.path.join(d, ENTRY_FILENAME)
    binp = os.path.join(d, "solution")
    open(src, "w").write(real_source)
    b = subprocess.run(BUILD_CMD + [src, "-o", binp],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=d)
    if b.returncode != 0:
        return (False, b.returncode, "", b.stderr.decode("utf-8", "replace"), d)
    try:
        r = subprocess.run([binp], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=RUN_TIMEOUT_S, cwd=d)
    except subprocess.TimeoutExpired:
        return (False, None, "", "TIMEOUT after %ds" % RUN_TIMEOUT_S, d)
    return (r.returncode == 0, r.returncode, r.stdout.decode("utf-8", "replace"),
            r.stderr.decode("utf-8", "replace"), d)


# ------------------------------------------------------------------ driver


def validate_anon(anon_source, posmap_offsets, real_source=None, oracle=None, res=None):
    res = res or Result()
    oracle = oracle if oracle is not None else open(
        os.path.join(HERE, "expected_output.txt")).read()

    # G1 conformance gate on the transformed (pre-inverse) submission
    hits = conformance_gate(anon_source)
    res.add("G1", not hits, "conformance gate H_REAL events: %s"
            % ("none" if not hits else sorted(set(hits))))

    # G2 inverse ambiguity
    amb = inverse_ambiguity(anon_source)
    res.add("G2", not amb, "inverse ambiguity: %s" % ("none" if not amb else sorted(set(amb))))

    # inverse
    real = inv.inverse(anon_source, MAPPING, posmap_offsets)

    if real_source is not None:
        res.add("R1", real == real_source,
                "byte identity of inverse(forward(F)) against F: %s"
                % ("identical" if real == real_source else "DIFFERS"))
        changed = fwd.forward_detailed(real_source, MAPPING)[2]
        res.add("R4", len(changed) > 0, "non-triviality: %d tokens substituted" % len(changed))
        outside = [a for a, _b in changed if a not in REAL_TOKENS]
        res.add("R5", not outside, "domain containment: %s"
                % ("every changed token is a bound keyword" if not outside else outside))

    ok, rc, out, err, d = build_and_run(real, "rt")
    res.add("R2", ok, "build+run of the inverse-mapped source: exit=%s %s"
            % (rc, (err.strip().splitlines() or [""])[0][:140]))
    res.add("R3", out == oracle, "stdout byte-identical to the oracle: %s"
            % ("yes" if out == oracle else "NO -> %r" % out[:120]))
    return res


def _arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def main():
    src_path = _arg("--source", os.path.join(HERE, "fixture_anon.rs"))
    real_path = _arg("--real", os.path.join(HERE, "fixture_real.rs"))
    posmap_path = _arg("--posmap")
    expect_reject = "--expect-reject" in sys.argv

    offsets = None
    if posmap_path and os.path.exists(posmap_path):
        offsets = json.load(open(posmap_path))["inserted_whitespace_offsets"]

    anon = open(src_path).read()
    real_source = open(real_path).read() if (real_path and os.path.exists(real_path)
                                             and "--no-r1" not in sys.argv) else None

    res = Result()
    if "--no-mapping-check" not in sys.argv:
        check_mapping(res)
    validate_anon(anon, offsets, real_source, None, res)

    print("VALIDATOR REPORT for %s" % os.path.basename(src_path))
    res.report()
    verdict = "ACCEPT" if res.ok else "REJECT"
    print("VERDICT: %s" % verdict)
    if expect_reject:
        sys.exit(0 if not res.ok else 1)
    sys.exit(0 if res.ok else 1)


if __name__ == "__main__":
    main()
