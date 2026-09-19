#!/usr/bin/env python3
"""
The harness half of the I1 pipeline for language_id = "zig", and the PF-13
"harness-convention sufficiency" test.

Pipeline (methodology 10 section 6.5):
    raw model output -> extract (9.2) -> gate (4.2a) -> inverse (6.2)
                     -> fixups (9.3) -> build -> run -> oracle (4.2)

The conventions this harness supplies, because the prompt withholds them
(section 9.1 -- stating them would identify the toolchain):

    entry filename   solution.zig
    build command    zig build-exe solution.zig -O ReleaseSafe -femit-bin=solution
    run command      ./solution
    working dir      a fresh directory per trial (H6: the emitted binary's name)

The trial is TOLD the entry filename and the build command in its prompt, so it is
never penalised for not guessing them; the harness applies them anyway, so a
submission that ignores them still builds.

What the harness does NOT supply, because the Reference Pack documents it
(section 9.3, "no fixup may supply anything the pack documents"):
    the entry-point shape, its visibility marker, its failure-propagating result
    designation, the standard-namespace binding, and the output sequence.
An absent one of those is a MODEL failure and the trial proceeds to build as
submitted.  H2 (compilation-unit declaration) does not apply: this language needs
none.  H3 (entry point inside a named type) does not apply.
"""

import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ZIG = shutil.which("zig") or "zig"

ENTRY_FILENAME = "solution.zig"
BUILD_CMD = [ZIG, "build-exe", ENTRY_FILENAME, "-O", "ReleaseSafe", "-femit-bin=solution"]
RUN_CMD = ["./solution"]

PACK_DOCUMENTED = ("entry_point_shape", "visibility_marker", "failure_designation",
                   "standard_namespace_binding", "output_sequence")


def _load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


lex10 = _load("lex10")
forward_mod = _load("forward")
inverse_mod = _load("inverse")
MAP_DOC, FWD = forward_mod.load_mapping()
REV = dict(MAP_DOC["reverse"])

FENCE = re.compile(r"```[^\n]*\n(.*?)```", re.S)


def extract(raw):
    """Section 9.2, frozen: the content of the LAST fenced code block if any fenced
    block exists, otherwise the entire output.  A leading language tag on the fence
    is ignored, never used as a signal."""
    blocks = FENCE.findall(raw)
    if blocks:
        return blocks[-1], len(blocks)
    return raw, 0


def gate_i1(text):
    bound_real = set(FWD.keys())
    bound_pseudo = set(REV.keys())
    words = [t for t in lex10.lex(text) if t.kind == "WORD" and not t.builtin]
    leaked = sorted(set(t.text for t in words if t.text in bound_real))
    if leaked:
        return False, "REAL_TOKENS_PRESENT:" + ",".join(leaked)
    if not any(t.text in bound_pseudo for t in words):
        return False, "NO_TRANSFORMED_TOKEN_PRESENT"
    return True, "ok"


def fixups(real_source):
    """Section 9.3.  Returns (source, applied) where `applied` splits into the two
    published columns.  H1 and H6 are file-system facts; they never touch the text."""
    applied = {"pack_documented": [], "unstated_convention": ["H1_entry_filename",
                                                             "H6_emitted_binary_name"]}
    return real_source, applied


def run_pipeline(raw_output, workdir, expected_path=None):
    os.makedirs(workdir, exist_ok=True)
    code, nblocks = extract(raw_output)

    accepted, why = gate_i1(code)
    if not accepted:
        return {"stage": "gate", "ok": False, "reason": why, "fenced_blocks": nblocks}

    ambiguous = inverse_mod.ambiguous_names(code)
    if ambiguous:
        return {"stage": "inverse", "ok": False, "reason": "INVERSE_AMBIGUOUS:%s"
                % ",".join(ambiguous), "fenced_blocks": nblocks}

    real = inverse_mod.inverse(code, REV, None)
    real, applied = fixups(real)

    with open(os.path.join(workdir, ENTRY_FILENAME), "w", encoding="utf-8") as fh:
        fh.write(real)
    b = subprocess.run(BUILD_CMD, cwd=workdir, capture_output=True, text=True)
    if b.returncode != 0:
        return {"stage": "build", "ok": False, "reason": b.stderr.strip()[:400],
                "fixups": applied, "fenced_blocks": nblocks}
    r = subprocess.run(RUN_CMD, cwd=workdir, capture_output=True, text=True)

    expected = None
    if expected_path:
        with open(expected_path, "r", encoding="utf-8") as fh:
            expected = fh.read()
    oracle = (r.returncode == 0 and (expected is None or r.stdout == expected))
    return {"stage": "oracle", "ok": oracle, "exit_status": r.returncode,
            "stdout": r.stdout, "fixups": applied, "fenced_blocks": nblocks,
            "reason": "" if oracle else "OUTPUT_MISMATCH_OR_NONZERO_EXIT"}


