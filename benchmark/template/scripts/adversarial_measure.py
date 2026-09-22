#!/usr/bin/env python3
"""Mechanical adversarial / safety scorer for Language Quality.

The frozen case set in methodology-assets/language_quality/adversarial_cases.json
states that every decision it needs is a build command, a run command, a string
comparison or a regular expression. This script is that decision procedure,
executed by the trusted runner rather than by a model: it builds and runs the
34 scored case-variants (37 programs) for every language under the frozen
primary recipe, runs each once more under the frozen non-scoring secondary
recipe, replays decision rules D0..D7 on the captured bytes, and computes the
nine Safety / Robustness metrics from the recorded rungs exactly as their
methodology documents define them. Every rung can be re-derived from the
preserved build output, run outputs, exit statuses and signals alone.

Comparison-language programs come from template/programs/<lang>/adversarial.
Quidra programs come from the evaluated snapshot itself, under the path frozen
in primary.json (language_quality.quidra_program_root), together with the
type-binding amendment the frozen case set requires before any Quidra row may
be scored; without that amendment every Quidra row is `na_authoring_defect`,
exactly as the case set prescribes.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Any

SCRIPTS_DIR = Path(__file__).resolve().parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))
# The template tree is hashed for integrity; importing a sibling module must not
# leave a __pycache__ in it.
sys.dont_write_bytecode = True

import micro_measure as mm  # noqa: E402  (sibling module inside the frozen template)

TEMPLATE_DIR = SCRIPTS_DIR.parent
ASSET_DIR = "methodology-assets/language_quality"
LANGUAGES = list(mm.LANGUAGES)
LANGUAGE_IDS = {
    "Quidra": "quidra", "Python": "python", "C++": "cpp", "Rust": "rust", "Go": "go",
    "Java": "java", "TypeScript": "typescript", "Kotlin": "kotlin", "Swift": "swift",
    "Zig": "zig",
}
EXTENSIONS = {lang: mm.CONFIGS[lang][2] for lang in LANGUAGES}
COMPARISON_LANGUAGES = [lang for lang in LANGUAGES if lang != "Quidra"]

PBC = "Prevented By Construction"
CTD = "Compile-time Detection"
SCBE = "Static Checking Before Execution"
RSD = "Runtime Safe Detection"
TF = "Test Failure"
OV = "Output Verification"
CRASH = "Crash"
UB = "Undefined Behavior"
SB = "Silent Bug"
STATIC_STAGES = {CTD, SCBE, PBC}
FAULT_SIGNALS = {11, 10, 4, 8}
OUTPUT_CAP_BYTES = 262144
REQUIREMENT_METRICS = {
    "metric.type_safety": "Type_Safety",
    "metric.memory_safety": "Memory_Safety",
    "metric.runtime_safety": "Runtime_Safety",
    "metric.boundary_value_safety": "Boundary_Value_Safety",
    "metric.adversarial_input_robustness": "Adversarial_Input_Robustness",
    "metric.early_error_detection": "Early_Error_Detection",
    "metric.debuggability": "Debuggability",
    "metric.silent_bug_resistance": "Silent_Bug_Resistance",
    "metric.implementation_robustness": "Implementation_Robustness",
}
#: What a checked/sanitizer configuration says when it reports undefined,
#: illegal or erroneous behaviour (decision rule D7b, first condition). Frozen
#: here, applied identically to every language's secondary configuration.
D7B_REPORT_PATTERN = re.compile(
    r"undefined behavio(u)?r|runtime error|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"MemorySanitizer|ThreadSanitizer|illegal behavio(u)?r|erroneous|panicked at|"
    r"\bpanic:|Fatal error:|assertion failed|reached unreachable",
    re.IGNORECASE,
)
#: Source-position and fault-key forms, verbatim from debuggability.json.
DBG_POSITION_FORMS = [
    r"[\w./+-]+\.[A-Za-z][A-Za-z0-9+]{0,4}:\d+(?::\d+)?",
    r"[\w./+-]+\.[A-Za-z][A-Za-z0-9+]{0,4}\(\d+,\d+\)",
    r"\bline\s+\d+\b",
    r"\bat\s+\d+:\d+",
    r"<[A-Za-z][A-Za-z0-9_]*>:\d+(?::\d+)?",
]
DBG_KEY_FORMS = [
    r"\berror\[[A-Za-z0-9_]+\]",
    r"\b(?:error|code)\s*[ =:]\s*[A-Z]{0,5}\d{2,5}\b",
    r"\b[A-Z][A-Za-z0-9_]*(?:Error|Exception)\b",
    r"\berror\.[A-Za-z][A-Za-z0-9_]*\b",
    r"\berror:[ \t]*[A-Z][A-Za-z0-9_]*[ \t]*(?:\n|\r|$)",
    r"\[-W[a-z0-9-]+\]",
]


class MeasureError(mm.MeasureError):
    pass


# --------------------------------------------------------------------------
# Frozen inputs
# --------------------------------------------------------------------------


def load_asset(root: Path, name: str) -> dict[str, Any]:
    return mm.load_json(root / "template" / ASSET_DIR / name)


class Frozen:
    """The frozen case set and its companion documents, resolved once."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.asset = load_asset(root, "adversarial_cases.json")
        self.type_safety = load_asset(root, "type_safety.json")
        self.memory_safety = load_asset(root, "memory_safety.json")
        self.debuggability = load_asset(root, "debuggability.json")
        self.tm3 = load_asset(root, "tm3_determinations.json")
        self.citations = load_asset(root, "normative_ub_citations.json")
        self.stage_scores = {
            row["earliest_observable_stage"]: int(row["score"])
            for row in self.asset["fixed_stage_score_table"]["table"]
        }
        registered = self.asset["fixed_stage_score_table"]["registered_addition"]
        self.stage_scores[registered["earliest_observable_stage"]] = int(registered["score"])
        self.cases = {c["case_id"]: c for c in self.asset["case_set"]["cases"]}
        self.rows = list(self.asset["fixed_scored_case_variant_list"]["rows"])
        self.fragments = {
            name: re.compile(pattern, re.IGNORECASE)
            for name, pattern in self.asset["global_hazard_diagnostic_lexicon"]["fragments"].items()
        }
        self.toolchain_markers = [
            re.compile(p, re.IGNORECASE)
            for p in self.asset["toolchain_failure_markers"]["case_insensitive_regexes"]
        ]
        self.recipes = self.asset["toolchain_binding"]["recipes"]
        self.secondary = self.asset["secondary_non_scoring_configurations"]["configurations"]
        environment = self.asset["execution_environment"]
        self.build_timeout = int(environment["build_timeout_seconds"])
        self.run_timeout = int(environment["run_timeout_seconds"])
        self.repetitions = int(environment["repetitions_primary"])

    def case_for_program(self, program_id: str) -> tuple[dict[str, Any], dict[str, Any] | None]:
        """The case and, for a sub-program (ADV-04a, ADV-22b), its sub-program entry."""
        base = program_id.split("/", 1)[0]
        case_id = re.match(r"(ADV-\d\d)", base).group(1)
        case = self.cases[case_id]
        sub_id = base.split("_", 1)[0]
        for sub in case.get("sub_programs", []) or []:
            if sub.get("id") == sub_id:
                return case, sub
        return case, None

    def case_lexicon(self, case: dict[str, Any]) -> list[tuple[str, re.Pattern[str]]]:
        patterns: list[tuple[str, re.Pattern[str]]] = []
        for entry in case.get("hazard_diagnostic_lexicon", []):
            if entry in self.fragments:
                patterns.append((entry, self.fragments[entry]))
            else:
                patterns.append((entry, re.compile(entry, re.IGNORECASE)))
        return patterns

    def match_case_lexicon(self, case: dict[str, Any], text: str) -> str | None:
        for name, pattern in self.case_lexicon(case):
            if pattern.search(text):
                return name
        return None

    def match_global_lexicon(self, text: str) -> str | None:
        for name, pattern in self.fragments.items():
            if pattern.search(text):
                return name
        return None

    def toolchain_marker(self, text: str) -> str | None:
        for pattern in self.toolchain_markers:
            if pattern.search(text):
                return pattern.pattern
        return None

    def tm3_for(self, language: str, program_id: str,
                amendment: dict[str, Any] | None) -> dict[str, Any] | None:
        """The pre-declared TM3 determination for (language, program), if any."""
        base = program_id.split("/", 1)[0]
        case_id = re.match(r"(ADV-\d\d)", base).group(1)
        table = self.tm3.get("determinations", {})
        if language == "Quidra":
            table = (amendment or {}).get("tm3_determinations", {}) if amendment else {}
            rows = table
        else:
            rows = table.get(language, {})
        for key in (program_id, base, case_id):
            if key in rows:
                return rows[key]
        return None

    def ub_citation(self, language: str, program_id: str) -> dict[str, Any] | None:
        base = program_id.split("/", 1)[0]
        case_id = re.match(r"(ADV-\d\d)", base).group(1)
        rows = self.citations.get("citations", {}).get(language, {})
        for key in (program_id, base, case_id):
            if key in rows:
                return rows[key]
        return None


