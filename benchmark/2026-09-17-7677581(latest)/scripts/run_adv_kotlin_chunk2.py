#!/usr/bin/env python3
"""Adversarial harness, Kotlin chunk 2 (ADV-14 .. ADV-26).

Frozen recipe (environment/environment.json -> frozen_toolchain_recipes.kotlin):
    build: kotlinc FILE.kt -include-runtime -d FILE.jar
    run:   java -jar FILE.jar
Frozen execution environment (methodology/08 -> execution_environment):
    LC_ALL/LANG en_US.UTF-8, TZ UTC; build timeout 300s; run timeout 60s;
    5 primary runs; stdin from the frozen input file or /dev/null.
"""
import json, os, shutil, subprocess, sys, time

ROOT = "/private/tmp/claude-501/-Users-koba-Desktop-Quidra/c7dc7758-3bc7-4b38-ac54-4af31eae0682/scratchpad/bench/quidra-scratch/benchmark/2026-09-17-7677581"
SRC = os.path.join(ROOT, "standard/src/adversarial/kotlin")
INP = os.path.join(ROOT, "standard/src/adversarial/inputs")
WORK = os.path.join(ROOT, "work/adv_kotlin_chunk2")
JAVA_HOME = "/opt/homebrew/opt/openjdk"

def env():
    e = dict(os.environ)
    for k in ("PYTHONUTF8", "PYTHONIOENCODING", "RUST_BACKTRACE",
              "JAVA_TOOL_OPTIONS", "NODE_OPTIONS", "GOFLAGS", "_JAVA_OPTIONS"):
        e.pop(k, None)
    e["LC_ALL"] = "en_US.UTF-8"
    e["LANG"] = "en_US.UTF-8"
    e["TZ"] = "UTC"
    e["JAVA_HOME"] = JAVA_HOME
    e["PATH"] = JAVA_HOME + "/bin:" + e.get("PATH", "")
    return e

# (tag, source basename, stdin file or None, extra files to stage into cwd)
CASES = [
    ("ADV-14_R",  "ADV-14_R.kt",  "ADV-14.in", []),
    ("ADV-15",    "ADV-15.kt",    None,        []),
    ("ADV-16",    "ADV-16.kt",    None,        []),
    ("ADV-17",    "ADV-17.kt",    None,        []),
    ("ADV-18",    "ADV-18.kt",    "ADV-18.in", []),
    ("ADV-19",    "ADV-19.kt",    "ADV-19.in", []),
    ("ADV-20",    "ADV-20.kt",    "ADV-20.in", []),
    ("ADV-21",    "ADV-21.kt",    None,        []),
    ("ADV-22a",   "ADV-22a.kt",   None,        []),
    ("ADV-22b",   "ADV-22b.kt",   None,        []),
    ("ADV-23",    "ADV-23.kt",    None,        ["ADV-23.bin"]),
    ("ADV-24",    "ADV-24.kt",    None,        []),
    ("ADV-25_R",  "ADV-25_R.kt",  "ADV-25.in", []),
    ("ADV-26_R",  "ADV-26_R.kt",  "ADV-26.in", []),
]

def one(tag, srcname, stdin_name, extra):
    wd = os.path.join(WORK, tag)
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd, exist_ok=True)
    src = os.path.join(SRC, srcname)
    jar = os.path.join(wd, "prog.jar")
    rec = {"tag": tag, "source": src}

    bcmd = ["kotlinc", src, "-include-runtime", "-d", jar]
    t0 = time.time()
    try:
        bp = subprocess.run(bcmd, cwd=wd, env=env(), capture_output=True,
                            text=True, errors="replace", timeout=300)
        bout, berr, bexit, btimeout = bp.stdout, bp.stderr, bp.returncode, False
    except subprocess.TimeoutExpired as ex:
        bout = (ex.stdout or b"").decode("utf-8", "replace") if isinstance(ex.stdout, bytes) else (ex.stdout or "")
        berr = (ex.stderr or b"").decode("utf-8", "replace") if isinstance(ex.stderr, bytes) else (ex.stderr or "")
        bexit, btimeout = None, True
    rec["build"] = {"cmd": bcmd, "exit": bexit, "stdout": bout, "stderr": berr,
                    "wall_seconds": round(time.time() - t0, 2), "timed_out": btimeout,
                    "artifact_produced": os.path.exists(jar)}

    if btimeout or bexit != 0 or not os.path.exists(jar):
        rec["runs"] = []
        return rec

    # stage input files the program opens by relative path
    if extra:
        os.makedirs(os.path.join(wd, "inputs"), exist_ok=True)
        for f in extra:
            shutil.copy(os.path.join(INP, f), os.path.join(wd, "inputs", f))

    stdin_path = os.path.join(INP, stdin_name) if stdin_name else "/dev/null"
    rcmd = [JAVA_HOME + "/bin/java", "-jar", jar]
    runs = []
    for i in range(5):
        with open(stdin_path, "rb") as fh:
            t1 = time.time()
            try:
                rp = subprocess.run(rcmd, cwd=wd, env=env(), stdin=fh,
                                    capture_output=True, timeout=60)
                out = rp.stdout.decode("utf-8", "replace")
                err = rp.stderr.decode("utf-8", "replace")
                code, sig, to = rp.returncode, None, False
                if code is not None and code < 0:
                    sig, code = -code, None
            except subprocess.TimeoutExpired as ex:
                out = (ex.stdout or b"").decode("utf-8", "replace")
                err = (ex.stderr or b"").decode("utf-8", "replace")
                code, sig, to = None, None, True
        runs.append({"cmd": rcmd, "exit": code, "signal": sig, "stdout": out,
                     "stderr": err, "timed_out": to,
                     "wall_seconds": round(time.time() - t1, 2)})
        if to:
            break
    rec["runs"] = runs
    return rec

if __name__ == "__main__":
    want = sys.argv[1:] or [c[0] for c in CASES]
    os.makedirs(WORK, exist_ok=True)
    results = {}
    outpath = os.path.join(WORK, "harness_out.json")
    if os.path.exists(outpath):
        results = json.load(open(outpath))
    for tag, srcname, stdin_name, extra in CASES:
        if tag not in want:
            continue
        sys.stderr.write("### %s\n" % tag); sys.stderr.flush()
        results[tag] = one(tag, srcname, stdin_name, extra)
        json.dump(results, open(outpath, "w"), indent=1)
        r = results[tag]
        sys.stderr.write("  build exit=%s timeout=%s wall=%s\n" % (
            r["build"]["exit"], r["build"]["timed_out"], r["build"]["wall_seconds"]))
        if r["runs"]:
            r0 = r["runs"][0]
            sys.stderr.write("  run exit=%s sig=%s to=%s\n    OUT %r\n    ERR %r\n" % (
                r0["exit"], r0["signal"], r0["timed_out"], r0["stdout"][:400], r0["stderr"][:600]))
        sys.stderr.flush()
    print(outpath)
