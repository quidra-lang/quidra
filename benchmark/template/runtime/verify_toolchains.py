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
import shlex
import subprocess
import sys
import tempfile

MANIFEST = Path(__file__).resolve().parent / "toolchains.json"
F20_FIXTURES = Path(__file__).resolve().parent / "f20_interop_fixtures.json"
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


def render_f20_recipe(recipe: str, source: Path, work: Path) -> list[str]:
    replacements = {
        "FILE.py": source.name,
        "FILE.go": source.name,
        "FILE.java": source.name,
        "FILE.kt": source.name,
        "FILE.jar": "program.jar",
        "./BIN": "./program",
        "BIN": "program",
        "OUT": "out",
    }
    argv: list[str] = []
    for token in shlex.split(recipe):
        rendered = token
        for key in sorted(replacements, key=len, reverse=True):
            rendered = rendered.replace(key, replacements[key])
        argv.append(rendered)
    return argv


def semantic_ffi_smoke() -> tuple[dict[str, dict[str, object]], list[str]]:
    """Execute the frozen F20.P1 fixture contract in the pinned Linux image."""
    fixtures = json.loads(F20_FIXTURES.read_text(encoding="utf-8"))
    if fixtures.get("schema_version") != 1 or fixtures.get("probe_id") != "F20.P1":
        raise SystemExit("invalid F20.P1 runtime fixture contract")
    cases = fixtures.get("languages") or {}
    expected_languages = {"Python", "Go", "Java", "Kotlin"}
    if set(cases) != expected_languages:
        raise SystemExit(
            "F20.P1 runtime fixtures must cover exactly "
            + ", ".join(sorted(expected_languages))
        )

    observed: dict[str, dict[str, object]] = {}
    problems: list[str] = []
    with tempfile.TemporaryDirectory(prefix="quidra-f20-") as raw:
        base = Path(raw)
        for language in sorted(cases):
            case = cases[language]
            if not isinstance(case, dict):
                problems.append(f"{language}: F20.P1 fixture is not an object")
                continue
            work = base / language.lower()
            work.mkdir()
            source = work / str(case.get("filename") or "")
            if not source.name or source.parent != work:
                problems.append(f"{language}: invalid F20.P1 fixture filename")
                continue
            source.write_text(str(case.get("source") or ""), encoding="utf-8")
            (work / "out").mkdir(exist_ok=True)

            build_recipe = case.get("build")
            build = None
            if build_recipe is not None:
                build = run_process(
                    render_f20_recipe(str(build_recipe), source, work),
                    work,
                )
            build_ok = build is None or int(build["exit_code"]) == 0
            run_recipe = str(case.get("run") or "")
            run = (
                run_process(render_f20_recipe(run_recipe, source, work), work)
                if build_ok and run_recipe
                else {
                    "argv": [],
                    "exit_code": None,
                    "stdout": "",
                    "stderr": (
                        "not run because the frozen-recipe build failed"
                        if not build_ok
                        else "fixture has no run recipe"
                    ),
                }
            )
            passed = (
                build_ok
                and run.get("exit_code") is not None
                and int(run["exit_code"]) == 0
                and str(run["stdout"]).strip() == "3"
            )
            observed[language] = {
                "probe_id": "F20.P1",
                "mechanism": case.get("mechanism"),
                "frozen_recipe_build": build_recipe,
                "frozen_recipe_run": run_recipe,
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
