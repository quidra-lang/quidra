#!/usr/bin/env python3
"""Build+run driver for the TypeScript adversarial chunk 2 (ADV-14 .. ADV-26).

Frozen recipe (environment.json -> frozen_toolchain_recipes.typescript, and
methodology 08 -> toolchain_binding.typescript_note which fixes the BARE form):
    build: tsc FILE.ts          (no tsconfig.json in the build directory)
    run:   node FILE.js
Build timeout 300 s, run timeout 60 s, 5 repetitions
(08_adversarial_cases.json -> execution_environment).

Secondary NON-SCORING configuration (secondary_non_scoring_configurations.TypeScript):
    build: tsc --strict FILE.ts ; run: node FILE.js
"""
import json, os, shutil, subprocess, sys, time

ROOT = ("/private/tmp/claude-501/-Users-koba-Desktop-Quidra/"
        "c7dc7758-3bc7-4b38-ac54-4af31eae0682/scratchpad/bench/quidra-scratch/"
        "benchmark/2026-09-17-7677581")
SRC = os.path.join(ROOT, "standard", "src", "adversarial")
TSD = os.path.join(SRC, "typescript")
INP = os.path.join(SRC, "inputs")
WORK = ("/private/tmp/claude-501/-Users-koba-Desktop-Quidra/"
        "c7dc7758-3bc7-4b38-ac54-4af31eae0682/scratchpad/advts/work")

BASE_ENV = dict(os.environ)
for v in ("PYTHONUTF8", "PYTHONIOENCODING", "RUST_BACKTRACE",
          "JAVA_TOOL_OPTIONS", "NODE_OPTIONS", "GOFLAGS"):
    BASE_ENV.pop(v, None)
BASE_ENV["LC_ALL"] = "en_US.UTF-8"
BASE_ENV["LANG"] = "en_US.UTF-8"
BASE_ENV["TZ"] = "UTC"


def fresh_dir(name):
    d = os.path.join(WORK, name.replace("/", "_"))
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    return d


def tsc_build(workdir, tsname, strict=False, timeout=300):
    cmd = ["tsc"] + (["--strict"] if strict else []) + [tsname]
    t0 = time.time()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, env=BASE_ENV,
                           cwd=workdir, timeout=timeout)
        rc, so, se, to = p.returncode, p.stdout, p.stderr, False
    except subprocess.TimeoutExpired as e:
        rc, so, se, to = None, (e.stdout or ""), (e.stderr or ""), True
        if isinstance(so, bytes):
            so = so.decode("utf-8", "replace")
        if isinstance(se, bytes):
            se = se.decode("utf-8", "replace")
    js = os.path.join(workdir, tsname[:-3] + ".js")
    return {"cmd": cmd, "exit": rc, "stdout": so, "stderr": se,
            "timed_out": to, "wall_seconds": round(time.time() - t0, 2),
            "artifact_produced": os.path.exists(js), "artifact": js}


def run_once(js, stdin_path, cwd, timeout=60):
    fin = open(stdin_path, "rb") if stdin_path else open(os.devnull, "rb")
    t0 = time.time()
    try:
        p = subprocess.run(["node", js], stdin=fin, capture_output=True,
                           env=BASE_ENV, cwd=cwd, timeout=timeout)
        rc, so, se, to = p.returncode, p.stdout, p.stderr, False
    except subprocess.TimeoutExpired as e:
        rc, so, se, to = None, (e.stdout or b""), (e.stderr or b""), True
    finally:
        fin.close()
    return {"cmd": ["node", js], "exit": rc,
            "stdout": so.decode("utf-8", "replace"),
            "stderr": se.decode("utf-8", "replace"), "timed_out": to,
            "signal": (-rc if rc is not None and rc < 0 else None),
            "wall_seconds": round(time.time() - t0, 2)}


def obs_of(stdout):
    for line in stdout.splitlines():
        if line.startswith("OBS="):
            return line[4:]
    return None


def measure(name, tsfile, stdin_name=None, run_cwd=None, reps=5,
            run_timeout=60, strict=False):
    wd = fresh_dir(name + ("_strict" if strict else ""))
    shutil.copy(os.path.join(TSD, tsfile), os.path.join(wd, tsfile))
    b = tsc_build(wd, tsfile, strict=strict)
    rec = {"name": name, "strict": strict,
           "program": os.path.join(TSD, tsfile), "build": b, "runs": []}
    if b["artifact_produced"]:
        stdin_path = os.path.join(INP, stdin_name) if stdin_name else None
        cwd = run_cwd or wd
        for _ in range(reps):
            rec["runs"].append(run_once(b["artifact"], stdin_path, cwd,
                                        run_timeout))
    return rec


CASES = [
    ("ADV-14/R",      "ADV-14_R.ts",          "ADV-14.in", None),
    ("ADV-15",        "ADV-15.ts",            None,        None),
    ("ADV-16",        "ADV-16.ts",            None,        None),
    ("ADV-17",        "ADV-17.ts",            None,        None),
    ("ADV-18",        "ADV-18.ts",            "ADV-18.in", None),
    ("ADV-19",        "ADV-19.ts",            "ADV-19.in", None),
    ("ADV-20",        "ADV-20.ts",            "ADV-20.in", None),
    ("ADV-21",        "ADV-21.ts",            None,        None),
    ("ADV-21/d1000",  "ADV-21_depth1000.ts",  None,        None),
    ("ADV-21/d10000", "ADV-21_depth10000.ts", None,        None),
    ("ADV-22-valid",  "ADV-22-valid.ts",      None,        None),
    ("ADV-22/a",      "ADV-22a.ts",           None,        None),
    ("ADV-22/b",      "ADV-22b.ts",           None,        None),
    ("ADV-23",        "ADV-23.ts",            None,        SRC),
    ("ADV-24",        "ADV-24.ts",            None,        None),
    ("ADV-25/R",      "ADV-25_R.ts",          "ADV-25.in", None),
    ("ADV-26/R",      "ADV-26_R.ts",          "ADV-26.in", None),
]

if __name__ == "__main__":
    os.makedirs(WORK, exist_ok=True)
    args = sys.argv[1:]
    strict = "--strict" in args
    only = [a for a in args if a != "--strict"] or None
    results = []
    for name, tsfile, stdin_name, cwd in CASES:
        if only and name not in only:
            continue
        sys.stderr.write("== %s%s\n" % (name, " [strict]" if strict else ""))
        sys.stderr.flush()
        r = measure(name, tsfile, stdin_name, cwd, strict=strict)
        results.append(r)
        sys.stderr.write("   build exit=%s artifact=%s runs=%d obs=%r\n" % (
            r["build"]["exit"], r["build"]["artifact_produced"],
            len(r["runs"]),
            obs_of(r["runs"][0]["stdout"]) if r["runs"] else None))
        sys.stderr.flush()
    print(json.dumps(results, indent=1))
