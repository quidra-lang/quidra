#!/usr/bin/env python3
import json, os, shutil, subprocess, sys, time

BASE = "/private/tmp/claude-501/-Users-koba-Desktop-Quidra/c7dc7758-3bc7-4b38-ac54-4af31eae0682/scratchpad/bench/quidra-scratch/benchmark/2026-09-17-7677581"
SRC  = os.path.join(BASE, "standard/src/adversarial/cpp")
INP  = os.path.join(BASE, "standard/src/adversarial/inputs")
WORK = os.path.join(BASE, "work/adv_cpp_chunk2")

ENV = {k: v for k, v in os.environ.items()
       if k not in ("PYTHONUTF8","PYTHONIOENCODING","RUST_BACKTRACE",
                    "JAVA_TOOL_OPTIONS","NODE_OPTIONS","GOFLAGS")}
ENV.update({"LC_ALL":"en_US.UTF-8","LANG":"en_US.UTF-8","TZ":"UTC"})

# program -> (stdin file or None, extra files to stage)
PROGS = [
    ("ADV-14_R.cpp",  "ADV-14.in", []),
    ("ADV-15_single.cpp",    None,        []),
    ("ADV-16_single.cpp",    None,        []),
    ("ADV-17_single.cpp",    None,        []),
    ("ADV-18_single.cpp",    "ADV-18.in", []),
    ("ADV-19_single.cpp",    "ADV-19.in", []),
    ("ADV-20_single.cpp",    "ADV-20.in", []),
    ("ADV-21_single.cpp",    None,        []),
    ("ADV-22a_single.cpp",   None,        []),
    ("ADV-22b_single.cpp",   None,        []),
    ("ADV-23_single.cpp",    None,        ["ADV-23.bin"]),
    ("ADV-24_single.cpp",    None,        []),
    ("ADV-25_R.cpp",  "ADV-25.in", []),
    ("ADV-26_R.cpp",  "ADV-26.in", []),
    ("ADV-21_single_depth1000.cpp",  None, []),
    ("ADV-21_single_depth10000.cpp", None, []),
]

def dec(b): return b.decode("utf-8", "replace")

def run_one(name, stdin_file, extra):
    stem = name[:-4]
    wd = os.path.join(WORK, stem)
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd)
    src = os.path.join(SRC, name)
    if extra:
        os.makedirs(os.path.join(wd, "inputs"), exist_ok=True)
        for e in extra:
            shutil.copy(os.path.join(INP, e), os.path.join(wd, "inputs", e))
    binp = os.path.join(wd, "prog")
    bcmd = ["clang++", "-std=c++20", "-O2", src, "-o", binp]
    t0 = time.time()
    try:
        bp = subprocess.run(bcmd, capture_output=True, cwd=wd, env=ENV, timeout=300)
        bex, bto = bp.returncode, False
        bout, berr = dec(bp.stdout), dec(bp.stderr)
    except subprocess.TimeoutExpired as e:
        bex, bto = None, True
        bout, berr = dec(e.stdout or b""), dec(e.stderr or b"")
    bw = round(time.time() - t0, 2)
    rec = {"program": src,
           "build": {"cmd": bcmd, "exit": bex, "stdout": bout, "stderr": berr,
                     "wall_seconds": bw, "timed_out": bto,
                     "artifact_produced": os.path.exists(binp)},
           "runs": []}
    if bex != 0 or not os.path.exists(binp):
        return rec
    sin = os.path.join(INP, stdin_file) if stdin_file else os.devnull
    for i in range(5):
        with open(sin, "rb") as fh:
            data = fh.read()
        t0 = time.time()
        try:
            rp = subprocess.run([binp], input=data, capture_output=True,
                                cwd=wd, env=ENV, timeout=60)
            rc, to = rp.returncode, False
            so, se = dec(rp.stdout), dec(rp.stderr)
        except subprocess.TimeoutExpired as e:
            rc, to = None, True
            so, se = dec(e.stdout or b""), dec(e.stderr or b"")
        rec["runs"].append({"cmd": [binp], "exit": rc,
                            "signal": (-rc if (rc is not None and rc < 0) else None),
                            "stdout": so, "stderr": se, "timed_out": to,
                            "wall_seconds": round(time.time()-t0, 2)})
        if to:
            break
    return rec

out = {}
only = sys.argv[1:] or None
for name, sf, ex in PROGS:
    if only and name not in only:
        continue
    sys.stderr.write("=== %s\n" % name); sys.stderr.flush()
    out[name] = run_one(name, sf, ex)
    dst = os.path.join(WORK, "raw_%s.json" % name[:-4])
    os.makedirs(WORK, exist_ok=True)
    json.dump(out[name], open(dst, "w"), indent=1)
print(json.dumps(out, indent=1))
