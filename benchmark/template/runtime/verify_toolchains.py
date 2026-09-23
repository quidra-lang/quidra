#!/usr/bin/env python3
"""Assert the image's installed toolchains equal the declared pins.

Running this at image build time turns `toolchains.json` from documentation into
an enforced contract: if an upstream repository serves a different build than the
pin names, the image fails to build instead of quietly shipping an unrecorded
toolchain into a measurement run.

The observed fingerprints are also written into the image so a finished run can
state exactly what it measured against.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

MANIFEST = Path(__file__).resolve().parent / "toolchains.json"
OBSERVED = Path("/opt/quidra-benchmark/toolchains-observed.json")

# Each entry: language -> (argv, pin key, regex capturing the comparable version)
CHECKS: dict[str, tuple[list[str], str, str]] = {
    "Python": (["python3", "--version"], "PYTHON_PIN", r"Python (\d+\.\d+\.\d+)"),
    "C++": (["c++", "--version"], "CLANG_PIN", r"clang version (\d+\.\d+\.\d+)"),
    "CMake": (["cmake", "--version"], "CMAKE_PIN", r"cmake version (\d+\.\d+\.\d+)"),
    "Rust": (["rustc", "--version"], "RUST_PIN", r"rustc (\d+\.\d+\.\d+)"),
    "Go": (["go", "version"], "GO_PIN", r"go(\d+\.\d+\.\d+)"),
    # Java omits trailing zeros, so a .0 release reports "javac 26" rather than
    # "javac 26.0.0". Accept both spellings and let the pin comparison decide.
    "Java": (["javac", "-version"], "JAVA_PIN", r"javac (\d+(?:\.\d+\.\d+)?)"),
    "Kotlin": (["kotlinc", "-version"], "KOTLIN_PIN", r"kotlinc-jvm (\d+\.\d+\.\d+)"),
    "Node": (["node", "--version"], "NODE_PIN", r"v(\d+\.\d+\.\d+)"),
    "TypeScript": (["tsc", "--version"], "TYPESCRIPT_PIN", r"Version (\d+\.\d+\.\d+)"),
    "Swift": (["swift", "--version"], "SWIFT_PIN", r"Swift version (\d+\.\d+(?:\.\d+)?)"),
    "Zig": (["zig", "version"], "ZIG_PIN", r"(\d+\.\d+\.\d+)"),
}


def capture(argv: list[str]) -> str:
    completed = subprocess.run(
        argv,
        shell=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env={**os.environ, "LC_ALL": "C"},
    )
    if completed.returncode != 0:
        raise SystemExit(
            f"toolchain check failed ({' '.join(argv)}): "
            f"{(completed.stderr or completed.stdout).strip()}"
        )
    return (completed.stdout + "\n" + completed.stderr).strip()


def run_process(argv: list[str], cwd: Path) -> dict[str, object]:
    completed = subprocess.run(
        argv,
        cwd=cwd,
        shell=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env={**os.environ, "LC_ALL": "C"},
    )
    return {
        "argv": argv,
        "exit_code": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
    }


def semantic_ffi_smoke() -> tuple[dict[str, dict[str, object]], list[str]]:
    """Prove disputed F20.P1 mechanisms in the exact pinned Linux image.

    These are positive capability observations, not substitutes for every
    language's canonical fragment.  They exist to prevent a packet-only worker
    from inventing claims such as "cgo needs an extra flag" or "JDK FFM is
    preview-only" when the frozen recipe itself can demonstrate otherwise.
    """
    cases: dict[str, dict[str, object]] = {
        "Python": {
            "mechanism": "standard-library ctypes",
            "filename": "ffi.py",
            "source": """import ctypes
libc = ctypes.CDLL(None)
abs_fn = libc.abs
abs_fn.argtypes = [ctypes.c_int]
abs_fn.restype = ctypes.c_int
value: int = int(abs_fn(ctypes.c_int(-3)))
print(value)
""",
            "build": None,
            "run": ["python3", "ffi.py"],
        },
        "Go": {
            "mechanism": "cgo shipped with the Go toolchain",
            "filename": "ffi.go",
            "source": """package main
/*
#include <stdlib.h>
*/
import "C"
import "fmt"