# --------------------------------------------------------------------------
# Program resolution and inputs
# --------------------------------------------------------------------------


def program_stems(program_id: str) -> list[str]:
    """Candidate file stems for a program id, across the template's spellings."""
    base, _, variant = program_id.partition("/")
    if variant:
        return [f"{base}_{variant}"]
    if base.endswith("-valid"):
        head = base[: -len("-valid")]
        return [base, f"{head}_single-valid"]
    if "_depth" in base:
        head, depth = base.split("_depth", 1)
        return [base, f"{head}_single_depth{depth}"]
    return [base, f"{base}_single"]


def resolve_source(language: str, programs_dir: Path, program_id: str) -> Path | None:
    ext = EXTENSIONS[language]
    for stem in program_stems(program_id):
        if language == "Java":
            candidate = programs_dir / stem / "Main.java"
        else:
            candidate = programs_dir / f"{stem}.{ext}"
        if candidate.is_file():
            return candidate
    return None


def input_tokens_for(case: dict[str, Any], sub: dict[str, Any] | None) -> list[str] | None:
    inputs = (sub or case).get("inputs") or []
    if not inputs or any("file" in entry for entry in inputs):
        return None
    return [str(entry["text"]) for entry in sorted(inputs, key=lambda e: int(e.get("line", 0)))]


def write_inputs(frozen: Frozen, workdir: Path, language_dir: Path,
                 case: dict[str, Any], sub: dict[str, Any] | None) -> Path | None:
    """Materialise the frozen input file(s) for one program into its work dir.

    Shared inputs are generated from the case set (frozen_generators.input_files);
    a language may carry an override under inputs_lang/ for the one branch the
    case set freezes per index type (ADV-09).
    """
    inputs_dir = workdir / "inputs"
    inputs_dir.mkdir(parents=True, exist_ok=True)
    stdin_file: Path | None = None
    for entry in case.get("inputs") or []:
        if "file" in entry:
            data = bytes.fromhex(str(entry["bytes_hex"]))
            (inputs_dir / Path(str(entry["file"])).name).write_bytes(data)
    tokens = input_tokens_for(case, sub)
    if tokens is not None:
        name = f"{(sub or case).get('id') or case['case_id']}.in"
        stdin_file = inputs_dir / name
        stdin_file.write_text("".join(f"{t}\n" for t in tokens), encoding="ascii")
        override = language_dir / "inputs_lang" / f"{case['case_id']}.in"
        if override.is_file() and sub is None:
            stdin_file.write_bytes(override.read_bytes())
    return stdin_file


def construction_line_range(source: Path) -> dict[str, int] | None:
    """Line span between the skeleton's ADV-START statement and its OBS= statement."""
    try:
        lines = source.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None
    start = next((i for i, line in enumerate(lines, start=1) if "ADV-START" in line), None)
    end = next((i for i, line in enumerate(lines, start=1) if "OBS=" in line), None)
    if end is None:
        # A file with no observation line is a malformed-source program (the
        # ADV-22 mutations), whose hazardous construction is the whole file.
        return {"first_line": 1, "last_line": max(1, len(lines))}
    if start is None or end <= start:
        return None
    return {"first_line": start + 1, "last_line": end}


# --------------------------------------------------------------------------
# Execution
# --------------------------------------------------------------------------


def recipe_argv(template: str, files: dict[str, str]) -> list[str]:
    argv: list[str] = []
    for token in template.split():
        replaced = token
        for key in ("FILE.jar", "FILE.js", "FILE.qui", "FILE.py", "FILE.cpp", "FILE.rs",
                    "FILE.go", "FILE.java", "FILE.ts", "FILE.kt", "FILE.swift", "FILE.zig",
                    "./BIN", "BIN", "OUT"):
            if key in replaced:
                replaced = replaced.replace(key, files[key.replace("./", "")])
        if replaced == "quidra":
            replaced = files["quidra"]
        argv.append(replaced)
    return argv


def env_overrides(spec: str | None) -> dict[str, str]:
    result: dict[str, str] = {}
    for part in (spec or "").split(","):
        part = part.strip()
        if "=" in part:
            key, value = part.split("=", 1)
            result[key.strip()] = value.strip()
    return result


