#!/usr/bin/env python3
"""The I1 'rust' harness pipeline, used for the PF-13-style sufficiency check.

  raw model output
    -> code extraction (methodology 9.2: content of the LAST fenced block)
    -> conformance gate (4.2a)
    -> inverse mapping (6.5 step 3)
    -> harness conventions (9.3): file name, build command, output binary name
    -> build, run, oracle (4.2)

Every convention the prompt withholds is supplied here and is listed in
HARNESS_CONVENTIONS below.  None of them is a fact about the language: they are
facts about this benchmark's file system and invocation.

  python3 harness.py --submission raw_output.txt
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import inverse as inv      # noqa: E402
import validate as val     # noqa: E402

HARNESS_CONVENTIONS = {
    "entry_filename": "solution.rs",
    "build_command": "rustc -O -C debug-assertions=on -C overflow-checks=on solution.rs -o solution",
    "run_command": "./solution",
    "output_binary": "solution",
    "working_directory": "a fresh temporary directory per trial",
    "stated_to_the_trial": True,
}

FENCE = re.compile(r"```[^\n]*\n(.*?)```", re.S)


def extract(raw):
    blocks = FENCE.findall(raw)
    return blocks[-1] if blocks else raw


def run_pipeline(raw, verbose=True):
    steps = []
    code = extract(raw)
    steps.append(("extract", True, "%d fenced block(s); took the last one, %d bytes"
                  % (len(FENCE.findall(raw)), len(code))))

    hits = val.conformance_gate(code)
    steps.append(("gate", not hits,
                  "H_REAL events: %s" % ("none" if not hits else sorted(set(hits)))))
    if hits:
        return steps, False

    amb = val.inverse_ambiguity(code)
    steps.append(("ambiguity", not amb,
                  "INVERSE_AMBIGUOUS: %s" % ("none" if not amb else sorted(set(amb)))))
    if amb:
        return steps, False

    real = inv.inverse(code, val.MAPPING, None)
    steps.append(("inverse", True, "%d bytes of real source produced" % len(real)))

    d = tempfile.mkdtemp(prefix="i1rust_pf13_")
    src = os.path.join(d, HARNESS_CONVENTIONS["entry_filename"])
    open(src, "w").write(real)
    steps.append(("fixup H1", True, "wrote the submission to %s (unstated convention)"
                  % HARNESS_CONVENTIONS["entry_filename"]))
    steps.append(("fixups H2-H6", True, "none applied; 0 pack-documented fixups"))

    binp = os.path.join(d, HARNESS_CONVENTIONS["output_binary"])
    b = subprocess.run(["rustc", "-O", "-C", "debug-assertions=on",
                        "-C", "overflow-checks=on", src, "-o", binp],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=d)
    steps.append(("build", b.returncode == 0, "exit=%d stderr=%r"
                  % (b.returncode, b.stderr.decode()[:200])))
    if b.returncode != 0:
        return steps, False

    r = subprocess.run([binp], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=10, cwd=d)
    steps.append(("run", r.returncode == 0, "exit=%d stderr=%r"
                  % (r.returncode, r.stderr.decode()[:200])))

    oracle = open(os.path.join(HERE, "expected_output.txt")).read()
    got = r.stdout.decode()
    steps.append(("oracle", got == oracle,
                  "stdout byte-identical to expected_output.txt: %s"
                  % ("yes" if got == oracle else "NO -> %r" % got)))
    return steps, (r.returncode == 0 and got == oracle)


def main():
    p = sys.argv[sys.argv.index("--submission") + 1]
    raw = open(p).read()
    steps, ok = run_pipeline(raw)
    print("PIPELINE for %s" % os.path.basename(p))
    for name, good, detail in steps:
        print("  %-12s %-5s %s" % (name, "ok" if good else "FAIL", detail))
    print("RESULT: %s" % ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
