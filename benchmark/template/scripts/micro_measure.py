#!/usr/bin/env python3
"""Mechanical micro-benchmark execution for Quidra Language Quality.

Quidra sources live in the evaluated snapshot (primary.json
language_quality.quidra_program_root) and are re-audited against the compiler
built from that snapshot by the `audit` command before anything is timed. This
script owns all repeatable work: the audit, target compiler build, correctness
validation, build/run timing, peak RSS, source/artifact sizes and normalization
into requirement-level 0-100 scores.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import time
from typing import Any

WORKLOADS = [f"mb{i:02d}" for i in range(1, 12)]
STARTUP_WORKLOAD = "mb00"
TEMPLATE_DIR = Path(__file__).resolve().parent.parent
LANGUAGES = list(
    json.loads(
        (TEMPLATE_DIR / "config" / "benchmark_metadata.json").read_text(encoding="utf-8")
    )["languages"]
)
CONFIGS = {
    "Quidra": ("quidra_native", "quidra", "qui"),
    "Python": ("python", "python", "py"),
    "C++": ("cpp", "cpp", "cpp"),
    "Rust": ("rust", "rust", "rs"),
    "Go": ("go", "go", "go"),
    "Java": ("java", "java", "java"),
    "TypeScript": ("typescript", "typescript", "ts"),
    "Kotlin": ("kotlin", "kotlin", "kt"),
    "Swift": ("swift", "swift", "swift"),
    "Zig": ("zig", "zig", "zig"),
}
TIMEOUT_SECONDS = 1800
IDLE_SECONDS = 2.0
HOST_LOAD_LIMIT = 2.0
HOST_WAIT_SECONDS = 600.0
HOST_POLL_SECONDS = 15.0
HOST_DEFERRED_RETRIES = 3


class MeasureError(RuntimeError):
    pass


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def dump_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def root_from(value: str) -> Path:
    root = Path(value).resolve()
    if not (root / "run.json").is_file() or not (root / "template").is_dir():
        raise MeasureError(f"invalid benchmark workspace: {root}")
    return root


def synthetic_mode(root: Path) -> bool:
    return (
        root != Path("/quidra-benchmark").resolve()
        and os.environ.get("QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS") == "1"
    )


def benchmark_env(root: Path, cwd: Path) -> dict[str, str]:
    env = {
        "HOME": str(root / "home"),
        "TMPDIR": str(root / "tmp"),
        "TMP": str(root / "tmp"),
        "TEMP": str(root / "tmp"),
        "PWD": str(cwd),
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin:/opt/homebrew/bin",
        "PYTHONNOUSERSITE": "1",
        "LC_ALL": "C",
        "LANG": "C",
        "TZ": "UTC",
    }
    for var, candidate in (
        ("JAVA_HOME", "/opt/homebrew/opt/openjdk"),
        ("JAVA_HOME", "/opt/java"),
        ("RUSTUP_HOME", "/opt/rust"),
        ("CARGO_HOME", "/opt/rust"),
    ):
        if var not in env and Path(candidate).exists():
            env[var] = candidate
    # Toolchains that keep a build cache must be able to write it somewhere the
    # sandbox allows; HOME is inside the workspace but say so explicitly.
    env.setdefault("GOCACHE", str(root / "tmp" / "go-build"))
    env.setdefault("GOPATH", str(root / "tmp" / "gopath"))
    env.setdefault("GOTOOLCHAIN", "local")
    return env


def run_command(
    argv: list[str], cwd: Path, env: dict[str, str], timeout: int = TIMEOUT_SECONDS
) -> dict[str, Any]:
    started = time.perf_counter_ns()
    try:
        p = subprocess.run(
            argv,
            cwd=cwd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
            shell=False,
        )
        return {
            "argv": argv,
            "exit_code": p.returncode,
            "stdout": p.stdout,
            "stderr": p.stderr,
            "wall_seconds": (time.perf_counter_ns() - started) / 1e9,
            "timed_out": False,
        }
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout.decode() if isinstance(exc.stdout, bytes) else (exc.stdout or "")
        stderr = exc.stderr.decode() if isinstance(exc.stderr, bytes) else (exc.stderr or "")
        return {
            "argv": argv,
            "exit_code": None,
            "stdout": stdout,
            "stderr": stderr,
            "wall_seconds": float(timeout),
            "timed_out": True,
        }


def require_ok(result: dict[str, Any], what: str) -> None:
    if result["timed_out"]:
        raise MeasureError(f"{what} timed out after {TIMEOUT_SECONDS}s")
    if result["exit_code"] != 0:
        raise MeasureError(
            f"{what} failed with exit {result['exit_code']}: {result['stderr'] or result['stdout']}"
        )


def ensure_target_compiler(root: Path) -> Path:
    record = root / "results" / "target_toolchain.json"
    if record.is_file():
        data = load_json(record)
        compiler = Path(str(data.get("compiler_path", "")))
        if compiler.is_file():
            return compiler

    build = root / "work" / "root" / "target-build"
    build.mkdir(parents=True, exist_ok=True)
    env = benchmark_env(root, build)
    configure = run_command(
        ["cmake", "-S", str(root / "repo"), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release"],
        root,
        env,
    )
    require_ok(configure, "Quidra target configure")
    jobs = max(1, min(os.cpu_count() or 1, 8))
    built = run_command(["cmake", "--build", str(build), f"-j{jobs}"], root, env)
    require_ok(built, "Quidra target build")
    candidates = [build / "quidra", build / "bin" / "quidra"]
    compiler = next((p for p in candidates if p.is_file()), None)
    if compiler is None:
        raise MeasureError("target build succeeded but the Quidra compiler was not found")
    version = run_command([str(compiler), "--version"], root, env, timeout=60)
    require_ok(version, "Quidra --version")
    dump_json(record, {
        "schema_version": 1,
        "compiler_path": str(compiler),
        "version": version["stdout"].strip(),
        "configure_wall_seconds": configure["wall_seconds"],
        "build_wall_seconds": built["wall_seconds"],
    })
    return compiler


def manifest_unit(root: Path, unit_id: str) -> dict[str, Any]:
    manifest = load_json(root / "work" / "root" / "manifest.json")
    for unit in manifest.get("work_units", []):
        if unit.get("id") == unit_id:
            return unit
    raise MeasureError(f"unknown work unit: {unit_id}")


def quidra_program_root(root: Path) -> Path:
    """Where the evaluated snapshot keeps its own benchmark programs.

    Quidra's programs are maintained with the compiler, under the path frozen
    in primary.json, and are read from the snapshot mounted read-only at
    /quidra-benchmark/repo. They are re-audited for every evaluated commit by
    the quidra-audit command unit before anything is measured.
    """
    primary = load_json(root / "template" / "config" / "primary.json")
    relative = str(
        (primary.get("language_quality") or {}).get("quidra_program_root")
        or "tests/benchmark/quidra"
    )
    return root / "repo" / relative


def quidra_representation_path(root: Path) -> Path:
    return quidra_program_root(root) / "representation.json"


def quidra_source_path(root: Path, workload: str) -> Path:
    path = quidra_program_root(root) / "micro" / f"{workload}.qui"
    if not path.is_file():
        raise MeasureError(
            f"missing Quidra source for {workload} in the evaluated snapshot: {path}"
        )
    return path


def expected_outputs(root: Path) -> dict[str, str]:
    text = (root / "template" / "workloads" / "micro.md").read_text(encoding="utf-8")
    out: dict[str, str] = {}
    for index, workload in enumerate(WORKLOADS, start=1):
        marker = f"### MB-{index:02d}"
        start = text.find(marker)
        if start < 0:
            raise MeasureError(f"missing workload section {marker}")
        next_start = text.find("\n### MB-", start + len(marker))
        section = text[start:] if next_start < 0 else text[start:next_start]
        tag = f"MB{index:02d}"
        found = None
        for block in re.findall(r"```[^\n]*\n(.*?)```", section, flags=re.DOTALL):
            for line in block.splitlines():
                if line.startswith(tag + " "):
                    found = line.strip()
                    break
            if found:
                break
        if not found:
            raise MeasureError(f"could not extract frozen expected output for {tag}")
        out[workload] = found
    return out


def checker_module(root: Path):
    path = root / "template" / "validators" / "micro" / "check_micro.py"
    spec = importlib.util.spec_from_file_location("quidra_micro_checker", path)
    if spec is None or spec.loader is None:
        raise MeasureError("could not load frozen micro validator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def validate_output(checker, expected: str, stdout: str) -> None:
    tag = expected.split()[0]
    actual_lines = [line.strip() for line in stdout.splitlines() if line.strip().startswith(tag + " ")]
    if len(actual_lines) != 1:
        raise MeasureError(f"expected exactly one {tag} result line, got {len(actual_lines)}")
    ok, details = checker.compare(expected + "\n", actual_lines[0] + "\n")
    if not ok:
        raise MeasureError(f"correctness validation failed for {tag}: {details}")


def validate_startup_output(stdout: str) -> None:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if lines != ["HELLO"]:
        raise MeasureError(f"startup probe expected exactly HELLO, got: {lines!r}")


def source_for(root: Path, language: str, workload: str) -> Path:
    _config, slug, ext = CONFIGS[language]
    if language == "Quidra":
        path = quidra_source_path(root, workload)
    else:
        path = root / "template" / "programs" / slug / "micro" / f"{workload}.{ext}"
    if not path.is_file():
        raise MeasureError(f"missing source for {language} {workload}: {path}")
    return path


def artifact_size(cell: dict[str, Any]) -> int:
    kind = cell["artifact_kind"]
    if kind == "none":
        return 0
    if kind == "file":
        path = Path(cell["artifact"])
        if not path.is_file():
            raise MeasureError(f"missing build artifact: {path}")
        return path.stat().st_size
    if kind == "class-tree":
        root = Path(cell["artifact"])
        files = list(root.rglob("*.class")) if root.exists() else []
        if not files:
            raise MeasureError(f"no class files produced under {root}")
        return sum(p.stat().st_size for p in files)
    raise MeasureError(f"unknown artifact kind: {kind}")


def clean_artifact(cell: dict[str, Any]) -> None:
    artifact = Path(cell["artifact"])
    if artifact.is_dir():
        shutil.rmtree(artifact)
    elif artifact.exists():
        artifact.unlink()
    js = cell.get("typescript_js")
    if js:
        p = Path(js)
        if p.exists():
            p.unlink()


def prepare_cell(root: Path, language: str, workload: str, compiler: Path) -> dict[str, Any]:
    config, _slug, ext = CONFIGS[language]
    work = root / "work" / "root" / "micro" / config / workload
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True, exist_ok=True)
    source = source_for(root, language, workload)
    local = work / f"{workload}.{ext}"
    shutil.copy2(source, local)
    binary = work / workload
    out_dir = work / "out"
    jar = work / f"{workload}.jar"
    js = work / f"{workload}.js"

    build_cmd: list[str] | None
    run_cmd: list[str]
    artifact: Path
    artifact_kind: str
    if language == "Quidra":
        build_cmd = [str(compiler), "build", str(local), "-o", str(binary)]
        run_cmd = [str(binary)]
        artifact, artifact_kind = binary, "file"
    elif language == "Python":
        build_cmd = None
        run_cmd = ["python3", str(local)]
        artifact, artifact_kind = local, "none"
    elif language == "C++":
        build_cmd = ["clang++", "-std=c++20", "-O2", str(local), "-o", str(binary)]
        run_cmd = [str(binary)]
        artifact, artifact_kind = binary, "file"
    elif language == "Rust":
        build_cmd = ["rustc", "-O", str(local), "-o", str(binary)]
        run_cmd = [str(binary)]
        artifact, artifact_kind = binary, "file"
    elif language == "Go":
        build_cmd = ["go", "build", "-o", str(binary), str(local)]
        run_cmd = [str(binary)]
        artifact, artifact_kind = binary, "file"
    elif language == "Java":
        build_cmd = ["javac", "-d", str(out_dir), str(local)]
        run_cmd = ["java", "-cp", str(out_dir), "Main"]
        artifact, artifact_kind = out_dir, "class-tree"
    elif language == "TypeScript":
        build_cmd = ["tsc", str(local)]
        run_cmd = ["node", str(js)]
        artifact, artifact_kind = js, "file"
    elif language == "Kotlin":
        build_cmd = ["kotlinc", str(local), "-include-runtime", "-d", str(jar)]
        run_cmd = ["java", "-jar", str(jar)]
        artifact, artifact_kind = jar, "file"
    elif language == "Swift":
        build_cmd = ["swiftc", "-O", str(local), "-o", str(binary)]
        run_cmd = [str(binary)]
        artifact, artifact_kind = binary, "file"
    elif language == "Zig":
        build_cmd = ["zig", "build-exe", "-OReleaseFast", str(local), f"-femit-bin={binary}"]
        run_cmd = [str(binary)]
        artifact, artifact_kind = binary, "file"
    else:
        raise MeasureError(language)
    return {
        "language": language,
        "config": config,
        "workload": workload,
        "workdir": str(work),
        "source": str(local),
        "build_cmd": build_cmd,
        "run_cmd": run_cmd,
        "artifact": str(artifact),
        "artifact_kind": artifact_kind,
        "typescript_js": str(js) if language == "TypeScript" else None,
    }


def build_once(root: Path, cell: dict[str, Any]) -> dict[str, Any] | None:
    cmd = cell["build_cmd"]
    if cmd is None:
        return None
    clean_artifact(cell)
    result = run_command(cmd, Path(cell["workdir"]), benchmark_env(root, Path(cell["workdir"])))
    require_ok(result, f"build {cell['language']} {cell['workload']}")
    return result


def run_once(root: Path, cell: dict[str, Any], mode: str = "once", k: int | None = None) -> dict[str, Any]:
    cmd = list(cell["run_cmd"])
    if mode == "steady":
        cmd.append("steady")
        if k is not None:
            cmd.append(str(k))
    elif mode != "once":
        cmd.append(mode)
    return run_command(cmd, Path(cell["workdir"]), benchmark_env(root, Path(cell["workdir"])))


def deterministic_order(run_id: str, workload: str, phase: str, round_index: int, cells: list[dict[str, Any]]) -> list[dict[str, Any]]:
    def key(cell: dict[str, Any]) -> str:
        seed = f"{run_id}|{workload}|{phase}|{round_index}|{cell['language']}"
        return hashlib.sha256(seed.encode("utf-8")).hexdigest()
    return sorted(cells, key=key)


def wait_for_host_window() -> tuple[bool, list[float]]:
    """Wait for one frozen contention window without aborting the whole command."""
    deadline = time.monotonic() + HOST_WAIT_SECONDS
    observations: list[float] = []
    while True:
        try:
            load = float(os.getloadavg()[0])
        except (AttributeError, OSError):
            return True, observations
        observations.append(load)
        if load < HOST_LOAD_LIMIT:
            return True, observations
        if time.monotonic() >= deadline:
            return False, observations
        time.sleep(HOST_POLL_SECONDS)


def run_host_gated_batches(
    items: list[str],
    phase: str,
    callback,
    host_checks: list[dict[str, Any]],
) -> set[str]:
    """Run batches serially, deferring contended batches and retrying them later.

    The initial pass is followed by at most HOST_DEFERRED_RETRIES passes over only
    the deferred batches. Unresolved batches become infrastructure N/A instead of
    aborting unrelated measurements or forcing the whole command to restart.
    """
    pending = list(items)
    for retry_index in range(HOST_DEFERRED_RETRIES + 1):
        if not pending:
            break
        deferred: list[str] = []
        for item in pending:
            ready, loads = wait_for_host_window()
            host_checks.append({
                "phase": phase,
                "batch": item,
                "attempt": retry_index + 1,
                "load_avg_1min_observations": loads,
                "status": "ready" if ready else "deferred",
            })
            if ready:
                callback(item)
            else:
                deferred.append(item)
        pending = deferred
    unresolved = set(pending)
    for item in sorted(unresolved):
        host_checks.append({
            "phase": phase,
            "batch": item,
            "status": "N/A",
            "na_reason": "host_contention_unresolved",
            "attempts": HOST_DEFERRED_RETRIES + 1,
        })
    return unresolved


def pause() -> None:
    time.sleep(IDLE_SECONDS)


# Peak RSS is read from the kernel's own accounting of the measured child, not
# from a platform-specific `time` binary: a throwaway interpreter runs the
# program and reports getrusage(RUSAGE_CHILDREN), which only that one child can
# have contributed to. Linux reports ru_maxrss in kilobytes, Darwin in bytes.
RSS_WRAPPER = (
    "import json, resource, subprocess, sys\n"
    "p = subprocess.run(sys.argv[1:])\n"
    "r = resource.getrusage(resource.RUSAGE_CHILDREN)\n"
    "scale = 1 if sys.platform == 'darwin' else 1024\n"
    "sys.stderr.write('\\n@@rss ' + json.dumps({'peak_rss_bytes': r.ru_maxrss * scale}) + '\\n')\n"
    "sys.exit(p.returncode)\n"
)


def wrapped_memory_run(root: Path, cell: dict[str, Any]) -> dict[str, Any]:
    cmd = [sys.executable, "-c", RSS_WRAPPER, *cell["run_cmd"]]
    result = run_command(cmd, Path(cell["workdir"]), benchmark_env(root, Path(cell["workdir"])))
    if not result["timed_out"] and result["exit_code"] != 0:
        raise MeasureError(
            f"wrapped run failed for {cell['language']} {cell['workload']}: {result['stderr']}"
        )
    m = re.search(r"(?m)^@@rss (\{.*\})\s*$", result["stderr"])
    if not m:
        raise MeasureError(
            f"could not read peak RSS for {cell['language']} {cell['workload']}"
        )
    result["peak_rss_bytes"] = int(json.loads(m.group(1))["peak_rss_bytes"])
    result["stderr"] = result["stderr"][: m.start()].rstrip()
    return result


def median(values: list[float]) -> float:
    if not values:
        raise MeasureError("cannot take median of empty sample set")
    return float(statistics.median(values))


def summarize(values: list[float]) -> dict[str, Any]:
    m = median(values)
    deviations = [abs(v - m) for v in values]
    return {
        "samples": values,
        "n": len(values),
        "median": m,
        "min": min(values),
        "max": max(values),
        "mean": float(statistics.mean(values)),
        "stdev": float(statistics.stdev(values)) if len(values) > 1 else 0.0,
        "mad": median(deviations),
    }


def true_spawn_epsilon(root: Path) -> tuple[float, list[float]]:
    env = benchmark_env(root, root)
    samples = []
    for _ in range(20):
        r = run_command(["/usr/bin/true"], root, env, timeout=60)
        require_ok(r, "/usr/bin/true calibration")
        samples.append(float(r["wall_seconds"]))
    raw = median(samples)
    if raw <= 0:
        raise MeasureError("spawn calibration produced non-positive median")
    epsilon = 10.0 ** math.ceil(math.log10(raw))
    return epsilon, samples


def na_score(reason: str) -> dict[str, str]:
    return {"status": "N/A", "reason": reason}


def family_c_workload_scores(
    raw: dict[str, dict[str, float | None]],
    epsilon: float = 0.0,
) -> dict[str, float | dict[str, str]]:
    per_language: dict[str, list[float]] = {lang: [] for lang in LANGUAGES}
    for workload in WORKLOADS:
        available = {
            lang: float(value)
            for lang, value in raw[workload].items()
            if value is not None
        }
        if not available:
            continue
        if epsilon == 0.0 and any(v <= 0 for v in available.values()):
            raise MeasureError(
                f"non-positive raw value for unshifted family C: {workload} {available}"
            )
        best = min(available.values())
        for lang, value in available.items():
            score = (
                100.0 * (best + epsilon) / (value + epsilon)
                if epsilon > 0.0
                else 100.0 * best / value
            )
            per_language[lang].append(max(0.0, min(100.0, score)))
    return {
        lang: (
            float(statistics.mean(scores))
            if scores
            else na_score("host_contention_unresolved")
        )
        for lang, scores in per_language.items()
    }


def family_c_language_scores(
    raw: dict[str, float | None],
) -> dict[str, float | dict[str, str]]:
    if set(raw) != set(LANGUAGES):
        raise MeasureError("single-value family C input must contain all fixed languages")
    available = {
        lang: float(value)
        for lang, value in raw.items()
        if value is not None
    }
    if not available:
        return {
            lang: na_score("host_contention_unresolved")
            for lang in LANGUAGES
        }
    if any(v <= 0.0 for v in available.values()):
        raise MeasureError(f"single-value family C input must be positive: {available}")
    best = min(available.values())
    return {
        lang: (
            max(0.0, min(100.0, 100.0 * best / float(raw[lang])))
            if raw[lang] is not None
            else na_score("host_contention_unresolved")
        )
        for lang in LANGUAGES
    }


def summarize_or_na(values: list[float], reason: str | None = None) -> dict[str, Any]:
    if values:
        return summarize(values)
    return {
        "status": "N/A",
        "reason": reason or "measurement_unavailable",
        "samples": [],
        "n": 0,
    }


def parse_steady_samples(stdout: str) -> list[float]:
    values = []
    for line in stdout.splitlines():
        m = re.fullmatch(r"ITER\s+(\d+)\s+(\d+)", line.strip())
        if m:
            values.append(int(m.group(2)) / 1e9)
    return values


def quidra_representation_schema(root: Path) -> dict[str, Any]:
    path = root / "template" / "config" / "quidra_representation_schema.json"
    if not path.is_file():
        raise MeasureError("Quidra representation schema is missing from the frozen template")
    schema = load_json(path)
    if schema.get("schema_version") != 1:
        raise MeasureError("Quidra representation schema_version must be 1")
    return schema


def validate_quidra_representation(root: Path, representation_path: Path) -> dict[str, Any]:
    if not representation_path.is_file():
        raise MeasureError("Quidra current-run representation manifest is missing")
    schema = quidra_representation_schema(root)
    representation = load_json(representation_path)
    if representation.get("schema_version") != schema.get("schema_version"):
        raise MeasureError("quidra_representation.json schema_version mismatch")
    expected_commit = str(
        load_json(root / "run.json").get("evaluated", {}).get("commit_sha", "")
    )
    recorded = representation.get("evaluated_commit_sha")
    # A manifest maintained inside the evaluated snapshot cannot know its own
    # commit; it says so with the literal sentinel and is bound to the commit by
    # being part of it.
    if recorded != "evaluated-snapshot" and (not expected_commit or recorded != expected_commit):
        raise MeasureError("Quidra representation manifest commit SHA mismatch")
    required_fields = set(schema.get("required_top_level_fields", []))
    missing_fields = sorted(required_fields - set(representation))
    if missing_fields:
        raise MeasureError(
            "Quidra representation manifest missing fields: "
            + ", ".join(missing_fields)
        )
    required_sections = set(schema.get("required_object_sections", []))
    if not required_sections:
        raise MeasureError("Quidra representation schema has no required object sections")
    for section in sorted(required_sections):
        value = representation.get(section)
        if not isinstance(value, dict) or not value:
            raise MeasureError(
                f"Quidra representation section {section!r} must be a non-empty object"
            )
    evidence = representation.get("documentation_evidence")
    if (
        not isinstance(evidence, list)
        or not evidence
        or not all(isinstance(x, str) and x.strip() for x in evidence)
    ):
        raise MeasureError(
            "Quidra representation manifest requires non-empty documentation_evidence"
        )
    docs_root = (root / "repo" / "docs").resolve()
    for item in evidence:
        source = item.split("#", 1)[0].strip()
        if not source.startswith("repo/docs/"):
            raise MeasureError(
                "Quidra representation evidence must reference current repo/docs files"
            )
        evidence_path = (root / source).resolve()
        if not evidence_path.is_relative_to(docs_root) or not evidence_path.is_file():
            raise MeasureError(
                f"Quidra representation evidence file does not exist in current snapshot: {source}"
            )
    return representation


def audit(root: Path, unit_id: str) -> int:
    """Re-audit the snapshot's Quidra benchmark programs against its own compiler.

    This is the `quidra-audit` command unit. It replaces per-run authoring of
    Quidra programs by a model: the programs live in the evaluated snapshot and
    are maintained with the compiler, so a run's job is to prove they are current
    - every micro program builds with the compiler built from this commit, runs
    once, matches the frozen oracle and speaks steady mode; every scored
    adversarial program is present with the frozen skeleton; the generated
    adversarial sources are exactly what the frozen generators produce; and the
    representation manifest and type-binding amendment are valid. Any failure is
    an authoring/infrastructure blocker for the evaluation, never a language
    score.
    """
    unit = manifest_unit(root, unit_id)
    if unit.get("runner_action") != "quidra-audit":
        raise MeasureError(f"{unit_id} is not a quidra-audit command unit")
    out_dir = root / "work" / "root" / "commands" / unit_id
    out_dir.mkdir(parents=True, exist_ok=True)
    gate = "gate.quidra_programs_current"
    if synthetic_mode(root):
        dump_json(out_dir / "result.json", {
            "schema_version": 1,
            "evaluation": "language_quality",
            "requirements": {gate: True},
            "evidence": {"synthetic_ci": True},
        })
        print(json.dumps({"ok": True, "unit_id": unit_id, "synthetic_ci": True}, indent=2))
        return 0

    programs = quidra_program_root(root)
    problems: list[str] = []
    evidence: dict[str, Any] = {"program_root": str(programs), "micro": [], "adversarial": {}}
    if not programs.is_dir():
        raise MeasureError(
            f"quidra benchmark programs are absent from the evaluated snapshot at {programs}"
        )

    representation_path = quidra_representation_path(root)
    try:
        validate_quidra_representation(root, representation_path)
        evidence["representation_sha256"] = hashlib.sha256(
            representation_path.read_bytes()
        ).hexdigest()
    except MeasureError as exc:
        problems.append(f"representation.json: {exc}")

    amendment_path = programs / "quidra_type_binding_amendment.json"
    if not amendment_path.is_file():
        problems.append("quidra_type_binding_amendment.json is missing")
    else:
        amendment = load_json(amendment_path)
        evidence["amendment_sha256"] = hashlib.sha256(amendment_path.read_bytes()).hexdigest()
        for key in ("default_arithmetic_type", "fixed_width_i64", "fixed_width_i32",
                    "fixed_width_u32", "float64", "default_string_type",
                    "default_ordered_sequence", "most_general_reference", "null_or_absent_value"):
            row = (amendment.get("bindings") or {}).get(key) or {}
            if not row.get("construct") or not row.get("citation"):
                problems.append(f"amendment: binding {key} lacks a construct or citation")

    compiler = ensure_target_compiler(root)
    expected = expected_outputs(root)
    checker = checker_module(root)
    for workload in [STARTUP_WORKLOAD, *WORKLOADS]:
        try:
            source = quidra_source_path(root, workload)
            cell = prepare_cell(root, "Quidra", workload, compiler)
            build = build_once(root, cell)
            run = run_once(root, cell)
            require_ok(run, f"Quidra correctness run {workload}")
            if workload == STARTUP_WORKLOAD:
                validate_startup_output(run["stdout"])
            else:
                validate_output(checker, expected[workload], run["stdout"])
                steady = run_once(root, cell, "steady", 3)
                require_ok(steady, f"Quidra steady-mode smoke {workload}")
                if len(parse_steady_samples(steady["stdout"])) != 3:
                    raise MeasureError(f"{workload} steady mode did not emit exactly 3 ITER rows")
                validate_output(checker, expected[workload], steady["stdout"])
            evidence["micro"].append({
                "workload": workload,
                "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "build_wall_seconds": build["wall_seconds"] if build else 0.0,
                "correct": True,
            })
        except MeasureError as exc:
            problems.append(f"micro {workload}: {exc}")

    adversarial = programs / "adversarial"
    generator = adversarial / "generate.py"
    if generator.is_file():
        check = run_command(
            [sys.executable, str(generator), "--check"], adversarial,
            benchmark_env(root, adversarial), timeout=120,
        )
        if check["exit_code"] != 0:
            problems.append("adversarial generated sources are stale: " + (check["stderr"] or check["stdout"]).strip()[:300])
    else:
        problems.append("adversarial/generate.py is missing")
    asset = load_json(
        root / "template" / "methodology-assets" / "language_quality" / "adversarial_cases.json"
    )
    tm3a = set()
    if amendment_path.is_file():
        tm3a = {
            k for k, v in (load_json(amendment_path).get("tm3_determinations") or {}).items()
            if v.get("branch") == "TM3a"
        }
    present = 0
    for row in asset["fixed_scored_case_variant_list"]["rows"]:
        for program in row["programs"]:
            base, _, variant = program.partition("/")
            case_id = base.split("_", 1)[0]
            if case_id in tm3a or program in tm3a:
                continue
            stem = f"{base}_{variant}" if variant else base
            source = adversarial / f"{stem}.qui"
            if not source.is_file():
                problems.append(f"adversarial {program}: no source {source.name}")
                continue
            text = source.read_text(encoding="utf-8", errors="replace")
            if program not in ("ADV-22a", "ADV-22b") and not all(
                marker in text for marker in ('print("ADV-START")', 'print("ADV-END")', "OBS=")
            ):
                problems.append(f"adversarial {program}: frozen skeleton lines are missing")
            present += 1
    evidence["adversarial"] = {"programs_present": present, "tm3a_cases": sorted(tm3a)}

    if problems:
        dump_json(out_dir / "result.json", {
            "schema_version": 1,
            "evaluation": "language_quality",
            "requirements": {gate: False},
            "evidence": {**evidence, "problems": problems},
        })
        raise MeasureError(
            "quidra benchmark programs audit failed: " + "; ".join(problems)[:1500]
        )
    dump_json(out_dir / "result.json", {
        "schema_version": 1,
        "evaluation": "language_quality",
        "requirements": {gate: True},
        "evidence": evidence,
    })
    print(json.dumps({"ok": True, "unit_id": unit_id, "result": str(out_dir / "result.json")}, indent=2))
    return 0



def measure(root: Path, unit_id: str) -> int:
    unit = manifest_unit(root, unit_id)
    if unit.get("runner_action") != "micro-measure":
        raise MeasureError(f"{unit_id} is not a micro-measure command unit")
    if synthetic_mode(root):
        out_dir = root / "work" / "root" / "commands" / unit_id
        scores = {lang: float(90 - index) for index, lang in enumerate(LANGUAGES)}
        requirements = {
            rid: dict(scores)
            for rid in unit.get("requirement_ids", [])
            if str(rid).startswith(("metric.", "condition."))
        }
        dump_json(out_dir / "micro_raw.json", {
            "schema_version": 1,
            "synthetic_ci": True,
            "note": "Orchestration-only CI path; never enabled at /quidra-benchmark.",
        })
        dump_json(out_dir / "result.json", {
            "schema_version": 1,
            "evaluation": "language_quality",
            "requirements": requirements,
            "evidence": {"synthetic_ci": True},
        })
        print(json.dumps({"ok": True, "unit_id": unit_id, "synthetic_ci": True}, indent=2))
        return 0
    validate_quidra_representation(root, quidra_representation_path(root))
    compiler = ensure_target_compiler(root)
    expected = expected_outputs(root)
    checker = checker_module(root)
    primary = load_json(root / "template" / "config" / "primary.json")
    warmups = int(primary["timing"]["warmups_per_cell"])
    measured = int(primary["timing"]["measured_runs_per_cell"])
    run_id = str(load_json(root / "run.json").get("run_id", "unknown-run"))
    out_dir = root / "work" / "root" / "commands" / unit_id
    raw_path = out_dir / "micro_raw.json"
    out_dir.mkdir(parents=True, exist_ok=True)

    required_bins = ["cmake", "python3", "clang++", "rustc", "go", "javac", "java", "tsc", "node", "kotlinc", "swiftc", "zig"]
    missing = [name for name in required_bins if shutil.which(name) is None]
    if missing:
        raise MeasureError("missing required micro toolchains: " + ", ".join(missing))

    cells: dict[str, dict[str, dict[str, Any]]] = {w: {} for w in WORKLOADS}
    for workload in WORKLOADS:
        for language in LANGUAGES:
            cells[workload][language] = prepare_cell(root, language, workload, compiler)

    correctness = []
    for workload in WORKLOADS:
        for language in LANGUAGES:
            cell = cells[workload][language]
            if cell["build_cmd"] is not None:
                build_once(root, cell)
            run = run_once(root, cell)
            require_ok(run, f"correctness run {language} {workload}")
            validate_output(checker, expected[workload], run["stdout"])
            correctness.append({"workload": workload, "language": language, "correct": True})

    compile_epsilon, spawn_samples = true_spawn_epsilon(root)
    compile_samples: dict[str, dict[str, list[float]]] = {
        w: {lang: [] for lang in LANGUAGES} for w in WORKLOADS
    }
    cold_samples: dict[str, dict[str, list[float]]] = {
        w: {lang: [] for lang in LANGUAGES} for w in WORKLOADS
    }
    rss_samples: dict[str, dict[str, list[float]]] = {
        w: {lang: [] for lang in LANGUAGES} for w in WORKLOADS
    }
    steady_samples: dict[str, dict[str, list[float]]] = {
        w: {lang: [] for lang in LANGUAGES} for w in WORKLOADS
    }
    schedules: list[dict[str, Any]] = []
    host_checks: list[dict[str, Any]] = []
    compile_na: set[str] = set()
    cold_na: set[str] = set()
    rss_na: set[str] = set()
    steady_na: set[str] = set()
    startup_na = False

    startup_cells = {
        language: prepare_cell(root, language, STARTUP_WORKLOAD, compiler)
        for language in LANGUAGES
    }
    for language in LANGUAGES:
        cell = startup_cells[language]
        if cell["build_cmd"] is not None:
            build_once(root, cell)
        run = run_once(root, cell)
        require_ok(run, f"startup correctness run {language}")
        validate_startup_output(run["stdout"])
        correctness.append({
            "workload": STARTUP_WORKLOAD,
            "language": language,
            "correct": True,
        })

    startup_samples: dict[str, list[float]] = {lang: [] for lang in LANGUAGES}
    startup_rss_samples: dict[str, list[float]] = {lang: [] for lang in LANGUAGES}
    startup_group = [startup_cells[lang] for lang in LANGUAGES]

    def measure_startup(_batch: str) -> None:
        for round_index in range(warmups):
            order = deterministic_order(
                run_id, STARTUP_WORKLOAD, "startup-warmup", round_index, startup_group
            )
            schedules.append({
                "workload": STARTUP_WORKLOAD,
                "phase": "startup-warmup",
                "round": round_index,
                "order": [c["language"] for c in order],
            })
            for cell in order:
                run = run_once(root, cell)
                require_ok(run, f"startup warmup {cell['language']}")
                validate_startup_output(run["stdout"])
                pause()
        for round_index in range(measured):
            order = deterministic_order(
                run_id, STARTUP_WORKLOAD, "startup", round_index, startup_group
            )
            schedules.append({
                "workload": STARTUP_WORKLOAD,
                "phase": "startup",
                "round": round_index,
                "order": [c["language"] for c in order],
            })
            for cell in order:
                run = run_once(root, cell)
                require_ok(run, f"startup measured run {cell['language']}")
                validate_startup_output(run["stdout"])
                startup_samples[cell["language"]].append(float(run["wall_seconds"]))
                pause()

    startup_na = bool(
        run_host_gated_batches(
            [STARTUP_WORKLOAD], "startup", measure_startup, host_checks
        )
    )

    def measure_startup_rss(_batch: str) -> None:
        for round_index in range(warmups):
            order = deterministic_order(
                run_id, STARTUP_WORKLOAD, "startup-rss-warmup", round_index, startup_group
            )
            schedules.append({
                "workload": STARTUP_WORKLOAD,
                "phase": "startup-rss-warmup",
                "round": round_index,
                "order": [c["language"] for c in order],
            })
            for cell in order:
                run = wrapped_memory_run(root, cell)
                if not run["timed_out"]:
                    validate_startup_output(run["stdout"])
                pause()
        for round_index in range(measured):
            order = deterministic_order(
                run_id, STARTUP_WORKLOAD, "startup-rss", round_index, startup_group
            )
            schedules.append({
                "workload": STARTUP_WORKLOAD,
                "phase": "startup-rss",
                "round": round_index,
                "order": [c["language"] for c in order],
            })
            for cell in order:
                run = wrapped_memory_run(root, cell)
                if run["timed_out"]:
                    raise MeasureError(
                        f"startup RSS run timed out for {cell['language']}; no valid peak RSS"
                    )
                validate_startup_output(run["stdout"])
                startup_rss_samples[cell["language"]].append(
                    float(run["peak_rss_bytes"])
                )
                pause()

    startup_rss_na = bool(
        run_host_gated_batches(
            [STARTUP_WORKLOAD], "startup-rss", measure_startup_rss, host_checks
        )
    )

    def measure_compile(workload: str) -> None:
        build_cells = [
            cells[workload][lang]
            for lang in LANGUAGES
            if cells[workload][lang]["build_cmd"] is not None
        ]
        for round_index in range(measured):
            order = deterministic_order(
                run_id, workload, "compile", round_index, build_cells
            )
            schedules.append({
                "workload": workload,
                "phase": "compile",
                "round": round_index,
                "order": [c["language"] for c in order],
            })
            for cell in order:
                result = build_once(root, cell)
                assert result is not None
                compile_samples[workload][cell["language"]].append(
                    float(result["wall_seconds"])
                )
                pause()
        for lang in LANGUAGES:
            if cells[workload][lang]["build_cmd"] is None:
                compile_samples[workload][lang] = [0.0] * measured

    compile_na = run_host_gated_batches(
        WORKLOADS, "compile", measure_compile, host_checks
    )
    # Downstream execution still needs a current artifact even when compile timing
    # for a workload became infrastructure N/A.
    for workload in WORKLOADS:
        for lang in LANGUAGES:
            if cells[workload][lang]["build_cmd"] is not None:
                build_once(root, cells[workload][lang])

    def measure_cold(workload: str) -> None:
        group = [cells[workload][lang] for lang in LANGUAGES]
        for round_index in range(warmups):
            order = deterministic_order(
                run_id, workload, "cold-warmup", round_index, group
            )
            schedules.append({
                "workload": workload,
                "phase": "cold-warmup",
                "round": round_index,
                "order": [c["language"] for c in order],
            })
            for cell in order:
                r = run_once(root, cell)
                require_ok(r, f"cold warmup {cell['language']} {workload}")
                validate_output(checker, expected[workload], r["stdout"])
                pause()
        for round_index in range(measured):
            order = deterministic_order(run_id, workload, "cold", round_index, group)
            schedules.append({
                "workload": workload,
                "phase": "cold",
                "round": round_index,
                "order": [c["language"] for c in order],
            })
            for cell in order:
                r = run_once(root, cell)
                if r["timed_out"]:
                    cold_samples[workload][cell["language"]].append(
                        float(TIMEOUT_SECONDS)
                    )
                else:
                    require_ok(r, f"cold run {cell['language']} {workload}")
                    validate_output(checker, expected[workload], r["stdout"])
                    cold_samples[workload][cell["language"]].append(
                        float(r["wall_seconds"])
                    )
                pause()

    cold_na = run_host_gated_batches(
        WORKLOADS, "cold", measure_cold, host_checks
    )

    def measure_rss(workload: str) -> None:
        group = [cells[workload][lang] for lang in LANGUAGES]
        for round_index in range(warmups):
            order = deterministic_order(
                run_id, workload, "rss-warmup", round_index, group
            )
            schedules.append({
                "workload": workload,
                "phase": "rss-warmup",
                "round": round_index,
                "order": [c["language"] for c in order],
            })
            for cell in order:
                r = wrapped_memory_run(root, cell)
                if not r["timed_out"]:
                    validate_output(checker, expected[workload], r["stdout"])
                pause()
        for round_index in range(measured):
            order = deterministic_order(run_id, workload, "rss", round_index, group)
            schedules.append({
                "workload": workload,
                "phase": "rss",
                "round": round_index,
                "order": [c["language"] for c in order],
            })
            for cell in order:
                r = wrapped_memory_run(root, cell)
                if r["timed_out"]:
                    raise MeasureError(
                        f"RSS run timed out for {cell['language']} {workload}; "
                        "no valid peak RSS"
                    )
                validate_output(checker, expected[workload], r["stdout"])
                rss_samples[workload][cell["language"]].append(
                    float(r["peak_rss_bytes"])
                )
                pause()

    rss_na = run_host_gated_batches(
        WORKLOADS, "rss", measure_rss, host_checks
    )

    def measure_steady(workload: str) -> None:
        group = [cells[workload][lang] for lang in LANGUAGES]
        for process_index in range(2):
            order = deterministic_order(
                run_id, workload, "steady", process_index, group
            )
            schedules.append({
                "workload": workload,
                "phase": "steady",
                "round": process_index,
                "order": [c["language"] for c in order],
            })
            for cell in order:
                lang = cell["language"]
                if cold_samples[workload][lang]:
                    cold_median = median(cold_samples[workload][lang])
                    if cold_median > 600.0:
                        k = 3
                    elif cold_median > 257.0:
                        k = max(
                            3,
                            min(7, int(math.floor(TIMEOUT_SECONDS / cold_median))),
                        )
                    else:
                        k = 7
                else:
                    # Cold measurement may be infrastructure N/A while a later
                    # steady batch is runnable. Use the conservative frozen minimum.
                    k = 3
                r = run_once(root, cell, "steady", k)
                iterations = parse_steady_samples(r["stdout"])
                kept = iterations[2:]
                if not r["timed_out"]:
                    require_ok(r, f"steady run {lang} {workload}")
                    validate_output(checker, expected[workload], r["stdout"])
                elif not kept:
                    kept = [float(TIMEOUT_SECONDS)]
                steady_samples[workload][lang].extend(kept)
                pause()

    steady_na = run_host_gated_batches(
        WORKLOADS, "steady", measure_steady, host_checks
    )

    source_raw: dict[str, dict[str, float | None]] = {w: {} for w in WORKLOADS}
    artifact_raw: dict[str, dict[str, float | None]] = {w: {} for w in WORKLOADS}
    compile_raw: dict[str, dict[str, float | None]] = {w: {} for w in WORKLOADS}
    cold_raw: dict[str, dict[str, float | None]] = {w: {} for w in WORKLOADS}
    rss_raw: dict[str, dict[str, float | None]] = {w: {} for w in WORKLOADS}
    steady_raw: dict[str, dict[str, float | None]] = {w: {} for w in WORKLOADS}
    summaries: dict[str, Any] = {}
    for workload in WORKLOADS:
        summaries[workload] = {}
        for language in LANGUAGES:
            cell = cells[workload][language]
            source_raw[workload][language] = float(Path(cell["source"]).stat().st_size)
            artifact_raw[workload][language] = float(artifact_size(cell))
            compile_raw[workload][language] = (
                median(compile_samples[workload][language])
                if compile_samples[workload][language]
                else None
            )
            cold_raw[workload][language] = (
                median(cold_samples[workload][language])
                if cold_samples[workload][language]
                else None
            )
            rss_raw[workload][language] = (
                median(rss_samples[workload][language])
                if rss_samples[workload][language]
                else None
            )
            steady_raw[workload][language] = (
                median(steady_samples[workload][language])
                if steady_samples[workload][language]
                else None
            )
            summaries[workload][language] = {
                "compile": summarize_or_na(
                    compile_samples[workload][language],
                    "host_contention_unresolved" if workload in compile_na else None,
                ),
                "cold": summarize_or_na(
                    cold_samples[workload][language],
                    "host_contention_unresolved" if workload in cold_na else None,
                ),
                "rss": summarize_or_na(
                    rss_samples[workload][language],
                    "host_contention_unresolved" if workload in rss_na else None,
                ),
                "steady": summarize_or_na(
                    steady_samples[workload][language],
                    "host_contention_unresolved" if workload in steady_na else None,
                ),
                "source_bytes": int(source_raw[workload][language]),
                "artifact_bytes": int(artifact_raw[workload][language]),
            }

    startup_raw: dict[str, float | None] = {
        lang: (
            median(startup_samples[lang])
            if startup_samples[lang]
            else None
        )
        for lang in LANGUAGES
    }
    runtime_overhead_raw: dict[str, float | None] = {
        lang: (
            median(startup_rss_samples[lang])
            if startup_rss_samples[lang]
            else None
        )
        for lang in LANGUAGES
    }
    all_requirements = {
        "metric.native_execution_performance": family_c_workload_scores(cold_raw),
        "metric.long_running_performance": family_c_workload_scores(steady_raw),
        "metric.compile_build_performance": family_c_workload_scores(compile_raw, compile_epsilon),
        "metric.startup_latency": family_c_language_scores(startup_raw),
        "metric.memory_efficiency": family_c_workload_scores(rss_raw),
        "metric.runtime_overhead": family_c_language_scores(runtime_overhead_raw),
        "metric.source_code_size": family_c_workload_scores(source_raw),
        "metric.binary_artifact_size": family_c_workload_scores(artifact_raw, 4096.0),
    }
    assigned_requirement_ids = list(unit.get("requirement_ids", []))
    unsupported = sorted(set(assigned_requirement_ids) - set(all_requirements))
    if unsupported:
        raise MeasureError(
            "micro-measure received unsupported requirement IDs: " + ", ".join(unsupported)
        )
    requirements = {
        rid: all_requirements[rid]
        for rid in assigned_requirement_ids
    }
    result = {
        "schema_version": 1,
        "evaluation": "language_quality",
        "requirements": requirements,
        "evidence": {
            "raw": str(raw_path),
            "compile_epsilon_seconds": compile_epsilon,
            "artifact_epsilon_bytes": 4096,
            "target_compiler": str(compiler),
        },
    }
    dump_json(raw_path, {
        "schema_version": 1,
        "run_id": run_id,
        "warmups_per_cell": warmups,
        "measured_runs_per_cell": measured,
        "spawn_calibration_seconds": spawn_samples,
        "compile_epsilon_seconds": compile_epsilon,
        "correctness": correctness,
        "measurement_schedule": schedules,
        "host_contention_checks": host_checks,
        "startup": {
            lang: summarize_or_na(
                startup_samples[lang],
                "host_contention_unresolved" if startup_na else None,
            )
            for lang in LANGUAGES
        },
        "startup_rss": {
            lang: summarize_or_na(
                startup_rss_samples[lang],
                "host_contention_unresolved" if startup_rss_na else None,
            )
            for lang in LANGUAGES
        },
        "summaries": summaries,
    })
    dump_json(out_dir / "result.json", result)
    print(json.dumps({"ok": True, "unit_id": unit_id, "result": str(out_dir / 'result.json')}, indent=2))
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Quidra mechanical micro benchmark runner")
    sub = p.add_subparsers(dest="command", required=True)
    audit_p = sub.add_parser("audit", help="re-audit the snapshot's Quidra benchmark programs")
    audit_p.add_argument("--workspace", required=True)
    audit_p.add_argument("--unit-id", required=True)
    measure_p = sub.add_parser("measure")
    measure_p.add_argument("--workspace", required=True)
    measure_p.add_argument("--unit-id", required=True)
    build_p = sub.add_parser(
        "build-target",
        help="build the evaluated Quidra compiler into the workspace before scored work needs it",
    )
    build_p.add_argument("--workspace", required=True)
    check_p = sub.add_parser(
        "build-check",
        help="compile every comparison-language micro program once, without measuring",
    )
    check_p.add_argument("--workspace", required=True)
    check_p.add_argument(
        "--language", action="append", default=None,
        help="restrict to one language (repeatable); default: every comparison language",
    )
    return p


def build_check(root: Path, languages: list[str] | None = None) -> int:
    """Compile every comparison-language micro program once, without measuring.

    The third paid run was the first to reach the mechanical measurement, and
    it found the Swift programs importing Darwin, which no Linux toolchain has;
    the whole Language Quality evaluation was blocked on a build nothing had
    tried before paying. This is that try, for the runtime image's CI: every
    program is copied into the workspace and built exactly as `measure` builds
    it, and any failure is reported with the compiler's message.
    """
    chosen = list(languages or [language for language in CONFIGS if language != "Quidra"])
    unknown = [language for language in chosen if language not in CONFIGS]
    if unknown:
        raise MeasureError(f"unknown language(s): {', '.join(unknown)}")
    report: dict[str, Any] = {"schema_version": 1, "ok": True, "built": [], "failed": []}
    # The build environment points TMPDIR at the workspace's own tmp directory.
    (root / "tmp").mkdir(exist_ok=True)
    for language in chosen:
        for workload in [STARTUP_WORKLOAD, *WORKLOADS]:
            try:
                cell = prepare_cell(root, language, workload, Path("quidra"))
                build_once(root, cell)
                report["built"].append(f"{language}/{workload}")
            except (MeasureError, OSError, subprocess.SubprocessError) as exc:
                report["ok"] = False
                report["failed"].append({
                    "language": language, "workload": workload, "error": str(exc)[:2000],
                })
    print(json.dumps(report, indent=2))
    return 0 if report["ok"] else 1


def build_target(root: Path) -> int:
    compiler = ensure_target_compiler(root)
    record = load_json(root / "results" / "target_toolchain.json")
    print(json.dumps({"ok": True, "compiler_path": str(compiler), **record}, indent=2))
    return 0


def main() -> int:
    args = build_parser().parse_args()
    try:
        root = root_from(args.workspace)
        if args.command == "audit":
            return audit(root, args.unit_id)
        if args.command == "measure":
            return measure(root, args.unit_id)
        if args.command == "build-target":
            return build_target(root)
        if args.command == "build-check":
            return build_check(root, args.language)
        raise MeasureError(f"unknown command: {args.command}")
    except (MeasureError, OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        print(f"micro measure error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
