#!/usr/bin/env python3
"""Harness for the Python adversarial chunk-2 rows.

Frozen recipe (environment.json -> frozen_toolchain_recipes.python): no build
step; run is `python3 FILE.py`. Fresh empty working directory per (case,
language, configuration). 5 primary repetitions, 60 s run timeout.
"""
import json, os, shutil, subprocess, sys, time

BASE = "/private/tmp/claude-501/-Users-koba-Desktop-Quidra/c7dc7758-3bc7-4b38-ac54-4af31eae0682/scratchpad/bench/quidra-scratch/benchmark/2026-09-17-7677581"
SRC = os.path.join(BASE, "standard/src/adversarial/python")
INP = os.path.join(BASE, "standard/src/adversarial/inputs")
WORK = os.path.join(BASE, "standard/build/adversarial/python")

ENV = {k: v for k, v in os.environ.items()
       if k not in ("PYTHONUTF8", "PYTHONIOENCODING", "RUST_BACKTRACE",
                    "JAVA_TOOL_OPTIONS", "NODE_OPTIONS", "GOFLAGS")}
ENV["LC_ALL"] = "en_US.UTF-8"
ENV["LANG"] = "en_US.UTF-8"
ENV["TZ"] = "UTC"
ENV["PATH"] = "/opt/homebrew/bin:" + ENV.get("PATH", "")

# (row_key, source_basename, stdin file or None, needs inputs/ dir)
JOBS = [
    ("ADV-14/R", "ADV-14_R.py", "ADV-14.in", False),
    ("ADV-15",   "ADV-15.py",   None,        False),
    ("ADV-16",   "ADV-16.py",   None,        False),
    ("ADV-17",   "ADV-17.py",   None,        False),
    ("ADV-18",   "ADV-18.py",   "ADV-18.in", False),
    ("ADV-19",   "ADV-19.py",   "ADV-19.in", False),
    ("ADV-20",   "ADV-20.py",   "ADV-20.in", False),
    ("ADV-21",   "ADV-21.py",   None,        False),
    ("ADV-22-valid", "ADV-22-valid.py", None, False),
    ("ADV-22a",  "ADV-22a.py",  None,        False),
    ("ADV-22b",  "ADV-22b.py",  None,        False),
    ("ADV-23",   "ADV-23.py",   None,        True),
    ("ADV-24",   "ADV-24.py",   None,        False),
    ("ADV-25/R", "ADV-25_R.py", "ADV-25.in", False),
    ("ADV-26/R", "ADV-26_R.py", "ADV-26.in", False),
    ("ADV-21-sec-1000",  "ADV-21_depth1000.py",  None, False),
    ("ADV-21-sec-10000", "ADV-21_depth10000.py", None, False),
]

results = {}
for key, src, stdin_name, needs_inputs in JOBS:
    wd = os.path.join(WORK, key.replace("/", "_"))
    if os.path.isdir(wd):
        shutil.rmtree(wd)
    os.makedirs(wd)
    shutil.copy2(os.path.join(SRC, src), os.path.join(wd, src))
    if needs_inputs:
        os.makedirs(os.path.join(wd, "inputs"))
        shutil.copy2(os.path.join(INP, "ADV-23.bin"),
                     os.path.join(wd, "inputs", "ADV-23.bin"))
    data = b""
    if stdin_name:
        data = open(os.path.join(INP, stdin_name), "rb").read()
    runs = []
    for i in range(5):
        t0 = time.time()
        timed_out = False
        try:
            p = subprocess.run(["python3", src], cwd=wd, env=ENV, input=data,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               timeout=60)
            out, err, rc = p.stdout, p.stderr, p.returncode
        except subprocess.TimeoutExpired as e:
            timed_out = True
            out = e.stdout or b""
            err = e.stderr or b""
            rc = None
        wall = time.time() - t0
        so = out.decode("utf-8", "backslashreplace")
        se = err.decode("utf-8", "backslashreplace")
        obs = None
        for line in so.splitlines():
            if line.startswith("OBS="):
                obs = line[4:]
        runs.append({
            "index": i, "exit_status": rc,
            "signal": (-rc if (rc is not None and rc < 0) else None),
            "timed_out": timed_out, "wall_seconds": round(wall, 3),
            "stdout": so, "stderr": se,
            "adv_start_present": "ADV-START" in so,
            "adv_end_present": "ADV-END" in so,
            "obs_payload": obs,
        })
        open(os.path.join(wd, "run%d.out" % i), "w").write(so)
        open(os.path.join(wd, "run%d.err" % i), "w").write(se)
    results[key] = {"source": os.path.join(SRC, src), "runs": runs,
                    "obs_identical": len({r["obs_payload"] for r in runs}) == 1}
    r0 = runs[0]
    print("%-18s exit=%-6s to=%-5s start=%-5s obs=%-24s end=%s" % (
        key, r0["exit_status"], r0["timed_out"], r0["adv_start_present"],
        r0["obs_payload"], r0["adv_end_present"]))
    if r0["stderr"]:
        print("     stderr tail: " + " | ".join(r0["stderr"].strip().splitlines()[-2:])[:200])

json.dump(results, open(os.path.join(WORK, "raw_runs.json"), "w"), indent=1)
print("\nwrote", os.path.join(WORK, "raw_runs.json"))
