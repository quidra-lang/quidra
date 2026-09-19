#!/usr/bin/env python3
import json, os, shutil, subprocess, sys, time

BASE = "/private/tmp/claude-501/-Users-koba-Desktop-Quidra/c7dc7758-3bc7-4b38-ac54-4af31eae0682/scratchpad/bench/quidra-scratch/benchmark/2026-09-17-7677581"
SRC  = os.path.join(BASE, "standard/src/adversarial/java")
INP  = os.path.join(BASE, "standard/src/adversarial/inputs")
WORK = os.path.join(BASE, "work/adv_java_chunk2")
JAVA_HOME = "/opt/homebrew/opt/openjdk"

ENV = {k: v for k, v in os.environ.items()
       if k not in ("PYTHONUTF8","PYTHONIOENCODING","RUST_BACKTRACE",
                    "JAVA_TOOL_OPTIONS","NODE_OPTIONS","GOFLAGS")}
ENV.update({"LC_ALL":"en_US.UTF-8","LANG":"en_US.UTF-8","TZ":"UTC",
            "JAVA_HOME": JAVA_HOME,
            "PATH": JAVA_HOME + "/bin:" + ENV.get("PATH","")})

PROGS = [
    ("ADV-14_R",  "ADV-14.in", []),
    ("ADV-15",    None,        []),
    ("ADV-16",    None,        []),
    ("ADV-17",    None,        []),
    ("ADV-18",    "ADV-18.in", []),
    ("ADV-19",    "ADV-19.in", []),
    ("ADV-20",    "ADV-20.in", []),
    ("ADV-21",    None,        []),
    ("ADV-22-valid", None,     []),
    ("ADV-22a",   None,        []),
    ("ADV-22b",   None,        []),
    ("ADV-23",    None,        ["ADV-23.bin"]),
    ("ADV-24",    None,        []),
    ("ADV-25_R",  "ADV-25.in", []),
    ("ADV-26_R",  "ADV-26.in", []),
    ("ADV-21_depth1000",  None, []),
    ("ADV-21_depth10000", None, []),
]

def dec(b): return b.decode("utf-8", "replace")

def run_one(stem, stdin_file, extra):
    wd = os.path.join(WORK, stem)
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd)
    src = os.path.join(SRC, stem, "Main.java")
    if extra:
        os.makedirs(os.path.join(wd, "inputs"), exist_ok=True)
        for e in extra:
            shutil.copy(os.path.join(INP, e), os.path.join(wd, "inputs", e))
    classes = os.path.join(wd, "classes")
    os.makedirs(classes, exist_ok=True)
    bcmd = [JAVA_HOME + "/bin/javac", "-d", classes, src]
    t0 = time.time()
    try:
        bp = subprocess.run(bcmd, capture_output=True, cwd=wd, env=ENV, timeout=300)
        bex, bto = bp.returncode, False
        bout, berr = dec(bp.stdout), dec(bp.stderr)
    except subprocess.TimeoutExpired as e:
        bex, bto = None, True
        bout, berr = dec(e.stdout or b""), dec(e.stderr or b"")
    bw = round(time.time() - t0, 2)
    artifact = os.path.exists(os.path.join(classes, "Main.class"))
    rec = {"program": src,
           "build": {"cmd": bcmd, "exit": bex, "stdout": bout, "stderr": berr,
                     "wall_seconds": bw, "timed_out": bto,
                     "artifact_produced": artifact},
           "runs": []}
    if bex != 0 or not artifact:
        return rec
    rcmd = [JAVA_HOME + "/bin/java", "-cp", classes, "Main"]
    sin = os.path.join(INP, stdin_file) if stdin_file else os.devnull
    for i in range(5):
        with open(sin, "rb") as fh:
            data = fh.read()
        t0 = time.time()
        try:
            rp = subprocess.run(rcmd, input=data, capture_output=True,
                                cwd=wd, env=ENV, timeout=60)
            rc, to = rp.returncode, False
            so, se = dec(rp.stdout), dec(rp.stderr)
        except subprocess.TimeoutExpired as e:
            rc, to = None, True
            so, se = dec(e.stdout or b""), dec(e.stderr or b"")
        rec["runs"].append({"cmd": rcmd, "exit": rc,
                            "signal": (-rc if (rc is not None and rc < 0) else None),
                            "stdout": so, "stderr": se, "timed_out": to,
                            "wall_seconds": round(time.time()-t0, 2)})
        if to:
            break
    return rec

out = {}
only = sys.argv[1:] or None
os.makedirs(WORK, exist_ok=True)
for name, sf, ex in PROGS:
    if only and name not in only:
        continue
    sys.stderr.write("=== %s\n" % name); sys.stderr.flush()
    out[name] = run_one(name, sf, ex)
    json.dump(out[name], open(os.path.join(WORK, "raw_%s.json" % name), "w"), indent=1)
print(json.dumps(out, indent=1))
