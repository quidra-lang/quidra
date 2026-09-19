#!/usr/bin/env python3
"""
Frozen build/run driver for the ten fixed benchmark languages.

One place that knows how each language is compiled and executed, so every
workload (micro, SVM, GMM, LightGrad, adversarial, LLM trial verification) uses
byte-identical toolchain invocations. The recipes here are the ones recorded in
environment.json under "frozen_toolchain_recipes" and were frozen before any
measurement.

Quidra appears as two execution modes -- `quidra` (native, compiled ahead of
time) and `quidra_interp` (run through the interpreter/REPL path) -- because
spec section 11 requires both to be measured separately. They collapse to the
single language column "Quidra" only at final scoring, by the aggregation rule
frozen in the methodology.
"""

import os
import shutil
import subprocess

# Java is pinned to the native arm64 JDK 26. The `java` first on PATH is an
# x86_64 JRE 8 with no javac, which would run under Rosetta translation and
# would not be a fair measurement of present-day Java.
JAVA_HOME = "/opt/homebrew/opt/openjdk"

QUIDRA_BIN = os.environ.get(
    "QUIDRA_BIN",
    "/private/tmp/claude-501/-Users-koba-Desktop-Quidra/"
    "c7dc7758-3bc7-4b38-ac54-4af31eae0682/scratchpad/bench/quidra-scratch/build/quidra",
)

# Fixed presentation order used by every comparison table in the report.
LANGUAGE_ORDER = ["Quidra", "Python", "C++", "Rust", "Go", "Java",
                  "TypeScript", "Kotlin", "Swift", "Zig"]

# Internal execution targets. `column` is the final table column each maps to.
TARGETS = {
    "quidra":        {"column": "Quidra",     "ext": ".qui",   "compiled": True},
    "quidra_interp": {"column": "Quidra",     "ext": ".qui",   "compiled": False},
    "python":        {"column": "Python",     "ext": ".py",    "compiled": False},
    "cpp":           {"column": "C++",        "ext": ".cpp",   "compiled": True},
    "rust":          {"column": "Rust",       "ext": ".rs",    "compiled": True},
    "go":            {"column": "Go",         "ext": ".go",    "compiled": True},
    "java":          {"column": "Java",       "ext": ".java",  "compiled": True},
    "typescript":    {"column": "TypeScript", "ext": ".ts",    "compiled": True},
    "kotlin":        {"column": "Kotlin",     "ext": ".kt",    "compiled": True},
    "swift":         {"column": "Swift",      "ext": ".swift", "compiled": True},
    "zig":           {"column": "Zig",        "ext": ".zig",   "compiled": True},
}

# Runtimes that need warm-up separated from cold start (spec section 12).
MANAGED_RUNTIMES = {"java", "kotlin", "typescript", "python"}


def env_for(target):
    env = dict(os.environ)
    if target in ("java", "kotlin"):
        env["JAVA_HOME"] = JAVA_HOME
        env["PATH"] = f"{JAVA_HOME}/bin:" + env.get("PATH", "")
    if target == "go":
        env.setdefault("GOFLAGS", "")
    return env


def build_cmd(target, src, outdir, stem="prog"):
    """Return (argv, artifact_path) for compiling `src`, or (None, None) when the
    language has no separate build step."""
    os.makedirs(outdir, exist_ok=True)
    binpath = os.path.join(outdir, stem)

    if target == "quidra":
        return [QUIDRA_BIN, "build", src, "-o", binpath], binpath
    if target == "quidra_interp":
        return None, None
    if target == "python":
        return None, None
    if target == "cpp":
        return ["clang++", "-std=c++20", "-O2", src, "-o", binpath], binpath
    if target == "rust":
        return ["rustc", "-O", src, "-o", binpath], binpath
    if target == "go":
        return ["go", "build", "-o", binpath, src], binpath
    if target == "java":
        classes = os.path.join(outdir, "classes")
        os.makedirs(classes, exist_ok=True)
        return [f"{JAVA_HOME}/bin/javac", "-d", classes, src], classes
    if target == "typescript":
        # tsc 7 removed --outFile; it emits alongside the source by default.
        return ["tsc", "--target", "es2022", "--module", "commonjs",
                "--outDir", outdir, src], os.path.join(
                    outdir, os.path.basename(src)[:-3] + ".js")
    if target == "kotlin":
        jar = os.path.join(outdir, stem + ".jar")
        return ["kotlinc", src, "-include-runtime", "-d", jar], jar
    if target == "swift":
        return ["swiftc", "-O", src, "-o", binpath], binpath
    if target == "zig":
        return ["zig", "build-exe", "-OReleaseFast", src,
                f"-femit-bin={binpath}"], binpath
    raise ValueError(f"unknown target {target}")


def run_cmd(target, src, outdir, stem="prog", main_class="Main"):
    """Return argv that executes the (already built) program."""
    binpath = os.path.join(outdir, stem)
    if target == "quidra":
        return [binpath]
    if target == "quidra_interp":
        return [QUIDRA_BIN, "run", src]
    if target == "python":
        return ["python3", src]
    if target in ("cpp", "rust", "go", "swift", "zig"):
        return [binpath]
    if target == "java":
        return [f"{JAVA_HOME}/bin/java", "-cp", os.path.join(outdir, "classes"), main_class]
    if target == "typescript":
        return ["node", os.path.join(outdir, os.path.basename(src)[:-3] + ".js")]
    if target == "kotlin":
        return [f"{JAVA_HOME}/bin/java", "-jar", os.path.join(outdir, stem + ".jar")]
    raise ValueError(f"unknown target {target}")


