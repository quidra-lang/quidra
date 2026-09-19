#!/usr/bin/env python3
"""Build+run driver for the Go adversarial chunk 2 (ADV-14 .. ADV-26).

Frozen recipe (environment.json -> frozen_toolchain_recipes.go):
    build: go build -o BIN FILE.go
    run:   ./BIN
Build timeout 300 s, run timeout 60 s, 5 repetitions (08_adversarial_cases.json
-> execution_environment).
"""
import json, os, subprocess, sys, time

ROOT = "/private/tmp/claude-501/-Users-koba-Desktop-Quidra/c7dc7758-3bc7-4b38-ac54-4af31eae0682/scratchpad/bench/quidra-scratch/benchmark/2026-09-17-7677581"
SRC = os.path.join(ROOT, "standard", "src", "adversarial")
GO = os.path.join(SRC, "go")
INP = os.path.join(SRC, "inputs")
OUT = "/private/tmp/claude-501/-Users-koba-Desktop-Quidra/c7dc7758-3bc7-4b38-ac54-4af31eae0682/scratchpad/advgo/out"
os.makedirs(OUT, exist_ok=True)

BASE_ENV = dict(os.environ)
for v in ("PYTHONUTF8", "PYTHONIOENCODING", "RUST_BACKTRACE",
          "JAVA_TOOL_OPTIONS", "NODE_OPTIONS", "GOFLAGS"):
    BASE_ENV.pop(v, None)
BASE_ENV["LC_ALL"] = "en_US.UTF-8"
BASE_ENV["LANG"] = "en_US.UTF-8"
BASE_ENV["TZ"] = "UTC"
BASE_ENV["GOFLAGS"] = ""


def go_build(src, binpath, timeout=300):
    cmd = ["go", "build", "-o", binpath, src]
    t0 = time.time()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, env=BASE_ENV,
                           cwd=GO, timeout=timeout)
        return {"cmd": cmd, "exit": p.returncode, "stdout": p.stdout,
                "stderr": p.stderr, "timed_out": False,
                "wall": round(time.time() - t0, 2),
                "artifact_produced": os.path.exists(binpath)}
    except subprocess.TimeoutExpired as e:
        return {"cmd": cmd, "exit": None, "stdout": e.stdout or "",
                "stderr": e.stderr or "", "timed_out": True,
                "wall": round(time.time() - t0, 2), "artifact_produced": False}


def run_once(binpath, stdin_path, cwd, timeout=60):
    fin = open(stdin_path, "rb") if stdin_path else open(os.devnull, "rb")
    t0 = time.time()
    try:
        p = subprocess.run([binpath], stdin=fin, capture_output=True,
                           env=BASE_ENV, cwd=cwd, timeout=timeout)
        rc, so, se, to = p.returncode, p.stdout, p.stderr, False
    except subprocess.TimeoutExpired as e:
        rc, so, se, to = None, (e.stdout or b""), (e.stderr or b""), True
    finally:
        fin.close()
    return {"exit": rc, "stdout": so.decode("utf-8", "replace"),
            "stderr": se.decode("utf-8", "replace"), "timed_out": to,
            "signal": (-rc if rc is not None and rc < 0 else None),
            "wall": round(time.time() - t0, 2)}


def obs_of(stdout):
    for line in stdout.splitlines():
        if line.startswith("OBS="):
            return line[4:]
    return None


def measure(name, srcfile, stdin_name=None, cwd=None, reps=5, run_timeout=60,
            build_timeout=300):
    src = os.path.join(GO, srcfile)
    binpath = os.path.join(OUT, name.replace("/", "_"))
    if os.path.exists(binpath):
        os.remove(binpath)
    b = go_build(src, binpath, build_timeout)
    rec = {"name": name, "program": src, "build": b, "runs": []}
    if b["exit"] == 0 and b["artifact_produced"]:
        stdin_path = os.path.join(INP, stdin_name) if stdin_name else None
        for i in range(reps):
            rec["runs"].append(run_once(binpath, stdin_path, cwd or GO, run_timeout))
    return rec


CASES = [
    ("ADV-14/R", "ADV-14_R.go", "ADV-14.in", None),
    ("ADV-15",   "ADV-15.go",   None,        None),
    ("ADV-16",   "ADV-16.go",   None,        None),
    ("ADV-17",   "ADV-17.go",   None,        None),
    ("ADV-18",   "ADV-18.go",   "ADV-18.in", None),
    ("ADV-19",   "ADV-19.go",   "ADV-19.in", None),
    ("ADV-20",   "ADV-20.go",   "ADV-20.in", None),
    ("ADV-21",   "ADV-21.go",   None,        None),
    ("ADV-21/d1000",  "ADV-21_depth1000.go",  None, None),
    ("ADV-21/d10000", "ADV-21_depth10000.go", None, None),
    ("ADV-22a",  "ADV-22a.go",  None,        None),
    ("ADV-22b",  "ADV-22b.go",  None,        None),
    ("ADV-22-valid", "ADV-22-valid.go", None, None),
    ("ADV-23",   "ADV-23.go",   None,        SRC),
    ("ADV-24",   "ADV-24.go",   None,        None),
    ("ADV-25/R", "ADV-25_R.go", "ADV-25.in", None),
    ("ADV-26/R", "ADV-26_R.go", "ADV-26.in", None),
]

if __name__ == "__main__":
    only = sys.argv[1:] or None
    results = []
    for name, srcfile, stdin_name, cwd in CASES:
        if only and name not in only:
            continue
        sys.stderr.write("== %s\n" % name)
        sys.stderr.flush()
        reps = 5
        r = measure(name, srcfile, stdin_name, cwd, reps=reps)
        results.append(r)
        sys.stderr.write("   build exit=%s  runs=%d\n" % (r["build"]["exit"], len(r["runs"])))
        sys.stderr.flush()
    print(json.dumps(results, indent=1))
