#!/usr/bin/env python3
"""
Trial verification oracle.

Takes a candidate source file for one of the ten languages, builds it, runs it,
and classifies the outcome mechanically. Used identically by the Standard
workloads, the LLM Practical track and the LLM Intrinsic track, so a "pass" means
the same thing everywhere.

The classification that matters most is SILENT_BUG: a program that builds and
runs to a clean exit but produces a wrong answer. Spec sections 13 and 17 require
that this be recorded as a silent bug rather than as a success, and it is the
single easiest thing for a benchmark to get wrong by accident.
"""

from __future__ import annotations

import json
import math
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import langs  # noqa: E402

# Outcome classes.
BUILD_FAILURE = "build_failure"        # never produced a runnable artifact
RUNTIME_FAILURE = "runtime_failure"    # ran, exited non-zero / crashed / timed out
SILENT_BUG = "silent_bug"              # ran cleanly, wrong answer
PASS = "pass"                          # ran cleanly, right answer
HARNESS_ERROR = "harness_error"        # our fault, never a language result


def _floats(text):
    """Extract every numeric literal from text, in order."""
    return [float(m) for m in re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", text)]


def compare_output(actual, expected, tolerance=None, mode="exact"):
    """Compare program output to the expected output.

    mode="exact"     : whitespace-normalized string equality
    mode="numeric"   : every numeric token must agree within `tolerance`
                       (absolute), and the count of numbers must match
    Returns (ok, detail).
    """
    a = actual.strip()
    e = expected.strip()
    if mode == "exact":
        an = "\n".join(l.rstrip() for l in a.splitlines())
        en = "\n".join(l.rstrip() for l in e.splitlines())
        return an == en, {"mode": "exact", "equal": an == en}

    if mode == "numeric":
        av, ev = _floats(a), _floats(e)
        if len(av) != len(ev):
            return False, {"mode": "numeric", "reason": "different count of numeric values",
                           "actual_count": len(av), "expected_count": len(ev)}
        tol = 0.0 if tolerance is None else float(tolerance)
        worst, worst_i = 0.0, -1
        for i, (x, y) in enumerate(zip(av, ev)):
            if math.isnan(x) and math.isnan(y):
                continue
            if math.isinf(x) or math.isinf(y):
                if x != y:
                    return False, {"mode": "numeric", "reason": "infinity mismatch", "index": i}
                continue
            d = abs(x - y)
            if d > worst:
                worst, worst_i = d, i
        ok = worst <= tol
        return ok, {"mode": "numeric", "tolerance": tol, "max_abs_diff": worst,
                    "worst_index": worst_i}

    raise ValueError(f"unknown compare mode {mode!r}")


def verify(target, src, workdir, expected_stdout, stem=None, timeout=300,
           tolerance=None, compare_mode="exact", stdin_path=None, run_args=None,
           main_class="Main"):
    """Build + run + classify one candidate implementation.

    Returns a dict with the outcome class, every raw artifact needed to audit
    the decision, and the build/run logs that a repair turn would be shown.
    """
    stem = stem or ("Main" if target == "java" else "prog")
    os.makedirs(workdir, exist_ok=True)
    result = {
        "target": target,
        "language_column": langs.TARGETS[target]["column"],
        "source_path": src,
        "outcome": None,
        "build": None,
        "run": None,
        "correctness": None,
    }

    # --- build -------------------------------------------------------------
    try:
        b = langs.build(target, src, workdir, stem, timeout=timeout)
    except subprocess.TimeoutExpired:
        result["outcome"] = BUILD_FAILURE
        result["build"] = {"ok": False, "timed_out": True,
                           "diagnostics": f"build exceeded {timeout}s"}
        return result
    except Exception as e:  # noqa: BLE001
        result["outcome"] = HARNESS_ERROR
        result["build"] = {"ok": False, "harness_error": str(e)}
        return result

    result["build"] = {
        "ok": b["ok"],
        "needs_build": b["needs_build"],
        "cmd": b.get("cmd"),
        "exit_code": b.get("exit_code"),
        "diagnostics": ((b.get("stderr") or "") + (b.get("stdout") or "")).strip(),
    }
    if not b["ok"]:
        result["outcome"] = BUILD_FAILURE
        return result

    # --- run ---------------------------------------------------------------
    cmd = langs.run_cmd(target, src, workdir, stem, main_class=main_class)
    if run_args:
        cmd = cmd + list(run_args)
    stdin_f = open(stdin_path, "rb") if stdin_path else subprocess.DEVNULL
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                           env=langs.env_for(target), stdin=stdin_f,
                           cwd=os.path.dirname(src) or None)
        run = {"ok": p.returncode == 0, "exit_code": p.returncode,
               "stdout": p.stdout, "stderr": p.stderr, "timed_out": False}
    except subprocess.TimeoutExpired as e:
        run = {"ok": False, "exit_code": None, "timed_out": True,
               "stdout": (e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or ""),
               "stderr": f"execution exceeded {timeout}s"}
    except Exception as e:  # noqa: BLE001
        result["outcome"] = HARNESS_ERROR
        result["run"] = {"harness_error": str(e)}
        return result
    finally:
        if stdin_path:
            stdin_f.close()

    result["run"] = run
    if not run["ok"]:
        result["outcome"] = RUNTIME_FAILURE
        return result

    # --- correctness -------------------------------------------------------
    ok, detail = compare_output(run["stdout"], expected_stdout, tolerance, compare_mode)
    result["correctness"] = detail
    # Clean exit + wrong answer is the case the spec singles out: it is a silent
    # bug, never a success.
    result["outcome"] = PASS if ok else SILENT_BUG
    return result


