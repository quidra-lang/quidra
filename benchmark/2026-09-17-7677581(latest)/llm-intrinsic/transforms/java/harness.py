#!/usr/bin/env python3
"""The harness path a scored I1/java trial takes, end to end.

  raw model output
    -> code extraction            (methodology 10 section 9.2: last fenced block)
    -> conformance gate           (section 4.2a, condition I1)
    -> inverse mapping            (section 6.5 step 3)
    -> harness fixups H1/H6       (section 9.3: unstated conventions ONLY)
    -> build + run with the frozen recipe
    -> oracle                     (section 4.2)

The two facts this file supplies on the model's behalf are the entry FILE NAME and the
BUILD/RUN COMMAND.  Both are facts about this benchmark's file system and invocation,
not about the language, so section 9.1 makes them the harness's responsibility.  Nothing
the Reference Pack documents is ever supplied here.

Usage:  harness.py RAW_OUTPUT_FILE [--expected expected_output.txt]
"""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from inverse import inverse  # noqa: E402
from validate import gate  # noqa: E402

JAVA_HOME = "/opt/homebrew/opt/openjdk"

# ---- the unstated conventions the harness supplies (section 9.1) ------------- #
ENTRY_FILENAME = "Main.java"                       # H1
BUILD_CMD = [os.path.join(JAVA_HOME, "bin", "javac"),
             "-J-Duser.language=en", "-J-Duser.country=US",
             "-d", "{OUT}", "{FILE}"]              # frozen recipe, methodology 10 section 0.1
RUN_CMD = [os.path.join(JAVA_HOME, "bin", "java"), "-cp", "{OUT}", "Main"]
OUT_DIR_NAME = "out"                               # H6


def extract_code(raw):
    """Frozen extraction rule: the LAST fenced block, else the whole output."""
    if "```" not in raw:
        return raw
    parts = raw.split("```")
    if len(parts) < 3:
        return raw
    block = parts[-2]
    lines = block.split("\n")
    if lines and lines[0].strip() and " " not in lines[0].strip():
        lines = lines[1:]          # drop a language tag line, never used as a signal
    code = "\n".join(lines)
    return code.lstrip("\n")


def run_trial(raw, expected=None):
    rec = {}
    code = extract_code(raw)
    rec["extracted_chars"] = len(code)

    ok, events = gate(code)
    rec["gate_conformant"] = ok
    rec["gate_events"] = sorted(set(events))
    if not ok:
        rec["verdict"] = "FAIL"
        rec["reason"] = "CONFORMANT check failed (section 4.2a)"
        return rec

    real = inverse(code)
    rec["inverse_substitutions"] = inverse.last_changed

    d = tempfile.mkdtemp(prefix="i1java-harness-")
    try:
        src = os.path.join(d, ENTRY_FILENAME)           # H1
        with open(src, "w", encoding="utf-8") as f:
            f.write(real if real.endswith("\n") else real + "\n")
        out = os.path.join(d, OUT_DIR_NAME)             # H6
        os.makedirs(out, exist_ok=True)
        bc = [a.replace("{OUT}", out).replace("{FILE}", src) for a in BUILD_CMD]
        rec["build_cmd"] = " ".join(bc)
        p = subprocess.run(bc, capture_output=True, text=True)
        rec["build_exit"] = p.returncode
        rec["build_stderr"] = p.stderr.strip()
        if p.returncode != 0:
            rec["verdict"] = "FAIL"
            rec["reason"] = "build failed"
            return rec
        rc = [a.replace("{OUT}", out) for a in RUN_CMD]
        rec["run_cmd"] = " ".join(rc)
        r = subprocess.run(rc, capture_output=True, text=True, timeout=10)
        rec["run_exit"] = r.returncode
        rec["stdout"] = r.stdout
        rec["stderr"] = r.stderr.strip()
    finally:
        shutil.rmtree(d, ignore_errors=True)

    if expected is not None:
        rec["stdout_byte_identical"] = (r.stdout == expected)
        rec["verdict"] = ("PASS" if (rec["run_exit"] == 0 and rec["stdout_byte_identical"]
                                     and not rec["stderr"]) else "FAIL")
    else:
        rec["verdict"] = "PASS" if rec["run_exit"] == 0 else "FAIL"
    return rec


def main(argv):
    raw = open(argv[1], encoding="utf-8").read()
    expected = None
    if "--expected" in argv:
        expected = open(argv[argv.index("--expected") + 1], encoding="utf-8").read()
    rec = run_trial(raw, expected)
    for k, v in rec.items():
        if k == "stdout":
            print("stdout:")
            for line in v.splitlines():
                print("    | " + line)
        else:
            print("%-24s %s" % (k + ":", v))
    return 0 if rec["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