def zig_cleanup(outdir):
    """zig build-exe drops object/cache artifacts beside the binary; remove them
    so artifact-size measurement sees only the executable."""
    for name in os.listdir(outdir):
        if name.endswith(".o") or name.endswith(".pdb") or name == "zig-cache":
            p = os.path.join(outdir, name)
            shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else os.remove(p)


def artifact_size(target, outdir, stem="prog", src=None):
    """Bytes of the deployable build artifact for this language.

    Interpreted targets have no build artifact; their source file size is the
    artifact and is reported as such (deployment footprint, which additionally
    counts the required runtime, is a separate metric measured elsewhere).
    """
    if target in ("python", "quidra_interp"):
        return os.path.getsize(src) if src and os.path.exists(src) else None
    if target == "java":
        classes = os.path.join(outdir, "classes")
        if not os.path.isdir(classes):
            return None
        return sum(os.path.getsize(os.path.join(dp, f))
                   for dp, _, fs in os.walk(classes) for f in fs)
    if target == "kotlin":
        jar = os.path.join(outdir, stem + ".jar")
        return os.path.getsize(jar) if os.path.exists(jar) else None
    if target == "typescript":
        js = os.path.join(outdir, os.path.basename(src)[:-3] + ".js") if src else None
        return os.path.getsize(js) if js and os.path.exists(js) else None
    binpath = os.path.join(outdir, stem)
    return os.path.getsize(binpath) if os.path.exists(binpath) else None


def build(target, src, outdir, stem="prog", timeout=1800):
    """Compile if needed. Returns a dict describing the outcome."""
    cmd, artifact = build_cmd(target, src, outdir, stem)
    if cmd is None:
        return {"target": target, "needs_build": False, "ok": True,
                "cmd": None, "stdout": "", "stderr": "", "exit_code": 0}
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          env=env_for(target), timeout=timeout,
                          cwd=os.path.dirname(src) or None)
    if target == "zig":
        try:
            zig_cleanup(outdir)
        except OSError:
            pass
    return {
        "target": target,
        "needs_build": True,
        "ok": proc.returncode == 0,
        "cmd": cmd,
        "exit_code": proc.returncode,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
        "artifact": artifact,
    }


def self_test(tmpdir):
    """Compile and run a hello-world in every target; returns per-target status.
    Used as a harness pre-flight so an infrastructure defect is never mistaken
    for a language failure (spec 10.4)."""
    samples = {
        "quidra":        ('print("ok")\n', ".qui"),
        "quidra_interp": ('print("ok")\n', ".qui"),
        "python":        ('print("ok")\n', ".py"),
        "cpp":           ('#include <cstdio>\nint main(){printf("ok\\n");}\n', ".cpp"),
        "rust":          ('fn main(){println!("ok");}\n', ".rs"),
        "go":            ('package main\nimport "fmt"\nfunc main(){fmt.Println("ok")}\n', ".go"),
        "java":          ('public class Main{public static void main(String[] a){System.out.println("ok");}}\n', ".java"),
        "typescript":    ('console.log("ok");\n', ".ts"),
        "kotlin":        ('fun main(){println("ok")}\n', ".kt"),
        "swift":         ('print("ok")\n', ".swift"),
        "zig":           ('const std = @import("std");\n'
                          'pub fn main() !void {\n'
                          '    var t = std.Io.Threaded.init_single_threaded;\n'
                          '    try std.Io.File.stdout().writeStreamingAll(t.io(), "ok\\n");\n'
                          '}\n', ".zig"),
    }
    results = {}
    for target, (code, ext) in samples.items():
        d = os.path.join(tmpdir, target)
        os.makedirs(d, exist_ok=True)
        stem = "Main" if target == "java" else "prog"
        src = os.path.join(d, stem + ext)
        with open(src, "w") as f:
            f.write(code)
        try:
            b = build(target, src, d, stem)
            if not b["ok"]:
                results[target] = {"ok": False, "stage": "build",
                                   "error": (b["stderr"] or b["stdout"])[:400]}
                continue
            r = subprocess.run(run_cmd(target, src, d, stem), capture_output=True,
                               text=True, env=env_for(target), timeout=300)
            results[target] = {
                "ok": r.returncode == 0 and r.stdout.strip() == "ok",
                "stage": "run",
                "exit_code": r.returncode,
                "stdout": r.stdout.strip()[:200],
                "error": r.stderr.strip()[:400],
                "artifact_size_bytes": artifact_size(target, d, stem, src),
            }
        except Exception as e:  # noqa: BLE001 - surface any harness defect
            results[target] = {"ok": False, "stage": "exception", "error": str(e)[:400]}
    return results


if __name__ == "__main__":
    import json
    import sys
    out = self_test(sys.argv[1] if len(sys.argv) > 1 else "/tmp/langs_selftest")
    print(json.dumps(out, indent=2))
    bad = [k for k, v in out.items() if not v.get("ok")]
    print(("ALL TARGETS OK" if not bad else "FAILING: " + ", ".join(bad)), file=sys.stderr)
    sys.exit(1 if bad else 0)