def diagnostics_for_repair(result, max_chars=4000):
    """The real compiler/runtime evidence fed back to a model on a repair turn.

    Spec section 9 requires repairs be driven by ACTUAL diagnostics, errors or
    test results -- never by a human explaining the fix.
    """
    o = result.get("outcome")
    if o == BUILD_FAILURE:
        d = (result.get("build") or {}).get("diagnostics", "")
        return f"The program failed to build. Compiler output:\n\n{d[:max_chars]}"
    if o == RUNTIME_FAILURE:
        r = result.get("run") or {}
        return (f"The program built but failed at runtime (exit code {r.get('exit_code')}"
                f"{', timed out' if r.get('timed_out') else ''}).\n\n"
                f"stderr:\n{(r.get('stderr') or '')[:max_chars]}\n\n"
                f"stdout so far:\n{(r.get('stdout') or '')[:1000]}")
    if o == SILENT_BUG:
        r = result.get("run") or {}
        c = result.get("correctness") or {}
        return ("The program built and ran to a clean exit, but produced the wrong result.\n\n"
                f"Actual output:\n{(r.get('stdout') or '')[:max_chars]}\n\n"
                f"Comparison detail: {json.dumps(c)}")
    return ""


if __name__ == "__main__":
    # Self-test: prove the oracle can BOTH pass a correct program AND reject a
    # wrong one, in every language. Spec 10.4 pre-flight requirement (c): a
    # validator that cannot fail is measuring nothing.
    import tempfile

    good = {
        "quidra":        'print("42")\n',
        "quidra_interp": 'print("42")\n',
        "python":        'print(42)\n',
        "cpp":           '#include <cstdio>\nint main(){printf("42\\n");}\n',
        "rust":          'fn main(){println!("42");}\n',
        "go":            'package main\nimport "fmt"\nfunc main(){fmt.Println(42)}\n',
        "java":          'public class Main{public static void main(String[] a){System.out.println(42);}}\n',
        "typescript":    'console.log(42);\n',
        "kotlin":        'fun main(){println(42)}\n',
        "swift":         'print(42)\n',
        "zig":           ('const std = @import("std");\npub fn main() !void {\n'
                          '    var t = std.Io.Threaded.init_single_threaded;\n'
                          '    try std.Io.File.stdout().writeStreamingAll(t.io(), "42\\n");\n}\n'),
    }
    root = tempfile.mkdtemp(prefix="verify_selftest_")
    report = {}
    for tgt, code in good.items():
        ext = langs.TARGETS[tgt]["ext"]
        stem = "Main" if tgt == "java" else "prog"

        d1 = os.path.join(root, tgt, "good")
        os.makedirs(d1, exist_ok=True)
        s1 = os.path.join(d1, stem + ext)
        open(s1, "w").write(code)
        r_pass = verify(tgt, s1, d1, "42", stem=stem)

        # Same program, wrong expectation -> must be classified SILENT_BUG.
        d2 = os.path.join(root, tgt, "wrong")
        os.makedirs(d2, exist_ok=True)
        s2 = os.path.join(d2, stem + ext)
        open(s2, "w").write(code)
        r_fail = verify(tgt, s2, d2, "99", stem=stem)

        report[tgt] = {
            "correct_program_passes": r_pass["outcome"] == PASS,
            "wrong_answer_detected": r_fail["outcome"] == SILENT_BUG,
            "pass_outcome": r_pass["outcome"],
            "fail_outcome": r_fail["outcome"],
        }

    print(json.dumps(report, indent=2))
    bad = [k for k, v in report.items()
           if not (v["correct_program_passes"] and v["wrong_answer_detected"])]
    print(("ORACLE OK: passes correct input and rejects corrupted input in all targets"
           if not bad else "ORACLE DEFECTIVE IN: " + ", ".join(bad)), file=sys.stderr)
    sys.exit(1 if bad else 0)
