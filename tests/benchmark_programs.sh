#!/usr/bin/env bash
# Audit the Quidra benchmark programs under tests/benchmark/quidra against the
# compiler built from the current tree.
#
#   tests/benchmark_programs.sh [path/to/quidra]
#
# The compiler defaults to build/quidra. The micro suite is built and run once
# in `once` mode and validated against the frozen oracle; the adversarial
# programs are built (a build rejection is a legitimate outcome for several of
# them, so only the skeleton and the generated sources are checked here); the
# two JSON files are validated for shape. This is the same audit the benchmark
# runner performs before it measures anything.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QUIDRA="${1:-$ROOT/build/quidra}"
PROGRAMS="$ROOT/tests/benchmark/quidra"
TEMPLATE="$ROOT/benchmark/template"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$QUIDRA" ]; then
  echo "quidra compiler not found at $QUIDRA (build the tree first)" >&2
  exit 1
fi

fail=0
note() { printf '%s\n' "$*"; }

# --- micro suite -----------------------------------------------------------
python3 - "$ROOT" "$QUIDRA" "$WORK" "$TEMPLATE" <<'PY' || fail=1
import importlib.util, os, subprocess, sys, time
from pathlib import Path

root, quidra, work, template = (Path(a) for a in sys.argv[1:5])
sys.path.insert(0, str(template / "scripts"))
spec = importlib.util.spec_from_file_location("mm", template / "scripts" / "micro_measure.py")
mm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mm)

# expected_outputs()/checker_module() read the template through a workspace
# root; a minimal one pointing at the real template is enough here.
ws = work / "ws"
ws.mkdir(parents=True, exist_ok=True)
if not (ws / "template").exists():
    (ws / "template").symlink_to(template, target_is_directory=True)
expected = mm.expected_outputs(ws)
checker = mm.checker_module(ws)

src = root / "tests" / "benchmark" / "quidra" / "micro"
problems = []
for name in ["mb00"] + [f"mb{i:02d}" for i in range(1, 12)]:
    cwd = work / name
    cwd.mkdir(parents=True, exist_ok=True)
    build = subprocess.run([str(quidra), "build", str(src / f"{name}.qui"), "-o", str(cwd / name)],
                           capture_output=True, text=True, cwd=cwd)
    if build.returncode != 0:
        problems.append(f"{name}: build failed: {(build.stderr or build.stdout).strip()[:400]}")
        continue
    started = time.time()
    run = subprocess.run([str(cwd / name)], capture_output=True, text=True, cwd=cwd, timeout=1800)
    elapsed = time.time() - started
    if run.returncode != 0:
        problems.append(f"{name}: exit {run.returncode}: {(run.stderr or run.stdout).strip()[:400]}")
        continue
    if name == "mb00":
        try:
            mm.validate_startup_output(run.stdout)
        except Exception as exc:
            problems.append(f"{name}: {exc}")
        continue
    try:
        mm.validate_output(checker, expected[name], run.stdout)
    except Exception as exc:
        problems.append(f"{name}: {exc}")
        continue
    print(f"  {name}: ok ({elapsed:.1f}s)")
    # steady mode must print ITER lines and the same result line.
    steady = subprocess.run([str(cwd / name), "steady", "1"], capture_output=True, text=True,
                            cwd=cwd, timeout=1800)
    lines = steady.stdout.strip().splitlines()
    if steady.returncode != 0 or not lines or not lines[0].startswith("ITER 0 "):
        problems.append(f"{name}: steady mode did not print ITER lines: {steady.stdout[:200]!r}")
    else:
        try:
            mm.validate_output(checker, expected[name], lines[-1] + "\n")
        except Exception as exc:
            problems.append(f"{name}: steady result differs: {exc}")
if problems:
    print("micro suite problems:")
    for p in problems:
        print("  - " + p)
    sys.exit(1)
print("micro suite: all 12 programs build, run and match the frozen oracle")
PY

# --- adversarial programs --------------------------------------------------
python3 "$PROGRAMS/adversarial/generate.py" --check || fail=1
python3 - "$PROGRAMS" "$QUIDRA" "$WORK" "$TEMPLATE" <<'PY' || fail=1
import json, re, subprocess, sys
from pathlib import Path

programs, quidra, work, template = (Path(a) for a in sys.argv[1:5])
asset = json.loads((template / "methodology-assets" / "language_quality" / "adversarial_cases.json").read_text())
amendment = json.loads((programs / "quidra_type_binding_amendment.json").read_text())
tm3a = {k for k, v in amendment.get("tm3_determinations", {}).items() if v.get("branch") == "TM3a"}

def stems(program_id):
    base, _, variant = program_id.partition("/")
    return [f"{base}_{variant}"] if variant else [base]

problems = []
scored = [p for row in asset["fixed_scored_case_variant_list"]["rows"] for p in row["programs"]]
for program in scored:
    if program.split("/")[0].split("_")[0] in tm3a or program in tm3a:
        continue
    path = next((programs / "adversarial" / f"{s}.qui" for s in stems(program)
                 if (programs / "adversarial" / f"{s}.qui").is_file()), None)
    if path is None:
        problems.append(f"{program}: no source under adversarial/")
        continue
    text = path.read_text(encoding="utf-8", errors="replace")
    if program not in ("ADV-22a", "ADV-22b"):
        for marker in ('print("ADV-START")', 'print("ADV-END")', "OBS="):
            if marker not in text:
                problems.append(f"{program}: skeleton line {marker!r} is missing")
    out = work / "adv" / path.stem
    out.mkdir(parents=True, exist_ok=True)
    build = subprocess.run([str(quidra), "build", str(path), "-o", str(out / "bin")],
                           capture_output=True, text=True, cwd=out, timeout=300)
    verdict = "builds" if build.returncode == 0 else "rejected by the compiler"
    print(f"  {program}: {verdict}")

for name in ("quidra_type_binding_amendment.json", "representation.json"):
    data = json.loads((programs / name).read_text())
    if data.get("schema_version") != 1:
        problems.append(f"{name}: schema_version must be 1")
required = ["numeric_arrays", "string_buffer", "file_io", "collections", "monotonic_clock",
            "runtime_checks", "documentation_evidence"]
rep = json.loads((programs / "representation.json").read_text())
for key in required:
    if not rep.get(key):
        problems.append(f"representation.json: {key} is missing or empty")
docs_root = programs.parents[2] / "docs"
for item in rep.get("documentation_evidence", []):
    rel = item.split("#", 1)[0]
    if not rel.startswith("repo/docs/") or not (docs_root / rel[len("repo/docs/"):]).is_file():
        problems.append(f"representation.json: documentation evidence does not name a snapshot file: {item}")
for key in ("default_arithmetic_type", "fixed_width_i64", "fixed_width_i32", "fixed_width_u32",
            "float64", "default_string_type", "default_ordered_sequence", "most_general_reference",
            "null_or_absent_value"):
    row = amendment.get("bindings", {}).get(key)
    if not row or not row.get("construct") or not row.get("citation"):
        problems.append(f"amendment: binding {key} lacks a construct or citation")
if problems:
    print("adversarial problems:")
    for p in problems:
        print("  - " + p)
    sys.exit(1)
print("adversarial set: every scored program is present with the frozen skeleton; JSON pins validated")
PY

if [ "$fail" -ne 0 ]; then
  echo "benchmark programs audit FAILED" >&2
  exit 1
fi
echo "benchmark programs audit passed"