# --------------------------------------------------------------------- PF-13


BARE_BODY_PREAMBLE = """Here is my solution.

```
"""


def pf13(workdir, evidence=None):
    """PF-13: a BARE-BODY submission built from the pack alone.

    It contains every element the pack documents -- the standard-namespace binding,
    the visibility marker, the entry-point shape, its failure-propagating result
    designation, the output sequence -- in their I1-transformed spelling, and omits
    only the conventions the pack does NOT state: the file name and the emitted
    binary's name.  It must pass with ZERO pack-documented fixups applied.
    """
    with open(os.path.join(HERE, "fixture_anon.zig"), "r", encoding="utf-8") as fh:
        body = fh.read()
    raw = BARE_BODY_PREAMBLE + body + "```\n\nHope that helps!\n"
    res = run_pipeline(raw, workdir, os.path.join(HERE, "expected_output.txt"))
    res["pack_documented_fixups"] = res.get("fixups", {}).get("pack_documented", [])
    res["pass"] = bool(res["ok"]) and res["pack_documented_fixups"] == []
    if evidence:
        with open(os.path.join(evidence, "pf13_bare_body_submission.txt"), "w",
                  encoding="utf-8") as fh:
            fh.write(raw)
        with open(os.path.join(evidence, "pf13_result.json"), "w", encoding="utf-8") as fh:
            fh.write(json.dumps(res, indent=2) + "\n")
    return res


def extractor_selftest():
    """Section 9.2 exercised with zero, one and several fenced blocks (PF-05)."""
    cases = [
        ("zero", "const a = 1;\n", "const a = 1;\n", 0),
        ("one", "text\n```zig\nA\n```\nmore\n", "A\n", 1),
        ("several", "```\nFIRST\n```\nand\n```zig\nLAST\n```\n", "LAST\n", 2),
    ]
    out = []
    for name, raw, want, nb in cases:
        got, n = extract(raw)
        out.append({"case": name, "pass": got == want and n == nb,
                    "got": got, "blocks": n})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pf13", action="store_true")
    ap.add_argument("--extractor", action="store_true")
    ap.add_argument("--work", default=os.path.join(HERE, ".work_harness"))
    ap.add_argument("--evidence", default=None)
    args = ap.parse_args()

    ev = args.evidence
    if ev:
        ev = ev if os.path.isabs(ev) else os.path.join(HERE, ev)
        os.makedirs(ev, exist_ok=True)

    rc = 0
    if args.extractor:
        for r in extractor_selftest():
            print("  [%s] extractor[%s] blocks=%d" %
                  ("PASS" if r["pass"] else "FAIL", r["case"], r["blocks"]))
            rc |= 0 if r["pass"] else 1
    if args.pf13:
        res = pf13(os.path.join(args.work, "pf13"), ev)
        print("  [%s] PF-13 bare-body submission: stage=%s exit=%s "
              "pack_documented_fixups=%s unstated=%s"
              % ("PASS" if res["pass"] else "FAIL", res["stage"],
                 res.get("exit_status"), res["pack_documented_fixups"],
                 res.get("fixups", {}).get("unstated_convention")))
        print("      stdout:")
        for line in (res.get("stdout") or res.get("reason", "")).rstrip("\n").split("\n"):
            print("        " + line)
        rc |= 0 if res["pass"] else 1
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