func main() {
    var value int32 = int32(C.abs(C.int(-3)))
    fmt.Println(value)
}
""",
            "build": ["go", "build", "-o", "ffi-go", "ffi.go"],
            "run": ["./ffi-go"],
        },
        "Java": {
            "mechanism": "java.lang.foreign FFM API",
            "filename": "Main.java",
            "source": """import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;

public class Main {
    public static void main(String[] args) throws Throwable {
        Linker linker = Linker.nativeLinker();
        MethodHandle abs = linker.downcallHandle(
            linker.defaultLookup().find("abs").orElseThrow(),
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.JAVA_INT)
        );
        int value = (int) abs.invokeWithArguments(-3);
        System.out.println(value);
    }
}
""",
            "build": ["javac", "-d", "java-out", "Main.java"],
            "run": ["java", "-cp", "java-out", "Main"],
        },
        "Kotlin": {
            "mechanism": "Kotlin/JVM calling the JDK java.lang.foreign FFM API",
            "filename": "ffi.kt",
            "source": """import java.lang.foreign.FunctionDescriptor
import java.lang.foreign.Linker
import java.lang.foreign.ValueLayout

fun main() {
    val linker = Linker.nativeLinker()
    val abs = linker.downcallHandle(
        linker.defaultLookup().find("abs").orElseThrow(),
        FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.JAVA_INT)
    )
    val value = abs.invokeWithArguments(-3) as Int
    println(value)
}
""",
            "build": ["kotlinc", "ffi.kt", "-include-runtime", "-d", "ffi.jar"],
            "run": ["java", "-jar", "ffi.jar"],
        },
    }

    observed: dict[str, dict[str, object]] = {}
    problems: list[str] = []
    with tempfile.TemporaryDirectory(prefix="quidra-f20-") as raw:
        base = Path(raw)
        for language, case in cases.items():
            work = base / language.lower()
            work.mkdir()
            (work / str(case["filename"])).write_text(
                str(case["source"]), encoding="utf-8"
            )
            build_argv = case["build"]
            build = None
            if isinstance(build_argv, list):
                build = run_process([str(value) for value in build_argv], work)
            build_ok = build is None or int(build["exit_code"]) == 0
            run = (
                run_process([str(value) for value in case["run"]], work)
                if build_ok
                else {
                    "argv": case["run"],
                    "exit_code": None,
                    "stdout": "",
                    "stderr": "not run because the frozen-recipe build failed",
                }
            )
            passed = (
                build_ok
                and int(run["exit_code"]) == 0
                and str(run["stdout"]).strip() == "3"
            )
            observed[language] = {
                "probe_id": "F20.P1",
                "mechanism": case["mechanism"],
                "frozen_recipe_extra_flags": [],
                "build": build,
                "run": run,
                "expected_stdout": "3",
                "passed": passed,
            }
            if not passed:
                problems.append(
                    f"{language}: F20.P1 smoke failed: build={build!r}; run={run!r}"
                )
    return observed, problems


def main() -> int:
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    pins = manifest["toolchains"]
    problems: list[str] = []
    observed: dict[str, str] = {}

    for language, (argv, pin_key, pattern) in CHECKS.items():
        try:
            output = capture(argv)
        except SystemExit as exc:
            problems.append(str(exc))
            continue
        observed[language] = output.splitlines()[0]
        match = re.search(pattern, output)
        if not match:
            problems.append(f"{language}: could not parse a version from {output!r}")
            continue
        expected = pins[pin_key]
        if match.group(1) != expected:
            problems.append(
                f"{language}: image installed {match.group(1)} but toolchains.json pins "
                f"{expected}. Update the pin deliberately; do not silently ship a "
                "different toolchain into a measurement run."
            )

    ffi_smoke, ffi_problems = semantic_ffi_smoke()
    problems.extend(ffi_problems)

    if problems:
        print("runtime image toolchain verification failed:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 2

    OBSERVED.parent.mkdir(parents=True, exist_ok=True)
    OBSERVED.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "image": manifest["image"],
                "pins": pins,
                "observed": observed,
                "semantic_ffi_smoke": ffi_smoke,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    print(json.dumps(
        {"ok": True, "observed": observed, "semantic_ffi_smoke": ffi_smoke},
        indent=2,
        sort_keys=True,
    ))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