def locale_value(root: Path) -> str:
    """en_US.UTF-8 when the host has it (the frozen value); C.UTF-8 otherwise."""
    try:
        listed = subprocess.run(
            ["locale", "-a"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            timeout=30, env=mm.benchmark_env(root, root),
        ).stdout.lower()
    except (OSError, subprocess.SubprocessError):
        listed = ""
    if "en_us.utf-8" in listed or "en_us.utf8" in listed:
        return "en_US.UTF-8"
    return "C.UTF-8"


def adversarial_env(root: Path, cwd: Path, locale: str, extra: dict[str, str]) -> dict[str, str]:
    env = mm.benchmark_env(root, cwd)
    env["LC_ALL"] = locale
    env["LANG"] = locale
    env["TZ"] = "UTC"
    for name in ("PYTHONUTF8", "PYTHONIOENCODING", "RUST_BACKTRACE", "JAVA_TOOL_OPTIONS",
                 "NODE_OPTIONS", "GOFLAGS"):
        env.pop(name, None)
    env.update(extra)
    return env


def capture(argv: list[str], cwd: Path, env: dict[str, str], timeout: int,
            stdin_file: Path | None) -> dict[str, Any]:
    import time
    stdin_handle = open(stdin_file, "rb") if stdin_file else subprocess.DEVNULL
    started = time.perf_counter_ns()
    try:
        try:
            completed = subprocess.run(
                argv, cwd=cwd, env=env, stdin=stdin_handle, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=timeout, shell=False,
            )
            result = {
                "argv": argv,
                "exit_code": completed.returncode,
                "stdout": completed.stdout.decode("utf-8", "replace"),
                "stderr": completed.stderr.decode("utf-8", "replace"),
                "wall_seconds": (time.perf_counter_ns() - started) / 1e9,
                "timed_out": False,
            }
        except subprocess.TimeoutExpired as exc:
            result = {
                "argv": argv,
                "exit_code": None,
                "stdout": (exc.stdout or b"").decode("utf-8", "replace")
                if isinstance(exc.stdout, bytes) else str(exc.stdout or ""),
                "stderr": (exc.stderr or b"").decode("utf-8", "replace")
                if isinstance(exc.stderr, bytes) else str(exc.stderr or ""),
                "wall_seconds": float(timeout),
                "timed_out": True,
            }
        except OSError as exc:
            # The recipe's program is not installed or not executable on this
            # host: a toolchain defect, recorded like a build that never ran.
            result = {
                "argv": argv, "exit_code": 127, "stdout": "",
                "stderr": f"could not start {argv[0]!r}: {exc}",
                "wall_seconds": (time.perf_counter_ns() - started) / 1e9,
                "timed_out": False,
            }
    finally:
        if stdin_file:
            stdin_handle.close()
    code = result["exit_code"]
    result["signal"] = -code if isinstance(code, int) and code < 0 else None
    if result["signal"] is not None:
        result["exit_code"] = 128 + result["signal"]
    return result


def store_text(path: Path, text: str) -> str:
    path.parent.mkdir(parents=True, exist_ok=True)
    data = text.encode("utf-8", "replace")[:OUTPUT_CAP_BYTES]
    path.write_bytes(data)
    return str(path)


def parse_run(run: dict[str, Any]) -> dict[str, Any]:
    lines = run["stdout"].splitlines()
    obs = None
    for line in lines:
        if line.startswith("OBS="):
            obs = line[len("OBS="):]
            break
    return {
        "adv_start_present": "ADV-START" in lines,
        "adv_end_present": "ADV-END" in lines,
        "obs_payload": obs,
    }


class Cell:
    """One (program, language, configuration) build-and-run, with its captured bytes."""

    def __init__(self, frozen: Frozen, language: str, program_id: str, source: Path,
                 language_dir: Path, configuration: str, workdir: Path, compiler: Path | None,
                 locale: str) -> None:
        self.frozen = frozen
        self.language = language
        self.program_id = program_id
        self.source = source
        self.configuration = configuration
        self.workdir = workdir
        self.compiler = compiler
        self.locale = locale
        self.language_dir = language_dir
        self.record: dict[str, Any] = {}

    #: Set by the driver for Quidra: the checked configuration the frozen
    #: amendment names, or None when the amendment records none available.
    quidra_secondary: dict[str, Any] | None = None

    def recipe(self) -> dict[str, Any]:
        if self.configuration == "primary":
            return dict(self.frozen.recipes[self.language])
        if self.language == "Quidra":
            return dict(self.quidra_secondary or {})
        return dict(self.frozen.secondary.get(self.language) or {})

    def execute(self) -> dict[str, Any]:
        frozen = self.frozen
        if self.workdir.exists():
            shutil.rmtree(self.workdir)
        self.workdir.mkdir(parents=True, exist_ok=True)
        ext = EXTENSIONS[self.language]
        stem = "Main" if self.language == "Java" else "program"
        local = self.workdir / f"{stem}.{ext}"
        shutil.copy2(self.source, local)
        case, sub = frozen.case_for_program(self.program_id)
        stdin_file = write_inputs(frozen, self.workdir, self.language_dir, case, sub)
        variant = self.program_id.partition("/")[2]
        if variant == "C":
            stdin_file = None
        files = {
            f"FILE.{ext}": str(local),
            "FILE.jar": str(self.workdir / "program.jar"),
            "FILE.js": str(self.workdir / f"{stem}.js"),
            "BIN": str(self.workdir / "program.bin"),
            "OUT": str(self.workdir / "out"),
            "quidra": str(self.compiler) if self.compiler else "quidra",
        }
        recipe = self.recipe()
        build_template = recipe.get("build")
        run_template = recipe.get("run")
        extra_env = env_overrides(recipe.get("env"))
        env = adversarial_env(frozen.root, self.workdir, self.locale, extra_env)
        record: dict[str, Any] = {
            "case_variant_id": self.program_id,
            "language_id": LANGUAGE_IDS[self.language],
            "language": self.language,
            "configuration": self.configuration,
            "source_path": str(self.source),
            "source_sha256": sha256_file(self.source),
            "construction_line_range": construction_line_range(self.source),
            "build_command": build_template,
            "run_command": run_template,
            "locale": self.locale,
            "stdin": str(stdin_file) if stdin_file else "/dev/null",
            "build_exit_status": None,
            "build_signal": None,
            "build_timed_out": False,
            "artifact_produced": None,
            "runs": [],
        }
        if not run_template:
            record["na"] = {"reason": "no frozen recipe for this configuration"}
            self.record = record
            return record
        if build_template:
            build = capture(recipe_argv(build_template, files), self.workdir, env,
                            frozen.build_timeout, None)
            record["build_exit_status"] = build["exit_code"]
            record["build_signal"] = build["signal"]
            record["build_timed_out"] = build["timed_out"]
            record["build_wall_seconds"] = build["wall_seconds"]
            record["build_stdout_path"] = store_text(self.workdir / "build.stdout", build["stdout"])
            record["build_stderr_path"] = store_text(self.workdir / "build.stderr", build["stderr"])
            record["build_stdout"] = build["stdout"][:OUTPUT_CAP_BYTES]
            record["build_stderr"] = build["stderr"][:OUTPUT_CAP_BYTES]
            if build["exit_code"] == 0 and not build["timed_out"]:
                record["artifact_produced"] = self.artifact_present(files)
            else:
                record["artifact_produced"] = False
        repetitions = frozen.repetitions if self.configuration == "primary" else 1
        if build_template is None or record["artifact_produced"]:
            argv = recipe_argv(run_template, files)
            for index in range(1, repetitions + 1):
                run = capture(argv, self.workdir, env, frozen.run_timeout, stdin_file)
                parsed = parse_run(run)
                record["runs"].append({
                    "index": index,
                    "exit_status": run["exit_code"],
                    "signal": run["signal"],
                    "timed_out": run["timed_out"],
                    "wall_seconds": run["wall_seconds"],
                    "stdout_path": store_text(self.workdir / f"run{index}.stdout", run["stdout"]),
                    "stderr_path": store_text(self.workdir / f"run{index}.stderr", run["stderr"]),
                    "stdout": run["stdout"][:OUTPUT_CAP_BYTES],
                    "stderr": run["stderr"][:OUTPUT_CAP_BYTES],
                    **parsed,
                })
        payloads = {r["obs_payload"] for r in record["runs"]}
        record["obs_identical_across_runs"] = len(payloads) <= 1
        self.cleanup_artifacts()
        self.record = record
        return record

    def cleanup_artifacts(self) -> None:
        """Keep the captured text and the exact source; drop build products.

        The cell directory sits under work/root/commands, which the retained
        set imports. Binaries, class trees, jars and debug bundles are neither
        evidence nor privacy-safe (a dSYM records host paths), so only the
        source copy, the inputs and the captured stdout/stderr survive.
        """
        keep_suffixes = {".stdout", ".stderr"}
        for child in list(self.workdir.iterdir()):
            if child.name == "inputs" or child.suffix in keep_suffixes:
                continue
            if child.is_file() and child.suffix == f".{EXTENSIONS[self.language]}":
                continue
            if child.is_dir() and not child.is_symlink():
                shutil.rmtree(child, ignore_errors=True)
            else:
                try:
                    child.unlink()
                except OSError:
                    pass

    def artifact_present(self, files: dict[str, str]) -> bool:
        if self.language == "Java":
            out = Path(files["OUT"])
            return out.is_dir() and any(out.rglob("*.class"))
        if self.language == "Kotlin":
            return Path(files["FILE.jar"]).is_file()
        if self.language == "TypeScript":
            return Path(files["FILE.js"]).is_file()
        if self.language == "Python":
            return True
        return Path(files["BIN"]).is_file()


def sha256_file(path: Path) -> str:
    import hashlib
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


# --------------------------------------------------------------------------
# Classification: the frozen decision list D0..D7
# --------------------------------------------------------------------------


DIAGNOSTIC_KEYWORD = re.compile(r"error|fatal|panic|exception|traceback", re.IGNORECASE)


def first_diagnostic_line(text: str) -> str:
    """The first line of the first diagnostic in a toolchain's output.

    A diagnostic is a line naming an error and, preferably, a source position.
    Toolchains that print a header before the positioned line (Go's
    `# command-line-arguments`, Rust's `error:` followed by `-->`) or that name
    the position without an English keyword (a localized javac) must still yield
    the diagnostic rather than the header, so the order of preference is:
    keyword and position, keyword alone, position alone, first non-empty line.
    """
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    for line in lines:
        if DIAGNOSTIC_KEYWORD.search(line) and diagnostic_line_number(line) is not None:
            return line
    for line in lines:
        if DIAGNOSTIC_KEYWORD.search(line):
            return line
    for line in lines:
        if diagnostic_line_number(line) is not None:
            return line
    return lines[0] if lines else ""


def first_diagnostic_line_number(text: str) -> int | None:
    """Source line the first diagnostic points at, for the D1b-ii-2 range test.

    Multi-line diagnostics (rustc's `error:` header with the position on the
    following `-->` line) carry their position within the next few lines of
    the same diagnostic, before the next keyword line or blank line.
    """
    first = first_diagnostic_line(text)
    if not first:
        return None
    number = diagnostic_line_number(first)
    if number is not None:
        return number
    lines = [line.strip() for line in text.splitlines()]
    try:
        start = lines.index(first)
    except ValueError:
        return None
    for line in lines[start + 1:start + 6]:
        if not line or DIAGNOSTIC_KEYWORD.search(line):
            break
        number = diagnostic_line_number(line)
        if number is not None:
            return number
    return None


def diagnostic_line_number(line: str) -> int | None:
    for pattern in (r":(\d+):\d+", r":(\d+)\b", r"\((\d+),\d+\)", r"\bline\s+(\d+)\b",
                    r"\bat\s+(\d+):\d+"):
        match = re.search(pattern, line)
        if match:
            return int(match.group(1))
    return None


def predicate_holds(predicate: dict[str, Any], payload: str) -> bool:
    regex = predicate.get("regex")
    if not regex or not re.match(regex, payload):
        return False
    field = predicate.get("numeric_field")
    interval = predicate.get("interval")
    if field and interval:
        match = re.search(rf"(?:^|\|){re.escape(field)}:(-?\d+)", payload)
        if not match:
            return False
        value = int(match.group(1))
        return float(interval[0]) <= value <= float(interval[1])
    return True


def classify(frozen: Frozen, language: str, program_id: str, primary: dict[str, Any],
             secondary: dict[str, Any] | None, tm3: dict[str, Any] | None) -> dict[str, Any]:
    """Replay D0..D7 on one program's captured primary observation."""
    case, sub = frozen.case_for_program(program_id)
    spec = sub or case
    reference = str(spec.get("reference_observation", case.get("reference_observation")))
    predicate = spec.get("plausible_domain_predicate") or case.get("plausible_domain_predicate") or {}
    evidence: list[dict[str, Any]] = []
    flags: list[str] = []

    def fired(rule: str, stage: str, why: str, **extra: Any) -> dict[str, Any]:
        evidence.append({"rule_id": rule, "fired": True, "why": why})
        classes = {PBC: PBC, CTD: CTD, SCBE: CTD, RSD: RSD, OV: OV, CRASH: CRASH, UB: UB, SB: SB}
        return {
            "class": classes[stage],
            "earliest_observable_stage": stage,
            "stage_score": frozen.stage_scores[stage],
            "decision_rule_fired": rule,
            "stage_evidence": evidence,
            "flags": sorted(set(flags + list(extra.pop("flags", [])))),
            **extra,
        }

    def skipped(rule: str, why: str) -> None:
        evidence.append({"rule_id": rule, "fired": False, "why": why})

    if primary.get("na"):
        return {
            "class": None, "earliest_observable_stage": None, "stage_score": None,
            "decision_rule_fired": None, "stage_evidence": evidence,
            "flags": ["na"], "na": primary["na"],
        }

    # D0 - pre-declared capability absence.
    if tm3 and str(tm3.get("branch")) == "TM3a":
        flags.append("construct_absent")
        return fired("D0", PBC, "pre-declared TM3a determination", tm3_applied="TM3a",
                     normative_citation=tm3.get("citation"))
    skipped("D0", "no pre-declared TM3a determination for this language and case")
    if tm3 and str(tm3.get("branch")) == "TM3b":
        flags.append("construct_absent")

    has_build = bool(primary.get("build_command"))
    build_out = (primary.get("build_stdout") or "") + "\n" + (primary.get("build_stderr") or "")
    # D1 - the build rejected the program or the toolchain broke.
    if has_build and (primary.get("build_timed_out") or primary.get("build_exit_status") != 0):
        if primary.get("build_timed_out"):
            flags.append("build_timeout")
            return fired("D1a", CRASH, "build exceeded the frozen build timeout",
                         sublabel="build_timeout", flags=["toolchain_crash"])
        marker = frozen.toolchain_marker(build_out)
        if primary.get("build_signal") is not None or marker:
            flags.append("toolchain_crash")
            return fired("D1a", CRASH, f"build died by signal or matched toolchain marker {marker!r}",
                         sublabel="toolchain_crash")
        skipped("D1a", "build exited non-zero without a toolchain failure marker")
        matched = frozen.match_case_lexicon(case, build_out) or frozen.match_global_lexicon(build_out)
        first = first_diagnostic_line(build_out)
        if matched:
            return fired("D1b-i", CTD, f"build diagnostic matched lexicon {matched}",
                         first_diagnostic=first, lexicon_match=matched)
        skipped("D1b-i", "no case or global lexicon fragment matched the build output")
        span = primary.get("construction_line_range")
        number = first_diagnostic_line_number(build_out)
        if span and number is not None and span["first_line"] <= number <= span["last_line"]:
            flags.append("compile_rejection_unmatched_lexicon")
            return fired("D1b-ii-2", CTD, "first diagnostic points inside the construction line range",
                         first_diagnostic=first, diagnostic_line=number)
        skipped("D1b-ii-2", "first diagnostic does not point inside the construction line range")
        flags.append("na_authoring_defect")
        return {
            "class": None, "earliest_observable_stage": None, "stage_score": None,
            "decision_rule_fired": "D1b-ii-3", "stage_evidence": evidence,
            "flags": sorted(set(flags)),
            "na": {"reason": "build failed for a reason unrelated to the case's hazard "
                             "(authoring defect); first diagnostic: " + first[:300]},
        }
    skipped("D1", "no build step or the build exited zero")
    # D2 - built but nothing runnable.
    if has_build and not primary.get("artifact_produced"):
        flags.append("toolchain_produced_no_artifact")
        return fired("D2", CRASH, "build exited zero but produced no runnable artifact",
                     sublabel="toolchain_produced_no_artifact")
    skipped("D2", "artifact produced" if has_build else "no build step")

    runs = primary.get("runs") or []
    if not runs:
        return {
            "class": None, "earliest_observable_stage": None, "stage_score": None,
            "decision_rule_fired": None, "stage_evidence": evidence, "flags": sorted(set(flags)),
            "na": {"reason": "no run was captured"},
        }
    run = runs[0]
    run_out = (run.get("stdout") or "") + "\n" + (run.get("stderr") or "")
    abnormal = run.get("timed_out") is False and (run.get("exit_status") != 0 or run.get("signal") is not None)
    # D3 - the defect was reported before the first statement executed.
    if abnormal and not run.get("adv_start_present"):
        if run.get("signal") in FAULT_SIGNALS or frozen.toolchain_marker(run.get("stderr") or ""):
            flags.append("pre_execution_toolchain_crash")
            return fired("D3a", CRASH, "fatal-fault signal or toolchain marker before execution began",
                         sublabel="pre_execution_toolchain_crash")
        return fired("D3b", SCBE, "the program was rejected before its first statement executed",
                     first_diagnostic=first_diagnostic_line(run_out))
    skipped("D3", "execution began or the process exited normally")
    # D4 - non-termination.
    if run.get("timed_out"):
        flags.append("nontermination")
        return fired("D4", OV, "the run exceeded the frozen run timeout", sublabel="nontermination")
    skipped("D4", "the run terminated within the timeout")
    # D5 - abnormal termination after execution began.
    if abnormal:
        matched = frozen.match_case_lexicon(case, run_out)
        if matched:
            return fired("D5a", RSD, f"runtime diagnostic matched case lexicon {matched}",
                         first_diagnostic=first_diagnostic_line(run_out), lexicon_match=matched)
        skipped("D5a", "no case lexicon pattern matched")
        matched = frozen.match_global_lexicon(run_out)
        if matched:
            flags.append("diagnostic_mismatch")
            return fired("D5a_prime", RSD, f"runtime diagnostic matched global fragment {matched}",
                         first_diagnostic=first_diagnostic_line(run_out), lexicon_match=matched)
        skipped("D5a_prime", "no global lexicon fragment matched")
        return fired("D5b", CRASH, "abnormal termination with no hazard-naming diagnostic")
    skipped("D5", "the process exited zero")
    # D6 - normal completion.
    if not (run.get("adv_start_present") and run.get("obs_payload") is not None and run.get("adv_end_present")):
        flags.append("missing_observation")
        return fired("D6a", UB, "exit 0 but the skeleton's observation or end line is missing",
                     sublabel="missing_observation")
    skipped("D6a", "ADV-START, OBS= and ADV-END all present")
    payload = str(run.get("obs_payload"))
    if reference != "DIAGNOSED_FAILURE" and payload == reference:
        return fired("D6b", PBC, "observation equals the reference byte for byte",
                     obs_payload=payload, reference_observation=reference)
    skipped("D6b", "observation differs from the reference or the reference is DIAGNOSED_FAILURE")
    if not predicate_holds(predicate, payload):
        return fired("D6c-i", OV, "observation fails the plausible-domain predicate",
                     obs_payload=payload)
    skipped("D6c-i", "observation is plausible on its face")
    # D7 - a wrong but apparently valid value.
    if not primary.get("obs_identical_across_runs", True):
        return fired("D7a", UB, "the primary runs did not all produce a byte-identical OBS payload",
                     obs_payloads=[r.get("obs_payload") for r in runs], evidence_kind="nondeterministic_observation")
    skipped("D7a", "all primary runs produced the same OBS payload")
    if secondary and not secondary.get("na"):
        sec_out = "\n".join([
            secondary.get("build_stdout") or "", secondary.get("build_stderr") or "",
            *[(r.get("stdout") or "") + "\n" + (r.get("stderr") or "") for r in secondary.get("runs") or []],
        ])
        sec_runs = secondary.get("runs") or []
        sec_payload = sec_runs[0].get("obs_payload") if sec_runs else None
        names_ub = bool(D7B_REPORT_PATTERN.search(sec_out)) or bool(
            frozen.match_case_lexicon(case, sec_out)
        )
        if names_ub or (sec_payload is not None and sec_payload != payload):
            return fired("D7b", UB, "the checked configuration reports the hazard or changes the payload",
                         evidence_kind="checked_build_divergence", secondary_payload=sec_payload,
                         secondary_report=first_diagnostic_line(sec_out))
        skipped("D7b", "the checked configuration neither reported the hazard nor changed the payload")
    else:
        skipped("D7b", "no checked configuration observation")
    citation = frozen.ub_citation(language, program_id)
    if citation and citation.get("status") == "undefined":
        return fired("D7c", UB, "the language's normative specification declares the operation undefined",
                     normative_citation=citation.get("citation"), normative_status="undefined",
                     evidence_kind="normative_citation")
    skipped("D7c", "no frozen normative citation of undefined behaviour for this operation")
    return fired("D7d", SB, "deterministic, plausible, wrong value with no positive UB evidence",
                 obs_payload=payload, reference_observation=reference,
                 normative_status=(
                     "defined" if citation and citation.get("status") == "defined" else "no_citation_found"
                 ),
                 normative_citation=(citation or {}).get("citation"))


def classify_all_runs(frozen: Frozen, language: str, program_id: str, primary: dict[str, Any],
                      secondary: dict[str, Any] | None, tm3: dict[str, Any] | None) -> dict[str, Any]:
    """D5 consistency: the lowest-scoring rung any of the 5 runs reaches is recorded."""
    verdict = classify(frozen, language, program_id, primary, secondary, tm3)
    runs = primary.get("runs") or []
    if verdict.get("stage_score") is None or len(runs) <= 1:
        return verdict
    lowest = verdict
    divergent = False
    for run in runs[1:]:
        alternate = dict(primary)
        alternate["runs"] = [run, *[r for r in runs if r is not run]]
        other = classify(frozen, language, program_id, alternate, secondary, tm3)
        if other.get("stage_score") is None:
            continue
        if other["earliest_observable_stage"] != verdict["earliest_observable_stage"]:
            divergent = True
        if other["stage_score"] < lowest["stage_score"]:
            lowest = other
    if divergent:
        lowest = dict(lowest)
        lowest["flags"] = sorted(set(lowest.get("flags", [])) | {"run_to_run_rung_divergence"})
    return lowest


# --------------------------------------------------------------------------
# Metrics
# --------------------------------------------------------------------------


def mean(values: list[float]) -> float | None:
    return sum(values) / len(values) if values else None


def clamp_score(value: float | None) -> float | None:
    if value is None:
        return None
    return round(max(0.0, min(100.0, float(value))), 6)


class LanguageRecords:
    """Per-language view: program verdicts keyed by program id and by row."""

    def __init__(self, frozen: Frozen, language: str, verdicts: dict[str, dict[str, Any]]) -> None:
        self.frozen = frozen
        self.language = language
        self.verdicts = verdicts

    def program_stage(self, program_id: str) -> str | None:
        verdict = self.verdicts.get(program_id)
        if not verdict or verdict.get("stage_score") is None:
            return None
        return verdict["earliest_observable_stage"]

    def row_programs(self, row: dict[str, Any]) -> list[str]:
        return list(row["programs"])

    def row_stage_score(self, row_id: str) -> float | None:
        row = next(r for r in self.frozen.rows if r["case_variant_id"] == row_id)
        scores = [
            float(self.verdicts[p]["stage_score"]) for p in self.row_programs(row)
            if self.verdicts.get(p) and self.verdicts[p].get("stage_score") is not None
        ]
        return mean(scores)

    def row_indicator(self, row_id: str, predicate) -> float | None:
        row = next(r for r in self.frozen.rows if r["case_variant_id"] == row_id)
        values = []
        for program in self.row_programs(row):
            stage = self.program_stage(program)
            if stage is not None:
                values.append(1.0 if predicate(stage, self.verdicts[program]) else 0.0)
        return mean(values)

    def case_score(self, case_id: str, row_ids: list[str] | None = None) -> float | None:
        rows = [r["case_variant_id"] for r in self.frozen.rows if r["case_id"] == case_id]
        if row_ids is not None:
            rows = [r for r in rows if r in row_ids]
        scores = [s for s in (self.row_stage_score(r) for r in rows) if s is not None]
        return mean(scores)

    def stage_family_mean(self, case_ids: list[str]) -> tuple[float | None, dict[str, Any]]:
        per_case = {c: self.case_score(c) for c in case_ids}
        applicable = [v for v in per_case.values() if v is not None]
        rows = [r["case_variant_id"] for r in self.frozen.rows if r["case_id"] in case_ids]
        stages = [self.program_stage(p) for r in self.frozen.rows if r["case_id"] in case_ids
                  for p in r["programs"]]
        stages = [s for s in stages if s is not None]
        detected_only = [float(self.frozen.stage_scores[s]) for s in stages if s != PBC]
        companion = {
            "n_cases_applicable": len(applicable),
            "n_cases_na": len(case_ids) - len(applicable),
            "per_variant_mean": mean([s for s in (self.row_stage_score(r) for r in rows) if s is not None]),
            "n_prevented_by_construction": sum(1 for s in stages if s == PBC),
            "n_detected": sum(1 for s in stages if s in {CTD, SCBE, RSD}),
            "detected_only_mean": mean(detected_only),
            "detected_only_rows": len(detected_only),
        }
        return mean(applicable), companion


def compute_metrics(frozen: Frozen, language: str, verdicts: dict[str, dict[str, Any]],
                    fault_reports: dict[str, str]) -> dict[str, Any]:
    recs = LanguageRecords(frozen, language, verdicts)
    all_cases = [c["case_id"] for c in frozen.asset["case_set"]["cases"]]
    derived = frozen.asset["metrics_computed_from_this_evidence"]
    out: dict[str, Any] = {}

    eed, eed_comp = recs.stage_family_mean(all_cases)
    out["Early_Error_Detection"] = {"score": eed, **eed_comp}
    bvs, bvs_comp = recs.stage_family_mean(list(derived["Boundary_Value_Safety"]["subset_case_ids"]))
    out["Boundary_Value_Safety"] = {"score": bvs, **bvs_comp}
    air, air_comp = recs.stage_family_mean(list(derived["Adversarial_Input_Robustness"]["subset_case_ids"]))
    out["Adversarial_Input_Robustness"] = {"score": air, **air_comp}

    # Runtime Safety: fractional per-row counts so a two-program row weighs once.
    numerator = 0.0
    denominator = 0.0
    all_rows_numerator = 0.0
    applicable_rows = 0.0
    for row in frozen.rows:
        rid = row["case_variant_id"]
        hit = recs.row_indicator(rid, lambda s, v: s in {RSD, PBC})
        reached = recs.row_indicator(rid, lambda s, v: s not in {CTD, SCBE})
        if hit is None or reached is None:
            continue
        applicable_rows += 1
        all_rows_numerator += hit
        # Only the part of the row that reached execution belongs in the denominator.
        row_obj = row
        for program in row_obj["programs"]:
            stage = recs.program_stage(program)
            if stage is None:
                continue
            weight = 1.0 / len(row_obj["programs"])
            if stage not in {CTD, SCBE}:
                denominator += weight
                if stage in {RSD, PBC}:
                    numerator += weight
    if applicable_rows == 0:
        runtime_safety = None
    elif denominator == 0:
        runtime_safety = 100.0
    else:
        runtime_safety = 100.0 * numerator / denominator
    out["Runtime_Safety"] = {
        "score": runtime_safety,
        "denominator_rows": denominator,
        "low_denominator": bool(applicable_rows and denominator < 8),
        "degenerate_denominator": bool(applicable_rows and denominator == 0),
        "Runtime_Safety_all_rows": (100.0 * all_rows_numerator / applicable_rows) if applicable_rows else None,
    }

    silent = []
    toolchain = []
    for row in frozen.rows:
        rid = row["case_variant_id"]
        s = recs.row_indicator(rid, lambda st, v: st in {SB, UB})
        t = recs.row_indicator(rid, lambda st, v: bool(
            {"toolchain_crash", "pre_execution_toolchain_crash", "toolchain_produced_no_artifact",
             "build_timeout"} & set(v.get("flags", []))
        ))
        if s is not None:
            silent.append(s)
        if t is not None:
            toolchain.append(t)
    out["Silent_Bug_Resistance"] = {
        "score": (100.0 * (1.0 - mean(silent))) if silent else None,
        "rows_applicable": len(silent),
        "silent_or_undefined_rows": sum(silent),
    }
    out["Implementation_Robustness"] = {
        "score": (100.0 * (1.0 - mean(toolchain))) if toolchain else None,
        "rows_applicable": len(toolchain),
        "toolchain_failure_rows": sum(toolchain),
    }

    # Type Safety: 0.5 * stage mean + 0.5 * static-guarantee fraction over 6 hazards.
    ts_rows = list(frozen.type_safety["row_set"]["rows"])
    ts_hazards = list(frozen.type_safety["row_set"]["hazards"])
    stage_parts: list[float] = []
    static_parts: list[float] = []
    n_static = n_dynamic = n_fabricated = n_pbc = 0
    for hazard in ts_hazards:
        rows = [r for r in ts_rows if r.startswith(hazard)]
        stage_scores = [s for s in (recs.row_stage_score(r) for r in rows) if s is not None]
        static_values = [s for s in (recs.row_indicator(r, lambda st, v: st in STATIC_STAGES) for r in rows) if s is not None]
        if stage_scores:
            stage_parts.append(mean(stage_scores))
        if static_values:
            static_parts.append(mean(static_values))
        for r in rows:
            row = next(x for x in frozen.rows if x["case_variant_id"] == r)
            for p in row["programs"]:
                st = recs.program_stage(p)
                n_static += st in STATIC_STAGES
                n_dynamic += st == RSD
                n_fabricated += st in {SB, UB}
                n_pbc += st == PBC
    ts_stage = mean(stage_parts)
    ts_static = 100.0 * mean(static_parts) if static_parts else None
    out["Type_Safety"] = {
        "score": (0.5 * ts_stage + 0.5 * ts_static) if ts_stage is not None and ts_static is not None else None,
        "TS_stage": ts_stage,
        "TS_static": ts_static,
        "hazards_applicable": len(stage_parts),
        "n_static": n_static, "n_dynamic_only": n_dynamic, "n_fabricated": n_fabricated,
        "n_prevented_by_construction": n_pbc,
    }

    # Memory Safety: integrity indicator per row, per-case mean over 9 cases.
    ms_cases = list(frozen.memory_safety["row_set"]["cases"])
    ms_rows = list(frozen.memory_safety["row_set"]["rows"])
    preserved_stages = set(frozen.memory_safety["formula"]["per_row_indicator"]["value_1_stages"])
    case_values: list[float] = []
    composition: dict[str, int] = {}
    for case_id in ms_cases:
        rows = [r for r in ms_rows if r.startswith(case_id)]
        values = [v for v in (recs.row_indicator(r, lambda st, v: st in preserved_stages) for r in rows) if v is not None]
        if values:
            case_values.append(mean(values))
        for r in rows:
            row = next(x for x in frozen.rows if x["case_variant_id"] == r)
            for p in row["programs"]:
                st = recs.program_stage(p)
                if st:
                    composition[st] = composition.get(st, 0) + 1
    out["Memory_Safety"] = {
        "score": (100.0 * mean(case_values)) if case_values else None,
        "cases_applicable": len(case_values),
        "MS_composition": composition,
    }

    # Debuggability: diagnostic-evidence proxy over the fault-report text.
    dbg_case_scores: list[float] = []
    a1s: list[float] = []
    a2s: list[float] = []
    a3s: list[float] = []
    n_reports = 0
    n_pbc_rungs = 0
    per_case_dbg: dict[str, float | None] = {}
    for case_id in all_cases:
        case = frozen.cases[case_id]
        rows = [r for r in frozen.rows if r["case_id"] == case_id]
        row_scores: list[float] = []
        for row in rows:
            program_scores: list[float] = []
            for program in row["programs"]:
                verdict = verdicts.get(program)
                if not verdict or verdict.get("stage_score") is None:
                    continue
                if verdict["earliest_observable_stage"] == PBC:
                    n_pbc_rungs += 1
                    program_scores.append(100.0)
                    continue
                report = fault_reports.get(program, "")
                if report.strip():
                    n_reports += 1
                    a1 = 1.0 if frozen.match_case_lexicon(case, report) else (
                        0.5 if frozen.match_global_lexicon(report) else 0.0
                    )
                    a2 = 1.0 if any(re.search(p, report) for p in DBG_POSITION_FORMS) else 0.0
                    a3 = 1.0 if any(re.search(p, report, re.MULTILINE) for p in DBG_KEY_FORMS) else 0.0
                else:
                    a1 = a2 = a3 = 0.0
                a1s.append(a1)
                a2s.append(a2)
                a3s.append(a3)
                program_scores.append(100.0 * (a1 + a2 + a3) / 3.0)
            if program_scores:
                row_scores.append(mean(program_scores))
        per_case_dbg[case_id] = mean(row_scores)
        if row_scores:
            dbg_case_scores.append(mean(row_scores))
    out["Debuggability"] = {
        "score": mean(dbg_case_scores),
        "label": "diagnostic-evidence proxy (debuggability.json); the frozen 80-session agent protocol was not executed",
        "cases_applicable": len(dbg_case_scores),
        "A1_mean": mean(a1s), "A2_mean": mean(a2s), "A3_mean": mean(a3s),
        "n_rows_with_fault_report": n_reports,
        "n_prevented_by_construction_rungs": n_pbc_rungs,
        "time_to_detection_human": "N/A - no human study performed",
        "time_to_root_cause_human": "N/A - no human study performed",
    }
    for key, value in out.items():
        value["score"] = clamp_score(value.get("score"))
    return out


def fault_report_for(record: dict[str, Any]) -> str:
    """debuggability.json fault_report_definition: failed-build output plus run #1 output, sentinels stripped."""
    parts: list[str] = []
    if record.get("build_command") and record.get("build_exit_status") not in (None, 0):
        parts.append(record.get("build_stdout") or "")
        parts.append(record.get("build_stderr") or "")
    elif record.get("build_command") and record.get("build_timed_out"):
        parts.append(record.get("build_stderr") or "")
    runs = record.get("runs") or []
    if runs:
        parts.append(runs[0].get("stdout") or "")
        parts.append(runs[0].get("stderr") or "")
    lines = []
    for line in "\n".join(parts).splitlines():
        if line.strip() in {"ADV-START", "ADV-END"} or line.startswith("OBS="):
            continue
        lines.append(line)
    return "\n".join(lines)


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------


def quidra_program_root(root: Path) -> str:
    primary = mm.load_json(root / "template" / "config" / "primary.json")
    return str((primary.get("language_quality") or {}).get("quidra_program_root") or "tests/benchmark/quidra")


def quidra_amendment(root: Path, programs_dir: Path) -> dict[str, Any] | None:
    path = programs_dir.parent / "quidra_type_binding_amendment.json"
    if not path.is_file():
        return None
    data = mm.load_json(path)
    if data.get("schema_version") != 1:
        raise MeasureError(f"unsupported Quidra type binding amendment schema: {path}")
    return data


def missing_comparison_toolchains(root: Path) -> list[str]:
    report_path = root / "results" / "toolchains.json"
    if not report_path.is_file():
        raise MeasureError("toolchains.json is missing; run toolchain-scan first")
    missing = [str(m) for m in mm.load_json(report_path).get("missing", [])]
    return sorted(set(missing) & set(COMPARISON_LANGUAGES))


def synthetic_result(root: Path, unit: dict[str, Any], out_dir: Path) -> int:
    scores = {lang: float(80 - index) for index, lang in enumerate(LANGUAGES)}
    requirements = {
        rid: dict(scores) for rid in unit.get("requirement_ids", [])
        if str(rid).startswith(("metric.", "condition."))
    }
    mm.dump_json(out_dir / "adversarial_raw.json", {
        "schema_version": 1, "synthetic_ci": True,
        "note": "Orchestration-only CI path; never enabled at /quidra-benchmark.",
    })
    mm.dump_json(out_dir / "result.json", {
        "schema_version": 1, "evaluation": "language_quality",
        "requirements": requirements, "evidence": {"synthetic_ci": True},
    })
    print(json.dumps({"ok": True, "unit_id": unit["id"], "synthetic_ci": True}, indent=2))
    return 0


def measure(root: Path, unit_id: str) -> int:
    unit = mm.manifest_unit(root, unit_id)
    if unit.get("runner_action") != "adversarial-measure":
        raise MeasureError(f"{unit_id} is not an adversarial-measure command unit")
    out_dir = root / "work" / "root" / "commands" / unit_id
    out_dir.mkdir(parents=True, exist_ok=True)
    if mm.synthetic_mode(root):
        return synthetic_result(root, unit, out_dir)

    missing = missing_comparison_toolchains(root)
    if missing:
        raise MeasureError(
            "missing required comparison toolchains for the adversarial case set: "
            + ", ".join(missing)
        )
    frozen = Frozen(root)
    locale = locale_value(root)
    quidra_root = quidra_program_root(root)
    quidra_dir = root / "repo" / quidra_root / "adversarial"
    amendment = quidra_amendment(root, quidra_dir) if quidra_dir.is_dir() else None
    compiler: Path | None = None
    quidra_na_reason: str | None = None
    if not quidra_dir.is_dir():
        quidra_na_reason = (
            f"authoring defect - Quidra adversarial programs are absent from the evaluated "
            f"snapshot at {quidra_root}/adversarial"
        )
    elif amendment is None:
        quidra_na_reason = "authoring defect - Quidra binding not frozen before measurement"
    else:
        compiler = mm.ensure_target_compiler(root)

    records: dict[str, dict[str, dict[str, Any]]] = {}
    verdicts: dict[str, dict[str, dict[str, Any]]] = {}
    reports: dict[str, dict[str, str]] = {}
    scored_programs = [p for row in frozen.rows for p in row["programs"]]
    secondary_programs = ["ADV-21_depth1000", "ADV-21_depth10000"]
    cells_root = out_dir / "cells"
    for language in LANGUAGES:
        slug = LANGUAGE_IDS[language]
        language_dir = quidra_dir if language == "Quidra" else (
            root / "template" / "programs" / slug / "adversarial"
        )
        records[language] = {}
        verdicts[language] = {}
        reports[language] = {}
        for program_id in scored_programs + secondary_programs:
            scored = program_id in scored_programs
            if language == "Quidra" and quidra_na_reason:
                if scored:
                    records[language][program_id] = {
                        "primary": {"case_variant_id": program_id, "language": language,
                                    "configuration": "primary",
                                    "na": {"reason": quidra_na_reason}},
                    }
                    verdicts[language][program_id] = {
                        "class": None, "earliest_observable_stage": None, "stage_score": None,
                        "decision_rule_fired": None, "stage_evidence": [],
                        "flags": ["na_authoring_defect"], "na": {"reason": quidra_na_reason},
                    }
                continue
            tm3 = frozen.tm3_for(language, program_id, amendment) if scored else None
            if tm3 and str(tm3.get("branch")) == "TM3a":
                # D0 fires before any tool runs: the construct does not exist in
                # this language, so there is no program to build. The frozen
                # determination and its citation are the whole record.
                records[language][program_id] = {
                    "primary": {"case_variant_id": program_id, "language": language,
                                "configuration": "primary", "tm3_applied": "TM3a",
                                "build_command": None, "runs": []},
                }
                verdicts[language][program_id] = classify(frozen, language, program_id,
                                                          records[language][program_id]["primary"],
                                                          None, tm3)
                reports[language][program_id] = ""
                continue
            source = resolve_source(language, language_dir, program_id)
            if source is None:
                if scored:
                    reason = f"authoring defect - no source for {program_id} under {language_dir}"
                    records[language][program_id] = {"primary": {"na": {"reason": reason}}}
                    verdicts[language][program_id] = {
                        "class": None, "earliest_observable_stage": None, "stage_score": None,
                        "decision_rule_fired": None, "stage_evidence": [],
                        "flags": ["na_authoring_defect"], "na": {"reason": reason},
                    }
                continue
            stem = program_stems(program_id)[0]
            primary_cell = Cell(frozen, language, program_id, source, language_dir, "primary",
                                cells_root / slug / "primary" / stem, compiler, locale)
            primary = primary_cell.execute()
            secondary = None
            if language == "Quidra":
                declared = (amendment or {}).get("secondary_configuration") or {}
                secondary_recipe = (
                    {"build": declared.get("build"), "run": declared.get("run")}
                    if declared.get("available") else None
                )
            else:
                secondary_recipe = frozen.secondary.get(language) or None
            if scored and secondary_recipe and secondary_recipe.get("run"):
                secondary_cell = Cell(frozen, language, program_id, source, language_dir, "secondary",
                                      cells_root / slug / "secondary" / stem, compiler, locale)
                secondary_cell.quidra_secondary = secondary_recipe
                secondary = secondary_cell.execute()
            records[language][program_id] = {"primary": primary, "secondary": secondary}
            if scored:
                verdict = classify_all_runs(frozen, language, program_id, primary, secondary, tm3)
                if tm3 and str(tm3.get("branch")) == "TM3b":
                    verdict["tm3_applied"] = "TM3b"
                verdicts[language][program_id] = verdict
                reports[language][program_id] = fault_report_for(primary)

    metrics = {
        language: compute_metrics(frozen, language, verdicts[language], reports[language])
        for language in LANGUAGES
    }
    requirements: dict[str, Any] = {}
    for rid in unit.get("requirement_ids", []):
        metric = REQUIREMENT_METRICS.get(str(rid))
        if metric is None:
            raise MeasureError(f"adversarial-measure received an unsupported requirement ID: {rid}")
        per_language: dict[str, Any] = {}
        for language in LANGUAGES:
            score = metrics[language][metric]["score"]
            if score is None:
                na_reasons = sorted({
                    str((v.get("na") or {}).get("reason", "not executed"))
                    for v in verdicts[language].values() if v.get("na")
                })
                per_language[language] = {
                    "status": "N/A",
                    "reason": "; ".join(na_reasons)[:400] or "no applicable rows",
                }
            else:
                per_language[language] = round(float(score), 2)
        requirements[rid] = per_language

    raw_path = out_dir / "adversarial_raw.json"
    # Captured bytes live in the cell directories; the raw file keeps the records
    # and verdicts compact enough to read.
    compact_records = {
        language: {
            program: {
                config: {k: v for k, v in (record or {}).items()
                         if k not in {"build_stdout", "build_stderr"}}
                | ({"runs": [{k: v for k, v in run.items() if k not in {"stdout", "stderr"}}
                             for run in (record or {}).get("runs", [])]}
                   if record and record.get("runs") is not None else {})
                for config, record in configs.items() if record is not None
            }
            for program, configs in language_records.items()
        }
        for language, language_records in records.items()
    }
    mm.dump_json(raw_path, {
        "schema_version": 1,
        "asset": str(root / "template" / ASSET_DIR / "adversarial_cases.json"),
        "asset_sha256": sha256_file(root / "template" / ASSET_DIR / "adversarial_cases.json"),
        "locale": locale,
        "quidra_program_root": quidra_root,
        "quidra_binding_amendment_sha256": (
            sha256_file(quidra_dir.parent / "quidra_type_binding_amendment.json") if amendment else None
        ),
        "quidra_na_reason": quidra_na_reason,
        "records": compact_records,
        "verdicts": verdicts,
        "metrics": metrics,
        "check_posture_note": frozen.asset["toolchain_binding"]["check_posture_disclosure"]["statement"],
    })
    mm.dump_json(out_dir / "result.json", {
        "schema_version": 1,
        "evaluation": "language_quality",
        "requirements": requirements,
        "evidence": {
            "raw": str(raw_path),
            "scorer": "adversarial_measure.py (mechanical replay of D0..D7)",
            "programs_per_language": len(scored_programs),
            "quidra_na_reason": quidra_na_reason,
        },
    })
    print(json.dumps({"ok": True, "unit_id": unit_id, "result": str(out_dir / "result.json")}, indent=2))
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Mechanical adversarial / safety scorer")
    sub = p.add_subparsers(dest="command", required=True)
    measure_p = sub.add_parser("measure")
    measure_p.add_argument("--workspace", required=True)
    measure_p.add_argument("--unit-id", required=True)
    return p


def main() -> int:
    args = build_parser().parse_args()
    try:
        root = mm.root_from(args.workspace)
        if args.command == "measure":
            return measure(root, args.unit_id)
        raise MeasureError(f"unknown command: {args.command}")
    except (mm.MeasureError, OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        print(f"adversarial measure error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
