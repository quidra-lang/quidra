#!/usr/bin/env python3
"""Reusable Quidra benchmark orchestration helpers.

This CLI handles deterministic benchmark bookkeeping. It intentionally does not
implement language-specific scoring logic from the benchmark methodology.
"""

from __future__ import annotations

import argparse
import collections
import contextlib
import datetime as dt
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
from typing import Any, Iterable


class BenchmarkError(RuntimeError):
    pass


CANONICAL_WORKSPACE = Path("/quidra-benchmark")
HOST_WORKSPACE_RELATIVE = Path(".quidra-benchmark")
HOST_SENTINEL_NAME = "quidra-benchmark-host.json"
HOST_SENTINEL_KIND = "quidra-benchmark-host-staging-v1"
TEMPLATE_DIR = Path(__file__).resolve().parent.parent
BENCHMARK_METADATA_RELATIVE = PurePosixPath("config/benchmark_metadata.json")


def load_benchmark_metadata(template: Path) -> dict[str, Any]:
    path = template / BENCHMARK_METADATA_RELATIVE
    try:
        metadata = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise BenchmarkError(f"benchmark metadata is missing: {path}") from None
    except json.JSONDecodeError as exc:
        raise BenchmarkError(f"benchmark metadata is not valid JSON: {path}: {exc}") from None
    if metadata.get("schema_version") != 1:
        raise BenchmarkError(f"unsupported benchmark metadata schema_version: {path}")
    evaluations = metadata.get("evaluations")
    languages = metadata.get("languages")
    if not isinstance(evaluations, list) or not evaluations:
        raise BenchmarkError(f"benchmark metadata has no evaluations: {path}")
    if not isinstance(languages, list) or not languages:
        raise BenchmarkError(f"benchmark metadata has no languages: {path}")
    for entry in evaluations:
        if (
            not isinstance(entry, dict)
            or not isinstance(entry.get("id"), str)
            or not isinstance(entry.get("display_name"), str)
        ):
            raise BenchmarkError(f"benchmark metadata evaluation entries need id/display_name: {path}")
    if not all(isinstance(language, str) for language in languages):
        raise BenchmarkError(f"benchmark metadata languages must be strings: {path}")
    return metadata


def metadata_languages(root: Path) -> list[str]:
    """Fixed evaluated-language set, in ranking/table order, from the frozen template."""
    return list(load_benchmark_metadata(root / "template")["languages"])


BENCHMARK_METADATA = load_benchmark_metadata(TEMPLATE_DIR)
PRIMARY_NAMES = tuple(entry["id"] for entry in BENCHMARK_METADATA["evaluations"])
PRIMARY_DISPLAY_NAMES = {
    entry["id"]: entry["display_name"] for entry in BENCHMARK_METADATA["evaluations"]
}
EVALUATION_SPEC_FILES = {name: f"{name}.md" for name in PRIMARY_NAMES}
TEXT_SUFFIXES = {
    ".txt", ".md", ".json", ".csv", ".tsv", ".yaml", ".yml", ".toml",
    ".py", ".sh", ".ps1", ".c", ".cc", ".cpp", ".h", ".hpp", ".rs",
    ".go", ".java", ".kt", ".swift", ".zig", ".ts", ".js", ".qui",
}
# Text files the privacy gate must read even though they carry no suffix.
TEXT_FILENAMES = {"Dockerfile", "Containerfile", "Makefile"}
TOOLCHAIN_COMMANDS = {
    "C++": [["c++", "--version"]],
    "Go": [["go", "version"]],
    "Java": [["javac", "-version"]],
    "Kotlin": [["kotlinc", "-version"]],
    "Python": [["python3", "--version"]],
    "Rust": [["rustc", "--version"]],
    "Swift": [["swift", "--version"]],
    "TypeScript": [["tsc", "--version"], ["node", "--version"]],
    "Zig": [["zig", "version"]],
}

#: Image-level toolchain locations that scored subprocesses keep. None of these
#: can carry a credential; all of them decide whether a compiler works at all.
#: rustc in the runtime image is a rustup proxy that resolves its toolchain
#: through RUSTUP_HOME and otherwise looks under $HOME/.rustup, which does not
#: exist in the sandbox home - so without RUSTUP_HOME every rustc call tried to
#: download a toolchain, failed offline, and the first paid run measured Rust
#: with a compiler that never ran once.
TOOLCHAIN_ENVIRONMENT_PASSTHROUGH = (
    "RUSTUP_HOME", "CARGO_HOME", "JAVA_HOME", "GOROOT", "GOTOOLCHAIN",
)

#: Programs whose successful `run` in an agent trace evidences that the assigned
#: language's toolchain actually compiled or executed something. A learnability
#: unit attests fixtures_compile_and_run=true; this is the mechanical check that
#: the attestation is backed by a real invocation rather than a string matcher.
#: How a Task Packet's components are ordered. `task-first` is the original
#: layout. `shared-inputs-first` renders the embedded inputs every sibling
#: unit shares before the per-unit header, and the trusted gateway places a
#: prompt-cache breakpoint where the header begins.
PACKET_LAYOUTS = ("task-first", "shared-inputs-first")

#: The line that opens every Task Packet's per-unit header. In the
#: shared-inputs-first layout it marks where the shared prefix ends.
PACKET_HEADER_MARKER = "# Task Packet: "

LANGUAGE_TOOLCHAIN_PROGRAMS = {
    "Python": ("python3",),
    "C++": ("c++", "g++", "clang++", "cc", "gcc", "clang"),
    "Rust": ("rustc", "cargo"),
    "Go": ("go",),
    "Java": ("javac", "java"),
    "Kotlin": ("kotlinc",),
    "TypeScript": ("tsc", "node"),
    "Swift": ("swiftc", "swift"),
    "Zig": ("zig",),
    "Quidra": ("quidra",),
}

SECRET_ENV_PARTS = (
    "TOKEN", "SECRET", "PASSWORD", "PASSWD", "API_KEY", "APIKEY",
    "PRIVATE_KEY", "SSH_AUTH_SOCK", "EMAIL",
)

GATEWAY_CONFIG_RELATIVE = PurePosixPath("config/inference_gateway.json")
GATEWAY_SOCKET_ENV = "QUIDRA_BENCHMARK_INFERENCE_SOCKET"
LAUNCHER_CONTRACT_ENV = "QUIDRA_BENCHMARK_LAUNCHER_CONTRACT"
LAUNCHER_CONTRACT_VERSION = "quidra-sandbox-launcher-v1"

# Mount points a hardened container legitimately carries. Anything else visible in
# /proc/self/mountinfo means the sandbox was handed something it did not ask for -
# typically a host home directory, an SSH agent socket or a credential file.
ALLOWED_MOUNT_POINTS = frozenset({
    "/",
    "/etc/hosts",
    "/etc/hostname",
    "/etc/resolv.conf",
    "/tmp",
    "/quidra-benchmark",
    "/quidra-benchmark/repo",
    "/quidra-benchmark/template",
    "/quidra-benchmark/cache",
    "/quidra-benchmark/gateway",
})
ALLOWED_MOUNT_PREFIXES = ("/proc", "/sys", "/dev")

RETAINED_RUN_PATHS = (
    "run.json",
    "results",
    "raw",
    "prompts",
    "work/agents",
    "work/attempts",
    "work/audit",
    "work/root/manifest.json",
    "work/root/ledger.json",
    "work/root/plans",
    "work/root/semantic-probes",
    "work/root/commands",
    "work/root/proficiency-verification",
    "work/root/comparability_blinding.json",
    "work/root/f20_runtime_facts.json",
)


def eprint(*args: Any) -> None:
    print(*args, file=sys.stderr)


def slug_id(value: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", value.lower()).strip("-")


def json_dump(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def json_load(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def sha256_tree(root: Path) -> str:
    h = hashlib.sha256()
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        rel = path.relative_to(root).as_posix()
        h.update(rel.encode("utf-8"))
        h.update(b"\\0")
        h.update(sha256_file(path).encode("ascii"))
        h.update(b"\\n")
    return h.hexdigest()


def lexical_absolute(path: Path) -> Path:
    """Return an absolute path without resolving filesystem symlinks."""
    return Path(os.path.abspath(os.fspath(path)))


def require_under(path: Path, root: Path) -> Path:
    p = lexical_absolute(path)
    r = lexical_absolute(root)
    try:
        p.relative_to(r)
    except ValueError as exc:
        raise BenchmarkError(f"path escapes workspace: {p}") from exc

    p_real = p.resolve()
    r_real = r.resolve()
    try:
        p_real.relative_to(r_real)
    except ValueError as exc:
        raise BenchmarkError(f"path escapes workspace through symlink: {p}") from exc
    return p


def workspace(args: argparse.Namespace) -> Path:
    return lexical_absolute(Path(getattr(args, "workspace", CANONICAL_WORKSPACE)))


def host_workspace(source: Path) -> Path:
    """Return the fixed trusted host staging directory for this checkout."""
    root = lexical_absolute(source / HOST_WORKSPACE_RELATIVE)
    if root.is_symlink():
        raise BenchmarkError("host benchmark workspace may not be a symlink")
    return root


def host_sentinel_path(source: Path) -> Path:
    """Return the Git-private host guard path, outside the sandbox staging tree."""
    raw = Path(
        run_capture(["git", "rev-parse", "--git-path", HOST_SENTINEL_NAME], source)
    )
    path = lexical_absolute(raw if raw.is_absolute() else source / raw)
    root = lexical_absolute(source / HOST_WORKSPACE_RELATIVE)
    try:
        path.relative_to(root)
    except ValueError:
        pass
    else:
        raise BenchmarkError("host benchmark sentinel must be outside ./.quidra-benchmark")
    return path


def ensure_host_workspace_ignored(source: Path) -> None:
    tracked = run_capture(["git", "ls-files", "--", HOST_WORKSPACE_RELATIVE.as_posix()], source)
    if tracked.strip():
        raise BenchmarkError("host benchmark workspace path must not be Git-tracked")

    # cmd_init creates the empty staging directory before this check. Probe the
    # directory form explicitly so a directory-only rule such as
    # '/.quidra-benchmark/' is evaluated exactly as Git will treat the workspace.
    ignore_probe = HOST_WORKSPACE_RELATIVE.as_posix() + "/"
    ignored = subprocess.run(
        ["git", "check-ignore", "--quiet", "--no-index", "--", ignore_probe],
        cwd=source,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if ignored.returncode != 0:
        raise BenchmarkError(
            "host benchmark workspace must be ignored by Git; add '/.quidra-benchmark/'"
        )


def host_workspace_sentinel(
    run_id: str,
    evaluated_commit_sha: str,
) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "kind": HOST_SENTINEL_KIND,
        "run_id": run_id,
        "evaluated_commit_sha": evaluated_commit_sha,
        "canonical_workspace_root": CANONICAL_WORKSPACE.as_posix(),
    }


def write_host_workspace_sentinel(
    source: Path,
    run_id: str,
    evaluated_commit_sha: str,
) -> None:
    path = host_sentinel_path(source)
    if path.is_symlink() or path.exists():
        raise BenchmarkError(
            "host benchmark sentinel already exists; use discard-workspace before starting a new run"
        )
    marker = host_workspace_sentinel(run_id, evaluated_commit_sha)
    json_dump(path, marker)
    os.chmod(path, 0o600)


def validate_host_workspace_path(root: Path, source: Path) -> None:
    expected_root = lexical_absolute(source / HOST_WORKSPACE_RELATIVE)
    if root != expected_root or root.name != HOST_WORKSPACE_RELATIVE.name:
        raise BenchmarkError("refusing host workspace operation outside ./.quidra-benchmark")
    if root.is_symlink():
        raise BenchmarkError("refusing host workspace operation through a symlink")


def validate_host_workspace_guard(source: Path) -> dict[str, Any]:
    marker_path = host_sentinel_path(source)
    if marker_path.is_symlink() or not marker_path.is_file():
        raise BenchmarkError("host benchmark sentinel is missing or invalid")
    try:
        marker = json_load(marker_path)
    except (OSError, json.JSONDecodeError) as exc:
        raise BenchmarkError("host benchmark sentinel is unreadable or invalid") from exc
    expected_static = {
        "schema_version": 1,
        "kind": HOST_SENTINEL_KIND,
        "canonical_workspace_root": CANONICAL_WORKSPACE.as_posix(),
    }
    for key, value in expected_static.items():
        if marker.get(key) != value:
            raise BenchmarkError("host benchmark sentinel does not match the guard contract")
    if not isinstance(marker.get("run_id"), str) or not marker["run_id"]:
        raise BenchmarkError("host benchmark sentinel has no valid run_id")
    if (
        not isinstance(marker.get("evaluated_commit_sha"), str)
        or not marker["evaluated_commit_sha"]
    ):
        raise BenchmarkError("host benchmark sentinel has no valid evaluated commit")
    return marker


def validate_host_workspace_sentinel(
    root: Path,
    source: Path,
    run: dict[str, Any],
) -> None:
    validate_host_workspace_path(root, source)
    marker = validate_host_workspace_guard(source)
    if marker.get("run_id") != run.get("run_id"):
        raise BenchmarkError("host benchmark sentinel run_id does not match this run")
    if marker.get("evaluated_commit_sha") != run.get("evaluated", {}).get("commit_sha"):
        raise BenchmarkError("host benchmark sentinel commit does not match this run")
    if run.get("workspace_root") != CANONICAL_WORKSPACE.as_posix():
        raise BenchmarkError("run.json does not name the canonical /quidra-benchmark root")


def cmd_restore_workspace_guard(args: argparse.Namespace) -> int:
    """Re-create the Git-private guard after a trusted CI workspace handoff.

    GitHub-hosted jobs do not share .git, so the host sentinel created by init
    cannot travel with the scored workspace. The workspace itself is handed
    off as an artifact; this command proves that its run_id/evaluated commit
    belong to the exact clean checkout before recreating the guard. It never
    changes scored workspace contents.
    """
    source = Path(args.source_repo).resolve()
    if not source.is_dir():
        raise BenchmarkError(f"source repository does not exist: {source}")
    root = host_workspace(source)
    validate_host_workspace_path(root, source)
    ensure_host_workspace_ignored(source)
    run_path = root / "run.json"
    if not run_path.is_file():
        raise BenchmarkError("restored benchmark workspace is missing run.json")
    run = json_load(run_path)
    evaluated = str((run.get("evaluated") or {}).get("commit_sha") or "")
    run_id = str(run.get("run_id") or "")
    if not evaluated or not run_id:
        raise BenchmarkError("restored benchmark workspace has incomplete identity")
    expected = str(getattr(args, "expected_commit", "") or "").strip()
    if expected and evaluated != expected:
        raise BenchmarkError(
            f"restored workspace evaluates {evaluated}, expected {expected}"
        )
    meta = git_metadata(source)
    if meta["working_tree_status"] != "clean":
        raise BenchmarkError("workspace guard restore requires a clean source checkout")
    if meta["commit_sha"] != evaluated:
        raise BenchmarkError(
            "workspace guard restore requires checkout HEAD to equal the evaluated commit"
        )
    marker_path = host_sentinel_path(source)
    if marker_path.exists():
        marker = validate_host_workspace_guard(source)
        if (
            marker.get("run_id") == run_id
            and marker.get("evaluated_commit_sha") == evaluated
        ):
            print(json.dumps({
                "ok": True, "restored": False, "run_id": run_id,
                "evaluated_commit_sha": evaluated,
            }, indent=2))
            return 0
        raise BenchmarkError("a different host benchmark sentinel already exists")
    write_host_workspace_sentinel(source, run_id, evaluated)
    print(json.dumps({
        "ok": True, "restored": True, "run_id": run_id,
        "evaluated_commit_sha": evaluated,
    }, indent=2))
    return 0


def cmd_refresh_cache_snapshot(args: argparse.Namespace) -> int:
    """Accept a newer certified-cache snapshot before any recovered work starts.

    CI recovery may have to rebuild a workspace when an Actions handoff artifact
    is missing. The remote benchmark branch can already contain certified
    records checkpointed by the previous job. The trusted host copies only
    benchmark/cache into the newly initialized workspace, then this command
    updates run.json so ordinary integrity checks bind the run to that exact
    recovered snapshot. Once a manifest/ledger exists, changing the cache
    snapshot is forbidden.
    """
    source = Path(args.source_repo).resolve()
    if not source.is_dir():
        raise BenchmarkError(f"source repository does not exist: {source}")
    root = host_workspace(source)
    validate_host_workspace_path(root, source)
    run_path = root / "run.json"
    if not run_path.is_file():
        raise BenchmarkError("cache snapshot refresh requires an initialized workspace")
    run = json_load(run_path)
    validate_host_workspace_sentinel(root, source, run)
    expected = str(getattr(args, "expected_commit", "") or "").strip()
    evaluated = str((run.get("evaluated") or {}).get("commit_sha") or "")
    if expected and evaluated != expected:
        raise BenchmarkError(
            f"cache snapshot refresh evaluates {evaluated}, expected {expected}"
        )
    if (root / "work" / "root" / "manifest.json").exists() or (
        root / "work" / "root" / "ledger.json"
    ).exists():
        raise BenchmarkError(
            "certified cache snapshot may be refreshed only before manifest freeze"
        )
    cache = root / "cache"
    if not cache.is_dir():
        raise BenchmarkError("recovered workspace has no certified cache directory")
    previous = str(run.get("cache_tree_sha256") or "")
    current = sha256_tree(cache)
    run["cache_tree_sha256"] = current
    json_dump(run_path, run)
    print(json.dumps({
        "ok": True,
        "updated": previous != current,
        "previous_cache_tree_sha256": previous,
        "cache_tree_sha256": current,
        "evaluated_commit_sha": evaluated,
    }, indent=2))
    return 0


def delete_host_workspace(root: Path, source: Path) -> bool:
    validate_host_workspace_path(root, source)
    marker_path = host_sentinel_path(source)
    validate_host_workspace_guard(source)
    workspace_existed = root.exists()
    if workspace_existed:
        if not root.is_dir():
            raise BenchmarkError("host benchmark workspace is not a directory")
        shutil.rmtree(root)
    try:
        marker_path.unlink()
    except OSError as exc:
        raise BenchmarkError(
            f"workspace deletion succeeded but host sentinel cleanup failed: {exc}"
        ) from exc
    return workspace_existed


def run_capture(cmd: list[str], cwd: Path) -> str:
    p = subprocess.run(
        cmd,
        cwd=cwd,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return p.stdout.strip()


def git_metadata(source: Path) -> dict[str, str]:
    branch = run_capture(["git", "rev-parse", "--abbrev-ref", "HEAD"], source)
    if branch not in {"develop", "benchmark"}:
        raise BenchmarkError(
            f"benchmark target must be develop or the isolated benchmark branch, got {branch!r}"
        )
    sha = run_capture(["git", "rev-parse", "HEAD"], source)
    short_sha = run_capture(["git", "rev-parse", "--short", "HEAD"], source)
    subject = run_capture(["git", "show", "-s", "--format=%s", "HEAD"], source)
    commit_date = run_capture(["git", "show", "-s", "--format=%cI", "HEAD"], source)
    status = run_capture(["git", "status", "--porcelain=v1"], source)
    return {
        "branch": branch,
        "commit_sha": sha,
        "short_sha": short_sha,
        "commit_subject": subject,
        "commit_date": commit_date,
        "working_tree_status": "clean" if not status else "dirty",
    }


def manifest_version(source: Path) -> str | None:
    p = source / "quidra.manifest.json"
    if not p.exists():
        return None
    try:
        return str(json_load(p).get("compiler_version"))
    except Exception:
        return None


def validate_source_symlinks(source: Path) -> None:
    source_real = source.resolve()
    for path in source.rglob("*"):
        if not path.is_symlink():
            continue
        target_text = os.readlink(path)
        target = Path(target_text)
        resolved = (path.parent / target).resolve() if not target.is_absolute() else target.resolve()
        try:
            resolved.relative_to(source_real)
        except ValueError as exc:
            raise BenchmarkError(
                f"source checkout contains a symlink escaping the repository: {path}"
            ) from exc


def repo_ignore(directory: str, names: list[str]) -> set[str]:
    ignored: set[str] = set()
    base = Path(directory).name
    for name in names:
        if name in {".git", "build", ".cache", "__pycache__", ".pytest_cache"}:
            ignored.add(name)
        if base == "benchmark" and (
            name == "runs"
            or name == "template"
            or re.fullmatch(r"\d{4}-\d{2}-\d{2}-[0-9a-f]+(?:\(latest\))?", name)
        ):
            ignored.add(name)
    return ignored



def _safe_archive_parts(name: str) -> tuple[str, ...]:
    path = PurePosixPath(name)
    if path.is_absolute():
        raise BenchmarkError(f"git archive contains an absolute path: {name}")
    parts: list[str] = []
    for part in path.parts:
        if part in {"", "."}:
            continue
        if part == "..":
            raise BenchmarkError(f"git archive path escapes the repository: {name}")
        parts.append(part)
    if not parts:
        raise BenchmarkError(f"git archive contains an empty path: {name!r}")
    return tuple(parts)


def _safe_symlink_target(parent_parts: tuple[str, ...], target_text: str) -> tuple[str, ...]:
    target = PurePosixPath(target_text)
    if target.is_absolute():
        raise BenchmarkError(f"tracked symlink has an absolute target: {target_text}")
    parts = list(parent_parts)
    for part in target.parts:
        if part in {"", "."}:
            continue
        if part == "..":
            if not parts:
                raise BenchmarkError(f"tracked symlink escapes the repository: {target_text}")
            parts.pop()
        else:
            parts.append(part)
    if not parts:
        raise BenchmarkError(f"tracked symlink resolves to repository root: {target_text}")
    return tuple(parts)


def copy_tracked_snapshot(
    source: Path,
    dest: Path,
    *,
    excluded_top_level: set[str] | None = None,
) -> dict[str, Any]:
    """Materialize only files tracked by HEAD.

    Local ignored/untracked files are deliberately excluded so benchmark agents
    cannot see host build caches, .env files, editor state or other private data.
    """
    excluded = set(excluded_top_level or set())
    if dest.exists():
        raise BenchmarkError(f"snapshot destination already exists: {dest}")
    dest.mkdir(parents=True)

    listing = subprocess.run(
        ["git", "ls-tree", "-z", "--name-only", "HEAD"],
        cwd=source,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    top_level = [
        raw.decode("utf-8", "surrogateescape")
        for raw in listing.stdout.split(b"\0")
        if raw
    ]
    selected = [name for name in top_level if name not in excluded]
    if not selected:
        raise BenchmarkError("evaluated commit contains no selected tracked paths")

    proc = subprocess.Popen(
        ["git", "archive", "--format=tar", "HEAD", "--", *selected],
        cwd=source,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if proc.stdout is None or proc.stderr is None:
        proc.kill()
        raise BenchmarkError("failed to open git archive pipes")

    files = 0
    directories = 0
    symlinks = 0
    try:
        with tarfile.open(fileobj=proc.stdout, mode="r|") as archive:
            for member in archive:
                parts = _safe_archive_parts(member.name)
                if parts[0] in excluded:
                    raise BenchmarkError(
                        f"excluded top-level path unexpectedly entered snapshot: {member.name}"
                    )
                output = dest.joinpath(*parts)
                require_under(output.parent, dest)
                if member.isdir():
                    output.mkdir(parents=True, exist_ok=True)
                    directories += 1
                elif member.isfile():
                    output.parent.mkdir(parents=True, exist_ok=True)
                    src = archive.extractfile(member)
                    if src is None:
                        raise BenchmarkError(f"could not read archived file: {member.name}")
                    with src, output.open("wb") as dst:
                        shutil.copyfileobj(src, dst)
                    os.chmod(output, member.mode & 0o777)
                    files += 1
                elif member.issym():
                    target_parts = _safe_symlink_target(parts[:-1], member.linkname)
                    if target_parts[0] in excluded:
                        raise BenchmarkError(
                            f"tracked symlink points into excluded snapshot content: {member.name}"
                        )
                    output.parent.mkdir(parents=True, exist_ok=True)
                    os.symlink(member.linkname, output)
                    symlinks += 1
                else:
                    raise BenchmarkError(
                        f"unsupported tracked archive member type: {member.name}"
                    )
    except Exception:
        proc.kill()
        proc.wait()
        raise

    stderr = proc.stderr.read().decode("utf-8", "replace").strip()
    returncode = proc.wait()
    if returncode != 0:
        raise BenchmarkError(f"git archive failed with exit {returncode}: {stderr}")

    return {
        "mode": "git-archive-HEAD-tracked-only",
        "files": files,
        "directories": directories,
        "symlinks": symlinks,
        "excluded_top_level": sorted(excluded),
    }



def copy_tracked_tree(source: Path, repo_relative_path: str, dest: Path) -> dict[str, Any]:
    prefix = _safe_archive_parts(repo_relative_path)
    if dest.exists():
        raise BenchmarkError(f"tracked tree destination already exists: {dest}")
    dest.mkdir(parents=True)

    proc = subprocess.Popen(
        ["git", "archive", "--format=tar", "HEAD", "--", repo_relative_path],
        cwd=source,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if proc.stdout is None or proc.stderr is None:
        proc.kill()
        raise BenchmarkError("failed to open git archive pipes")

    files = 0
    directories = 0
    symlinks = 0
    try:
        with tarfile.open(fileobj=proc.stdout, mode="r|") as archive:
            for member in archive:
                full_parts = _safe_archive_parts(member.name)
                if len(full_parts) < len(prefix):
                    if member.isdir() and prefix[:len(full_parts)] == full_parts:
                        # git archive emits tracked ancestor directory entries
                        # (for example "benchmark/" before "benchmark/template/").
                        continue
                    raise BenchmarkError(
                        f"git archive member escaped requested tree {repo_relative_path}: "
                        f"{member.name}"
                    )
                if full_parts[:len(prefix)] != prefix:
                    raise BenchmarkError(
                        f"git archive member escaped requested tree {repo_relative_path}: "
                        f"{member.name}"
                    )
                rel_parts = full_parts[len(prefix):]
                if not rel_parts:
                    continue
                output = dest.joinpath(*rel_parts)
                require_under(output.parent, dest)

                if member.isdir():
                    output.mkdir(parents=True, exist_ok=True)
                    directories += 1
                elif member.isfile():
                    output.parent.mkdir(parents=True, exist_ok=True)
                    src = archive.extractfile(member)
                    if src is None:
                        raise BenchmarkError(f"could not read archived file: {member.name}")
                    with src, output.open("wb") as dst:
                        shutil.copyfileobj(src, dst)
                    os.chmod(output, member.mode & 0o777)
                    files += 1
                elif member.issym():
                    target_full = _safe_symlink_target(full_parts[:-1], member.linkname)
                    if target_full[:len(prefix)] != prefix:
                        raise BenchmarkError(
                            f"tracked symlink escapes reusable tree {repo_relative_path}: "
                            f"{member.name}"
                        )
                    output.parent.mkdir(parents=True, exist_ok=True)
                    os.symlink(member.linkname, output)
                    symlinks += 1
                else:
                    raise BenchmarkError(
                        f"unsupported tracked archive member type: {member.name}"
                    )
    except Exception:
        proc.kill()
        proc.wait()
        raise

    stderr = proc.stderr.read().decode("utf-8", "replace").strip()
    returncode = proc.wait()
    if returncode != 0:
        raise BenchmarkError(
            f"git archive failed for {repo_relative_path} with exit {returncode}: {stderr}"
        )
    return {
        "mode": "git-archive-HEAD-tracked-tree",
        "repo_relative_path": repo_relative_path,
        "files": files,
        "directories": directories,
        "symlinks": symlinks,
    }


def copy_tracked_blob(source: Path, repo_relative_path: str, dest: Path) -> dict[str, Any]:
    rel = PurePosixPath(repo_relative_path)
    if rel.is_absolute() or ".." in rel.parts:
        raise BenchmarkError(f"unsafe tracked blob path: {repo_relative_path!r}")
    if dest.exists():
        raise BenchmarkError(f"tracked blob destination already exists: {dest}")
    tree_row = run_capture(
        ["git", "ls-tree", "HEAD", "--", repo_relative_path],
        source,
    )
    if not tree_row:
        raise BenchmarkError(f"tracked blob is missing: {repo_relative_path}")
    mode = tree_row.split(None, 1)[0]
    if not mode.startswith("100"):
        raise BenchmarkError(f"tracked path is not a regular blob: {repo_relative_path}")
    payload = subprocess.run(
        ["git", "show", f"HEAD:{repo_relative_path}"],
        cwd=source,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    ).stdout
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(payload)
    os.chmod(dest, int(mode, 8) & 0o777)
    return {
        "mode": "git-show-HEAD-tracked-blob",
        "repo_relative_path": repo_relative_path,
        "bytes": len(payload),
    }


def validate_reuse_catalog(catalog: dict[str, Any]) -> None:
    if catalog.get("schema_version") != 3:
        raise BenchmarkError("reuse catalog schema_version must be 3")
    artifacts = catalog.get("artifacts")
    if not isinstance(artifacts, list):
        raise BenchmarkError("reuse catalog artifacts must be an array")

    ids: set[str] = set()
    destinations: set[str] = set()
    dependencies: dict[str, list[str]] = {}
    for item in artifacts:
        if not isinstance(item, dict):
            raise BenchmarkError("reuse catalog artifact must be an object")
        artifact_id = str(item.get("id") or "").strip()
        if not re.fullmatch(r"[A-Za-z0-9._-]+", artifact_id):
            raise BenchmarkError(f"invalid reusable artifact id: {artifact_id!r}")
        if artifact_id in ids:
            raise BenchmarkError(f"duplicate reusable artifact id: {artifact_id}")
        ids.add(artifact_id)

        kind = item.get("kind")
        if kind not in {"tree", "blob"}:
            raise BenchmarkError(f"{artifact_id}: kind must be tree or blob")

        destination = str(item.get("destination") or "").replace("\\", "/")
        destination_path = PurePosixPath(destination)
        if not destination or destination_path.is_absolute() or ".." in destination_path.parts:
            raise BenchmarkError(f"{artifact_id}: unsafe destination: {destination!r}")
        if destination in destinations:
            raise BenchmarkError(f"duplicate reusable artifact destination: {destination}")
        destinations.add(destination)

        deps = item.get("reuse_dependencies", [])
        if not isinstance(deps, list) or not all(isinstance(x, str) for x in deps):
            raise BenchmarkError(f"{artifact_id}: reuse_dependencies must be a string array")
        if artifact_id in deps:
            raise BenchmarkError(f"{artifact_id}: reusable artifact cannot depend on itself")
        dependencies[artifact_id] = deps

        if item.get("language") == "Quidra" and destination.startswith("programs/"):
            raise BenchmarkError(
                f"{artifact_id}: Quidra program sources may not be reused across target commits"
            )

    for artifact_id, deps in dependencies.items():
        unknown = sorted(set(deps) - ids)
        if unknown:
            raise BenchmarkError(
                f"{artifact_id}: unknown reusable artifact dependencies: {', '.join(unknown)}"
            )

    visiting: set[str] = set()
    visited: set[str] = set()
    def visit(artifact_id: str) -> None:
        if artifact_id in visited:
            return
        if artifact_id in visiting:
            raise BenchmarkError(f"reuse dependency cycle includes {artifact_id}")
        visiting.add(artifact_id)
        for dep in dependencies.get(artifact_id, []):
            visit(dep)
        visiting.remove(artifact_id)
        visited.add(artifact_id)
    for artifact_id in ids:
        visit(artifact_id)


def materialize_reuse_catalog(source: Path, template_dest: Path) -> list[dict[str, Any]]:
    """Index and verify reusable assets already tracked inside the current template."""
    catalog_path = template_dest / "reuse" / "catalog.json"
    if not catalog_path.exists():
        return []
    catalog = json_load(catalog_path)
    validate_reuse_catalog(catalog)
    records: list[dict[str, Any]] = []
    for item in catalog.get("artifacts", []):
        dest = require_under(template_dest / str(item["destination"]), template_dest)
        if item["kind"] == "tree" and not dest.is_dir():
            raise BenchmarkError(
                f"self-contained reusable tree missing from template: {item['id']} -> {dest}"
            )
        if item["kind"] == "blob" and not dest.is_file():
            raise BenchmarkError(
                f"self-contained reusable file missing from template: {item['id']} -> {dest}"
            )
        repo_rel = Path("benchmark") / "template" / str(item["destination"])
        observed = run_capture(["git", "rev-parse", f"HEAD:{repo_rel.as_posix()}"], source)
        records.append({
            "id": item["id"],
            "destination": item["destination"],
            "content_destination": item["destination"],
            "content_git_object_sha1": observed,
            "language": item.get("language"),
            "workload": item.get("workload"),
            "validated_toolchain": item.get("validated_toolchain"),
            "reuse_dependencies": item.get("reuse_dependencies", []),
        })
    json_dump(
        template_dest / "reuse" / "materialized.json",
        {"schema_version": 3, "artifacts": records},
    )
    return records


def cmd_init(args: argparse.Namespace) -> int:
    source = Path(args.source_repo).resolve()
    meta = git_metadata(source)
    if meta["working_tree_status"] != "clean":
        raise BenchmarkError(
            "benchmark target must have a clean working tree so its recorded commit SHA "
            "fully identifies the evaluated snapshot"
        )
    root = host_workspace(source)
    if root.exists() and any(root.iterdir()):
        raise BenchmarkError(f"workspace must be absent or empty: {root}")
    root.mkdir(parents=True, exist_ok=True)
    try:
        # Directory-only ignore rules cannot be validated reliably before the
        # directory exists. Verify while it is still empty, before staging data.
        ensure_host_workspace_ignored(source)
    except Exception:
        try:
            root.rmdir()
        except OSError:
            pass
        raise
    run_id = args.run_id or f"{dt.date.today().isoformat()}-{meta['short_sha']}"
    write_host_workspace_sentinel(source, run_id, meta["commit_sha"])

    template_src = source / "benchmark" / "template"
    master_src = source / "benchmark" / "master_prompt.md"
    if not template_src.is_dir() or not master_src.is_file():
        raise BenchmarkError("benchmark/template or benchmark/master_prompt.md is missing")

    for d in (
        "work/root", "work/agents", "work/attempts", "raw", "results", "prompts/by-hash",
        "prompts/components/by-hash", "prompts/manifests", "home", "tmp",
        # Mountpoint only. The launcher mounts the trusted gateway's socket volume
        # here, so nothing on the host side ever lands in this directory.
        "gateway",
    ):
        (root / d).mkdir(parents=True, exist_ok=True)

    snapshot = copy_tracked_snapshot(
        source,
        root / "repo",
        excluded_top_level={"benchmark"},
    )
    # Benchmark infrastructure is staged physically under .quidra-benchmark/template,
    # then exposed to scored processes only as /quidra-benchmark/template by the
    # external isolation boundary. Materialize from Git, never ignored/untracked input.
    template_snapshot = copy_tracked_tree(
        source,
        "benchmark/template",
        root / "template",
    )
    cache_snapshot = copy_tracked_tree(
        source,
        "benchmark/cache",
        root / "cache",
    )
    reused = materialize_reuse_catalog(source, root / "template")

    master_dest = root / "prompts" / "by-hash" / "master-prompt.tmp.md"
    copy_tracked_blob(source, "benchmark/master_prompt.md", master_dest)
    master_hash = sha256_file(master_dest)
    hashed_master_dest = root / "prompts" / "by-hash" / f"{master_hash}.md"
    os.replace(master_dest, hashed_master_dest)
    master_dest = hashed_master_dest
    config_path = root / "template" / "config" / "primary.json"
    config_hash = sha256_file(config_path)
    catalog_path = root / "template" / "reuse" / "catalog.json"
    materialized_path = root / "template" / "reuse" / "materialized.json"
    catalog_hash = sha256_file(catalog_path) if catalog_path.exists() else None
    materialized_hash = sha256_file(materialized_path) if materialized_path.exists() else None
    template_hash = sha256_tree(root / "template")
    cache_cfg_for_identity = json_load(
        root / "template" / "config" / "cache_policy.json"
    )
    target_execution_identity = quidra_execution_identity_from_git(
        source,
        meta["commit_sha"],
        quidra_execution_input_paths(cache_cfg_for_identity),
    )

    run = {
        "schema_version": 1,
        "run_id": run_id,
        "evaluated": {
            **meta,
            "compiler_version": manifest_version(root / "repo"),
            "quidra_execution_identity": target_execution_identity,
        },
        "workspace_root": str(CANONICAL_WORKSPACE),
        "sandbox_mode": args.sandbox_mode,
        "master_prompt_sha256": master_hash,
        "primary_config_sha256": config_hash,
        "reuse_catalog_sha256": catalog_hash,
        "reuse_materialization_sha256": materialized_hash,
        "template_tree_sha256": template_hash,
        "cache_tree_sha256": sha256_tree(root / "cache"),
        "inference_identity": {
            "provider": getattr(args, "provider", None),
            "model": getattr(args, "model", None),
        },
        "materialized_reusable_artifacts": len(reused),
        "source_snapshot": snapshot,
        "template_source_snapshot": template_snapshot,
        "cache_source_snapshot": cache_snapshot,
        "created_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
    }
    json_dump(root / "run.json", run)
    print(json.dumps(run, indent=2))
    return 0


def template_integrity_problems(root: Path, run: dict[str, Any]) -> list[str]:
    problems: list[str] = []
    if run.get("workspace_root") != CANONICAL_WORKSPACE.as_posix():
        problems.append("run_workspace_root_must_be_/quidra-benchmark")

    master_hash = str(run.get("master_prompt_sha256") or "")
    master_path = root / "prompts" / "by-hash" / f"{master_hash}.md"
    if not master_hash:
        problems.append("master_prompt_hash_missing_from_run")
    elif not master_path.is_file():
        problems.append("master_prompt_missing")
    elif sha256_file(master_path) != master_hash:
        problems.append("master_prompt_hash_mismatch")

    template = root / "template"
    expected = run.get("template_tree_sha256")
    if not expected:
        problems.append("template_tree_hash_missing_from_run")
    elif template.exists() and sha256_tree(template) != expected:
        problems.append("template_tree_hash_mismatch")

    primary = template / "config" / "primary.json"
    if primary.exists() and run.get("primary_config_sha256"):
        if sha256_file(primary) != run["primary_config_sha256"]:
            problems.append("primary_config_hash_mismatch")

    catalog = template / "reuse" / "catalog.json"
    if catalog.exists() and run.get("reuse_catalog_sha256"):
        if sha256_file(catalog) != run["reuse_catalog_sha256"]:
            problems.append("reuse_catalog_hash_mismatch")
    materialized = template / "reuse" / "materialized.json"
    if materialized.exists() and run.get("reuse_materialization_sha256"):
        if sha256_file(materialized) != run["reuse_materialization_sha256"]:
            problems.append("reuse_materialization_hash_mismatch")
    cache = root / "cache"
    if run.get("cache_tree_sha256") and cache.exists():
        if sha256_tree(cache) != run["cache_tree_sha256"]:
            problems.append("certified_cache_hash_mismatch")
    return problems


def assert_template_integrity(root: Path) -> None:
    run_path = root / "run.json"
    if not run_path.is_file():
        raise BenchmarkError("run.json is required before benchmark planning")
    problems = template_integrity_problems(root, json_load(run_path))
    if problems:
        raise BenchmarkError("template integrity failed: " + ", ".join(problems))


def command_version_output(
    cmd: list[str], cwd: Path, env: dict[str, str] | None = None
) -> str:
    p = subprocess.run(
        cmd,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    text = (p.stdout + "\n" + p.stderr).strip()
    if p.returncode != 0:
        raise BenchmarkError(f"toolchain command failed ({' '.join(cmd)}): {text}")
    return text


def canonical_toolchain_fingerprint(language: str, outputs: list[str]) -> str:
    combined = "\n".join(outputs)
    versions = re.findall(r"(?<!\d)(\d+\.\d+(?:\.\d+)?)(?!\d)", combined)
    if language == "Swift":
        match = re.search(
            r"(?:Apple\s+)?Swift\s+version[:\s]+(\d+\.\d+(?:\.\d+)?)",
            combined,
            re.IGNORECASE,
        )
        if match:
            return match.group(1)
    if language == "TypeScript":
        if len(versions) < 2:
            raise BenchmarkError("could not identify both TypeScript and Node versions")
        return f"tsc {versions[0]}; node {versions[1]}"
    if not versions:
        stripped = combined.strip().splitlines()
        return stripped[0] if stripped else ""
    return versions[0]


def cmd_toolchain_scan(args: argparse.Namespace) -> int:
    root = workspace(args)
    results: dict[str, Any] = {}
    missing: list[str] = []
    # Scan with exactly the environment scored subprocesses get. A toolchain
    # that answers here but not there is the worst kind of missing: it passes
    # bootstrap and then fails every measurement that pays to reach it.
    env = sanitized_subprocess_env(root, root / "repo") if (root / "repo").is_dir() else None
    for language, commands in TOOLCHAIN_COMMANDS.items():
        outputs: list[str] = []
        try:
            for cmd in commands:
                outputs.append(command_version_output(cmd, root / "repo", env))
            results[language] = {
                "canonical": canonical_toolchain_fingerprint(language, outputs),
                "raw": outputs,
                "commands": commands,
            }
        except (BenchmarkError, FileNotFoundError) as exc:
            results[language] = {"error": str(exc), "commands": commands}
            missing.append(language)
    runtime_image_record: dict[str, Any] = {}
    runtime_record_path = Path("/opt/quidra-benchmark/toolchains-observed.json")
    if runtime_record_path.is_file():
        try:
            runtime_image_record = json_load(runtime_record_path)
        except (OSError, json.JSONDecodeError) as exc:
            raise BenchmarkError(
                "runtime image toolchain observation record is unreadable: "
                f"{exc}"
            ) from exc
    semantic_ffi_smoke = runtime_image_record.get("semantic_ffi_smoke", {})
    if (
        lexical_absolute(root) == lexical_absolute(CANONICAL_WORKSPACE)
        and runtime_record_path.is_file()
    ):
        # The lightweight CI base image intentionally has no comparison
        # toolchains and therefore no image observation record. Production
        # uses the full toolchains image, whose build-time verifier must write
        # this record and whose F20 smoke is mandatory.
        stable_f20 = normalize_f20_runtime_smoke(semantic_ffi_smoke)
        json_dump(root / F20_RUNTIME_FACTS_RELATIVE, stable_f20)
    payload = {
        "schema_version": 1,
        "checked_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "toolchains": results,
        "semantic_ffi_smoke": semantic_ffi_smoke,
        "runtime_image": runtime_image_record.get("image"),
        "missing": missing,
        "ok": not missing,
    }
    out = root / "results" / "toolchains.json"
    json_dump(out, payload)
    print(json.dumps(payload, indent=2))
    return 0 if (not args.strict or not missing) else 2


def cmd_target_toolchain(args: argparse.Namespace) -> int:
    """Build the evaluated Quidra compiler before any scored work can need it.

    Sandbox agents for Quidra units run `quidra` from the build directory that
    `sanitized_subprocess_env` puts on their PATH. In the first paid run that
    build existed only once the Language Quality audit happened to run, and the
    Quidra learnability agents that ran earlier were told the compiler was not
    installed. Building it during prepare makes the order irrelevant.
    """
    root = workspace(args)
    if not (root / "repo" / "CMakeLists.txt").is_file():
        payload = {"ok": True, "skipped": "the evaluated snapshot has no CMake build"}
        print(json.dumps(payload, indent=2))
        return 0
    script = root / "template" / "scripts" / "micro_measure.py"
    p = subprocess.run(
        [sys.executable, str(script), "build-target", "--workspace", str(root)],
        cwd=root,
        env=sanitized_subprocess_env(root, root / "work" / "root"),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if p.returncode != 0:
        raise BenchmarkError(
            "the evaluated Quidra compiler could not be built from the snapshot: "
            f"{(p.stderr or p.stdout).strip()[:2000]}"
        )
    print(p.stdout)
    return 0


def expected_toolchain_canonical(language: str, value: str | None) -> str | None:
    if not value:
        return None
    versions = re.findall(r"(?<!\d)(\d+\.\d+(?:\.\d+)?)(?!\d)", value)
    if language == "Swift":
        match = re.search(
            r"(?:Apple\s+)?Swift\s+version[:\s]+(\d+\.\d+(?:\.\d+)?)",
            value,
            re.IGNORECASE,
        )
        if match:
            return match.group(1)
    if language == "TypeScript":
        if len(versions) >= 2:
            return f"tsc {versions[0]}; node {versions[1]}"
        return None
    return versions[0] if versions else value.strip()


def cmd_reuse_status(args: argparse.Namespace) -> int:
    root = workspace(args)
    materialized = root / "template" / "reuse" / "materialized.json"
    toolchains_path = root / "results" / "toolchains.json"
    if not materialized.is_file():
        raise BenchmarkError("reuse materialization record is missing")
    if not toolchains_path.is_file():
        raise BenchmarkError("toolchains.json is missing; run toolchain-scan first")
    mat = json_load(materialized)
    scanned = json_load(toolchains_path).get("toolchains", {})
    rows = []
    audit_required = []
    for artifact in mat.get("artifacts", []):
        language = artifact.get("language")
        validated = artifact.get("validated_toolchain")
        if not language or not validated:
            rows.append({
                "id": artifact["id"],
                "language": language,
                "status": "REUSABLE_SHARED_ASSET",
            })
            continue
        current = scanned.get(language, {})
        if current.get("error"):
            status = "AUDIT_REQUIRED"
            reason = "current_toolchain_unavailable"
        else:
            expected = expected_toolchain_canonical(language, str(validated))
            observed = current.get("canonical")
            status = "REUSABLE" if expected == observed else "AUDIT_REQUIRED"
            reason = None if status == "REUSABLE" else "toolchain_fingerprint_changed"
        row = {
            "id": artifact["id"],
            "language": language,
            "workload": artifact.get("workload"),
            "validated_toolchain": validated,
            "validated_canonical": expected_toolchain_canonical(language, str(validated)),
            "current_canonical": current.get("canonical"),
            "status": status,
            "reason": reason,
        }
        rows.append(row)
        if status == "AUDIT_REQUIRED":
            audit_required.append(artifact["id"])
    payload = {
        "schema_version": 1,
        "checked_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "artifacts": rows,
        "audit_required": audit_required,
        "ok": not audit_required,
        "note": (
            "AUDIT_REQUIRED does not forbid reuse. It requires the capability-currency audit "
            "defined by the execution policy before the affected artifact may be used."
        ),
    }
    json_dump(root / "results" / "reuse_status.json", payload)
    print(json.dumps(payload, indent=2))
    return 0 if (not args.strict or not audit_required) else 2


def suspicious_env() -> list[str]:
    bad = []
    for key, value in os.environ.items():
        upper = key.upper()
        if value and any(part in upper for part in SECRET_ENV_PARTS):
            bad.append(key)
    return sorted(set(bad))


_GATEWAY_CLIENT_MODULE: Any = None


def gateway_client_module() -> Any:
    """Load the sibling credential-less client without touching sys.path."""
    global _GATEWAY_CLIENT_MODULE
    if _GATEWAY_CLIENT_MODULE is None:
        import importlib.util

        path = Path(__file__).resolve().parent / "gateway_client.py"
        spec = importlib.util.spec_from_file_location(
            "quidra_benchmark_gateway_client", path
        )
        if spec is None or spec.loader is None:
            raise BenchmarkError(f"inference gateway client is missing: {path}")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _GATEWAY_CLIENT_MODULE = module
    return _GATEWAY_CLIENT_MODULE


def gateway_config(root: Path) -> dict[str, Any]:
    path = root / "template" / GATEWAY_CONFIG_RELATIVE
    config = json_load(path)
    if config.get("schema_version") != 1:
        raise BenchmarkError(f"unsupported inference gateway config schema: {path}")
    return config


def gateway_socket_path(root: Path, config: dict[str, Any]) -> Path:
    return Path(os.environ.get(GATEWAY_SOCKET_ENV) or config["socket_path"])


# --------------------------------------------------------------------------
# Observed isolation facts
#
# Everything below reads the state of the *running* process rather than trusting
# a declaration. An environment variable can be typed by anyone; an empty
# capability bounding set, a loopback-only network namespace and a read-only
# snapshot mount cannot.
# --------------------------------------------------------------------------


def _path_is_writable(path: Path) -> bool:
    if not path.is_dir():
        return False
    probe = path / f".quidra-write-probe-{os.getpid()}"
    try:
        with probe.open("wb"):
            pass
    except OSError:
        return False
    try:
        probe.unlink()
    except OSError:
        pass
    return True


def _proc_status_field(name: str) -> str | None:
    status = Path("/proc/self/status")
    if not status.is_file():
        return None
    try:
        for line in status.read_text(encoding="utf-8").splitlines():
            if line.startswith(f"{name}:"):
                return line.split(":", 1)[1].strip()
    except OSError:
        return None
    return None


def parse_mountinfo(text: str) -> list[dict[str, str]]:
    """Extract mount target, options and source from /proc/self/mountinfo."""
    mounts = []
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 7 or "-" not in fields:
            continue
        separator = fields.index("-")
        mounts.append({
            "target": fields[4],
            "options": fields[5],
            "source": fields[separator + 2] if len(fields) > separator + 2 else "",
        })
    return mounts


def collect_isolation_observations(root: Path) -> dict[str, Any]:
    observations: dict[str, Any] = {
        "platform": sys.platform,
        "euid": os.geteuid() if hasattr(os, "geteuid") else None,
        "cwd": os.getcwd(),
        "network_interfaces": None,
        "mounts": None,
        "no_new_privileges": None,
        "capability_bounding_set": None,
        "writable": {},
    }

    interfaces = Path("/sys/class/net")
    if interfaces.is_dir():
        try:
            observations["network_interfaces"] = sorted(p.name for p in interfaces.iterdir())
        except OSError:
            observations["network_interfaces"] = None

    mountinfo = Path("/proc/self/mountinfo")
    if mountinfo.is_file():
        try:
            observations["mounts"] = parse_mountinfo(mountinfo.read_text(encoding="utf-8"))
        except OSError:
            observations["mounts"] = None

    no_new_privs = _proc_status_field("NoNewPrivs")
    if no_new_privs is not None:
        observations["no_new_privileges"] = no_new_privs
    observations["capability_bounding_set"] = _proc_status_field("CapBnd")

    for name in ("repo", "template"):
        observations["writable"][name] = _path_is_writable(root / name)
    observations["writable"]["work"] = _path_is_writable(root / "work")

    return observations


def isolation_problems(root: Path, observations: dict[str, Any]) -> list[str]:
    """Hard requirements, judged only from what the process can observe."""
    problems: list[str] = []

    if observations.get("euid") == 0:
        problems.append("sandbox_process_runs_as_root")

    interfaces = observations.get("network_interfaces")
    if interfaces is None:
        problems.append("network_isolation_unverifiable")
    else:
        routable = sorted(set(interfaces) - {"lo"})
        if routable:
            problems.append("sandbox_network_not_isolated:" + ",".join(routable))

    mounts = observations.get("mounts")
    if mounts is None:
        problems.append("mount_visibility_unverifiable")
    else:
        for mount in mounts:
            target = mount.get("target", "")
            if target in ALLOWED_MOUNT_POINTS:
                continue
            if any(target == p or target.startswith(p + "/") for p in ALLOWED_MOUNT_PREFIXES):
                continue
            problems.append(f"unexpected_mount_visible_in_sandbox:{target}")
        for mount in mounts:
            source = mount.get("source", "")
            if re.match(r"^/(?:Users|home)/[^/]+", source) and mount.get("target") != "/":
                problems.append(f"host_home_path_mounted:{mount.get('target')}")

    writable = observations.get("writable", {})
    if writable.get("repo"):
        problems.append("evaluated_repo_snapshot_must_be_mounted_read_only")
    if writable.get("template"):
        problems.append("frozen_template_must_be_mounted_read_only")
    if not writable.get("work"):
        problems.append("agent_work_directory_is_not_writable")

    no_new_privs = observations.get("no_new_privileges")
    if no_new_privs is None:
        problems.append("no_new_privileges_unverifiable")
    elif no_new_privs.strip() != "1":
        problems.append("no_new_privileges_not_set")

    capabilities = observations.get("capability_bounding_set")
    if capabilities is None:
        problems.append("capability_bounding_set_unverifiable")
    elif capabilities.strip("0") != "":
        problems.append(f"capabilities_not_dropped:{capabilities}")

    return problems


def launcher_contract_problems(
    raw_contract: str | None, observations: dict[str, Any], env: dict[str, str]
) -> tuple[dict[str, Any], list[str]]:
    """Cross-check the launcher's declaration against observed reality.

    The contract can only ever *fail* a run. Every hard requirement is also
    checked directly against the process, so a forged contract that claims more
    than the sandbox delivers is caught by the mismatch, and one that claims less
    is caught by the direct check.
    """
    problems: list[str] = []
    if not raw_contract:
        return {}, ["launcher_contract_missing"]
    try:
        contract = json.loads(raw_contract)
    except json.JSONDecodeError as exc:
        return {}, [f"launcher_contract_invalid_json:{exc}"]
    if not isinstance(contract, dict):
        return {}, ["launcher_contract_must_be_an_object"]

    if contract.get("contract") != LAUNCHER_CONTRACT_VERSION:
        problems.append("launcher_contract_version_mismatch")
    if contract.get("workspace_root") != CANONICAL_WORKSPACE.as_posix():
        problems.append("launcher_contract_workspace_root_mismatch")
    if contract.get("network") != "none":
        problems.append("launcher_contract_network_must_be_none")
    for claim in (
        "no_new_privileges",
        "read_only_root_filesystem",
    ):
        if contract.get(claim) is not True:
            problems.append(f"launcher_contract_{claim}_not_claimed")
    for claim in (
        "run_as_root",
        "host_home_mounted",
        "ssh_agent_forwarded",
        "provider_credentials_in_sandbox",
        "claude_configuration_mounted",
    ):
        if contract.get(claim) is not False:
            problems.append(f"launcher_contract_{claim}_must_be_false")
    if contract.get("capabilities_dropped") != "ALL":
        problems.append("launcher_contract_capabilities_must_drop_all")

    if observations.get("euid") is not None and contract.get("uid") != observations["euid"]:
        problems.append("launcher_contract_uid_does_not_match_running_process")

    declared_env = contract.get("environment")
    if not isinstance(declared_env, dict):
        problems.append("launcher_contract_environment_missing")
    else:
        for key, value in declared_env.items():
            if env.get(key) != value:
                problems.append(f"launcher_contract_environment_mismatch:{key}")

    socket_claim = contract.get("inference_socket")
    if socket_claim != env.get(GATEWAY_SOCKET_ENV):
        problems.append("launcher_contract_inference_socket_mismatch")

    return contract, problems


def credential_exposure_problems(
    root: Path, config: dict[str, Any], env: dict[str, str]
) -> list[str]:
    """Provider credentials must simply not exist on this side of the boundary."""
    problems: list[str] = []

    for name in config.get("sandbox_forbidden_credential_environment_names", []):
        if env.get(name):
            problems.append(f"provider_credential_in_sandbox_environment:{name}")

    for key, value in env.items():
        if not value:
            continue
        for kind, pattern in PRIVACY_PATTERNS.items():
            if kind in {"unix_home", "windows_home", "host_temp_path", "email"}:
                continue
            if pattern.search(value):
                problems.append(f"credential_shaped_environment_value:{key}")
                break

    # Only the environment under test is consulted. Reading the interpreter's own
    # HOME would report the operator's real credentials as if the sandbox could
    # see them, which is both wrong and unfalsifiable. preflight separately
    # requires HOME to be set and to live inside the workspace.
    homes = {Path(env.get("HOME") or root / "home"), root / "home"}
    for home in homes:
        for relative in config.get("sandbox_forbidden_credential_paths", []):
            candidate = home / relative
            try:
                exists = candidate.exists()
            except OSError:
                exists = False
            if exists:
                problems.append(f"provider_credential_visible_in_sandbox:{relative}")

    agent_socket = env.get("SSH_AUTH_SOCK")
    if agent_socket and Path(agent_socket).exists():
        problems.append("ssh_agent_socket_forwarded_into_sandbox")

    return sorted(set(problems))


def tool_surface_problems(health: dict[str, Any]) -> tuple[list[str], list[dict[str, Any]]]:
    """Judge what the gateway admits it may enable, rather than demanding silence.

    An empty tool surface is no longer the right test. A network-enabled task may
    legitimately reach a provider-side retrieval tool that the trusted side froze
    in advance. What must stay true is narrower and more important: the sandbox
    cannot select or configure any tool, and nothing on the surface grants host
    access. A gateway that declares nothing while its provider attaches a tool
    would make this attestation a lie, so the declaration is recorded here and
    every entry has to justify itself.
    """
    problems: list[str] = []
    if health.get("sandbox_selectable_tools"):
        problems.append("inference_gateway_lets_the_sandbox_select_tools")

    surface = health.get("exposed_tool_surface")
    if surface is None:
        return ["inference_gateway_does_not_declare_its_tool_surface"], []
    if not isinstance(surface, list):
        return ["inference_gateway_tool_surface_is_malformed"], []

    recorded: list[dict[str, Any]] = []
    for entry in surface:
        if not isinstance(entry, dict):
            problems.append("inference_gateway_tool_surface_is_malformed")
            continue
        name = str(entry.get("name") or entry.get("type") or "unnamed")
        if entry.get("scope") != "provider-side":
            problems.append(f"inference_gateway_exposes_a_non_provider_tool:{name}")
        if entry.get("selectable_by_sandbox") is not False:
            problems.append(f"inference_gateway_tool_is_sandbox_selectable:{name}")
        if entry.get("grants_host_access") is not False:
            problems.append(f"inference_gateway_tool_grants_host_access:{name}")
        recorded.append({
            "name": name,
            "type": entry.get("type"),
            "enabled_for": entry.get("enabled_for"),
            "max_uses_per_request": entry.get("max_uses_per_request"),
        })
    return problems, recorded


def gateway_attestation(
    root: Path, config: dict[str, Any], timeout: float = 15.0
) -> tuple[dict[str, Any], list[str]]:
    """Prove the gateway exists, answers, and refuses everything but inference.

    Two of these checks are deliberately negative. A broker that *claims* to
    expose no tools is worth nothing; a broker observed refusing a request that
    carries a tool field, and refusing an unsupported request kind, is evidence.
    """
    problems: list[str] = []
    info: dict[str, Any] = {}
    socket_path = gateway_socket_path(root, config)
    info["socket_path"] = str(socket_path)

    try:
        require_under(socket_path, root)
    except BenchmarkError:
        return info, ["inference_gateway_socket_outside_workspace"]

    expected_socket = config["socket_path"]
    if lexical_absolute(socket_path).as_posix() != expected_socket:
        problems.append("inference_gateway_socket_path_not_canonical")
    if not socket_path.exists():
        return info, [*problems, "inference_gateway_socket_missing"]
    if not socket_path.is_socket():
        return info, [*problems, "inference_gateway_socket_is_not_a_socket"]

    client_module = gateway_client_module()
    client = client_module.InferenceGatewayClient(socket_path, timeout=timeout)
    try:
        health = client.health()
    except client_module.GatewayClientError as exc:
        return info, [*problems, f"inference_gateway_handshake_failed:{exc}"]

    info["gateway"] = health.get("gateway")
    info["provider"] = health.get("provider", {}).get("id")
    info["network_policy"] = health.get("network_policy")

    if health.get("credential_less_client") is not True:
        problems.append("inference_gateway_requires_client_credentials")
    if health.get("host_tools_exposed") is not False:
        problems.append("inference_gateway_exposes_host_tools")
    surface_problems, surface = tool_surface_problems(health)
    problems.extend(surface_problems)
    info["provider_tool_surface"] = surface
    if sorted(health.get("capabilities", [])) != sorted(config["allowed_request_kinds"]):
        problems.append("inference_gateway_capabilities_mismatch")

    probes = (
        (
            "tool_field",
            {
                "schema_version": 1,
                "kind": "inference.request",
                "request_id": "preflight-tool-probe",
                "messages": [{"role": "user", "content": "preflight probe"}],
                "tools": [{"name": "shell", "description": "run a host command"}],
            },
        ),
        (
            "unsupported_kind",
            {
                "schema_version": 1,
                "kind": "shell.exec",
                "request_id": "preflight-kind-probe",
                "argv": ["id"],
            },
        ),
    )
    refusals = {}
    for label, payload in probes:
        try:
            response = client.raw_exchange(payload)
        except client_module.GatewayClientError as exc:
            problems.append(f"inference_gateway_probe_failed:{label}:{exc}")
            continue
        refused = (
            response.get("kind") == "inference.error"
            and response.get("error", {}).get("class") == "policy"
        )
        refusals[label] = refused
        if not refused:
            problems.append(f"inference_gateway_accepted_a_forbidden_request:{label}")
    info["policy_probes"] = refusals

    return info, problems


def cmd_preflight(args: argparse.Namespace) -> int:
    root = workspace(args)
    required = (
        "run.json", "repo", "template", "work/root", "work/agents",
        "raw", "results", "prompts/by-hash", "home", "tmp", "gateway",
    )
    missing = [name for name in required if not (root / name).exists()]
    problems: list[str] = []
    if missing:
        problems.append("missing:" + ",".join(missing))

    try:
        run = json_load(root / "run.json")
    except Exception as exc:
        run = {}
        problems.append(f"invalid_run_json:{exc}")

    if root != lexical_absolute(CANONICAL_WORKSPACE):
        problems.append("workspace_root_must_be_/quidra-benchmark")
    if run.get("workspace_root") != CANONICAL_WORKSPACE.as_posix():
        problems.append("run_workspace_root_must_be_/quidra-benchmark")

    declared_sandbox_mode = run.get("sandbox_mode")
    if declared_sandbox_mode not in {"container", "chroot", "namespace", "external-sandbox"}:
        problems.append("sandbox_mode_not_isolating")

    env = dict(os.environ)
    observations = collect_isolation_observations(root)

    # The isolation boundary is judged from the running process, not from a
    # declaration. These checks are what makes the attestations below meaningful:
    # the launcher may only set them after actually applying the restrictions,
    # and a run that sets them without applying anything fails here.
    problems.extend(isolation_problems(root, observations))

    contract, contract_problems = launcher_contract_problems(
        env.get(LAUNCHER_CONTRACT_ENV), observations, env
    )
    problems.extend(contract_problems)

    if env.get("QUIDRA_BENCHMARK_SANDBOX_ATTESTED") != declared_sandbox_mode:
        problems.append("sandbox_runner_attestation_missing_or_mode_mismatch")
    if env.get("QUIDRA_BENCHMARK_SANDBOX_ROOT") != str(root):
        problems.append("sandbox_runner_root_attestation_missing_or_mismatch")
    if env.get("QUIDRA_BENCHMARK_WORKER_GATEWAY_ATTESTED") != "packet-gateway-v1":
        problems.append("worker_gateway_attestation_missing_or_mismatch")
    if env.get("QUIDRA_BENCHMARK_PACKET_WORKER_LOCAL_TOOLS") != "disabled":
        problems.append("packet_worker_local_tools_not_disabled")
    if env.get("QUIDRA_BENCHMARK_SANDBOX_AGENT_LAUNCHER_ATTESTED") != "inside-sandbox-v1":
        problems.append("sandbox_agent_launcher_attestation_missing_or_mismatch")

    gateway_info: dict[str, Any] = {}
    try:
        gateway_cfg = gateway_config(root)
    except (BenchmarkError, OSError, json.JSONDecodeError) as exc:
        gateway_cfg = {}
        problems.append(f"inference_gateway_config_invalid:{exc}")
    if gateway_cfg:
        problems.extend(credential_exposure_problems(root, gateway_cfg, env))
        gateway_info, gateway_problems = gateway_attestation(
            root, gateway_cfg, timeout=float(getattr(args, "gateway_timeout", 15.0))
        )
        problems.extend(gateway_problems)

    problems.extend(template_integrity_problems(root, run))

    for env_name in ("HOME", "TMPDIR", "PWD"):
        value = env.get(env_name)
        if value:
            try:
                require_under(Path(value), root)
            except BenchmarkError:
                problems.append(f"{env_name.lower()}_outside_workspace")

    bad_env = suspicious_env()
    if bad_env:
        problems.append("sensitive_environment_names:" + ",".join(bad_env))

    config = root / "template" / "config" / "primary.json"
    try:
        for evaluation in PRIMARY_NAMES:
            load_evaluation_requirements(root, evaluation)
    except BenchmarkError as exc:
        problems.append(f"evaluation_requirements_invalid:{exc}")
    if config.exists():
        try:
            primary_cfg = json_load(config)
            if primary_cfg.get("workspace_root") != CANONICAL_WORKSPACE.as_posix():
                problems.append("primary_config_workspace_root_must_be_/quidra-benchmark")
            allowed_prefix = primary_cfg.get("privacy", {}).get("allowed_absolute_path_prefix")
            if allowed_prefix != CANONICAL_WORKSPACE.as_posix() + "/":
                problems.append("primary_config_allowed_path_prefix_mismatch")
            worker_iso = primary_cfg.get("worker_isolation", {})
            if worker_iso.get("default_mode") != "packet-only":
                problems.append("primary_config_worker_default_mode_mismatch")
            if set(worker_iso.get("allowed_modes", [])) != {"packet-only", "sandbox-agent"}:
                problems.append("primary_config_worker_allowed_modes_mismatch")
            if worker_iso.get("host_tool_capable_leaf_policy") != "forbidden":
                problems.append("primary_config_host_tool_capable_leaf_policy_mismatch")
            if worker_iso.get("inference_transport") != "credential-less-gateway-socket":
                problems.append("primary_config_inference_transport_mismatch")
            if worker_iso.get("gateway_protocol") != "quidra-inference-gateway-v1":
                problems.append("primary_config_gateway_protocol_mismatch")
            if worker_iso.get("sandbox_provider_credentials") != "forbidden":
                problems.append("primary_config_sandbox_provider_credentials_mismatch")
            try:
                sampling_config(root)
            except BenchmarkError as exc:
                problems.append(f"primary_config_sampling_invalid:{exc}")
        except Exception as exc:
            problems.append(f"primary_config_invalid:{exc}")
    if config.exists() and run:
        if sha256_file(config) != run.get("primary_config_sha256"):
            problems.append("primary_config_hash_mismatch")

    result = {
        "ok": not problems,
        "problems": problems,
        "observed_isolation": observations,
        "launcher_contract": {
            "present": bool(contract),
            "contract": contract.get("contract"),
            "image": contract.get("image"),
        },
        "inference_gateway": gateway_info,
        "checked_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
    }
    json_dump(root / "results" / "preflight.json", result)
    print(json.dumps(result, indent=2))
    return 0 if result["ok"] else 2


def plan_counts(cfg: dict[str, Any], languages: list[str]) -> dict[str, Any]:
    nlang = len(languages)
    sc = cfg["semantic_compression"]
    ll = cfg["llm_learnability"]
    counts: dict[str, Any] = {
        "semantic_compression": {
            "minimum_probe_language_units": int(sc["fixed_probe_count"]) * nlang
        },
        "llm_learnability": {
            "minimum_condition_language_units": nlang * (
                int(ll["keyword_anonymization_seeds"])
                + int(ll["vocabulary_anonymization_seeds"])
                + int(ll["structural_surface_transformation_sets"])
                + int(ll["novel_rule_generalization_trials_per_language"])
                + int(ll["held_out_rule_composition_trials_per_language"])
                + int(ll["prior_conflict_resistance_trials_per_language"])
            )
        },
        "language_quality": {"requires_manifest": True},
        "ecosystem": {"requires_manifest": True},
        "llm_proficiency": {"requires_manifest": True},
    }
    return counts


def cmd_plan(args: argparse.Namespace) -> int:
    root = workspace(args)
    run = json_load(root / "run.json")
    integrity = template_integrity_problems(root, run)
    if integrity:
        raise BenchmarkError("template integrity failed: " + ", ".join(integrity))
    cfg = json_load(root / "template" / "config" / "primary.json")
    counts = plan_counts(cfg, metadata_languages(root))
    manifest_path = root / "work" / "root" / "manifest.json"
    manifest = json_load(manifest_path) if manifest_path.exists() else None
    ledger_path = root / "work" / "root" / "ledger.json"
    ledger = json_load(ledger_path) if ledger_path.exists() else None
    manifest_ready = bool(
        manifest
        and ledger
        and ledger.get("manifest_sha256") == sha256_file(manifest_path)
    )
    missing = []
    if not manifest_ready:
        missing = list(PRIMARY_NAMES)
    else:
        by_eval: dict[str, int] = {name: 0 for name in PRIMARY_NAMES}
        max_llm_calls = 0
        max_input_tokens = 0
        max_output_tokens = 0
        for unit in manifest.get("work_units", []):
            ev = unit.get("evaluation")
            if ev in by_eval:
                by_eval[ev] += 1
            calls = int(unit.get("max_llm_calls", 0) or 0)
            max_llm_calls += calls
            max_input_tokens += calls * int(unit.get("estimated_input_tokens_per_call", 0) or 0)
            max_output_tokens += calls * int(unit.get("max_output_tokens_per_call", 0) or 0)
        for ev, n in by_eval.items():
            counts.setdefault(ev, {})["manifest_work_units"] = n
        counts["llm_capacity"] = {
            "configured_max_calls": max_llm_calls,
            "estimated_max_input_tokens": max_input_tokens,
            "configured_max_output_tokens": max_output_tokens,
            "estimated_max_total_tokens": max_input_tokens + max_output_tokens,
        }
        missing = [ev for ev in PRIMARY_NAMES if by_eval.get(ev, 0) == 0]

    result = {
        "primary_counts": counts,
        "manifest_present": manifest is not None,
        "ledger_present": ledger is not None,
        "manifest_ready": manifest_ready,
        "missing_primary_manifest_sections": missing,
        "strict_ready": bool(manifest_ready and not missing),
    }
    json_dump(root / "results" / "plan.json", result)
    print(json.dumps(result, indent=2))
    if args.strict and missing:
        return 2
    return 0





def sanitized_subprocess_env(root: Path, cwd: Path) -> dict[str, str]:
    root = lexical_absolute(root)
    cwd = require_under(cwd, root)
    env = {
        "HOME": str(root / "home"),
        "TMPDIR": str(root / "tmp"),
        "TMP": str(root / "tmp"),
        "TEMP": str(root / "tmp"),
        "PWD": str(cwd),
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin:/opt/homebrew/bin",
        "PYTHONNOUSERSITE": "1",
        # The template tree is integrity-hashed; a runner command that imports a
        # sibling template module must not drop a __pycache__ into it.
        "PYTHONDONTWRITEBYTECODE": "1",
    }
    for key in ("LANG", "LC_ALL", "LC_CTYPE", "TZ", *TOOLCHAIN_ENVIRONMENT_PASSTHROUGH):
        value = os.environ.get(key)
        if value:
            env[key] = value
    # The compiler under evaluation is built from the snapshot into the
    # workspace; `quidra` on a scored process's PATH must resolve to that build.
    target_bin = root / "work" / "root" / "target-build"
    if (target_bin / "quidra").is_file():
        env["PATH"] = f"{target_bin}:{env['PATH']}"
    if (
        root != lexical_absolute(CANONICAL_WORKSPACE)
        and os.environ.get("QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS") == "1"
    ):
        env["QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS"] = "1"
    return env


def validator_argv(command: str, root: Path) -> list[str]:
    try:
        argv = shlex.split(command, posix=True)
    except ValueError as exc:
        raise BenchmarkError(f"invalid validator command quoting: {exc}") from exc
    if not argv:
        raise BenchmarkError("validator command is empty")

    if argv == ["true"]:
        if lexical_absolute(root) == lexical_absolute(CANONICAL_WORKSPACE):
            raise BenchmarkError(
                "unconditional 'true' validator is forbidden in a real benchmark run"
            )
        return argv

    forbidden = (";", "|", "&", ">", "<", "`", "\n", "\r")
    for arg in argv:
        if any(ch in arg for ch in forbidden):
            raise BenchmarkError(
                f"validator argument contains forbidden shell/control syntax: {arg!r}"
            )
        if ".." in PurePosixPath(arg).parts:
            raise BenchmarkError(
                f"validator argument may not traverse with '..': {arg!r}"
            )
        if arg.startswith("/"):
            require_under(Path(arg), root)
        if arg.startswith("-") and "/" in arg:
            raise BenchmarkError(
                f"path-valued validator options must use a separate sandbox path argument: {arg!r}"
            )

    executable = Path(argv[0]).name
    interpreter_flags = {
        "python": {"-c", "-m"},
        "python3": {"-c", "-m"},
        "bash": {"-c"},
        "sh": {"-c"},
        "node": {"-e", "--eval", "-p", "--print"},
    }
    if executable not in interpreter_flags:
        raise BenchmarkError(
            "validator must invoke a frozen script through python/python3/bash/sh/node "
            "or use CI-only 'true'"
        )
    banned = interpreter_flags[executable]
    if any(arg in banned for arg in argv[1:]):
        raise BenchmarkError(
            f"inline/module validator mode is forbidden for {executable}"
        )

    script_index = None
    for i, arg in enumerate(argv[1:], start=1):
        if not arg.startswith("-"):
            script_index = i
            break
    if script_index is None:
        raise BenchmarkError("validator command requires a frozen script path")

    script = Path(argv[script_index])
    if not script.is_absolute():
        raise BenchmarkError("validator script path must be absolute")
    script = require_under(script, root)
    trusted_roots = (
        lexical_absolute(root / "template" / "validators"),
        lexical_absolute(root / "template" / "scripts"),
        lexical_absolute(root / "template" / "scoring"),
    )
    trusted = False
    for trusted_root in trusted_roots:
        try:
            script.relative_to(trusted_root)
            trusted = True
            break
        except ValueError:
            pass
    if not trusted:
        raise BenchmarkError(
            f"validator script is not inside a frozen trusted template subtree: {script}"
        )
    if not script.is_file():
        raise BenchmarkError(f"validator script does not exist: {script}")

    if script.name == "benchmark.py":
        if script_index + 1 >= len(argv):
            raise BenchmarkError(
                "benchmark.py validator requires an approved subcommand"
            )
        approved = {"plan-check", "result-check", "command-result-check", "aggregate-check"}
        if argv[script_index + 1] not in approved:
            raise BenchmarkError(
                "benchmark.py may be a Task Packet validator only for: "
                + ", ".join(sorted(approved))
            )

    return argv


def load_evaluation_requirements(root: Path, evaluation: str) -> tuple[Path, list[str]]:
    path = root / "template" / "config" / "evaluation_requirements.json"
    if not path.is_file():
        raise BenchmarkError("evaluation_requirements.json is missing")
    data = json_load(path)
    if data.get("schema_version") != 1:
        raise BenchmarkError("evaluation_requirements schema_version must be 1")
    evaluations = data.get("evaluations")
    if not isinstance(evaluations, dict):
        raise BenchmarkError("evaluation_requirements evaluations must be an object")
    entry = evaluations.get(evaluation)
    if not isinstance(entry, dict):
        raise BenchmarkError(f"evaluation requirements missing for {evaluation}")
    required = entry.get("required")
    if not isinstance(required, list) or not required or not all(
        isinstance(x, str) and x.strip() for x in required
    ):
        raise BenchmarkError(
            f"{evaluation}: required evaluation IDs must be a non-empty string array"
        )
    normalized = [x.strip() for x in required]
    if len(set(normalized)) != len(normalized):
        raise BenchmarkError(f"{evaluation}: duplicate required evaluation IDs")
    return path, normalized


def execution_ownership_for(
    root: Path, evaluation: str, required_requirement_ids: Iterable[str]
) -> dict[str, set[str] | tuple[str, ...]]:
    """Freeze which requirement IDs belong to deterministic code vs LLM judgment."""
    path = root / "template" / "config" / "execution_ownership.json"
    if not path.is_file():
        raise BenchmarkError("execution_ownership.json is required")
    data = json_load(path)
    if data.get("schema_version") != 1:
        raise BenchmarkError("execution_ownership.json has unsupported schema_version")
    row = (data.get("evaluations") or {}).get(evaluation)
    if not isinstance(row, dict):
        raise BenchmarkError(f"execution ownership is missing {evaluation}")
    runner = {str(v) for v in (row.get("runner_command_requirements") or [])}
    agent = {str(v) for v in (row.get("agent_requirements") or [])}
    prefixes = tuple(str(v) for v in (row.get("agent_requirement_prefixes") or []))
    overlap = sorted(runner & agent)
    if overlap:
        raise BenchmarkError(
            f"{evaluation}: execution ownership overlaps: {', '.join(overlap)}"
        )
    required = {str(v) for v in required_requirement_ids}
    unclassified = sorted(
        rid for rid in required
        if rid not in runner
        and rid not in agent
        and not any(rid.startswith(prefix) for prefix in prefixes)
    )
    if unclassified:
        raise BenchmarkError(
            f"{evaluation}: execution ownership leaves requirements unclassified: "
            + ", ".join(unclassified)
        )
    return {
        "runner": runner,
        "agent": agent,
        "agent_prefixes": prefixes,
    }


def validate_requirement_execution_ownership(
    uid: str,
    requirement_ids: Iterable[str],
    execution_kind: str,
    ownership: dict[str, set[str] | tuple[str, ...]],
) -> None:
    """Refuse paying an LLM for runner work, or scripting a judgment metric."""
    for rid in requirement_ids:
        runner_owned = rid in ownership["runner"]
        agent_owned = (
            rid in ownership["agent"]
            or any(
                rid.startswith(prefix)
                for prefix in ownership["agent_prefixes"]
            )
        )
        if runner_owned and execution_kind != "command":
            raise BenchmarkError(
                f"{uid}: {rid} is runner-owned and may not be delegated to an LLM"
            )
        if agent_owned and execution_kind != "agent":
            raise BenchmarkError(
                f"{uid}: {rid} requires LLM judgment and may not be replaced "
                "by a deterministic command without revising the frozen contract"
            )


def validate_work_plan_data(root: Path, evaluation: str, plan: dict[str, Any]) -> dict[str, Any]:
    if evaluation not in PRIMARY_NAMES:
        raise BenchmarkError(f"unknown Primary evaluation: {evaluation}")
    if plan.get("evaluation") != evaluation:
        raise BenchmarkError(f"work plan evaluation mismatch: {plan.get('evaluation')!r}")
    requirements_path, required_requirement_ids = load_evaluation_requirements(root, evaluation)
    allowed_requirement_ids = set(required_requirement_ids)
    ownership = execution_ownership_for(
        root, evaluation, required_requirement_ids
    )
    covered_non_aggregation: set[str] = set()
    raw_units = plan.get("work_units")
    if not isinstance(raw_units, list) or not raw_units:
        raise BenchmarkError("work plan must contain a non-empty work_units array")
    seen_ids: set[str] = set()
    seen_agents: set[str] = set()
    normalized = []
    for raw in raw_units:
        if not isinstance(raw, dict):
            raise BenchmarkError("work-plan units must be objects")
        uid = str(raw.get("id") or "").strip()
        agent = str(raw.get("assigned_agent_id") or "").strip()
        goal = str(raw.get("goal") or "").strip()
        validator = str(raw.get("validator_command") or "").strip()
        phase = str(raw.get("phase") or "").strip()
        if not uid or not agent or not goal or not validator:
            raise BenchmarkError(
                "every work unit requires id, assigned_agent_id, goal and validator_command"
            )
        validator_argv(validator, root)
        if phase not in {"readiness", "measurement", "aggregation"}:
            raise BenchmarkError(
                f"{uid}: phase must be readiness, measurement or aggregation"
            )
        if uid in seen_ids:
            raise BenchmarkError(f"duplicate work-unit id in plan: {uid}")
        if agent in seen_agents:
            raise BenchmarkError(
                f"assigned_agent_id must be unique per leaf work unit; group related work into one unit instead: {agent}"
            )
        seen_ids.add(uid)
        seen_agents.add(agent)
        deps = raw.get("dependencies", [])
        evidence = raw.get("evidence_paths", [])
        reads = raw.get("read_paths", [])
        hashes = raw.get("input_hashes", {})
        reuse_audit_for = raw.get("reuse_audit_for", [])
        requirement_ids = raw.get("requirement_ids", [])
        workload_ids = raw.get("workload_ids", [])
        if not isinstance(deps, list) or not all(isinstance(x, str) for x in deps):
            raise BenchmarkError(f"{uid}: dependencies must be a string array")
        if not isinstance(evidence, list) or not evidence or not all(isinstance(x, str) for x in evidence):
            raise BenchmarkError(f"{uid}: evidence_paths must be a non-empty string array")
        if not isinstance(reads, list) or not all(isinstance(x, str) for x in reads):
            raise BenchmarkError(f"{uid}: read_paths must be a string array")
        if not isinstance(hashes, dict):
            raise BenchmarkError(f"{uid}: input_hashes must be an object")
        if not isinstance(reuse_audit_for, list) or not all(isinstance(x, str) for x in reuse_audit_for):
            raise BenchmarkError(f"{uid}: reuse_audit_for must be a string array")
        if not isinstance(requirement_ids, list) or not all(
            isinstance(x, str) and x.strip() for x in requirement_ids
        ):
            raise BenchmarkError(f"{uid}: requirement_ids must be a string array")
        if not isinstance(workload_ids, list) or not all(
            isinstance(x, str) and re.fullmatch(r"mb\d\d", x)
            for x in workload_ids
        ):
            raise BenchmarkError(f"{uid}: workload_ids must contain mbNN identifiers")
        if len(workload_ids) != len(set(workload_ids)):
            raise BenchmarkError(f"{uid}: duplicate workload_ids are not allowed")
        requirement_ids = [x.strip() for x in requirement_ids]
        allowed_agent_prefixes = tuple(ownership.get("agent_prefixes") or ())
        unknown_requirements = sorted(
            rid for rid in set(requirement_ids) - allowed_requirement_ids
            if not any(str(rid).startswith(prefix) for prefix in allowed_agent_prefixes)
        )
        if unknown_requirements:
            raise BenchmarkError(
                f"{uid}: unknown requirement_ids for {evaluation}: "
                + ", ".join(unknown_requirements)
            )
        if phase != "aggregation":
            covered_non_aggregation.update(requirement_ids)
        execution_kind = str(raw.get("execution_kind", "agent"))
        if execution_kind not in {"agent", "command"}:
            raise BenchmarkError(f"{uid}: execution_kind must be agent or command")
        validate_requirement_execution_ownership(
            uid, requirement_ids, execution_kind, ownership
        )
        if execution_kind == "agent":
            worker_mode = str(raw.get("worker_mode") or "packet-only")
            if worker_mode not in {"packet-only", "sandbox-agent"}:
                raise BenchmarkError(
                    f"{uid}: worker_mode must be packet-only or sandbox-agent"
                )
        else:
            configured_worker_mode = raw.get("worker_mode")
            if configured_worker_mode not in (None, "runner-command"):
                raise BenchmarkError(
                    f"{uid}: command work may only use worker_mode=runner-command"
                )
            worker_mode = "runner-command"
        prompt_sections = raw.get("prompt_sections", [])
        if not isinstance(prompt_sections, list) or not all(isinstance(x, str) for x in prompt_sections):
            raise BenchmarkError(f"{uid}: prompt_sections must be a string array")
        max_attempts = int(raw.get("max_attempts", runner_max_attempts(root)) or 3)
        if max_attempts < 1:
            raise BenchmarkError(f"{uid}: max_attempts must be at least 1")
        result_kind = str(raw.get("result_kind", "requirements"))
        if result_kind not in {"requirements", "audit", "aggregate"}:
            raise BenchmarkError(f"{uid}: invalid result_kind")
        runner_action = raw.get("runner_action")
        if execution_kind == "command":
            if phase == "aggregation":
                if runner_action not in (None, "aggregate-primary"):
                    raise BenchmarkError(f"{uid}: aggregation command has invalid runner_action")
            elif runner_action not in {
                "micro-measure", "adversarial-measure", "quidra-audit", "static-coverage",
                "semantic-capability-coverage", "semantic-premeasurement-validation",
                "learnability-integrity", "proficiency-integrity",
            }:
                raise BenchmarkError(
                    f"{uid}: command work requires an approved runner_action"
                )
        elif runner_action is not None:
            raise BenchmarkError(f"{uid}: agent work may not define runner_action")
        assigned_languages = raw.get("assigned_languages", [])
        if not isinstance(assigned_languages, list) or not all(
            isinstance(x, str) for x in assigned_languages
        ):
            raise BenchmarkError(f"{uid}: assigned_languages must be a string array")
        fixed_languages = set(metadata_languages(root))
        unknown_languages = sorted(set(assigned_languages) - fixed_languages)
        if unknown_languages:
            raise BenchmarkError(
                f"{uid}: unknown assigned_languages: {', '.join(unknown_languages)}"
            )

        primary_cfg = json_load(root / "template" / "config" / "primary.json")
        max_multi_requirements = int(
            primary_cfg.get("runner", {}).get(
                "max_requirement_ids_per_multi_language_agent", 3
            )
        )
        if (
            execution_kind == "agent"
            and result_kind == "requirements"
            and len(requirement_ids) > max_multi_requirements
            and len(assigned_languages) != 1
        ):
            raise BenchmarkError(
                f"{uid}: multi-language agent owns {len(requirement_ids)} requirement IDs; "
                f"maximum is {max_multi_requirements}. Split the work unit or assign exactly "
                "one language when the requirements intentionally share one trial history."
            )

        max_calls = int(raw.get("max_llm_calls", 0) or 0)
        input_tokens = int(raw.get("estimated_input_tokens_per_call", 0) or 0)
        output_tokens = int(raw.get("max_output_tokens_per_call", 0) or 0)
        if max_calls < 0:
            raise BenchmarkError(f"{uid}: max_llm_calls cannot be negative")
        if input_tokens < 0 or output_tokens < 0:
            raise BenchmarkError(f"{uid}: token estimates cannot be negative")
        if max_calls > 0 and (input_tokens <= 0 or output_tokens <= 0):
            raise BenchmarkError(
                f"{uid}: LLM work requires estimated_input_tokens_per_call "
                "and max_output_tokens_per_call"
            )
        normalized.append({
            **raw,
            "id": uid,
            "assigned_agent_id": agent,
            "goal": goal,
            "validator_command": validator,
            "phase": phase,
            "dependencies": deps,
            "evidence_paths": normalize_paths(evidence, root),
            "read_paths": normalize_paths(reads, root),
            "input_hashes": hashes,
            "reuse_audit_for": reuse_audit_for,
            "requirement_ids": requirement_ids,
            "workload_ids": workload_ids,
            "max_llm_calls": max_calls,
            "estimated_input_tokens_per_call": input_tokens,
            "max_output_tokens_per_call": output_tokens,
            "network_allowed": bool(raw.get("network_allowed", False)),
            "execution_kind": execution_kind,
            "worker_mode": worker_mode,
            "prompt_sections": prompt_sections,
            "max_attempts": max_attempts,
            "result_kind": result_kind,
            "assigned_languages": assigned_languages,
            "packet_layout": str(raw.get("packet_layout") or "task-first"),
            "runner_action": runner_action,
        })
    missing_requirements = sorted(allowed_requirement_ids - covered_non_aggregation)
    if missing_requirements:
        raise BenchmarkError(
            f"{evaluation}: Primary plan omits required evaluation coverage: "
            + ", ".join(missing_requirements)
        )

    if evaluation == "language_quality":
        by_id = {unit["id"]: unit for unit in normalized}
        # Quidra's programs live in the evaluated snapshot and are re-audited
        # mechanically for every commit; no model authors them during a run.
        audit_unit = by_id.get("lq-quidra-audit")
        if (
            audit_unit is None
            or audit_unit.get("execution_kind") != "command"
            or audit_unit.get("runner_action") != "quidra-audit"
            or "gate.quidra_programs_current" not in set(audit_unit.get("requirement_ids", []))
        ):
            raise BenchmarkError(
                "language_quality: missing the quidra-audit command unit that gates "
                "measurement on the snapshot's own benchmark programs"
            )
        mechanical = by_id.get("lq-micro-mechanical")
        required_dependencies = {"lq-quidra-audit"}
        # Currency audits are a legitimate additional dependency here, not a
        # defect. The mechanical unit reads the reusable comparison-language
        # programs, so when one of those needs a toolchain-currency audit the
        # planner correctly makes the measurement wait for it. Requiring exact
        # equality rejected that, which made the plan unbuildable on any host
        # whose toolchains differ from the ones the catalog was validated on -
        # the ordinary case, since the catalog is validated on macOS and runs
        # execute on Linux.
        audit_unit_ids = {
            unit["id"] for unit in normalized if unit.get("reuse_audit_for")
        }
        mechanical_dependencies = set(mechanical.get("dependencies", [])) if mechanical else set()
        unexpected = mechanical_dependencies - required_dependencies - audit_unit_ids
        if (
            mechanical is None
            or mechanical.get("execution_kind") != "command"
            or not required_dependencies <= mechanical_dependencies
            or unexpected
        ):
            raise BenchmarkError(
                "language_quality: mechanical micro unit must depend on the "
                "quidra-audit unit and nothing beyond the currency audits of the "
                "artifacts it measures"
                + (f"; unexpected: {sorted(unexpected)}" if unexpected else "")
            )

    return {
        "evaluation": evaluation,
        "work_units": normalized,
        "work_unit_count": len(normalized),
        "requirements_path": str(requirements_path),
        "required_requirement_ids": required_requirement_ids,
        "covered_requirement_ids": sorted(covered_non_aggregation),
    }


def cmd_plan_check(args: argparse.Namespace) -> int:
    root = workspace(args)
    path = Path(args.file)
    if not path.is_absolute():
        path = root / path
    path = require_under(path, root)
    if not path.is_file():
        raise BenchmarkError(f"work plan is missing: {path}")
    checked = validate_work_plan_data(root, args.evaluation, json_load(path))
    result = {
        "ok": True,
        "evaluation": args.evaluation,
        "work_unit_count": checked["work_unit_count"],
        "file": str(path),
        "sha256": sha256_file(path),
    }
    print(json.dumps(result, indent=2))
    return 0



def require_privacy_pass(root: Path) -> None:
    path = root / "results" / "privacy_check.json"
    if not path.is_file():
        raise BenchmarkError(
            "privacy check has not passed for this workspace; run privacy-check before dispatch"
        )
    data = json_load(path)
    if not data.get("ok"):
        raise BenchmarkError(
            "privacy check failed for this workspace; do not dispatch benchmark agents"
        )


def runner_max_attempts(root: Path) -> int:
    """The default attempt limit of a work unit.

    The runtime configuration (sandbox_agent.json) wins over primary.json's
    runner section. Primary Task Packets now embed an evaluation-scoped Primary
    projection, but this attempt limit is still runtime-only policy and belongs
    outside the scored cache dependency set. A unit that declares its own
    max_attempts keeps it.
    """
    runtime_path = root / "template" / "config" / "sandbox_agent.json"
    if runtime_path.is_file():
        declared = json_load(runtime_path).get("max_attempts_per_work_unit")
        if declared is not None:
            value = int(declared)
            if value < 1:
                raise BenchmarkError("max_attempts_per_work_unit must be at least 1")
            return value
    primary = json_load(root / "template" / "config" / "primary.json")
    return int((primary.get("runner") or {}).get("max_attempts_per_work_unit", 3) or 3)


def load_work_plan_templates(root: Path) -> dict[str, Any]:
    path = root / "template" / "config" / "work_plan_templates.json"
    data = json_load(path)
    if data.get("schema_version") != 1 or not isinstance(data.get("evaluations"), dict):
        raise BenchmarkError("invalid work_plan_templates.json")
    return data


PRIMARY_EVALUATION_CONFIG_KEYS = {
    "semantic_compression": "semantic_compression",
    "llm_learnability": "llm_learnability",
    "language_quality": "language_quality",
    "ecosystem": None,
    "llm_proficiency": "llm_proficiency",
}


def primary_config_projection_from_data(
    config: dict[str, Any], evaluation: str
) -> dict[str, Any]:
    """Return only the Primary config visible to one evaluation.

    Evaluation-local sections must not invalidate or perturb another evaluation's
    paid prompt/cache. Shared runner/safety/sampling policy remains visible to all
    evaluations, so a genuinely global policy change still invalidates them.
    """
    if evaluation not in PRIMARY_EVALUATION_CONFIG_KEYS:
        raise BenchmarkError(f"unknown Primary evaluation for config projection: {evaluation}")
    own_key = PRIMARY_EVALUATION_CONFIG_KEYS[evaluation]
    local_keys = {
        key for key in PRIMARY_EVALUATION_CONFIG_KEYS.values() if key is not None
    }
    projected = {
        key: value
        for key, value in config.items()
        if key not in local_keys or key == own_key
    }

    # This legacy Language Quality flag described the pre-certified-cache era.
    # It is orchestration documentation, not a scientific condition, and must
    # not make an otherwise identical paid measurement cold.  Keep it out of
    # the evaluation-visible projection so old prompts carrying the flag and
    # current prompts after its correction remain projection-compatible.
    if own_key == "language_quality":
        local = projected.get("language_quality")
        if isinstance(local, dict):
            local = dict(local)
            local.pop("reuse_never_includes_measurements", None)
            projected["language_quality"] = local
    return projected


def primary_config_projection_data(root: Path, evaluation: str) -> dict[str, Any]:
    return primary_config_projection_from_data(
        json_load(root / "template" / "config" / "primary.json"), evaluation
    )


def primary_config_projection_text(root: Path, evaluation: str) -> str:
    return json.dumps(
        primary_config_projection_data(root, evaluation),
        indent=2,
        ensure_ascii=False,
    ) + "\n"


def primary_config_projection_sha256(root: Path, evaluation: str) -> str:
    data = json.dumps(
        primary_config_projection_data(root, evaluation),
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")
    return sha256_bytes(data)


def evaluation_spec_projection_text(
    root: Path, evaluation: str, selectors: list[str]
) -> str:
    path = root / "template" / "methodology" / EVALUATION_SPEC_FILES[evaluation]
    return extract_markdown_sections(path.read_text(encoding="utf-8"), selectors)


def evaluation_spec_projection_sha256(
    root: Path, evaluation: str, selectors: list[str]
) -> str:
    return sha256_bytes(
        evaluation_spec_projection_text(root, evaluation, selectors).encode("utf-8")
    )


def assigned_requirements_projection_text(
    evaluation: str, requirement_ids: list[str]
) -> str:
    return json.dumps(
        {
            "schema_version": 1,
            "evaluation": evaluation,
            "assigned": requirement_ids,
        },
        indent=2,
        sort_keys=True,
    ) + "\n"


def assigned_requirements_projection_sha256(
    evaluation: str, requirement_ids: list[str]
) -> str:
    return sha256_bytes(
        assigned_requirements_projection_text(evaluation, requirement_ids).encode("utf-8")
    )


def derive_llm_call_budget(
    primary: dict[str, Any],
    languages: list[str],
    evaluation: str,
    raw: dict[str, Any],
) -> int:
    """Derive the maximum scored-model call budget from the frozen configuration only."""
    formula = raw.get("llm_budget_formula")
    if not formula:
        return int(raw.get("max_llm_calls", 0) or 0)

    if formula == "learnability_conditions":
        cfg = primary["llm_learnability"]
        count_keys = {
            "condition.i1_keyword_anonymization": "keyword_anonymization_seeds",
            "condition.i2_vocabulary_anonymization": "vocabulary_anonymization_seeds",
            "condition.i3_structural_surface_perturbation": "structural_surface_transformation_sets",
            "condition.i4_novel_rule_generalization": "novel_rule_generalization_trials_per_language",
            "condition.i5_held_out_rule_composition": "held_out_rule_composition_trials_per_language",
            "condition.i6_prior_conflict_resistance": "prior_conflict_resistance_trials_per_language",
        }
        cells_per_language = 0
        for rid in raw.get("requirement_ids", []):
            key = count_keys.get(str(rid))
            if key:
                cells_per_language += int(cfg[key])
        repair_turns = int(cfg["max_repair_turns"])
        return len(languages) * cells_per_language * (1 + repair_turns)

    if formula == "proficiency_primary_cells":
        cfg = primary["llm_proficiency"]
        workloads = list(cfg["primary_workloads"])
        scenarios = list(cfg["primary_scenarios"])
        trials = int(cfg["independent_trials_per_replicated_cell"])
        repair_turns = int(cfg["max_repair_turns"])
        return (
            len(languages)
            * len(workloads)
            * len(scenarios)
            * trials
            * (1 + repair_turns)
        )

    raise BenchmarkError(
        f"{evaluation}: unknown llm_budget_formula {formula!r}"
    )


def planned_read_paths(
    root: Path,
    raw_paths: list[str],
    assigned_languages: list[str],
    *,
    language_scoped_program_reads: bool = False,
) -> list[str]:
    """Resolve task reads, optionally narrowing program trees to one language."""
    target = str(
        load_benchmark_metadata(root / "template").get(
            "evaluated_target_language", "Quidra"
        )
    )
    comparison_only = bool(assigned_languages) and target not in assigned_languages
    target_only = assigned_languages == [target]
    expanded: list[str] = []
    catalog: dict[str, Any] | None = None

    for value in raw_paths:
        if comparison_only and value == "repo/docs":
            continue
        if (
            language_scoped_program_reads
            and comparison_only
            and value == "repo/tests/benchmark/quidra"
        ):
            continue
        if (
            language_scoped_program_reads
            and target_only
            and value == "template/programs"
        ):
            continue
        if comparison_only and value == "template/programs":
            if catalog is None:
                catalog = json_load(root / "template" / "reuse" / "catalog.json")
                validate_reuse_catalog(catalog)
            matches = sorted({
                "template/" + str(artifact["destination"])
                for artifact in catalog.get("artifacts", [])
                if artifact.get("language") in assigned_languages
                and str(artifact.get("destination", "")).startswith("programs/")
            })
            if not matches:
                raise BenchmarkError(
                    "no reusable comparison-language programs found for: "
                    + ", ".join(assigned_languages)
                )
            expanded.extend(matches)
            continue
        expanded.append(value)

    resolved: list[str] = []
    seen: set[str] = set()
    for value in expanded:
        path = root / value
        if not path.exists():
            continue
        canonical = str(require_under(path, root))
        if canonical not in seen:
            resolved.append(canonical)
            seen.add(canonical)
    return resolved


def cmd_deterministic_plan(args: argparse.Namespace) -> int:
    root = workspace(args)
    assert_template_integrity(root)
    require_privacy_pass(root)
    templates = load_work_plan_templates(root)
    primary = json_load(root / "template" / "config" / "primary.json")
    default_max_attempts = runner_max_attempts(root)
    plan_root = root / "work" / "root" / "plans"
    if (root / "work" / "root" / "manifest.json").exists():
        raise BenchmarkError("manifest already frozen; deterministic plan cannot change this run")
    plan_root.mkdir(parents=True, exist_ok=True)

    reuse_status_path = root / "results" / "reuse_status.json"
    reuse_status = json_load(reuse_status_path) if reuse_status_path.is_file() else {"audit_required": []}
    materialized_path = root / "template" / "reuse" / "materialized.json"
    materialized = json_load(materialized_path) if materialized_path.is_file() else {"artifacts": []}
    artifacts = {str(a["id"]): a for a in materialized.get("artifacts", [])}
    audits_by_eval: dict[str, list[str]] = {name: [] for name in PRIMARY_NAMES}
    for artifact_id in reuse_status.get("audit_required", []):
        artifact = artifacts.get(str(artifact_id))
        if not artifact:
            raise BenchmarkError(f"reuse-status names unknown artifact: {artifact_id}")
        dest = str(artifact.get("content_destination") or artifact.get("destination") or "")
        if "semantic_compression" in dest:
            audits_by_eval["semantic_compression"].append(str(artifact_id))
        else:
            audits_by_eval["language_quality"].append(str(artifact_id))

    written = []
    for evaluation in PRIMARY_NAMES:
        spec = templates["evaluations"].get(evaluation)
        if not isinstance(spec, dict):
            raise BenchmarkError(f"work plan template missing evaluation: {evaluation}")
        units: list[dict[str, Any]] = []
        audit_ids: list[str] = []
        audit_artifact_by_unit: dict[str, dict[str, Any]] = {}
        for artifact_id in audits_by_eval[evaluation]:
            artifact = artifacts[artifact_id]
            uid = f"audit-{artifact_id}"
            agent_id = f"worker-{uid}"
            audit_ids.append(uid)
            audit_artifact_by_unit[uid] = artifact
            language = str(artifact.get("language") or "")
            status_row = next(
                (
                    row for row in (reuse_status.get("artifacts") or [])
                    if str(row.get("id") or "") == artifact_id
                ),
                {},
            )
            audit_input = plan_root / "reuse-audit-inputs" / f"{slug_id(artifact_id)}.json"
            json_dump(audit_input, {
                "schema_version": 1,
                "artifact_id": artifact_id,
                "language": language or None,
                "validated_toolchain": status_row.get("validated_toolchain"),
                "validated_canonical": status_row.get("validated_canonical"),
                "current_canonical": status_row.get("current_canonical"),
                "reason": status_row.get("reason"),
            })
            units.append({
                "id": uid,
                "evaluation": evaluation,
                "phase": "readiness",
                "execution_kind": "agent",
                "worker_mode": "packet-only",
                "result_kind": "audit",
                "goal": (
                    f"Audit reusable artifact {artifact_id} against the current toolchain/capability "
                    "contract. The attached audit input is the authoritative old/current toolchain "
                    "comparison. Confirm whether the source/harness remains semantically valid; do "
                    "not reuse old measurements."
                ),
                "assigned_agent_id": agent_id,
                "assigned_languages": [language] if language else [],
                "dependencies": [],
                "input_hashes": {"artifact_git_object": artifact.get("content_git_object_sha1")},
                "reuse_audit_for": [artifact_id],
                "requirement_ids": [],
                "read_paths": [
                    str(root / "template" / str(artifact["content_destination"])),
                    str(audit_input),
                ],
                "evidence_paths": [str(root / "work" / "agents" / agent_id / "result.json")],
                "validator_command": (
                    f"python3 {root / 'template' / 'scripts' / 'benchmark.py'} "
                    f"result-check --workspace {root} --id {agent_id}"
                ),
                "network_allowed": True,
                "prompt_sections": [],
                "max_attempts": default_max_attempts,
                "max_llm_calls": 1,
                "estimated_input_tokens_per_call": 4096,
                "max_output_tokens_per_call": 4096,
            })

        regular_ids: list[str] = []
        split_modes = {
            str(raw["id"]): (
                "probe_language"
                if bool(raw.get("split_by_probe_language", False))
                else (
                    "language"
                    if bool(raw.get("split_by_language", False))
                    else str(raw.get("split_mode") or "")
                )
            )
            for raw in spec.get("units", [])
            if (
                bool(raw.get("split_by_probe_language", False))
                or bool(raw.get("split_by_language", False))
                or raw.get("split_mode")
            )
        }
        fixed_languages = metadata_languages(root)
        for raw in spec.get("units", []):
            base_uid = str(raw["id"])
            execution_kind = str(raw.get("execution_kind", "agent"))
            result_kind = str(raw.get("result_kind", "requirements"))
            split_mode = (
                "probe_language"
                if bool(raw.get("split_by_probe_language", False))
                else (
                    "language"
                    if bool(raw.get("split_by_language", False))
                    else str(raw.get("split_mode") or "")
                )
            )
            if split_mode == "probe_language":
                shards: list[tuple[list[str], str | None]] = [
                    ([language], probe_id)
                    for language in fixed_languages
                    for probe_id in semantic_probe_ids(root)
                ]
            elif split_mode == "language":
                shards = [([language], None) for language in fixed_languages]
            elif split_mode == "target_vs_comparison":
                target = str(
                    load_benchmark_metadata(root / "template").get(
                        "evaluated_target_language", "Quidra"
                    )
                )
                comparison = [language for language in fixed_languages if language != target]
                shards = [([target], None), (comparison, None)]
            elif split_mode:
                raise BenchmarkError(
                    f"{base_uid}: unsupported split_mode {split_mode!r}"
                )
            else:
                shards = [([], None)]
            for assigned_languages, canonical_probe_id in shards:
                suffix = ""
                if canonical_probe_id is not None:
                    suffix = (
                        f"--{slug_id(assigned_languages[0])}--{slug_id(canonical_probe_id)}"
                    )
                elif assigned_languages:
                    suffix = (
                        f"--{slug_id(assigned_languages[0])}"
                        if len(assigned_languages) == 1
                        else "--comparison"
                    )
                uid = base_uid + suffix
                agent_id = (
                    f"worker-{uid}" if execution_kind == "agent" else f"system-{uid}"
                )
                task_read_paths = planned_read_paths(
                    root,
                    [str(value) for value in raw.get("read_paths", [])],
                    assigned_languages,
                    language_scoped_program_reads=bool(
                        raw.get("language_scoped_program_reads", False)
                    ),
                )
                if canonical_probe_id is not None:
                    if len(assigned_languages) != 1:
                        raise BenchmarkError(
                            f"{uid}: canonical probe owner must have one language"
                        )
                    probe_contract = materialize_semantic_probe_contract(
                        root, canonical_probe_id, assigned_languages[0]
                    )
                    task_read_paths.append(str(probe_contract))
                    if canonical_probe_id == "F20.P1":
                        runtime_facts = (
                            root / "work" / "root" / "f20_runtime_facts.json"
                        )
                        if runtime_facts.is_file():
                            task_read_paths.append(str(runtime_facts))
                        elif lexical_absolute(root) == lexical_absolute(
                            CANONICAL_WORKSPACE
                        ):
                            raise BenchmarkError(
                                "F20.P1 requires frozen runtime facts"
                            )
                deps: list[str] = []
                for dep in [str(x) for x in raw.get("dependencies", [])]:
                    dep_mode = split_modes.get(dep, "")
                    if dep_mode == "language":
                        if assigned_languages:
                            deps.extend(
                                dep + f"--{slug_id(language)}"
                                for language in assigned_languages
                            )
                        else:
                            deps.extend(
                                dep + f"--{slug_id(language)}"
                                for language in fixed_languages
                            )
                    elif dep_mode == "probe_language":
                        dep_languages = assigned_languages or fixed_languages
                        deps.extend(
                            dep + f"--{slug_id(language)}--{slug_id(probe_id)}"
                            for language in dep_languages
                            for probe_id in semantic_probe_ids(root)
                        )
                    elif dep_mode == "target_vs_comparison":
                        target = str(
                            load_benchmark_metadata(root / "template").get(
                                "evaluated_target_language", "Quidra"
                            )
                        )
                        if assigned_languages == [target]:
                            deps.append(dep + f"--{slug_id(target)}")
                        elif assigned_languages and target not in assigned_languages:
                            deps.append(dep + "--comparison")
                        else:
                            deps.extend([
                                dep + f"--{slug_id(target)}",
                                dep + "--comparison",
                            ])
                    else:
                        deps.append(dep)

                target_language = str(
                    load_benchmark_metadata(root / "template").get(
                        "evaluated_target_language", "Quidra"
                    )
                )
                target_only_dependencies = raw.get("target_only_dependencies", [])
                if not isinstance(target_only_dependencies, list) or not all(
                    isinstance(dep, str) for dep in target_only_dependencies
                ):
                    raise BenchmarkError(
                        f"{base_uid}: target_only_dependencies must be a string array"
                    )
                if assigned_languages == [target_language]:
                    deps.extend(target_only_dependencies)

                if audit_ids:
                    raw_reads = [
                        lexical_absolute(Path(path))
                        for path in task_read_paths
                    ]
                    relevant_audits: list[str] = []
                    for audit_uid, audit_artifact in audit_artifact_by_unit.items():
                        dest = lexical_absolute(
                            root / "template" / str(
                                audit_artifact.get(
                                    "content_destination",
                                    audit_artifact.get("destination", ""),
                                )
                            )
                        )
                        for read_root in raw_reads:
                            overlaps = False
                            try:
                                dest.relative_to(read_root)
                                overlaps = True
                            except ValueError:
                                try:
                                    read_root.relative_to(dest)
                                    overlaps = True
                                except ValueError:
                                    pass
                            if overlaps:
                                relevant_audits.append(audit_uid)
                                break
                    deps = sorted(set(deps + relevant_audits))
                regular_ids.append(uid)

                unit_requirement_ids = list(raw.get("requirement_ids", []))
                if canonical_probe_id is not None:
                    unit_requirement_ids = [
                        CANONICAL_FRAGMENT_PREFIX + slug_id(canonical_probe_id)
                    ]

                total_calls = (
                    derive_llm_call_budget(primary, fixed_languages, evaluation, raw)
                    if execution_kind == "agent"
                    else 0
                )
                max_calls = total_calls
                if assigned_languages and total_calls:
                    max_calls = (
                        total_calls * len(assigned_languages)
                        + len(fixed_languages) - 1
                    ) // len(fixed_languages)
                language_text = (
                    " Assigned language set: " + ", ".join(assigned_languages) + "."
                    if assigned_languages
                    else " Evaluate the fixed comparison set symmetrically."
                )
                default_goal = (
                    f"Produce validated requirement-level evidence for {evaluation}: "
                    + ", ".join(unit_requirement_ids)
                    + "."
                    + language_text
                    + " Write result.json."
                )
                goal = str(raw.get("goal") or default_goal)

                if execution_kind == "agent":
                    evidence_paths = [
                        str(root / "work" / "agents" / agent_id / "result.json")
                    ]
                    validator_action = raw.get("validator_action")
                    if validator_action is None:
                        validator_command = (
                            f"python3 {root / 'template' / 'scripts' / 'benchmark.py'} "
                            f"result-check --workspace {root} --id {agent_id}"
                        )
                    else:
                        raise BenchmarkError(
                            f"{uid}: unknown validator_action {validator_action!r}"
                        )
                else:
                    evidence_paths = [
                        str(root / "work" / "root" / "commands" / uid / "result.json")
                    ]
                    validator_command = (
                        f"python3 {root / 'template' / 'scripts' / 'benchmark.py'} "
                        f"command-result-check --workspace {root} --id {uid}"
                    )

                units.append({
                    "id": uid,
                    "evaluation": evaluation,
                    "phase": raw["phase"],
                    "execution_kind": execution_kind,
                    "worker_mode": (
                        str(raw.get("worker_mode") or "packet-only")
                        if execution_kind == "agent"
                        else "runner-command"
                    ),
                    "result_kind": result_kind,
                    "required_for_complete": bool(
                        raw.get("required_for_complete", True)
                    ),
                    "runner_action": raw.get("runner_action"),
                    "goal": goal,
                    "assigned_agent_id": agent_id,
                    "assigned_languages": assigned_languages,
                    "dependencies": deps,
                    "input_hashes": {
                        "primary_config": primary_config_projection_sha256(
                            root, evaluation
                        ),
                        "benchmark_metadata": sha256_file(
                            root / "template" / BENCHMARK_METADATA_RELATIVE
                        ),
                        "evaluation_spec_sections": evaluation_spec_projection_sha256(
                            root,
                            evaluation,
                            list(raw.get("prompt_sections", [])),
                        ),
                    },
                    "reuse_audit_for": [],
                    "requirement_ids": unit_requirement_ids,
                    "workload_ids": list(raw.get("workload_ids", [])),
                    "read_paths": task_read_paths,
                    "evidence_paths": evidence_paths,
                    "validator_command": validator_command,
                    "network_allowed": bool(raw.get("network_allowed", False)),
                    "packet_layout": str(raw.get("packet_layout") or "task-first"),
                    "prompt_sections": list(raw.get("prompt_sections", [])),
                    "max_attempts": int(
                        raw.get("max_attempts", default_max_attempts)
                    ),
                    "max_llm_calls": max_calls,
                    "llm_budget_formula": raw.get("llm_budget_formula"),
                    "estimated_input_tokens_per_call": int(
                        raw.get("estimated_input_tokens_per_call", 0) or 0
                    ),
                    "max_output_tokens_per_call": int(
                        raw.get("max_output_tokens_per_call", 0) or 0
                    ),
                    "canonical_fragment_owner": bool(
                        raw.get("canonical_fragment_owner", False)
                    ),
                    "canonical_probe_id": canonical_probe_id,
                    "canonical_fragment_source_requirement": raw.get(
                        "canonical_fragment_source_requirement"
                    ),
                })

        aggregate_id = f"{evaluation}-aggregate"
        result_path = root / "results" / "evaluations" / f"{evaluation}.json"
        units.append({
            "id": aggregate_id,
            "evaluation": evaluation,
            "phase": "aggregation",
            "execution_kind": "command",
            "worker_mode": "runner-command",
            "result_kind": "aggregate",
            "required_for_complete": True,
            "runner_action": "aggregate-primary",
            "goal": f"Mechanically aggregate {evaluation} and generate its language ranking.",
            "assigned_agent_id": f"system-{aggregate_id}",
            "dependencies": sorted(set(regular_ids + audit_ids)),
            "input_hashes": {
                "aggregation_config": sha256_file(
                    root / "template" / "config" / "aggregation.json"
                )
            },
            "reuse_audit_for": [],
            "requirement_ids": [],
            "read_paths": [],
            "evidence_paths": [str(result_path)],
            "validator_command": (
                f"python3 {root / 'template' / 'scripts' / 'benchmark.py'} "
                f"aggregate-check --workspace {root} --evaluation {evaluation} --file {result_path}"
            ),
            "network_allowed": False,
            "prompt_sections": [],
            "max_attempts": 1,
            "max_llm_calls": 0,
            "estimated_input_tokens_per_call": 0,
            "max_output_tokens_per_call": 0,
        })

        plan = {"schema_version": 1, "evaluation": evaluation, "work_units": units}
        validate_work_plan_data(root, evaluation, plan)
        path = plan_root / f"{evaluation}.json"
        json_dump(path, plan)
        written.append({"evaluation": evaluation, "path": str(path), "work_units": len(units)})

    payload = {"ok": True, "plans": written}
    json_dump(root / "results" / "deterministic_plan.json", payload)
    print(json.dumps(payload, indent=2))
    return 0



COMPARABILITY_GATE = "gate.comparability_audit"
COMPARABILITY_SAMPLE_RELATIVE = "work/audit/semantic-compression/comparability_sample.json"
COMPARABILITY_BLINDING_RELATIVE = "work/root/comparability_blinding.json"
COMPARABILITY_REPAIR_RELATIVE = "work/audit/semantic-compression/comparability_repairs.json"
COMPARABILITY_POLICY_RELATIVE = "template/config/semantic_compression_comparability.json"
COMPARABILITY_TEXT_LIMIT = 300
# Measured on the first full run: the audit's other inputs (the snapshot docs
# and the frozen Semantic Compression assets) embed 707,897 of the 1,048,576
# bytes a packet-only Task Packet may carry, and the sample of all twenty
# families costs 237,000 of the remaining 340,679. The ceiling here is a sanity
# bound on the sample itself; the packet's own limit still guards the total.
COMPARABILITY_SAMPLE_BUDGET = 280_000

COMPARABILITY_AUDIT_INSTRUCTIONS = """

The blinded comparability sample for this audit is embedded as a task input.
Methodology 6.1.1A requires the audit to judge the language-specific
annotations against the frozen matrix. Entries use opaque per-run labels.

When an entry carries `support_adjudication`, that object is the sole
authoritative support record for FULL/PARTIAL/NONE, the selected fragment,
P-letter or N-reason, justification and citation. Metric evidence is grouped
under `metric_annotations` by the exact source work-unit ID. Never splice a
fragment, note, rationale or other field from one source into another source's
annotation, and never treat shard support prose as a second vote.

If the sample is mutually comparable, return gate.comparability_audit=true.
If it is not, return false and identify every affected pair under
`evidence.gate_result.affected_pairs_requiring_revalidation` as objects with
`probe_id` and opaque `label`.

For every affected pair that can be resolved from the frozen evidence, also put
a complete replacement under `evidence.repair_directives`:
{"probe_id":"FNN.PN","label":"A","record":{"level":"FULL|PARTIAL|NONE",
"fragment":"exact code or null","partial_reasons":["P-a"],"none_reason":null,
"justification":"...","citation":"..."}}
FULL and PARTIAL require the exact selected fragment. PARTIAL requires one or
more P-a..P-e reasons. NONE requires fragment=null and exactly one N-1..N-4
reason. Justification and citation are always required.

The runner may apply only valid pair-specific directives, rebuild the identical
predeclared blinded sample, and rerun this audit. This repair loop never sees
aggregate scores or rankings and is capped, so it cannot be used to tune the
outcome. If evidence is insufficient to author a justified repair, report the
affected pair without inventing one; that unresolved disagreement will remain
a scientific blocker."""




def comparability_sample_probes(root: Path) -> list[dict[str, Any]]:
    """The predeclared audit sample: fixed by a rule, never by observed scores.

    Methodology 6.1.1A wants at least 20% of the frozen probes and every
    capability family. Taking the first frozen probe of each family satisfies
    both from the matrix alone, so the sample is decided before any annotation
    exists and cannot be steered by what the run measured.
    """
    matrix = json_load(
        root / "template" / "methodology-assets" / "semantic_compression"
        / "semantic_site_matrix.json"
    )
    probes = list(matrix.get("probes") or [])
    if not probes:
        raise BenchmarkError("frozen semantic-site matrix declares no probes")
    chosen: list[dict[str, Any]] = []
    families: set[str] = set()
    for probe in probes:
        family = str(probe.get("family") or "")
        if family and family not in families:
            families.add(family)
            chosen.append(probe)
    minimum = -(-len(probes) // 5)
    for probe in probes:
        if len(chosen) >= minimum:
            break
        if probe not in chosen:
            chosen.append(probe)
    return chosen


# Every pattern is replaced by the entry's own label, so one flat list serves
# all ten languages: a pattern matching the "wrong" language's name still
# redacts to the same token. Words that are also ordinary English - "go",
# "swift", "rust" - are matched case-sensitively or only in compound forms, so
# prose survives. Lower-case package roots (java.lang, kotlin.io) leaked in the
# first full run and are why the alphabetic patterns are case-insensitive.
COMPARABILITY_LANGUAGE_PATTERNS = (
    r"(?i)\bjava\w*", r"(?i)\bkotlin\w*", r"(?i)\bpython\w*", r"(?i)\bcpython\b",
    r"(?i)\bquidra\w*", r"(?i)\brust\w*", r"(?i)\bswiftc?\b", r"(?i)\btypescript\b",
    r"(?i)\bjavascript\b", r"(?i)\becmascript\b", r"(?i)\bzig\w*",
    r"(?i)\bgolang\b", r"(?i)\bgo(routine|fmt)\w*", r"(?i)\bclang\b",
    r"(?i)\bcpp\b", r"(?i)\bpep\s*\d+", r"C\+\+", r"g\+\+", r"libstdc\+\+",
    r"std::", r"\bGo\b", r"\bC\b", r"\bC\d\d\b", r"\bTS\b", r"\bJS\b",
    r"\bNode(\.js)?\b", r"\bJVM\b", r"\bgcc\b", r"\bglibc\b", r"\bpip\b",
    r"\bcargo\b", r"\bClippy\b",
)


def redact_language_identity(value: Any, label: str) -> Any:
    """Replace language names in an annotation with the entry's label.

    Blinding an annotation is worth nothing while its own notes say which
    language wrote it, and the evidence names languages in its keys as well as
    its prose. Code fragments are left verbatim - redacting inside them would
    corrupt the thing being judged - so the sample says plainly that a fragment
    can still betray its language and that the audit must be decided against
    the frozen matrix either way.
    """
    if isinstance(value, str):
        redacted = value
        for pattern in COMPARABILITY_LANGUAGE_PATTERNS:
            redacted = re.sub(pattern, f"<{label}>", redacted)
        return redacted
    if isinstance(value, dict):
        return {
            redact_language_identity(key, label): redact_language_identity(item, label)
            for key, item in value.items()
        }
    if isinstance(value, list):
        return [redact_language_identity(item, label) for item in value]
    return value


def redact_language_identity_in_key(key: str, label: str) -> str:
    """Redact a language name an evidence key spells as one of its segments.

    The prose patterns are anchored on `\b`, and `_` is a word character, so
    `per_probe_mean_lookups_zig` sails through a redaction that catches the same
    name in a sentence. A key names the language in its own segment, so redact
    segment by segment: over-redacting a key costs nothing, while one surviving
    suffix identifies an entry in every probe it appears in.
    """
    segments = str(key).split("_")
    return "_".join(
        str(redact_language_identity(segment, label)) for segment in segments
    )


def blind_annotation(fields: dict[str, Any], label: str) -> dict[str, Any]:
    """Clip an annotation's prose and strip the language identity out of it."""
    blinded: dict[str, Any] = {}
    for key, value in fields.items():
        clipped = clip_annotation_text(value)
        name = redact_language_identity_in_key(key, label)
        # Only the verbatim fragment is exempt: redacting inside code would
        # corrupt the thing being judged. Its prose companions - a note, a
        # summary - are sentences about the fragment, and exempting every key
        # that merely contains "fragment" let them name the language outright.
        verbatim = key == "fragment" or key.endswith("_fragment")
        blinded[name] = (
            clipped if verbatim else redact_language_identity(clipped, label)
        )
    return blinded


def clip_annotation_text(value: Any, limit: int | None = None) -> Any:
    """Keep an annotation's free text long enough to judge and short enough to embed."""
    if limit is None:
        limit = COMPARABILITY_TEXT_LIMIT
    if isinstance(value, str):
        return value if len(value) <= limit else value[:limit] + " ...[clipped]"
    if isinstance(value, dict):
        return {key: clip_annotation_text(item, limit) for key, item in value.items()}
    if isinstance(value, list):
        return [clip_annotation_text(item, limit) for item in value]
    return value


def merge_annotation_fields_strict(
    target: dict[str, Any],
    incoming: dict[str, Any],
    *,
    context: str,
) -> None:
    """Merge annotation fields only when duplicate values are identical.

    Semantic Compression is assembled from several metric shards.  A plain
    dict.update() made the last shard silently win when two shards emitted the
    same field with different values, so one sampled row could become a
    synthetic combination that no worker actually wrote.  Equal duplicates are
    harmless; unequal duplicates are a measurement-integrity error and must be
    repaired at the producing shard instead of hidden by the runner.
    """
    for key, value in incoming.items():
        if key in target:
            if target[key] != value:
                raise BenchmarkError(
                    f"{context}: conflicting annotation field {key!r} across "
                    "completed shards"
                )
            continue
        target[key] = value


def sc_add_annotation_source(
    target: dict[str, Any], source_id: str, fields: dict[str, Any]
) -> None:
    """Preserve one shard as one annotation instead of splicing shard fields."""
    sources = target.setdefault("metric_annotations", {})
    if not isinstance(sources, dict):
        raise BenchmarkError("Semantic Compression metric_annotations must be an object")
    clipped = clip_annotation_text(fields)
    previous = sources.get(source_id)
    if previous is not None and previous != clipped:
        raise BenchmarkError(
            f"Semantic Compression source {source_id!r} changed while one audit "
            "packet was being assembled"
        )
    sources[source_id] = clipped


def probe_annotation_fields(
    result: dict[str, Any], wanted: set[str]
) -> dict[str, dict[str, Any]]:
    """Collect every per-probe annotation a metric shard recorded, by probe ID.

    The shards report per-probe evidence in whatever shape their metric needs -
    a list of objects carrying `probe_id`, or a mapping keyed by probe ID - so
    this reads both and names each value after the evidence key it came from.
    """
    rows: dict[str, dict[str, Any]] = {probe: {} for probe in wanted}

    def visit(node: Any, name: str) -> None:
        if isinstance(node, dict):
            for key, value in node.items():
                if key in rows:
                    merge_annotation_fields_strict(
                        rows[key], {name: value}, context=f"probe {key}"
                    )
                else:
                    visit(value, key)
        elif isinstance(node, list):
            for item in node:
                probe_key = None
                if isinstance(item, dict):
                    probe_key = item.get("probe_id", item.get("probe"))
                if probe_key is not None and str(probe_key) in rows:
                    probe_id = str(probe_key)
                    row = rows[probe_id]
                    merge_annotation_fields_strict(
                        row,
                        {
                            key: value
                            for key, value in item.items()
                            if key not in {"probe_id", "probe"}
                        },
                        context=f"probe {probe_id}",
                    )
                else:
                    visit(item, name)

    visit(result.get("evidence") or {}, "evidence")
    return rows


def comparability_blinding(run_id: str, languages: list[str]) -> dict[str, str]:
    """Label the languages so the reviewer cannot tell which one wrote an entry."""
    order = sorted(
        languages,
        key=lambda language: sha256_bytes(f"{run_id}\0{language}".encode("utf-8")),
    )
    return {language: chr(ord("A") + index) for index, language in enumerate(order)}


SC_SUPPORT_FIELD_HINTS = (
    "support", "awarded", "points", "p_letter", "letters", "n_reason",
    "justification", "rationale", "citation", "basis", "note",
)
SC_P_LETTERS = {"P-a", "P-b", "P-c", "P-d", "P-e"}
SC_N_REASONS = {"N-1", "N-2", "N-3", "N-4"}


def sc_support_is_level(value: Any) -> str | None:
    """The FULL/PARTIAL/NONE level a field holds, if it holds one plainly."""
    if isinstance(value, str) and value.strip().upper() in ("FULL", "PARTIAL", "NONE"):
        return value.strip().upper()
    return None


def sc_owner_support_levels(row: dict[str, Any], fields: list[str]) -> set[str]:
    """Every level the owning shard stated for one probe, however it nested it."""
    found: set[str] = set()

    def visit(node: Any, key: str, depth: int) -> None:
        if depth > 4:
            return
        if isinstance(node, dict):
            for name, value in node.items():
                visit(value, str(name), depth + 1)
        elif isinstance(node, list):
            for item in node:
                visit(item, key, depth + 1)
        elif any(key == name or key.endswith("_" + name) for name in fields):
            level = sc_support_is_level(node)
            if level:
                found.add(level)

    visit(row, "", 0)
    return found


def sc_adjudicated_level(value: Any) -> str | None:
    """The level an adjudication states for one language, however it wraps it."""
    level = sc_support_is_level(value)
    if level is not None:
        return level
    if isinstance(value, dict):
        found = sc_owner_support_levels(value, ["support", "support_level", "level"])
        if len(found) == 1:
            return found.pop()
    return None


SC_PARTIAL_REASONS = {"P-a", "P-b", "P-c", "P-d", "P-e"}
SC_NONE_REASONS = {"N-1", "N-2", "N-3", "N-4"}


def sc_adjudicated_record(value: Any) -> dict[str, Any] | None:
    """Normalize one authoritative support adjudication record.

    Bare levels were intentionally accepted in early runs, but that discarded
    the P-letter/N-reason, citation and the exact fragment. The comparability
    audit then had to read stale shard prose and could reject a correction that
    had already been made. New adjudications are complete records and therefore
    become the sole support source for their (probe, language) pair.
    """
    if not isinstance(value, dict):
        return None
    level = sc_adjudicated_level(value)
    if level is None:
        return None
    fragment = value.get("fragment")
    if fragment is not None:
        fragment = str(fragment).strip() or None
    raw_partial = (
        value.get("partial_reasons")
        if "partial_reasons" in value
        else value.get("applicable_letters", value.get("letters", []))
    )
    if raw_partial is None:
        raw_partial = []
    if not isinstance(raw_partial, list):
        return None
    partial = [str(item).strip() for item in raw_partial if str(item).strip()]
    none_reason = value.get("none_reason", value.get("n_reason"))
    if none_reason is not None:
        none_reason = str(none_reason).strip() or None
    justification = str(value.get("justification", value.get("rationale", ""))).strip()
    citation = str(value.get("citation", "")).strip()
    if not justification or not citation:
        return None
    if level == "FULL":
        if fragment is None or partial or none_reason is not None:
            return None
    elif level == "PARTIAL":
        if fragment is None or not partial or any(code not in SC_PARTIAL_REASONS for code in partial):
            return None
        if none_reason is not None:
            return None
    else:
        if fragment is not None or partial or none_reason not in SC_NONE_REASONS:
            return None
    return {
        "level": level,
        "fragment": fragment,
        "partial_reasons": partial,
        "none_reason": none_reason,
        "justification": justification,
        "citation": citation,
    }


def sc_p_a_allowed_probes(root: Path) -> set[str]:
    """Machine-readable R9 scope for the P-a weaker-substitution reason."""
    asset = json_load(
        root / "template" / "methodology-assets" / "semantic_compression"
        / "capability_universe.json"
    )
    raw = (asset.get("support_rubric") or {}).get("partial_p_a_allowed_probe_ids")
    if not isinstance(raw, list) or not raw or not all(isinstance(x, str) for x in raw):
        raise BenchmarkError(
            "capability_universe support_rubric must declare "
            "partial_p_a_allowed_probe_ids"
        )
    allowed = {str(value) for value in raw}
    if any(not re.fullmatch(r"F\d{2}\.P\d+", value) for value in allowed):
        raise BenchmarkError("partial_p_a_allowed_probe_ids contains an invalid probe ID")
    return allowed


def validate_sc_record_for_probe(
    root: Path, probe_id: str, record: dict[str, Any], *, context: str
) -> dict[str, Any]:
    """Enforce support rules that depend on the identity of the frozen probe."""
    if (
        record.get("level") == "PARTIAL"
        and "P-a" in (record.get("partial_reasons") or [])
        and probe_id not in sc_p_a_allowed_probes(root)
    ):
        raise BenchmarkError(
            f"{context}: {probe_id} cannot cite P-a; R9 permits weaker "
            "substitution only for the frozen partial_p_a_allowed_probe_ids"
        )
    return record


SC_NONAUTHORITATIVE_METRIC_SUPPORT_KEYS = {
    "fragment", "selected_fragment", "source_fragment",
    "support", "support_level", "support_factor",
    "awarded", "awarded_points",
    "p_letter", "p_letters", "partial_reasons", "applicable_letters", "letters",
    "n_reason", "none_reason",
}


def sc_metric_only_annotation(fields: dict[str, Any]) -> dict[str, Any]:
    """Remove duplicate support authority from a metric-shard annotation.

    The blinded comparability sample carries one canonical support record at the
    probe/language level. Metric shards remain useful for density/determinacy/
    locality/hidden-cost evidence, but their copied support level, fragment or
    P/N reason is stale by definition after cohort adjudication and must not
    become a second vote visible to the auditor.
    """
    cleaned: dict[str, Any] = {}
    for key, value in fields.items():
        lowered = str(key).lower()
        support_field = (
            lowered in SC_NONAUTHORITATIVE_METRIC_SUPPORT_KEYS
            or (lowered == "level" and sc_support_is_level(value) is not None)
            or lowered.startswith("support_")
            or lowered.endswith("_support")
            or "p_letter" in lowered
            or "n_reason" in lowered
        )
        if not support_field:
            cleaned[key] = value
    return cleaned


def sc_reconcile_support(
    by_language: dict[str, dict[str, dict[str, Any]]],
    owner_rows: dict[str, dict[str, dict[str, Any]]],
    owner: dict[str, Any],
    adjudicated: dict[str, dict[str, dict[str, Any]]] | None = None,
) -> list[dict[str, Any]]:
    """Give every (language, probe) one authoritative support statement."""
    fields = [str(name) for name in (owner.get("fields") or ["support"])]
    levels = {
        str(name).upper(): float(factor)
        for name, factor in (owner.get("levels") or {}).items()
    }
    adjudicated = adjudicated or {}
    replaced: list[dict[str, Any]] = []
    stale_names = {
        "fragment", "selected_fragment", "source_fragment", "reason", "rationale",
        "justification", "citation", "notes", "n_reason", "none_reason",
        "applicable_letters", "letters", "p_letter", "partial_reasons",
    }
    for language in sorted(by_language):
        for probe_id, row in sorted(by_language[language].items()):
            canonical = (adjudicated.get(probe_id) or {}).get(language)
            owner_row = (owner_rows.get(language) or {}).get(probe_id) or {}
            candidates = (
                {canonical["level"]}
                if canonical is not None
                else sc_owner_support_levels(owner_row, fields)
            )
            dropped = {
                key: value
                for key, value in row.items()
                if any(hint in str(key).lower() for hint in SC_SUPPORT_FIELD_HINTS)
                or (canonical is not None and str(key).lower() in stale_names)
            }
            level = candidates.pop() if len(candidates) == 1 else None
            for key in dropped:
                row.pop(key, None)
            if level is None:
                row["support"] = "UNRECONCILED"
                row["support_note"] = (
                    "The owning Capability Coverage annotation did not state one "
                    "determinate level for this probe. Judge it as unstated."
                )
            else:
                row["support"] = level
                row["support_factor"] = levels.get(level, 0.0)
                if canonical is not None:
                    row["fragment"] = canonical["fragment"]
                    row["support_reason_codes"] = (
                        canonical["partial_reasons"]
                        if level == "PARTIAL"
                        else ([canonical["none_reason"]] if level == "NONE" else [])
                    )
                    row["support_adjudication"] = dict(canonical)
                    row["support_source"] = "cohort_adjudication_or_repair"
            if dropped or level is None or canonical is not None:
                replaced.append({
                    "language": language,
                    "probe_id": probe_id,
                    "reconciled_to": row["support"],
                    "authoritative_adjudication": canonical,
                    "replaced_fields": {
                        key: value for key, value in sorted(dropped.items())
                    },
                })
    return replaced


SUPPORT_ADJUDICATION_PREFIX = "annotation.support_adjudication--"
CANONICAL_FRAGMENT_PREFIX = "annotation.canonical_fragment--"


def canonical_fragment_probe(requirement_ids: list[str]) -> str | None:
    """The exact frozen probe owned by one probe×language fragment unit."""
    for rid in requirement_ids:
        if str(rid).startswith(CANONICAL_FRAGMENT_PREFIX):
            slug = str(rid)[len(CANONICAL_FRAGMENT_PREFIX):]
            head, _, tail = slug.partition("-")
            if head and tail:
                return f"{head.upper()}.{tail.upper()}"
    return None


def support_adjudication_probe(requirement_ids: list[str]) -> str | None:
    """The frozen probe a support-adjudication unit answers for, if it is one."""
    for rid in requirement_ids:
        if str(rid).startswith(SUPPORT_ADJUDICATION_PREFIX):
            slug = str(rid)[len(SUPPORT_ADJUDICATION_PREFIX):]
            head, _, tail = slug.partition("-")
            return f"{head.upper()}.{tail.upper()}"
    return None



def sc_probe_mechanical_verification(
    root: Path, language: str, probe_id: str
) -> dict[str, Any] | None:
    """Compact trusted build/run evidence for one canonical Semantic probe."""
    local = (
        root / "work" / "audit" / "semantic-compression"
        / f"canonical_verification_{slug_id(language)}--{slug_id(probe_id)}.json"
    )
    legacy = (
        root / "work" / "audit" / "semantic-compression"
        / f"canonical_verification_{slug_id(language)}.json"
    )
    path = local if local.is_file() else legacy
    if not path.is_file():
        return None
    report = json_load(path)
    if report.get("synthetic_ci") is True:
        return {
            "verified": False,
            "synthetic_ci": True,
            "note": "synthetic CI carries no scored toolchain execution evidence",
        }
    raw = (report.get("probes") or {}).get(probe_id)
    if not isinstance(raw, dict):
        return None
    if report.get("probe_projection_from_current_owner") is True:
        return {
            "verified": False,
            "legacy_evidence_recertified": True,
            "source": "current-validator-certified retained owner evidence",
            "mode": raw.get("mode"),
            "canonical_fragment_sha256": raw.get("canonical_fragment_sha256"),
        }

    def process(value: Any) -> dict[str, Any] | None:
        if not isinstance(value, dict):
            return None
        return {
            "argv": list(value.get("argv") or []),
            "exit_code": value.get("exit_code"),
            "stdout": clip_annotation_text(str(value.get("stdout") or ""), 500),
            "stderr": clip_annotation_text(str(value.get("stderr") or ""), 500),
        }

    return {
        "verified": True,
        "source": "trusted canonical-fragment validator in the pinned runtime",
        "mode": raw.get("mode"),
        "run_count": raw.get("run_count"),
        "build": process(raw.get("build")),
        "symbol_add2_defined": raw.get("symbol_add2_defined"),
        "nm": process(raw.get("nm")),
        "runs": [
            process(value) for value in (raw.get("runs") or [])
            if isinstance(value, dict)
        ],
    }

def build_support_adjudication_input(
    root: Path, unit: dict[str, Any], manifest: dict[str, Any], probe_id: str
) -> Path:
    """Put one probe's ten annotations plus trusted verification context together."""
    units = {str(item.get("id")): item for item in manifest.get("work_units", [])}
    by_language: dict[str, dict[str, Any]] = {}
    cross_probe: dict[str, dict[str, Any]] = {}
    sampled = {
        str(entry.get("probe_id")) for entry in comparability_sample_probes(root)
    }
    support_owner = (
        json_load(root / "template" / "config" / "aggregation.json")
        .get("evaluations", {}).get("semantic_compression", {})
        .get("support_level_owner") or {}
    )
    support_owner_id = str(support_owner.get("requirement_id") or "")
    for dependency in unit.get("dependencies", []):
        source = units.get(str(dependency))
        if source is None:
            continue
        languages = list(source.get("assigned_languages") or [])
        if len(languages) != 1:
            continue
        language = str(languages[0])
        result_path = (
            root / "work" / "agents" / str(source.get("assigned_agent_id"))
            / "result.json"
        )
        if not result_path.is_file():
            continue
        result = json_load(result_path)
        collected = probe_annotation_fields(result, {probe_id})
        fields = collected.get(probe_id) or {}
        if fields:
            sc_add_annotation_source(
                by_language.setdefault(language, {}),
                str(source.get("id") or dependency),
                fields,
            )
        if support_owner_id in (source.get("requirement_ids") or []):
            context = probe_annotation_fields(result, sampled)
            cross_probe[language] = {
                pid: clip_annotation_text(values, 180)
                for pid, values in sorted(context.items())
                if values
            }
    mechanical_verification = {
        language: evidence
        for language in sorted(by_language)
        if (evidence := sc_probe_mechanical_verification(root, language, probe_id))
        is not None
    }
    frozen_toolchains: dict[str, Any] = {}
    toolchains_path = root / "results" / "toolchains.json"
    if toolchains_path.is_file():
        scanned = (json_load(toolchains_path).get("toolchains") or {})
        for language in sorted(by_language):
            row = scanned.get(language)
            if isinstance(row, dict):
                frozen_toolchains[language] = {
                    "canonical": row.get("canonical"),
                    "commands": row.get("commands"),
                }
    probe = next(
        (entry for entry in comparability_sample_probes(root)
         if str(entry.get("probe_id")) == probe_id),
        None,
    )
    universe_path = (
        root / "template" / "methodology-assets" / "semantic_compression"
        / "capability_universe.json"
    )
    universe = json_load(universe_path)
    probe_contracts = {
        str(entry.get("probe_id")): entry
        for entry in (universe.get("probes") or [])
        if str(entry.get("probe_id")) in sampled
    }
    if probe_id not in probe_contracts:
        raise BenchmarkError(
            f"support adjudication probe {probe_id} is missing from capability universe"
        )
    frozen_support_contract = {
        "authoring_rules": universe.get("authoring_rules_for_probe_fragments") or {},
        "support_rubric": universe.get("support_rubric") or {},
        "na_policy": universe.get("na_policy") or {},
        "toolchain_binding": universe.get("toolchain_binding") or {},
        "sampled_probe_contracts": {
            pid: probe_contracts[pid] for pid in sorted(probe_contracts)
        },
    }
    policy_path = root / COMPARABILITY_POLICY_RELATIVE
    policy = json_load(policy_path) if policy_path.is_file() else {}
    payload = {
        "schema_version": 1,
        "probe_id": probe_id,
        "task": (
            "Assign one authoritative support record for this probe in every "
            "language, applying the frozen rubric and comparability policy "
            "symmetrically. Use the cross-probe context to avoid classifying the "
            "same standard mechanism differently unless numbered requirements "
            "materially distinguish the probes. mechanical_verification is trusted "
            "runner evidence from the pinned runtime using the exact frozen recipe, "
            "and frozen_toolchains records the installed versions seen by this run. "
            "When present, do not contradict those build/run facts or invent a P-b "
            "flag requirement that its argv does not contain. Return objects, never "
            "bare levels."
        ),
        "output_contract": {
            "level": "FULL|PARTIAL|NONE",
            "fragment": "exact selected fragment for FULL/PARTIAL; null for NONE",
            "partial_reasons": "[] for FULL/NONE; one or more P-a..P-e for PARTIAL",
            "none_reason": "N-1..N-4 for NONE; null otherwise",
            "justification": "compact rubric-grounded reason",
            "citation": "documentation/evidence relied on",
        },
        "frozen_probe": probe,
        "frozen_support_contract": frozen_support_contract,
        "comparability_policy": policy,
        "mechanical_verification": mechanical_verification,
        "frozen_toolchains": frozen_toolchains,
        "annotation_count": len(by_language),
        "annotations": {language: by_language[language] for language in sorted(by_language)},
        "cross_probe_support_context": {
            language: cross_probe[language] for language in sorted(cross_probe)
        },
        "trusted_runtime_baselines": (
            f20_runtime_baseline_data(root)
            if probe_id == "F20.P1"
            else None
        ),
    }
    destination = require_under(
        root / "work" / "audit" / "semantic-compression"
        / f"support_adjudication_{probe_id.lower().replace('.', '-')}.json",
        root,
    )
    destination.parent.mkdir(parents=True, exist_ok=True)
    json_dump(destination, payload)
    return destination


def sc_adjudicated_records(root: Path) -> dict[str, dict[str, dict[str, Any]]]:
    """Every complete cohort adjudication record, keyed by probe and language."""
    records: dict[str, dict[str, dict[str, Any]]] = {}
    agents = root / "work" / "agents"
    if not agents.is_dir():
        return records
    for result_path in sorted(agents.glob("*/result.json")):
        try:
            result = json_load(result_path)
        except (OSError, json.JSONDecodeError):
            continue
        for rid, value in (result.get("requirements") or {}).items():
            if not str(rid).startswith(SUPPORT_ADJUDICATION_PREFIX):
                continue
            probe = support_adjudication_probe([rid])
            if probe is None or not isinstance(value, dict):
                continue
            for language, raw in value.items():
                record = sc_adjudicated_record(raw)
                if record is not None:
                    records.setdefault(probe, {})[str(language)] = record
    return records


def sc_adjudicated_levels(root: Path) -> dict[str, dict[str, str]]:
    """Authoritative levels used by both comparability and published coverage."""
    return {
        probe: {language: record["level"] for language, record in rows.items()}
        for probe, rows in sc_adjudicated_annotations(root).items()
    }


def sc_comparability_repairs(root: Path) -> dict[str, dict[str, dict[str, Any]]]:
    path = root / COMPARABILITY_REPAIR_RELATIVE
    if not path.is_file():
        return {}
    data = json_load(path)
    out: dict[str, dict[str, dict[str, Any]]] = {}
    for row in data.get("repairs", []) or []:
        record = sc_adjudicated_record(row.get("record"))
        if record is not None:
            out.setdefault(str(row.get("probe_id")), {})[str(row.get("language"))] = record
    return out


def sc_adjudicated_annotations(
    root: Path,
) -> dict[str, dict[str, dict[str, Any]]]:
    """Authoritative support records, with validated comparability repairs last.

    Cohort adjudication is the normal source. A blinded comparability repair is
    pair-specific and may replace only the affected (probe, language) record.
    This keeps the frozen metric shards untouched while letting the same run
    reconcile an inconsistency on any sampled probe, including probes without a
    dedicated cohort-adjudication work unit.
    """
    merged = {
        probe: {language: dict(record) for language, record in rows.items()}
        for probe, rows in sc_adjudicated_records(root).items()
    }
    for probe, rows in sc_comparability_repairs(root).items():
        merged.setdefault(probe, {}).update(
            {language: dict(record) for language, record in rows.items()}
        )
    return merged


def validate_comparability_repair_directives(
    root: Path, result: dict[str, Any]
) -> list[dict[str, Any]]:
    evidence = result.get("evidence") or {}
    raw = evidence.get("repair_directives", [])
    if raw is None:
        raw = []
    if not isinstance(raw, list):
        raise BenchmarkError("comparability repair_directives must be an array")
    blinding_path = root / COMPARABILITY_BLINDING_RELATIVE
    valid_labels: set[str] = set()
    if blinding_path.is_file():
        valid_labels = set((json_load(blinding_path).get("labels") or {}).values())
    valid_probes = {
        str(probe.get("probe_id")) for probe in comparability_sample_probes(root)
    }
    sample_path = root / COMPARABILITY_SAMPLE_RELATIVE
    sample_rows: dict[tuple[str, str], dict[str, Any]] = {}
    if sample_path.is_file():
        sample = json_load(sample_path)
        for probe in sample.get("probes", []) or []:
            probe_id = str(probe.get("probe_id") or "")
            for entry in probe.get("annotations", []) or []:
                if isinstance(entry, dict):
                    sample_rows[(probe_id, str(entry.get("label") or ""))] = entry
    affected_pairs: set[tuple[str, str]] = set()
    evidence_gate = (evidence.get("gate_result") or {})
    affected_rows = evidence_gate.get("affected_pairs_requiring_revalidation")
    if affected_rows is None:
        affected_rows = evidence.get("affected_pairs_requiring_revalidation")
    if isinstance(affected_rows, list):
        for row in affected_rows:
            if isinstance(row, dict):
                affected_pairs.add((
                    str(row.get("probe_id") or "").upper().strip(),
                    str(row.get("label") or "").strip(),
                ))
    normalized: list[dict[str, Any]] = []
    for item in raw:
        if not isinstance(item, dict):
            raise BenchmarkError("comparability repair directive must be an object")
        probe_id = str(item.get("probe_id") or "")
        label = str(item.get("label") or "")
        record = sc_adjudicated_record(item.get("record"))
        if not re.fullmatch(r"F\d\d\.P\d+", probe_id):
            raise BenchmarkError(f"invalid comparability repair probe_id: {probe_id!r}")
        if probe_id not in valid_probes:
            raise BenchmarkError(
                f"comparability repair targets non-sampled probe: {probe_id!r}"
            )
        if valid_labels and label not in valid_labels:
            raise BenchmarkError(f"unknown comparability repair label: {label!r}")
        if affected_pairs and (probe_id, label) not in affected_pairs:
            raise BenchmarkError(
                f"comparability repair {probe_id}/{label} was not declared affected"
            )
        if record is None:
            raise BenchmarkError(
                f"comparability repair for {probe_id}/{label} is not a complete support record"
            )
        validate_sc_record_for_probe(
            root, probe_id, record,
            context=f"comparability repair {probe_id}/{label}",
        )
        current = sample_rows.get((probe_id, label))
        if current is None:
            raise BenchmarkError(
                f"comparability repair {probe_id}/{label} has no sampled annotation"
            )
        current_level = sc_support_is_level(current.get("support"))
        current_fragment = current.get("fragment")
        # A run-local repair may reconcile support classification and its
        # reasons, but it may not silently replace what the metric shards
        # actually measured. A new fragment requires new metric measurements.
        if record["level"] != "NONE":
            if not isinstance(current_fragment, str) or (
                current_fragment.strip() != str(record["fragment"]).strip()
            ):
                raise BenchmarkError(
                    f"comparability repair {probe_id}/{label} changes the measured "
                    "fragment; a fresh measurement is required"
                )
        # Crossing NONE changes whether the probe is supported at all and can
        # change the common quality basis. That is a new measurement, not an
        # in-run annotation repair.
        if current_level == "NONE" or record["level"] == "NONE":
            if current_level != record["level"]:
                raise BenchmarkError(
                    f"comparability repair {probe_id}/{label} crosses the NONE "
                    "boundary; a fresh measurement is required"
                )
        normalized.append({"probe_id": probe_id, "label": label, "record": record})

    # V3 remains an invariant after the pre-measurement gate. Comparability may
    # refine FULL/PARTIAL on the existing measured fragment, but it may not
    # remove the cohort's last FULL implementation for a frozen probe.
    fixed_languages = metadata_languages(root)
    if normalized and len(valid_labels) == len(fixed_languages):
        proposed_levels = {
            (item["probe_id"], item["label"]): item["record"]["level"]
            for item in normalized
        }
        for probe_id in sorted({item["probe_id"] for item in normalized}):
            levels: list[str] = []
            complete = True
            for label in sorted(valid_labels):
                row = sample_rows.get((probe_id, label))
                if row is None:
                    complete = False
                    break
                level = proposed_levels.get(
                    (probe_id, label), sc_support_is_level(row.get("support"))
                )
                if level is None:
                    complete = False
                    break
                levels.append(level)
            if complete and "FULL" not in levels:
                raise BenchmarkError(
                    f"{probe_id}: comparability repair would violate pre-measurement "
                    "V3 by removing the cohort's last FULL implementation"
                )
    return normalized


def persist_comparability_repairs(root: Path, result: dict[str, Any]) -> int:
    directives = validate_comparability_repair_directives(root, result)
    if not directives:
        return 0
    blinding = json_load(root / COMPARABILITY_BLINDING_RELATIVE).get("labels") or {}
    by_label = {str(label): str(language) for language, label in blinding.items()}
    path = root / COMPARABILITY_REPAIR_RELATIVE
    existing = json_load(path) if path.is_file() else {"schema_version": 1, "repairs": []}
    rows = {
        (str(row.get("probe_id")), str(row.get("language"))): row
        for row in (existing.get("repairs") or [])
    }
    changed = 0
    for directive in directives:
        language = by_label.get(directive["label"])
        if not language:
            raise BenchmarkError(f"cannot resolve blinded label {directive['label']!r}")
        if directive["probe_id"] == "F20.P1":
            validate_f20_record_against_runtime_baseline(
                root, language, directive["record"]
            )
        key = (directive["probe_id"], language)
        row = {
            "probe_id": directive["probe_id"],
            "language": language,
            "record": directive["record"],
        }
        if rows.get(key) != row:
            rows[key] = row
            changed += 1
    json_dump(path, {
        "schema_version": 1,
        "repairs": [rows[key] for key in sorted(rows)],
    })
    return changed


def build_comparability_sample(
    root: Path, unit: dict[str, Any], manifest: dict[str, Any]
) -> Path:
    """Assemble the blinded cross-language sample the comparability audit judges.

    A Task Packet is rendered when its unit's dependencies are COMPLETE, so the
    annotations exist on the trusted side by the time this runs. Without this
    the audit only ever saw the frozen matrix template, whose every site is
    UNMEASURED, and it correctly refused to certify a sample that did not
    exist.
    """
    units = {str(item.get("id")): item for item in manifest.get("work_units", [])}
    probes = comparability_sample_probes(root)
    wanted = {str(probe.get("probe_id")) for probe in probes}
    support_owner = (
        json_load(root / "template" / "config" / "aggregation.json")
        .get("evaluations", {})
        .get("semantic_compression", {})
        .get("support_level_owner")
        or {}
    )
    support_owner_id = str(support_owner.get("requirement_id") or "")
    owner_rows: dict[str, dict[str, dict[str, Any]]] = {}
    by_language: dict[str, dict[str, dict[str, Any]]] = {}
    for dependency in unit.get("dependencies", []):
        source = units.get(str(dependency))
        if source is None:
            continue
        languages = list(source.get("assigned_languages") or [])
        if len(languages) != 1:
            continue
        result_path = (
            root / "work" / "agents" / str(source.get("assigned_agent_id"))
            / "result.json"
        )
        if not result_path.is_file():
            continue
        collected = probe_annotation_fields(json_load(result_path), wanted)
        annotations = by_language.setdefault(str(languages[0]), {})
        is_support_owner = support_owner_id in (source.get("requirement_ids") or [])
        for probe_id, fields in collected.items():
            if fields and not is_support_owner:
                metric_fields = sc_metric_only_annotation(fields)
                if metric_fields:
                    sc_add_annotation_source(
                        annotations.setdefault(probe_id, {}),
                        str(source.get("id") or dependency),
                        metric_fields,
                    )
        if is_support_owner:
            owned = owner_rows.setdefault(str(languages[0]), {})
            for probe_id, fields in collected.items():
                if fields:
                    merge_annotation_fields_strict(
                        owned.setdefault(probe_id, {}),
                        fields,
                        context=(
                            f"support owner {languages[0]}/{probe_id} "
                            f"from {source.get('id')}"
                        ),
                    )
    if not by_language:
        raise BenchmarkError(
            "comparability audit sample has no completed annotations to review"
        )

    adjudicated = sc_adjudicated_annotations(root)
    reconciled = (
        sc_reconcile_support(
            by_language, owner_rows, support_owner, adjudicated
        )
        if support_owner_id
        else []
    )
    labels = comparability_blinding(
        str(json_load(root / "run.json").get("run_id")), sorted(by_language)
    )
    sample = {
        "schema_version": 1,
        "audit": "blinded cross-language comparability audit (methodology 6.1.1A)",
        "blinded": True,
        "blinding_note": (
            "Entries are labelled by a per-run permutation and every language "
            "name has been redacted from their prose. Code fragments are "
            "verbatim, so a fragment's syntax may still reveal its language: "
            "judge every entry against the frozen matrix, never against what "
            "you believe the language to be."
        ),
        "sample_rule": (
            "the first frozen probe of every capability family: "
            f"{len(probes)} of {len(json_load(root / 'template' / 'methodology-assets' / 'semantic_compression' / 'semantic_site_matrix.json').get('probes') or [])} "
            "frozen probes, covering every capability family"
        ),
        "entry_labels": sorted(labels.values()),
        "support_reconciliation": {
            "owner_requirement_id": support_owner_id,
            "why": support_owner.get("why"),
            "note": (
                "Each entry's `support` and `support_factor` were set by the "
                "runner from the owning Capability Coverage annotation, because "
                "a shard sees one metric and one language and cannot reconcile "
                "across the others. Do not re-adjudicate a support level as a "
                "cross-entry disagreement; judge annotation depth, row "
                "interpretation and asymmetric treatment as the methodology "
                "directs. An entry whose owner stated no determinate level "
                "carries `support: UNRECONCILED` and is unstated, not "
                "comparable."
            ),
            "reconciled_entries": sorted(
                {
                    (labels[row["language"]], row["probe_id"], row["reconciled_to"])
                    for row in reconciled
                }
            ),
            "replaced_field_names": sorted(
                {name for row in reconciled for name in row["replaced_fields"]}
            ),
            "adjudicated_probes": sorted(adjudicated),
            "adjudication_note": (
                "For the probes listed above, the whole support annotation - "
                "level, P-letter or N-reason, citation and justification - was "
                "assigned by a unit that saw all ten languages at once. The "
                "runner removed superseded shard support explanations before "
                "building this sample. Judge support only from the nested "
                "support_adjudication object."
            ),
            "full_record": (
                "work/audit/semantic-compression/support_reconciliation.json, "
                "kept with the run's evidence rather than in this packet"
            ),
        },
        "probes": [
            {
                "probe_id": str(probe.get("probe_id")),
                "family": probe.get("family"),
                "capability_denominator": probe.get("capability_denominator"),
                "unsupported_rule": probe.get("unsupported_rule"),
                "frozen_sites": [
                    site.get("semantic_fact") for site in (probe.get("sites") or [])
                ],
                "annotations": [
                    {
                        "label": labels[language],
                        **blind_annotation(
                            by_language[language].get(str(probe.get("probe_id")), {}),
                            labels[language],
                        ),
                    }
                    for language in sorted(
                        by_language, key=lambda item: labels[item]
                    )
                ],
            }
            for probe in probes
        ],
    }
    encoded = (
        json.dumps(sample, separators=(",", ":"), sort_keys=True) + "\n"
    ).encode("utf-8")
    if len(encoded) > COMPARABILITY_SAMPLE_BUDGET:
        raise BenchmarkError(
            f"blinded comparability sample is {len(encoded)} bytes, over the "
            f"{COMPARABILITY_SAMPLE_BUDGET} the Task Packet reserves for it"
        )
    destination = require_under(root / COMPARABILITY_SAMPLE_RELATIVE, root)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(encoded)
    if support_owner_id:
        # Every annotation the runner replaced, with the values it replaced and
        # the language it belongs to: too large for the auditor's packet, and
        # the whole point of reconciling in the open rather than silently.
        json_dump(
            destination.parent / "support_reconciliation.json",
            {
                "schema_version": 1,
                "owner_requirement_id": support_owner_id,
                "why": support_owner.get("why"),
                "replacements": reconciled,
            },
        )
    blinding = require_under(root / COMPARABILITY_BLINDING_RELATIVE, root)
    blinding.parent.mkdir(parents=True, exist_ok=True)
    blinding.write_text(
        json.dumps(
            {"schema_version": 1, "labels": labels}, indent=2, sort_keys=True
        ) + "\n",
        encoding="utf-8",
    )
    return destination




def semantic_probe_ids(root: Path) -> list[str]:
    matrix = json_load(
        root / "template" / "methodology-assets" / "semantic_compression"
        / "semantic_site_matrix.json"
    )
    probe_ids = [str(row.get("probe_id") or "") for row in matrix.get("probes", [])]
    if not probe_ids or any(not probe_id for probe_id in probe_ids):
        raise BenchmarkError("Semantic Compression has no frozen probe set")
    if len(probe_ids) != len(set(probe_ids)):
        raise BenchmarkError("Semantic Compression frozen probe IDs are not unique")
    return probe_ids


def materialize_semantic_probe_contract(
    root: Path, probe_id: str, language: str
) -> Path:
    """Freeze the exact shared rules and one probe row read by an atomic owner."""
    asset_root = (
        root / "template" / "methodology-assets" / "semantic_compression"
    )
    universe = json_load(asset_root / "capability_universe.json")
    matrix = json_load(asset_root / "semantic_site_matrix.json")
    universe_rows = {
        str(row.get("probe_id")): row
        for row in (universe.get("probes") or [])
    }
    matrix_rows = {
        str(row.get("probe_id")): row
        for row in (matrix.get("probes") or [])
    }
    if probe_id not in universe_rows or probe_id not in matrix_rows:
        raise BenchmarkError(
            f"unknown Semantic Compression probe contract: {probe_id}"
        )
    payload = {
        "schema_version": 1,
        "language": language,
        "probe_id": probe_id,
        "global_rules": {
            "spec_authority": universe.get("spec_authority"),
            "toolchain_binding": universe.get("toolchain_binding"),
            "semantic_fact_kinds": universe.get("semantic_fact_kinds"),
            "authoring_rules_for_probe_fragments": universe.get(
                "authoring_rules_for_probe_fragments"
            ),
            "support_rubric": universe.get("support_rubric"),
            "na_policy": universe.get("na_policy"),
            "pre_measurement_validation": universe.get(
                "pre_measurement_validation"
            ),
            "annotation_states": matrix.get("annotation_states"),
            "scored_annotation_states": matrix.get(
                "scored_annotation_states"
            ),
            "metrics": matrix.get("metrics"),
        },
        "capability_probe": universe_rows[probe_id],
        "semantic_site_probe": matrix_rows[probe_id],
        "generation_failure_policy": (
            "A failed candidate, compile/run failure, or exhausted repair attempt "
            "is not evidence of NONE. NONE requires capability-absence evidence."
        ),
    }
    destination = (
        root / "work" / "root" / "semantic-probes"
        / f"{slug_id(language)}--{slug_id(probe_id)}.json"
    )
    json_dump(destination, payload)
    return destination


def canonical_fragment_catalog(
    root: Path,
    result: dict[str, Any],
    expected_probe_ids: Iterable[str] | None = None,
) -> dict[str, dict[str, Any]]:
    """Validate and normalize canonical fragment/support records."""
    evidence = result.get("evidence") or {}
    raw = evidence.get("canonical_fragments")
    if not isinstance(raw, dict):
        raise BenchmarkError(
            "canonical fragment owner must write evidence.canonical_fragments"
        )
    expected = (
        set(semantic_probe_ids(root))
        if expected_probe_ids is None
        else {str(value) for value in expected_probe_ids}
    )
    if set(raw) != expected:
        raise BenchmarkError(
            "canonical fragment catalog has the wrong frozen probe set; "
            f"missing={sorted(expected-set(raw))}, extra={sorted(set(raw)-expected)}"
        )
    normalized: dict[str, dict[str, Any]] = {}
    for probe_id in sorted(expected):
        record = sc_adjudicated_record(raw[probe_id])
        if record is None:
            raise BenchmarkError(
                f"canonical fragment {probe_id} must be a complete support record"
            )
        validate_sc_record_for_probe(
            root, probe_id, record, context=f"canonical fragment {probe_id}"
        )
        normalized[probe_id] = record
    return normalized


def canonical_fragment_input_for_unit(
    root: Path, unit: dict[str, Any], manifest: dict[str, Any]
) -> tuple[Path, str]:
    """Materialize one language catalog from its independently certified probe leaves."""
    source_requirement = str(
        unit.get("canonical_fragment_source_requirement") or ""
    )
    assigned = list(unit.get("assigned_languages") or [])
    if not source_requirement or len(assigned) != 1:
        raise BenchmarkError(
            f"{unit.get('id')}: canonical fragment consumer must own one language"
        )
    language = str(assigned[0])
    expected = set(semantic_probe_ids(root))
    by_probe: dict[str, dict[str, Any]] = {}
    source_ids: list[str] = []
    for source in manifest.get("work_units", []):
        if not source.get("canonical_fragment_owner"):
            continue
        if list(source.get("assigned_languages") or []) != [language]:
            continue
        probe_id = str(source.get("canonical_probe_id") or "")
        if probe_id not in expected:
            continue
        result_path = (
            root / "work" / "agents" / str(source.get("assigned_agent_id"))
            / "result.json"
        )
        if not result_path.is_file():
            raise BenchmarkError(
                f"{unit.get('id')}: canonical fragment leaf is missing: {source.get('id')}"
            )
        leaf = canonical_fragment_catalog(
            root, json_load(result_path), {probe_id}
        )
        if probe_id in by_probe:
            raise BenchmarkError(
                f"{unit.get('id')}: duplicate canonical fragment leaf for "
                f"{language} {probe_id}"
            )
        by_probe[probe_id] = leaf[probe_id]
        source_ids.append(str(source.get("id")))

    if set(by_probe) != expected:
        raise BenchmarkError(
            f"{unit.get('id')}: canonical fragment leaves incomplete for {language}; "
            f"missing={sorted(expected-set(by_probe))}, "
            f"extra={sorted(set(by_probe)-expected)}"
        )
    payload = {
        "schema_version": 2,
        "language": language,
        "source_work_unit_ids": sorted(source_ids),
        "source_requirement_prefix": CANONICAL_FRAGMENT_PREFIX,
        "rule": (
            "Use exactly these independently certified probe fragments for every "
            "downstream Semantic Compression metric. FULL/PARTIAL entries must be "
            "measured verbatim; NONE entries have no fragment and must not receive "
            "a numeric per-probe A/B/C/D measurement."
        ),
        "canonical_fragments": {
            probe_id: by_probe[probe_id] for probe_id in semantic_probe_ids(root)
        },
    }
    destination = require_under(
        root / "work" / "audit" / "semantic-compression"
        / f"canonical_fragments_{slug_id(language)}.json",
        root,
    )
    destination.parent.mkdir(parents=True, exist_ok=True)
    encoded = (
        json.dumps(payload, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")
    if destination.exists() and destination.read_bytes() != encoded:
        raise BenchmarkError(
            f"canonical fragment input changed after probe completion: {language}"
        )
    destination.write_bytes(encoded)
    return destination, sha256_bytes(encoded)


def canonical_owner_record_for_probe(
    root: Path, language: str, probe_id: str
) -> dict[str, Any]:
    """Read the independently certified fragment/support record for one pair."""
    manifest = json_load(root / "work" / "root" / "manifest.json")
    matches = [
        unit for unit in manifest.get("work_units", [])
        if unit.get("canonical_fragment_owner")
        and str(unit.get("canonical_probe_id") or "") == probe_id
        and list(unit.get("assigned_languages") or []) == [language]
    ]
    if len(matches) != 1:
        raise BenchmarkError(
            f"{probe_id}: expected one canonical fragment owner for {language}; "
            f"found {len(matches)}"
        )
    result_path = (
        root / "work" / "agents" / str(matches[0].get("assigned_agent_id"))
        / "result.json"
    )
    if not result_path.is_file():
        raise BenchmarkError(
            f"{probe_id}: canonical fragment owner result missing for {language}"
        )
    return canonical_fragment_catalog(
        root, json_load(result_path), {probe_id}
    )[probe_id]


def canonical_fragment_catalog_for_language(
    root: Path, language: str
) -> dict[str, dict[str, Any]]:
    return {
        probe_id: canonical_owner_record_for_probe(root, language, probe_id)
        for probe_id in semantic_probe_ids(root)
    }

def validate_support_adjudication_against_canonical_fragments(
    root: Path, requirement_id: str, value: dict[str, Any]
) -> None:
    """Adjudication may refine support, never rewrite what A/B/C/D/E measured."""
    probe_id = support_adjudication_probe([requirement_id])
    if probe_id is None:
        raise BenchmarkError(f"invalid support adjudication id: {requirement_id}")
    for language in metadata_languages(root):
        adjudicated = sc_adjudicated_record(value.get(language))
        if adjudicated is None:
            # Shape errors are reported by the ordinary result validator.
            continue
        validate_sc_record_for_probe(
            root, probe_id, adjudicated,
            context=f"{requirement_id}: {language}",
        )
        canonical = canonical_owner_record_for_probe(root, language, probe_id)
        canonical_none = canonical["level"] == "NONE"
        adjudicated_none = adjudicated["level"] == "NONE"
        if canonical_none != adjudicated_none:
            raise BenchmarkError(
                f"{requirement_id}: {language} crosses the canonical NONE "
                "boundary; this requires a fresh metric measurement"
            )
        if not canonical_none:
            if str(adjudicated["fragment"]).strip() != str(
                canonical["fragment"]
            ).strip():
                raise BenchmarkError(
                    f"{requirement_id}: {language} changes the canonical measured "
                    "fragment; adjudication may only refine FULL/PARTIAL and its "
                    "reasoning on the existing fragment"
                )
        if probe_id == "F20.P1":
            validate_f20_record_against_runtime_baseline(
                root, language, adjudicated
            )

    adjudicated_records = [
        sc_adjudicated_record(value.get(language))
        for language in metadata_languages(root)
    ]
    if all(record is not None for record in adjudicated_records) and not any(
        record["level"] == "FULL" for record in adjudicated_records if record is not None
    ):
        raise BenchmarkError(
            f"{requirement_id}: cohort adjudication would violate pre-measurement V3; "
            f"{probe_id} must retain at least one FULL implementation"
        )


SEMANTIC_VERIFICATION_ENTRY_FILES = {
    "Quidra": "main.qui",
    "Python": "main.py",
    "C++": "main.cpp",
    "Rust": "main.rs",
    "Go": "main.go",
    "Java": "Main.java",
    "TypeScript": "main.ts",
    "Kotlin": "main.kt",
    "Swift": "main.swift",
    "Zig": "main.zig",
}
SEMANTIC_MULTI_UNIT_PROBES = {"F14.P3", "F18.P2"}
SEMANTIC_NATIVE_NM_LANGUAGES = {"Quidra", "C++", "Rust", "Go", "Swift", "Zig"}


def _semantic_verification_path(raw: Any) -> PurePosixPath:
    text = str(raw or "")
    path = PurePosixPath(text)
    if (
        not text
        or path.is_absolute()
        or any(part in {"", ".", ".."} for part in path.parts)
    ):
        raise BenchmarkError(f"invalid Semantic Compression verification path: {text!r}")
    return path


def _semantic_code_normalize(text: str) -> str:
    return " ".join(str(text).split())


def _semantic_verification_schema(
    catalog: dict[str, dict[str, Any]], evidence: dict[str, Any]
) -> dict[str, dict[str, Any]]:
    raw = evidence.get("canonical_verification")
    if not isinstance(raw, dict):
        raise BenchmarkError(
            "canonical fragment owner must write evidence.canonical_verification"
        )
    expected = {
        probe_id
        for probe_id, record in catalog.items()
        if str(record.get("level")).upper() in {"FULL", "PARTIAL"}
    }
    if set(raw) != expected:
        raise BenchmarkError(
            "canonical verification must cover exactly every FULL/PARTIAL probe; "
            f"missing={sorted(expected-set(raw))}, extra={sorted(set(raw)-expected)}"
        )

    normalized: dict[str, dict[str, Any]] = {}
    for probe_id in sorted(expected):
        row = raw[probe_id]
        if not isinstance(row, dict):
            raise BenchmarkError(f"{probe_id}: canonical verification must be an object")
        allowed = {"entry_file", "fragment_files", "files", "mode", "run_count"}
        unknown = sorted(set(row) - allowed)
        if unknown:
            raise BenchmarkError(
                f"{probe_id}: canonical verification has unknown fields: {unknown}"
            )
        files = row.get("files")
        if not isinstance(files, dict) or not files or len(files) > 12:
            raise BenchmarkError(
                f"{probe_id}: verification files must contain 1..12 text files"
            )
        normalized_files: dict[str, str] = {}
        total_bytes = 0
        for raw_path, content in files.items():
            path = _semantic_verification_path(raw_path)
            if not isinstance(content, str):
                raise BenchmarkError(
                    f"{probe_id}: verification file {path} must be UTF-8 text"
                )
            encoded = content.encode("utf-8")
            total_bytes += len(encoded)
            if total_bytes > 262_144:
                raise BenchmarkError(
                    f"{probe_id}: verification fixture exceeds 262144 bytes"
                )
            normalized_files[path.as_posix()] = content

        entry = _semantic_verification_path(row.get("entry_file")).as_posix()
        if entry not in normalized_files:
            raise BenchmarkError(
                f"{probe_id}: entry_file {entry!r} is not present in verification files"
            )
        fragment_files = row.get("fragment_files")
        if (
            not isinstance(fragment_files, list)
            or not fragment_files
            or not all(isinstance(item, str) for item in fragment_files)
        ):
            raise BenchmarkError(
                f"{probe_id}: fragment_files must be a non-empty string array"
            )
        fragment_paths = [
            _semantic_verification_path(item).as_posix() for item in fragment_files
        ]
        if len(set(fragment_paths)) != len(fragment_paths):
            raise BenchmarkError(f"{probe_id}: fragment_files contains duplicates")
        missing_fragment_files = sorted(set(fragment_paths) - set(normalized_files))
        if missing_fragment_files:
            raise BenchmarkError(
                f"{probe_id}: fragment_files are missing from files: "
                + ", ".join(missing_fragment_files)
            )
        if (
            probe_id not in SEMANTIC_MULTI_UNIT_PROBES
            and fragment_paths != [entry]
        ):
            raise BenchmarkError(
                f"{probe_id}: single-unit verification fragment_files must be "
                f"exactly [entry_file] ({entry!r}); measured code may not live "
                "in an unbuilt or unused fixture file"
            )
        measured = _semantic_code_normalize(
            "\n".join(normalized_files[name] for name in fragment_paths)
        )
        fragment = _semantic_code_normalize(str(catalog[probe_id]["fragment"]))
        if not fragment or fragment not in measured:
            raise BenchmarkError(
                f"{probe_id}: verification fixture does not contain the canonical "
                "fragment verbatim modulo whitespace"
            )

        expected_mode = "nm-add2" if probe_id == "F20.P2" else "run"
        mode = str(row.get("mode") or "")
        if mode != expected_mode:
            raise BenchmarkError(
                f"{probe_id}: verification mode must be {expected_mode!r}, got {mode!r}"
            )
        expected_runs = 0 if probe_id == "F20.P2" else (20 if probe_id == "F19.P2" else 1)
        try:
            run_count = int(row.get("run_count"))
        except (TypeError, ValueError) as exc:
            raise BenchmarkError(
                f"{probe_id}: verification run_count must be {expected_runs}"
            ) from exc
        if run_count != expected_runs:
            raise BenchmarkError(
                f"{probe_id}: verification run_count must be {expected_runs}, got {run_count}"
            )
        normalized[probe_id] = {
            "entry_file": entry,
            "fragment_files": fragment_paths,
            "files": normalized_files,
            "mode": mode,
            "run_count": run_count,
        }
    return normalized


def _semantic_validate_real_fragment_files(
    language: str,
    probe_id: str,
    row: dict[str, Any],
) -> None:
    """Require every measured fragment file to participate in the frozen recipe."""
    if probe_id not in SEMANTIC_MULTI_UNIT_PROBES:
        return

    entry = str(row["entry_file"])
    fragment_files = list(row["fragment_files"])
    files = row["files"]
    if entry not in fragment_files:
        raise BenchmarkError(
            f"{probe_id}: multi-unit verification fragment_files must include "
            f"the executed entry file {entry!r}"
        )
    if len(set(fragment_files)) < 2:
        raise BenchmarkError(
            f"{probe_id}: multi-unit verification must measure code from at least "
            "two source files"
        )

    fixed_helpers = {
        "C++": {"util.cpp"},
        "Java": {"util/Util.java"},
        "Kotlin": {"util.kt"},
        "Swift": {"util.swift"},
    }
    suffixes = {
        "Quidra": ".qui",
        "Python": ".py",
        "Rust": ".rs",
        "TypeScript": ".ts",
        "Zig": ".zig",
    }

    if language in fixed_helpers:
        required_helpers = fixed_helpers[language]
        allowed = {entry, *required_helpers}
        missing_helpers = sorted(required_helpers - set(fragment_files))
        if missing_helpers:
            raise BenchmarkError(
                f"{probe_id}: {language} multi-unit measured fragment must include "
                "the frozen helper source(s): " + ", ".join(missing_helpers)
            )
    elif language == "Go":
        helpers = {
            name for name in files
            if name.startswith("util/") and name.endswith(".go")
        }
        if not helpers.intersection(fragment_files):
            raise BenchmarkError(
                f"{probe_id}: Go multi-unit measured fragment must include at least "
                "one util/*.go source used by the frozen package build"
            )
        allowed = {entry, *helpers}
    else:
        suffix = suffixes.get(language)
        if suffix is None:
            raise BenchmarkError(
                f"{probe_id}: unknown Semantic Compression language {language!r}"
            )
        allowed = {
            name for name in files
            if name == entry or name.endswith(suffix)
        }

    unbuilt = sorted(set(fragment_files) - allowed)
    if unbuilt:
        raise BenchmarkError(
            f"{probe_id}: {language} fragment_files contain non-source/unbuilt "
            "fixture files: " + ", ".join(unbuilt)
        )


def _semantic_verification_recipe(
    language: str,
    probe_id: str,
    entry: str,
    files: dict[str, str],
) -> tuple[list[str] | None, list[str] | None, str | None]:
    expected_entry = SEMANTIC_VERIFICATION_ENTRY_FILES.get(language)
    if expected_entry is None:
        raise BenchmarkError(f"unknown Semantic Compression language: {language}")
    if entry != expected_entry:
        raise BenchmarkError(
            f"{probe_id}: {language} verification entry must be {expected_entry!r}"
        )

    multi = probe_id in SEMANTIC_MULTI_UNIT_PROBES
    if multi:
        if language == "C++" and "util.cpp" not in files:
            raise BenchmarkError(f"{probe_id}: C++ multi-unit verification requires util.cpp")
        if language == "Swift" and "util.swift" not in files:
            raise BenchmarkError(f"{probe_id}: Swift multi-unit verification requires util.swift")
        if language == "Java" and "util/Util.java" not in files:
            raise BenchmarkError(f"{probe_id}: Java multi-unit verification requires util/Util.java")
        if language == "Kotlin" and "util.kt" not in files:
            raise BenchmarkError(f"{probe_id}: Kotlin multi-unit verification requires util.kt")
        if language == "Go":
            if "go.mod" not in files or not any(
                name.startswith("util/") and name.endswith(".go") for name in files
            ):
                raise BenchmarkError(
                    f"{probe_id}: Go multi-unit verification requires go.mod and util/*.go"
                )

    if language == "Quidra":
        return ["quidra", "build", entry, "-o", "program"], ["./program"], "program"
    if language == "Python":
        return None, ["python3", entry], None
    if language == "C++":
        sources = (["util.cpp"] if multi else []) + [entry]
        return ["clang++", "-std=c++20", "-O2", *sources, "-o", "program"], ["./program"], "program"
    if language == "Rust":
        return ["rustc", "-O", "-C", "debug-assertions=on", entry, "-o", "program"], ["./program"], "program"
    if language == "Go":
        build = ["go", "build", "-o", "program", "."] if multi else [
            "go", "build", "-o", "program", entry
        ]
        return build, ["./program"], "program"
    if language == "Java":
        sources = (["util/Util.java"] if multi else []) + [entry]
        return ["javac", "-d", "out", *sources], ["java", "-cp", "out", "Main"], None
    if language == "TypeScript":
        return [
            "tsc", "--strict", "--target", "es2022", "--module", "nodenext", entry
        ], ["node", str(PurePosixPath(entry).with_suffix(".js"))], None
    if language == "Kotlin":
        sources = (["util.kt"] if multi else []) + [entry]
        return [
            "kotlinc", *sources, "-include-runtime", "-d", "program.jar"
        ], ["java", "-jar", "program.jar"], None
    if language == "Swift":
        sources = (["util.swift"] if multi else []) + [entry]
        return ["swiftc", "-O", *sources, "-o", "program"], ["./program"], "program"
    if language == "Zig":
        return [
            "zig", "build-exe", "-OReleaseSafe", entry, "-femit-bin=program"
        ], ["./program"], "program"
    raise BenchmarkError(f"unsupported Semantic Compression verification language: {language}")


def _semantic_render_frozen_recipe(command: str, entry: str) -> list[str]:
    """Render one frozen Semantic Compression recipe with verification fixture names."""
    replacements = {
        "FILE.qui": entry,
        "FILE.py": entry,
        "FILE.cpp": entry,
        "FILE.rs": entry,
        "FILE.go": entry,
        "FILE.java": entry,
        "FILE.ts": entry,
        "FILE.kt": entry,
        "FILE.swift": entry,
        "FILE.zig": entry,
        "FILE.jar": "program.jar",
        "FILE.js": str(PurePosixPath(entry).with_suffix(".js")),
        "BIN": "program",
        "OUT": "out",
    }
    argv: list[str] = []
    for raw in shlex.split(command):
        value = raw
        for key in sorted(replacements, key=len, reverse=True):
            value = value.replace(key, replacements[key])
        argv.append(value)
    return argv


def semantic_verification_recipe_drift_problems(root: Path) -> list[str]:
    """Detect drift between the frozen SC recipe contract and the runner executor."""
    asset = json_load(
        root / "template" / "methodology-assets" / "semantic_compression"
        / "capability_universe.json"
    )
    binding = asset.get("toolchain_binding") or {}
    recipes = binding.get("recipes") or {}
    multi_recipes = binding.get("multi_unit_recipes") or {}
    problems: list[str] = []

    def frozen_single(language: str, entry: str) -> tuple[list[str] | None, list[str]]:
        recipe = recipes.get(language)
        if not isinstance(recipe, dict) or not isinstance(recipe.get("run"), str):
            raise BenchmarkError(
                f"Semantic Compression frozen recipe is missing for {language}"
            )
        build_raw = recipe.get("build")
        build = (
            _semantic_render_frozen_recipe(build_raw, entry)
            if isinstance(build_raw, str) and build_raw.strip()
            else None
        )
        return build, _semantic_render_frozen_recipe(str(recipe["run"]), entry)

    multi_files = {
        "C++": {"util.cpp": ""},
        "Swift": {"util.swift": ""},
        "Java": {"util/Util.java": ""},
        "Kotlin": {"util.kt": ""},
        "Go": {"go.mod": "module example\n", "util/util.go": "package util\n"},
    }

    for language, entry in SEMANTIC_VERIFICATION_ENTRY_FILES.items():
        files = {entry: ""}
        actual_build, actual_run, _ = _semantic_verification_recipe(
            language, "F01.P1", entry, files
        )
        expected_build, expected_run = frozen_single(language, entry)
        if actual_build != expected_build:
            problems.append(
                f"{language}: single-unit build recipe drift: "
                f"runner={actual_build!r}, frozen={expected_build!r}"
            )
        if actual_run != expected_run:
            problems.append(
                f"{language}: single-unit run recipe drift: "
                f"runner={actual_run!r}, frozen={expected_run!r}"
            )

        multi_files_for_language = {entry: "", **multi_files.get(language, {})}
        actual_multi_build, actual_multi_run, _ = _semantic_verification_recipe(
            language, "F18.P2", entry, multi_files_for_language
        )
        frozen_build, frozen_run = frozen_single(language, entry)
        multi_raw = multi_recipes.get(language)
        if isinstance(multi_raw, str) and not multi_raw.startswith("unchanged"):
            build_command = multi_raw.split("  (", 1)[0].strip()
            expected_multi_build = _semantic_render_frozen_recipe(
                build_command, entry
            )
        else:
            expected_multi_build = frozen_build
        if actual_multi_build != expected_multi_build:
            problems.append(
                f"{language}: multi-unit build recipe drift: "
                f"runner={actual_multi_build!r}, frozen={expected_multi_build!r}"
            )
        if actual_multi_run != frozen_run:
            problems.append(
                f"{language}: multi-unit run recipe drift: "
                f"runner={actual_multi_run!r}, frozen={frozen_run!r}"
            )

    return problems


def _semantic_process_record(
    argv: list[str], completed: subprocess.CompletedProcess[str]
) -> dict[str, Any]:
    stdout = completed.stdout or ""
    stderr = completed.stderr or ""
    return {
        "argv": list(argv),
        "exit_code": int(completed.returncode),
        "stdout_sha256": sha256_bytes(stdout.encode("utf-8")),
        "stderr_sha256": sha256_bytes(stderr.encode("utf-8")),
        "stdout": stdout[:4096],
        "stderr": stderr[:4096],
    }


def _semantic_run_process(
    root: Path, cwd: Path, argv: list[str], *, label: str
) -> dict[str, Any]:
    try:
        completed = subprocess.run(
            argv,
            cwd=cwd,
            env=sanitized_subprocess_env(root, cwd),
            shell=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=300,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise BenchmarkError(f"{label} could not execute: {exc}") from exc
    record = _semantic_process_record(argv, completed)
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout or "").strip()[:1200]
        raise BenchmarkError(
            f"{label} failed with exit {completed.returncode}: {detail}"
        )
    return record


def semantic_fixed_stdout_oracles(root: Path) -> dict[str, str]:
    """Frozen exact-output checks for probes with a language-neutral result."""
    asset = json_load(
        root / "template" / "methodology-assets" / "semantic_compression"
        / "capability_universe.json"
    )
    cfg = (asset.get("pre_measurement_validation") or {}).get(
        "V1_fixed_stdout_oracles"
    )
    if not isinstance(cfg, dict) or not isinstance(cfg.get("by_probe"), dict):
        raise BenchmarkError(
            "capability_universe must declare V1_fixed_stdout_oracles.by_probe"
        )
    probes = {
        str(probe.get("probe_id"))
        for probe in (asset.get("probes") or [])
        if probe.get("probe_id")
    }
    raw = cfg["by_probe"]
    if not raw:
        raise BenchmarkError("V1_fixed_stdout_oracles.by_probe may not be empty")
    unknown = sorted(set(str(key) for key in raw) - probes)
    if unknown:
        raise BenchmarkError(
            "V1_fixed_stdout_oracles names unknown probes: " + ", ".join(unknown)
        )
    out: dict[str, str] = {}
    for probe_id, expected in raw.items():
        if not isinstance(expected, str) or not expected or expected != expected.strip():
            raise BenchmarkError(
                f"V1 fixed stdout oracle must be a non-empty stripped string: {probe_id}"
            )
        out[str(probe_id)] = expected
    return out


F20_RUNTIME_FACTS_RELATIVE = Path("work/root/f20_runtime_facts.json")
F20_RUNTIME_REQUIRED_LANGUAGES = ("Python", "Go", "Java", "Kotlin")


def normalize_f20_runtime_smoke(smoke: dict[str, Any]) -> dict[str, Any]:
    """Stable, cache-key-safe projection of the image's F20.P1 smoke evidence."""
    languages: dict[str, Any] = {}
    for language in F20_RUNTIME_REQUIRED_LANGUAGES:
        row = smoke.get(language)
        if not isinstance(row, dict):
            raise BenchmarkError(
                f"runtime image F20.P1 smoke is missing {language}"
            )
        if row.get("passed") is not True:
            raise BenchmarkError(
                f"runtime image F20.P1 smoke failed for {language}"
            )
        extra = row.get("frozen_recipe_extra_flags")
        if extra != []:
            raise BenchmarkError(
                f"runtime image F20.P1 smoke for {language} used extra flags: {extra!r}"
            )
        build = row.get("build")
        run = row.get("run") or {}
        languages[language] = {
            "mechanism": row.get("mechanism"),
            "passed": True,
            "frozen_recipe_extra_flags": [],
            "build_argv": (build or {}).get("argv") if isinstance(build, dict) else None,
            "run_argv": run.get("argv") if isinstance(run, dict) else None,
            "observed_stdout": str(run.get("stdout") or "").strip(),
        }
        if languages[language]["observed_stdout"] != "3":
            raise BenchmarkError(
                f"runtime image F20.P1 smoke for {language} did not observe stdout 3"
            )
    return {
        "schema_version": 1,
        "probe_id": "F20.P1",
        "source": "/opt/quidra-benchmark/toolchains-observed.json",
        "languages": languages,
    }


def f20_runtime_baseline_data(root: Path) -> dict[str, Any] | None:
    path = root / F20_RUNTIME_FACTS_RELATIVE
    if not path.is_file():
        return None
    data = json_load(path)
    if data.get("schema_version") != 1 or data.get("probe_id") != "F20.P1":
        raise BenchmarkError("F20 runtime facts file is invalid")
    return data


def validate_f20_record_against_runtime_baseline(
    root: Path, language: str, record: dict[str, Any]
) -> None:
    if language not in F20_RUNTIME_REQUIRED_LANGUAGES:
        return
    data = f20_runtime_baseline_data(root)
    if data is None:
        if lexical_absolute(root) == lexical_absolute(CANONICAL_WORKSPACE):
            raise BenchmarkError(
                f"F20.P1: trusted runtime facts are missing for {language}"
            )
        return
    if language not in (data.get("languages") or {}):
        raise BenchmarkError(
            f"F20.P1: trusted runtime facts omitted configured language {language}"
        )
    if record["level"] == "NONE":
        raise BenchmarkError(
            f"F20.P1: {language} cannot be NONE: the pinned runtime image "
            "demonstrates the numbered task under the exact frozen recipe"
        )
    if "P-b" in record["partial_reasons"]:
        raise BenchmarkError(
            f"F20.P1: {language} cannot cite P-b: the pinned runtime image "
            "demonstrates the mechanism with no extra compiler/runtime flags"
        )


def _semantic_nm_has_add2(output: str) -> bool:
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 2 or fields[-1] not in {"add2", "_add2"}:
            continue
        if fields[-2].upper() != "U":
            return True
    return False


def _semantic_nm_self_test(root: Path, cwd: Path) -> dict[str, Any]:
    positive = cwd / "nm_positive.c"
    negative = cwd / "nm_negative.c"
    positive.write_text("int add2(int a, int b) { return a + b; }\n", encoding="utf-8")
    negative.write_text("int other(int a, int b) { return a + b; }\n", encoding="utf-8")
    pos_build = _semantic_run_process(
        root, cwd, ["cc", "-c", positive.name, "-o", "nm_positive.o"],
        label="Semantic Compression nm positive-control build",
    )
    neg_build = _semantic_run_process(
        root, cwd, ["cc", "-c", negative.name, "-o", "nm_negative.o"],
        label="Semantic Compression nm negative-control build",
    )
    pos_nm = _semantic_run_process(
        root, cwd, ["nm", "nm_positive.o"],
        label="Semantic Compression nm positive control",
    )
    neg_nm = _semantic_run_process(
        root, cwd, ["nm", "nm_negative.o"],
        label="Semantic Compression nm negative control",
    )
    positive_ok = _semantic_nm_has_add2(pos_nm["stdout"])
    negative_ok = not _semantic_nm_has_add2(neg_nm["stdout"])
    if not positive_ok or not negative_ok:
        raise BenchmarkError(
            "Semantic Compression nm validator failed its mandatory positive/negative controls"
        )
    return {
        "positive_build": pos_build,
        "negative_build": neg_build,
        "positive_nm": pos_nm,
        "negative_nm": neg_nm,
        "positive_control_passed": positive_ok,
        "negative_control_passed": negative_ok,
    }


def validate_canonical_fragment_verification(
    root: Path,
    language: str,
    catalog: dict[str, dict[str, Any]],
    evidence: dict[str, Any],
) -> dict[str, Any]:
    """Mechanically enforce capability-universe R6 / pre-measurement V1."""
    verification = _semantic_verification_schema(catalog, evidence)
    synthetic_marker = bool(evidence.get("synthetic"))
    synthetic_allowed = (
        lexical_absolute(root) != lexical_absolute(CANONICAL_WORKSPACE)
        and os.environ.get("QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS") == "1"
    )
    if synthetic_marker and not synthetic_allowed:
        raise BenchmarkError(
            "synthetic canonical verification is allowed only in an explicit "
            "non-canonical CI workspace; scored workers may not self-declare it"
        )
    synthetic = synthetic_marker and synthetic_allowed
    if not synthetic:
        for probe_id, row in verification.items():
            _semantic_validate_real_fragment_files(
                language, probe_id, row
            )
    audit_suffix = ""
    if len(catalog) == 1:
        audit_suffix = "--" + slug_id(next(iter(catalog)))
    audit_path = (
        root / "work" / "audit" / "semantic-compression"
        / f"canonical_verification_{slug_id(language)}{audit_suffix}.json"
    )
    if synthetic:
        payload = {
            "schema_version": 1,
            "language": language,
            "synthetic_ci": True,
            "verified_probes": sorted(verification),
            "note": (
                "Synthetic CI validates the complete verification-fixture contract "
                "but never substitutes fake toolchain execution for paid-run evidence."
            ),
        }
        json_dump(audit_path, payload)
        return payload

    verify_root = (
        root / "work" / "root" / "commands"
        / f"sc-canonical-verification-{slug_id(language)}"
    )
    shutil.rmtree(verify_root, ignore_errors=True)
    verify_root.mkdir(parents=True, exist_ok=True)
    report: dict[str, Any] = {
        "schema_version": 1,
        "language": language,
        "synthetic_ci": False,
        "probes": {},
    }
    nm_self_test: dict[str, Any] | None = None
    stdout_oracles = semantic_fixed_stdout_oracles(root)

    for probe_id in sorted(verification):
        row = verification[probe_id]
        probe_dir = verify_root / slug_id(probe_id)
        probe_dir.mkdir(parents=True, exist_ok=True)
        for relative, content in row["files"].items():
            destination = require_under(
                probe_dir.joinpath(*PurePosixPath(relative).parts), probe_dir
            )
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content, encoding="utf-8")

        build_argv, run_argv, native_artifact = _semantic_verification_recipe(
            language, probe_id, row["entry_file"], row["files"]
        )
        probe_report: dict[str, Any] = {
            "mode": row["mode"],
            "run_count": row["run_count"],
            "canonical_fragment_sha256": sha256_bytes(
                str(catalog[probe_id]["fragment"]).encode("utf-8")
            ),
            "source_files": {
                name: sha256_bytes(content.encode("utf-8"))
                for name, content in sorted(row["files"].items())
            },
        }
        if build_argv is not None:
            probe_report["build"] = _semantic_run_process(
                root, probe_dir, build_argv,
                label=f"{language} {probe_id} frozen build recipe",
            )

        if row["mode"] == "nm-add2":
            if language not in SEMANTIC_NATIVE_NM_LANGUAGES or not native_artifact:
                raise BenchmarkError(
                    f"{probe_id}: {language} has no native artifact under the frozen "
                    "recipe, so it cannot be FULL/PARTIAL for the mandatory nm probe"
                )
            if nm_self_test is None:
                nm_self_test = _semantic_nm_self_test(root, verify_root)
                report["nm_validator_self_test"] = nm_self_test
            nm_record = _semantic_run_process(
                root, probe_dir, ["nm", native_artifact],
                label=f"{language} {probe_id} nm verification",
            )
            if not _semantic_nm_has_add2(nm_record["stdout"]):
                raise BenchmarkError(
                    f"{probe_id}: {language} built successfully but nm did not expose "
                    "a defined exact add2/_add2 symbol"
                )
            probe_report["nm"] = nm_record
            probe_report["symbol_add2_defined"] = True
        else:
            if run_argv is None:
                raise BenchmarkError(
                    f"{probe_id}: {language} frozen recipe has no runnable command"
                )
            expected_stdout = stdout_oracles.get(probe_id)
            if expected_stdout is not None:
                probe_report["expected_stdout"] = expected_stdout
            runs = []
            for index in range(row["run_count"]):
                run_record = _semantic_run_process(
                    root, probe_dir, run_argv,
                    label=(
                        f"{language} {probe_id} frozen run recipe"
                        + (f" #{index + 1}" if row["run_count"] > 1 else "")
                    ),
                )
                observed_stdout = str(run_record.get("stdout") or "").strip()
                if expected_stdout is not None and observed_stdout != expected_stdout:
                    raise BenchmarkError(
                        f"{language} {probe_id} observed stdout mismatch: "
                        f"expected {expected_stdout!r}, got {observed_stdout!r}"
                    )
                runs.append(run_record)
            probe_report["runs"] = runs
        report["probes"][probe_id] = probe_report

    report["verified_probe_count"] = len(report["probes"])
    json_dump(audit_path, report)
    return report



def validate_canonical_none_verification(
    root: Path,
    language: str,
    probe_id: str,
    record: dict[str, Any],
    evidence: dict[str, Any],
) -> None:
    raw = evidence.get("canonical_none_verification")
    row = raw.get(probe_id) if isinstance(raw, dict) else None
    if not isinstance(row, dict):
        raise BenchmarkError(
            f"{probe_id}: NONE requires evidence.canonical_none_verification"
        )
    if str(row.get("none_reason") or "") != str(record.get("none_reason") or ""):
        raise BenchmarkError(f"{probe_id}: NONE verification reason mismatch")
    citations = row.get("citations")
    if (
        not isinstance(citations, list)
        or not citations
        or not all(isinstance(value, str) and value.strip() for value in citations)
    ):
        raise BenchmarkError(
            f"{probe_id}: NONE verification requires authoritative citation text"
        )
    kinds = row.get("evidence_kinds")
    if (
        not isinstance(kinds, list)
        or not kinds
        or not all(isinstance(value, str) and value.strip() for value in kinds)
    ):
        raise BenchmarkError(
            f"{probe_id}: NONE verification requires evidence_kinds"
        )
    if row.get("generation_failures_not_used_as_evidence") is not True:
        raise BenchmarkError(
            f"{probe_id}: generation failure must never be treated as NONE evidence"
        )
    if not str(row.get("conclusion") or "").strip():
        raise BenchmarkError(f"{probe_id}: NONE verification conclusion is missing")
    if not str(record.get("justification") or "").strip():
        raise BenchmarkError(f"{probe_id}: NONE support justification is missing")


def validate_canonical_probe_projection(
    root: Path,
    language: str,
    probe_id: str,
    catalog: dict[str, dict[str, Any]],
    evidence: dict[str, Any],
) -> bool:
    metadata = evidence.get("probe_recertification")
    if not isinstance(metadata, dict):
        return False
    source_rel = str(metadata.get("source_record") or "")
    expected_hash = str(metadata.get("source_record_sha256") or "")
    rel = PurePosixPath(source_rel)
    if (
        not source_rel
        or rel.is_absolute()
        or any(part in {"", ".", ".."} for part in rel.parts)
    ):
        raise BenchmarkError(f"{probe_id}: invalid probe recertification source path")
    source_path = require_under(
        root / "cache" / Path(*rel.parts), root / "cache"
    )
    if not source_path.is_file() or sha256_file(source_path) != expected_hash:
        raise BenchmarkError(
            f"{probe_id}: probe recertification source provenance drifted"
        )
    source = json_load(source_path)
    problem = _cache_record_self_integrity_problem(source)
    if problem:
        raise BenchmarkError(
            f"{probe_id}: probe recertification source failed self-integrity: {problem}"
        )
    certification = source.get("certification") or {}
    if certification.get("validator_pass") is not True:
        raise BenchmarkError(
            f"{probe_id}: source owner was not current-validator certified"
        )
    if list(source.get("assigned_languages") or []) != [language]:
        raise BenchmarkError(
            f"{probe_id}: probe recertification source language mismatch"
        )
    source_catalog = canonical_fragment_catalog(
        root, source.get("result") or {}
    )
    if source_catalog.get(probe_id) != catalog.get(probe_id):
        raise BenchmarkError(
            f"{probe_id}: projected canonical record differs from certified source"
        )
    if str(metadata.get("source_result_sha256") or "") != str(
        source.get("result_sha256") or ""
    ):
        raise BenchmarkError(
            f"{probe_id}: projected source result digest drifted"
        )

    audit_path = (
        root / "work" / "audit" / "semantic-compression"
        / f"canonical_verification_{slug_id(language)}--{slug_id(probe_id)}.json"
    )
    record = catalog[probe_id]
    probe_rows: dict[str, Any] = {}
    if record["level"] in {"FULL", "PARTIAL"}:
        source_verification = (
            ((source.get("result") or {}).get("evidence") or {})
            .get("canonical_verification") or {}
        ).get(probe_id)
        if not isinstance(source_verification, dict):
            raise BenchmarkError(
                f"{probe_id}: certified source lacks canonical verification evidence"
            )
        probe_rows[probe_id] = {
            "mode": "probe-projection-from-current-owner",
            "canonical_fragment_sha256": sha256_bytes(
                str(record["fragment"]).encode("utf-8")
            ),
            "source_verification": source_verification,
        }
    json_dump(audit_path, {
        "schema_version": 1,
        "language": language,
        "probe_projection_from_current_owner": True,
        "legacy_evidence_recertified": True,
        "verification_mode": "probe-projection-from-current-owner",
        "mechanical_verification_performed": False,
        "source_record": source_rel,
        "source_record_sha256": expected_hash,
        "probes": probe_rows,
    })
    return True


def validate_canonical_fragment_owner_result(
    root: Path, task: dict[str, Any], result: dict[str, Any]
) -> None:
    """Validate exactly one probe×language canonical fragment/support leaf."""
    assigned = list(task.get("assigned_languages") or [])
    if len(assigned) != 1:
        raise BenchmarkError("canonical fragment owner must be language-sharded")
    language = str(assigned[0])
    probe_id = canonical_fragment_probe(
        [str(value) for value in (task.get("requirement_ids") or [])]
    )
    if probe_id is None:
        raise BenchmarkError(
            "canonical fragment owner must own exactly one canonical probe requirement"
        )
    catalog = canonical_fragment_catalog(root, result, {probe_id})
    record = catalog[probe_id]
    evidence = result.get("evidence") or {}

    if validate_canonical_probe_projection(
        root, language, probe_id, catalog, evidence
    ):
        if record["level"] == "NONE":
            validate_canonical_none_verification(
                root, language, probe_id, record, evidence
            )
        return

    if record["level"] == "NONE":
        raw_verification = evidence.get("canonical_verification")
        if raw_verification not in (None, {}):
            if isinstance(raw_verification, dict) and probe_id not in raw_verification:
                pass
            else:
                raise BenchmarkError(
                    f"{probe_id}: NONE must not carry a runnable fragment verification"
                )
        validate_canonical_none_verification(
            root, language, probe_id, record, evidence
        )
        return

    if probe_id == "F20.P1" and not bool(evidence.get("synthetic")):
        validate_f20_record_against_runtime_baseline(
            root, language, record
        )
    validate_canonical_fragment_verification(
        root, language, catalog, evidence
    )

def _fragment_values(node: Any, key: str = "") -> set[str]:
    found: set[str] = set()
    if isinstance(node, dict):
        for name, value in node.items():
            found |= _fragment_values(value, str(name))
    elif isinstance(node, list):
        for value in node:
            found |= _fragment_values(value, key)
    elif isinstance(node, str) and (key == "fragment" or key.endswith("_fragment")):
        if node.strip():
            found.add(node.strip())
    return found


def project_semantic_consumer_recertification(
    root: Path,
    unit: dict[str, Any],
    task: dict[str, Any],
    record: dict[str, Any],
    source_path: Path | None = None,
) -> tuple[dict[str, Any] | None, str | None]:
    """Rebind preserved SC metric shards to the current canonical catalog safely.

    Explicit historical fragments must still match byte-for-byte. Older shards
    that stored only raw metric counts may omit fragment text only when the
    recertified owner proves the exact same shared scientific experiment
    identity and owns the current catalog digest.
    """
    if str(unit.get("evaluation") or "") != "semantic_compression":
        return record, None
    if not str(unit.get("canonical_fragment_source_requirement") or ""):
        return record, None

    digest = str(task.get("canonical_fragment_catalog_sha256") or "")
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        return None, (
            f"{unit.get('id')}: current canonical-fragment consumer task "
            "has no frozen catalog digest"
        )

    catalog_path = None
    for raw in task.get("read_paths", []) or []:
        candidate = resolve_recorded_workspace_path(root, raw)
        if (
            candidate.name.startswith("canonical_fragments_")
            and candidate.suffix == ".json"
        ):
            catalog_path = candidate
            break
    if catalog_path is None or not catalog_path.is_file():
        return None, (
            f"{unit.get('id')}: current canonical-fragment catalog is not "
            "materialized yet"
        )
    if sha256_file(catalog_path) != digest:
        return None, (
            f"{unit.get('id')}: canonical-fragment catalog digest does not "
            "match the frozen task"
        )

    catalog_payload = json_load(catalog_path)
    catalog = catalog_payload.get("canonical_fragments") or {}
    if not isinstance(catalog, dict) or not catalog:
        return None, f"{unit.get('id')}: canonical-fragment catalog is empty"
    # task.canonical_fragment_catalog_sha256 is the serialized input-file hash.
    # Legacy owner metadata intentionally binds the canonical catalog object
    # itself. Keep those two hash domains separate instead of comparing them.
    catalog_content_digest = _legacy_sc_catalog_digest(catalog)

    assigned = [str(value) for value in (unit.get("assigned_languages") or [])]
    owner_meta = (
        _semantic_owner_legacy_metadata(root, assigned[0])
        if len(assigned) == 1
        else None
    )
    legacy_source: dict[str, Any] | None = None
    legacy_identity_sha: str | None = None
    source_rel: str | None = None
    source_hash: str | None = None
    if source_path is not None:
        try:
            legacy_source = json_load(source_path)
        except (OSError, json.JSONDecodeError, TypeError, ValueError):
            return None, f"{unit.get('id')}: legacy consumer source record is unreadable"
        problem = _cache_record_self_integrity_problem(legacy_source)
        if problem:
            return None, (
                f"{unit.get('id')}: legacy consumer source failed self-integrity: "
                f"{problem}"
            )
        identity = _legacy_sc_shared_experiment_identity(legacy_source)
        if identity:
            legacy_identity_sha = sha256_bytes(
                json.dumps(
                    identity,
                    sort_keys=True,
                    separators=(",", ":"),
                    ensure_ascii=False,
                ).encode("utf-8")
            )
        try:
            source_rel = source_path.relative_to(root / "cache").as_posix()
        except ValueError:
            return None, f"{unit.get('id')}: legacy consumer source is outside cache"
        source_hash = sha256_file(source_path)

    result = json.loads(json.dumps(record.get("result") or {}))

    def drop_probe_annotations(node: Any, probe: str) -> None:
        if isinstance(node, dict):
            node.pop(probe, None)
            for value in list(node.values()):
                drop_probe_annotations(value, probe)
        elif isinstance(node, list):
            node[:] = [
                value for value in node
                if not (
                    isinstance(value, dict)
                    and str(value.get("probe_id", value.get("probe", ""))) == probe
                )
            ]
            for value in node:
                drop_probe_annotations(value, probe)

    rows = probe_annotation_fields(result, set(catalog))
    used_owner_identity_join = False
    used_prior_equivalence = False
    dropped_now_none: list[str] = []
    for probe_id, raw_record in catalog.items():
        canonical = sc_adjudicated_record(raw_record)
        if canonical is None:
            return None, f"{unit.get('id')}: invalid canonical record {probe_id}"
        values = _fragment_values(rows.get(probe_id) or {})
        if canonical["level"] == "NONE":
            # NONE is scored through Capability Coverage, not as a synthetic
            # zero in Q. Historical quality evidence can retain a per-probe row
            # without embedding the fragment text itself (for example
            # Determinacy/Locality). Remove that row regardless of whether
            # _fragment_values() found text so the common-basis intersection
            # excludes every current-NONE probe exactly once.
            drop_probe_annotations(result.get("evidence") or {}, probe_id)
            dropped_now_none.append(probe_id)
            continue

        expected = str(canonical["fragment"]).strip()
        if values:
            if values != {expected}:
                return None, (
                    f"{unit.get('id')}: legacy {probe_id} fragment differs from "
                    "the current canonical fragment"
                )
            continue

        if len(assigned) != 1:
            return None, (
                f"{unit.get('id')}: legacy {probe_id} carries no explicit "
                "fragment and the consumer is not language-scoped"
            )
        prior_cert = record.get("certification") or {}
        if (
            prior_cert.get("semantic_legacy_consumer_recertified") is True
            and prior_cert.get("canonical_fragment_equivalence_proved") is True
        ):
            used_prior_equivalence = True
            continue
        if (
            not isinstance(owner_meta, dict)
            or not legacy_identity_sha
            or owner_meta.get("scientific_identity_sha256") != legacy_identity_sha
            or owner_meta.get("canonical_catalog_sha256")
            != catalog_content_digest
        ):
            return None, (
                f"{unit.get('id')}: legacy {probe_id} carries no explicit fragment "
                "and neither prior certified equivalence nor the current owner "
                "proves the same scientific experiment identity"
            )
        used_owner_identity_join = True

    evidence = result.get("evidence")
    if not isinstance(evidence, dict):
        return None, f"{unit.get('id')}: legacy result evidence is not an object"
    evidence["canonical_fragment_catalog_sha256"] = digest

    source_run = str((record.get("provenance") or {}).get("run_id") or "")
    proof_mode = (
        "prior-certified-equivalence-with-none-drop"
        if used_prior_equivalence or dropped_now_none
        else (
            "shared-experiment-owner-catalog"
            if used_owner_identity_join
            else "explicit-fragment-match"
        )
    )
    equivalence: dict[str, Any] = {
        "schema_version": 1,
        "proof_mode": proof_mode,
        "source_run_id": source_run,
        "canonical_fragment_catalog_sha256": digest,
        "canonical_fragment_catalog_content_sha256": catalog_content_digest,
        "all_explicit_fragments_matched": (
            not used_owner_identity_join
            and not used_prior_equivalence
            and not dropped_now_none
        ),
        "dropped_now_none_probes": sorted(dropped_now_none),
    }
    if source_rel is not None and source_hash is not None:
        equivalence["source_metric_record"] = source_rel
        equivalence["source_metric_record_sha256"] = source_hash
    if legacy_identity_sha is not None:
        equivalence["scientific_identity_sha256"] = legacy_identity_sha
    if isinstance(owner_meta, dict):
        equivalence["source_owner_record"] = owner_meta.get("source_owner_record")
        equivalence["source_owner_record_sha256"] = owner_meta.get(
            "source_owner_record_sha256"
        )
        equivalence["source_density_record"] = owner_meta.get(
            "source_density_record"
        )
        equivalence["source_density_record_sha256"] = owner_meta.get(
            "source_density_record_sha256"
        )
    evidence["legacy_fragment_equivalence"] = equivalence

    result_raw = json.dumps(
        result, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    projected = json.loads(json.dumps(record))
    projected["result"] = result
    projected["result_sha256"] = sha256_bytes(result_raw)
    projected["certification"] = {
        **(projected.get("certification") or {}),
        "canonical_fragment_equivalence_proved": True,
        "canonical_fragment_catalog_sha256": digest,
        "semantic_legacy_consumer_recertified": True,
    }
    return projected, None


# One-time Semantic Compression legacy recertification support.
#
# Fresh scored SC owners still have to provide executable canonical_verification
# fixtures and pass the full V1 build/run/nm contract.  The bridge below is only
# for preserved paid evidence from the explicitly allowed legacy epochs.  It
# reconstructs the current canonical catalog from the old Capability Coverage
# support ledger plus the old Semantic Density fragments, records exact source
# hashes, and lets the current validator re-check every structural/current
# invariant.  Missing historical fragments are filled only by the small,
# reviewable overrides below; they are never inferred from a score.
LEGACY_SC_FRAGMENT_OVERRIDES: dict[str, dict[str, str]] = {
    "Java": {
        "F01.P1": "final int n = 7;\nreturn n;",
        "F01.P2": "static long counter = 0;\nstatic final long LIMIT = 100;",
        "F02.P1": "int v;\nif (cond) { v = 5; } else { v = 9; }\nreturn v;",
        "F02.P2": "byte[] buf = new byte[16];\nbuf[0] = 1;\nreturn buf[0];",
        "F03.P1": "x++;",
        "F03.P2": "xs.set(1, 42);",
        "F04.P2": "List<Integer> window = Collections.unmodifiableList(xs);\nreturn window.get(0);",
        "F04.P3": "Obj first = new Obj(5);\nList<Obj> box = List.of(first);\nObj second = box.get(0);\nboolean same = first == second;\nreturn same;",
        "F05.P1": "List<Integer> x = new ArrayList<>(List.of(1, 2, 3));\nf(x);\nreturn x.get(0);",
        "F05.P3": "Function<Integer,Integer> neg = a -> k - a;\nList<Integer> ys = xs.stream().map(neg).toList();\nreturn ys.get(0);",
        "F06.P1": "static int mid(List<Integer> xs) { return xs.get(1); }\nList<Integer> xs = List.of(1, 2, 3);\nint y = mid(xs);\nreturn y;",
        "F06.P2": "record Pair(int q, int r) {}\nstatic Pair divmod2(int a, int b) { return new Pair(a / b, a % b); }\nPair p = divmod2(a, b);\nreturn p.q() + p.r();",
        "F07.P1": "int r = a * b + c;",
        "F07.P2": "int q = a / b;\nint m = a % b;\ndouble d = (double) a / b;",
        "F08.P1": "int m = Integer.MAX_VALUE;\nint o = m + 1;\nreturn o;",
        "F08.P2": "int q = (b == 0) ? 0 : a / b;\nreturn q;",
        "F09.P1": "boolean eq = s1.equals(s2);",
        "F09.P2": "xs.sort(Comparator.reverseOrder());\nboolean lt = a < b;",
        "F10.P1": "int small;\ntry { small = Math.toIntExact(big); } catch (ArithmeticException e) { small = 0; }\nreturn small;",
        "F10.P2": "double sum = i + d;",
        "F11.P1": "int e = xs.get(i);\nreturn e;",
        "F11.P2": "List<Integer> part = xs.subList(1, 4);\nreturn part.get(0);",
        "F12.P1": "Optional<Integer> o = Optional.empty();\nint n = o.orElse(0);\nreturn n;",
        "F12.P2": "Function<Integer,Integer> h = a -> a + 1;\nOptional<Integer> p = o.map(h);\nint result = p.orElse(0);\nreturn result;",
        "F13.P1": "static int parseTwice(String s) { return Integer.parseInt(s) * 2; }",
        "F13.P2": "int n;\ntry { n = Integer.parseInt(s); } catch (NumberFormatException e) { n = 0; }\nreturn n;",
        "F14.P1": "sealed interface Shape permits Circle, Rect {}\nrecord Circle(double r) implements Shape {}\nrecord Rect(double w, double h) implements Shape {}\nShape s = new Circle(2.0);",
        "F14.P2": "double area = switch (s) {\ncase Circle c -> Math.PI * c.r() * c.r();\ncase Rect r -> r.w() * r.h();\n};",
        "F14.P3": "interface Shape { double area(); }\nclass Tri implements Shape { public double area() { return 6.0; } }\nShape s = new Tri();\ndouble result = s.area();",
        "F15.P1": "static <T> T head(List<T> xs) { return xs.get(0); }\nint a = head(List.of(4, 5, 6));\nString b = head(List.of(\"p\", \"q\"));\nString result = Integer.toString(a) + b;",
        "F15.P2": "static <T extends Comparable<T>> T maxOf(T a, T b) { return a.compareTo(b) > 0 ? a : b; }\nint m = maxOf(3, 5);\nreturn m;",
        "F15.P3": "interface Named { String tag(); }\nclass A implements Named { public String tag() { return \"a\"; } }\nclass B implements Named { public String tag() { return \"b\"; } }\nList<Named> items = List.of(new A(), new B());\nreturn items.get(0).tag();",
        "F16.P1": "List<Integer> xs = List.of(1, 2, 3);\nint total = xs.stream().mapToInt(Integer::intValue).sum();\nreturn total;",
        "F16.P2": "Map<String,Integer> mp = new HashMap<>();\nmp.put(\"a\", 1);\nint total = 0;\nfor (var e : mp.entrySet()) total += e.getValue();\nint miss = mp.getOrDefault(\"b\", 0);\nreturn total + miss;",
        "F17.P1": "try (FileReader r = new FileReader(\"data.txt\")) {\n    return r.read();\n}",
        "F17.P2": "class Handle implements AutoCloseable { public void close() { released++; } }\ntry (Handle h = new Handle()) { }\nreturn released;",
        "F18.P1": "System.out.println(\"x\");",
        "F18.P2": "class Util { public static int pubAdd(int a, int b) { return a + b + secret(); } private static int secret() { return 1; } }\nint result = Util.pubAdd(2, 3);",
        "F19.P1": "Future<Integer> a = pool.submit(() -> 20);\nFuture<Integer> b = pool.submit(() -> 22);\nint sum = a.get() + b.get();\nreturn sum;",
        "F19.P2": "AtomicLong counter = new AtomicLong();\nThread a = new Thread(() -> { for (int i = 0; i < 1000; i++) counter.incrementAndGet(); });\nThread b = new Thread(() -> { for (int i = 0; i < 1000; i++) counter.incrementAndGet(); });\na.start(); b.start(); a.join(); b.join();\nreturn counter.get();",
        "F20.P1": "Linker linker = Linker.nativeLinker();\nMemorySegment symbol = linker.defaultLookup().findOrThrow(\"abs\");\nMethodHandle abs = linker.downcallHandle(symbol, FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.JAVA_INT));\nint result = (int) abs.invokeExact(-3);\nreturn result;",
    },
    "TypeScript": {
        "F04.P2": "const window: ReadonlyArray<number> = xs;\nreturn window[0];",
    },
    "Python": {
        "F20.P1": "import ctypes\nlibc = ctypes.CDLL(None)\nlibc.abs.argtypes = [ctypes.c_int]\nlibc.abs.restype = ctypes.c_int\nresult = libc.abs(ctypes.c_int(-3))\nreturn result",
    },
    "Kotlin": {
        "F20.P1": "val linker = Linker.nativeLinker()\nval symbol = linker.defaultLookup().findOrThrow(\"abs\")\nval abs = linker.downcallHandle(symbol, FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.JAVA_INT))\nval result = abs.invokeWithArguments(-3) as Int\nreturn result",
    },
}


LEGACY_SC_SUPPORT_OVERRIDES: dict[str, dict[str, dict[str, Any]]] = {
    "Quidra": {
        "F06.P2": {
            "level": "FULL",
            "reason": (
                "The current canonical task explicitly permits a small record/class "
                "as the normal multiple-result mechanism and requires one binding "
                "construct only when the language has such destructuring syntax. "
                "Quidra therefore satisfies the numbered task without a P-a "
                "substitution; the legacy PARTIAL judgment is FULL under the current rubric."
            ),
        },
        "F09.P2": {
            "level": "NONE",
            "none_reason": "N-1",
            "reason": (
                "Current Quidra documents only ascending array.sorted() and no "
                "comparator, key, reverse descriptor, or named reverse-ordering "
                "facility. The retained manual reversal loop is not the canonical "
                "task's required standard descending-order facility, and F09.P2 has "
                "no R9 substitution. The legacy PARTIAL/P-a judgment is therefore "
                "NONE under the current rubric."
            ),
        },
    },
    "TypeScript": {
        "F08.P1": {
            "level": "NONE",
            "none_reason": "N-1",
            "reason": (
                "TypeScript/JavaScript number is IEEE-754 binary64 rather than a "
                "signed 32-bit integer type. ToInt32 coercion through a bitwise "
                "operator is a conversion substitute for the required fixed-width "
                "binding, and F08.P1 has no R9 substitution. The prior FULL/P-a "
                "interpretations are therefore NONE under the current rubric."
            ),
        },
        "F13.P1": {
            "level": "NONE",
            "none_reason": "N-1",
            "reason": (
                "The standard Number/parseInt conversions report malformed input "
                "as NaN rather than a propagating failure. Adding an isNaN check "
                "handles and manufactures the failure locally, contrary to the "
                "canonical requirement to propagate the standard conversion's own "
                "failure without handling it. F13.P1 has no R9 substitution."
            ),
        },
        "F14.P2": {
            "level": "FULL",
            "reason": (
                "The retained discriminated-union switch delivers every numbered "
                "requirement with no default arm and computes both Circle and Rect "
                "areas. Compiler-enforced exhaustiveness is explicitly an OBSERVATION "
                "TARGET scored by B/D, not a Capability Coverage support requirement, "
                "so the legacy PARTIAL/P-c judgment double-counted determinacy."
            ),
        },
    },
    "Swift": {
        "F04.P1": {
            "level": "PARTIAL",
            "partial_reasons": ["P-e"],
            "reason": (
                "withUnsafeMutablePointer(to:) gives a documented writable pointer "
                "to the same scalar storage, but it requires a closure-scope scaffold "
                "not requested by F04.P1. The retained mechanism therefore remains "
                "PARTIAL under P-e only; P-a is not permitted for this probe."
            ),
        },
    },
    "Python": {
        "F01.P1": {
            "level": "PARTIAL",
            "partial_reasons": ["P-c"],
            "reason": (
                "Python can bind and return the required value but does not document "
                "a language-enforced never-reassignable local binding; the missing "
                "immutability guarantee is therefore PARTIAL under P-c, not P-a."
            ),
        },
        "F01.P2": {
            "level": "PARTIAL",
            "partial_reasons": ["P-c"],
            "reason": (
                "Python's module-level constant spelling is a documented convention "
                "rather than an enforced non-reassignability guarantee, so the "
                "guarantee is PARTIAL under P-c."
            ),
        },
        "F10.P1": {
            "level": "FULL",
            "reason": (
                "ctypes.c_int32 is a standard-library signed 32-bit representation "
                "and its explicit construction is a standard narrowing conversion. "
                "The unchanged preference ladder therefore reaches rung (ii) without "
                "a named-substitution exception."
            ),
        },
    },
    "Go": {
        "F04.P2": {
            "level": "NONE",
            "none_reason": "N-4",
            "reason": (
                "A Go slice is non-copying but not read-only. F04.P2 explicitly "
                "directs languages without a non-copying read-only view to write no "
                "fragment, so the old PARTIAL classification is NONE under N-4."
            ),
        },
    },
    "Zig": {
        "F08.P2": {
            "level": "FULL",
            "reason": (
                "The unchanged canonical task explicitly makes a local b == 0 guard "
                "preference rung (ii). The retained guard therefore delivers every "
                "numbered requirement and is FULL, not a P-a substitution."
            ),
        },
        "F14.P3": {
            "level": "FULL",
            "reason": (
                "The current canonical task requires an extensible abstract operation "
                "across a separate unit; it does not require a built-in nominal "
                "interface keyword. Zig's documented type-erasure/vtable idiom using "
                "*anyopaque plus function pointers lets the shapes unit define the "
                "abstract Shape value while a separate unit adds Tri without editing "
                "shapes and dispatches area at runtime. All numbered requirements are "
                "therefore delivered without an R9 substitution."
            ),
        },
        "F15.P3": {
            "level": "FULL",
            "reason": (
                "The current canonical task asks for one declared abstract element "
                "type that can hold A and B and dispatch tag at runtime; it does not "
                "require a built-in interface construct. A Zig type-erased Named "
                "value with *anyopaque context plus a tag function pointer is one "
                "declared element type, can populate a heterogeneous sequence, and "
                "dispatches to the concrete implementation. The legacy PARTIAL/P-a "
                "classification therefore treated the ordinary vtable idiom as a "
                "substitute when it actually satisfies every numbered requirement."
            ),
        },
    },
}


def _legacy_sc_codes(node: Any, prefix: str) -> list[str]:
    pattern = re.compile(rf"\b{re.escape(prefix)}-[a-e1-4]\b")
    found: set[str] = set()

    def visit(value: Any) -> None:
        if isinstance(value, dict):
            for key, item in value.items():
                visit(key)
                visit(item)
        elif isinstance(value, list):
            for item in value:
                visit(item)
        elif isinstance(value, str):
            for match in pattern.findall(value):
                found.add(match)

    visit(node)
    return sorted(found)


def _legacy_sc_first_text(node: Any, keys: tuple[str, ...]) -> str:
    wanted = {key.lower() for key in keys}
    found: list[str] = []

    def visit(value: Any) -> None:
        if isinstance(value, dict):
            for key, item in value.items():
                if str(key).lower() in wanted and isinstance(item, str) and item.strip():
                    found.append(item.strip())
                visit(item)
        elif isinstance(value, list):
            for item in value:
                visit(item)

    visit(node)
    return found[0] if found else ""


LEGACY_SC_SOURCE_SNAPSHOTS: dict[str, str] = {
    "2026-09-22-56f2c65-gh3": "56f2c65cf938edce1de6f038148cf660eba9dd2b",
    "2026-09-23-402117e-gh11": "402117e2d4a15ca96ea92fa2a911a10e6ae35bf2",
    "2026-09-23-1231af5-gh12": "1231af5b6bdb6e525a303e2d79307d016f3bcc36",
    "2026-09-23-fce5cfa-gh16": "fce5cfa731cbf735dca4200257d4c54e084aacc4",
    "2026-09-23-61f50c1-gh22": "61f50c1a8c1518f6d6abcb9b1468e4a9e0aa0610",
}


# Reviewed projection bridge for the retained paid SC snapshots above.
#
# These five immutable snapshots all used the same full Primary JSON and the
# same full Semantic Compression specification.  The later cache schema hashes
# only the evaluation-visible Primary projection and selected methodology
# sections.  A full-file digest may cross that naming/projection migration only
# through this table, and only when the current projected digest is exactly the
# reviewed projection below.  This is not a generic ignored hash.
LEGACY_SC_INPUT_PROJECTION_MIGRATIONS: dict[str, dict[str, str]] = {
    run_id: {
        "source_snapshot_commit": snapshot,
        "primary_config_full_sha256":
            "aef74df02a01e9c5ccc2e9222ef12c8644476d8d6ce3f5dda194d1fcf48945f9",
        "primary_config_projection_sha256":
            "7a5bd4e4f93549b367e78316343f7e77724101205ef962105522ef04a67ca819",
        "evaluation_spec_full_sha256":
            "33d550d70ced707a0f6fcc0641ec08142a6257fdeb35a5f889773badc4660c92",
    }
    for run_id, snapshot in LEGACY_SC_SOURCE_SNAPSHOTS.items()
}



LEGACY_SC_SOURCE_PROVENANCE_ROOT = PurePosixPath(
    "provenance/semantic-compression"
)

# The retained SC snapshots encode these two guard magnitudes as JSON floats
# (8.0 / 3.0), while the current Primary file writes the mathematically
# identical values as integers (8 / 3). Python's json round-trip preserves that
# int-vs-float distinction in a projection hash even though the runner consumes
# both as the same numeric magnitude. Normalize only these reviewed legacy paths;
# every other Primary field remains byte/semantic sensitive.
LEGACY_SC_INTEGRAL_NUMBER_EQUIVALENCE_PATHS = (
    ("worker_isolation", "task_spend_guard", "floor_usd"),
    ("worker_isolation", "task_spend_guard", "envelope_multiplier"),
)


def _legacy_sc_primary_projection_data(
    config: dict[str, Any],
) -> dict[str, Any]:
    projected = json.loads(
        json.dumps(
            primary_config_projection_from_data(
                config, "semantic_compression"
            ),
            ensure_ascii=False,
        )
    )
    for parts in LEGACY_SC_INTEGRAL_NUMBER_EQUIVALENCE_PATHS:
        parent: dict[str, Any] | None = projected
        for part in parts[:-1]:
            value = parent.get(part) if isinstance(parent, dict) else None
            if not isinstance(value, dict):
                parent = None
                break
            parent = value
        if parent is None:
            continue
        key = parts[-1]
        value = parent.get(key)
        if isinstance(value, float) and value.is_integer():
            parent[key] = int(value)
    return projected


def _legacy_sc_source_projection_metadata_path(root: Path, run_id: str) -> Path:
    return (
        root / "cache" / Path(*LEGACY_SC_SOURCE_PROVENANCE_ROOT.parts)
        / "runs" / f"{slug_id(run_id)}.json"
    )


def _legacy_sc_source_projection_file(
    root: Path, raw: str
) -> Path | None:
    rel = PurePosixPath(str(raw or ""))
    if (
        not raw
        or rel.is_absolute()
        or any(part in {"", ".", ".."} for part in rel.parts)
    ):
        return None
    try:
        return require_under(root / "cache" / Path(*rel.parts), root / "cache")
    except BenchmarkError:
        return None


def record_legacy_sc_source_projection_provenance(
    source: Path, evidence: Path, snapshot: str
) -> dict[str, Any] | None:
    """Preserve exact public source bytes used to justify SC hash projection."""
    run = json_load(evidence / "run.json")
    run_id = str(run.get("run_id") or "")
    expected_snapshot = LEGACY_SC_SOURCE_SNAPSHOTS.get(run_id)
    if expected_snapshot is None:
        return None
    if snapshot != expected_snapshot:
        raise BenchmarkError(
            f"{run_id}: Semantic Compression source snapshot mismatch: "
            f"{snapshot} != {expected_snapshot}"
        )

    primary = evidence / "template" / "config" / "primary.json"
    spec = (
        evidence / "template" / "methodology"
        / EVALUATION_SPEC_FILES["semantic_compression"]
    )
    if not primary.is_file() or not spec.is_file():
        raise BenchmarkError(
            f"{run_id}: recovered Semantic Compression source inputs are missing"
        )

    migration = LEGACY_SC_INPUT_PROJECTION_MIGRATIONS.get(run_id)
    if migration is None:
        raise BenchmarkError(f"{run_id}: no reviewed SC projection migration exists")
    primary_full = sha256_file(primary)
    spec_full = sha256_file(spec)
    primary_projection = sha256_bytes(
        json.dumps(
            _legacy_sc_primary_projection_data(
                json_load(primary)
            ),
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=False,
        ).encode("utf-8")
    )
    if (
        primary_full != migration.get("primary_config_full_sha256")
        or spec_full != migration.get("evaluation_spec_full_sha256")
        or primary_projection
        != migration.get("primary_config_projection_sha256")
    ):
        raise BenchmarkError(
            f"{run_id}: recovered source bytes do not match the reviewed SC migration"
        )

    cache_root = source / "benchmark" / "cache"
    provenance_root = (
        cache_root / Path(*LEGACY_SC_SOURCE_PROVENANCE_ROOT.parts)
    )
    files_root = provenance_root / "source-files"
    runs_root = provenance_root / "runs"
    files_root.mkdir(parents=True, exist_ok=True)
    runs_root.mkdir(parents=True, exist_ok=True)
    primary_rel = (
        LEGACY_SC_SOURCE_PROVENANCE_ROOT / "source-files"
        / f"{primary_full}.primary.json"
    )
    spec_rel = (
        LEGACY_SC_SOURCE_PROVENANCE_ROOT / "source-files"
        / f"{spec_full}.semantic_compression.md"
    )

    for source_path, relative in ((primary, primary_rel), (spec, spec_rel)):
        destination = cache_root / Path(*relative.parts)
        source_bytes = source_path.read_bytes()
        if destination.exists():
            if destination.read_bytes() != source_bytes:
                raise BenchmarkError(
                    f"{run_id}: preserved SC source provenance drifted: {relative}"
                )
        else:
            destination.write_bytes(source_bytes)

    metadata = {
        "schema_version": 1,
        "run_id": run_id,
        "source_snapshot_commit": snapshot,
        "source_primary_config": primary_rel.as_posix(),
        "source_primary_config_sha256": primary_full,
        "source_primary_config_sc_projection_sha256": primary_projection,
        "source_evaluation_spec": spec_rel.as_posix(),
        "source_evaluation_spec_sha256": spec_full,
        "migration_rule": "semantic-source-snapshot-projection-v1",
        "migration_rule_version": 1,
    }
    metadata_path = runs_root / f"{slug_id(run_id)}.json"
    if metadata_path.exists():
        existing = json_load(metadata_path)
        if existing != metadata:
            raise BenchmarkError(
                f"{run_id}: preserved SC source projection metadata drifted"
            )
    else:
        json_dump(metadata_path, metadata)
    return metadata


def _legacy_sc_source_projection_attestation(
    root: Path, run_id: str, selectors: list[str]
) -> dict[str, str] | None:
    """Recompute legacy full-file -> current scoped hashes from snapshot bytes."""
    expected_snapshot = LEGACY_SC_SOURCE_SNAPSHOTS.get(run_id)
    migration = LEGACY_SC_INPUT_PROJECTION_MIGRATIONS.get(run_id)
    if expected_snapshot is None or migration is None:
        return None
    path = _legacy_sc_source_projection_metadata_path(root, run_id)
    if not path.is_file():
        return None
    try:
        metadata = json_load(path)
    except (OSError, json.JSONDecodeError, TypeError, ValueError):
        return None
    if (
        metadata.get("schema_version") != 1
        or metadata.get("run_id") != run_id
        or metadata.get("source_snapshot_commit") != expected_snapshot
        or metadata.get("migration_rule")
        != "semantic-source-snapshot-projection-v1"
        or metadata.get("migration_rule_version") != 1
    ):
        return None

    primary = _legacy_sc_source_projection_file(
        root, str(metadata.get("source_primary_config") or "")
    )
    spec = _legacy_sc_source_projection_file(
        root, str(metadata.get("source_evaluation_spec") or "")
    )
    if primary is None or spec is None or not primary.is_file() or not spec.is_file():
        return None
    try:
        primary_full = sha256_file(primary)
        spec_full = sha256_file(spec)
        primary_projection = sha256_bytes(
            json.dumps(
                _legacy_sc_primary_projection_data(
                json_load(primary)
            ),
                sort_keys=True,
                separators=(",", ":"),
                ensure_ascii=False,
            ).encode("utf-8")
        )
        spec_projection = sha256_bytes(
            extract_markdown_sections(
                spec.read_text(encoding="utf-8"), selectors
            ).encode("utf-8")
        )
    except (OSError, json.JSONDecodeError, TypeError, ValueError, BenchmarkError):
        return None

    if (
        primary_full != metadata.get("source_primary_config_sha256")
        or primary_full != migration.get("primary_config_full_sha256")
        or primary_projection
        != metadata.get("source_primary_config_sc_projection_sha256")
        or primary_projection
        != migration.get("primary_config_projection_sha256")
        or spec_full != metadata.get("source_evaluation_spec_sha256")
        or spec_full != migration.get("evaluation_spec_full_sha256")
    ):
        return None
    return {
        "source_snapshot_commit": expected_snapshot,
        "primary_config_full_sha256": primary_full,
        "primary_config_projection_sha256": primary_projection,
        "evaluation_spec_full_sha256": spec_full,
        "evaluation_spec_projection_sha256": spec_projection,
    }


LEGACY_SC_INPUT_HASH_PROJECTIONS: dict[str, dict[str, str]] = {
    # Explicit one-time projections proved from the approved retained paid
    # snapshots. The source digest is the historical whole-file SHA-256; the
    # target digest is the current Semantic Compression-visible projection of
    # that exact source. Unknown digests are never rewritten or ignored.
    "primary_config": {
        "aef74df02a01e9c5ccc2e9222ef12c8644476d8d6ce3f5dda194d1fcf48945f9":
            "7a5bd4e4f93549b367e78316343f7e77724101205ef962105522ef04a67ca819",
    },
    "evaluation_spec": {
        "33d550d70ced707a0f6fcc0641ec08142a6257fdeb35a5f889773badc4660c92":
            "d8697b825f009cc0517e1bb8f64a4ca31a35163c847261d6a3611c9db65a9b8a",
    },
}


def _normalize_sc_evaluation_spec_hash_aliases(
    hashes: dict[str, Any],
) -> dict[str, Any]:
    """Normalize only reviewed legacy SC whole-file -> scoped projections."""
    normalized = dict(hashes)

    primary = normalized.get("primary_config")
    if isinstance(primary, str):
        normalized["primary_config"] = (
            LEGACY_SC_INPUT_HASH_PROJECTIONS["primary_config"].get(
                primary, primary
            )
        )

    legacy = normalized.get("evaluation_spec")
    current = normalized.get("evaluation_spec_sections")
    if legacy is None:
        return normalized
    if not isinstance(legacy, str) or not re.fullmatch(r"[0-9a-f]{64}", legacy):
        return normalized

    projected_legacy = LEGACY_SC_INPUT_HASH_PROJECTIONS[
        "evaluation_spec"
    ].get(legacy, legacy)
    if current is not None:
        if (
            not isinstance(current, str)
            or not re.fullmatch(r"[0-9a-f]{64}", current)
            or current != projected_legacy
        ):
            # Conflicting declarations stay distinct and therefore fail the
            # fingerprint compatibility comparison.
            return normalized
        digest = current
    else:
        digest = projected_legacy

    normalized.pop("evaluation_spec", None)
    normalized.pop("evaluation_spec_sections", None)
    normalized["evaluation_spec_sections"] = digest
    return normalized


def _legacy_sc_shared_experiment_identity(
    record: dict[str, Any],
) -> dict[str, Any]:
    payload = json.loads(json.dumps(record.get("fingerprint_payload") or {}))
    if str(payload.get("evaluation") or "") != "semantic_compression":
        return {}
    for field in (
        "work_unit_id",
        "requirement_ids",
        "exact_task_packet_sha256",
        "validator_contract",
    ):
        payload.pop(field, None)
    payload["unit_input_hashes"] = _normalize_sc_evaluation_spec_hash_aliases(
        dict(payload.get("unit_input_hashes") or {})
    )
    return payload


def _legacy_sc_find_source_record(
    root: Path,
    language: str,
    work_unit_prefix: str,
    *,
    run_id: str | None = None,
    experiment_identity: dict[str, Any] | None = None,
) -> tuple[Path | None, dict[str, Any] | None]:
    directory = (
        root / "cache" / "v1" / "semantic-compression" / slug_id(language)
    )
    if not directory.is_dir():
        return None, None
    matches: list[tuple[str, str, Path, dict[str, Any]]] = []
    for path in sorted(directory.glob("*.json")):
        try:
            record = json_load(path)
        except (OSError, json.JSONDecodeError, TypeError, ValueError):
            continue
        if _cache_record_self_integrity_problem(record):
            continue
        certification = record.get("certification") or {}
        if (
            certification.get("unit_complete") is not True
            or certification.get("validator_pass") is not True
        ):
            continue
        if str(record.get("evaluation") or "") != "semantic_compression":
            continue
        if list(record.get("assigned_languages") or []) != [language]:
            continue
        provenance = record.get("provenance") or {}
        uid = str(provenance.get("work_unit_id") or "")
        if not uid.startswith(work_unit_prefix):
            continue
        source_run = str(provenance.get("run_id") or "")
        if run_id is not None and source_run != run_id:
            continue
        if (
            experiment_identity is not None
            and _legacy_sc_shared_experiment_identity(record)
            != experiment_identity
        ):
            continue
        matches.append((source_run, path.name, path, record))
    if not matches:
        return None, None
    _, _, path, record = sorted(matches)[-1]
    return path, record


def _legacy_sc_fragment_for_probe(
    language: str,
    probe_id: str,
    density_rows: dict[str, dict[str, Any]],
) -> tuple[str | None, str]:
    values = _fragment_values(density_rows.get(probe_id) or {})
    if len(values) == 1:
        return next(iter(values)), "legacy-semantic-density"
    override = (LEGACY_SC_FRAGMENT_OVERRIDES.get(language) or {}).get(probe_id)
    if override:
        return override.strip(), "reviewed-current-recertification-override"
    return None, "missing"


def _legacy_sc_catalog_digest(catalog: dict[str, dict[str, Any]]) -> str:
    return sha256_bytes(
        json.dumps(
            catalog, sort_keys=True, separators=(",", ":"), ensure_ascii=False
        ).encode("utf-8")
    )


def validate_legacy_canonical_fragment_recertification(
    root: Path,
    language: str,
    catalog: dict[str, dict[str, Any]],
    evidence: dict[str, Any],
) -> dict[str, Any]:
    metadata = evidence.get("legacy_recertification")
    if not isinstance(metadata, dict) or metadata.get("schema_version") != 1:
        raise BenchmarkError(
            "legacy Semantic Compression owner recertification metadata is missing"
        )
    if metadata.get("mode") != "paid-evidence-current-schema-v1":
        raise BenchmarkError("unknown legacy Semantic Compression recertification mode")
    if metadata.get("recertifier") != "GPT-5.6 Sol":
        raise BenchmarkError("legacy Semantic Compression recertifier identity drifted")
    if metadata.get("verdict") != "PASS":
        raise BenchmarkError("legacy Semantic Compression recertification verdict is not PASS")
    if metadata.get("current_sc_epoch") != cache_epoch(root, "semantic_compression"):
        raise BenchmarkError("legacy Semantic Compression epoch drifted")
    current_rubric_path = (
        root / "template" / "methodology-assets" / "semantic_compression"
        / "capability_universe.json"
    )
    if metadata.get("current_rubric_sha256") != sha256_file(current_rubric_path):
        raise BenchmarkError("legacy Semantic Compression current rubric hash drifted")
    expected_source_snapshot = LEGACY_SC_SOURCE_SNAPSHOTS.get(
        str(metadata.get("source_run_id") or "")
    )
    if (
        expected_source_snapshot is None
        or metadata.get("source_snapshot_commit") != expected_source_snapshot
    ):
        raise BenchmarkError("legacy Semantic Compression source snapshot drifted")
    expected_digest = _legacy_sc_catalog_digest(catalog)
    if metadata.get("canonical_catalog_sha256") != expected_digest:
        raise BenchmarkError("legacy Semantic Compression canonical catalog hash drifted")

    source_records: dict[str, dict[str, Any]] = {}
    for key in ("source_owner_record", "source_density_record"):
        raw = str(metadata.get(key) or "")
        rel = PurePosixPath(raw)
        if (
            not raw
            or rel.is_absolute()
            or any(part in {"", ".", ".."} for part in rel.parts)
        ):
            raise BenchmarkError(f"legacy Semantic Compression {key} is invalid")
        path = require_under(root / "cache" / Path(*rel.parts), root / "cache")
        if not path.is_file():
            raise BenchmarkError(f"legacy Semantic Compression {key} is missing")
        expected_hash = str(metadata.get(key + "_sha256") or "")
        if sha256_file(path) != expected_hash:
            raise BenchmarkError(f"legacy Semantic Compression {key} hash drifted")
        source_records[key] = json_load(path)

    density_run = str(metadata.get("source_density_run_id") or "")
    expected_density_snapshot = LEGACY_SC_SOURCE_SNAPSHOTS.get(density_run)
    if (
        expected_density_snapshot is None
        or metadata.get("source_density_snapshot_commit")
        != expected_density_snapshot
    ):
        raise BenchmarkError("legacy Semantic Compression density snapshot drifted")
    owner_identity = _legacy_sc_shared_experiment_identity(
        source_records["source_owner_record"]
    )
    density_identity = _legacy_sc_shared_experiment_identity(
        source_records["source_density_record"]
    )
    if not owner_identity or density_identity != owner_identity:
        raise BenchmarkError(
            "legacy Semantic Compression source scientific identity mismatch"
        )
    identity_sha = sha256_bytes(
        json.dumps(
            owner_identity,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=False,
        ).encode("utf-8")
    )
    if metadata.get("scientific_identity_sha256") != identity_sha:
        raise BenchmarkError(
            "legacy Semantic Compression scientific identity hash drifted"
        )
    if metadata.get("migration_rule") != "semantic-legacy-llm-recertification":
        raise BenchmarkError("legacy Semantic Compression migration rule drifted")
    if metadata.get("migration_rule_version") != CACHE_MIGRATION_RULE_VERSION:
        raise BenchmarkError(
            "legacy Semantic Compression migration rule version drifted"
        )
    selection = str(metadata.get("density_source_selection") or "")
    owner_run = str(metadata.get("source_run_id") or "")
    if selection == "same-run":
        if density_run != owner_run:
            raise BenchmarkError(
                "legacy Semantic Compression same-run density provenance drifted"
            )
    elif selection == "cross-run-scientific-identity":
        if density_run == owner_run:
            raise BenchmarkError(
                "legacy Semantic Compression cross-run provenance is not cross-run"
            )
    else:
        raise BenchmarkError(
            "legacy Semantic Compression density source selection is invalid"
        )

    verification = evidence.get("canonical_verification")
    if not isinstance(verification, dict):
        raise BenchmarkError(
            "legacy Semantic Compression recertification must write canonical_verification"
        )
    expected = {
        probe_id
        for probe_id, record in catalog.items()
        if record.get("level") in {"FULL", "PARTIAL"}
    }
    if set(verification) != expected:
        raise BenchmarkError(
            "legacy Semantic Compression canonical_verification coverage mismatch"
        )
    for probe_id in sorted(expected):
        row = verification.get(probe_id)
        if not isinstance(row, dict) or row.get("mode") != "legacy-evidence-recertification":
            raise BenchmarkError(
                f"{probe_id}: invalid legacy Semantic Compression verification row"
            )
        expected_fragment_sha = sha256_bytes(
            str(catalog[probe_id]["fragment"]).encode("utf-8")
        )
        if row.get("canonical_fragment_sha256") != expected_fragment_sha:
            raise BenchmarkError(
                f"{probe_id}: legacy Semantic Compression fragment hash mismatch"
            )
        fragment_origin = str(row.get("fragment_origin") or "")
        if fragment_origin == "legacy-semantic-density":
            if (
                row.get("source_run_id") != density_run
                or row.get("source_record") != metadata.get("source_density_record")
                or row.get("source_record_sha256")
                != metadata.get("source_density_record_sha256")
            ):
                raise BenchmarkError(
                    f"{probe_id}: legacy Semantic Compression density fragment "
                    "provenance drifted"
                )
        elif fragment_origin == "reviewed-current-recertification-override":
            expected_override = (
                (LEGACY_SC_FRAGMENT_OVERRIDES.get(language) or {}).get(probe_id)
                or ""
            ).strip()
            if (
                row.get("recertifier") != "GPT-5.6 Sol"
                or row.get("source") != "LEGACY_SC_FRAGMENT_OVERRIDES"
                or expected_override != str(catalog[probe_id]["fragment"]).strip()
            ):
                raise BenchmarkError(
                    f"{probe_id}: legacy Semantic Compression reviewed fragment "
                    "override provenance drifted"
                )
        else:
            raise BenchmarkError(
                f"{probe_id}: unknown legacy Semantic Compression fragment origin"
            )

    audit_path = (
        root / "work" / "audit" / "semantic-compression"
        / f"canonical_verification_{slug_id(language)}.json"
    )
    if not audit_path.is_file():
        raise BenchmarkError(
            f"legacy Semantic Compression runner attestation is missing for {language}"
        )
    audit = json_load(audit_path)
    if (
        audit.get("schema_version") != 1
        or audit.get("language") != language
        or audit.get("legacy_evidence_recertified") is not True
        or audit.get("verification_mode") != "legacy-evidence-recertification"
        or audit.get("mechanical_verification_performed") is not False
        or audit.get("canonical_catalog_sha256") != expected_digest
        or audit.get("source_owner_record") != metadata.get("source_owner_record")
        or audit.get("source_owner_record_sha256")
        != metadata.get("source_owner_record_sha256")
        or audit.get("source_density_record") != metadata.get("source_density_record")
        or audit.get("source_density_record_sha256")
        != metadata.get("source_density_record_sha256")
        or audit.get("source_density_run_id") != density_run
        or audit.get("source_density_snapshot_commit")
        != expected_density_snapshot
        or audit.get("scientific_identity_sha256") != identity_sha
        or audit.get("probes") != verification
    ):
        raise BenchmarkError(
            f"legacy Semantic Compression runner attestation is stale for {language}"
        )
    return audit


def project_semantic_owner_recertification(
    root: Path,
    unit: dict[str, Any],
    record: dict[str, Any],
    source_path: Path,
) -> tuple[dict[str, Any] | None, str | None]:
    if not unit.get("canonical_fragment_owner"):
        return record, None
    assigned = [str(value) for value in (unit.get("assigned_languages") or [])]
    if len(assigned) != 1:
        return None, "legacy Semantic Compression owner must be language-scoped"
    language = assigned[0]
    try:
        source_record = json_load(source_path)
    except (OSError, json.JSONDecodeError, TypeError, ValueError) as exc:
        return None, (
            f"{unit.get('id')}: legacy owner source is unreadable: "
            f"{type(exc).__name__}: {exc}"
        )
    source_run = str((source_record.get("provenance") or {}).get("run_id") or "")
    experiment_identity = _legacy_sc_shared_experiment_identity(source_record)
    if not experiment_identity:
        return None, (
            f"{unit.get('id')}: legacy owner has no valid Semantic Compression "
            "scientific experiment identity"
        )
    density_path, density_record = _legacy_sc_find_source_record(
        root,
        language,
        "sc-metrics-local--part-1--",
        run_id=source_run,
        experiment_identity=experiment_identity,
    )
    density_source_selection = "same-run"
    if density_path is None or density_record is None:
        density_path, density_record = _legacy_sc_find_source_record(
            root,
            language,
            "sc-metrics-local--part-1--",
            experiment_identity=experiment_identity,
        )
        density_source_selection = "cross-run-scientific-identity"
    if density_path is None or density_record is None:
        return None, (
            f"{unit.get('id')}: no certified legacy Semantic Density record has "
            "the same scientific experiment identity as the owner"
        )
    density_run = str(
        (density_record.get("provenance") or {}).get("run_id") or ""
    )

    matrix = json_load(
        root / "template" / "methodology-assets" / "semantic_compression"
        / "semantic_site_matrix.json"
    )
    probe_ids = [str(probe.get("probe_id") or "") for probe in matrix.get("probes", [])]
    owner_rows = probe_annotation_fields(
        source_record.get("result") or {}, set(probe_ids)
    )
    density_rows = probe_annotation_fields(
        density_record.get("result") or {}, set(probe_ids)
    )
    catalog: dict[str, dict[str, Any]] = {}
    verification: dict[str, dict[str, Any]] = {}
    current_runtime_override = set(F20_RUNTIME_REQUIRED_LANGUAGES)

    source_rel = source_path.relative_to(root / "cache")
    density_rel = density_path.relative_to(root / "cache")
    for probe_id in probe_ids:
        row = owner_rows.get(probe_id) or {}
        levels = sc_owner_support_levels(
            row, ["support", "support_level", "level"]
        )
        if len(levels) != 1:
            return None, (
                f"{unit.get('id')}: legacy support is not determinate for {probe_id}"
            )
        level = next(iter(levels))
        partial = [code for code in _legacy_sc_codes(row, "P") if code in SC_PARTIAL_REASONS]
        none_codes = [code for code in _legacy_sc_codes(row, "N") if code in SC_NONE_REASONS]

        if probe_id == "F20.P1" and language in current_runtime_override:
            level = "FULL"
            partial = []
            none_codes = []

        reviewed_override = (
            (LEGACY_SC_SUPPORT_OVERRIDES.get(language) or {}).get(probe_id) or {}
        )
        if reviewed_override:
            level = str(reviewed_override.get("level") or level).upper()
            partial = [
                str(value)
                for value in (reviewed_override.get("partial_reasons") or [])
            ]
            override_none = str(reviewed_override.get("none_reason") or "")
            none_codes = [override_none] if override_none else []

        fragment = None
        fragment_origin = "none"
        if level in {"FULL", "PARTIAL"}:
            fragment, fragment_origin = _legacy_sc_fragment_for_probe(
                language, probe_id, density_rows
            )
            if fragment is None:
                return None, (
                    f"{unit.get('id')}: no recoverable fragment exists for {probe_id}"
                )

        if level == "PARTIAL" and not partial:
            text = json.dumps(row, ensure_ascii=False)
            partial = [
                code for code in sorted(SC_PARTIAL_REASONS) if code in text
            ]
            if not partial:
                if probe_id in sc_p_a_allowed_probes(root):
                    partial = ["P-a"]
                else:
                    return None, (
                        f"{unit.get('id')}: PARTIAL {probe_id} has no recoverable "
                        "current reason code"
                    )
        none_reason = None
        if level == "NONE":
            none_reason = none_codes[0] if none_codes else "N-1"

        justification = str(reviewed_override.get("reason") or "").strip()
        if not justification:
            justification = _legacy_sc_first_text(
                row, ("justification", "rationale", "note", "notes", "reason", "citation")
            )
        if not justification:
            justification = (
                "Recovered from the preserved paid Capability Coverage support "
                "record and revalidated under the current support contract."
            )
        source_citation = _legacy_sc_first_text(row, ("citation",))
        citation = source_citation or (
            f"legacy-cache:{source_rel.as_posix()}#{probe_id}"
        )
        canonical = {
            "level": level,
            "fragment": fragment,
            "partial_reasons": partial if level == "PARTIAL" else [],
            "none_reason": none_reason,
            "justification": justification,
            "citation": citation,
        }
        validate_sc_record_for_probe(
            root, probe_id, canonical,
            context=f"legacy recertified canonical fragment {probe_id}",
        )
        catalog[probe_id] = canonical
        if level in {"FULL", "PARTIAL"}:
            verification_row = {
                "mode": "legacy-evidence-recertification",
                "canonical_fragment_sha256": sha256_bytes(
                    str(fragment).encode("utf-8")
                ),
                "fragment_origin": fragment_origin,
            }
            if fragment_origin == "legacy-semantic-density":
                verification_row.update({
                    "source_run_id": density_run,
                    "source_record": density_rel.as_posix(),
                    "source_record_sha256": sha256_file(density_path),
                })
            elif fragment_origin == "reviewed-current-recertification-override":
                verification_row.update({
                    "recertifier": "GPT-5.6 Sol",
                    "source": "LEGACY_SC_FRAGMENT_OVERRIDES",
                })
            verification[probe_id] = verification_row

    aggregation = json_load(root / "template" / "config" / "aggregation.json")
    owner_cfg = (
        aggregation.get("evaluations", {}).get("semantic_compression", {})
        .get("support_level_owner") or {}
    )
    factors = {
        str(name).upper(): float(value)
        for name, value in (owner_cfg.get("levels") or {}).items()
    }
    points_by_probe = {
        str(probe.get("probe_id")): float(probe.get("capability_denominator") or 0)
        for probe in matrix.get("probes", [])
    }
    total = sum(points_by_probe.values())
    awarded = sum(
        points_by_probe[probe_id] * factors.get(catalog[probe_id]["level"], 0.0)
        for probe_id in probe_ids
    )
    if total <= 0:
        return None, "legacy Semantic Compression capability denominator is empty"
    coverage = round(100.0 * awarded / total, 6)

    source_hash = sha256_file(source_path)
    density_hash = sha256_file(density_path)
    catalog_digest = _legacy_sc_catalog_digest(catalog)
    result = json.loads(json.dumps(record.get("result") or {}))
    result["schema_version"] = 1
    result["evaluation"] = "semantic_compression"
    result["requirements"] = {
        "metric.capability_coverage": {language: coverage}
    }
    evidence = dict(result.get("evidence") or {})
    evidence["canonical_fragments"] = catalog
    evidence["canonical_verification"] = verification
    source_snapshot = LEGACY_SC_SOURCE_SNAPSHOTS.get(source_run)
    density_snapshot = LEGACY_SC_SOURCE_SNAPSHOTS.get(density_run)
    if source_snapshot is None:
        return None, (
            f"{unit.get('id')}: legacy source run is not an approved "
            "Semantic Compression recertification snapshot"
        )
    if density_snapshot is None:
        return None, (
            f"{unit.get('id')}: legacy density run is not an approved "
            "Semantic Compression recertification snapshot"
        )
    scientific_identity_sha256 = sha256_bytes(
        json.dumps(
            experiment_identity,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=False,
        ).encode("utf-8")
    )
    current_rubric_path = (
        root / "template" / "methodology-assets" / "semantic_compression"
        / "capability_universe.json"
    )
    evidence["legacy_recertification"] = {
        "schema_version": 1,
        "mode": "paid-evidence-current-schema-v1",
        "recertifier": "GPT-5.6 Sol",
        "verdict": "PASS",
        "current_sc_epoch": cache_epoch(root, "semantic_compression"),
        "current_rubric_sha256": sha256_file(current_rubric_path),
        "source_snapshot_commit": source_snapshot,
        "source_run_id": source_run,
        "source_owner_record": source_rel.as_posix(),
        "source_owner_record_sha256": source_hash,
        "source_density_run_id": density_run,
        "source_density_snapshot_commit": density_snapshot,
        "source_density_record": density_rel.as_posix(),
        "source_density_record_sha256": density_hash,
        "density_source_selection": density_source_selection,
        "scientific_identity_sha256": scientific_identity_sha256,
        "canonical_catalog_sha256": catalog_digest,
        "migration_rule": "semantic-legacy-llm-recertification",
        "migration_rule_version": CACHE_MIGRATION_RULE_VERSION,
        "support_re_adjudications": {
            probe_id: dict(value)
            for probe_id, value in sorted(
                (LEGACY_SC_SUPPORT_OVERRIDES.get(language) or {}).items()
            )
        },
        "policy": (
            "Support judgments are recovered from the preserved paid Capability "
            "Coverage record and re-checked against the current rubric. Fragments "
            "come from the same scientific experiment's Semantic Density evidence (same-run "
            "preferred; cross-run only after exact shared-identity proof), with only "
            "the small reviewed fragment-override table filling historical omissions. "
            "Current trusted runtime facts own F20.P1 for Python/Go/Java/Kotlin. "
            "This is legacy LLM recertification and never claims that the historical "
            "run mechanically compiled or executed these reconstructed fixtures."
        ),
    }
    result["evidence"] = evidence

    audit_path = (
        root / "work" / "audit" / "semantic-compression"
        / f"canonical_verification_{slug_id(language)}.json"
    )
    json_dump(
        audit_path,
        {
            "schema_version": 1,
            "language": language,
            "legacy_evidence_recertified": True,
            "verification_mode": "legacy-evidence-recertification",
            "mechanical_verification_performed": False,
            "canonical_catalog_sha256": catalog_digest,
            "source_owner_record": source_rel.as_posix(),
            "source_owner_record_sha256": source_hash,
            "source_owner_run_id": source_run,
            "source_owner_snapshot_commit": source_snapshot,
            "source_density_record": density_rel.as_posix(),
            "source_density_record_sha256": density_hash,
            "source_density_run_id": density_run,
            "source_density_snapshot_commit": density_snapshot,
            "density_source_selection": density_source_selection,
            "scientific_identity_sha256": scientific_identity_sha256,
            "probes": verification,
        },
    )

    projected = json.loads(json.dumps(record))
    projected["result"] = result
    projected["result_sha256"] = sha256_bytes(
        json.dumps(
            result, sort_keys=True, separators=(",", ":")
        ).encode("utf-8")
    )
    projected["certification"] = {
        **(projected.get("certification") or {}),
        "semantic_legacy_owner_recertified": True,
        "canonical_catalog_sha256": catalog_digest,
        "current_validator_revalidation_required": True,
    }
    return projected, None



def materialize_legacy_semantic_owner_runner_attestation(
    root: Path,
    unit: dict[str, Any],
    result: dict[str, Any],
) -> Path | None:
    """Restore the redundant legacy SC runner attestation from certified evidence.

    A current-key cache record already carries the complete legacy recertification
    provenance and per-fragment hashes in its result.  The original migration also
    wrote an equivalent work/audit file, but that file is run-local and is not part
    of the certified cache.  Fresh hydration therefore recreates only that explicit
    legacy attestation before the ordinary current validator runs.  This never
    synthesizes build/run/nm evidence and always records mechanical verification as
    not performed.
    """
    if (
        str(unit.get("evaluation") or "") != "semantic_compression"
        or not unit.get("canonical_fragment_owner")
    ):
        return None
    assigned = [str(value) for value in (unit.get("assigned_languages") or [])]
    if len(assigned) != 1:
        return None
    evidence = result.get("evidence")
    if not isinstance(evidence, dict):
        return None
    metadata = evidence.get("legacy_recertification")
    verification = evidence.get("canonical_verification")
    catalog = evidence.get("canonical_fragments")
    if (
        not isinstance(metadata, dict)
        or metadata.get("mode") != "paid-evidence-current-schema-v1"
        or not isinstance(verification, dict)
        or not isinstance(catalog, dict)
    ):
        return None

    expected_catalog_sha256 = _legacy_sc_catalog_digest(catalog)
    if metadata.get("canonical_catalog_sha256") != expected_catalog_sha256:
        return None
    expected_probes = {
        str(probe_id)
        for probe_id, row in catalog.items()
        if isinstance(row, dict)
        and str(row.get("level") or "").upper() in {"FULL", "PARTIAL"}
    }
    if set(str(probe_id) for probe_id in verification) != expected_probes:
        return None
    for probe_id in sorted(expected_probes):
        row = verification.get(probe_id)
        canonical = catalog.get(probe_id)
        if not isinstance(row, dict) or not isinstance(canonical, dict):
            return None
        fragment = canonical.get("fragment")
        if (
            row.get("mode") != "legacy-evidence-recertification"
            or not isinstance(fragment, str)
            or row.get("canonical_fragment_sha256")
            != sha256_bytes(fragment.encode("utf-8"))
        ):
            return None

    language = assigned[0]
    audit_path = (
        root / "work" / "audit" / "semantic-compression"
        / f"canonical_verification_{slug_id(language)}.json"
    )
    json_dump(
        audit_path,
        {
            "schema_version": 1,
            "language": language,
            "legacy_evidence_recertified": True,
            "verification_mode": "legacy-evidence-recertification",
            "mechanical_verification_performed": False,
            "canonical_catalog_sha256": metadata.get("canonical_catalog_sha256"),
            "source_owner_record": metadata.get("source_owner_record"),
            "source_owner_record_sha256": metadata.get(
                "source_owner_record_sha256"
            ),
            "source_owner_run_id": metadata.get("source_run_id"),
            "source_owner_snapshot_commit": metadata.get(
                "source_snapshot_commit"
            ),
            "source_density_record": metadata.get("source_density_record"),
            "source_density_record_sha256": metadata.get(
                "source_density_record_sha256"
            ),
            "source_density_run_id": metadata.get("source_density_run_id"),
            "source_density_snapshot_commit": metadata.get(
                "source_density_snapshot_commit"
            ),
            "density_source_selection": metadata.get("density_source_selection"),
            "scientific_identity_sha256": metadata.get(
                "scientific_identity_sha256"
            ),
            "probes": verification,
        },
    )
    return audit_path


def _semantic_owner_legacy_metadata(
    root: Path, language: str
) -> dict[str, Any] | None:
    manifest = json_load(root / "work" / "root" / "manifest.json")
    for unit in manifest.get("work_units", []):
        if (
            unit.get("evaluation") != "semantic_compression"
            or not unit.get("canonical_fragment_owner")
            or unit.get("canonical_probe_id")
            or list(unit.get("assigned_languages") or []) != [language]
        ):
            continue
        path = (
            root / "work" / "agents" / str(unit.get("assigned_agent_id"))
            / "result.json"
        )
        if not path.is_file():
            continue
        evidence = (json_load(path).get("evidence") or {})
        metadata = evidence.get("legacy_recertification")
        if isinstance(metadata, dict):
            return dict(metadata)

    source_row = _current_semantic_language_owner_cache_source(root, language)
    if source_row is None:
        return None
    _path, source = source_row
    metadata = (((source.get("result") or {}).get("evidence") or {})
                .get("legacy_recertification"))
    return dict(metadata) if isinstance(metadata, dict) else None

def _current_semantic_language_owner_cache_source(
    root: Path, language: str
) -> tuple[Path, dict[str, Any]] | None:
    scope = (
        root / "cache" / "v1" / "semantic-compression" / slug_id(language)
    )
    if not scope.is_dir():
        return None
    expected_epoch = cache_epoch(root, "semantic_compression")
    candidates: list[tuple[int, Path, dict[str, Any]]] = []
    for path in sorted(scope.glob("*.json")):
        try:
            record = json_load(path)
        except (OSError, json.JSONDecodeError, TypeError, ValueError):
            continue
        if _cache_record_self_integrity_problem(record):
            continue
        if record.get("evaluation") != "semantic_compression":
            continue
        if list(record.get("assigned_languages") or []) != [language]:
            continue
        certification = record.get("certification") or {}
        if certification.get("validator_pass") is not True:
            continue
        payload = record.get("fingerprint_payload") or {}
        if payload.get("cache_epoch") != expected_epoch:
            continue
        try:
            catalog = canonical_fragment_catalog(root, record.get("result") or {})
        except (BenchmarkError, KeyError, TypeError, ValueError):
            continue
        if set(catalog) != set(semantic_probe_ids(root)):
            continue
        priority = int(bool(certification.get("current_validator_revalidated")))
        priority += int(bool(certification.get("semantic_legacy_owner_recertified")))
        candidates.append((priority, path, record))
    if not candidates:
        return None
    _priority, path, record = sorted(
        candidates, key=lambda row: (row[0], row[1].name), reverse=True
    )[0]
    return path, record


def project_semantic_probe_owner_from_current_cache(
    root: Path,
    unit: dict[str, Any],
    current_payload: dict[str, Any],
) -> tuple[Path | None, dict[str, Any] | None, str | None, str | None]:
    probe_id = canonical_fragment_probe(
        [str(value) for value in (unit.get("requirement_ids") or [])]
    )
    assigned = [str(value) for value in (unit.get("assigned_languages") or [])]
    if probe_id is None or len(assigned) != 1 or not unit.get("canonical_fragment_owner"):
        return None, None, None, None
    language = assigned[0]
    source_row = _current_semantic_language_owner_cache_source(root, language)
    if source_row is None:
        return None, None, None, None
    source_path, source = source_row
    source_catalog = canonical_fragment_catalog(root, source.get("result") or {})
    canonical = source_catalog[probe_id]
    source_evidence = ((source.get("result") or {}).get("evidence") or {})
    verification: dict[str, Any] = {}
    if canonical["level"] in {"FULL", "PARTIAL"}:
        row = (source_evidence.get("canonical_verification") or {}).get(probe_id)
        if not isinstance(row, dict):
            return (
                None, None,
                f"{unit.get('id')}: retained current owner lacks verification for {probe_id}",
                None,
            )
        verification[probe_id] = row

    rid = CANONICAL_FRAGMENT_PREFIX + slug_id(probe_id)
    evidence: dict[str, Any] = {
        "canonical_fragments": {probe_id: canonical},
        "canonical_verification": verification,
        "probe_recertification": {
            "schema_version": 1,
            "mode": "probe-from-current-certified-language-owner",
            "probe_id": probe_id,
            "language": language,
            "source_record": source_path.relative_to(root / "cache").as_posix(),
            "source_record_sha256": sha256_file(source_path),
            "source_result_sha256": source.get("result_sha256"),
        },
    }
    if canonical["level"] == "NONE":
        evidence["canonical_none_verification"] = {
            probe_id: {
                "none_reason": canonical["none_reason"],
                "conclusion": canonical["justification"],
                "citations": [str(canonical["citation"])],
                "evidence_kinds": [
                    "retained-current-validator-certified-owner",
                    "official-or-standard-evidence-cited-by-owner",
                ],
                "generation_failures_not_used_as_evidence": True,
            }
        }
    result = {
        "schema_version": 1,
        "evaluation": "semantic_compression",
        "requirements": {rid: True},
        "evidence": evidence,
    }
    fingerprint = sha256_bytes(
        json.dumps(
            current_payload,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=False,
        ).encode("utf-8")
    )
    projected: dict[str, Any] = {
        "schema_version": 1,
        "fingerprint": fingerprint,
        "fingerprint_payload": current_payload,
        "evaluation": "semantic_compression",
        "assigned_languages": assigned,
        "result": result,
        "result_sha256": sha256_bytes(
            json.dumps(
                result, sort_keys=True, separators=(",", ":")
            ).encode("utf-8")
        ),
        "certification": {
            "unit_complete": True,
            "validator_pass": False,
            "semantic_probe_projection_candidate": True,
        },
        "provenance": {
            **(source.get("provenance") or {}),
            "projection_source_fingerprint": source.get("fingerprint"),
            "work_unit_id": unit.get("id"),
            "derived_probe_id": probe_id,
        },
    }
    if isinstance(source.get("compatibility"), dict):
        projected["compatibility"] = source["compatibility"]
    return source_path, projected, None, "semantic-probe-from-current-owner"



def semantic_derived_recertification_record(
    root: Path,
    unit: dict[str, Any],
    current_payload: dict[str, Any],
) -> tuple[Path | None, dict[str, Any] | None, str | None, str | None]:
    if str(unit.get("evaluation") or "") != "semantic_compression":
        return None, None, None, None

    projected_probe = project_semantic_probe_owner_from_current_cache(
        root, unit, current_payload
    )
    if any(value is not None for value in projected_probe):
        return projected_probe

    requirement_ids = [str(value) for value in (unit.get("requirement_ids") or [])]
    probe_id = support_adjudication_probe(requirement_ids)
    is_comparability = COMPARABILITY_GATE in requirement_ids
    if probe_id is None and not is_comparability:
        return None, None, None, None

    languages = metadata_languages(root)
    source_rows: list[tuple[str, Path, dict[str, Any]]] = []
    owner_records: dict[str, dict[str, Any]] = {}
    for language in languages:
        source_row = _current_semantic_language_owner_cache_source(root, language)
        if source_row is None:
            return None, None, None, None
        path, source = source_row
        rel = path.relative_to(root / "cache").as_posix()
        source_rows.append((rel, path, source))
        if probe_id is not None:
            owner_records[language] = canonical_owner_record_for_probe(
                root, language, probe_id
            )

    source_record, source_path, source = sorted(
        source_rows, key=lambda row: row[0]
    )[0]
    source_fingerprint = str(source.get("fingerprint") or "")
    fingerprint = sha256_bytes(
        json.dumps(
            current_payload,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=False,
        ).encode("utf-8")
    )

    if probe_id is not None:
        rid = next(
            rid for rid in requirement_ids
            if rid.startswith(SUPPORT_ADJUDICATION_PREFIX)
        )
        result = {
            "schema_version": 1,
            "evaluation": "semantic_compression",
            "requirements": {
                rid: {
                    language: owner_records[language]
                    for language in languages
                }
            },
            "evidence": {
                "legacy_recertification": {
                    "schema_version": 1,
                    "mode": "support-from-current-canonical-probe-cache",
                    "probe_id": probe_id,
                    "source_owner_records": [
                        row[0] for row in sorted(source_rows, key=lambda item: item[0])
                    ],
                }
            },
        }
        mode = "semantic-support-from-current-catalog"
    else:
        result = {
            "schema_version": 1,
            "evaluation": "semantic_compression",
            "requirements": {COMPARABILITY_GATE: True},
            "evidence": {
                "legacy_recertification": {
                    "schema_version": 1,
                    "mode": "comparability-from-current-validated-probe-cache",
                    "language_count": len(languages),
                    "source_owner_records": [
                        row[0] for row in sorted(source_rows, key=lambda item: item[0])
                    ],
                    "reason": (
                        "Every probe×language canonical owner and downstream metric "
                        "dependency passed the current validator; the derived audit "
                        "introduces no second support authority."
                    ),
                }
            },
        }
        mode = "semantic-comparability-from-current-catalog"

    projected: dict[str, Any] = {
        "schema_version": 1,
        "fingerprint": fingerprint,
        "fingerprint_payload": current_payload,
        "evaluation": "semantic_compression",
        "assigned_languages": list(unit.get("assigned_languages") or []),
        "result": result,
        "result_sha256": sha256_bytes(
            json.dumps(
                result, sort_keys=True, separators=(",", ":")
            ).encode("utf-8")
        ),
        "certification": {
            "unit_complete": True,
            "validator_pass": False,
            "semantic_derived_recertification_candidate": True,
        },
        "provenance": {
            **(source.get("provenance") or {}),
            "projection_source_fingerprint": source_fingerprint,
            "work_unit_id": unit.get("id"),
            "derived_from_current_canonical_catalog": True,
        },
    }
    return source_path, projected, None, mode

def validate_canonical_fragment_consumer_result(
    root: Path, task: dict[str, Any], result: dict[str, Any]
) -> None:
    """Reject a metric result that drifted from its frozen canonical catalog."""
    digest = str(task.get("canonical_fragment_catalog_sha256") or "")
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise BenchmarkError("canonical fragment consumer task has no catalog digest")
    evidence = result.get("evidence") or {}
    if str(evidence.get("canonical_fragment_catalog_sha256") or "") != digest:
        raise BenchmarkError(
            "result must attest the exact canonical fragment catalog SHA-256"
        )
    catalog_path = None
    for raw in task.get("read_paths", []) or []:
        path = resolve_recorded_workspace_path(root, raw)
        if path.name.startswith("canonical_fragments_") and path.suffix == ".json":
            catalog_path = path
            break
    if catalog_path is None or not catalog_path.is_file():
        raise BenchmarkError("canonical fragment catalog task input is missing")
    payload = json_load(catalog_path)
    catalog = payload.get("canonical_fragments") or {}
    wanted = set(catalog)
    rows = probe_annotation_fields(result, wanted)
    for probe_id, record in catalog.items():
        canonical = sc_adjudicated_record(record)
        if canonical is None:
            raise BenchmarkError(f"invalid task catalog record: {probe_id}")
        values = _fragment_values(rows.get(probe_id) or {})
        if canonical["level"] == "NONE":
            if values:
                raise BenchmarkError(
                    f"{probe_id}: NONE probe may not measure a fragment: {sorted(values)}"
                )
            continue
        expected = str(canonical["fragment"]).strip()
        if values and values != {expected}:
            raise BenchmarkError(
                f"{probe_id}: metric evidence drifted from canonical fragment"
            )


def cmd_tasks_create(args: argparse.Namespace) -> int:
    root = workspace(args)
    assert_template_integrity(root)
    require_privacy_pass(root)
    manifest_path = root / "work" / "root" / "manifest.json"
    ledger_path = root / "work" / "root" / "ledger.json"
    if not manifest_path.is_file() or not ledger_path.is_file():
        raise BenchmarkError("manifest/ledger missing; run manifest-merge first")
    ledger = json_load(ledger_path)
    if ledger.get("manifest_sha256") != sha256_file(manifest_path):
        raise BenchmarkError("manifest changed after freeze")
    manifest = json_load(manifest_path)
    selected = [
        unit for unit in manifest.get("work_units", [])
        if args.evaluation is None or unit.get("evaluation") == args.evaluation
    ]
    if not selected:
        raise BenchmarkError("no manifest work units matched")
    created = []
    skipped = []
    for unit in selected:
        uid = unit["id"]
        state = ledger.get("units", {}).get(uid, {})
        if state.get("status", "PENDING") != "PENDING":
            skipped.append({"id": uid, "reason": f"ledger_status={state.get('status')}"})
            continue
        if unit.get("execution_kind", "agent") == "command":
            skipped.append({"id": uid, "reason": "runner_command_unit"})
            continue
        unmet = [
            dep for dep in unit.get("dependencies", [])
            if ledger.get("units", {}).get(dep, {}).get("status") != "COMPLETE"
        ]
        if unmet:
            skipped.append({
                "id": uid,
                "reason": "dependencies_not_complete",
                "dependencies": unmet,
            })
            continue
        agent_id = unit["assigned_agent_id"]
        agent_dir = root / "work" / "agents" / agent_id
        parent = args.parent or "RUNNER"
        requirement_ids = unit.get("requirement_ids", [])
        workload_ids = unit.get("workload_ids", [])
        assigned_languages = unit.get("assigned_languages", [])
        goal = unit["goal"]
        if workload_ids:
            goal += (
                "\n\nFrozen workload IDs served by this work unit:\n- "
                + "\n- ".join(workload_ids)
            )
        if requirement_ids:
            goal += (
                "\n\nFrozen Primary requirement IDs served by this work unit:\n- "
                + "\n- ".join(requirement_ids)
            )
        if COMPARABILITY_GATE in requirement_ids:
            # Appended before the retry check so a re-dispatched audit compares
            # equal to the packet it is retrying.
            goal += COMPARABILITY_AUDIT_INSTRUCTIONS
        reads = list(unit.get("read_paths", []))
        canonical_catalog_sha = None
        source_requirement = unit.get("canonical_fragment_source_requirement")
        if source_requirement:
            catalog_path, canonical_catalog_sha = canonical_fragment_input_for_unit(
                root, unit, manifest
            )
            reads.append(str(catalog_path))
            goal += (
                "\n\nCanonical fragment contract:\n"
                f"- Use exactly the supplied catalog ({canonical_catalog_sha}).\n"
                "- Do not author, substitute, or rewrite a probe fragment.\n"
                "- A NONE catalog entry has no measurable fragment; do not emit "
                "a numeric per-probe A/B/C/D value for it.\n"
                "- Write evidence.canonical_fragment_catalog_sha256 with exactly "
                "the catalog SHA-256 above."
            )
        if agent_dir.exists() and any(agent_dir.iterdir()):
            task_path = agent_dir / "task.json"
            if task_path.is_file():
                task_meta = json_load(task_path)
                expected_outputs = normalize_paths(unit.get("evidence_paths", []), root)
                frozen_mismatch = (
                    task_meta.get("evaluation") != unit.get("evaluation")
                    or task_meta.get("goal") != goal
                    or task_meta.get("requirement_ids") != unit.get("requirement_ids", [])
                    or task_meta.get("prompt_sections") != unit.get("prompt_sections", [])
                    or task_meta.get("assigned_languages", []) != assigned_languages
                    or task_meta.get("worker_mode") != unit.get("worker_mode", "packet-only")
                    or task_meta.get("expected_outputs") != expected_outputs
                    or task_meta.get("validation_command") != unit.get("validator_command")
                    or bool(task_meta.get("network_allowed")) != bool(unit.get("network_allowed", False))
                    or bool(task_meta.get("canonical_fragment_owner")) != bool(
                        unit.get("canonical_fragment_owner", False)
                    )
                    or task_meta.get("canonical_probe_id")
                    != unit.get("canonical_probe_id")
                    or task_meta.get("canonical_fragment_source_requirement")
                    != unit.get("canonical_fragment_source_requirement")
                    or task_meta.get("canonical_fragment_catalog_sha256")
                    != canonical_catalog_sha
                )
                if frozen_mismatch:
                    raise BenchmarkError(
                        f"{uid}: existing retry Task Packet does not match frozen manifest"
                    )
                created.append({
                    "work_unit_id": uid,
                    "agent_id": agent_id,
                    "task_path": str(task_path),
                    "reused_packet": True,
                })
                continue
            skipped.append({"id": uid, "reason": "agent_directory_initialized_without_task"})
            continue
        if COMPARABILITY_GATE in requirement_ids:
            reads.append(str(build_comparability_sample(root, unit, manifest)))
        adjudicated_probe = support_adjudication_probe(requirement_ids)
        if adjudicated_probe is not None:
            reads.append(str(build_support_adjudication_input(
                root, unit, manifest, adjudicated_probe
            )))
        ns = argparse.Namespace(
            workspace=str(root),
            id=agent_id,
            parent=parent,
            evaluation=unit["evaluation"],
            goal=goal,
            read=reads,
            write=str(agent_dir),
            output=unit["evidence_paths"],
            validate=unit["validator_command"],
            network=bool(unit.get("network_allowed", False)),
            depth=int(args.depth),
            section=unit.get("prompt_sections", []),
            requirement_id=unit.get("requirement_ids", []),
            language=assigned_languages,
            worker_mode=unit.get("worker_mode", "packet-only"),
            layout=unit.get("packet_layout", "task-first"),
            canonical_fragment_owner=bool(
                unit.get("canonical_fragment_owner", False)
            ),
            canonical_probe_id=unit.get("canonical_probe_id"),
            canonical_fragment_source_requirement=source_requirement,
            canonical_fragment_catalog_sha256=canonical_catalog_sha,
        )
        cmd_task_create(ns)
        created.append({
            "work_unit_id": uid,
            "agent_id": agent_id,
            "task_path": str(agent_dir / "task.json"),
        })
    result = {"ok": True, "created": created, "skipped": skipped}
    print(json.dumps(result, indent=2))
    return 0

def _validate_acyclic(units: dict[str, dict[str, Any]]) -> None:
    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(uid: str) -> None:
        if uid in visited:
            return
        if uid in visiting:
            raise BenchmarkError(f"manifest dependency cycle includes {uid}")
        visiting.add(uid)
        for dep in units[uid].get("dependencies", []):
            visit(dep)
        visiting.remove(uid)
        visited.add(uid)

    for uid in units:
        visit(uid)


def cmd_manifest_merge(args: argparse.Namespace) -> int:
    root = workspace(args)
    assert_template_integrity(root)
    manifest_path = root / "work" / "root" / "manifest.json"
    ledger_path = root / "work" / "root" / "ledger.json"
    if manifest_path.exists() or ledger_path.exists():
        raise BenchmarkError("manifest/ledger already frozen; start a new run to change the Primary plan")
    units_by_id: dict[str, dict[str, Any]] = {}
    plans: list[dict[str, Any]] = []
    for evaluation in PRIMARY_NAMES:
        plan_path = root / "work" / "root" / "plans" / f"{evaluation}.json"
        agent_id = "deterministic-runner"
        if not plan_path.is_file():
            raise BenchmarkError(
                f"missing deterministic plan: {plan_path}; run deterministic-plan first"
            )
        plan = json_load(plan_path)
        checked_plan = validate_work_plan_data(root, evaluation, plan)
        raw_units = checked_plan["work_units"]
        plans.append({"evaluation": evaluation, "agent_id": agent_id, "path": str(plan_path), "sha256": sha256_file(plan_path)})
        for raw in raw_units:
            if not isinstance(raw, dict):
                raise BenchmarkError(f"{plan_path}: work units must be objects")
            uid = str(raw.get("id", "")).strip()
            if not uid or uid in units_by_id:
                raise BenchmarkError(f"missing or duplicate work unit id: {uid!r}")
            if raw.get("evaluation", evaluation) != evaluation:
                raise BenchmarkError(f"{uid}: evaluation mismatch")
            assigned = str(raw.get("assigned_agent_id", "")).strip()
            goal = str(raw.get("goal", "")).strip()
            validator = str(raw.get("validator_command", "")).strip()
            phase = str(raw.get("phase", "")).strip()
            if not assigned or not goal or not validator:
                raise BenchmarkError(f"{uid}: goal, assigned_agent_id and validator_command are required")
            deps = raw.get("dependencies", [])
            evidence = raw.get("evidence_paths", [])
            hashes = raw.get("input_hashes", {})
            reads = raw.get("read_paths", [])
            reuse_audit_for = raw.get("reuse_audit_for", [])
            requirement_ids = raw.get("requirement_ids", [])
            if phase not in {"readiness", "measurement", "aggregation"}:
                raise BenchmarkError(
                    f"{uid}: phase must be readiness, measurement or aggregation"
                )
            if not isinstance(deps, list) or not all(isinstance(x, str) for x in deps):
                raise BenchmarkError(f"{uid}: dependencies must be string IDs")
            if not isinstance(evidence, list) or not evidence or not all(isinstance(x, str) for x in evidence):
                raise BenchmarkError(f"{uid}: evidence_paths must be a non-empty list of strings")
            if not isinstance(hashes, dict):
                raise BenchmarkError(f"{uid}: input_hashes must be an object")
            if not isinstance(reuse_audit_for, list) or not all(isinstance(x, str) for x in reuse_audit_for):
                raise BenchmarkError(f"{uid}: reuse_audit_for must be a string array")
            execution_kind = str(raw.get("execution_kind", "agent"))
            prompt_sections = raw.get("prompt_sections", [])
            max_attempts = int(raw.get("max_attempts", runner_max_attempts(root)) or 3)
            result_kind = str(raw.get("result_kind", "requirements"))
            max_calls = int(raw.get("max_llm_calls", 0) or 0)
            input_tokens = int(raw.get("estimated_input_tokens_per_call", 0) or 0)
            output_tokens = int(raw.get("max_output_tokens_per_call", 0) or 0)
            if max_calls < 0:
                raise BenchmarkError(f"{uid}: max_llm_calls cannot be negative")
            if input_tokens < 0 or output_tokens < 0:
                raise BenchmarkError(f"{uid}: token estimates cannot be negative")
            if max_calls > 0 and (input_tokens <= 0 or output_tokens <= 0):
                raise BenchmarkError(
                    f"{uid}: LLM work requires estimated_input_tokens_per_call "
                    "and max_output_tokens_per_call"
                )
            units_by_id[uid] = {
                "id": uid,
                "evaluation": evaluation,
                "goal": goal,
                "phase": phase,
                "assigned_agent_id": assigned,
                "dependencies": deps,
                "input_hashes": hashes,
                "reuse_audit_for": reuse_audit_for,
                "requirement_ids": requirement_ids,
                "workload_ids": list(raw.get("workload_ids", [])),
                "read_paths": normalize_paths(reads, root),
                "evidence_paths": normalize_paths(evidence, root),
                "max_llm_calls": max_calls,
                "estimated_input_tokens_per_call": input_tokens,
                "max_output_tokens_per_call": output_tokens,
                "network_allowed": bool(raw.get("network_allowed", False)),
                "validator_command": validator,
                "execution_kind": execution_kind,
                "worker_mode": str(raw.get("worker_mode") or (
                    "packet-only" if execution_kind == "agent" else "runner-command"
                )),
                "prompt_sections": prompt_sections,
                "max_attempts": max_attempts,
                "result_kind": result_kind,
                "packet_layout": str(raw.get("packet_layout") or "task-first"),
                "assigned_languages": list(raw.get("assigned_languages", [])),
                "runner_action": raw.get("runner_action"),
                # These are part of the frozen Semantic Compression contract.
                # Dropping them at manifest-merge time lets downstream A/B/C/D/E
                # units dispatch without the canonical fragment selected by the
                # Capability Coverage owner.
                "canonical_fragment_owner": bool(
                    raw.get("canonical_fragment_owner", False)
                ),
                "canonical_probe_id": raw.get("canonical_probe_id"),
                "canonical_fragment_source_requirement": raw.get(
                    "canonical_fragment_source_requirement"
                ),
            }
    reuse_status_path = root / "results" / "reuse_status.json"
    if (root / "template" / "reuse" / "materialized.json").is_file():
        if not reuse_status_path.is_file():
            raise BenchmarkError("reuse_status.json is required before manifest freeze")
        reuse_status = json_load(reuse_status_path)
        required_audits = set(reuse_status.get("audit_required", []))
        audit_units_by_artifact: dict[str, set[str]] = {aid: set() for aid in required_audits}
        for uid, unit in units_by_id.items():
            for artifact_id in unit.get("reuse_audit_for", []):
                if artifact_id not in required_audits:
                    raise BenchmarkError(
                        f"{uid}: reuse_audit_for names an artifact that is not AUDIT_REQUIRED: {artifact_id}"
                    )
                audit_units_by_artifact.setdefault(artifact_id, set()).add(uid)
        missing_audits = sorted(
            aid for aid in required_audits if not audit_units_by_artifact.get(aid)
        )
        if missing_audits:
            raise BenchmarkError(
                "Primary plan omitted required reusable-artifact audits: "
                + ", ".join(missing_audits)
            )

        materialized = json_load(root / "template" / "reuse" / "materialized.json")
        artifact_dest = {
            str(a["id"]): require_under(
                root / "template" / str(a.get("content_destination", a["destination"])),
                root,
            )
            for a in materialized.get("artifacts", [])
            if str(a.get("id")) in required_audits
        }
        for uid, unit in units_by_id.items():
            unit_reads = [lexical_absolute(Path(p)) for p in unit.get("read_paths", [])]
            unit_deps = set(unit.get("dependencies", []))
            own_audits = set(unit.get("reuse_audit_for", []))
            for artifact_id, dest in artifact_dest.items():
                if artifact_id in own_audits:
                    continue
                sees_artifact = False
                for rp in unit_reads:
                    try:
                        dest.relative_to(rp)
                        sees_artifact = True
                    except ValueError:
                        try:
                            rp.relative_to(dest)
                            sees_artifact = True
                        except ValueError:
                            pass
                    if sees_artifact:
                        break
                if sees_artifact:
                    audits = audit_units_by_artifact.get(artifact_id, set())
                    if not (unit_deps & audits):
                        raise BenchmarkError(
                            f"{uid}: reads AUDIT_REQUIRED artifact {artifact_id} "
                            "without depending on its capability-currency audit unit"
                        )

    known = set(units_by_id)
    for uid, unit in units_by_id.items():
        for dep in unit["dependencies"]:
            if dep not in known or dep == uid:
                raise BenchmarkError(f"{uid}: invalid dependency {dep!r}")
    _validate_acyclic(units_by_id)

    def transitive_dependencies(uid: str) -> set[str]:
        result: set[str] = set()
        stack = list(units_by_id[uid].get("dependencies", []))
        while stack:
            dep = stack.pop()
            if dep in result:
                continue
            result.add(dep)
            stack.extend(units_by_id[dep].get("dependencies", []))
        return result

    for evaluation in PRIMARY_NAMES:
        eval_units = {
            uid: unit for uid, unit in units_by_id.items()
            if unit["evaluation"] == evaluation
        }
        measurements = [
            uid for uid, unit in eval_units.items()
            if unit.get("phase") == "measurement"
        ]
        aggregations = [
            uid for uid, unit in eval_units.items()
            if unit.get("phase") == "aggregation"
        ]
        if not measurements:
            raise BenchmarkError(f"{evaluation}: Primary plan requires at least one measurement unit")
        if len(aggregations) != 1:
            raise BenchmarkError(
                f"{evaluation}: Primary plan requires exactly one aggregation unit, got {len(aggregations)}"
            )
        aggregation = aggregations[0]
        required = set(eval_units) - {aggregation}
        covered = transitive_dependencies(aggregation)
        missing = sorted(required - covered)
        if missing:
            raise BenchmarkError(
                f"{evaluation}: aggregation unit {aggregation} does not transitively depend on: "
                + ", ".join(missing)
            )
        cross_eval = sorted(dep for dep in covered if units_by_id[dep]["evaluation"] != evaluation)
        if cross_eval:
            raise BenchmarkError(
                f"{evaluation}: aggregation may not depend on another Primary evaluation: "
                + ", ".join(cross_eval)
            )

    manifest = {"schema_version": 1, "frozen_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(), "plans": plans, "work_units": list(units_by_id.values())}
    json_dump(manifest_path, manifest)
    manifest_hash = sha256_file(manifest_path)
    now = dt.datetime.now(dt.timezone.utc).isoformat()
    ledger = {
        "schema_version": 1,
        "manifest_sha256": manifest_hash,
        "units": {
            uid: {
                "status": "PENDING",
                "evidence_paths": unit["evidence_paths"],
                "validation_result": None,
                "blocker": None,
                "blocker_class": None,
                "attempts": 0,
                "max_attempts": int(unit.get("max_attempts", 3)),
                "heartbeat_at_utc": None,
                "attempt_history": [],
                "updated_at_utc": now,
            }
            for uid, unit in units_by_id.items()
        },
    }
    json_dump(ledger_path, ledger)
    result = {"ok": True, "manifest_sha256": manifest_hash, "plan_count": len(plans), "work_unit_count": len(units_by_id), "by_evaluation": {name: sum(1 for u in units_by_id.values() if u["evaluation"] == name) for name in PRIMARY_NAMES}}
    json_dump(root / "results" / "manifest_merge.json", result)
    print(json.dumps(result, indent=2))
    return 0


@contextlib.contextmanager
def ledger_lock(root: Path):
    """Serialize ledger read-modify-write across the runner's worker processes.

    The ledger is rewritten whole. With several work units in flight, two
    `task-finish` processes that read the same ledger and each write back their
    own unit would keep only the later one. An exclusive lock on a sibling file
    makes every update see the previous one.
    """
    lock_path = root / "work" / "root" / "ledger.lock"
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with open(lock_path, "a+b") as handle:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def cmd_ledger_update(args: argparse.Namespace) -> int:
    root = workspace(args)
    with ledger_lock(root):
        return _cmd_ledger_update_locked(args)


def _cmd_ledger_update_locked(args: argparse.Namespace) -> int:
    root = workspace(args)
    manifest_path = root / "work" / "root" / "manifest.json"
    ledger_path = root / "work" / "root" / "ledger.json"
    if not manifest_path.is_file() or not ledger_path.is_file():
        raise BenchmarkError("manifest/ledger missing")
    ledger = json_load(ledger_path)
    if ledger.get("manifest_sha256") != sha256_file(manifest_path):
        raise BenchmarkError("manifest changed after freeze")
    units = ledger.get("units", {})
    if args.id not in units:
        raise BenchmarkError(f"unknown work unit: {args.id}")

    state = dict(units[args.id])
    current = str(state.get("status", "PENDING"))
    target = args.status
    allowed = {
        "PENDING": {"PENDING", "RUNNING", "BLOCKED", "INVALID"},
        "RUNNING": {"PENDING", "RUNNING", "COMPLETE", "BLOCKED", "INVALID"},
        "COMPLETE": {"COMPLETE", "INVALID"},
        "BLOCKED": {"BLOCKED", "PENDING", "RUNNING", "INVALID"},
        "INVALID": {"INVALID", "PENDING", "RUNNING"},
    }
    if target not in allowed.get(current, set()):
        raise BenchmarkError(f"invalid ledger transition: {current} -> {target}")

    now = dt.datetime.now(dt.timezone.utc).isoformat()
    evidence = normalize_paths(
        args.evidence or state.get("evidence_paths", []),
        root,
    )
    attempts = int(state.get("attempts", 0) or 0)
    max_attempts = int(state.get("max_attempts", 3) or 3)
    history = list(state.get("attempt_history", []))

    if target == "RUNNING":
        if current != "RUNNING":
            attempts += 1
            if attempts > max_attempts:
                raise BenchmarkError(
                    f"{args.id}: attempt limit exhausted ({attempts - 1}/{max_attempts})"
                )
            history.append({"attempt": attempts, "started_at_utc": now})
        state["heartbeat_at_utc"] = now

    if current == "RUNNING" and target == "PENDING" and history:
        history[-1]["finished_at_utc"] = now
        history[-1]["result"] = "RETRY"

    if target == "COMPLETE":
        missing = [p for p in evidence if not Path(p).exists()]
        if missing:
            raise BenchmarkError(f"missing evidence: {missing}")
        if args.validation_result != "PASS":
            raise BenchmarkError("COMPLETE requires PASS validation")
        if history:
            history[-1]["finished_at_utc"] = now
            history[-1]["result"] = "PASS"

    if target in {"BLOCKED", "INVALID"} and not args.blocker:
        raise BenchmarkError(f"{target} requires --blocker")
    if target in {"BLOCKED", "INVALID"} and history:
        history[-1]["finished_at_utc"] = now
        history[-1]["result"] = target

    state.update({
        "status": target,
        "evidence_paths": evidence,
        "validation_result": args.validation_result,
        "blocker": args.blocker if target in {"BLOCKED", "INVALID"} else None,
        "blocker_class": (
            getattr(args, "blocker_class", None)
            if target in {"BLOCKED", "INVALID"}
            else None
        ),
        "attempts": attempts,
        "max_attempts": max_attempts,
        "attempt_history": history,
        "updated_at_utc": now,
    })
    if target != "RUNNING":
        state["heartbeat_at_utc"] = None
    units[args.id] = state
    json_dump(ledger_path, ledger)
    print(json.dumps({"id": args.id, **state}, indent=2))
    return 0


def normalize_paths(values: Iterable[str], root: Path) -> list[str]:
    out = []
    for value in values:
        p = Path(value)
        if not p.is_absolute():
            p = root / p
        out.append(str(require_under(p, root)))
    return out


def reject_overbroad_read_paths(paths: Iterable[str], root: Path) -> None:
    forbidden = {
        lexical_absolute(root),
        lexical_absolute(root / "repo"),
        lexical_absolute(root / "template"),
        lexical_absolute(root / "work"),
        lexical_absolute(root / "work" / "agents"),
    }
    for value in paths:
        if lexical_absolute(Path(value)) in forbidden:
            raise BenchmarkError(
                f"read path is broader than allowed for a delegated task: {value}"
            )


def render_workspace_paths(content: str, root: Path) -> str:
    """Keep prompt-visible workspace paths canonical and machine-independent."""
    canonical = CANONICAL_WORKSPACE.as_posix()
    aliases = {
        "./.quidra-benchmark",
        str(lexical_absolute(root)),
        lexical_absolute(root).as_posix(),
    }
    for alias in sorted(aliases, key=len, reverse=True):
        if alias and alias != canonical:
            content = content.replace(alias, canonical)
    return content


def store_prompt_component(root: Path, content: str, kind: str) -> dict[str, Any]:
    data = content.encode("utf-8")
    digest = sha256_bytes(data)
    canonical = (
        root / "template" / "prompts" / "components" / "by-hash" / f"{digest}.md"
    )
    if canonical.is_file():
        if canonical.read_bytes() != data:
            raise BenchmarkError(f"canonical prompt component hash collision: {digest}")
        dest = canonical
    else:
        dest = root / "prompts" / "components" / "by-hash" / f"{digest}.md"
        dest.parent.mkdir(parents=True, exist_ok=True)
        if dest.exists():
            if dest.read_bytes() != data:
                raise BenchmarkError(f"prompt component hash collision: {digest}")
        else:
            dest.write_bytes(data)
    return {
        "kind": kind,
        "sha256": digest,
        "path": str(dest),
        "bytes": len(data),
        "canonical": dest == canonical,
    }


def render_prompt_components(components: list[dict[str, Any]], expected_hash: str) -> bytes:
    chunks: list[bytes] = []
    for component in components:
        path = Path(component["path"])
        if not path.is_file():
            raise BenchmarkError(f"prompt component is missing: {path}")
        data = path.read_bytes()
        if sha256_bytes(data) != component["sha256"]:
            raise BenchmarkError(f"prompt component hash mismatch: {path}")
        chunks.append(data)
    rendered = b"".join(chunks)
    if sha256_bytes(rendered) != expected_hash:
        raise BenchmarkError("rendered Task Packet hash mismatch")
    return rendered


def extract_markdown_sections(content: str, selectors: list[str]) -> str:
    if not selectors:
        return content
    lines = content.splitlines()
    chunks: list[str] = []
    for selector in selectors:
        wanted = selector.strip()
        start = next((i for i, line in enumerate(lines) if line.strip() == wanted), None)
        if start is None:
            raise BenchmarkError(f"methodology section not found: {selector}")
        heading = re.match(r"^(#+)\s+", lines[start])
        if not heading:
            raise BenchmarkError(f"section selector is not a markdown heading: {selector}")
        level = len(heading.group(1))
        end = len(lines)
        for i in range(start + 1, len(lines)):
            m = re.match(r"^(#+)\s+", lines[i])
            if m and len(m.group(1)) <= level:
                end = i
                break
        chunks.append("\n".join(lines[start:end]).strip())
    return "\n\n".join(chunks) + "\n"



def sampling_config(root: Path, evaluation: str | None = None) -> dict[str, Any]:
    """Frozen declaration of how scored requests are decoded.

    With an evaluation, the depth that evaluation's frozen override in the
    gateway config pins for its scored requests replaces the run-wide one;
    without one, the run-wide declaration is returned unchanged, so records of
    evaluations without an override keep the key they always had.

    The evaluated model family removed temperature, top_p and top_k and rejects a
    request carrying one, so there is no fixed sampling value to set. What the
    benchmark can still freeze, and what section 6.2 of the LLM Proficiency
    specification actually asks for, is that the decoding state is identical for
    every language and is recorded. This returns that declaration; the scored
    paths send no decoding parameters at all.
    """
    cfg = json_load(root / "template" / "config" / "primary.json")
    sampling = cfg.get("sampling")
    if not isinstance(sampling, dict):
        raise BenchmarkError("primary config is missing the frozen sampling section")
    if sampling.get("sampling_parameters") != "omitted":
        raise BenchmarkError(
            "frozen sampling must declare sampling_parameters=omitted: the evaluated "
            "model family rejects temperature/top_p/top_k with HTTP 400"
        )
    if sampling.get("decoding_state") != "provider-controlled":
        raise BenchmarkError("frozen sampling must record a provider-controlled state")
    levels = {"low", "medium", "high", "xhigh", "max"}
    effort = sampling.get("effort")
    if effort not in levels:
        raise BenchmarkError(f"frozen sampling effort is not a known level: {effort}")
    # Sandbox-agent action turns are decoded at their own frozen depth. They are
    # never scored, and adaptive thinking is billed inside the output cap, so a
    # deep action turn can spend the whole cap reasoning and return nothing.
    orchestration = sampling.get("orchestration_effort", effort)
    if orchestration not in levels:
        raise BenchmarkError(
            f"frozen orchestration effort is not a known level: {orchestration}"
        )
    declared = {
        "sampling_parameters": "omitted",
        "decoding_state": "provider-controlled",
        "effort": str(effort),
        "orchestration_effort": str(orchestration),
    }
    if evaluation:
        override = (
            (gateway_config(root).get("anthropic_decoding") or {}).get("evaluation_effort") or {}
        ).get(evaluation)
        if override:
            if override not in levels:
                raise BenchmarkError(f"frozen effort override for {evaluation} is not a known level: {override}")
            declared["effort"] = str(override)
            declared["effort_source"] = "evaluation_effort"
    return declared



def cache_policy(root: Path) -> dict[str, Any]:
    path = root / "template" / "config" / "cache_policy.json"
    data = json_load(path)
    if data.get("schema_version") != 1 or data.get("cache_schema_version") != 1:
        raise BenchmarkError("unsupported certified cache policy schema")
    identity = data.get("quidra_execution_identity") or {}
    raw_paths = identity.get("input_paths")
    if not isinstance(raw_paths, list) or not raw_paths:
        # Retained paid workspaces from before execution-identity tracking have
        # neither the policy field nor a trusted identity in run.json. They may
        # still be opened to recover comparison-language evidence. A current
        # workspace (or any workspace claiming an identity) must never take
        # this legacy escape hatch.
        run_path = root / "run.json"
        run_identity = None
        if run_path.is_file():
            run_identity = (
                (json_load(run_path).get("evaluated") or {})
                .get("quidra_execution_identity")
            )
        if run_identity is None:
            return data
        raise BenchmarkError(
            "cache policy quidra_execution_identity.input_paths must be non-empty"
        )
    paths = quidra_execution_input_paths(data)
    baseline = identity.get("legacy_baseline") or {}
    baseline_objects = baseline.get("git_objects")
    if baseline_objects is not None and (
        not isinstance(baseline_objects, dict)
        or set(baseline_objects) != set(paths)
    ):
        raise BenchmarkError(
            "cache policy legacy Quidra execution baseline does not match input_paths"
        )
    return data


#: Runner actions whose results are mechanical measurements of the frozen
#: programs: no model is involved, only the pinned image, the snapshot's
#: compiler and the measurement scripts. Their results are certified like any
#: measurement, because the third rehearsal spent five hours and fifty minutes
#: of its six-hour job measuring them again for nothing that had changed, and
#: was cancelled before its one paid unit could finish.
MECHANICAL_ACTIONS = ("micro-measure", "adversarial-measure", "quidra-audit")

MEASUREMENT_SCRIPTS = ("micro_measure.py", "adversarial_measure.py")


def mechanical_unit(unit: dict[str, Any]) -> bool:
    return (
        unit.get("execution_kind") == "command"
        and str(unit.get("runner_action") or "") in MECHANICAL_ACTIONS
        and unit.get("result_kind", "requirements") == "requirements"
    )


def mechanical_task(unit: dict[str, Any]) -> dict[str, Any]:
    """The task-shaped view of a mechanical unit: its read paths, no packet."""
    return {"read_paths": list(unit.get("read_paths", []) or []), "prompt_sha256": None}


def mechanical_result_path(root: Path, unit: dict[str, Any]) -> Path:
    return root / "work" / "root" / "commands" / str(unit["id"]) / "result.json"


def measurement_script_hashes(root: Path) -> dict[str, str]:
    scripts = root / "template" / "scripts"
    return {name: sha256_file(scripts / name) for name in MEASUREMENT_SCRIPTS}


def cache_eligible_unit(root: Path, unit: dict[str, Any]) -> bool:
    if mechanical_unit(unit):
        return True
    if unit.get("execution_kind", "agent") != "agent":
        return False
    assigned = list(unit.get("assigned_languages", []) or [])
    if unit.get("result_kind", "requirements") == "requirements":
        if unit.get("phase") != "measurement":
            return False
        requirement_ids = [str(rid) for rid in (unit.get("requirement_ids") or [])]
        # Cohort support adjudication and the final blinded comparability gate
        # have no assigned language because they intentionally see the whole
        # fixed comparison set. Their Task Packets embed the exact completed
        # annotations/sample, so a certified COMPLETE+PASS result is just as
        # content-addressable as a language-scoped measurement.
        if support_adjudication_probe(requirement_ids) is not None:
            return True
        if COMPARABILITY_GATE in requirement_ids:
            return True
        return bool(assigned)
    # A reuse audit judges one reusable artifact against the current toolchain.
    # Its inputs are the artifact's own git object and the toolchain of the
    # artifact's language, both of which the key carries, so its verdict is as
    # reusable as any measurement. Left uncached, the four Python and C++
    # audits were bought again by every run - about three to six dollars each
    # time for a verdict nothing had changed.
    return (
        unit.get("result_kind") == "audit"
        and unit.get("phase") == "readiness"
        and bool(unit.get("reuse_audit_for"))
    )


def audit_languages(root: Path, unit: dict[str, Any]) -> list[str]:
    """The languages of the reusable artifacts a readiness audit judges."""
    materialized_path = root / "template" / "reuse" / "materialized.json"
    if not materialized_path.is_file():
        return []
    by_id = {
        str(a.get("id")): str(a.get("language") or "")
        for a in (json_load(materialized_path).get("artifacts") or [])
    }
    languages = sorted({by_id[a] for a in unit.get("reuse_audit_for", []) if by_id.get(a)})
    return languages


def quidra_execution_input_paths(policy: dict[str, Any]) -> tuple[str, ...]:
    """The cache policy is the single authority for Quidra execution inputs."""
    identity = policy.get("quidra_execution_identity") or {}
    raw = identity.get("input_paths")
    if not isinstance(raw, list) or not raw:
        raise BenchmarkError(
            "cache policy quidra_execution_identity.input_paths must be non-empty"
        )
    paths: list[str] = []
    for value in raw:
        if not isinstance(value, str) or not value:
            raise BenchmarkError(
                "cache policy Quidra execution input paths must be non-empty strings"
            )
        path = PurePosixPath(value)
        if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
            raise BenchmarkError(
                f"invalid cache policy Quidra execution input path: {value!r}"
            )
        paths.append(path.as_posix())
    if len(paths) != len(set(paths)):
        raise BenchmarkError("cache policy Quidra execution input paths contain duplicates")
    return tuple(paths)


def quidra_execution_identity_from_git(
    source: Path,
    revision: str = "HEAD",
    input_paths: tuple[str, ...] | list[str] | None = None,
) -> dict[str, Any]:
    """Content identity of the compiler/runtime implementation a run executes.

    The path set comes from cache_policy.json so benchmark code and policy
    cannot silently drift. Benchmark-only edits must not invalidate Quidra
    measurements, while a compiler/runtime change under the same declared
    version must.
    """
    if input_paths is None:
        policy_path = source / "benchmark" / "template" / "config" / "cache_policy.json"
        if not policy_path.is_file():
            raise BenchmarkError(
                "cannot locate cache_policy.json for Quidra execution identity"
            )
        input_paths = quidra_execution_input_paths(json_load(policy_path))
    else:
        input_paths = tuple(str(value) for value in input_paths)
        if not input_paths:
            raise BenchmarkError("Quidra execution identity requires at least one input path")

    objects: dict[str, str] = {}
    for relative in input_paths:
        try:
            observed = run_capture(
                ["git", "rev-parse", f"{revision}:{relative}"], source
            )
        except subprocess.CalledProcessError as exc:
            raise BenchmarkError(
                f"cannot identify Quidra execution input at {revision}:{relative}"
            ) from exc
        if not re.fullmatch(r"[0-9a-f]{40}", observed):
            raise BenchmarkError(
                f"invalid git object for Quidra execution input {relative}: {observed!r}"
            )
        objects[relative] = observed
    digest = sha256_bytes(
        json.dumps(
            objects, sort_keys=True, separators=(",", ":"), ensure_ascii=False
        ).encode("utf-8")
    )
    return {"schema_version": 1, "sha256": digest, "git_objects": objects}


def current_quidra_execution_identity(root: Path) -> dict[str, Any] | None:
    """Trusted target identity frozen by init before the sandbox is staged."""
    run_path = root / "run.json"
    if not run_path.is_file():
        return None
    raw = (json_load(run_path).get("evaluated") or {}).get(
        "quidra_execution_identity"
    )
    if raw is None:
        return None
    expected_paths = set(quidra_execution_input_paths(cache_policy(root)))
    if (
        not isinstance(raw, dict)
        or raw.get("schema_version") != 1
        or not re.fullmatch(r"[0-9a-f]{64}", str(raw.get("sha256") or ""))
        or not isinstance(raw.get("git_objects"), dict)
        or set(raw["git_objects"]) != expected_paths
    ):
        raise BenchmarkError("run.json carries an invalid Quidra execution identity")
    return raw


def _legacy_cache_run_commit_prefix(record: dict[str, Any]) -> str | None:
    run_id = str((record.get("provenance") or {}).get("run_id") or "")
    match = re.search(r"-([0-9a-f]{7,40})-gh\d+$", run_id)
    return match.group(1) if match else None


def cache_quidra_execution_reuse_problem(
    root: Path, record: dict[str, Any]
) -> str | None:
    """Reject same-version target cache when the compiler/runtime actually changed."""
    payload = record.get("fingerprint_payload") or {}
    target = str(cache_policy(root).get("target_language") or "Quidra")
    if target not in (payload.get("assigned_languages") or []):
        return None
    current = current_quidra_execution_identity(root)
    if current is None:
        if lexical_absolute(root) == lexical_absolute(CANONICAL_WORKSPACE):
            return "current run is missing the trusted Quidra execution identity"
        return None
    compatibility = record.get("compatibility") or {}
    recorded = compatibility.get("quidra_execution_identity")
    if recorded is not None:
        if not isinstance(recorded, dict):
            return "cached Quidra execution identity is malformed"
        if recorded.get("sha256") != current.get("sha256"):
            return "Quidra compiler/runtime implementation changed"
        if recorded.get("git_objects") != current.get("git_objects"):
            return "Quidra execution-input object map changed"
        return None
    policy = cache_policy(root).get("quidra_execution_identity") or {}
    baseline = policy.get("legacy_baseline") or {}
    verified = {
        str(value) for value in (policy.get("legacy_verified_commit_prefixes") or [])
    }
    prefix = _legacy_cache_run_commit_prefix(record)
    if prefix not in verified:
        return (
            "legacy Quidra cache has no execution identity and its source commit "
            "was not verified for migration"
        )
    if baseline.get("git_objects") != current.get("git_objects"):
        return (
            "legacy Quidra cache predates execution identities and the current "
            "compiler/runtime no longer matches the frozen migration baseline"
        )
    return None


def quidra_target_identity(root: Path) -> dict[str, str]:
    """The evaluated Quidra's declared versions, from the snapshot's project.toml.

    A unit that measures Quidra used to be a cache MISS on every run because
    Quidra is the changing target. Three paid runs showed the other side of
    that rule: when a comparison language failed, Quidra's own completed
    measurements were paid for again although nothing about Quidra had
    changed. The record is now keyed by the versions the snapshot declares
    (`version` and `language_version` in project.toml) and by the exact Task
    Packet, which embeds the snapshot's docs. Bumping either version, or
    changing the docs a packet embeds, is what makes Quidra "new" to the cache.
    """
    path = root / "repo" / "project.toml"
    if not path.is_file():
        raise BenchmarkError(f"evaluated snapshot has no project.toml: {path}")
    text = path.read_text(encoding="utf-8")
    identity: dict[str, str] = {}
    for key in ("version", "language_version"):
        match = re.search(rf'^{key} = "([^"]*)"$', text, re.M)
        if not match:
            raise BenchmarkError(f"project.toml does not declare {key} exactly once")
        identity[key] = match.group(1)
    return identity


def cache_epoch(root: Path, evaluation: str) -> str:
    policy = cache_policy(root)
    mode = str((policy.get("epochs") or {}).get(evaluation, "stable"))
    if mode == "stable":
        return "stable"
    if mode == "declared":
        # The epoch is a value a person sets, like a toolchain pin: records stay
        # valid until someone decides the outside world has changed enough to
        # measure it again. A calendar epoch expired every ecosystem record at
        # each month boundary, which re-bought about fifty dollars of
        # measurements that nothing had changed.
        declared = (policy.get("declared_epochs") or {}).get(evaluation)
        if not isinstance(declared, str) or not declared.strip():
            raise BenchmarkError(
                f"cache epoch for {evaluation} is 'declared' but declared_epochs names no value"
            )
        return declared.strip()
    if mode == "utc-month":
        run = json_load(root / "run.json")
        raw = str(run.get("created_at_utc") or "")
        try:
            stamp = dt.datetime.fromisoformat(raw.replace("Z", "+00:00"))
        except ValueError as exc:
            raise BenchmarkError("run created_at_utc is invalid for cache epoch") from exc
        return stamp.astimezone(dt.timezone.utc).strftime("%Y-%m")
    raise BenchmarkError(f"unsupported cache epoch mode: {mode}")


def cache_scope(unit: dict[str, Any]) -> str:
    if mechanical_unit(unit):
        return "mechanical-" + slug_id(str(unit.get("runner_action")))
    requirement_ids = [str(rid) for rid in (unit.get("requirement_ids") or [])]
    adjudication = support_adjudication_probe(requirement_ids)
    if adjudication is not None:
        return "cohort-" + slug_id(adjudication)
    if COMPARABILITY_GATE in requirement_ids:
        return "comparability"
    assigned = list(unit.get("assigned_languages", []) or [])
    canonical_probe = canonical_fragment_probe(requirement_ids)
    if canonical_probe is not None and len(assigned) == 1:
        return slug_id(assigned[0]) + "--" + slug_id(canonical_probe)
    if unit.get("result_kind") == "audit" and unit.get("reuse_audit_for"):
        return "audit-" + "-".join(slug_id(str(a)) for a in sorted(unit["reuse_audit_for"]))
    if len(assigned) == 1:
        return slug_id(assigned[0])
    return "comparison-" + sha256_bytes(
        json.dumps(assigned, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    )[:12]


def resolve_recorded_workspace_path(root: Path, raw: str | Path) -> Path:
    """Map a path recorded inside /quidra-benchmark back onto the current workspace."""
    recorded = Path(str(raw))
    if recorded.is_absolute():
        try:
            relative = recorded.relative_to(CANONICAL_WORKSPACE)
        except ValueError:
            return require_under(recorded, root)
        return require_under(root / relative, root)
    return require_under(root / recorded, root)


def cache_read_input_hashes(root: Path, task: dict[str, Any]) -> dict[str, str]:
    """Hash the exact readable inputs of a cacheable task using workspace-relative names."""
    rows: dict[str, str] = {}
    for raw in task.get("read_paths", []) or []:
        path = resolve_recorded_workspace_path(root, raw)
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            raise BenchmarkError(f"cacheable read path may not be a symlink: {relative}")
        if path.is_file():
            rows[relative] = sha256_file(path)
            continue
        if not path.is_dir():
            raise BenchmarkError(f"cacheable read path is missing: {relative}")
        h = hashlib.sha256()
        for child in sorted(path.rglob("*")):
            if child.is_symlink():
                raise BenchmarkError(
                    "cacheable read tree contains a symlink: "
                    + child.relative_to(root).as_posix()
                )
            if not child.is_file():
                continue
            child = require_under(child, root)
            child_rel = child.relative_to(path).as_posix()
            h.update(child_rel.encode("utf-8"))
            h.update(b"\0")
            h.update(sha256_file(child).encode("ascii"))
            h.update(b"\n")
        rows[relative] = h.hexdigest()
    return rows


def cache_fingerprint_payload(
    root: Path, unit: dict[str, Any], task: dict[str, Any]
) -> dict[str, Any] | None:
    if not cache_eligible_unit(root, unit):
        return None
    run = json_load(root / "run.json")
    identity = run.get("inference_identity") or {}
    provider = identity.get("provider")
    model = identity.get("model")
    mechanical = mechanical_unit(unit)
    if not mechanical and (not provider or not model):
        return None
    # The toolchain report is written by the toolchain check, which a run
    # performs before dispatch; the mechanical audit of the snapshot's own
    # programs is keyed before that and needs no comparison toolchain, while
    # a unit that does need one has no key until the report exists.
    report_path = root / "results" / "toolchains.json"
    toolchain_report = json_load(report_path) if report_path.is_file() else {}
    toolchains = toolchain_report.get("toolchains") or {}
    assigned = list(unit.get("assigned_languages", []) or [])
    if unit.get("result_kind") == "audit":
        assigned = audit_languages(root, unit)
        if not assigned:
            return None
    target = str(cache_policy(root).get("target_language") or "Quidra")
    if mechanical:
        # A language-sharded mechanical unit keeps its explicit assignment.
        # The interleaved micro cohort intentionally remains unassigned/all-language.
        if unit.get("runner_action") == "quidra-audit":
            assigned = [target]
        elif not assigned:
            assigned = list(metadata_languages(root))
        task = mechanical_task(unit)
    selected_toolchains: dict[str, str] = {}
    for language in assigned:
        if language == target:
            # The target is built from the snapshot, not installed from a pin;
            # its identity enters the key below instead of a toolchain row.
            continue
        row = toolchains.get(language) or {}
        canonical = row.get("canonical")
        if not canonical:
            return None
        selected_toolchains[language] = str(canonical)
    # Only the pins of the languages this unit measures enter its key. Hashing
    # the whole runtime manifest made a Zig pin bump invalidate every cached
    # Python, Rust and Go measurement, so one toolchain change forced a cold run.
    runtime_pins = (
        json_load(root / "template" / "runtime" / "toolchains.json").get("toolchains") or {}
    )
    pin_keys = {
        "Python": ["PYTHON_PIN"],
        "C++": ["CLANG_PIN", "CLANG_MAJOR", "CMAKE_PIN"],
        "Rust": ["RUST_PIN"],
        "Go": ["GO_PIN"],
        "Java": ["JAVA_PIN", "JAVA_BUILD"],
        "TypeScript": ["TYPESCRIPT_PIN", "NODE_PIN"],
        "Kotlin": ["KOTLIN_PIN", "JAVA_PIN", "JAVA_BUILD"],
        "Swift": ["SWIFT_PIN"],
        "Zig": ["ZIG_PIN"],
    }
    selected_pins = {
        language: {key: runtime_pins.get(key) for key in pin_keys.get(language, [])}
        for language in assigned
    }
    payload = {
        "schema_version": 1,
        "cache_schema_version": 1,
        "evaluation": unit.get("evaluation"),
        "work_unit_id": str(unit.get("id", "")),
        "requirement_ids": list(unit.get("requirement_ids", [])),
        "assigned_languages": assigned,
        "exact_task_packet_sha256": task.get("prompt_sha256"),
        "provider": None if mechanical else provider,
        "model": None if mechanical else model,
        "frozen_sampling": None if mechanical else sampling_config(root, str(unit.get("evaluation") or "")),
        "toolchains": selected_toolchains,
        "unit_input_hashes": unit.get("input_hashes") or {},
        "readable_input_content_hashes": cache_read_input_hashes(root, task),
        # Validator commands are runner-owned acceptance machinery, not part of
        # the paid semantic experiment. A stricter/new validator is applied again
        # on hydration, so changing only that command must not buy the same model
        # answer again.
        "worker_mode": unit.get("worker_mode"),
        "network_allowed": bool(unit.get("network_allowed")),
        "runtime_toolchain_pins": selected_pins,
        "cache_epoch": cache_epoch(
            root, "mechanical" if mechanical else str(unit.get("evaluation"))
        ),
    }
    support_probe = support_adjudication_probe(
        [str(rid) for rid in (unit.get("requirement_ids") or [])]
    )
    if not mechanical and support_probe is not None:
        # The paid experiment here is the semantic adjudication over the frozen
        # support input. Transport (packet-only vs another carrier), JSON/text
        # serialization and Task-Packet output instructions are runner concerns.
        # The generated support input is already content-hashed in readable
        # inputs, while rubric/config/model/sampling/toolchains remain in this
        # payload. Keep an explicit semantic-contract id so a real change to the
        # adjudication question is an intentional cache break.
        payload.pop("exact_task_packet_sha256", None)
        payload.pop("worker_mode", None)
        payload["work_unit_id"] = f"sc-support-adjudication:{support_probe}"
        payload["semantic_evidence_contract"] = "sc-support-adjudication-v1"
    if mechanical:
        # Mechanical records are measurements produced by this runner; their
        # validator/runner contract is part of the measurement implementation.
        # Preserve the historical key shape so existing mechanical cache stays
        # an exact hit. The semantic/validator separation above is for paid
        # model evidence only.
        payload["validator_contract"] = str(unit.get("validator_command") or "")
    if unit.get("evaluation") == "llm_proficiency":
        workload_contract_path = root / PROFICIENCY_WORKLOADS_RELATIVE
        if not workload_contract_path.is_file():
            return None
        payload["proficiency_workload_contract_sha256"] = sha256_file(
            workload_contract_path
        )
    if mechanical:
        payload["result_kind"] = "mechanical"
        payload["runner_action"] = str(unit.get("runner_action"))
        payload["measurement_script_hashes"] = measurement_script_hashes(root)
    if target in assigned:
        # A workspace whose snapshot declares no versions (the synthetic CI
        # harness stages no project.toml) has no key for Quidra work; the
        # unit simply runs. A real snapshot always declares them.
        if not (root / "repo" / "project.toml").is_file():
            return None
        payload["quidra_target"] = quidra_target_identity(root)
    if unit.get("result_kind") == "audit":
        payload["reuse_audit_for"] = sorted(str(a) for a in unit.get("reuse_audit_for", []))
        payload["result_kind"] = "audit"
    return payload


#: Evaluations whose certified records carry scored model trials, and whose
#: reuse therefore depends on the output cap those trials ran under.
TRIAL_EVALUATIONS = ("llm_learnability", "llm_proficiency")


def trial_cap_evidence(unit: dict[str, Any], trace: dict[str, Any]) -> dict[str, Any]:
    """What a unit's scored trials did against their output cap.

    The cap is not part of the cache key: a trial that finished well inside it
    is the same measurement under any larger cap, and keying on the cap would
    throw that measurement away every time the cap moved. What the record
    needs instead is the evidence to decide reuse later - how many trial calls
    the cap cut off, and the largest completion any call produced.
    """
    calls = 0
    truncated = 0
    largest = 0
    for session in ((trace.get("trials") or {}).get("trials") or {}).values():
        for call in session.get("calls", []) or []:
            calls += 1
            if str(call.get("stop_reason") or "") == "max_tokens":
                truncated += 1
            largest = max(largest, int((call.get("usage") or {}).get("output_tokens", 0) or 0))
    return {
        "scored_output_cap": int(unit.get("max_output_tokens_per_call", 0) or 0),
        "trial_calls": calls,
        "cap_truncated_trial_calls": truncated,
        "max_trial_output_tokens": largest,
    }


def cache_cap_reuse_problem(
    root: Path, record: dict[str, Any], unit: dict[str, Any]
) -> str | None:
    """Why a certified trial record cannot stand in for a run at the current cap.

    Same cap: same experiment, reusable. A different cap: reusable only when the
    cap demonstrably never mattered - no call was cut off, and no completion
    was larger than the cap that applies now. A record that predates this
    evidence is not reusable until `cache-annotate-caps` has read the run's
    agent traces and written it in.
    """
    evaluation = str(unit.get("evaluation") or "")
    if evaluation not in TRIAL_EVALUATIONS:
        return None
    certification = record.get("certification") or {}
    if evaluation == "llm_proficiency":
        expected_hash = proficiency_primary_trial_set_sha256(root)
        expected_count = len(proficiency_required_trial_ids(root))
        recorded_hash = certification.get("proficiency_primary_trial_set_sha256")
        recorded_count = certification.get("proficiency_primary_trial_count")
        if recorded_hash != expected_hash or recorded_count != expected_count:
            return (
                "the Proficiency record predates or disagrees with the exact "
                f"Primary trial allocation ({expected_count} frozen trials); "
                "it must be measured again"
            )
        assigned = [str(value) for value in (unit.get("assigned_languages") or [])]
        if len(assigned) != 1:
            return "the Proficiency cache unit does not identify exactly one language"
        expected_prompts = proficiency_primary_prompt_set_sha256(root, assigned[0])
        if certification.get("proficiency_primary_prompt_set_sha256") != expected_prompts:
            return (
                "the Proficiency record was not certified against the current "
                "runtime-owned workload/scenario prompt set; it must be measured again"
            )
        if certification.get("proficiency_toolchain_evidence") is not True:
            return (
                "the Proficiency record is not certified to have run the assigned "
                "language toolchain successfully before its first scored trial; "
                "it must be measured again"
            )
        if certification.get("proficiency_runtime_verification") is not True:
            return (
                "the Proficiency record predates runtime-owned per-completion "
                "compile/run verification; it must be measured again"
            )
        expected_contract = proficiency_workload_contract_sha256(root)
        if certification.get("proficiency_workload_contract_sha256") != expected_contract:
            return (
                "the Proficiency record was not certified against the current "
                "hidden-oracle workload contract; it must be measured again"
            )
    current = int(unit.get("max_output_tokens_per_call", 0) or 0)
    if current <= 0:
        return None
    required = ("scored_output_cap", "cap_truncated_trial_calls", "max_trial_output_tokens")
    if any(key not in certification for key in required):
        return (
            "the record carries no scored-cap evidence; run cache-annotate-caps on the "
            "run's evidence before it can be reused"
        )
    recorded = int(certification["scored_output_cap"] or 0)
    truncated = int(certification["cap_truncated_trial_calls"] or 0)
    largest = int(certification["max_trial_output_tokens"] or 0)
    if recorded == current:
        return None
    if truncated > 0:
        return (
            f"{truncated} trial call(s) were cut off at the recorded cap {recorded}; "
            f"the current cap is {current}, so the measurement must be repeated"
        )
    if largest > current:
        return (
            f"a trial completion used {largest} output tokens, above the current cap "
            f"{current}"
        )
    return None


def _prompt_store_manifest(
    store_root: Path, prompt_sha256: str
) -> dict[str, Any] | None:
    path = store_root / "manifests" / "by-hash" / f"{prompt_sha256}.json"
    if not path.is_file():
        return None
    data = json_load(path)
    if data.get("schema_version") != 1 or data.get("prompt_sha256") != prompt_sha256:
        return None
    return data


def _prompt_store_component_text(
    store_root: Path, component_sha256: str
) -> str | None:
    for suffix in (".md", ".json", ".txt"):
        path = store_root / "components" / "by-hash" / f"{component_sha256}{suffix}"
        if path.is_file():
            return path.read_text(encoding="utf-8")
    return None


def _primary_config_from_prompt_store(
    store_root: Path, prompt_sha256: str
) -> dict[str, Any] | None:
    manifest = _prompt_store_manifest(store_root, prompt_sha256)
    if manifest is None:
        return None
    for component in manifest.get("components", []) or []:
        if str(component.get("kind") or "") != "embedded:primary.json":
            continue
        text = _prompt_store_component_text(
            store_root, str(component.get("sha256") or "")
        )
        if text is None:
            return None
        start = text.find("{")
        if start < 0:
            return None
        try:
            parsed = json.loads(text[start:])
        except json.JSONDecodeError:
            return None
        return parsed if isinstance(parsed, dict) else None
    return None


def _scoped_prompt_component_kinds(evaluation: str) -> set[str]:
    return {
        "embedded:primary.json",
        f"embedded:{EVALUATION_SPEC_FILES[evaluation]}",
        "embedded:assigned_requirements.json",
    }


def _prompt_component_signature_without_scoped(
    components: Iterable[dict[str, Any]], evaluation: str
) -> list[tuple[str, str]]:
    scoped = _scoped_prompt_component_kinds(evaluation)
    return [
        (str(component.get("kind") or ""), str(component.get("sha256") or ""))
        for component in components
        if str(component.get("kind") or "") not in scoped
    ]


def _prompt_component_sha(
    components: Iterable[dict[str, Any]], kind: str
) -> str | None:
    """Return a validated component digest without requiring its stored bytes."""
    for component in components:
        if str(component.get("kind") or "") != kind:
            continue
        digest = str(component.get("sha256") or "")
        return digest if re.fullmatch(r"[0-9a-f]{64}", digest) else None
    return None


def _embedded_component_body(text: str) -> str | None:
    marker = "Source SHA-256:"
    start = text.find(marker)
    if start < 0:
        return None
    body = text.find("\n\n", start)
    if body < 0:
        return None
    return text[body + 2:]


def _stored_prompt_component_body(
    store_root: Path, prompt_sha256: str, kind: str
) -> str | None:
    manifest = _prompt_store_manifest(store_root, prompt_sha256)
    if manifest is None:
        return None
    for component in manifest.get("components", []) or []:
        if str(component.get("kind") or "") != kind:
            continue
        text = _prompt_store_component_text(
            store_root, str(component.get("sha256") or "")
        )
        return _embedded_component_body(text) if text is not None else None
    return None


def _task_prompt_component_body(task: dict[str, Any], kind: str) -> str | None:
    for component in task.get("prompt_components", []) or []:
        if str(component.get("kind") or "") != kind:
            continue
        path = Path(str(component.get("path") or ""))
        if not path.is_file():
            return None
        return _embedded_component_body(path.read_text(encoding="utf-8"))
    return None


def _cache_payload_without_scoped_prompt(
    payload: dict[str, Any],
) -> dict[str, Any]:
    normalized = dict(payload)
    normalized.pop("exact_task_packet_sha256", None)
    # Historical records included the validator command in their paid-result
    # fingerprint. Validation is runner-owned and current hydration always runs
    # the current validator, so it is safe (and necessary for reuse) to project
    # this field away when comparing legacy paid records.
    normalized.pop("validator_contract", None)
    hashes = dict(normalized.get("unit_input_hashes") or {})
    # Old records used the whole files. Current records use only the selected
    # evaluation section and Primary projection. Exact scoped component bodies
    # are compared below before any paid record is accepted.
    for key in ("primary_config", "evaluation_spec", "evaluation_spec_sections"):
        hashes.pop(key, None)
    normalized["unit_input_hashes"] = hashes
    return normalized


def _legacy_primary_prompt_compatible(
    root: Path,
    record: dict[str, Any],
    current_payload: dict[str, Any],
    task: dict[str, Any],
) -> bool:
    """Allow old paid records to cross safe scoped-prompt cache migrations.

    All non-scoped dependencies must match exactly. The old Primary file is
    projected to this evaluation, while the selected methodology and assigned
    requirement component bodies must be byte-identical. This rescues paid work
    from unrelated edits without accepting a semantically changed Task Packet.
    """
    evaluation = str(current_payload.get("evaluation") or "")
    old_payload = record.get("fingerprint_payload") or {}
    if _cache_payload_without_scoped_prompt(old_payload) != (
        _cache_payload_without_scoped_prompt(current_payload)
    ):
        return False
    old_prompt = str(old_payload.get("exact_task_packet_sha256") or "")
    store_root = root / "template" / "prompts"
    old_manifest = _prompt_store_manifest(store_root, old_prompt)
    if old_manifest is None:
        return False
    if _prompt_component_signature_without_scoped(
        old_manifest.get("components", []) or [], evaluation
    ) != _prompt_component_signature_without_scoped(
        task.get("prompt_components", []) or [], evaluation
    ):
        return False
    old_primary = _primary_config_from_prompt_store(store_root, old_prompt)
    if old_primary is None or primary_config_projection_from_data(
        old_primary, evaluation
    ) != primary_config_projection_data(root, evaluation):
        return False
    old_components = old_manifest.get("components", []) or []
    current_components = task.get("prompt_components", []) or []
    for kind in (
        f"embedded:{EVALUATION_SPEC_FILES[evaluation]}",
        "embedded:assigned_requirements.json",
    ):
        # Equal content digests already prove byte identity. This matters for
        # paid legacy records whose manifest survived but whose unchanged
        # methodology component was never separately promoted into the prompt
        # store. Fall back to body comparison only for migrations that changed
        # the component wrapper while preserving the selected semantic body.
        old_sha = _prompt_component_sha(old_components, kind)
        current_sha = _prompt_component_sha(current_components, kind)
        if old_sha is not None and old_sha == current_sha:
            continue
        old_body = _stored_prompt_component_body(store_root, old_prompt, kind)
        current_body = _task_prompt_component_body(task, kind)
        if old_body is None or current_body is None or old_body != current_body:
            return False
    return True



def _validator_recertification_config(
    root: Path, evaluation: str
) -> dict[str, Any] | None:
    """Return the narrowly approved legacy-result recertification rule.

    This is deliberately not a generic "ignore the cache epoch" escape hatch.
    An evaluation must opt in explicitly, list the historical epochs it accepts,
    and name the exact fingerprint fields whose change is runner-verifiable.
    The old result is still passed through the current validator before it can
    complete the leaf or be checkpointed under the current fingerprint.
    """
    raw = (
        (cache_policy(root).get("reuse_conditions") or {})
        .get("validator_recertification", {})
    )
    if not isinstance(raw, dict):
        raise BenchmarkError("validator_recertification cache policy must be an object")
    cfg = raw.get(evaluation)
    if cfg is None:
        return None
    if not isinstance(cfg, dict):
        raise BenchmarkError(
            f"validator_recertification policy for {evaluation} must be an object"
        )
    for key in (
        "legacy_epochs",
        "ignored_payload_fields",
        "ignored_unit_input_hashes",
        "ignored_readable_input_hashes",
    ):
        values = cfg.get(key)
        if not isinstance(values, list) or not all(
            isinstance(value, str) and value for value in values
        ):
            raise BenchmarkError(
                f"validator_recertification.{evaluation}.{key} "
                "must be a string array"
            )
    if "ignore_canonical_fragment_catalog" in cfg and not isinstance(
        cfg["ignore_canonical_fragment_catalog"], bool
    ):
        raise BenchmarkError(
            f"validator_recertification.{evaluation}."
            "ignore_canonical_fragment_catalog must be boolean"
        )
    return cfg


def _validator_recertification_payload(
    payload: dict[str, Any], cfg: dict[str, Any]
) -> dict[str, Any]:
    """Project away only fields the current validator can independently re-prove."""
    normalized = json.loads(json.dumps(payload))

    for field in cfg.get("ignored_payload_fields", []):
        normalized.pop(str(field), None)

    hashes = dict(normalized.get("unit_input_hashes") or {})
    if str(normalized.get("evaluation") or "") == "semantic_compression":
        hashes = _normalize_sc_evaluation_spec_hash_aliases(hashes)
    for field in cfg.get("ignored_unit_input_hashes", []):
        hashes.pop(str(field), None)
    normalized["unit_input_hashes"] = hashes

    reads = dict(normalized.get("readable_input_content_hashes") or {})
    for field in cfg.get("ignored_readable_input_hashes", []):
        reads.pop(str(field), None)
    if cfg.get("ignore_canonical_fragment_catalog") is True:
        reads = {
            key: value
            for key, value in reads.items()
            if not (
                str(key).startswith(
                    "work/audit/semantic-compression/canonical_fragments_"
                )
                and str(key).endswith(".json")
            )
        }
    normalized["readable_input_content_hashes"] = reads
    return normalized


def _semantic_validator_recertification_payloads_compatible(
    root: Path,
    unit: dict[str, Any],
    record: dict[str, Any],
    current_payload: dict[str, Any],
    cfg: dict[str, Any],
) -> bool:
    """Prove legacy full-file hashes are equivalent to current SC projections.

    Historical SC records hashed all of primary.json and semantic_compression.md.
    Current units hash only the evaluation-visible Primary projection and the
    selected methodology sections.  The migration is accepted only for an
    approved immutable source run and only when the raw old full-file digests
    and current projected digests match the reviewed bridge exactly.
    """
    old_payload = record.get("fingerprint_payload") or {}
    if (
        str(old_payload.get("evaluation") or "") != "semantic_compression"
        or str(current_payload.get("evaluation") or "") != "semantic_compression"
    ):
        return False

    raw_old_hashes = dict(old_payload.get("unit_input_hashes") or {})
    raw_current_hashes = dict(current_payload.get("unit_input_hashes") or {})
    raw_old_primary = raw_old_hashes.get("primary_config")
    raw_current_primary = raw_current_hashes.get("primary_config")
    raw_old_spec = (
        raw_old_hashes.get("evaluation_spec_sections")
        if "evaluation_spec_sections" in raw_old_hashes
        else raw_old_hashes.get("evaluation_spec")
    )
    raw_current_spec = (
        raw_current_hashes.get("evaluation_spec_sections")
        if "evaluation_spec_sections" in raw_current_hashes
        else raw_current_hashes.get("evaluation_spec")
    )
    projection_required = (
        raw_old_primary != raw_current_primary
        or raw_old_spec != raw_current_spec
        or (
            "evaluation_spec" in raw_old_hashes
            and "evaluation_spec_sections" in raw_current_hashes
        )
    )

    old = _validator_recertification_payload(old_payload, cfg)
    current = _validator_recertification_payload(current_payload, cfg)
    if old == current and not projection_required:
        return True

    run_id = str((record.get("provenance") or {}).get("run_id") or "")
    selectors = [
        str(value) for value in (unit.get("prompt_sections") or [])
    ]
    source_projection = _legacy_sc_source_projection_attestation(
        root, run_id, selectors
    )
    if source_projection is None:
        return False

    old_hashes = dict(old.get("unit_input_hashes") or {})
    current_hashes = dict(current.get("unit_input_hashes") or {})
    old_hashes.pop("primary_config", None)
    current_hashes.pop("primary_config", None)
    old_hashes.pop("evaluation_spec", None)
    old_hashes.pop("evaluation_spec_sections", None)
    current_hashes.pop("evaluation_spec", None)
    current_hashes.pop("evaluation_spec_sections", None)
    old["unit_input_hashes"] = old_hashes
    current["unit_input_hashes"] = current_hashes
    if old != current:
        return False

    if raw_old_primary != raw_current_primary:
        if (
            raw_old_primary
            != source_projection.get("primary_config_full_sha256")
            or raw_current_primary
            != source_projection.get("primary_config_projection_sha256")
            or raw_current_primary
            != primary_config_projection_sha256(root, "semantic_compression")
        ):
            return False

    if raw_old_spec != raw_current_spec:
        if (
            raw_old_spec
            != source_projection.get("evaluation_spec_full_sha256")
            or raw_current_spec
            != source_projection.get("evaluation_spec_projection_sha256")
            or raw_current_spec
            != evaluation_spec_projection_sha256(
                root, "semantic_compression", selectors
            )
        ):
            return False

    return True


def _semantic_validator_recertification_mismatch_summary(
    root: Path,
    unit: dict[str, Any],
    record: dict[str, Any],
    current_payload: dict[str, Any],
    cfg: dict[str, Any],
) -> str:
    """Explain a rejected SC compatibility candidate without relaxing its checks."""
    old_payload = record.get("fingerprint_payload") or {}
    run_id = str((record.get("provenance") or {}).get("run_id") or "")
    selectors = [str(value) for value in (unit.get("prompt_sections") or [])]
    source_projection = _legacy_sc_source_projection_attestation(
        root, run_id, selectors
    )
    if source_projection is None:
        return "source-snapshot projection attestation is missing or invalid"

    old = _validator_recertification_payload(old_payload, cfg)
    current = _validator_recertification_payload(current_payload, cfg)
    differing: list[str] = []
    for key in sorted(set(old) | set(current)):
        old_value = old.get(key)
        current_value = current.get(key)
        if old_value == current_value:
            continue
        if isinstance(old_value, dict) and isinstance(current_value, dict):
            nested = [
                str(subkey)
                for subkey in sorted(set(old_value) | set(current_value))
                if old_value.get(subkey) != current_value.get(subkey)
            ]
            differing.append(f"{key}[{','.join(nested)}]")
        else:
            differing.append(key)

    raw_old_hashes = dict(old_payload.get("unit_input_hashes") or {})
    raw_current_hashes = dict(current_payload.get("unit_input_hashes") or {})
    projection_notes: list[str] = []
    raw_old_primary = raw_old_hashes.get("primary_config")
    raw_current_primary = raw_current_hashes.get("primary_config")
    if raw_old_primary != raw_current_primary and (
        raw_old_primary
        != source_projection.get("primary_config_full_sha256")
        or raw_current_primary
        != source_projection.get("primary_config_projection_sha256")
    ):
        projection_notes.append("primary_config projection proof mismatch")
    raw_old_spec = (
        raw_old_hashes.get("evaluation_spec_sections")
        if "evaluation_spec_sections" in raw_old_hashes
        else raw_old_hashes.get("evaluation_spec")
    )
    raw_current_spec = (
        raw_current_hashes.get("evaluation_spec_sections")
        if "evaluation_spec_sections" in raw_current_hashes
        else raw_current_hashes.get("evaluation_spec")
    )
    if raw_old_spec != raw_current_spec and (
        raw_old_spec
        != source_projection.get("evaluation_spec_full_sha256")
        or raw_current_spec
        != source_projection.get("evaluation_spec_projection_sha256")
    ):
        projection_notes.append("evaluation_spec projection proof mismatch")
    if not differing and not projection_notes:
        return "candidate failed a strict SC projection compatibility check"
    return "; ".join(
        (["normalized fields differ: " + ", ".join(differing)] if differing else [])
        + projection_notes
    )


def find_validator_recertifiable_cache_record(
    root: Path,
    unit: dict[str, Any],
    current_payload: dict[str, Any],
    task: dict[str, Any] | None = None,
) -> tuple[Path | None, dict[str, Any] | None, str | None]:
    """Find historical paid evidence that today's validator can re-prove.

    The returned record is projected to the current fingerprint only in memory.
    hydrate_certified_cache stages its result under the current task and runs the
    current validator. Rejection becomes a normal leaf-local MISS; only a PASS
    can complete the leaf and later be checkpointed under the current key.

    LLM Proficiency intentionally has no rule here: its prompt/repair trajectory
    is itself the measured experiment, so a newer validator cannot retroactively
    make an old trial allocation or hidden-oracle feedback policy equivalent.
    """
    evaluation = str(unit.get("evaluation") or "")
    cfg = _validator_recertification_config(root, evaluation)
    if cfg is None:
        return None, None, None
    if cfg.get("language_scoped_only") is True and not (
        unit.get("assigned_languages") or []
    ):
        return None, None, None

    current_epoch = str(current_payload.get("cache_epoch") or "")
    legacy_epochs = [
        str(value) for value in (cfg.get("legacy_epochs") or [])
    ]
    priority = {value: index for index, value in enumerate(legacy_epochs)}
    directory = (
        root
        / "cache"
        / "v1"
        / slug_id(evaluation or "unknown")
        / cache_scope(unit)
    )
    if not directory.is_dir():
        return None, None, None

    normalized_current = _validator_recertification_payload(
        current_payload, cfg
    )
    matches: list[tuple[int, str, str, Path, dict[str, Any]]] = []
    invalid_candidates: list[str] = []
    semantic_incompatible_candidates: list[str] = []
    for path in sorted(directory.glob("*.json")):
        try:
            record = json_load(path)
        except (OSError, json.JSONDecodeError, TypeError, ValueError):
            continue
        old = record.get("fingerprint_payload") or {}
        if str(old.get("work_unit_id") or "") != str(unit.get("id") or ""):
            continue
        if evaluation == "semantic_compression":
            source_run = str((record.get("provenance") or {}).get("run_id") or "")
            if source_run not in LEGACY_SC_SOURCE_SNAPSHOTS:
                continue
        old_epoch = str(old.get("cache_epoch") or "")
        if old_epoch == current_epoch or old_epoch not in priority:
            continue
        if evaluation == "semantic_compression":
            if not _semantic_validator_recertification_payloads_compatible(
                root, unit, record, current_payload, cfg
            ):
                semantic_incompatible_candidates.append(
                    f"{path.name}: "
                    + _semantic_validator_recertification_mismatch_summary(
                        root, unit, record, current_payload, cfg
                    )
                )
                continue
        elif _validator_recertification_payload(old, cfg) != normalized_current:
            continue
        problem = _cache_record_self_integrity_problem(record)
        if problem:
            invalid_candidates.append(f"{path.name}: {problem}")
            continue
        run_id = str((record.get("provenance") or {}).get("run_id") or "")
        matches.append(
            (priority[old_epoch], run_id, path.name, path, record)
        )

    if not matches:
        if invalid_candidates:
            return (
                None,
                None,
                "all validator-recertification candidates failed self-integrity: "
                + "; ".join(invalid_candidates[:8]),
            )
        if evaluation == "semantic_compression" and semantic_incompatible_candidates:
            return (
                None,
                None,
                "all Semantic Compression validator-recertification candidates "
                "were incompatible: "
                + "; ".join(semantic_incompatible_candidates[:8]),
            )
        return None, None, None

    # Never choose by score/result. Prefer the newest explicitly allowed
    # protocol generation, then the newest run id, then a stable path tie-break.
    # This makes repeated historical measurements deterministic without
    # cherry-picking whichever output ranks a language best.
    _, _, _, path, record = sorted(matches)[-1]
    old_payload = record.get("fingerprint_payload") or {}
    source_fingerprint = str(record.get("fingerprint") or "")
    current_fingerprint = sha256_bytes(
        json.dumps(
            current_payload,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=False,
        ).encode("utf-8")
    )
    projected = {
        **record,
        "fingerprint": current_fingerprint,
        "fingerprint_payload": current_payload,
        "evaluation": evaluation,
        "assigned_languages": list(unit.get("assigned_languages", [])),
        "certification": {
            **(record.get("certification") or {}),
            "validator_recertification_candidate": True,
            "validator_recertification_source_epoch": old_payload.get(
                "cache_epoch"
            ),
            "validator_recertification_target_epoch": current_epoch,
        },
        "provenance": {
            **(record.get("provenance") or {}),
            "projection_source_fingerprint": source_fingerprint,
            "projection_source_epoch": old_payload.get("cache_epoch"),
            "work_unit_id": unit.get("id"),
        },
    }
    if task is not None and evaluation == "semantic_compression":
        if unit.get("canonical_fragment_owner"):
            projected, projection_problem = project_semantic_owner_recertification(
                root, unit, projected, path
            )
        else:
            projected, projection_problem = project_semantic_consumer_recertification(
                root, unit, task, projected, source_path=path
            )
        if projection_problem:
            return None, None, projection_problem
        if projected is None:
            return None, None, "semantic recertification projection produced no record"
    return path, projected, None


def ecosystem_snapshot_source_record_problem(
    cache_root: Path,
    language: str,
    requirement_id: str,
    row: dict[str, Any],
) -> str | None:
    """Prove that one v2 snapshot row is anchored to preserved paid evidence.

    The snapshot is a trusted re-adjudication layer, not a replacement for its
    source evidence.  Every row therefore names the exact legacy cache record,
    result hash and work unit it was derived from.  If that source disappears,
    is corrupted, or no longer contains the language/metric being re-adjudicated,
    the row is not eligible for cache recertification.
    """
    raw = str(row.get("source_record") or "")
    prefix = "benchmark/cache/"
    if not raw.startswith(prefix):
        return (
            f"Ecosystem snapshot {language}/{requirement_id} source_record must "
            "name benchmark/cache/v1/ecosystem evidence"
        )
    relative = PurePosixPath(raw[len(prefix):])
    if (
        relative.is_absolute()
        or any(part in {"", ".", ".."} for part in relative.parts)
        or len(relative.parts) < 4
        or relative.parts[0] != "v1"
        or relative.parts[1] != "ecosystem"
        or relative.suffix != ".json"
    ):
        return (
            f"Ecosystem snapshot {language}/{requirement_id} has invalid "
            f"source_record path: {raw!r}"
        )
    source_path = require_under(cache_root.joinpath(*relative.parts), cache_root)
    if not source_path.is_file():
        return (
            f"Ecosystem snapshot {language}/{requirement_id} source record is "
            f"missing: {raw}"
        )
    try:
        source = json_load(source_path)
    except (OSError, json.JSONDecodeError, TypeError, ValueError) as exc:
        return (
            f"Ecosystem snapshot {language}/{requirement_id} source record is "
            f"unreadable/corrupt: {type(exc).__name__}: {exc}"
        )
    integrity = _cache_record_self_integrity_problem(source)
    if integrity:
        return (
            f"Ecosystem snapshot {language}/{requirement_id} source record "
            f"failed self-integrity: {integrity}"
        )
    if str(source.get("evaluation") or "") != "ecosystem":
        return (
            f"Ecosystem snapshot {language}/{requirement_id} source record "
            "is not Ecosystem evidence"
        )
    if language not in [str(value) for value in (source.get("assigned_languages") or [])]:
        return (
            f"Ecosystem snapshot {language}/{requirement_id} source record "
            "does not contain the assigned language"
        )
    expected_work_unit = str(row.get("source_work_unit_id") or "")
    actual_work_unit = str((source.get("provenance") or {}).get("work_unit_id") or "")
    if not expected_work_unit or actual_work_unit != expected_work_unit:
        return (
            f"Ecosystem snapshot {language}/{requirement_id} source work unit "
            f"mismatch: expected={expected_work_unit!r}, actual={actual_work_unit!r}"
        )
    expected_result = str(row.get("source_result_sha256") or "")
    actual_result = str(source.get("result_sha256") or "")
    if (
        not re.fullmatch(r"[0-9a-f]{64}", expected_result)
        or actual_result != expected_result
    ):
        return (
            f"Ecosystem snapshot {language}/{requirement_id} source result_sha256 "
            f"mismatch: expected={expected_result!r}, actual={actual_result!r}"
        )
    cells = ((source.get("result") or {}).get("requirements") or {}).get(
        requirement_id
    )
    if not isinstance(cells, dict) or language not in cells:
        return (
            f"Ecosystem snapshot {language}/{requirement_id} source record "
            "does not contain the re-adjudicated metric cell"
        )
    return None


def ecosystem_snapshot_recertification_record(
    root: Path,
    unit: dict[str, Any],
    task: dict[str, Any],
    current_payload: dict[str, Any],
) -> tuple[Path | None, dict[str, Any] | None, str | None]:
    """Synthesize current Ecosystem evidence from the trusted v2 snapshot cache.

    Historical workers gathered valuable evidence but some used language-local
    rubrics.  This bridge never reuses those legacy normalized scores.  The
    centrally reviewed snapshot stores only current-v2 component judgments and
    provenance back to the preserved paid work.  We synthesize the ordinary
    current worker shape, then hydrate_certified_cache runs the full current
    validator/runner arithmetic before the leaf may complete or be checkpointed
    under today's exact fingerprint.
    """
    if str(unit.get("evaluation") or "") != "ecosystem":
        return None, None, None
    assigned = [str(value) for value in (unit.get("assigned_languages") or [])]
    requirement_ids = [
        str(value) for value in (unit.get("requirement_ids") or [])
        if str(value).startswith("metric.")
    ]
    if len(assigned) != 1 or not requirement_ids:
        return None, None, None

    cfg = (
        (cache_policy(root).get("reuse_conditions") or {})
        .get("ecosystem_snapshot_recertification")
    )
    if not isinstance(cfg, dict):
        return None, None, None
    relative = str(cfg.get("path") or "")
    rel_path = PurePosixPath(relative)
    if (
        not relative
        or rel_path.is_absolute()
        or any(part in {"", ".", ".."} for part in rel_path.parts)
    ):
        return None, None, "invalid Ecosystem snapshot-cache path in cache policy"
    path = require_under(root / "cache" / rel_path, root)
    if not path.is_file():
        return None, None, None
    try:
        snapshot = json_load(path)
    except (OSError, json.JSONDecodeError, TypeError, ValueError) as exc:
        return None, None, (
            "Ecosystem snapshot cache is unreadable/corrupt: "
            f"{type(exc).__name__}: {exc}"
        )
    if snapshot.get("schema_version") != 1 or snapshot.get("frozen") is not True:
        return None, None, "Ecosystem snapshot cache must be frozen schema_version 1"

    asset = ecosystem_rubric_asset(root)
    expected_date = str((asset.get("evidence_policy") or {}).get("snapshot_date") or "")
    if snapshot.get("rubric_set_id") != asset.get("rubric_set_id"):
        return None, None, "Ecosystem snapshot rubric_set_id differs from current frozen rubric"
    if snapshot.get("snapshot_date") != expected_date:
        return None, None, "Ecosystem snapshot date differs from current frozen evidence date"
    if snapshot.get("cache_epoch") != cache_epoch(root, "ecosystem"):
        return None, None, "Ecosystem snapshot epoch differs from current cache epoch"

    language = assigned[0]
    if language == str(cache_policy(root).get("target_language") or "Quidra"):
        current_identity = current_quidra_execution_identity(root)
        snapshot_identity = snapshot.get("quidra_execution_identity")
        if current_identity is None or snapshot_identity != current_identity:
            # A changed target is not a corrupt snapshot.  It simply needs fresh
            # Quidra evidence; comparison-language snapshot rows remain reusable.
            return None, None, None

    language_row = (snapshot.get("languages") or {}).get(language)
    if not isinstance(language_row, dict):
        return None, None, f"Ecosystem snapshot has no row for {language}"
    snapshot_metrics = language_row.get("metrics")
    if not isinstance(snapshot_metrics, dict):
        return None, None, f"Ecosystem snapshot metrics are missing for {language}"

    evidence: dict[str, Any] = {
        "snapshot_recertification": {
            "snapshot_id": snapshot.get("snapshot_id"),
            "rubric_set_id": snapshot.get("rubric_set_id"),
            "source": path.relative_to(root / "cache").as_posix(),
            "policy": "legacy evidence re-adjudicated under current runner-owned rubric",
        }
    }
    requirements: dict[str, dict[str, float]] = {}
    points = asset["scoring"]["level_points"]
    for rid in requirement_ids:
        rubric = asset["metrics"].get(rid)
        row = snapshot_metrics.get(rid)
        if not isinstance(rubric, dict) or not isinstance(row, dict):
            return None, None, f"Ecosystem snapshot is missing {language}/{rid}"
        component_ids = [str(item["id"]) for item in rubric["components"]]
        levels = row.get("component_levels")
        findings = row.get("component_findings")
        if not isinstance(levels, dict) or set(levels) != set(component_ids):
            return None, None, (
                f"Ecosystem snapshot {language}/{rid} component_levels differ "
                "from the current rubric"
            )
        if not isinstance(findings, dict) or set(findings) != set(component_ids):
            return None, None, (
                f"Ecosystem snapshot {language}/{rid} component_findings differ "
                "from the current rubric"
            )
        expected_score = 0
        normalized_levels: dict[str, int] = {}
        for cid in component_ids:
            level = levels[cid]
            if (
                isinstance(level, bool)
                or not isinstance(level, int)
                or level not in {0, 1, 2, 3, 4}
            ):
                return None, None, (
                    f"Ecosystem snapshot {language}/{rid}/{cid} has invalid level"
                )
            if not isinstance(findings[cid], str) or not findings[cid].strip():
                return None, None, (
                    f"Ecosystem snapshot {language}/{rid}/{cid} has empty finding"
                )
            normalized_levels[cid] = level
            expected_score += int(points[str(level)])
        if row.get("score_0_100") != expected_score:
            return None, None, (
                f"Ecosystem snapshot {language}/{rid} score does not match "
                "current runner arithmetic"
            )
        sources = row.get("sources")
        if (
            not isinstance(sources, list)
            or not sources
            or not all(isinstance(value, str) and value.strip() for value in sources)
        ):
            return None, None, f"Ecosystem snapshot {language}/{rid} has no sources"
        candidate_universe = row.get("candidate_universe")
        if not isinstance(candidate_universe, str) or not candidate_universe.strip():
            return None, None, (
                f"Ecosystem snapshot {language}/{rid} has no candidate universe"
            )
        limitations = row.get("limitations")
        if not isinstance(limitations, str):
            return None, None, (
                f"Ecosystem snapshot {language}/{rid} limitations must be a string"
            )
        provenance_problem = ecosystem_snapshot_source_record_problem(
            root / "cache", language, rid, row
        )
        if provenance_problem:
            return None, None, provenance_problem
        evidence[rid] = {
            "rubric_id": rubric["rubric_id"],
            "component_levels": normalized_levels,
            "component_findings": dict(findings),
            "sources": list(sources),
            "snapshot_date": expected_date,
            "limitations": limitations,
            "candidate_universe": candidate_universe,
            "selection_rule": rubric["selection_rule"],
            "retrieval_route": "provider-brokered web search",
            "legacy_score_0_100": row.get("legacy_score_0_100"),
            "source_work_unit_id": row.get("source_work_unit_id"),
        }
        requirements[rid] = {language: float(expected_score)}

    result = {
        "schema_version": 1,
        "evaluation": "ecosystem",
        "requirements": requirements,
        "evidence": evidence,
    }
    result_raw = json.dumps(
        result, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    fingerprint = sha256_bytes(
        json.dumps(
            current_payload,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=False,
        ).encode("utf-8")
    )
    compatibility: dict[str, Any] = {}
    if language == str(cache_policy(root).get("target_language") or "Quidra"):
        compatibility["quidra_execution_identity"] = snapshot.get(
            "quidra_execution_identity"
        )
    record = {
        "schema_version": 1,
        "fingerprint": fingerprint,
        "fingerprint_payload": current_payload,
        "evaluation": "ecosystem",
        "assigned_languages": assigned,
        "result": result,
        "result_sha256": sha256_bytes(result_raw),
        "certification": {
            "unit_complete": True,
            "primary_complete": False,
            "validator_pass": False,
            "ecosystem_snapshot_recertification_candidate": True,
            "ecosystem_snapshot_id": snapshot.get("snapshot_id"),
        },
        "provenance": {
            "run_id": f"snapshot:{snapshot.get('snapshot_id')}",
            "work_unit_id": unit.get("id"),
            "prompt_sha256": task.get("prompt_sha256"),
            "projection_source_fingerprint": sha256_file(path),
        },
    }
    if compatibility:
        record["compatibility"] = compatibility
    return path, record, None


def _cache_record_self_integrity_problem(record: dict[str, Any]) -> str | None:
    payload = record.get("fingerprint_payload")
    if record.get("schema_version") != 1 or not isinstance(payload, dict):
        return "schema/payload"
    expected_fingerprint = sha256_bytes(
        json.dumps(
            payload, sort_keys=True, separators=(",", ":"), ensure_ascii=False
        ).encode("utf-8")
    )
    if record.get("fingerprint") != expected_fingerprint:
        return "fingerprint"
    expected_result = sha256_bytes(
        json.dumps(
            record.get("result"), sort_keys=True, separators=(",", ":")
        ).encode("utf-8")
    )
    if record.get("result_sha256") != expected_result:
        return "result"
    return None


CACHE_MIGRATION_RULE_VERSION = 1

CACHE_MIGRATION_RULES: dict[str, dict[str, Any]] = {
    "scoped-input-projection": {
        "reason": (
            "Legacy paid evidence is scientifically unchanged; only the scoped "
            "Task Packet/fingerprint representation changed. Every non-scoped "
            "dependency and selected prompt component was proved identical."
        ),
        "transformed_fields": [
            "fingerprint",
            "fingerprint_payload",
            "provenance",
            "certification",
        ],
    },
    "ecosystem-runner-rubric-v2-snapshot": {
        "reason": (
            "Preserved paid Ecosystem findings/citations were centrally "
            "re-adjudicated under the current runner-owned fixed rubric. Legacy "
            "language-local scores were not copied."
        ),
        "transformed_fields": [
            "requirements",
            "evidence",
            "fingerprint",
            "fingerprint_payload",
            "provenance",
            "certification",
        ],
    },
    "validator-recertification": {
        "reason": (
            "Historical paid result was projected only across explicitly allowed "
            "runner/schema/epoch changes and then passed the complete current "
            "validator before promotion."
        ),
        "transformed_fields": [
            "fingerprint",
            "fingerprint_payload",
            "provenance",
            "certification",
            "current-validator-attestations",
        ],
    },
    "semantic-legacy-llm-recertification": {
        "reason": (
            "Historical paid Semantic Compression evidence was re-adjudicated "
            "against the current frozen rubric by the trusted GPT-5.6 Sol "
            "recertifier. The result is bound to a frozen provenance snapshot "
            "containing the exact legacy source hashes and current canonical "
            "fragments. This mode never claims that legacy fragments were "
            "mechanically compiled when the retained run did not perform that "
            "verification."
        ),
        "transformed_fields": [
            "requirements",
            "evidence",
            "fingerprint",
            "fingerprint_payload",
            "provenance",
            "certification",
            "canonical_fragments",
            "canonical_verification",
        ],
    },
    "semantic-legacy-owner-recertification": {
        "reason": (
            "Preserved paid Semantic Compression support judgments and scientifically "
            "identical raw fragments (same-run preferred) were reconstructed into "
            "the current canonical owner "
            "schema. Exact source bytes are retained and the complete current "
            "validator re-checks the reconstructed catalog."
        ),
        "transformed_fields": [
            "requirements", "evidence", "fingerprint", "fingerprint_payload",
            "provenance", "certification",
        ],
    },
    "semantic-legacy-consumer-recertification": {
        "reason": (
            "A preserved paid Semantic Compression metric shard with the same scientific "
            "experiment identity as the recertified owner was rebound to the current canonical "
            "catalog. Explicit legacy fragments, when present, matched exactly; "
            "the current metric validator re-checks the result."
        ),
        "transformed_fields": [
            "evidence", "fingerprint", "fingerprint_payload", "provenance",
            "certification",
        ],
    },
    "semantic-probe-from-current-owner": {
        "reason": (
            "A current-validator-certified language owner was split into its exact "
            "probe×language leaves without changing the selected canonical record. "
            "The retained source bytes and result digest remain explicit provenance."
        ),
        "transformed_fields": [
            "requirements", "evidence", "fingerprint", "fingerprint_payload",
            "provenance", "certification",
        ],
    },
    "semantic-support-from-current-catalog": {
        "reason": (
            "Legacy support adjudication is derived deterministically from the "
            "already current-validator-approved canonical owner records. No new "
            "model judgment or score is introduced."
        ),
        "transformed_fields": [
            "requirements", "evidence", "fingerprint", "fingerprint_payload",
            "provenance", "certification",
        ],
    },
    "semantic-comparability-from-current-catalog": {
        "reason": (
            "For the legacy bridge, comparability is derived after every current "
            "canonical owner, metric consumer and support-adjudication dependency "
            "has passed the current validator, leaving one support authority."
        ),
        "transformed_fields": [
            "requirements", "evidence", "fingerprint", "fingerprint_payload",
            "provenance", "certification",
        ],
    },
    "micro-measure-input-projection": {
        "reason": (
            "Historical mechanical measurement consumed the same scientific "
            "programs, fixtures, toolchains and measurement script; unrelated "
            "runner-only inputs were projected away and the current validator passed."
        ),
        "transformed_fields": [
            "fingerprint",
            "fingerprint_payload",
            "provenance",
            "certification",
        ],
    },
    "legacy-adversarial-cohort-to-language-shard": {
        "reason": (
            "A certified all-language mechanical adversarial cohort was split into "
            "language shards without changing that language's frozen program, "
            "assets, toolchain, epoch or requirement values."
        ),
        "transformed_fields": [
            "assigned_languages",
            "requirements",
            "evidence",
            "fingerprint",
            "fingerprint_payload",
            "provenance",
            "certification",
        ],
    },
}


def cache_migration_metadata(
    root: Path,
    compatibility_mode: str,
    source_rel: Path,
    source_fingerprint: str,
    evaluation: str,
) -> dict[str, Any]:
    """Create an auditable provenance chain for one compatibility migration.

    The source record/snapshot is never modified or removed. The newly certified
    record points back to its exact bytes and carries the rule that transformed
    it. This is metadata only: it never weakens the scientific cache key.
    """
    rule = CACHE_MIGRATION_RULES.get(compatibility_mode)
    if rule is None:
        raise BenchmarkError(
            f"unknown cache compatibility migration rule: {compatibility_mode}"
        )
    rel = PurePosixPath(source_rel.as_posix())
    if rel.is_absolute() or any(part in {"", ".", ".."} for part in rel.parts):
        raise BenchmarkError(f"invalid migration source record path: {source_rel}")
    source_path = require_under(root / "cache" / Path(*rel.parts), root / "cache")
    if not source_path.is_file():
        raise BenchmarkError(
            f"migration source record is missing: {source_rel.as_posix()}"
        )
    source_hash = sha256_file(source_path)
    source_result_sha256 = None
    try:
        source_json = json_load(source_path)
        if isinstance(source_json, dict):
            source_result_sha256 = source_json.get("result_sha256")
    except (OSError, json.JSONDecodeError, TypeError, ValueError):
        source_result_sha256 = None
    return {
        "schema_version": 1,
        "source_fingerprint": source_fingerprint,
        "source_record": source_rel.as_posix(),
        "source_record_sha256": source_hash,
        **(
            {"source_result_sha256": source_result_sha256}
            if isinstance(source_result_sha256, str)
            else {}
        ),
        "migration_rule": compatibility_mode,
        "migration_rule_version": CACHE_MIGRATION_RULE_VERSION,
        "migration_reason": str(rule["reason"]),
        "transformed_fields": list(rule["transformed_fields"]),
        "current_validator": "PASS",
        "current_mechanical_verification": (
            "not-performed-legacy-evidence-recertification"
            if evaluation == "semantic_compression"
            else (
                "current-validator"
                if evaluation == "language_quality"
                else "not-applicable"
            )
        ),
    }


def recover_current_migration_metadata(
    root: Path, record: dict[str, Any]
) -> dict[str, Any] | None:
    """Ratchet older current-key recertified records to explicit provenance.

    Before migration provenance became first-class, the Ecosystem v2 records
    stored their compatibility rule and snapshot id but not the snapshot byte
    hash. The snapshot is frozen and itself verifies every legacy source record,
    so the direct provenance link can be reconstructed deterministically.
    """
    if isinstance(record.get("migration"), dict):
        return dict(record["migration"])
    certification = record.get("certification") or {}
    mode = str(certification.get("compatibility_migration") or "")
    if mode != "ecosystem-runner-rubric-v2-snapshot":
        return None
    cfg = (
        (cache_policy(root).get("reuse_conditions") or {}).get(
            "ecosystem_snapshot_recertification"
        )
        or {}
    )
    relative = str(cfg.get("path") or "")
    rel = PurePosixPath(relative)
    if (
        not relative
        or rel.is_absolute()
        or any(part in {"", ".", ".."} for part in rel.parts)
    ):
        raise BenchmarkError("Ecosystem snapshot provenance path is invalid")
    path = require_under(root / "cache" / Path(*rel.parts), root / "cache")
    if not path.is_file():
        raise BenchmarkError(
            "Ecosystem snapshot required to reconstruct migration provenance is missing"
        )
    return cache_migration_metadata(
        root,
        mode,
        Path(*rel.parts),
        sha256_file(path),
        str(record.get("evaluation") or "ecosystem"),
    )


def cache_record_migration_problem(
    root: Path, record: dict[str, Any]
) -> str | None:
    """Verify the provenance source of a recertified current cache record."""
    migration = record.get("migration")
    if migration is None:
        return None
    if not isinstance(migration, dict) or migration.get("schema_version") != 1:
        return "migration provenance schema"
    mode = str(migration.get("migration_rule") or "")
    if mode not in CACHE_MIGRATION_RULES:
        return "migration provenance rule"
    if migration.get("migration_rule_version") != CACHE_MIGRATION_RULE_VERSION:
        return "migration provenance rule version"
    if migration.get("current_validator") != "PASS":
        return "migration provenance current-validator attestation"
    transformed = migration.get("transformed_fields")
    if not isinstance(transformed, list) or not transformed:
        return "migration provenance transformed_fields"
    raw = str(migration.get("source_record") or "")
    rel = PurePosixPath(raw)
    if (
        not raw
        or rel.is_absolute()
        or any(part in {"", ".", ".."} for part in rel.parts)
    ):
        return "migration provenance source path"
    source_path = require_under(root / "cache" / Path(*rel.parts), root / "cache")
    if not source_path.is_file():
        return "migration provenance source record missing"
    if sha256_file(source_path) != migration.get("source_record_sha256"):
        return "migration provenance source record hash"
    return None



def _micro_measure_semantic_payload(payload: dict[str, Any]) -> dict[str, Any]:
    """The scientific inputs of the runner-owned micro measurement.

    Historical mechanical keys conservatively included all measurement scripts
    and the whole Primary configuration.  micro-measure never executes
    adversarial_measure.py, and its numerical experiment is fixed by its
    readable programs/fixtures/workloads/validator/scoring inputs, toolchains,
    target identity, requirement set and micro_measure.py itself.  Keeping the
    unrelated adversarial script or orchestration-only Primary fields in this
    compatibility projection would force a multi-hour remeasurement without
    changing the experiment.

    This is intentionally a migration projection, not a weakening of the exact
    cache key.  The projected result still has to pass today's command validator
    before it can be promoted under the current fingerprint.
    """
    normalized = json.loads(json.dumps(payload))
    inputs = dict(normalized.get("unit_input_hashes") or {})
    inputs.pop("primary_config", None)
    # The old planner called the same no-prompt-section digest
    # evaluation_spec; the current planner calls it evaluation_spec_sections.
    # Neither file is read by micro_measure.py.  The executable experiment is
    # already content-bound by the concrete readable inputs and script hash.
    inputs.pop("evaluation_spec", None)
    inputs.pop("evaluation_spec_sections", None)
    normalized["unit_input_hashes"] = inputs
    scripts = normalized.get("measurement_script_hashes") or {}
    normalized["measurement_script_hashes"] = {
        "micro_measure.py": scripts.get("micro_measure.py")
    }
    return normalized


def find_micro_measure_projection_cache_record(
    root: Path,
    unit: dict[str, Any],
    current_payload: dict[str, Any],
) -> tuple[Path | None, dict[str, Any] | None, str | None]:
    """Recover a legacy micro measurement across runner-only key changes.

    Only the all-language runner-owned micro unit is eligible.  Every input
    actually consumed by that measurement remains exact; only unrelated
    adversarial-script and orchestration metadata differences are projected
    away.  A changed micro script, program, workload, validator/scoring input,
    toolchain, target version/implementation, requirement set or epoch cannot
    match this bridge.
    """
    if (
        str(unit.get("evaluation") or "") != "language_quality"
        or str(unit.get("runner_action") or "") != "micro-measure"
        or unit.get("execution_kind") != "command"
        or not mechanical_unit(unit)
    ):
        return None, None, None

    primary = json_load(root / "template" / "config" / "primary.json")
    program_root = str(
        (primary.get("language_quality") or {}).get("quidra_program_root") or ""
    )
    if program_root != "tests/benchmark/quidra":
        # The historical record below explicitly read this snapshot path.
        return None, None, None

    directory = (
        root / "cache" / "v1" / "language-quality" / "mechanical-micro-measure"
    )
    if not directory.is_dir():
        return None, None, None

    expected = _micro_measure_semantic_payload(current_payload)
    matches: list[tuple[str, str, Path, dict[str, Any]]] = []
    invalid_candidates: list[str] = []
    for path in sorted(directory.glob("*.json")):
        try:
            record = json_load(path)
        except (OSError, json.JSONDecodeError, TypeError, ValueError):
            continue
        old = record.get("fingerprint_payload") or {}
        if (
            str(old.get("work_unit_id") or "") != str(unit.get("id") or "")
            or str(old.get("evaluation") or "") != "language_quality"
            or str(old.get("runner_action") or "") != "micro-measure"
        ):
            continue
        if _micro_measure_semantic_payload(old) != expected:
            continue
        problem = _cache_record_self_integrity_problem(record)
        if problem:
            invalid_candidates.append(f"{path.name}: {problem}")
            continue
        run_id = str((record.get("provenance") or {}).get("run_id") or "")
        matches.append((run_id, path.name, path, record))

    if not matches:
        if invalid_candidates:
            return (
                None,
                None,
                "all micro-measure projection candidates failed self-integrity: "
                + "; ".join(invalid_candidates[:8]),
            )
        return None, None, None

    # Never choose by the measured score.  Prefer the newest run identity and
    # stable path tie-break when the same experiment was measured repeatedly.
    _, _, path, record = sorted(matches)[-1]
    source_fingerprint = str(record.get("fingerprint") or "")
    current_fingerprint = sha256_bytes(
        json.dumps(
            current_payload,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=False,
        ).encode("utf-8")
    )
    projected = {
        **record,
        "fingerprint": current_fingerprint,
        "fingerprint_payload": current_payload,
        "evaluation": "language_quality",
        "assigned_languages": list(unit.get("assigned_languages", [])),
        "certification": {
            **(record.get("certification") or {}),
            "mechanical_action_projection": "micro-measure-v1",
        },
        "provenance": {
            **(record.get("provenance") or {}),
            "projection_source_fingerprint": source_fingerprint,
            "work_unit_id": unit.get("id"),
        },
    }
    return path, projected, None


def find_adversarial_language_projection_cache_record(
    root: Path,
    unit: dict[str, Any],
    current_payload: dict[str, Any],
) -> tuple[Path | None, dict[str, Any] | None, str | None]:
    """Project an explicitly approved legacy cohort result to one comparison shard."""
    if (
        str(unit.get("runner_action") or "") != "adversarial-measure"
        or unit.get("execution_kind") != "command"
    ):
        return None, None, None
    assigned = [str(x) for x in (unit.get("assigned_languages") or [])]
    target = str(cache_policy(root).get("target_language") or "Quidra")
    if len(assigned) != 1 or assigned[0] == target:
        return None, None, None
    language = assigned[0]
    migration = (
        (cache_policy(root).get("reuse_conditions") or {}).get(
            "adversarial_language_shard_migration"
        )
        or {}
    )
    allowed = {
        str(value)
        for value in (migration.get("legacy_cohort_fingerprints") or [])
    }
    if not allowed:
        return None, None, None
    directory = (
        root / "cache" / "v1" / "language-quality"
        / "mechanical-adversarial-measure"
    )
    if not directory.is_dir():
        return None, None, None

    current_hashes = current_payload.get("unit_input_hashes") or {}
    current_reads = current_payload.get("readable_input_content_hashes") or {}
    broad_programs = cache_read_input_hashes(
        root, {"read_paths": [str(root / "template" / "programs")]}
    ).get("template/programs")
    matches: list[tuple[Path, dict[str, Any]]] = []
    for fingerprint in sorted(allowed):
        path = directory / f"{fingerprint}.json"
        if not path.is_file():
            continue
        try:
            record = json_load(path)
        except (OSError, json.JSONDecodeError, TypeError, ValueError) as exc:
            return None, None, (
                f"approved adversarial migration record is unreadable: "
                f"{path.name}: {exc}"
            )
        problem = _cache_record_self_integrity_problem(record)
        if problem:
            return None, None, (
                f"approved adversarial migration record failed self-integrity: "
                f"{path.name}: {problem}"
            )
        old = record.get("fingerprint_payload") or {}
        if (
            record.get("fingerprint") != fingerprint
            or old.get("work_unit_id") != "lq-adversarial-mechanical"
            or old.get("evaluation") != "language_quality"
            or old.get("runner_action") != "adversarial-measure"
            or old.get("cache_epoch") != current_payload.get("cache_epoch")
            or old.get("requirement_ids") != current_payload.get("requirement_ids")
            or old.get("network_allowed") != current_payload.get("network_allowed")
            or old.get("worker_mode") != current_payload.get("worker_mode")
        ):
            continue
        old_hashes = old.get("unit_input_hashes") or {}
        old_eval_hash = (
            old_hashes.get("evaluation_spec_sections")
            or old_hashes.get("evaluation_spec")
        )
        current_eval_hash = (
            current_hashes.get("evaluation_spec_sections")
            or current_hashes.get("evaluation_spec")
        )
        if (
            old_hashes.get("benchmark_metadata")
            != current_hashes.get("benchmark_metadata")
            or old_eval_hash != current_eval_hash
        ):
            continue

        old_reads = old.get("readable_input_content_hashes") or {}
        if old_reads.get("template/programs") != broad_programs:
            continue
        comparable_reads = {
            key: value for key, value in old_reads.items()
            if key != "template/programs"
        }
        if any(
            current_reads.get(key) != value
            for key, value in comparable_reads.items()
        ):
            continue
        if (old.get("toolchains") or {}).get(language) != (
            current_payload.get("toolchains") or {}
        ).get(language):
            continue
        if (old.get("runtime_toolchain_pins") or {}).get(language) != (
            current_payload.get("runtime_toolchain_pins") or {}
        ).get(language):
            continue

        old_result = record.get("result")
        old_requirements = (
            old_result.get("requirements")
            if isinstance(old_result, dict)
            else None
        )
        if not isinstance(old_requirements, dict):
            continue
        projected_requirements: dict[str, Any] = {}
        valid = True
        for rid in current_payload.get("requirement_ids") or []:
            values = old_requirements.get(rid)
            if not isinstance(values, dict) or language not in values:
                valid = False
                break
            projected_requirements[str(rid)] = {language: values[language]}
        if not valid:
            continue

        projected_result = {
            "schema_version": 1,
            "evaluation": "language_quality",
            "requirements": projected_requirements,
            "evidence": {
                "certified_projection": {
                    "mode": "legacy-adversarial-cohort-to-language-shard",
                    "source_fingerprint": fingerprint,
                    "source_run_id": (record.get("provenance") or {}).get("run_id"),
                    "language": language,
                    "reason": (
                        "same frozen comparison program/assets/toolchain; only "
                        "the mechanical orchestration unit was split"
                    ),
                }
            },
        }
        projected_raw = json.dumps(
            projected_result, sort_keys=True, separators=(",", ":")
        ).encode("utf-8")
        projected = {
            **record,
            "fingerprint": sha256_bytes(
                json.dumps(
                    current_payload,
                    sort_keys=True,
                    separators=(",", ":"),
                    ensure_ascii=False,
                ).encode("utf-8")
            ),
            "fingerprint_payload": current_payload,
            "assigned_languages": [language],
            "result": projected_result,
            "result_sha256": sha256_bytes(projected_raw),
            "certification": {
                **(record.get("certification") or {}),
                "projection_migration": (
                    "legacy-adversarial-cohort-to-language-shard"
                ),
                "projected_language": language,
            },
            "provenance": {
                **(record.get("provenance") or {}),
                "projection_source_fingerprint": fingerprint,
                "work_unit_id": unit.get("id"),
            },
        }
        matches.append((path, projected))

    if len(matches) > 1 and len({
        str(record.get("result_sha256") or "") for _, record in matches
    }) != 1:
        return None, None, (
            "multiple approved adversarial cohort records disagree on the "
            f"projected {language} result"
        )
    if not matches:
        return None, None, None
    return matches[-1][0], matches[-1][1], None


def find_primary_projection_compatible_cache_record(
    root: Path,
    unit: dict[str, Any],
    task: dict[str, Any],
    current_payload: dict[str, Any],
) -> tuple[Path | None, dict[str, Any] | None, str | None]:
    directory = (
        root
        / "cache"
        / "v1"
        / slug_id(str(unit.get("evaluation") or "unknown"))
        / cache_scope(unit)
    )
    if not directory.is_dir():
        return None, None, None
    matches: list[tuple[Path, dict[str, Any]]] = []
    invalid_candidates: list[str] = []
    for path in sorted(directory.glob("*.json")):
        try:
            record = json_load(path)
        except (OSError, json.JSONDecodeError, TypeError, ValueError):
            # A corrupt unrelated record in the same language/scope directory
            # must not abort discovery for this unit. Exact-key corruption is
            # handled by hydrate_certified_cache, where it can be attributed
            # unambiguously to the current leaf.
            continue
        payload = record.get("fingerprint_payload") or {}
        if str(payload.get("work_unit_id") or "") != str(unit.get("id") or ""):
            continue
        if not _legacy_primary_prompt_compatible(root, record, current_payload, task):
            continue
        problem = _cache_record_self_integrity_problem(record)
        if problem:
            invalid_candidates.append(f"{path.name}: {problem}")
            continue
        matches.append((path, record))
    if not matches:
        if invalid_candidates:
            return (
                None,
                None,
                "all primary-projection-compatible records failed self-integrity: "
                + "; ".join(invalid_candidates[:8]),
            )
        return None, None, None
    result_hashes = {str(record.get("result_sha256") or "") for _, record in matches}
    if len(result_hashes) != 1:
        return (
            None,
            None,
            "multiple primary-projection-compatible records disagree on result",
        )
    return matches[-1][0], matches[-1][1], None


def cache_fingerprint(root: Path, unit: dict[str, Any], task: dict[str, Any]) -> tuple[str, dict[str, Any]] | None:
    payload = cache_fingerprint_payload(root, unit, task)
    if payload is None:
        return None
    raw = json.dumps(
        payload, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    return sha256_bytes(raw), payload


def cache_record_relative(unit: dict[str, Any], fingerprint: str) -> Path:
    return (
        Path("v1")
        / slug_id(str(unit.get("evaluation") or "unknown"))
        / cache_scope(unit)
        / f"{fingerprint}.json"
    )



def _cache_status(root: Path) -> dict[str, Any]:
    path = root / "results" / "cache_status.json"
    if path.is_file():
        status = json_load(path)
        status.setdefault("hits", {})
        status.setdefault("misses", {})
        status.setdefault("invalidated", {})
        return status
    return {
        "schema_version": 1,
        "enabled": bool((json_load(root / "run.json").get("inference_identity") or {}).get("model")),
        "hits": {},
        "misses": {},
        "invalidated": {},
    }



def _write_cache_status(root: Path, status: dict[str, Any]) -> None:
    status.setdefault("hits", {})
    status.setdefault("misses", {})
    status.setdefault("invalidated", {})
    status["hit_count"] = len(status["hits"])
    status["miss_count"] = len(status["misses"])
    status["invalidated_count"] = len(status["invalidated"])
    json_dump(root / "results" / "cache_status.json", status)



def hydrate_certified_cache(
    root: Path, evaluation: str | None = None, *, mechanical_only: bool = False
) -> int:
    """Complete every PENDING cacheable unit that has a certified record.

    A cache defect is always leaf-local. Missing, corrupt, stale or newly
    validator-incompatible records are recorded as MISS/INVALIDATED decisions
    and only that work unit is re-executed; no bad cache entry may abort the
    rest of the benchmark.

    With `mechanical_only`, only the mechanical measurement units are
    considered; `cmd_advance` calls it that way before it runs any command
    unit, so a certified micro, adversarial or audit measurement is reused
    instead of being measured again. Agent units are hydrated after their
    tasks exist.
    """
    manifest = json_load(root / "work" / "root" / "manifest.json")
    ledger = json_load(root / "work" / "root" / "ledger.json")
    status = _cache_status(root)
    hits = 0

    def record_miss(
        uid: str,
        fingerprint: str,
        unit: dict[str, Any],
        reason: str,
        *,
        invalidated: bool,
        record_path: str | None = None,
    ) -> None:
        status["hits"].pop(uid, None)
        row = {
            "fingerprint": fingerprint,
            "scope": cache_scope(unit),
            "reason": reason,
        }
        if record_path:
            row["record"] = record_path
        status["misses"][uid] = row
        if invalidated:
            status["invalidated"][uid] = dict(row)
        else:
            status["invalidated"].pop(uid, None)

    for unit in manifest.get("work_units", []):
        if evaluation is not None and unit.get("evaluation") != evaluation:
            continue
        uid = str(unit["id"])
        state = ledger.get("units", {}).get(uid, {})
        if state.get("status", "PENDING") != "PENDING":
            continue
        if not cache_eligible_unit(root, unit):
            continue
        if not all(
            ledger.get("units", {}).get(dep, {}).get("status") == "COMPLETE"
            for dep in unit.get("dependencies", [])
        ):
            continue
        mechanical = mechanical_unit(unit)
        if mechanical_only and not mechanical:
            continue
        if mechanical:
            agent_dir = mechanical_result_path(root, unit).parent
            task = mechanical_task(unit)
        else:
            agent_dir = root / "work" / "agents" / str(unit["assigned_agent_id"])
            task_path = agent_dir / "task.json"
            if not task_path.is_file():
                continue
            task = json_load(task_path)
        pair = cache_fingerprint(root, unit, task)
        if pair is None:
            continue
        fingerprint, payload = pair
        rel = cache_record_relative(unit, fingerprint)
        cache_path = root / "cache" / rel
        compatibility_mode = None
        source_fingerprint = fingerprint
        source_rel = rel
        if cache_path.is_file():
            try:
                record = json_load(cache_path)
            except (OSError, json.JSONDecodeError, TypeError, ValueError) as exc:
                record_miss(
                    uid,
                    fingerprint,
                    unit,
                    f"certified record is unreadable/corrupt: {type(exc).__name__}: {exc}",
                    invalidated=True,
                    record_path=rel.as_posix(),
                )
                continue
            problem = _cache_record_self_integrity_problem(record)
            if problem:
                record_miss(
                    uid,
                    fingerprint,
                    unit,
                    f"certified record failed self-integrity: {problem}",
                    invalidated=True,
                    record_path=rel.as_posix(),
                )
                continue
            if record.get("fingerprint") != fingerprint:
                record_miss(
                    uid,
                    fingerprint,
                    unit,
                    "certified record fingerprint does not match its exact cache path",
                    invalidated=True,
                    record_path=rel.as_posix(),
                )
                continue
            if record.get("fingerprint_payload") != payload:
                record_miss(
                    uid,
                    fingerprint,
                    unit,
                    "certified record dependency fingerprint payload no longer matches",
                    invalidated=True,
                    record_path=rel.as_posix(),
                )
                continue
        else:
            compatible_path, record, compatibility_problem = (
                find_primary_projection_compatible_cache_record(
                    root, unit, task, payload
                )
            )
            if compatibility_problem:
                record_miss(
                    uid,
                    fingerprint,
                    unit,
                    compatibility_problem,
                    invalidated=True,
                )
                continue
            if compatible_path is not None and record is not None:
                compatibility_mode = "scoped-input-projection"
            else:
                compatible_path, record, ecosystem_problem = (
                    ecosystem_snapshot_recertification_record(
                        root, unit, task, payload
                    )
                )
                if ecosystem_problem:
                    record_miss(
                        uid,
                        fingerprint,
                        unit,
                        ecosystem_problem,
                        invalidated=True,
                    )
                    continue
                if compatible_path is not None and record is not None:
                    compatibility_mode = "ecosystem-runner-rubric-v2-snapshot"
                else:
                    (
                        compatible_path,
                        record,
                        semantic_problem,
                        semantic_mode,
                    ) = semantic_derived_recertification_record(
                        root, unit, payload
                    )
                    if semantic_problem:
                        record_miss(
                            uid,
                            fingerprint,
                            unit,
                            semantic_problem,
                            invalidated=True,
                        )
                        continue
                    if compatible_path is not None and record is not None:
                        compatibility_mode = semantic_mode
                    else:
                        compatible_path, record, recertification_problem = (
                            find_validator_recertifiable_cache_record(
                                root, unit, payload, task
                            )
                        )
                        if recertification_problem:
                            record_miss(
                                uid,
                                fingerprint,
                                unit,
                                recertification_problem,
                                invalidated=True,
                            )
                            continue
                        if compatible_path is not None and record is not None:
                            certification = record.get("certification") or {}
                            if certification.get("semantic_legacy_owner_recertified"):
                                compatibility_mode = "semantic-legacy-llm-recertification"
                            elif certification.get("semantic_legacy_consumer_recertified"):
                                compatibility_mode = "semantic-legacy-consumer-recertification"
                            else:
                                compatibility_mode = "validator-recertification"
                        else:
                            compatible_path, record, mechanical_problem = (
                                find_micro_measure_projection_cache_record(
                                    root, unit, payload
                                )
                            )
                            if mechanical_problem:
                                record_miss(
                                    uid,
                                    fingerprint,
                                    unit,
                                    mechanical_problem,
                                    invalidated=True,
                                )
                                continue
                            if compatible_path is not None and record is not None:
                                compatibility_mode = "micro-measure-input-projection"
                            else:
                                compatible_path, record, projection_problem = (
                                    find_adversarial_language_projection_cache_record(
                                        root, unit, payload
                                    )
                                )
                                if projection_problem:
                                    record_miss(
                                        uid,
                                        fingerprint,
                                        unit,
                                        projection_problem,
                                        invalidated=True,
                                    )
                                    continue
                                if compatible_path is not None and record is not None:
                                    compatibility_mode = (
                                        "legacy-adversarial-cohort-to-language-shard"
                                    )
            if compatible_path is None or record is None:
                record_miss(
                    uid,
                    fingerprint,
                    unit,
                    "no certified record",
                    invalidated=False,
                )
                continue
            source_fingerprint = str(
                (record.get("provenance") or {}).get(
                    "projection_source_fingerprint",
                    record.get("fingerprint") or "",
                )
            )
            source_rel = compatible_path.relative_to(root / "cache")
        migration_problem = cache_record_migration_problem(root, record)
        if migration_problem:
            record_miss(
                uid,
                fingerprint,
                unit,
                f"certified record failed migration provenance: {migration_problem}",
                invalidated=True,
                record_path=source_rel.as_posix(),
            )
            continue
        cap_problem = cache_cap_reuse_problem(root, record, unit)
        if cap_problem:
            record_miss(
                uid,
                fingerprint,
                unit,
                cap_problem,
                invalidated=True,
                record_path=source_rel.as_posix(),
            )
            continue
        target_problem = cache_quidra_execution_reuse_problem(root, record)
        if target_problem:
            record_miss(
                uid,
                fingerprint,
                unit,
                target_problem,
                invalidated=True,
                record_path=source_rel.as_posix(),
            )
            continue
        result_path = agent_dir / "result.json"
        agent_dir.mkdir(parents=True, exist_ok=True)
        migration_metadata = recover_current_migration_metadata(root, record)
        json_dump(result_path, record["result"])
        materialized_legacy_attestation = (
            None
            if mechanical
            else materialize_legacy_semantic_owner_runner_attestation(
                root, unit, record["result"]
            )
        )
        json_dump(agent_dir / "cache_receipt.json", {
            "schema_version": 1,
            "status": "HIT",
            "fingerprint": fingerprint,
            "record": str(source_rel.as_posix()),
            "certification": record.get("certification") or {},
            "fingerprint_payload": payload,
            **({"migration": migration_metadata} if migration_metadata else {}),
            **(
                {
                    "compatibility_mode": compatibility_mode,
                    "source_fingerprint": source_fingerprint,
                }
                if compatibility_mode
                else {}
            ),
        })
        validation_problem = None
        try:
            if mechanical:
                check_rc = cmd_command_result_check(
                    argparse.Namespace(workspace=str(root), id=uid)
                )
            else:
                check_rc = cmd_result_check(
                    argparse.Namespace(
                        workspace=str(root), id=unit["assigned_agent_id"]
                    )
                )
        except (BenchmarkError, OSError, ValueError, KeyError) as exc:
            check_rc = 2
            validation_problem = str(exc)
        if check_rc != 0:
            # Structurally intact cache may still be stale under a newer
            # validator. Treat that as a leaf-local invalidation and execute
            # only this unit again.
            try:
                result_path.unlink()
            except FileNotFoundError:
                pass
            receipt_path = agent_dir / "cache_receipt.json"
            try:
                receipt_path.unlink()
            except FileNotFoundError:
                pass
            if materialized_legacy_attestation is not None:
                try:
                    materialized_legacy_attestation.unlink()
                except FileNotFoundError:
                    pass
            record_miss(
                uid,
                fingerprint,
                unit,
                (
                    "certified record rejected by current validator"
                    + (f": {validation_problem}" if validation_problem else "")
                ),
                invalidated=True,
                record_path=source_rel.as_posix(),
            )
            continue
        if compatibility_mode is not None:
            # The receipt is created before validation so a failed projection can
            # be deleted atomically. Only after today's validator passes do we
            # certify the migration as current-valid for checkpoint promotion.
            receipt_path = agent_dir / "cache_receipt.json"
            receipt = json_load(receipt_path)
            certification = dict(receipt.get("certification") or {})
            certification["validator_pass"] = True
            certification["compatibility_migration"] = compatibility_mode
            certification["current_validator_revalidated"] = True
            receipt["certification"] = certification
            migration_metadata = cache_migration_metadata(
                root,
                compatibility_mode,
                source_rel,
                source_fingerprint,
                str(unit.get("evaluation") or ""),
            )
            receipt["migration"] = migration_metadata
            json_dump(receipt_path, receipt)
        cmd_ledger_update(argparse.Namespace(
            workspace=str(root), id=uid, status="RUNNING", evidence=[],
            validation_result=None, blocker=None, blocker_class=None,
        ))
        cmd_ledger_update(argparse.Namespace(
            workspace=str(root), id=uid, status="COMPLETE",
            evidence=unit.get("evidence_paths", []), validation_result="PASS",
            blocker=None, blocker_class=None,
        ))
        status["hits"][uid] = {
            "fingerprint": fingerprint,
            "scope": cache_scope(unit),
            "record": source_rel.as_posix(),
            "assigned_languages": list(unit.get("assigned_languages", [])),
            **(
                {
                    "compatibility_mode": compatibility_mode,
                    "source_fingerprint": source_fingerprint,
                }
                if compatibility_mode
                else {}
            ),
            **({"migration": migration_metadata} if migration_metadata else {}),
        }
        status["misses"].pop(uid, None)
        status["invalidated"].pop(uid, None)
        hits += 1
    _write_cache_status(root, status)
    return hits


def worker_isolation_config(root: Path) -> dict[str, Any]:
    cfg = json_load(root / "template" / "config" / "primary.json")
    worker = cfg.get("worker_isolation", {})
    if not isinstance(worker, dict):
        raise BenchmarkError("primary worker_isolation config must be an object")
    return worker


def collect_packet_only_inputs(
    root: Path, read_paths: list[str]
) -> tuple[list[dict[str, Any]], list[tuple[str, str]]]:
    cfg = worker_isolation_config(root)
    max_files = int(cfg.get("packet_only_max_embedded_files", 256))
    max_bytes = int(cfg.get("packet_only_max_embedded_bytes", 1048576))
    files: list[dict[str, Any]] = []
    sections: list[tuple[str, str]] = []
    seen: set[str] = set()
    total_bytes = 0
    for raw in read_paths:
        source = require_under(Path(raw), root)
        if source.is_symlink():
            raise BenchmarkError(f"packet-only input may not be a symlink: {source}")
        if source.is_file():
            candidates = [source]
        elif source.is_dir():
            candidates = sorted(
                path for path in source.rglob("*")
                if path.is_file() and not path.is_symlink()
            )
        else:
            raise BenchmarkError(f"packet-only input is missing: {source}")
        for path in candidates:
            path = require_under(path, root)
            relative = path.relative_to(root).as_posix()
            if relative in seen:
                continue
            data = path.read_bytes()
            if b"\x00" in data:
                raise BenchmarkError(
                    f"packet-only input is binary and cannot be embedded: {path}"
                )
            try:
                content = data.decode("utf-8")
            except UnicodeDecodeError as exc:
                raise BenchmarkError(
                    f"packet-only input is not UTF-8 text: {path}"
                ) from exc
            seen.add(relative)
            total_bytes += len(data)
            if len(seen) > max_files:
                raise BenchmarkError(
                    f"packet-only input file count exceeds frozen limit {max_files}"
                )
            if total_bytes > max_bytes:
                raise BenchmarkError(
                    f"packet-only input bytes exceed frozen limit {max_bytes}; "
                    "narrow the Task Packet or use sandbox-agent mode"
                )
            digest = sha256_bytes(data)
            canonical = (CANONICAL_WORKSPACE / relative).as_posix()
            files.append({
                "path": canonical,
                "sha256": digest,
                "bytes": len(data),
            })
            sections.append((
                f"task-input:{relative}",
                "\n\n---\n\n"
                f"## Embedded task input: {canonical}\n"
                f"Source SHA-256: `{digest}`\n\n"
                + content
            ))
    return files, sections



SC_TEXT_SECTION_NAMES = (
    "LEVEL", "FRAGMENT", "PARTIAL_REASONS", "NONE_REASON", "JUSTIFICATION", "CITATION",
)


def parse_sc_support_text(content: str, *, language: str) -> dict[str, Any]:
    """Parse one human/LLM-authored language.txt into a support record.

    The model never authors benchmark JSON for cohort support adjudication.
    Section labels are deliberately trivial text delimiters; the trusted runner
    owns JSON keys, nesting, language identity, requirement identity and escaping.
    """
    normalized = content.replace("\r\n", "\n").replace("\r", "\n")
    sections: dict[str, list[str]] = {}
    current: str | None = None
    for raw_line in normalized.split("\n"):
        label = raw_line.strip().upper()
        if label in SC_TEXT_SECTION_NAMES:
            current = label
            if current in sections:
                raise BenchmarkError(
                    f"{language}.txt repeats section {current}"
                )
            sections[current] = []
            continue
        if current is None:
            if raw_line.strip():
                raise BenchmarkError(
                    f"{language}.txt has text before the first section label"
                )
            continue
        sections[current].append(raw_line)
    missing = [name for name in SC_TEXT_SECTION_NAMES if name not in sections]
    if missing:
        raise BenchmarkError(
            f"{language}.txt is missing sections: {', '.join(missing)}"
        )

    def value(name: str) -> str:
        return "\n".join(sections[name]).strip()

    level = value("LEVEL").upper()
    if level not in {"FULL", "PARTIAL", "NONE"}:
        raise BenchmarkError(
            f"{language}.txt LEVEL must be FULL, PARTIAL or NONE"
        )
    fragment_raw = value("FRAGMENT")
    fragment = None if fragment_raw in {"", "-"} else fragment_raw
    partial_raw = value("PARTIAL_REASONS")
    partial = [] if partial_raw in {"", "-"} else [
        item.strip() for item in partial_raw.split(",") if item.strip()
    ]
    none_raw = value("NONE_REASON")
    none_reason = None if none_raw in {"", "-"} else none_raw
    record = {
        "level": level,
        "fragment": fragment,
        "partial_reasons": partial,
        "none_reason": none_reason,
        "justification": value("JUSTIFICATION"),
        "citation": value("CITATION"),
    }
    normalized_record = sc_adjudicated_record(record)
    if normalized_record is None:
        raise BenchmarkError(
            f"{language}.txt does not form a complete support adjudication record"
        )
    return normalized_record


def compile_sc_support_text_outputs(
    root: Path,
    meta: dict[str, Any],
    staged: list[tuple[Path, bytes, str]],
) -> tuple[list[tuple[Path, bytes, str]], set[str]]:
    """Compile per-language text leaves into runner-owned result.json."""
    requirement_ids = [str(value) for value in (meta.get("requirement_ids") or [])]
    probe_id = support_adjudication_probe(requirement_ids)
    if probe_id is None:
        return staged, {rel for _, _, rel in staged}
    support_ids = [
        rid for rid in requirement_ids if rid.startswith(SUPPORT_ADJUDICATION_PREFIX)
    ]
    if len(support_ids) != 1:
        raise BenchmarkError(
            "support adjudication text compiler requires exactly one support requirement"
        )
    expected_languages = metadata_languages(root)
    by_path = {rel: data for _, data, rel in staged}
    expected_text = {f"{language}.txt": language for language in expected_languages}
    unexpected = sorted(
        rel for rel in by_path
        if rel != "result.json" and rel not in expected_text
    )
    if unexpected:
        raise BenchmarkError(
            "support adjudication worker may return only language.txt leaves: "
            + ", ".join(unexpected)
        )
    # result.json is runner-owned for this task. Refuse a model-authored copy
    # rather than deciding which source is authoritative.
    if "result.json" in by_path:
        raise BenchmarkError(
            "support adjudication result.json is runner-owned; return only language.txt files"
        )
    missing = sorted(set(expected_text) - set(by_path))
    if missing:
        raise BenchmarkError(
            "support adjudication omitted language text files: " + ", ".join(missing)
        )
    records: dict[str, Any] = {}
    for filename, language in expected_text.items():
        try:
            content = by_path[filename].decode("utf-8")
        except UnicodeDecodeError as exc:
            raise BenchmarkError(f"{filename} is not UTF-8 text") from exc
        record = parse_sc_support_text(content, language=language)
        validate_sc_record_for_probe(
            root, probe_id, record, context=f"{support_ids[0]}: {language}"
        )
        records[language] = record

    result = {
        "schema_version": 1,
        "evaluation": "semantic_compression",
        "requirements": {support_ids[0]: records},
        "evidence": {
            "source_format": "runner-compiled-language-text-v1",
            "probe_id": probe_id,
            "source_files": list(expected_text),
        },
    }
    data = (
        json.dumps(result, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    ).encode("utf-8")
    agent_dir = require_under(
        root / "work" / "agents" / str(meta.get("id") or ""), root
    )
    compiled = (agent_dir / "result.json", data, "result.json")
    return [*staged, compiled], {rel for _, _, rel in [*staged, compiled]}


def apply_worker_response(
    root: Path, agent_id: str, raw: bytes, extra: dict[str, Any] | None = None
) -> dict[str, Any]:
    """Validate and materialize one packet-only Worker Response.

    Shared by `task-apply` (response piped in from the outer runner) and
    `task-infer` (response obtained through the credential-less gateway), so both
    paths get identical traversal, size, duplicate and expected-output checks.
    """
    agent_dir = require_under(root / "work" / "agents" / agent_id, root)
    meta = json_load(agent_dir / "task.json")
    if meta.get("worker_mode") != "packet-only":
        raise BenchmarkError("task-apply is only valid for packet-only workers")
    cfg = worker_isolation_config(root)
    max_bytes = int(cfg.get("packet_only_max_response_bytes", 2097152))
    max_files = int(cfg.get("packet_only_max_response_files", 64))
    if len(raw) > max_bytes:
        raise BenchmarkError(
            f"packet-only worker response exceeds frozen limit {max_bytes}"
        )
    try:
        response = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise BenchmarkError(
            f"packet-only worker response is not valid UTF-8 JSON: {exc}"
        ) from exc
    if not isinstance(response, dict):
        raise BenchmarkError("packet-only worker response must be a JSON object")
    if response.get("schema_version") != 1:
        raise BenchmarkError("packet-only worker response schema_version must be 1")
    if response.get("task_id") != agent_id:
        raise BenchmarkError("packet-only worker response task_id mismatch")
    output_files = response.get("files")
    if not isinstance(output_files, list) or not output_files:
        raise BenchmarkError(
            "packet-only worker response files must be a non-empty array"
        )
    if len(output_files) > max_files:
        raise BenchmarkError(
            f"packet-only worker response file count exceeds frozen limit {max_files}"
        )
    seen: set[str] = set()
    staged: list[tuple[Path, bytes, str]] = []
    total_output_bytes = 0
    for entry in output_files:
        if not isinstance(entry, dict):
            raise BenchmarkError(
                "packet-only worker response file entries must be objects"
            )
        rel_text = str(entry.get("path") or "")
        # The packet names the expected output by its canonical absolute path, so
        # a model that echoes what it was told to produce sends that form. Accept
        # it only when it lies inside this agent's own directory, and reduce it to
        # the relative form every other check operates on. Nothing outside stays
        # rejected, so the traversal surface is unchanged.
        for base in (
            (CANONICAL_WORKSPACE / "work" / "agents" / agent_id).as_posix() + "/",
            agent_dir.as_posix() + "/",
            f"work/agents/{agent_id}/",
        ):
            if rel_text.startswith(base):
                rel_text = rel_text[len(base):]
                break
        rel = PurePosixPath(rel_text)
        if (
            not rel_text
            or rel.is_absolute()
            or rel_text in {"task.json", "validation.json", "worker_response.json"}
            or any(part in {"", ".", ".."} for part in rel.parts)
        ):
            raise BenchmarkError(f"invalid packet-only output path: {rel_text!r}")
        if rel_text in seen:
            raise BenchmarkError(f"duplicate packet-only output path: {rel_text}")
        content = entry.get("content")
        if content is None and "json" in entry:
            # A JSON document may be returned as the object itself. Escaping a
            # ten-kilobyte result.json into a JSON string is the single most
            # common way a worker response arrived unparseable; the object form
            # cannot be corrupted that way, and it is serialized here verbatim.
            content = json.dumps(entry["json"], indent=2, sort_keys=True, ensure_ascii=False) + "\n"
        if not isinstance(content, str):
            raise BenchmarkError(
                f"packet-only output content must be UTF-8 text: {rel_text}"
            )
        if rel.suffix == ".json":
            try:
                json.loads(content)
            except json.JSONDecodeError as exc:
                raise BenchmarkError(
                    f"packet-only output {rel_text} is not valid JSON ({exc}); "
                    "return the document as an object under \"json\" instead of an "
                    "escaped string"
                ) from exc
        data = content.encode("utf-8")
        total_output_bytes += len(data)
        if total_output_bytes > max_bytes:
            raise BenchmarkError(
                f"packet-only output bytes exceed frozen limit {max_bytes}"
            )
        dest = require_under(agent_dir.joinpath(*rel.parts), agent_dir)
        if dest.exists():
            try:
                existing = dest.read_bytes()
            except OSError as exc:
                raise BenchmarkError(
                    f"packet-only output exists but cannot be verified: {dest}: {exc}"
                ) from exc
            if existing != data:
                raise BenchmarkError(
                    f"packet-only output already exists with different bytes: {dest}"
                )
        seen.add(rel_text)
        staged.append((dest, data, rel_text))
    staged, seen = compile_sc_support_text_outputs(root, meta, staged)
    # For support-adjudication tasks result.json is intentionally runner-owned:
    # the packet's historical expected output is satisfied by the deterministic
    # text compiler above, while the model returns only the ten language leaves.
    # This keeps paid semantic judgment separate from serialization/bookkeeping.
    expected_relative = []
    for expected in meta.get("expected_outputs", []):
        expected_path = require_under(Path(expected), agent_dir)
        expected_relative.append(
            expected_path.relative_to(agent_dir).as_posix()
        )
    missing_expected = sorted(set(expected_relative) - seen)
    if missing_expected:
        raise BenchmarkError(
            "packet-only worker response omitted expected outputs: "
            + ", ".join(missing_expected)
        )
    receipt_files = []
    for dest, data, rel_text in staged:
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists():
            dest.write_bytes(data)
        receipt_files.append({
            "path": rel_text,
            "sha256": sha256_bytes(data),
            "bytes": len(data),
        })
    receipt = {
        "schema_version": 1,
        "task_id": agent_id,
        "prompt_sha256": meta.get("prompt_sha256"),
        "response_sha256": sha256_bytes(raw),
        "files": receipt_files,
        "applied_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        **(extra or {}),
    }
    json_dump(agent_dir / "worker_response.json", receipt)
    return receipt



def _packet_paid_response_records(agent_dir: Path) -> list[Path]:
    directory = agent_dir / "paid_responses"
    if not directory.is_dir():
        return []
    return sorted(
        (path for path in directory.glob("*.json") if path.is_file()),
        key=lambda path: (path.stat().st_mtime_ns, path.name),
        reverse=True,
    )


def persist_packet_paid_response(
    agent_dir: Path,
    task: dict[str, Any],
    response: dict[str, Any],
) -> Path:
    """Durably commit a paid packet-only response before parsing or validation.

    This is the packet-only equivalent of the sandbox-agent trial call journal:
    once the provider has charged for a successful reply, a later parser,
    importer, validator or process failure must not erase the bytes that were
    bought.
    """
    encoded = json.dumps(
        response, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    digest = sha256_bytes(encoded)
    destination = agent_dir / "paid_responses" / f"{digest}.json"
    if destination.is_file():
        existing = json_load(destination)
        if existing.get("response_sha256") != digest:
            raise BenchmarkError(
                "persisted packet-only paid response path has conflicting content"
            )
        return destination
    json_dump(destination, {
        "schema_version": 1,
        "task_id": task.get("id"),
        "prompt_sha256": task.get("prompt_sha256"),
        "response_sha256": digest,
        "response": response,
        "persisted_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
    })
    return destination


def replay_packet_paid_response(
    root: Path,
    agent_id: str,
    meta: dict[str, Any],
    *,
    sampling: dict[str, Any],
) -> dict[str, Any] | None:
    """Re-apply a previously paid response before making another provider call.

    Only an exact prompt match is considered. Incomplete/unparseable historical
    replies remain evidence but are not treated as successful work. A response
    that was charged, persisted, and then stranded by a crash between inference
    and task-apply can therefore be recovered at zero additional API cost.
    """
    agent_dir = require_under(root / "work" / "agents" / agent_id, root)
    client_module = gateway_client_module()
    for path in _packet_paid_response_records(agent_dir):
        try:
            record = json_load(path)
        except (OSError, json.JSONDecodeError, TypeError, ValueError):
            continue
        if record.get("prompt_sha256") != meta.get("prompt_sha256"):
            continue
        response = record.get("response")
        if not isinstance(response, dict):
            continue
        completion = response.get("content")
        if not isinstance(completion, str):
            continue
        if client_module.completion_problem(response):
            continue
        try:
            worker_response = client_module.parse_model_json(completion)
            raw = json.dumps(worker_response, sort_keys=True).encode("utf-8")
            receipt = apply_worker_response(root, agent_id, raw, extra={
                "inference": {
                    "replayed_paid_response": True,
                    "network_allowed": bool(meta.get("network_allowed")),
                    "sampling": sampling,
                    "effective_decoding": response.get("decoding"),
                    "usage": response.get("usage", {}),
                    "completion_sha256": sha256_bytes(completion.encode("utf-8")),
                    "paid_response_record": path.relative_to(agent_dir).as_posix(),
                },
            })
        except (BenchmarkError, client_module.GatewayClientError, OSError, ValueError):
            continue
        return receipt
    return None



def cmd_task_infer(args: argparse.Namespace) -> int:
    """Render, infer through the credential-less gateway, and apply - in one step.

    Before spending money, replay any exact-prompt paid response that was
    durably committed but never successfully applied. Every newly paid response
    is itself persisted before completeness checks, JSON parsing or output
    materialization, so downstream failures cannot turn a successful provider
    call into an unrecoverable charge.
    """
    root = workspace(args)
    agent_dir = require_under(root / "work" / "agents" / args.id, root)
    meta = json_load(agent_dir / "task.json")
    if meta.get("worker_mode") != "packet-only":
        raise BenchmarkError(
            "task-infer is only valid for packet-only workers; sandbox-agent units "
            "run through scripts/sandbox_agent.py inside the sandbox"
        )
    packet = render_prompt_components(
        meta["prompt_components"], meta["prompt_sha256"]
    ).decode("utf-8")

    config = gateway_config(root)
    sampling = sampling_config(root, str(meta.get("evaluation") or ""))

    # Replay is only for a paid response stranded before the current runner
    # had a chance to judge it. Once a validator has rejected an attempt, the
    # same bytes are known-bad under the current contract; repeating them would
    # deadlock retries instead of asking the model to repair its answer.
    feedback = previous_attempt_feedback(root, args.id)
    replayed = None
    if not feedback:
        replayed = replay_packet_paid_response(
            root, args.id, meta, sampling=sampling
        )
    if replayed is not None:
        print(json.dumps({
            "ok": True,
            "reused_paid_response": True,
            **replayed,
        }, indent=2))
        return 0

    client_module = gateway_client_module()
    socket_path = args.socket or str(gateway_socket_path(root, config))
    client = client_module.InferenceGatewayClient(socket_path, timeout=float(args.timeout))
    try:
        health = client.health()
        if health.get("host_tools_exposed"):
            raise BenchmarkError(
                "inference gateway reports an exposed host tool surface; refusing to "
                "dispatch scored work through it"
            )
        system_text = (
            "You are a packet-only benchmark worker. You have no local "
            "filesystem, shell, process, editor or host-application tools. "
            "Every permitted input is embedded in the Task Packet below. "
            "Reply with exactly one JSON Worker Response object and nothing "
            "else. A file entry may carry a JSON document as an object under "
            "\"json\" instead of an escaped string under \"content\"; use that "
            "form for result.json so the document cannot be corrupted by string "
            "escaping."
        )
        if meta.get("network_allowed"):
            searches = int(
                (config.get("anthropic_web_search") or {}).get("max_uses_per_request", 0) or 0
            )
            if searches:
                system_text += (
                    f" Web retrieval is available and limited to {searches} searches "
                    "for this whole request; when they are spent, finish from the "
                    "evidence you already have. Never request any other tool."
                )
        messages = [
            {"role": "system", "content": system_text},
            {"role": "user", "content": packet},
        ]
        if feedback:
            messages.append({
                "role": "user",
                "content": (
                    "Your previous attempt at this exact Task Packet was rejected: "
                    f"{feedback}\n\nThe packet above is unchanged. Return a corrected "
                    "Worker Response now."
                ),
            })
        response = client.complete(
            messages,
            task_id=args.id,
            max_output_tokens=int(args.max_output_tokens),
            network_allowed=bool(meta.get("network_allowed")),
            purpose="scored",
        )
    except client_module.GatewayRefusal as exc:
        raise BenchmarkError(f"inference gateway refused this Task Packet: {exc}") from exc
    except client_module.GatewayClientError as exc:
        raise BenchmarkError(f"inference transport failure: {exc}") from exc

    if not isinstance(response, dict):
        raise BenchmarkError("inference gateway returned a non-object response")
    paid_record = persist_packet_paid_response(agent_dir, meta, response)
    completion = response.get("content")
    if not isinstance(completion, str):
        raise BenchmarkError(
            "packet-only paid response has no text content; preserved for audit/recovery"
        )
    incomplete = client_module.completion_problem(response)
    if incomplete:
        raise BenchmarkError(
            f"packet-only worker response is incomplete: {incomplete}; "
            f"paid response preserved at {paid_record.relative_to(agent_dir)}"
        )
    try:
        worker_response = client_module.parse_model_json(completion)
    except client_module.GatewayClientError as exc:
        raise BenchmarkError(
            "packet-only worker response is unusable but its paid bytes were preserved: "
            f"{exc}"
        ) from exc

    raw = json.dumps(worker_response, sort_keys=True).encode("utf-8")
    receipt = apply_worker_response(root, args.id, raw, extra={
        "inference": {
            "gateway": health.get("gateway"),
            "provider": health.get("provider", {}).get("id"),
            "network_allowed": bool(meta.get("network_allowed")),
            "sampling": sampling,
            "effective_decoding": response.get("decoding"),
            "usage": response.get("usage", {}),
            "completion_sha256": sha256_bytes(completion.encode("utf-8")),
            "paid_response_record": paid_record.relative_to(agent_dir).as_posix(),
        },
    })
    print(json.dumps({"ok": True, **receipt}, indent=2))
    return 0


def cmd_task_apply(args: argparse.Namespace) -> int:
    root = workspace(args)
    cfg = worker_isolation_config(root)
    max_bytes = int(cfg.get("packet_only_max_response_bytes", 2097152))
    raw = sys.stdin.buffer.read(max_bytes + 1)
    receipt = apply_worker_response(root, args.id, raw)
    print(json.dumps({"ok": True, **receipt}, indent=2))
    return 0


def cmd_task_render(args: argparse.Namespace) -> int:
    root = workspace(args)
    agent_dir = require_under(root / "work" / "agents" / args.id, root)
    meta = json_load(agent_dir / "task.json")
    data = render_prompt_components(meta["prompt_components"], meta["prompt_sha256"])
    sys.stdout.buffer.write(data)
    return 0


def cmd_task_create(args: argparse.Namespace) -> int:
    root = workspace(args)
    run = json_load(root / "run.json")
    integrity = template_integrity_problems(root, run)
    if integrity:
        raise BenchmarkError("template integrity failed: " + ", ".join(integrity))
    agent_dir = require_under(root / "work" / "agents" / args.id, root)
    if agent_dir.exists() and any(agent_dir.iterdir()):
        raise BenchmarkError(f"agent directory already initialized: {agent_dir}")
    agent_dir.mkdir(parents=True, exist_ok=True)

    reads = normalize_paths(args.read or [], root)
    reject_overbroad_read_paths(reads, root)
    write = normalize_paths([args.write or str(agent_dir)], root)[0]
    outputs = normalize_paths(args.output or [], root)
    validator_argv(args.validate, root)
    if not Path(write).is_relative_to(agent_dir):
        raise BenchmarkError("task write path must be inside the agent's own directory")

    depth = int(args.depth)
    if depth < 0 or depth > 3:
        raise BenchmarkError("delegation depth must be between 0 and 3")

    embedded_inputs: list[dict[str, str]] = []
    embedded_sections: list[tuple[str, str]] = []
    requirement_ids = list(getattr(args, "requirement_id", []) or [])
    prompt_sections = list(getattr(args, "section", []) or [])
    assigned_languages = list(getattr(args, "language", []) or [])
    worker_mode = str(getattr(args, "worker_mode", None) or "packet-only")
    if worker_mode not in {"packet-only", "sandbox-agent"}:
        raise BenchmarkError("worker_mode must be packet-only or sandbox-agent")
    packet_inputs: list[dict[str, Any]] = []
    packet_input_sections: list[tuple[str, str]] = []
    if worker_mode == "packet-only":
        packet_inputs, packet_input_sections = collect_packet_only_inputs(root, reads)
    if args.evaluation:
        methodology = root / "template" / "methodology"
        core_path = methodology / "worker_core.md"
        eval_path = methodology / EVALUATION_SPEC_FILES[args.evaluation]
        config_path = root / "template" / "config" / "primary.json"
        metadata_path = root / "template" / BENCHMARK_METADATA_RELATIVE
        for required_path in (core_path, eval_path, config_path, metadata_path):
            if not required_path.is_file():
                raise BenchmarkError(f"required Task Packet input is missing: {required_path}")

        core_content = render_workspace_paths(core_path.read_text(encoding="utf-8"), root)
        eval_source = eval_path.read_text(encoding="utf-8")
        eval_projection = extract_markdown_sections(eval_source, prompt_sections)
        eval_content = render_workspace_paths(eval_projection, root)
        eval_digest = sha256_bytes(eval_projection.encode("utf-8"))
        config_content = render_workspace_paths(
            primary_config_projection_text(root, args.evaluation), root
        )
        config_digest = primary_config_projection_sha256(root, args.evaluation)
        metadata_content = render_workspace_paths(
            metadata_path.read_text(encoding="utf-8"), root
        )

        for name, source_path, content, selected_digest in (
            ("worker_core.md", core_path, core_content, None),
            (eval_path.name, eval_path, eval_content, eval_digest),
            ("primary.json", config_path, config_content, config_digest),
            ("benchmark_metadata.json", metadata_path, metadata_content, None),
        ):
            digest = selected_digest or sha256_file(source_path)
            embedded_inputs.append({
                "path": str(source_path),
                "sha256": digest,
                **({"selection": args.evaluation} if name == "primary.json" else {}),
            })
            embedded_sections.append((
                name,
                "\n\n---\n\n"
                f"## Embedded input: {name}\n"
                f"Source SHA-256: `{digest}`\n\n"
                + content
            ))

        requirements_path, all_requirement_ids = load_evaluation_requirements(root, args.evaluation)
        ownership = execution_ownership_for(
            root, args.evaluation, all_requirement_ids
        )
        allowed_agent_prefixes = tuple(ownership.get("agent_prefixes") or ())
        unknown = sorted(
            rid for rid in set(requirement_ids) - set(all_requirement_ids)
            if not any(
                str(rid).startswith(prefix) for prefix in allowed_agent_prefixes
            )
        )
        if unknown:
            raise BenchmarkError(
                f"Task Packet names unknown requirement IDs: {', '.join(unknown)}"
            )
        requirements_content = assigned_requirements_projection_text(
            args.evaluation, requirement_ids
        )
        requirements_source_sha = assigned_requirements_projection_sha256(
            args.evaluation, requirement_ids
        )
        embedded_inputs.append({
            "path": str(requirements_path),
            "sha256": requirements_source_sha,
            "selection": args.evaluation,
        })
        embedded_sections.append((
            "assigned_requirements.json",
            "\n\n---\n\n"
            "## Embedded input: assigned_requirements.json\n"
            f"Source SHA-256: `{requirements_source_sha}`\n\n"
            + requirements_content
        ))

    if worker_mode == "packet-only":
        support_text_mode = support_adjudication_probe(requirement_ids) is not None
        if support_text_mode:
            language_files = ", ".join(f"{language}.txt" for language in metadata_languages(root))
            output_instruction = f"""- Do NOT author result.json. The trusted runner owns all benchmark JSON.
- Return exactly these per-language text leaves: {language_files}
- Each language.txt must use these section labels, each on its own line and in this order:
  LEVEL
  <FULL|PARTIAL|NONE>
  FRAGMENT
  <exact fragment, or ->
  PARTIAL_REASONS
  <comma-separated P-a..P-e, or ->
  NONE_REASON
  <N-1..N-4, or ->
  JUSTIFICATION
  <plain UTF-8 text; may span lines>
  CITATION
  <plain UTF-8 text; may span lines>
- The runner derives the language from the filename, validates every value, and mechanically compiles result.json."""
            response_example = (
                '{"schema_version":1,"task_id":"' + args.id
                + '","files":[{"path":"Quidra.txt","content":"LEVEL\\nFULL\\n..."}]}'
            )
        else:
            output_instruction = "- Return result.json as the expected task output."
            response_example = (
                '{"schema_version":1,"task_id":"' + args.id
                + '","files":[{"path":"result.json","content":"<UTF-8 text>"}]}'
            )
        isolation_block = f"""- Worker mode: packet-only
- Local filesystem, shell, process, editor, IDE, and host-application tools: forbidden
- All permitted local source inputs are embedded in this packet.
- Provider-level network retrieval: {'allowed' if args.network else 'disabled'}
- Return exactly one JSON Worker Response envelope; do not write files directly.
{output_instruction}
- Worker Response transport schema: {response_example}"""
    else:
        isolation_block = """- Worker mode: sandbox-agent
- The tool-capable agent process itself must run inside the attested /quidra-benchmark sandbox.
- Host-side Read/Glob/Bash/editor/process tools are forbidden for the leaf.
- Do not mount or forward host credentials, SSH agent sockets, or host home directories."""

    packet = f"""# Task Packet: {args.id}

Evaluation: {args.evaluation or 'non-primary-support-task'}

Parent: {args.parent or 'ROOT'}
Goal: {args.goal}

## Assigned requirement IDs
""" + ("\n".join(f"- {rid}" for rid in requirement_ids) if requirement_ids else "- reusable-artifact audit/support task") + f"""

## Assigned languages
""" + ("\n".join(f"- {language}" for language in assigned_languages) if assigned_languages else "- full fixed comparison set") + f"""

## Readable paths
""" + "\n".join(f"- {p}" for p in reads) + f"""

## Writable path
- {write}

## Expected outputs
""" + ("\n".join(f"- {p}" for p in outputs) if outputs else "- defined by this task") + f"""

## Validation
`{args.validate}`

## Execution permissions
- Network: {'allowed' if args.network else 'disabled'}
- Further delegation depth remaining: {depth}

## Worker isolation
""" + isolation_block + f"""

## Rules
- This packet plus its embedded inputs (packet-only) or listed sandbox paths (sandbox-agent) is the complete task context.
- Do not depend on the parent conversation or hidden context.
- In packet-only mode, do not invoke local filesystem/shell/process/editor/application tools; return files only through the Worker Response JSON.
- In sandbox-agent mode, do not write outside the writable path and do not access paths outside the listed readable paths.
- Do not read sibling agent outputs unless explicitly listed above.
- Preserve machine-readable evidence required by the methodology.
- Use commands for mechanical work when a reusable command exists.
- If delegating, create a new self-contained Task Packet under the same policy.
- Do not mark the task COMPLETE unless the validation command succeeds.
- For a primary-evaluation task, the compact worker rules, selected methodology sections, assigned requirement IDs, and frozen primary configuration embedded below are authoritative.
- Do not read historical benchmark run directories.
- Packet-only workers return result.json through the Worker Response; sandbox-agent workers write result.json in the writable directory.
- A child agent must receive its own persisted self-contained Task Packet. Do not pass implicit parent conversation state.
"""
    packet = render_workspace_paths(packet, root)
    layout = str(getattr(args, "layout", None) or "task-first")
    if layout not in PACKET_LAYOUTS:
        raise BenchmarkError(f"unsupported packet layout: {layout}")
    task_component = store_prompt_component(root, packet, "task")
    embedded_components = [
        (name, store_prompt_component(root, section, f"embedded:{name}"))
        for name, section in embedded_sections
    ]
    input_components = [
        store_prompt_component(root, packet_input, name)
        for name, packet_input in packet_input_sections
    ]
    if layout == "shared-inputs-first":
        # Preserve the historical rendered Task Packet byte order because
        # prompt_sha256 is part of every certified result-cache fingerprint.
        # The provider cache breakpoint is a transport concern: the trusted
        # gateway splits the unchanged text before the first unit-specific
        # embedded task input (or, when none exists, before the Task Packet
        # header). Reordering components here would needlessly invalidate
        # already-paid certified records even though their semantic inputs did
        # not change.
        shared = [c for name, c in embedded_components if name != "assigned_requirements.json"]
        tail = [c for name, c in embedded_components if name == "assigned_requirements.json"]
        components = [*shared, *input_components, task_component, *tail]
    else:
        components = [task_component, *(c for _, c in embedded_components), *input_components]
    rendered = b"".join(Path(c["path"]).read_bytes() for c in components)
    prompt_hash = sha256_bytes(rendered)

    meta = {
        "schema_version": 1,
        "id": args.id,
        "parent": args.parent,
        "goal": args.goal,
        "evaluation": args.evaluation,
        "assigned_languages": assigned_languages,
        "worker_mode": worker_mode,
        "embedded_inputs": embedded_inputs,
        "packet_inputs": packet_inputs,
        "read_paths": reads,
        "write_path": write,
        "expected_outputs": outputs,
        "validation_command": args.validate,
        "network_allowed": bool(args.network),
        "delegation_depth_remaining": depth,
        "requirement_ids": requirement_ids,
        "prompt_sections": prompt_sections,
        "prompt_sha256": prompt_hash,
        "prompt_components": components,
        "packet_layout": layout,
        "rendered_bytes": len(rendered),
        "canonical_fragment_owner": bool(
            getattr(args, "canonical_fragment_owner", False)
        ),
        "canonical_probe_id": getattr(args, "canonical_probe_id", None),
        "canonical_fragment_source_requirement": getattr(
            args, "canonical_fragment_source_requirement", None
        ),
        "canonical_fragment_catalog_sha256": getattr(
            args, "canonical_fragment_catalog_sha256", None
        ),
    }
    prompt_manifest = root / "prompts" / "manifests" / f"{args.id}.json"
    meta["prompt_manifest_path"] = str(prompt_manifest)
    json_dump(prompt_manifest, {
        "schema_version": 1,
        "agent_id": args.id,
        "evaluation": args.evaluation,
        "prompt_sha256": prompt_hash,
        "rendered_bytes": len(rendered),
        "components": components,
    })
    json_dump(agent_dir / "task.json", meta)
    print(json.dumps(meta, indent=2))
    return 0


def cmd_prompt_save(args: argparse.Namespace) -> int:
    root = workspace(args)
    source = Path(args.file)
    if not source.is_absolute():
        source = root / source
    source = require_under(source, root)
    if not source.is_file():
        raise BenchmarkError(f"prompt file is missing: {source}")
    data = source.read_bytes()
    digest = sha256_bytes(data)
    suffix = source.suffix if source.suffix in {".md", ".txt", ".json"} else ".txt"
    dest = root / "prompts" / "by-hash" / f"{digest}{suffix}"
    if not dest.exists():
        dest.write_bytes(data)
    result = {
        "sha256": digest,
        "stored_path": str(dest),
        "source_path": str(source),
        "bytes": len(data),
    }
    print(json.dumps(result, indent=2))
    return 0


def cmd_task_validate(args: argparse.Namespace) -> int:
    root = workspace(args)
    agent_dir = require_under(root / "work" / "agents" / args.id, root)
    meta = json_load(agent_dir / "task.json")
    render_prompt_components(meta["prompt_components"], meta["prompt_sha256"])

    argv = validator_argv(str(meta["validation_command"]), root)
    p = subprocess.run(
        argv,
        shell=False,
        cwd=agent_dir,
        env=sanitized_subprocess_env(root, agent_dir),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    validation = {
        "exit_code": p.returncode,
        "stdout": p.stdout,
        "stderr": p.stderr,
        "validated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
    }
    json_dump(agent_dir / "validation.json", validation)
    print(json.dumps(validation, indent=2))
    return 0 if p.returncode == 0 else 2




def manifest_unit_by_id(root: Path, work_unit_id: str) -> dict[str, Any]:
    manifest = json_load(root / "work" / "root" / "manifest.json")
    for unit in manifest.get("work_units", []):
        if str(unit.get("id")) == work_unit_id:
            return unit
    raise BenchmarkError(f"unknown work unit: {work_unit_id}")


def score_or_na(value: Any) -> float | None:
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        score = float(value)
        if not (0.0 <= score <= 100.0):
            raise BenchmarkError(f"normalized score outside 0..100: {value}")
        return score
    if isinstance(value, dict) and value.get("status") == "N/A":
        if not str(value.get("reason") or "").strip():
            raise BenchmarkError("N/A score requires a reason")
        return None
    raise BenchmarkError(f"invalid normalized score value: {value!r}")


def ecosystem_rubric_asset(root: Path) -> dict[str, Any]:
    """Load and structurally validate the frozen runner-owned Ecosystem rubric."""
    path = (
        root / "template" / "methodology-assets" / "ecosystem" / "rubrics.json"
    )
    if not path.is_file():
        raise BenchmarkError("frozen Ecosystem rubric asset is missing")
    data = json_load(path)
    if data.get("schema_version") != 1 or data.get("frozen") is not True:
        raise BenchmarkError("Ecosystem rubric asset must be frozen schema_version 1")

    _, required = load_evaluation_requirements(root, "ecosystem")
    expected_metrics = {rid for rid in required if rid.startswith("metric.")}
    metrics = data.get("metrics")
    if not isinstance(metrics, dict) or set(metrics) != expected_metrics:
        raise BenchmarkError(
            "Ecosystem rubric metrics must exactly match evaluation requirements"
        )

    scoring = data.get("scoring")
    if not isinstance(scoring, dict) or scoring.get("runner_owned") is not True:
        raise BenchmarkError("Ecosystem scoring must be runner-owned")
    levels = scoring.get("allowed_levels")
    points = scoring.get("level_points")
    if levels != [0, 1, 2, 3, 4] or not isinstance(points, dict):
        raise BenchmarkError("Ecosystem rubric must freeze levels 0..4")
    expected_points = {"0": 0, "1": 5, "2": 10, "3": 15, "4": 20}
    if points != expected_points:
        raise BenchmarkError("Ecosystem level-to-points mapping must be 0/5/10/15/20")
    if int(scoring.get("component_count_per_metric", 0) or 0) != 5:
        raise BenchmarkError("Ecosystem metrics must have exactly five components")
    if int(scoring.get("component_weight_points", 0) or 0) != 20:
        raise BenchmarkError("Ecosystem components must carry 20 points each")

    rubric_ids: set[str] = set()
    for rid, row in metrics.items():
        if not isinstance(row, dict):
            raise BenchmarkError(f"{rid}: Ecosystem rubric row must be an object")
        rubric_id = row.get("rubric_id")
        if not isinstance(rubric_id, str) or not rubric_id:
            raise BenchmarkError(f"{rid}: Ecosystem rubric_id is missing")
        if rubric_id in rubric_ids:
            raise BenchmarkError(f"{rid}: duplicate Ecosystem rubric_id {rubric_id}")
        rubric_ids.add(rubric_id)
        if not isinstance(row.get("selection_rule"), str) or not row["selection_rule"].strip():
            raise BenchmarkError(f"{rid}: Ecosystem selection_rule is missing")
        components = row.get("components")
        if not isinstance(components, list) or len(components) != 5:
            raise BenchmarkError(f"{rid}: Ecosystem rubric needs five components")
        component_ids: list[str] = []
        for component in components:
            if not isinstance(component, dict):
                raise BenchmarkError(f"{rid}: Ecosystem component must be an object")
            cid = component.get("id")
            criterion = component.get("criterion")
            weight = component.get("weight_points")
            if not isinstance(cid, str) or not cid:
                raise BenchmarkError(f"{rid}: Ecosystem component id is missing")
            if not isinstance(criterion, str) or not criterion.strip():
                raise BenchmarkError(f"{rid}/{cid}: criterion is missing")
            if weight != 20:
                raise BenchmarkError(f"{rid}/{cid}: weight_points must be 20")
            component_ids.append(cid)
        if len(set(component_ids)) != 5:
            raise BenchmarkError(f"{rid}: Ecosystem component IDs must be unique")

    policy = data.get("evidence_policy")
    if not isinstance(policy, dict):
        raise BenchmarkError("Ecosystem evidence_policy is missing")
    required_policy = {
        "snapshot_basis", "snapshot_date", "activity_window_months",
        "provider_search_budget_per_worker", "source_priority",
        "symmetry_rule", "not_executed_rule", "popularity_rule",
    }
    if not required_policy <= set(policy):
        missing = sorted(required_policy - set(policy))
        raise BenchmarkError(
            "Ecosystem evidence_policy is incomplete: " + ", ".join(missing)
        )
    snapshot_date = policy.get("snapshot_date")
    if not isinstance(snapshot_date, str) or not re.fullmatch(
        r"\d{4}-\d{2}-\d{2}", snapshot_date
    ):
        raise BenchmarkError("Ecosystem snapshot_date must be YYYY-MM-DD")
    ecosystem_epoch = cache_epoch(root, "ecosystem")
    epoch_month = ecosystem_epoch[:7]
    if (
        re.fullmatch(r"\d{4}-\d{2}", epoch_month)
        and not snapshot_date.startswith(epoch_month + "-")
    ):
        raise BenchmarkError(
            f"Ecosystem snapshot date {snapshot_date} is outside the declared "
            f"epoch month {epoch_month}; advance both together"
        )
    if int(policy.get("activity_window_months", 0) or 0) <= 0:
        raise BenchmarkError("Ecosystem activity window must be positive")
    frozen_search_budget = int(
        policy.get("provider_search_budget_per_worker", 0) or 0
    )
    gateway = json_load(root / "template" / "config" / "inference_gateway.json")
    gateway_search_budget = int(
        ((gateway.get("anthropic_web_search") or {}).get("max_uses_per_request", 0))
        or 0
    )
    if frozen_search_budget <= 0 or frozen_search_budget != gateway_search_budget:
        raise BenchmarkError(
            "Ecosystem provider search budget must equal the frozen gateway "
            "max_uses_per_request"
        )
    if not isinstance(policy.get("source_priority"), list) or not policy["source_priority"]:
        raise BenchmarkError("Ecosystem source priority must be non-empty")
    return data


def apply_ecosystem_runner_scores(
    root: Path, task: dict[str, Any], result: dict[str, Any]
) -> None:
    """Validate LLM evidence and replace Ecosystem scores with runner arithmetic."""
    if task.get("evaluation") != "ecosystem":
        return
    requirement_ids = [
        str(rid) for rid in (task.get("requirement_ids") or [])
        if str(rid).startswith("metric.")
    ]
    if not requirement_ids:
        return
    assigned = list(task.get("assigned_languages") or [])
    if len(assigned) != 1:
        raise BenchmarkError(
            "Ecosystem metric workers must currently be one-language evidence shards"
        )
    language = str(assigned[0])
    asset = ecosystem_rubric_asset(root)
    points = asset["scoring"]["level_points"]
    frozen_snapshot_date = str(
        (asset.get("evidence_policy") or {}).get("snapshot_date") or ""
    )
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", frozen_snapshot_date):
        raise BenchmarkError(
            "Ecosystem rubric asset must freeze evidence_policy.snapshot_date"
        )
    ecosystem_epoch = cache_epoch(root, "ecosystem")
    epoch_month = ecosystem_epoch[:7]
    if (
        re.fullmatch(r"\d{4}-\d{2}", epoch_month)
        and not frozen_snapshot_date.startswith(epoch_month + "-")
    ):
        raise BenchmarkError(
            f"Ecosystem snapshot date {frozen_snapshot_date} is outside the "
            f"declared epoch month {epoch_month}; advance both together"
        )
    evidence = result.get("evidence")
    if not isinstance(evidence, dict):
        raise BenchmarkError("Ecosystem result requires evidence object")

    computed: dict[str, dict[str, float]] = {}
    runner_detail: dict[str, Any] = {}
    for rid in requirement_ids:
        row = evidence.get(rid)
        if not isinstance(row, dict):
            raise BenchmarkError(f"{rid}: Ecosystem evidence object is missing")
        rubric = asset["metrics"][rid]
        if row.get("rubric_id") != rubric["rubric_id"]:
            raise BenchmarkError(
                f"{rid}: rubric_id must equal frozen {rubric['rubric_id']}"
            )
        component_ids = [str(c["id"]) for c in rubric["components"]]
        levels = row.get("component_levels")
        findings = row.get("component_findings")
        if not isinstance(levels, dict) or set(levels) != set(component_ids):
            raise BenchmarkError(
                f"{rid}: component_levels must exactly match frozen component IDs"
            )
        if not isinstance(findings, dict) or set(findings) != set(component_ids):
            raise BenchmarkError(
                f"{rid}: component_findings must exactly match frozen component IDs"
            )
        score = 0
        normalized_levels: dict[str, int] = {}
        for cid in component_ids:
            level = levels[cid]
            if isinstance(level, bool) or not isinstance(level, int) or level not in {0,1,2,3,4}:
                raise BenchmarkError(f"{rid}/{cid}: component level must be integer 0..4")
            finding = findings[cid]
            if not isinstance(finding, str) or not finding.strip():
                raise BenchmarkError(f"{rid}/{cid}: component finding is empty")
            normalized_levels[cid] = level
            score += int(points[str(level)])
        sources = row.get("sources")
        if (
            not isinstance(sources, list)
            or not sources
            or not all(isinstance(src, str) and src.strip() for src in sources)
        ):
            raise BenchmarkError(f"{rid}: sources must be a non-empty string array")
        candidate_universe = row.get("candidate_universe")
        if not isinstance(candidate_universe, str) or not candidate_universe.strip():
            raise BenchmarkError(f"{rid}: candidate_universe must be non-empty")
        if row.get("selection_rule") != rubric["selection_rule"]:
            raise BenchmarkError(f"{rid}: selection_rule differs from frozen rubric")
        if row.get("retrieval_route") != "provider-brokered web search":
            raise BenchmarkError(
                f"{rid}: retrieval_route must be provider-brokered web search"
            )
        snapshot_date = row.get("snapshot_date")
        if not isinstance(snapshot_date, str) or not re.fullmatch(
            r"\d{4}-\d{2}-\d{2}", snapshot_date
        ):
            raise BenchmarkError(f"{rid}: snapshot_date must be YYYY-MM-DD")
        if snapshot_date != frozen_snapshot_date:
            raise BenchmarkError(
                f"{rid}: snapshot_date {snapshot_date} must equal the frozen "
                f"Ecosystem snapshot date {frozen_snapshot_date}"
            )
        limitations = row.get("limitations")
        if not isinstance(limitations, str):
            raise BenchmarkError(f"{rid}: limitations must be a string")
        row["runner_score_0_100"] = score
        row["component_levels"] = normalized_levels
        computed[rid] = {language: float(score)}
        runner_detail[rid] = {
            "rubric_id": rubric["rubric_id"],
            "component_levels": normalized_levels,
            "score_0_100": score,
        }

    # Worker arithmetic is deliberately non-authoritative.
    result["requirements"] = computed
    evidence["runner_scoring"] = {
        "rubric_set_id": asset.get("rubric_set_id"),
        "language": language,
        "metrics": runner_detail,
    }


LANGUAGE_QUALITY_DESIGN_RUBRICS_RELATIVE = Path(
    "template/methodology-assets/language_quality/design_rubrics.json"
)


def language_quality_design_rubric_asset(root: Path) -> dict[str, Any]:
    """Load the frozen intrinsic Language Quality design rubric contract."""
    path = root / LANGUAGE_QUALITY_DESIGN_RUBRICS_RELATIVE
    if not path.is_file():
        raise BenchmarkError("frozen Language Quality design rubric asset is missing")
    data = json_load(path)
    if data.get("schema_version") != 1 or data.get("frozen") is not True:
        raise BenchmarkError(
            "Language Quality design rubric asset must be frozen schema_version 1"
        )

    aggregation = json_load(root / "template" / "config" / "aggregation.json")
    expected = set(
        (
            aggregation.get("evaluations", {})
            .get("language_quality", {})
            .get("categories", {})
            .get("language_development", {})
            .get("metrics", [])
        )
    )
    metrics = data.get("metrics")
    if not isinstance(metrics, dict) or set(metrics) != expected:
        raise BenchmarkError(
            "Language Quality design rubrics must exactly match the "
            "language_development aggregation metric set"
        )

    scoring = data.get("scoring")
    expected_points = {"0": 0, "1": 5, "2": 10, "3": 15, "4": 20}
    if (
        not isinstance(scoring, dict)
        or scoring.get("runner_owned") is not True
        or scoring.get("allowed_levels") != [0, 1, 2, 3, 4]
        or scoring.get("level_points") != expected_points
        or int(scoring.get("component_count_per_metric", 0) or 0) != 5
        or int(scoring.get("component_weight_points", 0) or 0) != 20
    ):
        raise BenchmarkError(
            "Language Quality design rubric scoring must freeze five 20-point "
            "components with levels 0..4 and runner-owned arithmetic"
        )

    rubric_ids: set[str] = set()
    for rid, row in metrics.items():
        if not isinstance(row, dict):
            raise BenchmarkError(f"{rid}: Language Quality rubric row must be an object")
        rubric_id = row.get("rubric_id")
        selection_rule = row.get("selection_rule")
        components = row.get("components")
        if not isinstance(rubric_id, str) or not rubric_id:
            raise BenchmarkError(f"{rid}: Language Quality rubric_id is missing")
        if rubric_id in rubric_ids:
            raise BenchmarkError(f"{rid}: duplicate Language Quality rubric_id {rubric_id}")
        rubric_ids.add(rubric_id)
        if not isinstance(selection_rule, str) or not selection_rule.strip():
            raise BenchmarkError(f"{rid}: Language Quality selection_rule is missing")
        if not isinstance(components, list) or len(components) != 5:
            raise BenchmarkError(f"{rid}: Language Quality rubric needs five components")
        component_ids: list[str] = []
        for component in components:
            if not isinstance(component, dict):
                raise BenchmarkError(f"{rid}: Language Quality component must be an object")
            cid = component.get("id")
            criterion = component.get("criterion")
            if (
                not isinstance(cid, str)
                or not cid
                or not isinstance(criterion, str)
                or not criterion.strip()
                or component.get("weight_points") != 20
            ):
                raise BenchmarkError(
                    f"{rid}: each Language Quality component needs id, criterion "
                    "and weight_points=20"
                )
            component_ids.append(cid)
        if len(set(component_ids)) != 5:
            raise BenchmarkError(f"{rid}: Language Quality component IDs must be unique")

    policy = data.get("evidence_policy")
    if not isinstance(policy, dict):
        raise BenchmarkError("Language Quality design evidence_policy is missing")
    required_policy = {
        "network_allowed", "source_priority", "symmetry_rule",
        "source_size_boundary", "diagnostics_boundary", "na_rule",
    }
    if not required_policy <= set(policy):
        missing = sorted(required_policy - set(policy))
        raise BenchmarkError(
            "Language Quality design evidence_policy is incomplete: "
            + ", ".join(missing)
        )
    if policy.get("network_allowed") is not False:
        raise BenchmarkError("Language Quality design evidence must remain offline")
    if not isinstance(policy.get("source_priority"), list) or not policy["source_priority"]:
        raise BenchmarkError("Language Quality design source_priority must be non-empty")
    return data


def _language_quality_design_evidence_ref(root: Path, raw: Any) -> str:
    """Validate one worker citation as an existing frozen workspace input."""
    if not isinstance(raw, str) or not raw.strip():
        raise BenchmarkError("Language Quality evidence_refs must be non-empty strings")
    text = raw.strip()
    path = PurePosixPath(text)
    if (
        path.is_absolute()
        or any(part in {"", ".", ".."} for part in path.parts)
        or not path.parts
        or path.parts[0] not in {"repo", "template"}
    ):
        raise BenchmarkError(
            f"Language Quality evidence ref must be a relative repo/... or "
            f"template/... path: {text!r}"
        )
    candidate = require_under(root.joinpath(*path.parts), root)
    if not candidate.exists():
        raise BenchmarkError(f"Language Quality evidence ref does not exist: {text}")
    return path.as_posix()


def apply_language_quality_design_runner_scores(
    root: Path, task: dict[str, Any], result: dict[str, Any]
) -> None:
    """Validate fixed design judgments and own their 0-100 arithmetic in runner."""
    if task.get("evaluation") != "language_quality":
        return
    asset = language_quality_design_rubric_asset(root)
    design_metrics = asset["metrics"]
    requirement_ids = [
        str(rid)
        for rid in (task.get("requirement_ids") or [])
        if str(rid) in design_metrics
    ]
    if not requirement_ids:
        return
    assigned = list(task.get("assigned_languages") or [])
    if len(assigned) != 1:
        raise BenchmarkError(
            "Language Quality design workers must be one-language evidence shards"
        )
    language = str(assigned[0])
    evidence = result.get("evidence")
    if not isinstance(evidence, dict):
        raise BenchmarkError("Language Quality design result requires evidence object")

    points = asset["scoring"]["level_points"]
    computed: dict[str, dict[str, float]] = {}
    detail: dict[str, Any] = {}
    for rid in requirement_ids:
        row = evidence.get(rid)
        if not isinstance(row, dict):
            raise BenchmarkError(f"{rid}: Language Quality design evidence is missing")
        rubric = design_metrics[rid]
        if row.get("rubric_id") != rubric["rubric_id"]:
            raise BenchmarkError(
                f"{rid}: rubric_id must equal frozen {rubric['rubric_id']}"
            )
        if row.get("selection_rule") != rubric["selection_rule"]:
            raise BenchmarkError(f"{rid}: selection_rule differs from frozen rubric")

        component_ids = [str(item["id"]) for item in rubric["components"]]
        levels = row.get("component_levels")
        findings = row.get("component_findings")
        if not isinstance(levels, dict) or set(levels) != set(component_ids):
            raise BenchmarkError(
                f"{rid}: component_levels must exactly match frozen component IDs"
            )
        if not isinstance(findings, dict) or set(findings) != set(component_ids):
            raise BenchmarkError(
                f"{rid}: component_findings must exactly match frozen component IDs"
            )

        score = 0
        normalized_levels: dict[str, int] = {}
        for cid in component_ids:
            level = levels[cid]
            if (
                isinstance(level, bool)
                or not isinstance(level, int)
                or level not in {0, 1, 2, 3, 4}
            ):
                raise BenchmarkError(f"{rid}/{cid}: component level must be integer 0..4")
            finding = findings[cid]
            if not isinstance(finding, str) or not finding.strip():
                raise BenchmarkError(f"{rid}/{cid}: component finding is empty")
            normalized_levels[cid] = level
            score += int(points[str(level)])

        refs = row.get("evidence_refs")
        if not isinstance(refs, list) or not refs:
            raise BenchmarkError(f"{rid}: evidence_refs must be a non-empty array")
        normalized_refs = [
            _language_quality_design_evidence_ref(root, value) for value in refs
        ]
        if len(normalized_refs) != len(set(normalized_refs)):
            raise BenchmarkError(f"{rid}: evidence_refs contains duplicates")
        limitations = row.get("limitations")
        if not isinstance(limitations, str):
            raise BenchmarkError(f"{rid}: limitations must be a string")

        row["component_levels"] = normalized_levels
        row["evidence_refs"] = normalized_refs
        row["runner_score_0_100"] = score
        computed[rid] = {language: float(score)}
        detail[rid] = {
            "rubric_id": rubric["rubric_id"],
            "component_levels": normalized_levels,
            "evidence_refs": normalized_refs,
            "score_0_100": score,
        }

    requirements = result.get("requirements")
    if requirements is None:
        requirements = {}
    if not isinstance(requirements, dict):
        raise BenchmarkError("Language Quality result requirements must be an object")
    for rid, value in computed.items():
        requirements[rid] = value
    result["requirements"] = requirements
    evidence["language_quality_design_runner_scoring"] = {
        "rubric_set_id": asset.get("rubric_set_id"),
        "language": language,
        "metrics": detail,
    }


def cmd_result_check(args: argparse.Namespace) -> int:
    root = workspace(args)
    agent_dir = require_under(root / "work" / "agents" / args.id, root)
    task_path = agent_dir / "task.json"
    result_path = agent_dir / "result.json"
    if not task_path.is_file() or not result_path.is_file():
        raise BenchmarkError("task.json/result.json missing")
    task = json_load(task_path)
    try:
        result = json_load(result_path)
    except json.JSONDecodeError as exc:
        raise BenchmarkError(f"result.json is not valid JSON: {exc}") from exc
    if not isinstance(result, dict):
        raise BenchmarkError("result.json must be a JSON object")
    if result.get("schema_version") != 1:
        raise BenchmarkError("result.json schema_version must be 1")
    if result.get("evaluation") != task.get("evaluation"):
        raise BenchmarkError("result.json evaluation mismatch")

    apply_ecosystem_runner_scores(root, task, result)
    apply_language_quality_design_runner_scores(root, task, result)
    if task.get("evaluation") in {"ecosystem", "language_quality"}:
        # Persist only trusted runner-computed score projections.
        json_dump(result_path, result)

    requirement_ids = list(task.get("requirement_ids", []))
    if not requirement_ids:
        if not isinstance(result.get("audit_pass"), bool):
            raise BenchmarkError("audit result requires boolean audit_pass")
    else:
        req = result.get("requirements")
        if not isinstance(req, dict):
            raise BenchmarkError("result.json requires requirements object")
        missing = sorted(set(requirement_ids) - set(req))
        unknown = sorted(set(req) - set(requirement_ids))
        if missing or unknown:
            raise BenchmarkError(
                f"result requirement mismatch; missing={missing}, unknown={unknown}"
            )
        languages = metadata_languages(root)
        assigned_languages = list(task.get("assigned_languages", []) or [])
        expected_languages = assigned_languages or languages
        for rid in requirement_ids:
            value = req[rid]
            if rid.startswith("gate.") or rid.startswith("coverage."):
                if assigned_languages:
                    raise BenchmarkError(
                        f"{rid}: gate/coverage work may not be language-sharded"
                    )
                if not isinstance(value, bool):
                    raise BenchmarkError(f"{rid}: gate/coverage result must be boolean")
                if rid == COMPARABILITY_GATE and value is False:
                    validate_comparability_repair_directives(root, result)
                    affected = comparability_revalidation_probes(result)
                    if not affected:
                        raise BenchmarkError(
                            "failed comparability gate must identify at least one "
                            "affected probe/label pair"
                        )
            elif rid.startswith("metric.") or rid.startswith("condition."):
                if not isinstance(value, dict):
                    raise BenchmarkError(f"{rid}: score result must map assigned languages")
                if set(value) != set(expected_languages):
                    raise BenchmarkError(
                        f"{rid}: expected languages {sorted(expected_languages)}; "
                        f"got {sorted(value)}"
                    )
                for language in expected_languages:
                    score_or_na(value[language])
            elif rid.startswith(CANONICAL_FRAGMENT_PREFIX):
                if value is not True:
                    raise BenchmarkError(
                        f"{rid}: canonical fragment leaf requirement must be true"
                    )
                if len(assigned_languages) != 1:
                    raise BenchmarkError(
                        f"{rid}: canonical fragment leaf must own exactly one language"
                    )
            elif rid.startswith(SUPPORT_ADJUDICATION_PREFIX):
                # One probe, every language, one level each: the whole point of
                # the unit is that it answers for the cohort, so it may not be
                # language-sharded and may not answer for only some of them.
                if assigned_languages:
                    raise BenchmarkError(
                        f"{rid}: a support adjudication may not be language-sharded"
                    )
                if not isinstance(value, dict):
                    raise BenchmarkError(
                        f"{rid}: support adjudication must map every language to a level"
                    )
                if set(value) != set(languages):
                    raise BenchmarkError(
                        f"{rid}: expected languages {sorted(languages)}; "
                        f"got {sorted(value)}"
                    )
                for language in languages:
                    if sc_adjudicated_record(value[language]) is None:
                        raise BenchmarkError(
                            f"{rid}: {language}: support adjudication must be a "
                            "complete canonical record with level, fragment, "
                            "partial_reasons, none_reason, justification and citation"
                        )
                validate_support_adjudication_against_canonical_fragments(
                    root, rid, value
                )
            else:
                raise BenchmarkError(f"unsupported requirement result type: {rid}")

    if task.get("canonical_fragment_owner"):
        validate_canonical_fragment_owner_result(root, task, result)
    if task.get("canonical_fragment_source_requirement"):
        validate_canonical_fragment_consumer_result(root, task, result)

    print(json.dumps({"ok": True, "agent_id": args.id, "result": str(result_path)}, indent=2))
    return 0


def cmd_command_result_check(args: argparse.Namespace) -> int:
    root = workspace(args)
    unit = manifest_unit_by_id(root, args.id)
    if unit.get("execution_kind") != "command":
        raise BenchmarkError("command-result-check requires a command work unit")
    if unit.get("result_kind") != "requirements":
        raise BenchmarkError("command-result-check requires requirement-level command output")
    evidence = list(unit.get("evidence_paths", []))
    if len(evidence) != 1:
        raise BenchmarkError("command requirement unit must have exactly one result evidence path")
    result_path = Path(evidence[0])
    if not result_path.is_file():
        raise BenchmarkError(f"command result is missing: {result_path}")
    result = json_load(result_path)
    if result.get("schema_version") != 1:
        raise BenchmarkError("command result schema_version must be 1")
    if result.get("evaluation") != unit.get("evaluation"):
        raise BenchmarkError("command result evaluation mismatch")
    req = result.get("requirements")
    if not isinstance(req, dict):
        raise BenchmarkError("command result requires requirements object")
    requirement_ids = list(unit.get("requirement_ids", []))
    missing = sorted(set(requirement_ids) - set(req))
    unknown = sorted(set(req) - set(requirement_ids))
    if missing or unknown:
        raise BenchmarkError(
            f"command result requirement mismatch; missing={missing}, unknown={unknown}"
        )
    languages = metadata_languages(root)
    assigned_languages = list(unit.get("assigned_languages", []) or [])
    expected_languages = assigned_languages or languages
    for rid in requirement_ids:
        value = req[rid]
        if rid.startswith("gate.") or rid.startswith("coverage."):
            if not isinstance(value, bool):
                raise BenchmarkError(f"{rid}: command gate/coverage result must be boolean")
        elif rid.startswith("metric.") or rid.startswith("condition."):
            if not isinstance(value, dict) or set(value) != set(expected_languages):
                raise BenchmarkError(
                    f"{rid}: command score must contain exactly the assigned languages "
                    f"{sorted(expected_languages)}"
                )
            for language in expected_languages:
                score_or_na(value[language])
        else:
            raise BenchmarkError(f"unsupported command requirement type: {rid}")
    print(json.dumps({"ok": True, "work_unit_id": args.id, "result": str(result_path)}, indent=2))
    return 0


def first_accepted_trial_index(actions: list[dict[str, Any]]) -> int | None:
    """Position of the first trial_start the runtime accepted, in trace order.

    The runtime locks learnability trials until the preflight and leakage
    attestations pass, and answers an early trial_start with a denial. Such a
    denied start is not a scored trial: the model wrote the attestations after
    it and started again. Measuring "before the first trial" from the denied
    attempt, as the third paid run's promotion did, rejected three units whose
    trials were in fact locked until the attestations passed. Trace order is
    used rather than turn numbers because one turn may carry several actions.
    """
    for index, entry in enumerate(actions):
        if entry.get("action") != "trial_start":
            continue
        observation = entry.get("observation") or {}
        if observation.get("denied"):
            continue
        return index
    return None


def sandbox_agent_trace_problems(agent_dir: Path, agent_id: str) -> list[str]:
    """Validate the audit trace scripts/sandbox_agent.py leaves behind."""
    trace_path = agent_dir / "agent_trace.json"
    if not trace_path.is_file():
        return ["agent_trace.json is missing; the work unit did not run through "
                "scripts/sandbox_agent.py inside the sandbox"]
    try:
        trace = json_load(trace_path)
    except (OSError, json.JSONDecodeError) as exc:
        return [f"agent_trace.json is unreadable: {exc}"]

    problems: list[str] = []
    if trace.get("schema_version") != 1:
        problems.append("unsupported agent trace schema_version")
    if trace.get("agent_id") != agent_id:
        problems.append("agent trace belongs to a different agent")
    if trace.get("worker_mode") != "sandbox-agent":
        problems.append("agent trace does not record sandbox-agent mode")

    task_path = agent_dir / "task.json"
    if task_path.is_file():
        expected = json_load(task_path).get("prompt_sha256")
        if expected and trace.get("prompt_sha256") != expected:
            problems.append("agent trace was produced from a different Task Packet")
    if trace.get("stop_reason") != "final":
        problems.append(f"agent run did not finish deliberately: {trace.get('stop_reason')!r}")
    if trace.get("missing_outputs"):
        problems.append("agent run left expected outputs missing")

    gateway = trace.get("gateway") or {}
    if gateway.get("credential_less_client") is not True:
        problems.append("agent trace does not record a credential-less inference gateway")
    if gateway.get("host_tools_exposed") is not False:
        problems.append("agent trace records a gateway exposing host tools")
    return problems


def cmd_task_start(args: argparse.Namespace) -> int:
    root = workspace(args)
    unit = manifest_unit_by_id(root, args.id)
    if unit.get("execution_kind", "agent") != "agent":
        raise BenchmarkError("task-start is only valid for agent work units")
    agent_id = str(unit["assigned_agent_id"])
    task_path = root / "work" / "agents" / agent_id / "task.json"
    if not task_path.is_file():
        raise BenchmarkError(
            f"Task Packet has not been materialized for {args.id}; run advance first"
        )
    ledger = json_load(root / "work" / "root" / "ledger.json")
    if ledger.get("units", {}).get(args.id, {}).get("status", "PENDING") != "PENDING":
        raise BenchmarkError(f"task-start requires PENDING unit: {args.id}")
    worker_mode = str(unit.get("worker_mode") or "packet-only")
    if worker_mode == "packet-only":
        if os.environ.get("QUIDRA_BENCHMARK_WORKER_GATEWAY_ATTESTED") != "packet-gateway-v1":
            raise BenchmarkError("packet-only dispatch requires the attested worker gateway")
        if os.environ.get("QUIDRA_BENCHMARK_PACKET_WORKER_LOCAL_TOOLS") != "disabled":
            raise BenchmarkError("packet-only dispatch requires local worker tools to be disabled")
    elif worker_mode == "sandbox-agent":
        if os.environ.get("QUIDRA_BENCHMARK_SANDBOX_AGENT_LAUNCHER_ATTESTED") != "inside-sandbox-v1":
            raise BenchmarkError("sandbox-agent dispatch requires the in-sandbox launcher")
    else:
        raise BenchmarkError(f"unsupported worker_mode for task-start: {worker_mode}")
    ns = argparse.Namespace(
        workspace=str(root), id=args.id, status="RUNNING", evidence=[],
        validation_result=None, blocker=None, blocker_class=None,
    )
    return cmd_ledger_update(ns)


def cmd_heartbeat(args: argparse.Namespace) -> int:
    root = workspace(args)
    ledger_path = root / "work" / "root" / "ledger.json"
    ledger = json_load(ledger_path)
    state = ledger.get("units", {}).get(args.id)
    if not state or state.get("status") != "RUNNING":
        raise BenchmarkError("heartbeat requires a RUNNING work unit")
    ns = argparse.Namespace(
        workspace=str(root), id=args.id, status="RUNNING", evidence=[],
        validation_result=state.get("validation_result"), blocker=None, blocker_class=None,
    )
    return cmd_ledger_update(ns)



def archive_attempt(
    root: Path,
    unit: dict[str, Any],
    attempt: int,
    reason: str,
    *,
    reset: bool,
    detail: str | None = None,
) -> str | None:
    agent_id = str(unit["assigned_agent_id"])
    agent_dir = root / "work" / "agents" / agent_id
    if not agent_dir.is_dir() or not any(agent_dir.iterdir()):
        return None
    archive = (
        root / "work" / "attempts" / str(unit["id"]) / f"attempt-{attempt:02d}"
    )
    archive.parent.mkdir(parents=True, exist_ok=True)
    if archive.exists():
        shutil.rmtree(archive)
    shutil.copytree(agent_dir, archive)
    json_dump(archive / "attempt.json", {
        "schema_version": 1,
        "work_unit_id": unit["id"],
        "agent_id": agent_id,
        "attempt": attempt,
        "reason": reason,
        # Why it failed, in the words of the process that failed it. The first
        # paid run archived forty-three retried attempts with only a reason
        # code, and nothing could say afterwards what had gone wrong.
        "detail": (" ".join(str(detail or "").split())[:4000] or None),
        "archived_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
    })
    if reset:
        task = json_load(agent_dir / "task.json")
        # Scored trial calls are the expensive atomic measurements. A retry of the
        # surrounding worker must not buy them again merely because result.json,
        # an orchestration turn, or a validator failed. Preserve only runtime-owned
        # trial records plus the audit trace that proves when/how those calls were
        # made; ordinary worker-authored outputs are intentionally rebuilt.
        trial_resume = False
        trace_name: str | None = None
        security_violation = "sandbox filesystem policy violation:" in str(detail or "")
        if is_trial_unit(unit) and not security_violation:
            for candidate in ("agent_trace.json", "agent_trace.partial.json"):
                if (archive / candidate).is_file():
                    trace_name = candidate
                    trial_resume = True
                    break

        shutil.rmtree(agent_dir)
        agent_dir.mkdir(parents=True, exist_ok=True)
        json_dump(agent_dir / "task.json", task)

        if trial_resume and trace_name is not None:
            archived_trials = archive / "trials"
            if archived_trials.is_dir():
                shutil.copytree(archived_trials, agent_dir / "trials")
            # Learnability attestations are frozen-snapshot infrastructure evidence,
            # not model scores. Keeping them lets a resumed trial remain auditable
            # without pretending that a fresh preflight happened after paid calls.
            for name in (
                "learnability_preflight.json",
                "learnability_leakage.json",
                "trial_call_journal.json",
            ):
                src = archive / name
                if src.is_file():
                    shutil.copy2(src, agent_dir / name)
            shutil.copy2(archive / trace_name, agent_dir / "resume_trace.json")
    return str(archive)


def previous_attempt_feedback(root: Path, agent_id: str, limit: int = 1500) -> str | None:
    """What the last archived attempt of this agent's work unit was rejected for.

    A retried unit is otherwise a fresh worker with no memory of the failure,
    so a shape error in result.json comes back identical at full price. The
    detail is folded into the next attempt's prompt as feedback; the Task
    Packet itself is unchanged, so the frozen packet hash and the certified
    cache key are unchanged too.
    """
    manifest_path = root / "work" / "root" / "manifest.json"
    if not manifest_path.is_file():
        return None
    unit_id = None
    for unit in json_load(manifest_path).get("work_units", []):
        if str(unit.get("assigned_agent_id") or "") == agent_id:
            unit_id = str(unit.get("id"))
            break
    if not unit_id:
        return None
    attempts = sorted((root / "work" / "attempts" / unit_id).glob("attempt-*/attempt.json"))
    if not attempts:
        return None
    # Every earlier rejection, not only the last one. A unit rehearsed after
    # the third paid run fixed the first attempt's fault on the second attempt
    # and reintroduced it on the third, because each retry saw only the
    # rejection immediately before it.
    lines: list[str] = []
    for path in attempts:
        try:
            archived = json_load(path)
        except (OSError, json.JSONDecodeError):
            continue
        detail = str(archived.get("detail") or "").strip()
        if not detail:
            detail = f"archived as {archived.get('reason')}"
        detail = re.sub(r"/quidra-benchmark/work/agents/[^\s/]+/", "", detail)
        lines.append(f"attempt {archived.get('attempt')}: {detail[:limit]}")
    if not lines:
        return None
    return "\n".join(lines)


def _validation_detail(agent_dir: Path, limit: int = 1500) -> str:
    path = agent_dir / "validation.json"
    if not path.is_file():
        return "validator produced no record"
    try:
        validation = json_load(path)
    except (OSError, json.JSONDecodeError):
        return "validator record is unreadable"
    text = str(validation.get("stderr") or "").strip() or str(validation.get("stdout") or "").strip()
    text = " ".join(text.split())
    # A traceback is noise to the next attempt; its last line is the message.
    if "Traceback (most recent call last)" in text:
        text = text.rsplit("Traceback (most recent call last)", 1)[-1].split()[-1:]
        text = " ".join(text)
    return (text or "validator failed without a message")[-limit:]


def trial_toolchain_evidence_problems(
    unit: dict[str, Any], trace: dict[str, Any]
) -> list[str]:
    """Require the assigned toolchain to succeed before the first scored trial.

    This applies to both Learnability and Proficiency. A score may not claim
    compile/run success when the language toolchain never ran, and a token
    version probe performed after scored trials cannot retroactively validate
    those trials. Only successful direct toolchain invocations before the first
    accepted trial_start count.
    """
    problems: list[str] = []
    actions = list(trace.get("trace", []))
    boundary = first_accepted_trial_index(actions)
    if boundary is None:
        return ["no scored trial_start was accepted by the runtime"]
    actions = actions[:boundary]
    for language in unit.get("assigned_languages", []) or []:
        programs = LANGUAGE_TOOLCHAIN_PROGRAMS.get(str(language))
        if not programs:
            continue
        seen_success = False
        for entry in actions:
            if entry.get("action") != "run":
                continue
            observation = entry.get("observation") or {}
            argv = observation.get("argv") or []
            if not argv or observation.get("exit_code") != 0:
                continue
            program = str(argv[0]).rsplit("/", 1)[-1]
            if program in programs:
                seen_success = True
                break
        if not seen_success:
            problems.append(
                f"no successful {language} toolchain invocation ({', '.join(programs)}) "
                "appears before the first scored trial_start"
            )
    return problems


# Compatibility name retained for existing callers while the same contract is
# now enforced for both scored trial evaluations.
learnability_toolchain_evidence_problems = trial_toolchain_evidence_problems


def toolchain_evidence_required(root: Path) -> bool:
    """Whether a trial unit must show its toolchain actually ran.

    Always, in a real run at the canonical workspace root. The synthetic CI
    harness drives the runtime on a host with no comparison toolchains and no
    built Quidra compiler, under the same explicit escape hatch that lets it
    use synthetic runner commands; there the scripted agent's version probe is
    denied and the check would fail every unit for reasons unrelated to what
    CI proves. The hatch never applies at /quidra-benchmark.
    """
    return not (
        lexical_absolute(root) != lexical_absolute(CANONICAL_WORKSPACE)
        and os.environ.get("QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS") == "1"
    )


PROFICIENCY_WORKLOADS_RELATIVE = Path(
    "template/methodology-assets/llm_proficiency/workloads.json"
)


def proficiency_workload_contract_sha256(root: Path) -> str:
    """Hash the complete frozen prompt+hidden-oracle contract."""
    return sha256_file(root / PROFICIENCY_WORKLOADS_RELATIVE)


def proficiency_workload_contract(root: Path) -> dict[str, Any]:
    """Load and mechanically validate the frozen offline Proficiency tasks."""
    asset = json_load(root / PROFICIENCY_WORKLOADS_RELATIVE)
    if asset.get("schema_version") != 2:
        raise BenchmarkError("unsupported LLM Proficiency workload schema")
    policy = asset.get("policy") or {}
    if (
        policy.get("hidden_tests_runtime_only") is not True
        or policy.get("success_is_external_oracle_not_program_self_report") is not True
    ):
        raise BenchmarkError(
            "LLM Proficiency workload v2 must use runtime-only external oracle tests"
        )
    cfg = json_load(root / "template" / "config" / "primary.json")["llm_proficiency"]
    expected_workloads = [str(value) for value in cfg["primary_workloads"]]
    expected_scenarios = [str(value) for value in cfg["primary_scenarios"]]
    workloads = asset.get("workloads")
    scenarios = asset.get("scenarios")
    provenance = asset.get("source_provenance")
    if not isinstance(workloads, dict) or set(workloads) != set(expected_workloads):
        raise BenchmarkError(
            "LLM Proficiency workload asset must match primary_workloads exactly"
        )
    if not isinstance(scenarios, dict) or set(scenarios) != set(expected_scenarios):
        raise BenchmarkError(
            "LLM Proficiency scenario asset must match primary_scenarios exactly"
        )
    if not isinstance(provenance, dict) or set(provenance) != set(expected_workloads):
        raise BenchmarkError(
            "LLM Proficiency source provenance must cover every Primary workload"
        )
    for workload in expected_workloads:
        row = workloads[workload]
        source = provenance[workload]
        validation = (row or {}).get("validation") or {}
        tests = (row or {}).get("trusted_tests")
        if (
            not isinstance(row, dict)
            or not str(row.get("subset_id") or "").strip()
            or not str(row.get("specification") or "").strip()
            or not str(row.get("reference_cpp") or "").strip()
            or not isinstance(validation, dict)
            or not str(validation.get("output_prefix") or "").strip()
            or not isinstance(validation.get("float_absolute_tolerance"), (int, float))
            or float(validation["float_absolute_tolerance"]) <= 0
            or not isinstance(tests, list)
            or len(tests) < 2
            or not all(isinstance(test, dict) and str(test.get("id") or "") for test in tests)
        ):
            raise BenchmarkError(
                f"LLM Proficiency workload {workload} has an incomplete frozen contract"
            )
        test_ids = [str(test["id"]) for test in tests]
        if len(test_ids) != len(set(test_ids)) or "public" not in test_ids:
            raise BenchmarkError(
                f"LLM Proficiency workload {workload} must have unique tests including public"
            )
        if not any(test_id.startswith("hidden-") for test_id in test_ids):
            raise BenchmarkError(
                f"LLM Proficiency workload {workload} has no hidden oracle case"
            )
        if workload == "SVM":
            for test in tests:
                samples = test.get("samples")
                if (
                    not isinstance(samples, list)
                    or len(samples) < 2
                    or not all(
                        isinstance(sample, list)
                        and len(sample) == 3
                        and int(sample[0]) in {-1, 1}
                        for sample in samples
                    )
                    or float(test.get("learning_rate", 0)) <= 0
                    or float(test.get("convergence_limit", 0)) <= 0
                    or int(test.get("max_epochs", 0)) <= 0
                ):
                    raise BenchmarkError(f"SVM trusted test is invalid: {test.get('id')}")
        elif workload == "GMM":
            for test in tests:
                observations = test.get("observations")
                if (
                    not isinstance(observations, list)
                    or len(observations) < 2
                    or int(test.get("iterations", 0)) <= 0
                    or any(
                        not isinstance(test.get(name), list)
                        or len(test[name]) != 2
                        for name in ("weights", "means", "variances")
                    )
                    or any(float(value) <= 0 for value in test["variances"])
                ):
                    raise BenchmarkError(f"GMM trusted test is invalid: {test.get('id')}")
        elif workload == "LightGrad":
            for test in tests:
                values = test.get("inputs")
                if not isinstance(values, list) or len(values) != 3:
                    raise BenchmarkError(
                        f"LightGrad trusted test is invalid: {test.get('id')}"
                    )
        else:
            raise BenchmarkError(f"unsupported LLM Proficiency workload: {workload}")

        commit = str((source or {}).get("commit") or "")
        paths = (source or {}).get("paths")
        if (
            not re.fullmatch(r"[0-9a-f]{40}", commit)
            or not isinstance(paths, list)
            or not paths
            or not all(isinstance(path, str) and path for path in paths)
        ):
            raise BenchmarkError(
                f"LLM Proficiency workload {workload} has invalid source provenance"
            )
    for scenario in expected_scenarios:
        row = scenarios[scenario]
        if (
            not isinstance(row, dict)
            or not isinstance(row.get("include_reference"), bool)
            or not str(row.get("instruction") or "").strip()
        ):
            raise BenchmarkError(
                f"LLM Proficiency scenario {scenario} has an incomplete frozen contract"
            )
    if scenarios["specification_to_implementation"]["include_reference"] is not False:
        raise BenchmarkError(
            "specification_to_implementation must not expose the reference implementation"
        )
    if scenarios["reference_to_porting"]["include_reference"] is not True:
        raise BenchmarkError("reference_to_porting must expose the frozen reference")
    return asset


def proficiency_trial_manifest(root: Path) -> dict[str, dict[str, Any]]:
    """Exact Primary trial IDs mapped to their frozen cell identity."""
    cfg = json_load(root / "template" / "config" / "primary.json")["llm_proficiency"]
    proficiency_workload_contract(root)
    workloads = [str(value) for value in cfg["primary_workloads"]]
    scenarios = [str(value) for value in cfg["primary_scenarios"]]
    replications = int(cfg["independent_trials_per_replicated_cell"])
    if replications < 1:
        raise BenchmarkError("LLM Proficiency requires at least one Primary replication")
    variants = [str(value) for value in (cfg.get("primary_prompt_variants") or [])]
    if (
        len(variants) != replications
        or len(set(variants)) != len(variants)
        or any(not variant for variant in variants)
    ):
        raise BenchmarkError(
            "LLM Proficiency primary_prompt_variants must contain exactly one "
            "unique frozen variant per independent trial"
        )
    manifest: dict[str, dict[str, Any]] = {}
    for workload in workloads:
        for scenario in scenarios:
            for replication in range(1, replications + 1):
                trial_id = f"{slug_id(workload)}--{slug_id(scenario)}--t{replication}"
                if trial_id in manifest:
                    raise BenchmarkError(
                        "LLM Proficiency workload/scenario names collide after "
                        "trial-ID normalization"
                    )
                manifest[trial_id] = {
                    "workload": workload,
                    "scenario": scenario,
                    "replication": replication,
                    "prompt_variant": variants[replication - 1],
                }
    return manifest


def proficiency_required_trial_ids(root: Path) -> list[str]:
    """The exact fresh-session IDs required by the frozen Primary allocation."""
    return list(proficiency_trial_manifest(root))


def proficiency_expected_prompt(root: Path, language: str, trial_id: str) -> str:
    """Trusted initial prompt for one frozen Proficiency cell."""
    languages = metadata_languages(root)
    if language not in languages:
        raise BenchmarkError(f"unknown LLM Proficiency target language: {language}")
    manifest = proficiency_trial_manifest(root)
    cell = manifest.get(trial_id)
    if cell is None:
        raise BenchmarkError(f"unknown LLM Proficiency trial ID: {trial_id}")
    asset = proficiency_workload_contract(root)
    workload = asset["workloads"][cell["workload"]]
    scenario = asset["scenarios"][cell["scenario"]]
    environment = json_load(root / "template" / "environment" / "environment.json")
    recipe = (environment.get("frozen_toolchain_recipes") or {}).get(language)
    if not isinstance(recipe, dict):
        raise BenchmarkError(
            f"LLM Proficiency has no frozen toolchain recipe for {language}"
        )
    metadata = [
        "# Frozen LLM Proficiency Trial",
        f"Target language: {language}",
        f"Workload: {cell['workload']} ({workload['subset_id']})",
        f"Scenario: {cell['scenario']}",
        f"Prompt variant: {cell['prompt_variant']}",
    ]
    rules = [
        "Rules:",
        "- Return only one complete source program, with no Markdown fences or explanation.",
        "- Use only the target language and its standard library/runtime shipped in the frozen toolchain.",
        "- Do not use the network, package downloads, third-party dependencies, generated bindings, or external services.",
        "- Preserve the algorithm and validation contract exactly; do not replace the task with a simpler computation or hard-code the public example output.",
        "- The trusted runtime supplies additional hidden stdin cases; your program must compute its output from stdin on every run.",
        f"- Frozen build/run recipe: {json.dumps(recipe, sort_keys=True, separators=(',', ':'))}",
    ]
    scenario_block = ["Scenario instruction:", str(scenario["instruction"])]
    specification_block = [
        "Frozen specification:",
        str(workload["specification"]).strip(),
    ]
    reference_block = (
        [
            "Frozen C++ reference implementation:",
            str(workload["reference_cpp"]).strip(),
        ]
        if scenario["include_reference"]
        else []
    )
    validation_block = [
        "Frozen validation contract:",
        json.dumps(workload["validation"], sort_keys=True, separators=(",", ":")),
    ]
    layouts = {
        "canonical": [
            metadata, rules, scenario_block, specification_block,
            reference_block, validation_block,
        ],
        "scenario-first": [
            metadata, scenario_block, specification_block, reference_block,
            rules, validation_block,
        ],
        "contract-first": [
            metadata, validation_block, rules, scenario_block,
            specification_block, reference_block,
        ],
    }
    variant = str(cell["prompt_variant"])
    selected = layouts.get(variant)
    if selected is None:
        raise BenchmarkError(f"unknown frozen Proficiency prompt variant: {variant}")
    sections: list[str] = []
    for block in selected:
        if not block:
            continue
        if sections:
            sections.append("")
        sections.extend(block)
    sections.extend(["", "Return only the complete target-language source program."])
    return "\n".join(sections).rstrip() + "\n"


PROFICIENCY_SOURCE_FILES = {
    "Quidra": "main.qui",
    "Python": "main.py",
    "C++": "main.cpp",
    "Rust": "main.rs",
    "Go": "main.go",
    "Java": "Main.java",
    "TypeScript": "main.ts",
    "Kotlin": "main.kt",
    "Swift": "main.swift",
    "Zig": "main.zig",
}


def proficiency_verifier_commands(
    root: Path, language: str
) -> tuple[str, list[str] | None, list[str]]:
    """Render the frozen single-file build/run recipe for Proficiency."""
    source_name = PROFICIENCY_SOURCE_FILES.get(language)
    if source_name is None:
        raise BenchmarkError(f"unknown LLM Proficiency language: {language}")
    environment = json_load(root / "template" / "environment" / "environment.json")
    recipe = (environment.get("frozen_toolchain_recipes") or {}).get(language)
    if not isinstance(recipe, dict) or not isinstance(recipe.get("run"), str):
        raise BenchmarkError(f"LLM Proficiency has no frozen run recipe for {language}")
    replacements = {
        "FILE.qui": source_name,
        "FILE.py": source_name,
        "FILE.cpp": source_name,
        "FILE.rs": source_name,
        "FILE.go": source_name,
        "FILE.java": source_name,
        "FILE.ts": source_name,
        "FILE.kt": source_name,
        "FILE.swift": source_name,
        "FILE.zig": source_name,
        "FILE.jar": "program.jar",
        "FILE.js": str(PurePosixPath(source_name).with_suffix(".js")),
        "BIN": "program",
        "OUT": "out",
    }

    def render(command: str) -> list[str]:
        argv: list[str] = []
        for raw in shlex.split(command):
            value = raw
            for key in sorted(replacements, key=len, reverse=True):
                value = value.replace(key, replacements[key])
            argv.append(value)
        return argv

    build = recipe.get("build")
    return (
        source_name,
        render(build) if isinstance(build, str) and build.strip() else None,
        render(str(recipe["run"])),
    )


def _proficiency_svm_oracle(test: dict[str, Any]) -> tuple[str, dict[str, Any]]:
    samples = test["samples"]
    labels = [int(sample[0]) for sample in samples]
    points = [[float(sample[1]), float(sample[2])] for sample in samples]
    n = len(points)
    alpha = [0.0] * n
    beta = 1.0
    learning_rate = float(test["learning_rate"])
    limit = float(test["convergence_limit"])
    converged = False
    for _epoch in range(int(test["max_epochs"])):
        judge = False
        for i in range(n):
            item1 = 0.0
            item2 = 0.0
            for j in range(n):
                dot = points[i][0] * points[j][0] + points[i][1] * points[j][1]
                item1 += alpha[j] * labels[i] * labels[j] * dot
                item2 += alpha[j] * labels[i] * labels[j]
            delta = 1.0 - item1 - beta * item2
            alpha[i] += learning_rate * delta
            if alpha[i] < 0:
                alpha[i] = 0.0
            elif abs(delta) > limit:
                judge = True
        item3 = sum(alpha[i] * labels[i] for i in range(n))
        beta += item3 * item3 / 2.0
        if not judge:
            converged = True
            break
    if not converged:
        raise BenchmarkError(f"SVM trusted oracle did not converge: {test['id']}")
    support = [i for i in range(n) if alpha[i] > 1e-7]
    if not support:
        raise BenchmarkError(f"SVM trusted oracle has no support vector: {test['id']}")
    w = [0.0, 0.0]
    for i in support:
        w[0] += alpha[i] * labels[i] * points[i][0]
        w[1] += alpha[i] * labels[i] * points[i][1]
    b = sum(
        labels[i] - (w[0] * points[i][0] + w[1] * points[i][1])
        for i in support
    ) / len(support)
    predictions = [
        1 if w[0] * point[0] + w[1] * point[1] + b >= 0 else -1
        for point in points
    ]
    lines = [f"{n} 2"]
    lines.extend(
        f"{labels[i]} {points[i][0]:.17g} {points[i][1]:.17g}" for i in range(n)
    )
    lines.append(
        f"{learning_rate:.17g} {limit:.17g} {int(test['max_epochs'])}"
    )
    expected = {"floats": [w[0], w[1], b], "ints": predictions}
    return "\n".join(lines) + "\n", expected


def _proficiency_gmm_oracle(test: dict[str, Any]) -> tuple[str, dict[str, Any]]:
    observations = [float(value) for value in test["observations"]]
    weights = [float(value) for value in test["weights"]]
    means = [float(value) for value in test["means"]]
    variances = [float(value) for value in test["variances"]]
    n = len(observations)
    k_count = 2
    pi = 3.141592653589793
    for _ in range(int(test["iterations"])):
        responsibilities: list[list[float]] = []
        for x in observations:
            row = []
            for k in range(k_count):
                delta = x - means[k]
                row.append(
                    weights[k]
                    * math.exp(-0.5 * delta * delta / variances[k])
                    / math.sqrt(2.0 * pi * variances[k])
                )
            total = sum(row)
            if not math.isfinite(total) or total <= 0:
                raise BenchmarkError(f"GMM trusted oracle density failed: {test['id']}")
            responsibilities.append([value / total for value in row])
        nk = [
            sum(responsibilities[i][k] for i in range(n))
            for k in range(k_count)
        ]
        if any(value <= 0 for value in nk):
            raise BenchmarkError(f"GMM trusted oracle empty component: {test['id']}")
        weights = [value / n for value in nk]
        means = [
            sum(responsibilities[i][k] * observations[i] for i in range(n)) / nk[k]
            for k in range(k_count)
        ]
        variances = [
            sum(
                responsibilities[i][k] * (observations[i] - means[k]) ** 2
                for i in range(n)
            )
            / nk[k]
            for k in range(k_count)
        ]
        if any(not math.isfinite(value) or value <= 0 for value in variances):
            raise BenchmarkError(f"GMM trusted oracle variance failed: {test['id']}")
    log_likelihood = 0.0
    for x in observations:
        density = 0.0
        for k in range(k_count):
            delta = x - means[k]
            density += (
                weights[k]
                * math.exp(-0.5 * delta * delta / variances[k])
                / math.sqrt(2.0 * pi * variances[k])
            )
        if density <= 0 or not math.isfinite(density):
            raise BenchmarkError(f"GMM trusted oracle likelihood failed: {test['id']}")
        log_likelihood += math.log(density)
    stdin = (
        f"{n} 2 {int(test['iterations'])}\n"
        + " ".join(f"{value:.17g}" for value in observations)
        + "\n"
        + " ".join(f"{value:.17g}" for value in test["weights"])
        + "\n"
        + " ".join(f"{value:.17g}" for value in test["means"])
        + "\n"
        + " ".join(f"{value:.17g}" for value in test["variances"])
        + "\n"
    )
    expected = {
        "floats": [
            weights[0], weights[1], means[0], means[1],
            variances[0], variances[1], log_likelihood,
        ],
        "ints": [],
    }
    return stdin, expected


def _proficiency_lightgrad_oracle(test: dict[str, Any]) -> tuple[str, dict[str, Any]]:
    x1, x2, x3 = [float(value) for value in test["inputs"]]
    y = x1 * x1 * x1 * x2 * x2 + x1 * x3
    g1 = 3.0 * x1 * x1 * x2 * x2 + x3
    g2 = 2.0 * x1 * x1 * x1 * x2
    g3 = x1
    stdin = f"{x1:.17g} {x2:.17g} {x3:.17g}\n"
    return stdin, {"floats": [y, g1, g2, g3], "ints": []}


_PROFICIENCY_ORACLE_CASE_CACHE: dict[tuple[str, str, str], list[dict[str, Any]]] = {}


def proficiency_trusted_oracle_cases(
    root: Path, workload_name: str
) -> list[dict[str, Any]]:
    contract_sha = proficiency_workload_contract_sha256(root)
    cache_key = (lexical_absolute(root).as_posix(), contract_sha, workload_name)
    cached = _PROFICIENCY_ORACLE_CASE_CACHE.get(cache_key)
    if cached is not None:
        return cached
    asset = proficiency_workload_contract(root)
    workload = asset["workloads"].get(workload_name)
    if not isinstance(workload, dict):
        raise BenchmarkError(f"unknown LLM Proficiency workload: {workload_name}")
    validation = workload["validation"]
    prefix = str(validation["output_prefix"])
    tolerance = float(validation["float_absolute_tolerance"])
    cases: list[dict[str, Any]] = []
    for test in workload["trusted_tests"]:
        if workload_name == "SVM":
            stdin, expected = _proficiency_svm_oracle(test)
        elif workload_name == "GMM":
            stdin, expected = _proficiency_gmm_oracle(test)
        elif workload_name == "LightGrad":
            stdin, expected = _proficiency_lightgrad_oracle(test)
        else:
            raise BenchmarkError(f"unsupported LLM Proficiency workload: {workload_name}")
        expected_bytes = json.dumps(
            {
                "prefix": prefix,
                "tolerance": tolerance,
                "expected": expected,
            },
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
        cases.append({
            "id": str(test["id"]),
            "hidden": str(test["id"]) != "public",
            "stdin": stdin,
            "input_sha256": sha256_bytes(stdin.encode("utf-8")),
            "prefix": prefix,
            "tolerance": tolerance,
            "expected": expected,
            "expected_sha256": sha256_bytes(expected_bytes),
        })
    _PROFICIENCY_ORACLE_CASE_CACHE[cache_key] = cases
    return cases


def _proficiency_oracle_output_problem(
    case: dict[str, Any], stdout: str
) -> str | None:
    lines = [line.strip() for line in str(stdout).splitlines() if line.strip()]
    if len(lines) != 1:
        return f"expected exactly one non-empty stdout line, got {len(lines)}"
    tokens = lines[0].split()
    expected = case["expected"]
    float_values = list(expected["floats"])
    int_values = list(expected["ints"])
    required = 1 + len(float_values) + len(int_values)
    if len(tokens) != required:
        return f"expected {required} output fields, got {len(tokens)}"
    if tokens[0] != case["prefix"]:
        return f"output prefix must be {case['prefix']}"
    try:
        actual_floats = [float(value) for value in tokens[1:1 + len(float_values)]]
    except ValueError:
        return "floating-point output field is not numeric"
    tolerance = float(case["tolerance"])
    for index, (actual, wanted) in enumerate(zip(actual_floats, float_values), start=1):
        if (
            not math.isfinite(actual)
            or not math.isfinite(float(wanted))
            or abs(actual - float(wanted)) > tolerance
        ):
            return f"floating-point output field {index} is outside tolerance"
    if int_values:
        try:
            actual_ints = [int(value) for value in tokens[1 + len(float_values):]]
        except ValueError:
            return "integer output field is not an integer"
        if actual_ints != [int(value) for value in int_values]:
            return "prediction fields do not match the trusted oracle"
    return None


def verify_proficiency_completion(
    root: Path,
    language: str,
    trial_id: str,
    source_text: str,
    work_dir: Path,
) -> dict[str, Any]:
    """Compile/parse then execute every public/hidden external oracle case."""
    manifest = proficiency_trial_manifest(root)
    cell = manifest.get(trial_id)
    if cell is None:
        raise BenchmarkError(f"unknown LLM Proficiency trial ID: {trial_id}")
    workload_name = str(cell["workload"])
    oracle_cases = proficiency_trusted_oracle_cases(root, workload_name)

    work_dir.mkdir(parents=True, exist_ok=True)
    synthetic_allowed = (
        lexical_absolute(root) != lexical_absolute(CANONICAL_WORKSPACE)
        and os.environ.get("QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS") == "1"
    )
    if synthetic_allowed:
        result = {
            "schema_version": 2,
            "trial_id": trial_id,
            "language": language,
            "workload": workload_name,
            "source_sha256": sha256_bytes(source_text.encode("utf-8")),
            "workload_contract_sha256": proficiency_workload_contract_sha256(root),
            "compile_or_parse": {
                "label": "synthetic-ci",
                "argv": [],
                "exit_code": None,
                "stdout": "",
                "stderr": "",
                "error": "synthetic-ci-does-not-run-target-toolchain",
            },
            "compile_parse_ok": False,
            "run": None,
            "oracle_test_count": len(oracle_cases),
            "oracle_passed_count": 0,
            "oracle_tests": [],
            "test_passed": False,
            "synthetic_ci": True,
        }
        json_dump(work_dir / "verification.json", result)
        return result

    source_name, build_argv, run_argv = proficiency_verifier_commands(root, language)
    source_path = work_dir / source_name
    source_path.parent.mkdir(parents=True, exist_ok=True)
    source_path.write_text(source_text, encoding="utf-8")
    (work_dir / "out").mkdir(exist_ok=True)

    def invoke(
        argv: list[str], label: str, input_text: str | None = None
    ) -> dict[str, Any]:
        try:
            completed = subprocess.run(
                argv,
                cwd=work_dir,
                env=sanitized_subprocess_env(root, work_dir),
                shell=False,
                input=input_text,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=120,
            )
            return {
                "label": label,
                "argv": argv,
                "exit_code": int(completed.returncode),
                "stdout": (completed.stdout or "")[:8000],
                "stderr": (completed.stderr or "")[:8000],
            }
        except subprocess.TimeoutExpired as exc:
            return {
                "label": label,
                "argv": argv,
                "exit_code": None,
                "stdout": str(exc.stdout or "")[:8000],
                "stderr": str(exc.stderr or "")[:8000],
                "error": "timeout",
            }
        except OSError as exc:
            return {
                "label": label,
                "argv": argv,
                "exit_code": None,
                "stdout": "",
                "stderr": str(exc)[:8000],
                "error": type(exc).__name__,
            }

    if build_argv is None and language == "Python":
        compile_record = invoke(["python3", "-m", "py_compile", source_name], "parse")
    elif build_argv is not None:
        compile_record = invoke(build_argv, "build")
    else:
        compile_record = {
            "label": "no-build",
            "argv": [],
            "exit_code": 0,
            "stdout": "",
            "stderr": "",
        }

    compile_parse_ok = compile_record.get("exit_code") == 0
    case_rows: list[dict[str, Any]] = []
    if compile_parse_ok:
        for case in oracle_cases:
            run_record = invoke(
                run_argv,
                f"run:{case['id']}",
                str(case["stdin"]),
            )
            problem = (
                None
                if run_record.get("exit_code") == 0
                else f"program exited with {run_record.get('exit_code')}"
            )
            if problem is None:
                problem = _proficiency_oracle_output_problem(
                    case, str(run_record.get("stdout") or "")
                )
            case_rows.append({
                "id": case["id"],
                "hidden": bool(case["hidden"]),
                "input_sha256": case["input_sha256"],
                "expected_sha256": case["expected_sha256"],
                "run": run_record,
                "passed": problem is None,
                "problem": problem,
            })

    passed_count = sum(1 for row in case_rows if row["passed"])
    first_failure = next((row for row in case_rows if not row["passed"]), None)
    representative_run = (
        (first_failure or (case_rows[0] if case_rows else {})).get("run")
        if case_rows
        else None
    )
    result = {
        "schema_version": 2,
        "trial_id": trial_id,
        "language": language,
        "workload": workload_name,
        "source_sha256": sha256_bytes(source_text.encode("utf-8")),
        "workload_contract_sha256": proficiency_workload_contract_sha256(root),
        "compile_or_parse": compile_record,
        "compile_parse_ok": compile_parse_ok,
        "run": representative_run,
        "oracle_test_count": len(oracle_cases),
        "oracle_passed_count": passed_count,
        "oracle_tests": case_rows,
        "test_passed": bool(
            compile_parse_ok
            and len(case_rows) == len(oracle_cases)
            and passed_count == len(oracle_cases)
        ),
    }
    json_dump(work_dir / "verification.json", result)
    return result


def proficiency_reference_self_test(root: Path) -> dict[str, Any]:
    """Prove the frozen reference sources and hidden oracle agree before scoring."""
    asset = proficiency_workload_contract(root)
    cfg = json_load(root / "template" / "config" / "primary.json")["llm_proficiency"]
    first_scenario = str(cfg["primary_scenarios"][0])
    reports: dict[str, Any] = {}
    passed = True
    for workload_name in [str(value) for value in cfg["primary_workloads"]]:
        trial_id = (
            f"{slug_id(workload_name)}--{slug_id(first_scenario)}--t1"
        )
        report = verify_proficiency_completion(
            root,
            "C++",
            trial_id,
            str(asset["workloads"][workload_name]["reference_cpp"]),
            root / "work" / "root" / "commands"
            / "proficiency-reference-self-test" / slug_id(workload_name),
        )
        synthetic = report.get("synthetic_ci") is True
        row_passed = synthetic or (
            report.get("compile_parse_ok") is True
            and report.get("test_passed") is True
        )
        passed = passed and row_passed
        reports[workload_name] = {
            "passed": row_passed,
            "synthetic_structural_only": synthetic,
            "compile_parse_ok": report.get("compile_parse_ok"),
            "test_passed": report.get("test_passed"),
            "oracle_test_count": report.get("oracle_test_count"),
            "oracle_passed_count": report.get("oracle_passed_count"),
        }
    return {
        "passed": bool(passed),
        "workload_contract_sha256": proficiency_workload_contract_sha256(root),
        "workloads": reports,
    }



def proficiency_public_repair_gate_passed(
    verification: dict[str, Any]
) -> bool:
    """Whether compile + PUBLIC oracle cases permit the trial to stop repairing.

    Hidden cases are score-only holdout evidence. They must never decide whether
    another model turn is granted, otherwise the repair loop becomes a one-bit
    black-box oracle over the hidden test set.
    """
    if not isinstance(verification, dict):
        return False
    if verification.get("compile_parse_ok") is not True:
        return False
    rows = verification.get("oracle_tests")
    if verification.get("synthetic_ci") is True and not rows:
        # Synthetic CI has no scored hidden/public execution; preserve its
        # structural smoke behavior without creating a production exception.
        return verification.get("test_passed") is True
    if not isinstance(rows, list):
        return False
    public_rows = [
        row for row in rows
        if isinstance(row, dict) and row.get("hidden") is not True
    ]
    if not public_rows:
        return False
    return all(row.get("passed") is True for row in public_rows)


def proficiency_verification_summary(verification: dict[str, Any]) -> dict[str, Any]:
    """Worker-readable projection with no hidden-oracle verdict or cardinality."""
    if not isinstance(verification, dict):
        raise BenchmarkError("invalid Proficiency verification summary source")
    return {
        "schema_version": verification.get("schema_version"),
        "trial_id": verification.get("trial_id"),
        "language": verification.get("language"),
        "workload": verification.get("workload"),
        "source_sha256": verification.get("source_sha256"),
        "workload_contract_sha256": verification.get("workload_contract_sha256"),
        "compile_parse_ok": verification.get("compile_parse_ok"),
        "public_repair_gate_passed": proficiency_public_repair_gate_passed(
            verification
        ),
        "synthetic_ci": verification.get("synthetic_ci") is True,
    }


def proficiency_model_visible_feedback(verification: dict[str, Any]) -> dict[str, Any]:
    """Only compile/public diagnostics safe to send back to the scored model."""
    if not isinstance(verification, dict):
        raise BenchmarkError("LLM Proficiency repair requires trusted verification")

    def compact_process(value: Any) -> dict[str, Any] | None:
        if not isinstance(value, dict):
            return None
        return {
            "label": value.get("label"),
            "argv": list(value.get("argv") or []),
            "exit_code": value.get("exit_code"),
            "stdout": str(value.get("stdout") or "")[:4000],
            "stderr": str(value.get("stderr") or "")[:4000],
            "error": value.get("error"),
        }

    public_failures: list[dict[str, Any]] = []
    for row in verification.get("oracle_tests") or []:
        if (
            not isinstance(row, dict)
            or row.get("hidden") is True
            or row.get("passed") is True
        ):
            continue
        public_failures.append({
            "problem": row.get("problem"),
            "run": compact_process(row.get("run")),
        })
    return {
        "compile_parse_ok": verification.get("compile_parse_ok"),
        "public_repair_gate_passed": proficiency_public_repair_gate_passed(
            verification
        ),
        "compile_or_parse": compact_process(verification.get("compile_or_parse")),
        "public_failures": public_failures,
    }


def proficiency_repair_prompt(verification: dict[str, Any]) -> str:
    """Deterministic compile/public-only feedback for a Proficiency repair turn."""
    if not isinstance(verification, dict):
        raise BenchmarkError("LLM Proficiency repair requires trusted verification")
    if proficiency_public_repair_gate_passed(verification):
        raise BenchmarkError(
            "LLM Proficiency compile/public repair gate is terminal; "
            "hidden score-only cases never authorize a repair"
        )
    feedback = proficiency_model_visible_feedback(verification)
    return (
        "# Frozen LLM Proficiency Repair\n"
        "Your previous program did not pass the compile/public repair gate.\n"
        "The JSON below contains only compile diagnostics and public-case failures. "
        "Hidden cases are score-only holdout evidence: their inputs, outputs, "
        "identities, counts, pass counts, and verdicts are not repair feedback. "
        "Use the feedback and the original frozen task to repair the program.\n"
        + json.dumps(feedback, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        + "\nReturn only one complete replacement source program, with no Markdown "
        "fences or explanation.\n"
    )


def proficiency_trusted_verification(
    root: Path, call: dict[str, Any]
) -> dict[str, Any]:
    """Load runner-only oracle evidence and bind it to the sanitized trace row."""
    visible = call.get("verification")
    relative = call.get("verification_path")
    if not isinstance(visible, dict):
        raise BenchmarkError("Proficiency call has no sanitized verification summary")
    if not isinstance(relative, str) or not relative:
        raise BenchmarkError("Proficiency call has no trusted verification path")
    trusted_root = root / "work" / "root" / "proficiency-verification"
    path = require_under(root / relative, trusted_root)
    if not path.is_file():
        raise BenchmarkError(f"trusted Proficiency verification is missing: {relative}")
    trusted = json_load(path)
    if proficiency_verification_summary(trusted) != visible:
        raise BenchmarkError(
            "sanitized Proficiency verification disagrees with trusted evidence"
        )
    if trusted.get("source_sha256") != call.get("completion_sha256"):
        raise BenchmarkError("trusted Proficiency verification source hash mismatch")
    return trusted


def proficiency_runtime_metrics(
    root: Path, trace: dict[str, Any]
) -> dict[str, float] | None:
    """Metrics mechanically decidable from runtime-owned trial/oracle evidence.

    Generalization uses first-attempt hidden cases, robustness uses the worst
    frozen prompt variant. Repair metrics use only the compile/public repair gate;
    hidden oracle cases remain score-only and never create a repair opportunity.
    """
    trials = ((trace.get("trials") or {}).get("trials") or {})
    if not isinstance(trials, dict) or not trials:
        return None
    manifest = proficiency_trial_manifest(root)
    if set(str(trial_id) for trial_id in trials) != set(manifest):
        return None

    cfg = json_load(root / "template" / "config" / "primary.json")["llm_proficiency"]
    variants = [str(value) for value in (cfg.get("primary_prompt_variants") or [])]
    max_repairs = int(cfg.get("max_repair_turns", 0) or 0)
    if not variants or max_repairs < 0:
        return None
    variant_success: dict[str, list[int]] = {
        variant: [0, 0] for variant in variants
    }

    generation = compiled = correct1 = correctn = 0
    oracle_passed = oracle_total = 0
    hidden_passed = hidden_total = 0
    initial_failures = repaired_failures = first_repair_successes = 0
    repair_efficiency_total = 0.0
    silent_eligible = silent_bug_trials = 0

    for trial_id, summary in trials.items():
        calls = (summary or {}).get("calls") or []
        if not calls:
            return None
        trusted_calls: list[dict[str, Any]] = []
        for call in calls:
            if not isinstance(call, dict):
                return None
            trusted_calls.append(proficiency_trusted_verification(root, call))

        first = calls[0]
        first_verification = trusted_calls[0]
        if (
            isinstance(first.get("completion"), str)
            and first["completion"].strip()
            and first.get("incomplete") is None
        ):
            generation += 1
        if first_verification.get("compile_parse_ok") is True:
            compiled += 1

        first_ok = first_verification.get("test_passed") is True
        if first_ok:
            correct1 += 1
        success_call = next(
            (
                index
                for index, verification in enumerate(trusted_calls)
                if verification.get("test_passed") is True
            ),
            None,
        )
        if success_call is not None:
            correctn += 1

        first_public_ok = proficiency_public_repair_gate_passed(
            first_verification
        )
        public_success_call = next(
            (
                index
                for index, verification in enumerate(trusted_calls)
                if proficiency_public_repair_gate_passed(verification)
            ),
            None,
        )
        if first_public_ok:
            repair_efficiency_total += 100.0
        else:
            initial_failures += 1
            if public_success_call is not None and public_success_call > 0:
                repaired_failures += 1
                if public_success_call == 1:
                    first_repair_successes += 1
                repair_efficiency_total += (
                    100.0 * (
                        1.0 - public_success_call / float(max_repairs + 1)
                    )
                    if max_repairs >= public_success_call
                    else 0.0
                )

        cell = manifest.get(str(trial_id))
        if cell is None:
            return None
        variant = str(cell.get("prompt_variant") or "")
        if variant not in variant_success:
            return None
        variant_success[variant][1] += 1
        if first_ok:
            variant_success[variant][0] += 1

        first_rows = first_verification.get("oracle_tests")
        if not isinstance(first_rows, list):
            return None
        if first_rows:
            if first_verification.get("compile_parse_ok") is True:
                silent_eligible += 1
                if any(
                    isinstance(row, dict)
                    and row.get("passed") is not True
                    and isinstance(row.get("run"), dict)
                    and row["run"].get("exit_code") == 0
                    for row in first_rows
                ):
                    silent_bug_trials += 1
            for row in first_rows:
                if not isinstance(row, dict):
                    return None
                if row.get("hidden") is True:
                    hidden_total += 1
                    if row.get("passed") is True:
                        hidden_passed += 1
        else:
            # Synthetic CI has no hidden execution. Keep the real denominator
            # without manufacturing synthetic success.
            workload = str(cell.get("workload") or "")
            expected_cases = proficiency_trusted_oracle_cases(root, workload)
            hidden_total += sum(
                1 for row in expected_cases if row.get("hidden") is True
            )

        for verification in trusted_calls:
            try:
                total = int(verification.get("oracle_test_count", 0) or 0)
                passed = int(verification.get("oracle_passed_count", 0) or 0)
            except (TypeError, ValueError):
                return None
            if total < 1 or passed < 0 or passed > total:
                return None
            oracle_total += total
            oracle_passed += passed

    denominator = float(len(trials))
    if oracle_total <= 0 or hidden_total <= 0:
        return None
    if any(total <= 0 for _passed, total in variant_success.values()):
        return None
    prompt_robustness = min(
        100.0 * passed / float(total)
        for passed, total in variant_success.values()
    )
    repair_success = (
        100.0 * repaired_failures / float(initial_failures)
        if initial_failures
        else 100.0
    )
    diagnosis_efficiency = (
        100.0 * first_repair_successes / float(initial_failures)
        if initial_failures
        else 100.0
    )
    silent_resistance = (
        100.0 * (silent_eligible - silent_bug_trials) / float(silent_eligible)
        if silent_eligible
        else 0.0
    )
    return {
        "metric.generation_success_rate": 100.0 * generation / denominator,
        "metric.compile_parse_success_rate": 100.0 * compiled / denominator,
        "metric.correct_at_1": 100.0 * correct1 / denominator,
        "metric.correct_at_n": 100.0 * correctn / denominator,
        "metric.test_pass_rate": 100.0 * oracle_passed / float(oracle_total),
        "metric.repair_success_rate": repair_success,
        "metric.repair_efficiency": repair_efficiency_total / denominator,
        "metric.diagnosis_efficiency": diagnosis_efficiency,
        "metric.silent_bug_resistance": silent_resistance,
        "metric.prompt_robustness": prompt_robustness,
        "metric.unseen_case_generalization": (
            100.0 * hidden_passed / float(hidden_total)
        ),
    }


def project_proficiency_runtime_metrics(
    root: Path,
    unit: dict[str, Any],
    agent_dir: Path,
    trace: dict[str, Any],
) -> dict[str, Any] | None:
    """Make mechanically decidable Proficiency metrics runner-owned.

    The worker still authors the complete result document and every metric whose
    meaning requires qualitative judgment. Generation, compile/parse success,
    Correct@1, Correct@N and Test Pass Rate are facts from trusted compilation
    plus the hidden-input external oracle. Re-running paid trials because the
    worker summarized those facts incorrectly would change no experiment, so the
    runner projects these mechanically decidable cells and preserves the worker's
    original values in a separate audit record.
    """
    if str(unit.get("evaluation") or "") != "llm_proficiency":
        return None
    computed = proficiency_runtime_metrics(root, trace)
    if computed is None:
        return None
    assigned = [str(value) for value in (unit.get("assigned_languages") or [])]
    if len(assigned) != 1:
        return None
    language = assigned[0]
    result_path = agent_dir / "result.json"
    if not result_path.is_file():
        return None
    try:
        result = json_load(result_path)
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(result, dict):
        return None
    requirements = result.get("requirements")
    if not isinstance(requirements, dict):
        return None

    allowed = set(str(value) for value in (unit.get("requirement_ids") or []))
    original: dict[str, Any] = {}
    before_sha = sha256_file(result_path)
    projected: dict[str, float] = {}
    for metric, expected in computed.items():
        if metric not in allowed:
            continue
        original[metric] = requirements.get(metric)
        value = float(expected)
        requirements[metric] = {language: value}
        projected[metric] = value

    if not projected:
        return None
    json_dump(result_path, result)
    report = {
        "schema_version": 1,
        "work_unit_id": str(unit.get("id") or ""),
        "language": language,
        "source": "trusted Proficiency trial trace",
        "worker_result_sha256": before_sha,
        "projected_result_sha256": sha256_file(result_path),
        "worker_reported": original,
        "runtime_metrics": projected,
    }
    json_dump(agent_dir / "proficiency_runtime_projection.json", report)
    return report


def proficiency_primary_trial_set_sha256(root: Path) -> str:
    payload = json.dumps(
        proficiency_required_trial_ids(root),
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")
    return sha256_bytes(payload)


def proficiency_primary_prompt_set_sha256(root: Path, language: str) -> str:
    payload = {
        trial_id: sha256_bytes(
            proficiency_expected_prompt(root, language, trial_id).encode("utf-8")
        )
        for trial_id in proficiency_required_trial_ids(root)
    }
    return sha256_bytes(
        json.dumps(
            payload, sort_keys=True, separators=(",", ":"), ensure_ascii=False
        ).encode("utf-8")
    )


def proficiency_trial_coverage_problems(
    root: Path, unit: dict[str, Any], trace: dict[str, Any]
) -> list[str]:
    """Require the exact Primary cells, replications, and runtime-owned prompts."""
    expected = set(proficiency_required_trial_ids(root))
    assigned = [str(value) for value in (unit.get("assigned_languages") or [])]
    if len(assigned) != 1:
        return ["LLM Proficiency trial unit must be assigned exactly one language"]
    language = assigned[0]
    trials = ((trace.get("trials") or {}).get("trials") or {})
    if not isinstance(trials, dict):
        return ["scored trial records are not an object"]
    observed = {str(trial_id) for trial_id in trials}
    missing = sorted(expected - observed)
    extra = sorted(observed - expected)
    problems: list[str] = []
    if missing:
        problems.append("missing required Primary trials: " + ", ".join(missing))
    if extra:
        problems.append("unexpected Primary trial IDs: " + ", ".join(extra))
    if len(observed) != len(expected):
        problems.append(
            f"Primary trial count is {len(observed)}, expected exactly {len(expected)}"
        )
    for trial_id in sorted(expected & observed):
        session = trials.get(trial_id) or {}
        calls = session.get("calls") or []
        if not isinstance(calls, list) or not calls:
            problems.append(f"{trial_id}: no preserved scored calls")
            continue
        first = calls[0] if isinstance(calls[0], dict) else {}
        expected_prompt = proficiency_expected_prompt(root, language, trial_id)
        if first.get("prompt") != expected_prompt:
            problems.append(
                f"{trial_id}: initial prompt does not match the frozen runtime-owned "
                "workload/scenario prompt"
            )
        expected_hash = sha256_bytes(expected_prompt.encode("utf-8"))
        if first.get("prompt_sha256") != expected_hash:
            problems.append(
                f"{trial_id}: initial prompt hash does not match the frozen prompt"
            )
    return problems

def proficiency_runtime_verification_problems(
    root: Path,
    unit: dict[str, Any],
    agent_dir: Path,
    trace: dict[str, Any],
) -> list[str]:
    """Revalidate sanitized trial records against runner-only oracle evidence."""
    trials = ((trace.get("trials") or {}).get("trials") or {})
    problems: list[str] = []
    per_trial: dict[str, Any] = {}
    if not isinstance(trials, dict):
        return ["Proficiency runtime verification has no trial object"]
    trial_manifest = proficiency_trial_manifest(root)
    contract_sha = proficiency_workload_contract_sha256(root)
    max_repairs = int(
        json_load(root / "template" / "config" / "primary.json")
        ["llm_proficiency"].get("max_repair_turns", 0)
    )
    trusted_root = (
        root / "work" / "root" / "proficiency-verification"
        / str(unit.get("assigned_agent_id") or "")
    )

    for trial_id, summary in sorted(trials.items()):
        calls = (summary or {}).get("calls") or []
        trial_rows: list[dict[str, Any]] = []
        for index, call in enumerate(calls, start=1):
            if not isinstance(call, dict):
                problems.append(f"{trial_id}: call {index} is not an object")
                continue
            visible = call.get("verification")
            verification_path = call.get("verification_path")
            if not isinstance(visible, dict):
                problems.append(
                    f"{trial_id}: call {index} has no sanitized trusted verification"
                )
                continue
            if not isinstance(verification_path, str) or not verification_path:
                problems.append(f"{trial_id}: call {index} verification path is missing")
                continue
            try:
                path = require_under(root / verification_path, trusted_root)
            except BenchmarkError as exc:
                problems.append(
                    f"{trial_id}: call {index} invalid trusted verification path: {exc}"
                )
                continue
            if not path.is_file():
                problems.append(
                    f"{trial_id}: call {index} trusted verification file is missing"
                )
                continue
            try:
                verification = json_load(path)
            except (OSError, json.JSONDecodeError) as exc:
                problems.append(
                    f"{trial_id}: call {index} trusted verification is unreadable: {exc}"
                )
                continue
            if visible != proficiency_verification_summary(verification):
                problems.append(
                    f"{trial_id}: call {index} sanitized verification disagrees with trusted record"
                )
            if verification.get("trial_id") != trial_id:
                problems.append(f"{trial_id}: call {index} verification trial ID mismatch")
            if verification.get("source_sha256") != call.get("completion_sha256"):
                problems.append(f"{trial_id}: call {index} verification source hash mismatch")
            if verification.get("workload_contract_sha256") != contract_sha:
                problems.append(
                    f"{trial_id}: call {index} verification workload contract hash mismatch"
                )

            cell = trial_manifest.get(str(trial_id))
            if cell is None:
                problems.append(f"{trial_id}: call {index} has no frozen trial manifest row")
                expected_cases: list[dict[str, Any]] = []
            else:
                workload_name = str(cell["workload"])
                if verification.get("workload") != workload_name:
                    problems.append(
                        f"{trial_id}: call {index} verification workload mismatch"
                    )
                expected_cases = proficiency_trusted_oracle_cases(root, workload_name)

            compile_record = verification.get("compile_or_parse")
            compile_ok = (
                isinstance(compile_record, dict)
                and compile_record.get("exit_code") == 0
            )
            if verification.get("compile_parse_ok") is not compile_ok:
                problems.append(
                    f"{trial_id}: call {index} compile/parse verdict disagrees with evidence"
                )
            if int(verification.get("oracle_test_count", -1) or -1) != len(expected_cases):
                problems.append(f"{trial_id}: call {index} oracle test count mismatch")

            synthetic_call = verification.get("synthetic_ci") is True
            if synthetic_call:
                if not (
                    lexical_absolute(root) != lexical_absolute(CANONICAL_WORKSPACE)
                    and os.environ.get("QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS") == "1"
                ):
                    problems.append(
                        f"{trial_id}: call {index} synthetic verification in scored workspace"
                    )
            elif compile_ok:
                oracle_rows = verification.get("oracle_tests")
                if not isinstance(oracle_rows, list):
                    problems.append(
                        f"{trial_id}: call {index} has no trusted oracle result rows"
                    )
                    oracle_rows = []
                expected_by_id = {str(case["id"]): case for case in expected_cases}
                observed_by_id = {
                    str(row.get("id")): row
                    for row in oracle_rows
                    if isinstance(row, dict) and row.get("id") is not None
                }
                if set(observed_by_id) != set(expected_by_id):
                    problems.append(f"{trial_id}: call {index} oracle case set mismatch")
                recomputed_passed = 0
                for case_id, case in expected_by_id.items():
                    row = observed_by_id.get(case_id)
                    if row is None:
                        continue
                    if (
                        row.get("input_sha256") != case["input_sha256"]
                        or row.get("expected_sha256") != case["expected_sha256"]
                        or bool(row.get("hidden")) != bool(case["hidden"])
                    ):
                        problems.append(
                            f"{trial_id}: call {index} oracle case {case_id} hash/visibility mismatch"
                        )
                    run = row.get("run")
                    expected_problem = (
                        None
                        if isinstance(run, dict) and run.get("exit_code") == 0
                        else (
                            f"program exited with {run.get('exit_code')}"
                            if isinstance(run, dict)
                            else "missing run evidence"
                        )
                    )
                    if expected_problem is None:
                        expected_problem = _proficiency_oracle_output_problem(
                            case, str((run or {}).get("stdout") or "")
                        )
                    expected_passed = expected_problem is None
                    if bool(row.get("passed")) != expected_passed:
                        problems.append(
                            f"{trial_id}: call {index} oracle case {case_id} pass verdict mismatch"
                        )
                    if expected_passed:
                        recomputed_passed += 1
                if int(verification.get("oracle_passed_count", -1) or 0) != recomputed_passed:
                    problems.append(f"{trial_id}: call {index} oracle passed-count mismatch")
                expected_test_passed = (
                    len(expected_cases) > 0
                    and recomputed_passed == len(expected_cases)
                )
                if verification.get("test_passed") is not expected_test_passed:
                    problems.append(f"{trial_id}: call {index} final oracle verdict mismatch")
            else:
                if verification.get("test_passed") is True:
                    problems.append(
                        f"{trial_id}: call {index} cannot pass with failed compile/parse"
                    )
                if int(verification.get("oracle_passed_count", 0) or 0) != 0:
                    problems.append(
                        f"{trial_id}: call {index} records oracle passes without executable code"
                    )

            trial_rows.append({
                "call": index,
                "completion_sha256": call.get("completion_sha256"),
                "compile_parse_ok": visible.get("compile_parse_ok"),
                "test_passed": visible.get("test_passed"),
                "oracle_passed_count": visible.get("oracle_passed_count"),
                "oracle_test_count": visible.get("oracle_test_count"),
                "verification_path": verification_path,
            })
        per_trial[str(trial_id)] = trial_rows
        if calls:
            last = calls[-1] if isinstance(calls[-1], dict) else {}
            last_verification = last.get("verification")
            terminal_success = (
                isinstance(last_verification, dict)
                and last_verification.get("test_passed") is True
            )
            repairs_used = max(0, len(calls) - 1)
            if not terminal_success and repairs_used < max_repairs:
                problems.append(
                    f"{trial_id}: stopped after {repairs_used} repair(s) before "
                    f"success or the frozen {max_repairs}-repair budget was exhausted"
                )

    computed = proficiency_runtime_metrics(root, trace)
    synthetic = any(
        isinstance(call.get("verification"), dict)
        and call["verification"].get("synthetic_ci") is True
        for summary in trials.values()
        for call in ((summary or {}).get("calls") or [])
        if isinstance(call, dict)
    )
    if computed is None:
        problems.append("runtime-owned Proficiency metrics could not be recomputed")
    elif synthetic and (
        lexical_absolute(root) != lexical_absolute(CANONICAL_WORKSPACE)
        and os.environ.get("QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS") == "1"
    ):
        pass
    else:
        result_path = agent_dir / "result.json"
        if not result_path.is_file():
            problems.append("result.json is missing for runtime metric comparison")
        else:
            result = json_load(result_path)
            assigned = [str(value) for value in (unit.get("assigned_languages") or [])]
            if len(assigned) != 1:
                problems.append("runtime metric comparison requires exactly one language")
            else:
                language = assigned[0]
                requirements = result.get("requirements") or {}
                for metric, expected in computed.items():
                    value = requirements.get(metric)
                    if not isinstance(value, dict) or language not in value:
                        problems.append(f"{metric}: worker result is missing {language}")
                        continue
                    actual = score_or_na(value[language])
                    if actual is None or abs(float(actual) - expected) > 1e-6:
                        problems.append(
                            f"{metric}: worker reported {actual!r} for {language}, "
                            f"runtime evidence requires {expected:.6f}"
                        )

    audit = {
        "schema_version": 2,
        "work_unit_id": str(unit.get("id") or ""),
        "assigned_languages": list(unit.get("assigned_languages") or []),
        "workload_contract_sha256": contract_sha,
        "runtime_metrics": computed,
        "synthetic_ci": synthetic,
        "trials": per_trial,
        "passed": not problems,
        "problems": problems,
    }
    json_dump(agent_dir / "proficiency_runtime_verification.json", audit)
    return problems


def is_trial_unit(unit: dict[str, Any]) -> bool:
    evaluation = str(unit.get("evaluation") or "")
    if unit.get("execution_kind", "agent") != "agent":
        return False
    if evaluation == "llm_learnability":
        return any(str(r).startswith("condition.") for r in unit.get("requirement_ids", []))
    if evaluation == "llm_proficiency":
        return str(unit.get("id", "")).startswith("proficiency-trials--")
    return False


def trial_unit_problems(
    unit: dict[str, Any], agent_dir: Path
) -> tuple[bool, list[str]]:
    """Per-unit integrity of a scored trial unit, before it can count as COMPLETE.

    Returns (infrastructure, problems). `infrastructure` is true when the unit's
    own preflight says the assigned toolchain cannot compile and run inside the
    sandbox: retrying that at full price buys the same failure, so the unit is
    blocked as an infrastructure problem instead. Every other problem is a
    validation failure the next attempt is told about.
    """
    evaluation = str(unit.get("evaluation") or "")
    problems: list[str] = []
    infrastructure = False
    trace_path = agent_dir / "agent_trace.json"
    if not trace_path.is_file():
        return False, ["agent_trace.json is missing"]
    try:
        trace = json_load(trace_path)
    except (OSError, json.JSONDecodeError) as exc:
        return False, [f"agent_trace.json is unreadable: {exc}"]
    actions = list(trace.get("trace", []))
    if not any(x.get("action") == "trial_start" for x in actions):
        problems.append("no scored trial_start was performed")
    if evaluation == "llm_learnability":
        problems.extend(validate_learnability_attestations(agent_dir))
        preflight_path = agent_dir / "learnability_preflight.json"
        if preflight_path.is_file():
            try:
                preflight = json_load(preflight_path)
            except (OSError, json.JSONDecodeError):
                preflight = {}
            if preflight.get("fixtures_compile_and_run") is False:
                infrastructure = True
                evidence = preflight.get("evidence") or []
                first = str(evidence[0])[:400] if evidence else "no evidence recorded"
                problems.append(
                    "learnability preflight reports that the assigned toolchain cannot "
                    f"compile and run fixtures inside the sandbox: {first}"
                )
        if toolchain_evidence_required(agent_dir.parent.parent.parent):
            problems.extend(learnability_toolchain_evidence_problems(unit, trace))
    elif evaluation == "llm_proficiency":
        root = agent_dir.parent.parent.parent
        problems.extend(proficiency_trial_coverage_problems(root, unit, trace))
        problems.extend(_preserved_trial_problems(agent_dir, trace))
        if toolchain_evidence_required(root):
            problems.extend(trial_toolchain_evidence_problems(unit, trace))
        problems.extend(
            proficiency_runtime_verification_problems(root, unit, agent_dir, trace)
        )
    return infrastructure, problems


def comparability_revalidation_probes(result: dict[str, Any]) -> list[str]:
    evidence = result.get("evidence") or {}
    gate = evidence.get("gate_result") or {}
    rows = gate.get("affected_pairs_requiring_revalidation")
    if rows is None:
        rows = evidence.get("affected_pairs_requiring_revalidation")
    if not isinstance(rows, list):
        return []
    probes: list[str] = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        probe = str(row.get("probe_id") or "").upper().strip()
        if re.fullmatch(r"F\d{2}\.P\d+", probe) and probe not in probes:
            probes.append(probe)
    return probes


def comparability_repair_detail(result: dict[str, Any]) -> str:
    evidence = result.get("evidence") or {}
    gate = evidence.get("gate_result") or {}
    reason = str(gate.get("reason") or "").strip()
    if not reason:
        reason = str(evidence.get("method") or "comparability audit requested revalidation")
    return " ".join(reason.split())[:3000]


def schedule_comparability_repair(
    root: Path, unit: dict[str, Any], state: dict[str, Any]
) -> bool:
    """Repair the failed blinded audit without resetting completed measurements.

    Preferred path: apply complete pair-specific directives emitted by the
    blinded auditor. This works for any sampled probe and keeps the manifest
    frozen. Fallback path: if no direct repair was possible, re-run an existing
    cohort-adjudication unit for affected probes that already have one.
    """
    result_path = (
        root / "work" / "agents" / str(unit["assigned_agent_id"]) / "result.json"
    )
    if not result_path.is_file():
        return False
    result = json_load(result_path)
    attempts = int(state.get("attempts", 0) or 0)
    max_attempts = int(state.get("max_attempts", 3) or 3)
    if attempts >= max_attempts:
        return False

    changed = persist_comparability_repairs(root, result)
    detail = comparability_repair_detail(result)
    if changed > 0:
        archive_attempt(
            root, unit, attempts, "comparability-direct-repair",
            reset=False,
            detail=f"applied {changed} blinded pair repair(s): {detail}",
        )
        comparability_dir = root / "work" / "agents" / str(unit["assigned_agent_id"])
        if comparability_dir.exists():
            shutil.rmtree(comparability_dir)
        cmd_ledger_update(argparse.Namespace(
            workspace=str(root), id=str(unit["id"]), status="PENDING",
            evidence=[], validation_result="FAIL", blocker=None, blocker_class=None,
        ))
        print(json.dumps({
            "ok": False,
            "comparability_direct_repair_scheduled": True,
            "applied_repairs": changed,
        }, indent=2))
        return True

    # A worker may identify a disagreement but decline to invent a replacement
    # record. If the affected probe has a predeclared cohort adjudicator, give
    # that adjudicator another attempt with the audit's feedback.
    probes = comparability_revalidation_probes(result)
    if not probes:
        return False
    manifest = json_load(root / "work" / "root" / "manifest.json")
    units = {str(item["id"]): item for item in manifest.get("work_units", [])}
    ledger_path = root / "work" / "root" / "ledger.json"
    ledger = json_load(ledger_path)
    repair_units: list[dict[str, Any]] = []
    for probe in probes:
        uid = "sc-support-adjudication--" + probe.lower().replace(".", "-")
        support_unit = units.get(uid)
        support_state = (ledger.get("units") or {}).get(uid)
        if support_unit is None or support_state is None:
            continue
        if support_state.get("status") != "COMPLETE":
            continue
        if int(support_state.get("attempts", 0) or 0) >= int(
            support_state.get("max_attempts", 3) or 3
        ):
            continue
        repair_units.append(support_unit)
    if not repair_units:
        return False

    for support_unit in repair_units:
        support_state = ledger["units"][str(support_unit["id"])]
        archive_attempt(
            root, support_unit, int(support_state.get("attempts", 0) or 0),
            "comparability-revalidation", reset=True,
            detail=(
                f"Comparability requested cohort revalidation for "
                f"{support_adjudication_probe(support_unit.get('requirement_ids', []))}: "
                f"{detail}"
            ),
        )

    archive_attempt(
        root, unit, attempts, "comparability-triggered-revalidation",
        reset=False, detail=detail,
    )
    comparability_dir = root / "work" / "agents" / str(unit["assigned_agent_id"])
    if comparability_dir.exists():
        shutil.rmtree(comparability_dir)

    now = dt.datetime.now(dt.timezone.utc).isoformat()
    with ledger_lock(root):
        locked = json_load(ledger_path)
        if locked.get("manifest_sha256") != sha256_file(
            root / "work" / "root" / "manifest.json"
        ):
            raise BenchmarkError("manifest changed during comparability repair")
        for support_unit in repair_units:
            uid = str(support_unit["id"])
            current = locked["units"][uid]
            if current.get("status") != "COMPLETE":
                raise BenchmarkError(
                    f"{uid}: state changed while scheduling comparability repair"
                )
            current["status"] = "PENDING"
            current["validation_result"] = None
            current["blocker"] = None
            current["blocker_class"] = None
            current["heartbeat_at_utc"] = None
            current["updated_at_utc"] = now
        json_dump(ledger_path, locked)

    cmd_ledger_update(argparse.Namespace(
        workspace=str(root), id=str(unit["id"]), status="PENDING",
        evidence=[], validation_result="FAIL", blocker=None, blocker_class=None,
    ))
    print(json.dumps({
        "ok": False,
        "comparability_revalidation_scheduled": True,
        "support_units": [str(item["id"]) for item in repair_units],
    }, indent=2))
    return True


def cmd_task_finish(args: argparse.Namespace) -> int:
    root = workspace(args)
    unit = manifest_unit_by_id(root, args.id)
    if unit.get("execution_kind", "agent") != "agent":
        raise BenchmarkError("task-finish is only valid for agent work units")
    agent_id = str(unit["assigned_agent_id"])
    ledger_path = root / "work" / "root" / "ledger.json"
    ledger = json_load(ledger_path)
    state = ledger["units"][args.id]
    if state.get("status") != "RUNNING":
        raise BenchmarkError(f"task-finish requires RUNNING unit: {args.id}")
    worker_mode = str(unit.get("worker_mode") or "packet-only")
    agent_dir = root / "work" / "agents" / agent_id
    if worker_mode == "packet-only":
        if not (agent_dir / "worker_response.json").is_file():
            raise BenchmarkError("packet-only task-finish requires task-apply before validation")
    elif worker_mode == "sandbox-agent":
        if os.environ.get("QUIDRA_BENCHMARK_SANDBOX_AGENT_LAUNCHER_ATTESTED") != "inside-sandbox-v1":
            raise BenchmarkError("sandbox-agent task-finish requires the in-sandbox launcher attestation")
        # The packet-only path proves its provenance with worker_response.json.
        # The sandbox-agent path proves its own with the trace the in-sandbox
        # runtime writes: same frozen packet, finished deliberately, and reached
        # the model only through the credential-less gateway.
        problems = sandbox_agent_trace_problems(agent_dir, agent_id)
        if problems:
            raise BenchmarkError(
                "sandbox-agent task-finish requires a valid in-sandbox agent trace: "
                + ", ".join(problems)
            )
    else:
        raise BenchmarkError(f"unsupported worker_mode for task-finish: {worker_mode}")
    if (
        worker_mode == "sandbox-agent"
        and str(unit.get("evaluation") or "") == "llm_proficiency"
    ):
        trace_path = agent_dir / "agent_trace.json"
        if trace_path.is_file():
            project_proficiency_runtime_metrics(
                root, unit, agent_dir, json_load(trace_path)
            )
    validation_ns = argparse.Namespace(workspace=str(root), id=agent_id)
    rc = cmd_task_validate(validation_ns)
    integrity_detail: str | None = None
    if rc == 0 and worker_mode == "sandbox-agent" and is_trial_unit(unit):
        infrastructure, problems = trial_unit_problems(unit, agent_dir)
        if infrastructure:
            archive_attempt(
                root, unit, int(state.get("attempts", 0)), "toolchain-unusable-in-sandbox",
                reset=False, detail="; ".join(problems),
            )
            ns = argparse.Namespace(
                workspace=str(root), id=args.id, status="BLOCKED",
                evidence=unit.get("evidence_paths", []), validation_result="FAIL",
                blocker="assigned toolchain unusable inside the sandbox: " + "; ".join(problems),
                blocker_class="infrastructure",
            )
            cmd_ledger_update(ns)
            return 2
        if problems:
            rc = 2
            integrity_detail = "trial integrity: " + "; ".join(problems)
    if rc == 0:
        failed_gates = failed_gate_requirements(root, unit)
        if failed_gates:
            if (
                set(failed_gates) == {COMPARABILITY_GATE}
                and schedule_comparability_repair(root, unit, state)
            ):
                return 2
            ns = argparse.Namespace(
                workspace=str(root), id=args.id, status="BLOCKED",
                evidence=unit.get("evidence_paths", []), validation_result="PASS",
                blocker="required gate failed: " + ", ".join(failed_gates),
                blocker_class="scientific",
            )
            return cmd_ledger_update(ns)
        ns = argparse.Namespace(
            workspace=str(root), id=args.id, status="COMPLETE",
            evidence=unit.get("evidence_paths", []), validation_result="PASS",
            blocker=None, blocker_class=None,
        )
        return cmd_ledger_update(ns)

    attempts = int(state.get("attempts", 0))
    max_attempts = int(state.get("max_attempts", 3))
    detail = integrity_detail or _validation_detail(agent_dir)
    if attempts < max_attempts:
        archive_attempt(
            root, unit, attempts, "validation-failed-retry", reset=True, detail=detail
        )
        ns = argparse.Namespace(
            workspace=str(root), id=args.id, status="PENDING",
            evidence=unit.get("evidence_paths", []), validation_result="FAIL",
            blocker=None, blocker_class=None,
        )
        cmd_ledger_update(ns)
        print(json.dumps({
            "ok": False,
            "retry_scheduled": True,
            "work_unit_id": args.id,
            "attempts": attempts,
            "max_attempts": max_attempts,
        }, indent=2))
        return 2

    archive_attempt(
        root, unit, attempts, "validation-failed-exhausted", reset=False, detail=detail
    )
    ns = argparse.Namespace(
        workspace=str(root), id=args.id, status="BLOCKED",
        evidence=unit.get("evidence_paths", []), validation_result="FAIL",
        blocker=f"validator failed after {attempts} attempts: {detail[:600]}",
        blocker_class="ordinary-incomplete",
    )
    cmd_ledger_update(ns)
    return 2


def parse_utc(value: str) -> dt.datetime:
    parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=dt.timezone.utc)
    return parsed.astimezone(dt.timezone.utc)


def cmd_reclaim_stale(args: argparse.Namespace) -> int:
    root = workspace(args)
    primary = json_load(root / "template" / "config" / "primary.json")
    lease_seconds = int(primary.get("runner", {}).get("lease_seconds", 1800))
    ledger_path = root / "work" / "root" / "ledger.json"
    ledger = json_load(ledger_path)
    now = dt.datetime.now(dt.timezone.utc)
    reclaimed = []
    blocked = []
    for uid, state in ledger.get("units", {}).items():
        if state.get("status") != "RUNNING":
            continue
        heartbeat = state.get("heartbeat_at_utc") or state.get("updated_at_utc")
        if not heartbeat:
            continue
        age = (now - parse_utc(str(heartbeat))).total_seconds()
        if age <= lease_seconds:
            continue
        attempts = int(state.get("attempts", 0))
        max_attempts = int(state.get("max_attempts", 3))
        unit = manifest_unit_by_id(root, uid)
        if attempts < max_attempts:
            archive_attempt(root, unit, attempts, "stale-lease-retry", reset=True)
            ns = argparse.Namespace(
                workspace=str(root), id=uid, status="PENDING", evidence=[],
                validation_result="FAIL", blocker=None, blocker_class=None,
            )
            cmd_ledger_update(ns)
            reclaimed.append(uid)
        else:
            archive_attempt(
                root, unit, attempts, "stale-lease-exhausted", reset=False
            )
            ns = argparse.Namespace(
                workspace=str(root), id=uid, status="BLOCKED", evidence=[],
                validation_result="FAIL",
                blocker=f"worker heartbeat expired after {attempts} attempts",
                blocker_class="infrastructure",
            )
            cmd_ledger_update(ns)
            blocked.append(uid)
    payload = {"ok": True, "reclaimed": reclaimed, "blocked": blocked, "lease_seconds": lease_seconds}
    json_dump(root / "results" / "reclaim_stale.json", payload)
    print(json.dumps(payload, indent=2))
    return 0


def resolve_evidence_path(root: Path, raw: str) -> Path:
    """A recorded evidence path, resolved against this workspace.

    A command unit records its result by the absolute path it had inside the
    sandbox (/quidra-benchmark/...). That path is right while the run is live
    and wrong for anything that reads a finished run back from outside the
    container, so fall back to the same location under `root`.
    """
    path = Path(raw)
    if path.is_file() or not path.is_absolute():
        return path
    parts = path.parts[1:]
    if parts:
        candidate = root.joinpath(*parts[1:]) if len(parts) > 1 else root
        if candidate.is_file():
            return candidate
        candidate = root.joinpath(*parts)
        if candidate.is_file():
            return candidate
    return path


def requirement_results_for_evaluation(root: Path, evaluation: str) -> dict[str, Any]:
    manifest = json_load(root / "work" / "root" / "manifest.json")
    ledger = json_load(root / "work" / "root" / "ledger.json")
    fixed_languages = metadata_languages(root)
    merged: dict[str, Any] = {}
    for unit in manifest.get("work_units", []):
        if unit.get("evaluation") != evaluation or unit.get("phase") == "aggregation":
            continue
        execution_kind = unit.get("execution_kind", "agent")
        if execution_kind not in {"agent", "command"}:
            continue
        state = ledger.get("units", {}).get(unit["id"], {})
        if state.get("status") != "COMPLETE":
            raise BenchmarkError(f"{evaluation}: unit not complete: {unit['id']}")
        if execution_kind == "agent":
            result_path = (
                root / "work" / "agents" / unit["assigned_agent_id"] / "result.json"
            )
        else:
            evidence = list(unit.get("evidence_paths", []))
            if len(evidence) != 1:
                raise BenchmarkError(
                    f"{evaluation}: command requirement unit must have one result file"
                )
            result_path = resolve_evidence_path(root, evidence[0])
        result = json_load(result_path)
        if unit.get("result_kind") == "audit":
            if result.get("audit_pass") is not True:
                raise BenchmarkError(
                    f"{evaluation}: reusable audit failed: {unit['id']}"
                )
            continue
        for rid, value in result.get("requirements", {}).items():
            if rid.startswith(CANONICAL_FRAGMENT_PREFIX):
                continue
            if rid.startswith("metric.") or rid.startswith("condition."):
                if not isinstance(value, dict):
                    raise BenchmarkError(f"{evaluation}: {rid} must be a language map")
                bucket = merged.setdefault(rid, {})
                if not isinstance(bucket, dict):
                    raise BenchmarkError(f"{evaluation}: mixed result types for {rid}")
                overlap = sorted(set(bucket) & set(value))
                if overlap:
                    raise BenchmarkError(
                        f"{evaluation}: overlapping language shards for {rid}: {overlap}"
                    )
                bucket.update(value)
            else:
                if rid in merged:
                    raise BenchmarkError(
                        f"{evaluation}: duplicate requirement result: {rid}"
                    )
                merged[rid] = value

    _requirements_path, required = load_evaluation_requirements(root, evaluation)
    missing_requirements = sorted(set(required) - set(merged))
    if missing_requirements:
        raise BenchmarkError(
            f"{evaluation}: missing completed requirement results: "
            + ", ".join(missing_requirements)
        )
    for rid in required:
        if rid.startswith("metric.") or rid.startswith("condition."):
            value = merged[rid]
            if set(value) != set(fixed_languages):
                missing = sorted(set(fixed_languages) - set(value))
                extra = sorted(set(value) - set(fixed_languages))
                raise BenchmarkError(
                    f"{evaluation}: incomplete language shards for {rid}; "
                    f"missing={missing}, extra={extra}"
                )
    return merged


def weighted_score(weights: dict[str, float], req: dict[str, Any], language: str) -> float | None:
    numerator = 0.0
    denominator = 0.0
    for rid, weight in weights.items():
        if rid not in req:
            raise BenchmarkError(f"missing requirement result: {rid}")
        score = score_or_na(req[rid][language])
        if score is None:
            continue
        numerator += float(weight) * score
        denominator += float(weight)
    if denominator == 0:
        return None
    return numerator / denominator


def category_score(categories: dict[str, Any], req: dict[str, Any], language: str) -> float | None:
    total = 0.0
    total_weight = 0.0
    for category in categories.values():
        metric_scores = []
        for rid in category["metrics"]:
            if rid not in req:
                raise BenchmarkError(f"missing requirement result: {rid}")
            score = score_or_na(req[rid][language])
            if score is not None:
                metric_scores.append(score)
        if not metric_scores:
            continue
        value = sum(metric_scores) / len(metric_scores)
        weight = float(category["weight"])
        total += weight * value
        total_weight += weight
    if total_weight == 0:
        return None
    return total / total_weight


def deterministic_ranking(scores: dict[str, float], language_order: list[str]) -> list[dict[str, Any]]:
    """Rank only at the precision the benchmark actually publishes.

    Comparing hidden floating-point dust while displaying two decimals can print
    identical scores with different ranks. Round once to the frozen publication
    precision, sort those published values, and give identical published scores
    the same competition rank.
    """
    order_index = {name: i for i, name in enumerate(language_order)}
    published = {name: round(float(score), 2) for name, score in scores.items()}
    ordered = sorted(
        published.items(), key=lambda kv: (-kv[1], order_index[kv[0]])
    )
    ranking = []
    previous_score = None
    previous_rank = 0
    for index, (language, score) in enumerate(ordered, start=1):
        rank = previous_rank if previous_score is not None and score == previous_score else index
        ranking.append({"rank": rank, "language": language, "score": score})
        previous_score = score
        previous_rank = rank
    return ranking


SC_PROBE_ID = re.compile(r"^F\d\d\.P\d$")


def sc_probe_rows(evidence: Any) -> dict[str, list[Any]]:
    """Every per-probe row a metric shard recorded, by probe ID.

    Ten shards wrote ten shapes for the same metric - a list of objects with
    `probe_id`, a list keyed by `probe`, a mapping from probe ID to a number,
    to `{"L": 0, "rationale": ...}`, or to a category vector with a `total` -
    so this collects the rows wherever they are and leaves the reading of them
    to `sc_row_number`.
    """
    rows: dict[str, list[Any]] = collections.defaultdict(list)

    def walk(node: Any) -> None:
        if isinstance(node, dict):
            for key, value in node.items():
                if SC_PROBE_ID.match(str(key)):
                    rows[str(key)].append(value)
                else:
                    walk(value)
        elif isinstance(node, list):
            for item in node:
                probe = ""
                if isinstance(item, dict):
                    probe = str(item.get("probe_id") or item.get("probe") or "")
                if SC_PROBE_ID.match(probe):
                    rows[probe].append(
                        {k: v for k, v in item.items() if k not in ("probe_id", "probe")}
                    )
                else:
                    walk(item)

    walk(evidence)
    return rows


def sc_row_number(value: Any, names: Iterable[str]) -> float | None:
    """The one raw number a per-probe row records for a metric."""
    names = [str(name).lower() for name in names]
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, dict):
        numeric = {
            str(key).lower(): float(item)
            for key, item in value.items()
            if isinstance(item, (int, float)) and not isinstance(item, bool)
        }
        for key in numeric:
            if key in names:
                return numeric[key]
        for key in numeric:
            if any(name in key for name in names):
                return numeric[key]
        for nested_key, nested in value.items():
            if isinstance(nested, dict) and any(
                name in str(nested_key).lower() for name in names
            ):
                found = sc_row_number(nested, names + ["total"])
                if found is not None:
                    return found
        for generic in ("total", "count", "value", "units", "sum", "n"):
            if generic in numeric:
                return numeric[generic]
        if len(numeric) == 1:
            return next(iter(numeric.values()))
        for key, item in value.items():
            if isinstance(item, list) and any(
                hint in str(key).lower()
                for hint in ("triggered", "categories", "hops", "lookups")
            ):
                return float(len(item))
        return None
    if isinstance(value, list):
        return float(len(value))
    return None


def sc_per_probe(evidence: Any, names: Iterable[str]) -> dict[str, float]:
    """probe ID -> the raw number this shard recorded for it."""
    out: dict[str, float] = {}
    for probe, entries in sc_probe_rows(evidence).items():
        for entry in entries:
            found = sc_row_number(entry, names)
            if found is not None:
                out[probe] = found
                break
    return out


def sc_language_ratio(
    evidence: Any,
    spec: dict[str, Any],
    *,
    denominator_override: float | None = None,
) -> float | None:
    """A whole-universe ratio, optionally with a trusted runner-owned denominator."""
    raw_patterns = [re.compile(p, re.I) for p in spec.get("raw_patterns", [])]
    numerator = re.compile(spec["numerator_pattern"], re.I)
    denominator = re.compile(spec["denominator_pattern"], re.I)
    found: dict[str, float] = {}

    def walk(node: Any) -> None:
        if isinstance(node, dict):
            for key, value in node.items():
                name = str(key)
                if isinstance(value, (int, float)) and not isinstance(value, bool):
                    if "raw" not in found and any(p.search(name) for p in raw_patterns):
                        found["raw"] = float(value)
                    if "num" not in found and numerator.search(name):
                        found["num"] = float(value)
                    if "den" not in found and denominator.search(name):
                        found["den"] = float(value)
                elif isinstance(value, dict) and "raw" not in found and any(
                    p.search(name) for p in raw_patterns
                ):
                    for inner, item in value.items():
                        if str(inner).lower() in ("value", "raw_value", "computed_value") \
                                and isinstance(item, (int, float)):
                            found["raw"] = float(item)
                            break
                walk(value)
        elif isinstance(node, list):
            for item in node:
                walk(item)

    walk(evidence)
    if denominator_override is not None:
        if denominator_override <= 0 or "num" not in found:
            return None
        return found["num"] / denominator_override
    if "raw" in found:
        return found["raw"]
    if found.get("den"):
        return found.get("num", 0.0) / found["den"]
    return None


def sc_raw_values(
    root: Path, config: dict[str, Any], languages: list[str]
) -> dict[str, dict[str, float]]:
    """Each quality metric's raw value per language, on the common basis."""
    spec = config["recompute_from_evidence"]
    manifest = json_load(root / "work" / "root" / "manifest.json")
    shards: dict[tuple[str, str], Path] = {}
    for unit in manifest.get("work_units", []):
        assigned = list(unit.get("assigned_languages") or [])
        requirements = list(unit.get("requirement_ids") or [])
        if len(assigned) != 1 or len(requirements) != 1:
            continue
        result = (
            root / "work" / "agents" / str(unit.get("assigned_agent_id")) / "result.json"
        )
        if result.is_file():
            shards[(requirements[0], assigned[0])] = result

    supported_points = sc_supported_capability_points(root, config, languages)
    raw: dict[str, dict[str, float]] = {}
    for metric, rule in spec["metrics"].items():
        evidence_by_language: dict[str, Any] = {}
        for language in languages:
            path = shards.get((metric, language))
            if path is None:
                raise BenchmarkError(
                    f"{metric}: no completed shard for {language} to recompute from"
                )
            block = (json_load(path).get("evidence") or {})
            evidence_by_language[language] = block.get(metric, block)

        if rule["reduce"] == "language_ratio":
            values = {}
            for language, evidence in evidence_by_language.items():
                denominator_override = None
                if metric == "metric.capability_efficiency":
                    if supported_points is None:
                        raise BenchmarkError(
                            "metric.capability_efficiency: final supported capability "
                            "points could not be derived from the authoritative support ledger"
                        )
                    by_language, _ = supported_points
                    denominator_override = by_language.get(language)
                value = sc_language_ratio(
                    evidence, rule, denominator_override=denominator_override
                )
                if value is None:
                    detail = (
                        "semantic-complexity numerator and final supported capability points"
                        if metric == "metric.capability_efficiency"
                        else "raw value"
                    )
                    raise BenchmarkError(
                        f"{metric}: {language} recorded no {detail} to recompute from"
                    )
                values[language] = value
            raw[metric] = values
            continue

        if rule["reduce"] == "ratio":
            numerators = {
                language: sc_per_probe(evidence, rule["numerator"])
                for language, evidence in evidence_by_language.items()
            }
            denominators = {
                language: sc_per_probe(evidence, rule["denominator"])
                for language, evidence in evidence_by_language.items()
            }
            basis = set.intersection(*(
                set(numerators[l]) & set(denominators[l]) for l in languages
            ))
            if not basis:
                raise BenchmarkError(f"{metric}: the ten languages share no probe")
            values = {}
            for language in languages:
                top = sum(numerators[language][p] for p in basis)
                bottom = sum(denominators[language][p] for p in basis)
                if bottom <= 0:
                    raise BenchmarkError(f"{metric}: {language} counted no tokens")
                values[language] = top / bottom
            raw[metric] = values
            continue

        per_language = {
            language: sc_per_probe(evidence, rule["fields"])
            for language, evidence in evidence_by_language.items()
        }
        basis = set.intersection(*(set(per_language[l]) for l in languages))
        if not basis:
            raise BenchmarkError(f"{metric}: the ten languages share no probe")
        values = {}
        for language in languages:
            numbers = [per_language[language][p] for p in basis]
            if rule["reduce"] == "mean_log2":
                numbers = [math.log2(max(value, 1.0)) for value in numbers]
            values[language] = sum(numbers) / len(numbers)
        raw[metric] = values
    return raw


def sc_normalize(raw: dict[str, float], direction: str) -> dict[str, float]:
    """Map raw values onto 0-100 by where each language sits between the extremes.

    The methodology fixes the raw value and which end is better, and leaves the
    scale open; a shard that can see one language cannot close it. Min-max over
    the ten languages needs no invented constant, is reproducible from the
    evidence, and is what the shards themselves asked the runner for.
    """
    low = min(raw.values())
    high = max(raw.values())
    if high == low:
        return {language: 100.0 for language in raw}
    span = high - low
    return {
        language: round(
            100.0 * ((value - low) if direction == "higher_is_better" else (high - value))
            / span,
            2,
        )
        for language, value in raw.items()
    }



def sc_supported_capability_points(
    root: Path, config: dict[str, Any], languages: list[str]
) -> tuple[dict[str, float], float] | None:
    """Final supported capability points after optional cohort adjudication."""
    owner = config.get("support_level_owner") or {}
    factors = {
        str(name).upper(): float(value)
        for name, value in (owner.get("levels") or {}).items()
    }
    matrix = json_load(
        root / "template" / "methodology-assets" / "semantic_compression"
        / "semantic_site_matrix.json"
    )
    probes = {
        str(probe["probe_id"]): float(probe.get("capability_denominator") or 0)
        for probe in (matrix.get("probes") or [])
    }
    total = sum(probes.values())
    if not probes or total <= 0:
        return None

    adjudicated = sc_adjudicated_levels(root)
    awarded_by_language: dict[str, float] = {}
    for language in languages:
        awarded = 0.0
        for probe_id, points in probes.items():
            settled = (adjudicated.get(probe_id) or {}).get(language)
            if settled:
                level = settled
            else:
                level = str(
                    canonical_owner_record_for_probe(root, language, probe_id)["level"]
                ).upper()
            if level not in factors:
                return None
            awarded += points * factors[level]
        awarded_by_language[language] = awarded
    return awarded_by_language, total

def sc_coverage_from_support(
    root: Path, config: dict[str, Any], languages: list[str]
) -> dict[str, float] | None:
    """Capability Coverage as the frozen award_formula makes it: from the levels.

    A shard computes its own coverage from the levels it assigned, so once a
    support adjudication settles a level across the cohort the published
    coverage and the levels the comparability audit judged are two statements
    that can disagree. Deriving coverage from the levels keeps them one.

    This invents nothing: on the run it was written against it reproduced every
    shard's own reported coverage to the cent for all ten languages. Returns
    None - leaving the reported values alone - unless every frozen probe has a
    determinate level in every language, because a coverage computed from a
    partial ledger would be worse than the one the shards reported.
    """
    rule = config.get("coverage_from_support") or {}
    if not rule:
        return None
    settled = sc_supported_capability_points(root, config, languages)
    if settled is None:
        return None
    awarded_by_language, total = settled
    return {
        language: 100.0 * awarded / total
        for language, awarded in awarded_by_language.items()
    }


def sc_recomputed_requirements(
    root: Path, config: dict[str, Any], req: dict[str, Any], languages: list[str]
) -> dict[str, Any]:
    """Replace the shards' self-normalized metric values with comparable ones."""
    spec = config["recompute_from_evidence"]
    try:
        raw = sc_raw_values(root, config, languages)
    except BenchmarkError as exc:
        # A run whose ten shards are not all COMPLETE has nothing to put on a
        # common basis. It cannot be scored either, so leave the requirement
        # values alone and let the aggregate reach its ordinary PARTIAL rather
        # than turning an unscoreable evaluation into a failed run.
        json_dump(
            root / "results" / "semantic_compression_recomputation.json",
            {"schema_version": 1, "recomputed": False, "reason": str(exc)},
        )
        return req
    recomputed = dict(req)
    audit: dict[str, Any] = {}
    coverage = sc_coverage_from_support(root, config, languages)
    if coverage is not None:
        metric = str(config["coverage_metric"])
        reported = req.get(metric) or {}
        audit[metric] = {
            "derived_from": "support levels via the frozen award_formula",
            "reported": {
                language: reported.get(language) for language in sorted(coverage)
            },
            "derived": {
                language: round(value, 2) for language, value in sorted(coverage.items())
            },
        }
        recomputed[metric] = {
            language: round(value, 2) for language, value in coverage.items()
        }
    for metric, rule in spec["metrics"].items():
        scores = sc_normalize(raw[metric], rule["direction"])
        recomputed[metric] = scores
        audit[metric] = {
            "direction": rule["direction"],
            "raw": {language: round(value, 6) for language, value in raw[metric].items()},
            "normalized": scores,
        }
    json_dump(
        root / "results" / "semantic_compression_recomputation.json",
        {
            "schema_version": 1,
            "recomputed": True,
            "normalization": spec["normalization"],
            "common_basis": spec["common_basis"],
            "metrics": audit,
        },
    )
    return recomputed


def cmd_aggregate_primary(args: argparse.Namespace) -> int:
    root = workspace(args)
    evaluation = args.evaluation
    req = requirement_results_for_evaluation(root, evaluation)
    aggregation = json_load(root / "template" / "config" / "aggregation.json")
    languages = metadata_languages(root)
    config = aggregation["evaluations"][evaluation]
    if config.get("recompute_from_evidence"):
        req = sc_recomputed_requirements(root, config, req, languages)

    failed_gates = [
        rid for rid, value in req.items()
        if (rid.startswith("gate.") or rid.startswith("coverage.")) and value is False
    ]
    out_path = root / "results" / "evaluations" / f"{evaluation}.json"
    if failed_gates:
        manifest = json_load(root / "work" / "root" / "manifest.json")
        served_by = {}
        for unit in manifest.get("work_units", []):
            if unit.get("evaluation") != evaluation:
                continue
            for rid in unit.get("requirement_ids", []):
                served_by.setdefault(rid, str(unit["id"]))
        result = {
            "schema_version": 1,
            "evaluation": evaluation,
            "status": "WITHDRAWN",
            "scoreable": False,
            "score": None,
            "scores": None,
            "ranking": None,
            "blocker_class": (
                "infrastructure"
                if any("infrastructure" in rid for rid in failed_gates)
                else "scientific"
            ),
            "blockers": [
                {
                    "work_unit_id": served_by.get(rid, f"{evaluation}-aggregate"),
                    "requirement_id": rid,
                    "reason": "required gate returned false",
                }
                for rid in failed_gates
            ],
        }
        json_dump(out_path, result)
        print(json.dumps(result, indent=2))
        return 0

    scores: dict[str, float] = {}
    for language in languages:
        typ = config["type"]
        if typ == "weighted_mean":
            score = weighted_score(config["weights"], req, language)
        elif typ == "category_mean":
            score = category_score(config["categories"], req, language)
        elif typ == "semantic_harmonic":
            quality = weighted_score(config["quality_weights"], req, language)
            coverage = score_or_na(req[config["coverage_metric"]][language])
            if quality is None or coverage is None:
                score = None
            elif quality + coverage == 0:
                score = 0.0
            else:
                score = 2.0 * quality * coverage / (quality + coverage)
        else:
            raise BenchmarkError(f"unknown aggregation type: {typ}")
        if score is None:
            raise BenchmarkError(
                f"{evaluation}: cannot produce Primary score for {language}; "
                "all applicable inputs for a required aggregate are N/A"
            )
        scores[language] = float(score)

    ranking = deterministic_ranking(scores, languages)
    result = {
        "schema_version": 1,
        "evaluation": evaluation,
        "status": "COMPLETE",
        "scoreable": True,
        "score": scores["Quidra"],
        "scores": scores,
        "ranking": ranking,
        "ranking_basis": {
            "precision_decimals": 2,
            "tie_policy": "competition rank on published score",
            "interpretation": (
                "descriptive ranking of the frozen Primary sample; it does not "
                "claim statistical superiority beyond the measured sample"
            ),
        },
        "blocker_class": None,
        "blockers": [],
    }
    json_dump(out_path, result)
    print(json.dumps(result, indent=2))
    return 0


def cmd_aggregate_check(args: argparse.Namespace) -> int:
    root = workspace(args)
    path = Path(args.file)
    if not path.is_absolute():
        path = root / path
    path = require_under(path, root)
    data = json_load(path)
    if data.get("evaluation") != args.evaluation:
        raise BenchmarkError("aggregate evaluation mismatch")
    status = data.get("status")
    if status == "COMPLETE":
        languages = metadata_languages(root)
        scores = data.get("scores")
        ranking = data.get("ranking")
        if not isinstance(scores, dict) or set(scores) != set(languages):
            raise BenchmarkError("COMPLETE aggregate requires scores for all fixed languages")
        if not isinstance(ranking, list) or len(ranking) != len(languages):
            raise BenchmarkError("COMPLETE aggregate requires full runner-generated ranking")
        for language, score in scores.items():
            if not isinstance(score, (int, float)) or isinstance(score, bool):
                raise BenchmarkError(f"aggregate score must be numeric: {language}")
            if not (0.0 <= float(score) <= 100.0):
                raise BenchmarkError(f"aggregate score outside 0..100: {language}")
        expected_ranking = deterministic_ranking(
            {language: float(scores[language]) for language in languages},
            languages,
        )
        if ranking != expected_ranking:
            raise BenchmarkError("published ranking does not match runner recomputation")
        if data.get("ranking_basis") != {
            "precision_decimals": 2,
            "tie_policy": "competition rank on published score",
            "interpretation": (
                "descriptive ranking of the frozen Primary sample; it does not "
                "claim statistical superiority beyond the measured sample"
            ),
        }:
            raise BenchmarkError("published ranking basis does not match runner policy")
        if data.get("score") != scores["Quidra"]:
            raise BenchmarkError("Primary score field must equal the Quidra language score")
    elif status == "WITHDRAWN":
        if data.get("score") is not None or data.get("ranking") is not None:
            raise BenchmarkError("WITHDRAWN aggregate cannot contain score/ranking")
        if not data.get("blockers"):
            raise BenchmarkError("WITHDRAWN aggregate requires blockers")
    else:
        raise BenchmarkError(f"invalid aggregate status: {status}")
    print(json.dumps({"ok": True, "evaluation": args.evaluation, "path": str(path)}, indent=2))
    return 0


def current_primary_status(root: Path) -> dict[str, Any]:
    manifest = json_load(root / "work" / "root" / "manifest.json")
    ledger = json_load(root / "work" / "root" / "ledger.json")
    evaluations: dict[str, Any] = {}
    for evaluation in PRIMARY_NAMES:
        result_path = root / "results" / "evaluations" / f"{evaluation}.json"
        if result_path.is_file():
            data = json_load(result_path)
            evaluations[evaluation] = {
                "status": data.get("status"),
                "scoreable": data.get("status") == "COMPLETE",
                "score": data.get("score"),
                "scores": data.get("scores"),
                "ranking": data.get("ranking"),
                "ranking_basis": data.get("ranking_basis"),
                "blockers": data.get("blockers", []),
                "blocker_class": data.get("blocker_class"),
            }
            continue

        units = [u for u in manifest.get("work_units", []) if u.get("evaluation") == evaluation]
        bad = []
        active = []
        for unit in units:
            state = ledger.get("units", {}).get(unit["id"], {})
            st = state.get("status", "PENDING")
            if st in {"BLOCKED", "INVALID"}:
                bad.append({
                    "work_unit_id": unit["id"],
                    "reason": state.get("blocker") or st.lower(),
                })
            elif st != "COMPLETE":
                active.append({
                    "work_unit_id": unit["id"],
                    "reason": f"work unit is {st}",
                })
        blockers = bad or active
        bad_classes = [
            ledger.get("units", {}).get(b["work_unit_id"], {}).get("blocker_class")
            for b in bad
        ]
        if "infrastructure" in bad_classes:
            blocker_class = "infrastructure"
        elif "scientific" in bad_classes:
            blocker_class = "scientific"
        elif "budget-plan-defect" in bad_classes:
            blocker_class = "budget-plan-defect"
        else:
            blocker_class = "ordinary-incomplete"
        evaluations[evaluation] = {
            "status": "PARTIAL" if blockers else "NOT_EXECUTED",
            "scoreable": False,
            "score": None,
            "scores": None,
            "ranking": None,
            "blockers": blockers or [
                {"work_unit_id": f"{evaluation}-aggregate", "reason": "not executed"}
            ],
            "blocker_class": blocker_class,
        }
    return {
        "schema_version": 1,
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "evaluations": evaluations,
    }


def cmd_primary_status_derive(args: argparse.Namespace) -> int:
    root = workspace(args)
    data = current_primary_status(root)
    out = root / "results" / "primary_status.json"
    json_dump(out, data)
    errors, _ = validate_primary_status(data)
    if errors:
        raise BenchmarkError("derived Primary status invalid: " + "; ".join(errors))
    print(json.dumps({"ok": True, "path": str(out)}, indent=2))
    return 0



LEARNABILITY_PREFLIGHT_FIELDS = (
    "fixtures_compile_and_run",
    "harness_conventions_satisfied",
    "validator_positive_control_passed",
    "validator_negative_control_passed",
)
LEARNABILITY_LEAKAGE_FIELDS = (
    "exact_solution_absent",
    "expected_output_not_leaked",
    "isomorphic_example_absent",
    "withheld_mapping_absent",
    "validator_answer_absent",
    "metadata_leak_absent",
    "planted_leak_positive_control_passed",
)


def validate_learnability_attestations(agent_dir: Path) -> list[str]:
    problems: list[str] = []
    specs = (
        ("learnability_preflight.json", LEARNABILITY_PREFLIGHT_FIELDS),
        ("learnability_leakage.json", LEARNABILITY_LEAKAGE_FIELDS),
    )
    for filename, fields in specs:
        path = agent_dir / filename
        if not path.is_file():
            problems.append(f"{filename} is missing")
            continue
        try:
            payload = json_load(path)
        except (OSError, json.JSONDecodeError) as exc:
            problems.append(f"{filename} is unreadable: {exc}")
            continue
        if payload.get("schema_version") != 1:
            problems.append(f"{filename} has unsupported schema_version")
        if payload.get("passed") is not True:
            problems.append(f"{filename} does not attest passed=true")
        for field in fields:
            if payload.get(field) is not True:
                problems.append(f"{filename} does not attest {field}=true")
        evidence = payload.get("evidence")
        if not isinstance(evidence, list) or not evidence:
            problems.append(f"{filename} requires non-empty evidence")
    return problems


def _command_result_path(root: Path, unit: dict[str, Any]) -> Path:
    evidence = list(unit.get("evidence_paths", []))
    if len(evidence) != 1:
        raise BenchmarkError(f"{unit['id']}: command requirement unit needs one result path")
    return Path(evidence[0])


def _write_command_requirements(
    root: Path, unit: dict[str, Any], requirements: dict[str, Any], evidence: dict[str, Any]
) -> None:
    json_dump(_command_result_path(root, unit), {
        "schema_version": 1,
        "evaluation": unit["evaluation"],
        "requirements": requirements,
        "evidence": evidence,
    })



def semantic_site_matrix_problems(
    matrix: dict[str, Any], universe: dict[str, Any], languages: list[str]
) -> list[str]:
    problems: list[str] = []
    if matrix.get("schema_version") != 1:
        problems.append("schema_version must be 1")
    if matrix.get("frozen") is not True:
        problems.append("matrix must be frozen")
    if list(matrix.get("languages", [])) != languages:
        problems.append("matrix language order differs from benchmark metadata")
    allowed_states = list(matrix.get("annotation_states", []))
    if allowed_states != ["UNMEASURED", "SUPPORTED", "PARTIAL", "UNSUPPORTED"]:
        problems.append("annotation states are not the frozen four-state set")
    metric_ids = list(matrix.get("metrics", []))
    required_metrics = [
        "semantic_density",
        "semantic_determinacy",
        "semantic_locality",
        "hidden_semantic_cost",
        "capability_efficiency",
    ]
    if metric_ids != required_metrics:
        problems.append("matrix metric mapping is not the frozen five-metric order")

    universe_probes = list(universe.get("probes", []))
    matrix_probes = list(matrix.get("probes", []))
    if len(matrix_probes) != len(universe_probes):
        problems.append("matrix probe count differs from capability universe")
        return problems

    fact_kinds = universe.get("semantic_fact_kinds") or {}
    for expected, actual in zip(universe_probes, matrix_probes):
        probe_id = str(expected.get("probe_id", ""))
        if actual.get("probe_id") != probe_id:
            problems.append(f"{probe_id}: probe id/order mismatch")
            continue
        if actual.get("family") != expected.get("family"):
            problems.append(f"{probe_id}: capability family mismatch")
        if int(actual.get("capability_denominator", -1)) != int(
            expected.get("capability_points", -2)
        ):
            problems.append(f"{probe_id}: capability denominator mismatch")
        expected_facts = [str(x) for x in expected.get("semantic_facts_expected", [])]
        expected_sites = [f"{probe_id}:{fact}" for fact in expected_facts]
        sites = list(actual.get("sites", []))
        actual_sites = [str(site.get("site_id", "")) for site in sites]
        if actual_sites != expected_sites:
            problems.append(f"{probe_id}: site id set/order mismatch")
        unsupported = actual.get("unsupported_rule")
        if not isinstance(unsupported, str) or not unsupported:
            problems.append(f"{probe_id}: missing unsupported rule")
        for fact, site in zip(expected_facts, sites):
            if site.get("semantic_fact") != fact:
                problems.append(f"{probe_id}: semantic fact mismatch for {fact}")
            if site.get("semantic_question") != fact_kinds.get(fact):
                problems.append(f"{probe_id}: semantic question mismatch for {fact}")
            if int(site.get("multiplicity", -1)) != 1:
                problems.append(f"{probe_id}: multiplicity must be exactly one")
            if list(site.get("metrics", [])) != required_metrics:
                problems.append(f"{probe_id}: metric mapping differs for {fact}")
            if site.get("unsupported_rule") != unsupported:
                problems.append(f"{probe_id}: unsupported rule is inconsistent")
            if not str(site.get("inclusion_criterion", "")).strip():
                problems.append(f"{probe_id}: missing inclusion criterion")
            if not str(site.get("exclusion_criterion", "")).strip():
                problems.append(f"{probe_id}: missing exclusion criterion")
        templates = actual.get("language_templates") or {}
        if list(templates.keys()) != languages:
            problems.append(f"{probe_id}: language template set/order mismatch")
        for language in languages:
            row = templates.get(language) or {}
            if list(row.get("site_order", [])) != expected_sites:
                problems.append(f"{probe_id}: {language} site order mismatch")
            if row.get("default_state") != "UNMEASURED":
                problems.append(f"{probe_id}: {language} default state is invalid")
    return problems


def semantic_site_matrix_negative_self_tests(
    matrix: dict[str, Any], universe: dict[str, Any], languages: list[str]
) -> dict[str, bool]:
    def clone() -> dict[str, Any]:
        return json.loads(json.dumps(matrix))

    tests: dict[str, bool] = {}

    bad = clone()
    order = bad["probes"][0]["language_templates"][languages[0]]["site_order"]
    order[0], order[1] = order[1], order[0]
    tests["reordered_site_rejected"] = bool(
        semantic_site_matrix_problems(bad, universe, languages)
    )

    bad = clone()
    bad["probes"][0]["language_templates"][languages[0]]["site_order"].append(
        "F00.P0:language_only"
    )
    tests["language_only_site_rejected"] = bool(
        semantic_site_matrix_problems(bad, universe, languages)
    )

    bad = clone()
    bad["probes"][0]["language_templates"][languages[0]]["site_order"].pop()
    tests["missing_site_rejected"] = bool(
        semantic_site_matrix_problems(bad, universe, languages)
    )

    bad = clone()
    bad["probes"][0]["sites"][0]["multiplicity"] = 2
    tests["multiplicity_change_rejected"] = bool(
        semantic_site_matrix_problems(bad, universe, languages)
    )

    bad = clone()
    bad["probes"][0]["sites"][0]["metrics"] = ["semantic_density"]
    tests["metric_mapping_change_rejected"] = bool(
        semantic_site_matrix_problems(bad, universe, languages)
    )

    bad = clone()
    bad["probes"][0]["capability_denominator"] += 1
    tests["capability_denominator_change_rejected"] = bool(
        semantic_site_matrix_problems(bad, universe, languages)
    )

    bad = clone()
    bad["probes"][0]["sites"][0]["unsupported_rule"] = "inconsistent"
    tests["inconsistent_unsupported_rule_rejected"] = bool(
        semantic_site_matrix_problems(bad, universe, languages)
    )

    bad = clone()
    bad["probes"][0]["language_templates"][languages[0]]["default_state"] = "SUPPORTED"
    tests["premeasurement_state_change_rejected"] = bool(
        semantic_site_matrix_problems(bad, universe, languages)
    )

    return tests


def run_static_coverage(root: Path, unit: dict[str, Any]) -> None:
    metadata = json_load(root / "template" / "config" / "benchmark_metadata.json")
    languages = list(metadata.get("languages", []))
    fixed_10 = (
        len(languages) == 10
        and len(set(languages)) == 10
        and metadata.get("evaluated_target_language") == "Quidra"
        and "Quidra" in languages
    )
    requirements: dict[str, Any] = {}
    evidence: dict[str, Any] = {
        "languages": languages,
        "language_count": len(languages),
    }
    for rid in unit.get("requirement_ids", []):
        if rid == "coverage.all_10_languages":
            requirements[rid] = fixed_10
        elif rid == "gate.proficiency_workload_contract":
            asset = proficiency_workload_contract(root)
            trial_manifest = proficiency_trial_manifest(root)
            reference_self_test = proficiency_reference_self_test(root)
            prompt_hashes = {
                language: proficiency_primary_prompt_set_sha256(root, language)
                for language in languages
            }
            requirements[rid] = (
                fixed_10
                and len(trial_manifest)
                == (
                    len(asset["workloads"])
                    * len(asset["scenarios"])
                    * int(
                        json_load(root / "template" / "config" / "primary.json")
                        ["llm_proficiency"]["independent_trials_per_replicated_cell"]
                    )
                )
                and len(set(prompt_hashes.values())) == len(languages)
                and reference_self_test["passed"]
            )
            evidence.update({
                "proficiency_workload_document_id": asset.get("document_id"),
                "proficiency_workloads": sorted(asset["workloads"]),
                "proficiency_scenarios": sorted(asset["scenarios"]),
                "proficiency_trial_count": len(trial_manifest),
                "proficiency_prompt_set_sha256_by_language": prompt_hashes,
                "proficiency_source_commits": {
                    workload: row.get("commit")
                    for workload, row in sorted(
                        (asset.get("source_provenance") or {}).items()
                    )
                },
                "proficiency_reference_self_test": reference_self_test,
                "proficiency_workload_contract_sha256": (
                    proficiency_workload_contract_sha256(root)
                ),
            })
        elif rid == "gate.capability_universe":
            asset = json_load(
                root / "template" / "methodology-assets" / "semantic_compression"
                / "capability_universe.json"
            )
            environment = json_load(root / "template" / "environment" / "environment.json")
            probes = list(asset.get("probes", []))
            probe_ids = [str(p.get("probe_id", "")) for p in probes]
            families = {str(p.get("family", "")) for p in probes}
            facts = set((asset.get("semantic_fact_kinds") or {}).keys())
            fact_refs = {
                str(fact)
                for probe in probes
                for fact in (probe.get("semantic_facts_expected") or [])
            }
            recipes = (asset.get("toolchain_binding") or {}).get("recipes") or {}
            frozen_recipes = environment.get("frozen_toolchain_recipes") or {}
            verification_recipe_drift = semantic_verification_recipe_drift_problems(root)
            p_a_allowed = set(
                (asset.get("support_rubric") or {}).get(
                    "partial_p_a_allowed_probe_ids"
                ) or []
            )
            r9_text = str(
                (asset.get("authoring_rules_for_probe_fragments") or {}).get(
                    "R9_no_probe_substitution", ""
                )
            )
            named_substitution_text = str(
                (asset.get("support_rubric") or {})
                .get("deterministic_tie_break", {})
                .get("named_substitution", "")
            )
            r9_mentions = set(re.findall(r"F\d{2}\.P\d+", r9_text))
            tie_break_named = set(
                re.findall(r"F\d{2}\.P\d+", named_substitution_text)
            )
            ok = (
                asset.get("frozen") is True
                and int(asset.get("probe_count", -1)) == len(probes)
                and int(asset.get("family_count", -1)) == len(families)
                and int(asset.get("total_fixed_capability_points", -1)) == 2 * len(probes)
                and len(probes) > 0
                and all(probe_ids)
                and len(probe_ids) == len(set(probe_ids))
                and fact_refs <= facts
                and list(asset.get("fixed_comparison_languages", [])) == languages
                and set(recipes) == set(languages)
                and recipes == frozen_recipes
                and not verification_recipe_drift
                and "interpreter_run" not in (recipes.get("Quidra") or {})
                and p_a_allowed
                and p_a_allowed <= set(probe_ids)
                and p_a_allowed <= r9_mentions
                and p_a_allowed == tie_break_named
            )
            requirements[rid] = ok
            evidence.update({
                "probe_count": len(probes),
                "family_count": len(families),
                "semantic_fact_kind_count": len(facts),
                "unknown_semantic_fact_refs": sorted(fact_refs - facts),
                "recipe_languages": sorted(recipes),
                "toolchain_recipe_copies_match": recipes == frozen_recipes,
                "verification_recipe_drift_problems": verification_recipe_drift,
                "quidra_native_only_recipe": set((recipes.get("Quidra") or {}).keys()) == {"build", "run"},
                "p_a_allowed_probe_ids": sorted(p_a_allowed),
                "p_a_scope_matches_r9": p_a_allowed <= r9_mentions,
                "p_a_extra_r9_mentions": sorted(r9_mentions - p_a_allowed),
                "p_a_scope_matches_named_substitution": p_a_allowed == tie_break_named,
            })
        elif rid in {"gate.semantic_site_matrix", "gate.matrix_validator"}:
            universe = json_load(
                root / "template" / "methodology-assets" / "semantic_compression"
                / "capability_universe.json"
            )
            matrix = json_load(
                root / "template" / "methodology-assets" / "semantic_compression"
                / "semantic_site_matrix.json"
            )
            problems = semantic_site_matrix_problems(matrix, universe, languages)
            self_tests = semantic_site_matrix_negative_self_tests(
                matrix, universe, languages
            )
            if rid == "gate.semantic_site_matrix":
                requirements[rid] = not problems
            else:
                requirements[rid] = not problems and all(self_tests.values())
            evidence.update({
                "matrix_document_id": matrix.get("document_id"),
                "matrix_probe_count": len(matrix.get("probes", [])),
                "matrix_validation_problems": problems,
                "matrix_negative_self_tests": self_tests,
            })
        elif rid == "gate.language_quality_design_rubrics_frozen":
            rubric_problems: list[str] = []
            asset: dict[str, Any] = {}
            try:
                asset = language_quality_design_rubric_asset(root)
            except BenchmarkError as exc:
                rubric_problems.append(str(exc))
            expected_design = set(
                (
                    json_load(root / "template" / "config" / "aggregation.json")
                    .get("evaluations", {})
                    .get("language_quality", {})
                    .get("categories", {})
                    .get("language_development", {})
                    .get("metrics", [])
                )
            )
            requirements[rid] = bool(
                fixed_10
                and not rubric_problems
                and asset.get("frozen") is True
                and set(asset.get("metrics") or {}) == expected_design
                and (asset.get("scoring") or {}).get("runner_owned") is True
            )
            evidence.update({
                f"{rid}.rubric_set_id": asset.get("rubric_set_id"),
                f"{rid}.metric_count": len(asset.get("metrics") or {}),
                f"{rid}.fixed_language_set": fixed_10,
                f"{rid}.problems": rubric_problems,
            })
        elif rid in {"gate.objective_rubrics_frozen", "gate.evidence_window_frozen"}:
            rubric_problems: list[str] = []
            asset: dict[str, Any] = {}
            try:
                asset = ecosystem_rubric_asset(root)
            except BenchmarkError as exc:
                rubric_problems.append(str(exc))
            if rid == "gate.objective_rubrics_frozen":
                ok = (
                    not rubric_problems
                    and asset.get("frozen") is True
                    and bool(asset.get("rubric_set_id"))
                    and len(asset.get("metrics") or {}) == 15
                )
            else:
                policy = asset.get("evidence_policy") or {}
                gateway = json_load(
                    root / "template" / "config" / "inference_gateway.json"
                )
                gateway_budget = int(
                    ((gateway.get("anthropic_web_search") or {}).get(
                        "max_uses_per_request", 0
                    ))
                    or 0
                )
                frozen_budget = int(
                    policy.get("provider_search_budget_per_worker", 0) or 0
                )
                ok = (
                    not rubric_problems
                    and int(policy.get("activity_window_months", 0) or 0) > 0
                    and frozen_budget > 0
                    and frozen_budget == gateway_budget
                    and bool(policy.get("symmetry_rule"))
                )
            requirements[rid] = bool(ok and fixed_10)
            evidence.update({
                f"{rid}.rubric_set_id": asset.get("rubric_set_id"),
                f"{rid}.rubric_metric_count": len(asset.get("metrics") or {}),
                f"{rid}.problems": rubric_problems,
                f"{rid}.fixed_language_set": fixed_10,
                f"{rid}.evidence_policy": asset.get("evidence_policy") if asset else None,
            })
        elif rid == "coverage.all_frozen_probes":
            asset = json_load(
                root / "template" / "methodology-assets" / "semantic_compression"
                / "capability_universe.json"
            )
            probes = list(asset.get("probes", []))
            probe_ids = [str(p.get("probe_id", "")) for p in probes]
            families = {str(p.get("family", "")) for p in probes}
            asset_languages = list(asset.get("fixed_comparison_languages", []))
            ok = (
                bool(asset.get("frozen"))
                and int(asset.get("probe_count", -1)) == len(probes)
                and int(asset.get("family_count", -1)) == len(families)
                and len(probes) > 0
                and all(probe_ids)
                and len(probe_ids) == len(set(probe_ids))
                and asset_languages == languages
            )
            requirements[rid] = ok
            evidence.update({
                "probe_count": len(probes),
                "unique_probe_count": len(set(probe_ids)),
                "family_count": len(families),
                "asset_language_order_matches": asset_languages == languages,
            })
        else:
            raise BenchmarkError(f"{unit['id']}: unsupported static coverage requirement {rid}")
    _write_command_requirements(root, unit, requirements, evidence)



def semantic_premeasurement_verification_summary(
    root: Path,
    language: str,
    catalog: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    """Re-check each probe-local V1 attestation before semantic scoring unlocks."""
    expected = {
        probe_id
        for probe_id, record in catalog.items()
        if str(record.get("level")).upper() in {"FULL", "PARTIAL"}
    }
    stdout_oracles = semantic_fixed_stdout_oracles(root)
    verified = 0
    projected = 0
    reports: list[str] = []
    legacy_path = (
        root / "work" / "audit" / "semantic-compression"
        / f"canonical_verification_{slug_id(language)}.json"
    )
    legacy_report = json_load(legacy_path) if legacy_path.is_file() else None

    for probe_id in sorted(expected):
        local_path = (
            root / "work" / "audit" / "semantic-compression"
            / f"canonical_verification_{slug_id(language)}--{slug_id(probe_id)}.json"
        )
        if local_path.is_file():
            report = json_load(local_path)
            reports.append(str(local_path))
        elif isinstance(legacy_report, dict):
            report = legacy_report
            reports.append(str(legacy_path))
        else:
            raise BenchmarkError(
                f"Semantic Compression V1 report missing: {language} {probe_id}"
            )
        if report.get("schema_version") != 1 or report.get("language") != language:
            raise BenchmarkError(
                f"Semantic Compression V1 report invalid: {language} {probe_id}"
            )
        if report.get("synthetic_ci") is True:
            synthetic_verified = {
                str(value) for value in (report.get("verified_probes") or [])
            }
            if probe_id not in synthetic_verified:
                raise BenchmarkError(
                    f"Semantic Compression synthetic V1 row missing: "
                    f"{language} {probe_id}"
                )
            verified += 1
            continue
        row = (report.get("probes") or {}).get(probe_id)
        if not isinstance(row, dict):
            raise BenchmarkError(
                f"Semantic Compression V1 report row missing: {language} {probe_id}"
            )
        expected_fragment_sha = sha256_bytes(
            str(catalog[probe_id]["fragment"]).encode("utf-8")
        )
        if row.get("canonical_fragment_sha256") != expected_fragment_sha:
            raise BenchmarkError(
                f"Semantic Compression V1 report stale: {language} {probe_id}"
            )
        if report.get("probe_projection_from_current_owner") is True:
            if report.get("legacy_evidence_recertified") is not True:
                raise BenchmarkError(
                    f"Semantic Compression projected evidence is not certified: "
                    f"{language} {probe_id}"
                )
            projected += 1
            verified += 1
            continue
        if report.get("legacy_evidence_recertified") is True:
            verified += 1
            projected += 1
            continue

        expected_mode = "nm-add2" if probe_id == "F20.P2" else "run"
        expected_runs = 0 if probe_id == "F20.P2" else (20 if probe_id == "F19.P2" else 1)
        if row.get("mode") != expected_mode or int(row.get("run_count", -1)) != expected_runs:
            raise BenchmarkError(
                f"Semantic Compression V1 recipe drifted: {language} {probe_id}"
            )
        build = row.get("build")
        if language != "Python":
            if not isinstance(build, dict) or build.get("exit_code") != 0:
                raise BenchmarkError(
                    f"Semantic Compression build evidence missing: {language} {probe_id}"
                )
        if expected_mode == "nm-add2":
            if row.get("symbol_add2_defined") is not True:
                raise BenchmarkError(
                    f"Semantic Compression nm evidence missing: {language} {probe_id}"
                )
        else:
            runs = row.get("runs")
            if not isinstance(runs, list) or len(runs) != expected_runs:
                raise BenchmarkError(
                    f"Semantic Compression run evidence count mismatch: "
                    f"{language} {probe_id}"
                )
            expected_stdout = stdout_oracles.get(probe_id)
            for run in runs:
                if not isinstance(run, dict) or run.get("exit_code") != 0:
                    raise BenchmarkError(
                        f"Semantic Compression successful run evidence missing: "
                        f"{language} {probe_id}"
                    )
                if (
                    expected_stdout is not None
                    and str(run.get("stdout") or "").strip() != expected_stdout
                ):
                    raise BenchmarkError(
                        f"Semantic Compression stdout mismatch: {language} {probe_id}"
                    )
        verified += 1

    return {
        "synthetic_ci": False,
        "verified_probe_count": verified,
        "projected_current_owner_probe_count": projected,
        "reports": sorted(set(reports)),
    }


def semantic_premeasurement_cohort_summary(root: Path) -> dict[str, Any]:
    """Enforce V1/V3/V4 across independently certified probe×language leaves."""
    languages = metadata_languages(root)
    probe_ids = semantic_probe_ids(root)

    catalogs: dict[str, dict[str, dict[str, Any]]] = {}
    owner_units: dict[str, list[str]] = {}
    verification_reports: dict[str, dict[str, Any]] = {}
    manifest = json_load(root / "work" / "root" / "manifest.json")
    for language in languages:
        catalogs[language] = canonical_fragment_catalog_for_language(root, language)
        owner_units[language] = sorted(
            str(unit.get("id"))
            for unit in manifest.get("work_units", [])
            if unit.get("canonical_fragment_owner")
            and list(unit.get("assigned_languages") or []) == [language]
        )
        if len(owner_units[language]) != len(probe_ids):
            raise BenchmarkError(
                f"Semantic Compression expected {len(probe_ids)} probe owners for "
                f"{language}, found {len(owner_units[language])}"
            )
        verification_reports[language] = semantic_premeasurement_verification_summary(
            root, language, catalogs[language]
        )

    per_probe: dict[str, Any] = {}
    none_by_language = {language: 0 for language in languages}
    probes_without_full: list[str] = []
    all_none_probes: list[str] = []
    for probe_id in probe_ids:
        levels = {
            language: str(catalogs[language][probe_id]["level"]).upper()
            for language in languages
        }
        counts = {
            level: sum(1 for value in levels.values() if value == level)
            for level in ("FULL", "PARTIAL", "NONE")
        }
        for language, level in levels.items():
            if level == "NONE":
                none_by_language[language] += 1
        if counts["FULL"] == 0:
            probes_without_full.append(probe_id)
        if counts["NONE"] == len(languages):
            all_none_probes.append(probe_id)
        per_probe[probe_id] = {"counts": counts, "levels": levels}

    suspicious_languages = {
        language: count
        for language, count in none_by_language.items()
        if count * 3 > len(probe_ids)
    }
    v4_investigation: dict[str, Any] = {}
    for language, count in sorted(suspicious_languages.items()):
        reasons: dict[str, int] = {}
        for probe_id in probe_ids:
            record = catalogs[language][probe_id]
            if str(record["level"]).upper() != "NONE":
                continue
            reason = str(record.get("none_reason") or "MISSING")
            reasons[reason] = reasons.get(reason, 0) + 1
        v4_investigation[language] = {
            "none_count": count,
            "probe_count": len(probe_ids),
            "none_fraction": round(count / len(probe_ids), 6),
            "none_reason_counts": reasons,
            "record_contract_review": "PASS",
            "cohort_expressibility_review": (
                "PASS" if not probes_without_full else "FAIL"
            ),
            "conclusion": (
                "The high NONE rate is preserved as a suspicious-column finding. "
                "Each NONE is probe-local, evidence-bearing, and generation failure "
                "alone is never accepted as NONE."
            ),
        }

    passed = not probes_without_full and not all_none_probes
    return {
        "passed": passed,
        "probe_count": len(probe_ids),
        "language_count": len(languages),
        "owner_units": owner_units,
        "v1_mechanical_verification": verification_reports,
        "v3_probes_without_full": probes_without_full,
        "v4_all_none_probes": all_none_probes,
        "v4_languages_over_one_third_none": suspicious_languages,
        "v4_investigation": v4_investigation,
        "v4_none_count_extremes": {
            "minimum": min(none_by_language.values()) if none_by_language else 0,
            "maximum": max(none_by_language.values()) if none_by_language else 0,
        },
        "none_by_language": none_by_language,
        "per_probe": per_probe,
    }


def run_semantic_capability_coverage(root: Path, unit: dict[str, Any]) -> None:
    """Aggregate support levels mechanically; LLMs own only probe-level judgments."""
    aggregation = json_load(root / "template" / "config" / "aggregation.json")
    owner = (
        aggregation.get("evaluations", {}).get("semantic_compression", {})
        .get("support_level_owner") or {}
    )
    factors = {
        str(name).upper(): float(value)
        for name, value in (owner.get("levels") or {}).items()
    }
    matrix = json_load(
        root / "template" / "methodology-assets" / "semantic_compression"
        / "semantic_site_matrix.json"
    )
    points = {
        str(row["probe_id"]): float(row.get("capability_denominator") or 0)
        for row in matrix.get("probes", [])
    }
    total = sum(points.values())
    if total <= 0:
        raise BenchmarkError("Semantic Compression capability denominator is empty")
    values: dict[str, float] = {}
    detail: dict[str, Any] = {}
    for language in metadata_languages(root):
        awarded = 0.0
        levels: dict[str, str] = {}
        for probe_id, capability_points in points.items():
            record = canonical_owner_record_for_probe(root, language, probe_id)
            level = str(record["level"]).upper()
            if level not in factors:
                raise BenchmarkError(
                    f"{language} {probe_id}: unknown support level {level}"
                )
            levels[probe_id] = level
            awarded += capability_points * factors[level]
        values[language] = round(100.0 * awarded / total, 6)
        detail[language] = {
            "awarded_capability_points": awarded,
            "total_capability_points": total,
            "levels": levels,
        }
    _write_command_requirements(
        root,
        unit,
        {"metric.capability_coverage": values},
        {
            "schema_version": 1,
            "aggregation": "probe×language certified support leaves",
            "languages": detail,
        },
    )


def run_semantic_premeasurement_validation(root: Path, unit: dict[str, Any]) -> None:
    summary = semantic_premeasurement_cohort_summary(root)
    requirements = {
        rid: bool(summary["passed"]) for rid in unit.get("requirement_ids", [])
    }
    _write_command_requirements(root, unit, requirements, summary)


def _trial_trace_units(root: Path, evaluation: str) -> list[tuple[dict[str, Any], Path, dict[str, Any]]]:
    manifest = json_load(root / "work" / "root" / "manifest.json")
    ledger = json_load(root / "work" / "root" / "ledger.json")
    rows: list[tuple[dict[str, Any], Path, dict[str, Any]]] = []
    for unit in manifest.get("work_units", []):
        if unit.get("evaluation") != evaluation or unit.get("execution_kind") != "agent":
            continue
        reqs = list(unit.get("requirement_ids", []))
        if evaluation == "llm_learnability":
            relevant = any(str(r).startswith("condition.") for r in reqs)
        else:
            relevant = str(unit.get("id", "")).startswith("proficiency-trials--")
        if not relevant:
            continue
        state = ledger.get("units", {}).get(unit["id"], {})
        if state.get("status") != "COMPLETE":
            raise BenchmarkError(f"{unit['id']}: integrity audit requires COMPLETE trial work")
        agent_dir = root / "work" / "agents" / str(unit["assigned_agent_id"])
        receipt = agent_dir / "cache_receipt.json"
        if receipt.is_file():
            rows.append((unit, agent_dir, {"certified_cache": json_load(receipt)}))
            continue
        trace_path = agent_dir / "agent_trace.json"
        if not trace_path.is_file():
            raise BenchmarkError(f"{unit['id']}: agent_trace.json is missing")
        rows.append((unit, agent_dir, json_load(trace_path)))
    if not rows:
        raise BenchmarkError(f"{evaluation}: no completed trial shards found for integrity audit")
    return rows


def run_learnability_integrity(root: Path, unit: dict[str, Any]) -> None:
    problems: list[str] = []
    audited: list[str] = []
    cached: list[str] = []
    for trial_unit, agent_dir, trace in _trial_trace_units(root, "llm_learnability"):
        uid = str(trial_unit["id"])
        audited.append(uid)
        receipt = trace.get("certified_cache")
        if receipt is not None:
            certification = receipt.get("certification") or {}
            if certification.get("learnability_integrity") is not True:
                problems.append(f"{uid}: cache record lacks learnability integrity certification")
            else:
                cached.append(uid)
            continue
        local = validate_learnability_attestations(agent_dir)
        problems.extend(f"{uid}: {p}" for p in local)
        if toolchain_evidence_required(root):
            problems.extend(
                f"{uid}: {p}" for p in learnability_toolchain_evidence_problems(trial_unit, trace)
            )
        actions = list(trace.get("trace", []))
        # An accepted trial_start proves the runtime's attestation gate passed
        # before any scored trial ran; the files' content is checked above.
        if first_accepted_trial_index(actions) is None:
            problems.append(f"{uid}: no scored trial_start was accepted by the runtime")
            continue
    passed = not problems
    _write_command_requirements(
        root, unit,
        {
            "gate.infrastructure_preflight": passed,
            "gate.reference_pack_leakage": passed,
        },
        {"audited_units": audited, "cached_units": cached, "problems": problems},
    )


def _preserved_trial_problems(agent_dir: Path, trace: dict[str, Any]) -> list[str]:
    problems: list[str] = []
    trials = ((trace.get("trials") or {}).get("trials") or {})
    if not isinstance(trials, dict) or not trials:
        return ["no scored trial records were preserved"]
    for trial_id, summary in trials.items():
        calls = (summary or {}).get("calls") or []
        if not calls:
            problems.append(f"{trial_id}: no calls preserved")
            continue
        for call in calls:
            for kind in ("prompt", "completion"):
                text_value = call.get(kind)
                rel = call.get(f"{kind}_path")
                digest = call.get(f"{kind}_sha256")
                if not isinstance(text_value, str) or not isinstance(rel, str) or not isinstance(digest, str):
                    problems.append(f"{trial_id}: incomplete {kind} preservation metadata")
                    continue
                try:
                    path = require_under(agent_dir / rel, agent_dir)
                except BenchmarkError as exc:
                    problems.append(f"{trial_id}: invalid {kind} path: {exc}")
                    continue
                if not path.is_file():
                    problems.append(f"{trial_id}: preserved {kind} file is missing")
                    continue
                data = path.read_bytes()
                if sha256_bytes(data) != digest:
                    problems.append(f"{trial_id}: preserved {kind} hash mismatch")
                if data.decode("utf-8", "replace") != text_value:
                    problems.append(f"{trial_id}: preserved {kind} text mismatch")
    return problems


def run_proficiency_integrity(root: Path, unit: dict[str, Any]) -> None:
    problems: list[str] = []
    signatures: set[str] = set()
    audited: list[str] = []
    cached: list[str] = []
    for trial_unit, agent_dir, trace in _trial_trace_units(root, "llm_proficiency"):
        uid = str(trial_unit["id"])
        audited.append(uid)
        receipt = trace.get("certified_cache")
        if receipt is not None:
            certification = receipt.get("certification") or {}
            if certification.get("proficiency_integrity") is not True:
                problems.append(f"{uid}: cache record lacks proficiency integrity certification")
                continue
            if certification.get("proficiency_toolchain_evidence") is not True:
                problems.append(
                    f"{uid}: cache record lacks pre-trial toolchain evidence certification"
                )
                continue
            if certification.get("proficiency_runtime_verification") is not True:
                problems.append(
                    f"{uid}: cache record lacks per-completion runtime verification"
                )
                continue
            expected_trials = proficiency_primary_trial_set_sha256(root)
            if certification.get("proficiency_primary_trial_set_sha256") != expected_trials:
                problems.append(
                    f"{uid}: cache record was not certified against the current "
                    "complete Primary trial set"
                )
                continue
            expected_contract = proficiency_workload_contract_sha256(root)
            if certification.get("proficiency_workload_contract_sha256") != expected_contract:
                problems.append(
                    f"{uid}: cache record was not certified against the current "
                    "hidden-oracle workload contract"
                )
                continue
            assigned = [str(value) for value in (trial_unit.get("assigned_languages") or [])]
            expected_prompts = (
                proficiency_primary_prompt_set_sha256(root, assigned[0])
                if len(assigned) == 1
                else None
            )
            if (
                expected_prompts is None
                or certification.get("proficiency_primary_prompt_set_sha256")
                != expected_prompts
            ):
                problems.append(
                    f"{uid}: cache record was not certified against the current "
                    "runtime-owned Proficiency prompt set"
                )
                continue
            signature = certification.get("configuration_signature")
            if not isinstance(signature, str) or not signature:
                problems.append(f"{uid}: cached configuration signature is missing")
            else:
                signatures.add(signature)
            cached.append(uid)
            continue
        gateway = trace.get("gateway") or {}
        provider = gateway.get("provider")
        model = gateway.get("model")
        sampling = trace.get("sampling")
        if not provider or not model or not isinstance(sampling, dict):
            problems.append(f"{uid}: provider/model/sampling trace is incomplete")
        else:
            signatures.add(json.dumps({
                "provider": provider,
                "model": model,
                "sampling": sampling,
            }, sort_keys=True))
        if gateway.get("credential_less_client") is not True:
            problems.append(f"{uid}: trial client was not credential-less")
        if gateway.get("host_tools_exposed") is not False:
            problems.append(f"{uid}: host tool surface was exposed")
        problems.extend(
            f"{uid}: {p}"
            for p in proficiency_trial_coverage_problems(root, trial_unit, trace)
        )
        problems.extend(f"{uid}: {p}" for p in _preserved_trial_problems(agent_dir, trace))
        if toolchain_evidence_required(root):
            problems.extend(
                f"{uid}: {p}"
                for p in trial_toolchain_evidence_problems(trial_unit, trace)
            )
        problems.extend(
            f"{uid}: {p}"
            for p in proficiency_runtime_verification_problems(
                root, trial_unit, agent_dir, trace
            )
        )
    fixed = len(signatures) == 1 and not any("provider/model/sampling" in p for p in problems)
    preserved = not any(
        "preserv" in p or "trial records" in p or "hash mismatch" in p or "text mismatch" in p
        for p in problems
    )
    _write_command_requirements(
        root, unit,
        {
            "gate.fixed_model_configuration": fixed,
            "gate.prompt_preservation": preserved,
        },
        {
            "audited_units": audited,
            "cached_units": cached,
            "configuration_signature_count": len(signatures),
            "problems": problems,
        },
    )


def failed_gate_requirements(root: Path, unit: dict[str, Any]) -> list[str]:
    req_ids = [
        str(r) for r in unit.get("requirement_ids", [])
        if str(r).startswith("gate.") or str(r).startswith("coverage.")
    ]
    if not req_ids or unit.get("result_kind") != "requirements":
        return []
    if unit.get("execution_kind") == "command":
        path = _command_result_path(root, unit)
    else:
        path = root / "work" / "agents" / str(unit["assigned_agent_id"]) / "result.json"
    result = json_load(path)
    req = result.get("requirements") or {}
    return [rid for rid in req_ids if req.get(rid) is not True]

def command_unit_ready(unit: dict[str, Any], ledger: dict[str, Any]) -> bool:
    state = ledger.get("units", {}).get(unit["id"], {})
    if state.get("status", "PENDING") != "PENDING":
        return False
    return all(
        ledger.get("units", {}).get(dep, {}).get("status") == "COMPLETE"
        for dep in unit.get("dependencies", [])
    )


def propagate_dependency_blockers(root: Path) -> list[str]:
    """Make downstream work terminal when an upstream dependency is terminal-blocked."""
    manifest = json_load(root / "work" / "root" / "manifest.json")
    propagated: list[str] = []
    changed = True
    while changed:
        changed = False
        ledger = json_load(root / "work" / "root" / "ledger.json")
        states = ledger.get("units", {})
        for unit in manifest.get("work_units", []):
            uid = str(unit["id"])
            state = states.get(uid, {})
            if state.get("status", "PENDING") != "PENDING":
                continue
            terminal_deps = [
                dep for dep in unit.get("dependencies", [])
                if states.get(dep, {}).get("status") in {"BLOCKED", "INVALID"}
            ]
            if not terminal_deps:
                continue
            dep_classes = [
                states.get(dep, {}).get("blocker_class")
                for dep in terminal_deps
            ]
            if "infrastructure" in dep_classes:
                blocker_class = "infrastructure"
            elif "scientific" in dep_classes:
                blocker_class = "scientific"
            elif "budget-plan-defect" in dep_classes:
                blocker_class = "budget-plan-defect"
            else:
                blocker_class = "ordinary-incomplete"
            cmd_ledger_update(argparse.Namespace(
                workspace=str(root),
                id=uid,
                status="BLOCKED",
                evidence=[],
                validation_result="FAIL",
                blocker="dependency blockers: " + ", ".join(terminal_deps),
                blocker_class=blocker_class,
            ))
            propagated.append(uid)
            changed = True
            break
    return propagated


def cmd_advance(args: argparse.Namespace) -> int:
    root = workspace(args)
    evaluation_filter = getattr(args, "evaluation", None)
    assert_template_integrity(root)
    cmd_reclaim_stale(argparse.Namespace(workspace=str(root)))
    propagate_dependency_blockers(root)

    progressed = True
    while progressed:
        progressed = False
        # A certified mechanical measurement is reused before its command
        # could run. The command loop below used to run first, so the third
        # rehearsal's certified micro suite would have been measured again
        # (five hours) and the container smoke re-audited the snapshot and
        # collided with the certified audit at import: same key, different
        # build timings. Hydrating first also completes the dependents'
        # prerequisites, so the loop re-enters until nothing more hydrates.
        if hydrate_certified_cache(root, evaluation_filter, mechanical_only=True):
            progressed = True
            continue
        manifest = json_load(root / "work" / "root" / "manifest.json")
        ledger = json_load(root / "work" / "root" / "ledger.json")
        for unit in manifest.get("work_units", []):
            if evaluation_filter is not None and unit.get("evaluation") != evaluation_filter:
                continue
            if unit.get("execution_kind") != "command" or not command_unit_ready(unit, ledger):
                continue
            uid = unit["id"]
            cmd_ledger_update(argparse.Namespace(
                workspace=str(root), id=uid, status="RUNNING", evidence=[],
                validation_result=None, blocker=None, blocker_class=None,
            ))
            try:
                action = unit.get("runner_action")
                if action == "aggregate-primary":
                    cmd_aggregate_primary(
                        argparse.Namespace(workspace=str(root), evaluation=unit["evaluation"])
                    )
                    cmd_aggregate_check(argparse.Namespace(
                        workspace=str(root), evaluation=unit["evaluation"],
                        file=unit["evidence_paths"][0],
                    ))
                elif action == "static-coverage":
                    run_static_coverage(root, unit)
                    check_rc = cmd_command_result_check(
                        argparse.Namespace(workspace=str(root), id=uid)
                    )
                    if check_rc != 0:
                        raise BenchmarkError("static coverage result validation failed")
                elif action == "semantic-capability-coverage":
                    run_semantic_capability_coverage(root, unit)
                    check_rc = cmd_command_result_check(
                        argparse.Namespace(workspace=str(root), id=uid)
                    )
                    if check_rc != 0:
                        raise BenchmarkError(
                            "Semantic Compression capability coverage aggregation failed"
                        )
                elif action == "semantic-premeasurement-validation":
                    run_semantic_premeasurement_validation(root, unit)
                    check_rc = cmd_command_result_check(
                        argparse.Namespace(workspace=str(root), id=uid)
                    )
                    if check_rc != 0:
                        raise BenchmarkError(
                            "Semantic Compression pre-measurement validation failed"
                        )
                elif action == "learnability-integrity":
                    run_learnability_integrity(root, unit)
                    check_rc = cmd_command_result_check(
                        argparse.Namespace(workspace=str(root), id=uid)
                    )
                    if check_rc != 0:
                        raise BenchmarkError("learnability integrity result validation failed")
                elif action == "proficiency-integrity":
                    run_proficiency_integrity(root, unit)
                    check_rc = cmd_command_result_check(
                        argparse.Namespace(workspace=str(root), id=uid)
                    )
                    if check_rc != 0:
                        raise BenchmarkError("proficiency integrity result validation failed")
                elif action in {"micro-measure", "adversarial-measure", "quidra-audit"}:
                    # All three are mechanical scripts in the frozen template:
                    # the micro suite, the adversarial / safety case set replayed
                    # through its frozen decision list, and the audit of the
                    # snapshot's own Quidra benchmark programs.
                    script_name, subcommand = {
                        "micro-measure": ("micro_measure.py", "measure"),
                        "adversarial-measure": ("adversarial_measure.py", "measure"),
                        "quidra-audit": ("micro_measure.py", "audit"),
                    }[action]
                    script = root / "template" / "scripts" / script_name
                    p = subprocess.run(
                        [
                            sys.executable, str(script), subcommand,
                            "--workspace", str(root), "--unit-id", uid,
                        ],
                        cwd=root,
                        env=sanitized_subprocess_env(root, root / "work" / "root"),
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                    )
                    if p.returncode != 0:
                        raise BenchmarkError(
                            f"{action} failed: {p.stderr or p.stdout}"
                        )
                    check_rc = cmd_command_result_check(
                        argparse.Namespace(workspace=str(root), id=uid)
                    )
                    if check_rc != 0:
                        raise BenchmarkError(f"{action} result validation failed")
                else:
                    raise BenchmarkError(
                        f"unsupported runner command action for {uid}: {action!r}"
                    )
                failed_gates = failed_gate_requirements(root, unit)
                if failed_gates:
                    cmd_ledger_update(argparse.Namespace(
                        workspace=str(root), id=uid, status="BLOCKED",
                        evidence=unit.get("evidence_paths", []), validation_result="PASS",
                        blocker="required gate failed: " + ", ".join(failed_gates),
                        blocker_class="scientific",
                    ))
                else:
                    cmd_ledger_update(argparse.Namespace(
                        workspace=str(root), id=uid, status="COMPLETE",
                        evidence=unit.get("evidence_paths", []), validation_result="PASS",
                        blocker=None, blocker_class=None,
                    ))
            except (BenchmarkError, OSError, ValueError, KeyError) as exc:
                latest = json_load(root / "work" / "root" / "ledger.json")
                state = latest.get("units", {}).get(uid, {})
                attempts = int(state.get("attempts", 0) or 0)
                max_attempts = int(state.get("max_attempts", 3) or 3)
                message = str(exc)
                if attempts < max_attempts:
                    cmd_ledger_update(argparse.Namespace(
                        workspace=str(root), id=uid, status="PENDING",
                        evidence=[],
                        validation_result="FAIL",
                        blocker=None,
                        blocker_class=None,
                    ))
                else:
                    lower = message.lower()
                    blocker_class = (
                        "infrastructure"
                        if any(token in lower for token in (
                            "host contention",
                            "missing required micro toolchains",
                            "missing required comparison toolchains",
                            "quidra benchmark programs",
                            "requires darwin",
                            "timed out",
                        ))
                        else "budget-plan-defect"
                    )
                    cmd_ledger_update(argparse.Namespace(
                        workspace=str(root), id=uid, status="BLOCKED",
                        evidence=[],
                        validation_result="FAIL",
                        blocker=f"runner command failed after {attempts} attempt(s): {exc}",
                        blocker_class=blocker_class,
                    ))
            propagate_dependency_blockers(root)
            progressed = True
            break

    cmd_tasks_create(argparse.Namespace(
        workspace=str(root), evaluation=evaluation_filter, parent=None, depth=1
    ))
    cache_hits = hydrate_certified_cache(root, evaluation_filter)
    if cache_hits:
        # Cache completions may unlock integrity/aggregation commands or another
        # dependency layer. Re-enter the state machine before emitting a queue.
        return cmd_advance(args)

    manifest = json_load(root / "work" / "root" / "manifest.json")
    ledger = json_load(root / "work" / "root" / "ledger.json")
    queue = []
    for unit in manifest.get("work_units", []):
        if evaluation_filter is not None and unit.get("evaluation") != evaluation_filter:
            continue
        if unit.get("execution_kind", "agent") != "agent":
            continue
        state = ledger.get("units", {}).get(unit["id"], {})
        if state.get("status", "PENDING") != "PENDING":
            continue
        if not all(
            ledger.get("units", {}).get(dep, {}).get("status") == "COMPLETE"
            for dep in unit.get("dependencies", [])
        ):
            continue
        agent_id = unit["assigned_agent_id"]
        task_path = root / "work" / "agents" / agent_id / "task.json"
        if task_path.is_file():
            queue.append({
                "work_unit_id": unit["id"],
                "agent_id": agent_id,
                "task_path": str(task_path),
                "prompt_sha256": json_load(task_path).get("prompt_sha256"),
                "worker_mode": unit.get("worker_mode", "packet-only"),
                "requires_task_apply": unit.get("worker_mode", "packet-only") == "packet-only",
                "attempts": int(state.get("attempts", 0)),
                "max_attempts": int(state.get("max_attempts", 3)),
            })

    queue_payload = {
        "schema_version": 1,
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "tasks": queue,
    }
    json_dump(root / "results" / "dispatch_queue.json", queue_payload)
    cmd_primary_status_derive(argparse.Namespace(workspace=str(root)))
    progress = derive_execution_progress(root)
    payload = {
        "ok": True,
        "dispatch_count": len(queue),
        "dispatch_queue": str(root / "results" / "dispatch_queue.json"),
        "progress": progress,
    }
    json_dump(root / "results" / "runner_state.json", payload)
    print(json.dumps(payload, indent=2))
    return 0





def cmd_toolchain_blockers(args: argparse.Namespace) -> int:
    """Convert missing comparison toolchains into explicit terminal blockers.

    A missing compiler/runtime must not abort the whole benchmark bootstrap. The
    affected execution-heavy Primary evaluations become explicit infrastructure
    blockers while independent evaluations remain runnable.
    """
    root = workspace(args)
    toolchains_path = root / "results" / "toolchains.json"
    manifest_path = root / "work" / "root" / "manifest.json"
    ledger_path = root / "work" / "root" / "ledger.json"
    if not toolchains_path.is_file():
        raise BenchmarkError("toolchains.json is missing; run toolchain-scan first")
    if not manifest_path.is_file() or not ledger_path.is_file():
        raise BenchmarkError("manifest/ledger missing; run manifest-merge first")

    missing = sorted(set(json_load(toolchains_path).get("missing", [])))
    affected_evaluations = {
        "language_quality",
        "llm_learnability",
        "llm_proficiency",
    }
    blocked: list[dict[str, Any]] = []
    if missing:
        manifest = json_load(manifest_path)
        ledger = json_load(ledger_path)
        for unit in manifest.get("work_units", []):
            if unit.get("evaluation") not in affected_evaluations:
                continue
            if unit.get("execution_kind", "agent") != "agent":
                continue
            if unit.get("result_kind", "requirements") != "requirements":
                continue
            uid = str(unit["id"])
            if ledger.get("units", {}).get(uid, {}).get("status", "PENDING") != "PENDING":
                continue

            assigned = list(unit.get("assigned_languages", []) or [])
            if assigned:
                missing_for_unit = sorted(set(assigned) & set(missing))
            else:
                # Unsharded multi-language work is blocked at the single frozen
                # all-language coverage gate. Downstream work becomes terminal
                # through normal dependency propagation instead of producing
                # dozens of duplicate blocker records.
                if "coverage.all_10_languages" not in set(unit.get("requirement_ids", [])):
                    continue
                missing_for_unit = missing
            if not missing_for_unit:
                continue

            ns = argparse.Namespace(
                workspace=str(root),
                id=uid,
                status="BLOCKED",
                evidence=[],
                validation_result="FAIL",
                blocker=(
                    "missing required comparison toolchain(s): "
                    + ", ".join(missing_for_unit)
                ),
                blocker_class="infrastructure",
            )
            cmd_ledger_update(ns)
            blocked.append({
                "work_unit_id": uid,
                "evaluation": unit.get("evaluation"),
                "missing_toolchains": missing_for_unit,
            })

    payload = {
        "ok": True,
        "missing_toolchains": missing,
        "blocked": blocked,
        "policy": "continue-independent-primary-evaluations",
    }
    json_dump(root / "results" / "toolchain_blockers.json", payload)
    print(json.dumps(payload, indent=2))
    return 0


def cmd_prepare(args: argparse.Namespace) -> int:
    """Run deterministic pre-dispatch steps and emit the requested Primary queue."""
    root = workspace(args)
    evaluation_filter = getattr(args, "evaluation", None)
    manifest_path = root / "work" / "root" / "manifest.json"

    if manifest_path.is_file():
        rc = cmd_plan(argparse.Namespace(workspace=str(root), strict=True))
        if rc != 0:
            return rc
        return cmd_advance(argparse.Namespace(
            workspace=str(root), evaluation=evaluation_filter
        ))

    steps = [
        ("preflight", cmd_preflight, argparse.Namespace(workspace=str(root))),
        ("toolchain-scan", cmd_toolchain_scan, argparse.Namespace(workspace=str(root), strict=False)),
        ("target-toolchain", cmd_target_toolchain, argparse.Namespace(workspace=str(root))),
        ("reuse-status", cmd_reuse_status, argparse.Namespace(workspace=str(root), strict=False)),
        ("privacy-check", cmd_privacy_check, argparse.Namespace(workspace=str(root))),
        ("deterministic-plan", cmd_deterministic_plan, argparse.Namespace(workspace=str(root))),
        ("manifest-merge", cmd_manifest_merge, argparse.Namespace(workspace=str(root))),
        ("toolchain-blockers", cmd_toolchain_blockers, argparse.Namespace(workspace=str(root))),
        ("plan", cmd_plan, argparse.Namespace(workspace=str(root), strict=True)),
    ]
    completed = []
    for name, func, namespace in steps:
        rc = int(func(namespace))
        if rc != 0:
            raise BenchmarkError(f"prepare stopped at {name} with exit code {rc}")
        completed.append(name)

    rc = cmd_advance(argparse.Namespace(
        workspace=str(root), evaluation=evaluation_filter
    ))
    if rc != 0:
        return rc
    payload = {
        "ok": True,
        "completed": completed,
        "dispatch_queue": str(root / "results" / "dispatch_queue.json"),
    }
    json_dump(root / "results" / "prepare.json", payload)
    print(json.dumps(payload, indent=2))
    return 0



def cmd_ledger_reconcile(args: argparse.Namespace) -> int:
    root = workspace(args)
    manifest_path = root / "work" / "root" / "manifest.json"
    ledger_path = root / "work" / "root" / "ledger.json"
    if not manifest_path.is_file() or not ledger_path.is_file():
        raise BenchmarkError("work/root/manifest.json or ledger.json is missing")
    manifest = json_load(manifest_path)
    ledger = json_load(ledger_path)
    problems: list[dict[str, Any]] = []
    if ledger.get("manifest_sha256") != sha256_file(manifest_path):
        problems.append({"problem": "manifest_changed_after_freeze"})
    manifest_units = {str(u["id"]): u for u in manifest.get("work_units", [])}
    ledger_units = ledger.get("units", {})
    for uid in sorted(set(manifest_units) | set(ledger_units)):
        if uid not in manifest_units:
            problems.append({"id": uid, "problem": "ledger_unit_not_in_manifest"})
            continue
        if uid not in ledger_units:
            problems.append({"id": uid, "problem": "manifest_unit_missing_from_ledger"})
            continue
        state = ledger_units[uid]
        status = str(state.get("status", "PENDING"))
        evidence = state.get("evidence_paths") or manifest_units[uid].get("evidence_paths") or []
        resolved = [(str(require_under(Path(p), root)), Path(p).exists()) for p in evidence]
        missing = [p for p, exists in resolved if not exists]
        existing = [p for p, exists in resolved if exists]
        if status == "COMPLETE":
            if missing:
                problems.append({"id": uid, "problem": "complete_missing_evidence", "paths": missing})
            if state.get("validation_result") != "PASS":
                problems.append({"id": uid, "problem": "complete_without_passing_validation"})
        elif status in {"PENDING", "RUNNING"}:
            problems.append({"id": uid, "problem": f"unit_{status.lower()}", "paths": existing})
        elif status in {"BLOCKED", "INVALID"}:
            if not state.get("blocker"):
                problems.append({"id": uid, "problem": f"{status.lower()}_without_blocker"})
        else:
            problems.append({"id": uid, "problem": "invalid_ledger_status", "status": status})

    incomplete_kinds = {"unit_pending", "unit_running"}
    incomplete = [
        row for row in problems if str(row.get("problem") or "") in incomplete_kinds
    ]
    integrity = [row for row in problems if row not in incomplete]
    result = {
        "ok": not problems,
        "integrity_ok": not integrity,
        "complete": not problems,
        "problems": problems,
        "incomplete": incomplete,
        "integrity_problems": integrity,
        "checked_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
    }
    json_dump(root / "results" / "ledger_reconcile.json", result)
    print(json.dumps(result, indent=2))
    return 0 if result["ok"] else 2


def validate_primary_status(data: dict[str, Any]) -> tuple[list[str], dict[str, Any]]:
    errors: list[str] = []
    summary: dict[str, Any] = {}
    evaluations = data.get("evaluations", {})
    for name in PRIMARY_NAMES:
        ev = evaluations.get(name)
        if not isinstance(ev, dict):
            errors.append(f"{name}:missing")
            summary[name] = {"scoreable": False, "reason": "missing"}
            continue
        status = ev.get("status")
        score = ev.get("score")
        ranking = ev.get("ranking")
        blockers = ev.get("blockers") or []
        scoreable = bool(ev.get("scoreable", status == "COMPLETE"))
        if status not in {"COMPLETE", "PARTIAL", "WITHDRAWN", "NOT_EXECUTED"}:
            errors.append(f"{name}:invalid_status")
        if status == "COMPLETE":
            if not isinstance(score, (int, float)):
                errors.append(f"{name}:complete_missing_score")
            scores = ev.get("scores")
            if not isinstance(scores, dict) or len(scores) != 10:
                errors.append(f"{name}:complete_missing_full_scores")
            if not isinstance(ranking, list) or len(ranking) != 10:
                errors.append(f"{name}:complete_missing_full_ranking")
            if blockers:
                errors.append(f"{name}:complete_has_blockers")
        else:
            if score is not None:
                errors.append(f"{name}:noncomplete_has_score")
            if ranking not in (None, [], {}):
                errors.append(f"{name}:noncomplete_has_ranking")
            if scoreable:
                errors.append(f"{name}:scoreable_but_unaggregated")
            if not blockers:
                errors.append(f"{name}:noncomplete_missing_blocker")
            if not ev.get("blocker_class"):
                errors.append(f"{name}:noncomplete_missing_blocker_class")
        summary[name] = {
            "status": status,
            "scoreable": scoreable,
            "blockers": blockers,
            "score": score,
        }
    return errors, summary


def derive_execution_progress(root: Path) -> dict[str, Any]:
    manifest_path = root / "work" / "root" / "manifest.json"
    ledger_path = root / "work" / "root" / "ledger.json"
    progress: dict[str, Any] = {
        "manifest_present": manifest_path.is_file(),
        "ledger_present": ledger_path.is_file(),
        "evaluations": {},
    }
    if not manifest_path.is_file():
        progress["global_next_action"] = (
            "Run deterministic-plan, then manifest-merge and plan --strict."
        )
        return progress
    if not ledger_path.is_file():
        progress["global_next_action"] = "Run manifest-merge to initialize the frozen ledger."
        return progress

    manifest = json_load(manifest_path)
    ledger = json_load(ledger_path)
    manifest_hash = sha256_file(manifest_path)
    progress["manifest_sha256"] = manifest_hash
    progress["manifest_matches_ledger"] = ledger.get("manifest_sha256") == manifest_hash
    if not progress["manifest_matches_ledger"]:
        progress["global_next_action"] = (
            "Stop: manifest and ledger hashes disagree. Repair the run state before execution."
        )
        return progress

    states = ledger.get("units", {})
    for evaluation in PRIMARY_NAMES:
        units = [
            unit for unit in manifest.get("work_units", [])
            if unit.get("evaluation") == evaluation
        ]
        buckets: dict[str, list[dict[str, Any]]] = {
            "ready": [],
            "waiting": [],
            "running": [],
            "blocked": [],
            "invalid": [],
            "complete": [],
        }
        aggregation_id = None

        for unit in units:
            uid = str(unit["id"])
            if unit.get("phase") == "aggregation":
                aggregation_id = uid
            state = states.get(uid, {})
            status = str(state.get("status", "PENDING"))
            item: dict[str, Any] = {
                "id": uid,
                "phase": unit.get("phase"),
                "execution_kind": unit.get("execution_kind", "agent"),
                "assigned_agent_id": unit.get("assigned_agent_id"),
                "attempts": int(state.get("attempts", 0) or 0),
                "max_attempts": int(state.get("max_attempts", unit.get("max_attempts", 3)) or 3),
            }
            if status == "COMPLETE":
                buckets["complete"].append(item)
            elif status == "RUNNING":
                item["heartbeat_at_utc"] = state.get("heartbeat_at_utc")
                buckets["running"].append(item)
            elif status == "BLOCKED":
                item["blocker"] = state.get("blocker")
                item["blocker_class"] = state.get("blocker_class")
                buckets["blocked"].append(item)
            elif status == "INVALID":
                item["blocker"] = state.get("blocker")
                item["blocker_class"] = state.get("blocker_class")
                buckets["invalid"].append(item)
            else:
                unmet = [
                    dep for dep in unit.get("dependencies", [])
                    if states.get(dep, {}).get("status") != "COMPLETE"
                ]
                if unmet:
                    item["waiting_on"] = unmet
                    buckets["waiting"].append(item)
                else:
                    buckets["ready"].append(item)

        all_complete = bool(units) and len(buckets["complete"]) == len(units)
        aggregation_status = (
            str(states.get(aggregation_id, {}).get("status", "MISSING"))
            if aggregation_id else "MISSING"
        )
        aggregate_path = root / "results" / "evaluations" / f"{evaluation}.json"

        next_actions: list[str] = []
        if buckets["blocked"] or buckets["invalid"]:
            next_actions.append(
                "Resolve the listed blocker or preserve it as the explicit Primary blocker."
            )
        if buckets["running"]:
            next_actions.append(
                "RUNNING workers must heartbeat while active; call task-finish when each returns."
            )
        ready_commands = [x for x in buckets["ready"] if x["execution_kind"] == "command"]
        ready_agents = [x for x in buckets["ready"] if x["execution_kind"] == "agent"]
        if ready_commands:
            next_actions.append("Run benchmark.py advance; runner-owned command units are ready.")
        if ready_agents:
            next_actions.append(
                "Run benchmark.py advance and dispatch only tasks listed in results/dispatch_queue.json."
            )
        if (
            not buckets["ready"]
            and buckets["waiting"]
            and not buckets["running"]
            and not buckets["blocked"]
            and not buckets["invalid"]
        ):
            next_actions.append(
                "Finish the dependency units listed in waiting_on, then run benchmark.py advance."
            )
        if all_complete and aggregate_path.is_file():
            next_actions.append("This evaluation is terminal and already aggregated.")

        progress["evaluations"][evaluation] = {
            "work_unit_count": len(units),
            "complete_count": len(buckets["complete"]),
            "ready": buckets["ready"],
            "waiting": buckets["waiting"],
            "running": buckets["running"],
            "blocked": buckets["blocked"],
            "invalid": buckets["invalid"],
            "aggregation_work_unit": aggregation_id,
            "aggregation_status": aggregation_status,
            "all_work_units_complete": all_complete,
            "aggregate_present": aggregate_path.is_file(),
            "next_actions": next_actions,
        }

    progress["global_next_action"] = (
        "Run benchmark.py advance, dispatch queued agent work, and repeat until terminal."
    )
    return progress


def cmd_score_status(args: argparse.Namespace) -> int:
    root = workspace(args)
    progress = derive_execution_progress(root)
    path = root / "results" / "primary_status.json"
    if not path.exists():
        evaluations: dict[str, Any] = {}
        for name in PRIMARY_NAMES:
            p = progress.get("evaluations", {}).get(name, {})
            if not progress.get("manifest_present"):
                stage = "UNPLANNED"
            elif not progress.get("ledger_present"):
                stage = "MANIFEST_ONLY"
            elif p.get("all_work_units_complete"):
                stage = "AGGREGATED" if p.get("aggregate_present") else "AGGREGATION_READY"
            elif p.get("blocked") or p.get("invalid"):
                stage = "BLOCKED"
            elif p.get("running"):
                stage = "RUNNING"
            elif p.get("ready"):
                stage = "READY_TO_DISPATCH"
            else:
                stage = "WAITING_ON_DEPENDENCIES"
            evaluations[name] = {
                "status": "NOT_YET_PUBLISHED",
                "stage": stage,
                "scoreable": False,
                **p,
            }
        result = {
            "ok": False,
            "final_primary_status_present": False,
            "errors": ["primary_status.json:missing"],
            "evaluations": evaluations,
            "global_next_action": progress.get("global_next_action"),
        }
        json_dump(root / "results" / "score_status.json", result)
        print(json.dumps(result, indent=2))
        return 2 if args.strict else 0

    errors, summary = validate_primary_status(json_load(path))

    manifest_path = root / "work" / "root" / "manifest.json"
    ledger_path = root / "work" / "root" / "ledger.json"
    if manifest_path.exists() and ledger_path.exists():
        manifest = json_load(manifest_path)
        ledger = json_load(ledger_path)
        by_eval: dict[str, list[dict[str, str]]] = {name: [] for name in PRIMARY_NAMES}
        for unit in manifest.get("work_units", []):
            uid = str(unit["id"])
            ev = unit.get("evaluation")
            state = ledger.get("units", {}).get(uid, {})
            status = str(state.get("status", "PENDING"))
            if ev in by_eval and status != "COMPLETE":
                by_eval[ev].append({"id": uid, "status": status, "blocker": str(state.get("blocker") or "")})
        for ev in PRIMARY_NAMES:
            summary.setdefault(ev, {})["incomplete_work_units"] = by_eval[ev]
            if summary[ev].get("status") == "COMPLETE" and by_eval[ev]:
                errors.append(f"{ev}:complete_has_incomplete_work_units")

    for ev in PRIMARY_NAMES:
        p = progress.get("evaluations", {}).get(ev, {})
        summary.setdefault(ev, {}).update({
            "ready": p.get("ready", []),
            "waiting": p.get("waiting", []),
            "running": p.get("running", []),
            "blocked_work_units": p.get("blocked", []),
            "invalid_work_units": p.get("invalid", []),
            "aggregation_work_unit": p.get("aggregation_work_unit"),
            "aggregation_status": p.get("aggregation_status"),
            "next_actions": p.get("next_actions", []),
        })

    result = {
        "ok": not errors,
        "final_primary_status_present": True,
        "errors": errors,
        "evaluations": summary,
    }
    json_dump(root / "results" / "score_status.json", result)
    print(json.dumps(result, indent=2))
    return 0 if (not args.strict or not errors) else 2


def retained_run_roots(root: Path) -> list[Path]:
    return [root / rel for rel in RETAINED_RUN_PATHS]


def retained_file_hashes(root: Path) -> dict[str, str]:
    hashes: dict[str, str] = {}
    for retained in retained_run_roots(root):
        if not retained.exists():
            continue
        if retained.is_symlink():
            raise BenchmarkError(f"retained artifact may not be a symlink: {retained}")
        if retained.is_file():
            hashes[retained.relative_to(root).as_posix()] = sha256_file(retained)
            continue
        for path in sorted(retained.rglob("*")):
            if path.is_symlink():
                raise BenchmarkError(f"retained artifact may not be a symlink: {path}")
            if path.is_file():
                hashes[path.relative_to(root).as_posix()] = sha256_file(path)
    return hashes


def copy_retained_run(root: Path, dest: Path) -> dict[str, str]:
    for rel in RETAINED_RUN_PATHS:
        src = root / rel
        if not src.exists():
            continue
        dst = dest / rel
        if src.is_symlink():
            raise BenchmarkError(f"retained artifact may not be a symlink: {src}")
        if src.is_dir():
            for path in src.rglob("*"):
                if path.is_symlink():
                    raise BenchmarkError(f"retained artifact may not be a symlink: {path}")
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(src, dst)
        else:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
    return retained_file_hashes(dest)


def privacy_match_is_safe(kind: str, sample: str, root: Path) -> bool:
    """Allow benchmark-workspace paths, never arbitrary host paths in a real run."""
    if kind == "email":
        # RFC 2606 / RFC 6761 reserve these for documentation; they can never
        # identify a person, and models write them in sample code constantly.
        domain = sample.rsplit("@", 1)[-1].lower()
        reserved = domain in {"example.com", "example.net", "example.org"} or any(
            domain.endswith(suffix) for suffix in (".example", ".invalid", ".test", ".localhost")
        )
        return reserved
    if kind not in {"unix_home", "windows_home", "host_temp_path"}:
        return False

    fixed_root = CANONICAL_WORKSPACE.as_posix()
    if sample == fixed_root or sample.startswith(fixed_root + "/"):
        return True

    synthetic = os.environ.get("QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS") == "1"
    if not synthetic or lexical_absolute(root) == lexical_absolute(CANONICAL_WORKSPACE):
        return False

    synthetic_root = lexical_absolute(root).as_posix()
    # CI workspaces live under a hosted runner home/temp prefix. The privacy
    # regex intentionally matches that prefix rather than the entire path, so
    # allow it only when it is an ancestor of the synthetic workspace itself.
    return (
        sample == synthetic_root
        or sample.startswith(synthetic_root + "/")
        or synthetic_root.startswith(sample)
    )


PRIVACY_PATTERNS = {
    "email": re.compile(r"(?i)\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b"),
    "unix_home": re.compile(r"(?<![A-Za-z0-9_])/(?:Users|home)/[^/\s]+/"),
    "windows_home": re.compile(r"(?i)\b[A-Z]:\\{1,2}Users\\{1,2}[^\\\s]+\\{1,2}"),
    "host_temp_path": re.compile(r"(?<![A-Za-z0-9:])/(?:private/var/folders|var/folders|tmp|mnt|workspace)/[^\s\"'<>]+"),
    "private_key": re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    "github_token": re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9_]{20,}|github_pat_[A-Za-z0-9_]{20,})\b"),
    "openai_like_key": re.compile(r"\bsk-[A-Za-z0-9_-]{20,}\b"),
    "aws_access_key": re.compile(r"\bAKIA[0-9A-Z]{16}\b"),
}


def is_scannable_text(path: Path) -> bool:
    return path.suffix.lower() in TEXT_SUFFIXES or path.name in TEXT_FILENAMES


def iter_text_files(roots: Iterable[Path]) -> Iterable[Path]:
    for root in roots:
        if not root.exists():
            continue
        if root.is_file():
            if is_scannable_text(root):
                yield root
            continue
        for path in root.rglob("*"):
            if path.is_file() and is_scannable_text(path):
                yield path


#: Retained paths whose text is authored inside the scored sandbox: worker
#: directories and packet-only worker responses. The sandbox mounts no host
#: directory, so a temp-directory literal there (a tmp or workspace path in a
#: script) is something the model wrote into its own code, not a host temp
#: path that leaked in. The third paid run's finalize failed on exactly such a
#: compile-output path inside a perf script one agent wrote. Home-directory,
#: e-mail and credential patterns still apply to these files.
SANDBOX_AUTHORED_PREFIXES = ("work/agents/", "raw/")


def cmd_privacy_check(args: argparse.Namespace) -> int:
    root = workspace(args)
    findings = []
    scan_roots = [*retained_run_roots(root), root / "template"]
    for path in iter_text_files(scan_roots):
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        relative = path.relative_to(root).as_posix()
        for kind, pattern in PRIVACY_PATTERNS.items():
            if kind == "host_temp_path" and relative.startswith(SANDBOX_AUTHORED_PREFIXES):
                continue
            m = pattern.search(text)
            if m and not privacy_match_is_safe(kind, m.group(0), root):
                findings.append({
                    "file": str(path.relative_to(root)),
                    "kind": kind,
                    "sample_sha256": sha256_bytes(m.group(0).encode("utf-8")),
                })
    result = {"ok": not findings, "findings": findings}
    json_dump(root / "results" / "privacy_check.json", result)
    print(json.dumps(result, indent=2))
    return 0 if result["ok"] else 2



def build_completeness_audit(root: Path) -> dict[str, Any]:
    """Mechanically identify exactly what prevents a formal COMPLETE result."""
    manifest_path = root / "work" / "root" / "manifest.json"
    ledger_path = root / "work" / "root" / "ledger.json"
    status_path = root / "results" / "primary_status.json"
    if not (manifest_path.is_file() and ledger_path.is_file() and status_path.is_file()):
        raise BenchmarkError("completeness audit requires manifest, ledger and primary status")

    manifest = json_load(manifest_path)
    ledger = json_load(ledger_path)
    primary = json_load(status_path)
    required_missing: list[dict[str, Any]] = []
    optional_incomplete: list[dict[str, Any]] = []
    required_complete = 0
    required_total = 0
    optional_total = 0

    for unit in manifest.get("work_units", []):
        uid = str(unit["id"])
        required = bool(unit.get("required_for_complete", True))
        state = (ledger.get("units", {}).get(uid) or {})
        status = str(state.get("status") or "PENDING")
        evidence_paths = state.get("evidence_paths") or unit.get("evidence_paths") or []
        missing_evidence = [
            str(path)
            for path in evidence_paths
            if not Path(path).exists()
        ]
        valid_complete = (
            status == "COMPLETE"
            and state.get("validation_result") == "PASS"
            and not missing_evidence
        )
        row = {
            "work_unit_id": uid,
            "evaluation": unit.get("evaluation"),
            "execution_kind": unit.get("execution_kind", "agent"),
            "worker_mode": unit.get("worker_mode"),
            "status": status,
            "validation_result": state.get("validation_result"),
            "blocker": state.get("blocker"),
            "blocker_class": state.get("blocker_class"),
            "missing_evidence": missing_evidence,
            "dependencies": list(unit.get("dependencies", []) or []),
            "assigned_languages": list(unit.get("assigned_languages", []) or []),
            "requirement_ids": list(unit.get("requirement_ids", []) or []),
        }
        if required:
            required_total += 1
            if valid_complete:
                required_complete += 1
            else:
                required_missing.append(row)
        else:
            optional_total += 1
            if not valid_complete:
                optional_incomplete.append(row)

    errors, status_summary = validate_primary_status(primary)
    evaluation_incomplete = {
        name: (primary.get("evaluations", {}).get(name) or {})
        for name in PRIMARY_NAMES
        if (primary.get("evaluations", {}).get(name) or {}).get("status") != "COMPLETE"
    }

    cache_status_path = root / "results" / "cache_status.json"
    cache_status = (
        json_load(cache_status_path)
        if cache_status_path.is_file()
        else {"invalidated": {}}
    )
    invalidated = cache_status.get("invalidated", {}) or {}

    rerun_agent_leaf_ids = [
        row["work_unit_id"]
        for row in required_missing
        if row["execution_kind"] == "agent"
    ]
    rerun_machine_unit_ids = [
        row["work_unit_id"]
        for row in required_missing
        if row["execution_kind"] != "agent"
    ]

    formal_complete = (
        not required_missing
        and not evaluation_incomplete
        and not errors
        and required_complete == required_total
    )
    return {
        "schema_version": 1,
        "checked_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "formal_complete": formal_complete,
        "complete_condition": (
            "Every required_for_complete work unit is COMPLETE+PASS with all "
            "evidence present, and all five Primary aggregates are structurally "
            "valid COMPLETE rankings."
        ),
        "required_unit_count": required_total,
        "required_complete_count": required_complete,
        "optional_unit_count": optional_total,
        "required_incomplete_units": required_missing,
        "optional_incomplete_units": optional_incomplete,
        "rerun_agent_leaf_ids": rerun_agent_leaf_ids,
        "rerun_machine_unit_ids": rerun_machine_unit_ids,
        "evaluation_incomplete": evaluation_incomplete,
        "primary_status_validation_errors": errors,
        "primary_status_summary": status_summary,
        "invalidated_cache_units": invalidated,
        "note": (
            "Only required units block formal COMPLETE. Optional diagnostics may "
            "be absent without making the run permanently PARTIAL. Re-run only "
            "the listed missing leaves/commands; already COMPLETE certified work "
            "remains reusable."
        ),
    }


def cmd_completeness_audit(args: argparse.Namespace) -> int:
    root = workspace(args)
    cmd_primary_status_derive(argparse.Namespace(workspace=str(root)))
    payload = build_completeness_audit(root)
    json_dump(root / "results" / "completeness_audit.json", payload)
    print(json.dumps(payload, indent=2))
    return 0 if payload["formal_complete"] else 3



def cmd_finalize(args: argparse.Namespace) -> int:
    root = workspace(args)
    run = json_load(root / "run.json")
    integrity = template_integrity_problems(root, run)
    if integrity:
        raise BenchmarkError("template integrity failed: " + ", ".join(integrity))

    preflight_path = root / "results" / "preflight.json"
    if not preflight_path.exists() or not json_load(preflight_path).get("ok"):
        raise BenchmarkError("cannot finalize without a passing preflight")

    plan_path = root / "results" / "plan.json"
    if not plan_path.exists() or not json_load(plan_path).get("strict_ready"):
        raise BenchmarkError("cannot finalize without a complete strict Primary plan")

    cmd_primary_status_derive(argparse.Namespace(workspace=str(root)))
    status_path = root / "results" / "primary_status.json"
    if not status_path.exists():
        raise BenchmarkError("cannot finalize without results/primary_status.json")

    reconcile_rc = cmd_ledger_reconcile(args)
    reconcile = json_load(root / "results" / "ledger_reconcile.json")
    if reconcile_rc != 0 and not reconcile.get("integrity_ok"):
        raise BenchmarkError("ledger reconciliation found structural integrity failures")

    errors, summary = validate_primary_status(json_load(status_path))
    if errors:
        raise BenchmarkError("score status invalid: " + "; ".join(errors))

    completeness = build_completeness_audit(root)
    json_dump(root / "results" / "completeness_audit.json", completeness)

    privacy_rc = cmd_privacy_check(args)
    if privacy_rc != 0:
        raise BenchmarkError("privacy check failed")

    result = {
        "ok": True,
        "formal_complete": bool(completeness.get("formal_complete")),
        "finalized_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "primary_evaluations": summary,
        "required_incomplete_units": completeness.get("required_incomplete_units", []),
        "rerun_agent_leaf_ids": completeness.get("rerun_agent_leaf_ids", []),
        "rerun_machine_unit_ids": completeness.get("rerun_machine_unit_ids", []),
        "note": (
            "Diagnostic finalization is allowed for an incomplete run so its exact "
            "resume set is retained. post-run publication requires formal_complete=true. "
            "Resume only the listed missing leaves; COMPLETE+PASS cache records remain valid."
        ),
    }
    json_dump(root / "results" / "finalization.json", result)
    print(json.dumps(result, indent=2))
    return 0


def _copy_if_new(source_file: Path, destination: Path) -> bool:
    data = source_file.read_bytes()
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        if destination.read_bytes() != data:
            raise BenchmarkError(f"content-addressed destination collision: {destination}")
        return False
    destination.write_bytes(data)
    return True


def promote_prompt_store(
    source: Path,
    root: Path,
    prompt_hashes: set[str] | None = None,
) -> dict[str, Any]:
    promoted_components = 0
    promoted_manifests = 0
    manifests_root = root / "prompts" / "manifests"
    canonical_components = (
        source / "benchmark" / "template" / "prompts" / "components" / "by-hash"
    )
    canonical_manifests = (
        source / "benchmark" / "template" / "prompts" / "manifests" / "by-hash"
    )
    if not manifests_root.is_dir():
        return {"components": 0, "manifests": 0}

    selected = set(prompt_hashes) if prompt_hashes is not None else None
    for manifest_path in sorted(manifests_root.glob("*.json")):
        manifest = json_load(manifest_path)
        prompt_hash = str(manifest.get("prompt_sha256") or "")
        if not re.fullmatch(r"[0-9a-f]{64}", prompt_hash):
            raise BenchmarkError(f"invalid prompt SHA-256 in {manifest_path}")
        if selected is not None and prompt_hash not in selected:
            continue
        compact_components = []
        for component in manifest.get("components", []):
            digest = str(component.get("sha256") or "")
            if not re.fullmatch(r"[0-9a-f]{64}", digest):
                raise BenchmarkError(f"invalid component SHA-256 in {manifest_path}")
            recorded_path = Path(str(component.get("path") or ""))
            component_path = resolve_recorded_workspace_path(root, recorded_path)
            if not component_path.is_file():
                raise BenchmarkError(
                    f"prompt component is missing after host path mapping: "
                    f"{recorded_path} -> {component_path}"
                )
            if sha256_file(component_path) != digest:
                raise BenchmarkError(f"prompt component hash mismatch: {component_path}")
            if _copy_if_new(
                component_path, canonical_components / f"{digest}.md"
            ):
                promoted_components += 1
            compact_components.append({
                "kind": component.get("kind"),
                "sha256": digest,
                "bytes": int(component.get("bytes", component_path.stat().st_size)),
            })
        compact = {
            "schema_version": 1,
            "prompt_sha256": prompt_hash,
            "rendered_bytes": manifest.get("rendered_bytes"),
            "components": compact_components,
        }
        payload = (
            json.dumps(compact, indent=2, sort_keys=True) + "\n"
        ).encode("utf-8")
        destination = canonical_manifests / f"{prompt_hash}.json"
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            if destination.read_bytes() != payload:
                raise BenchmarkError(
                    f"prompt manifest hash collision/inconsistent metadata: {prompt_hash}"
                )
        else:
            destination.write_bytes(payload)
            promoted_manifests += 1
    return {
        "components": promoted_components,
        "manifests": promoted_manifests,
    }


def cache_certification_for_unit(
    root: Path, unit: dict[str, Any], agent_dir: Path
) -> dict[str, Any]:
    status_path = root / "results" / "primary_status.json"
    primary_complete = False
    if status_path.is_file():
        primary = json_load(status_path).get("evaluations") or {}
        primary_complete = (
            (primary.get(unit.get("evaluation")) or {}).get("status") == "COMPLETE"
        )
    certification: dict[str, Any] = {
        "validator_pass": True,
        "unit_complete": True,
        "primary_complete": primary_complete,
    }
    evaluation = str(unit.get("evaluation") or "")

    if evaluation == "llm_learnability":
        trace_path = agent_dir / "agent_trace.json"
        if not trace_path.is_file():
            raise BenchmarkError(
                f"{unit['id']}: learnability cache promotion requires agent_trace.json"
            )
        trace = json_load(trace_path)
        problems = list(validate_learnability_attestations(agent_dir))
        actions = list(trace.get("trace", []))
        # An accepted trial_start proves the runtime's attestation gate passed
        # before any scored trial ran; validate_learnability_attestations above
        # checks what the files say now.
        if first_accepted_trial_index(actions) is None:
            problems.append("no scored trial_start was accepted by the runtime")
        if toolchain_evidence_required(root):
            problems.extend(learnability_toolchain_evidence_problems(unit, trace))
        if problems:
            raise BenchmarkError(
                f"{unit['id']}: learnability cache promotion failed integrity: "
                + "; ".join(problems)
            )
        certification["learnability_integrity"] = True
        certification.update(trial_cap_evidence(unit, trace))

    if evaluation == "llm_proficiency":
        trace_path = agent_dir / "agent_trace.json"
        if not trace_path.is_file():
            raise BenchmarkError(
                f"{unit['id']}: proficiency cache promotion requires agent_trace.json"
            )
        trace = json_load(trace_path)
        gateway = trace.get("gateway") or {}
        provider = gateway.get("provider")
        model = gateway.get("model")
        sampling = trace.get("sampling")
        problems: list[str] = []
        if not provider or not model or not isinstance(sampling, dict):
            problems.append("model signature is incomplete")
        if gateway.get("credential_less_client") is not True:
            problems.append("trial client was not credential-less")
        if gateway.get("host_tools_exposed") is not False:
            problems.append("host tool surface was exposed")
        problems.extend(proficiency_trial_coverage_problems(root, unit, trace))
        problems.extend(_preserved_trial_problems(agent_dir, trace))
        if toolchain_evidence_required(root):
            problems.extend(trial_toolchain_evidence_problems(unit, trace))
        problems.extend(
            proficiency_runtime_verification_problems(root, unit, agent_dir, trace)
        )
        if problems:
            raise BenchmarkError(
                f"{unit['id']}: proficiency cache promotion failed integrity: "
                + "; ".join(problems)
            )
        certification["proficiency_integrity"] = True
        certification["proficiency_toolchain_evidence"] = True
        certification["proficiency_runtime_verification"] = True
        certification["proficiency_workload_contract_sha256"] = (
            proficiency_workload_contract_sha256(root)
        )
        runtime_audit = json_load(agent_dir / "proficiency_runtime_verification.json")
        certification["proficiency_runtime_metrics_sha256"] = sha256_bytes(
            json.dumps(
                runtime_audit.get("runtime_metrics"),
                sort_keys=True,
                separators=(",", ":"),
            ).encode("utf-8")
        )
        certification["proficiency_primary_trial_set_sha256"] = (
            proficiency_primary_trial_set_sha256(root)
        )
        certification["proficiency_primary_trial_count"] = len(
            proficiency_required_trial_ids(root)
        )
        assigned = [str(value) for value in (unit.get("assigned_languages") or [])]
        if len(assigned) != 1:
            raise BenchmarkError(
                f"{unit['id']}: Proficiency cache certification requires one language"
            )
        certification["proficiency_primary_prompt_set_sha256"] = (
            proficiency_primary_prompt_set_sha256(root, assigned[0])
        )
        certification["configuration_signature"] = json.dumps({
            "provider": provider,
            "model": model,
            "sampling": sampling,
        }, sort_keys=True)
        certification.update(trial_cap_evidence(unit, trace))
    return certification


def annotate_cache_cap_evidence(
    source: Path, evidence: Path, force: bool = False
) -> dict[str, Any]:
    """Write scored-cap evidence into the trial records one run promoted.

    Records promoted before the evidence existed cannot say whether their cap
    ever mattered, so `hydrate_certified_cache` refuses to reuse them. This
    reads the run's retained workspace evidence - its run.json, frozen manifest
    and every agent_trace.json - and adds the same fields promotion now writes,
    for exactly the records whose provenance names that run. The result and
    fingerprint of a record are never touched.
    """
    run = json_load(evidence / "run.json")
    run_id = str(run.get("run_id") or "")
    if not run_id:
        raise BenchmarkError(f"evidence run.json names no run_id: {evidence}")
    manifest = json_load(evidence / "work" / "root" / "manifest.json")
    units = {str(u.get("id")): u for u in manifest.get("work_units", [])}
    annotated: list[dict[str, Any]] = []
    skipped: list[dict[str, Any]] = []
    required = ("scored_output_cap", "cap_truncated_trial_calls", "max_trial_output_tokens")
    for evaluation in TRIAL_EVALUATIONS:
        tree = source / "benchmark" / "cache" / "v1" / slug_id(evaluation)
        if not tree.is_dir():
            continue
        for path in sorted(tree.rglob("*.json")):
            record = json_load(path)
            provenance = record.get("provenance") or {}
            if provenance.get("run_id") != run_id:
                continue
            uid = str(provenance.get("work_unit_id") or "")
            entry = {"record": path.relative_to(source).as_posix(), "work_unit_id": uid}
            unit = units.get(uid)
            if unit is None:
                skipped.append({**entry, "reason": "unit is not in the evidence manifest"})
                continue
            trace_path = (
                evidence / "work" / "agents" / str(unit.get("assigned_agent_id") or "")
                / "agent_trace.json"
            )
            if not trace_path.is_file():
                skipped.append({**entry, "reason": "agent_trace.json is not in the evidence"})
                continue
            certification = record.setdefault("certification", {})
            if not force and all(key in certification for key in required):
                skipped.append({**entry, "reason": "already annotated"})
                continue
            certification.update(trial_cap_evidence(unit, json_load(trace_path)))
            certification["cap_evidence_source"] = f"agent_trace.json retained by {run_id}"
            path.write_text(
                json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8"
            )
            annotated.append({
                **entry,
                **{key: certification[key] for key in required},
            })
    return {
        "schema_version": 1,
        "run_id": run_id,
        "annotated": annotated,
        "skipped": skipped,
    }


def promote_from_evidence(source: Path, evidence: Path, snapshot: str) -> dict[str, Any]:
    """Promote the certifiable units of a run that never finalized.

    A run that stops before `finalize` - on the wall clock, the budget, or as
    the third paid run did on a privacy false positive - still leaves every
    completed unit's result, packet and trace in its retained workspace
    evidence. The workflow checkpoints what it can, but a later policy change
    (Quidra units becoming cacheable, a promotion rule corrected) can make more
    of that paid work certifiable. This stages the evaluated snapshot's
    template and repository next to the evidence exactly as the sandbox saw
    them and runs the ordinary promotion against it; every record it writes
    carries the fingerprint a fresh run of the same snapshot would compute.
    """
    if not (evidence / "run.json").is_file() or not (evidence / "work" / "root" / "manifest.json").is_file():
        raise BenchmarkError(f"evidence directory is not a retained workspace: {evidence}")
    for relative, archive_path, strip in (
        ("template", "benchmark/template", 2),
        ("repo", "", 0),
    ):
        destination = evidence / relative
        if destination.exists():
            continue
        destination.mkdir(parents=True)
        argv = ["git", "archive", "--format=tar", snapshot]
        if archive_path:
            argv += ["--", archive_path]
        archive = subprocess.run(argv, cwd=source, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if archive.returncode != 0:
            raise BenchmarkError(
                f"git archive {snapshot} failed: {archive.stderr.decode('utf-8', 'replace')[:400]}"
            )
        extract = subprocess.run(
            ["tar", "-x", "-C", str(destination), f"--strip-components={strip}"],
            input=archive.stdout, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        if extract.returncode != 0:
            raise BenchmarkError(
                f"extracting {relative} failed: {extract.stderr.decode('utf-8', 'replace')[:400]}"
            )
    sc_source_provenance = record_legacy_sc_source_projection_provenance(
        source, evidence, snapshot
    )
    summary = promote_certified_cache(source, evidence)
    if sc_source_provenance is not None:
        summary["semantic_compression_source_provenance"] = sc_source_provenance
    summary["snapshot"] = snapshot
    summary["evidence"] = str(evidence)
    return summary


def cmd_cache_promote_evidence(args: argparse.Namespace) -> int:
    source = Path(args.source_repo).resolve()
    evidence = Path(args.evidence).resolve()
    if not (source / "benchmark" / "cache").is_dir():
        raise BenchmarkError(f"source repository has no certified cache: {source}")
    summary = promote_from_evidence(source, evidence, str(args.snapshot))
    print(json.dumps(summary, indent=2))
    return 0


def _checkout_path_for_workspace_relative(source: Path, relative: str) -> Path | None:
    """Map a workspace-relative read path to the checkout file or tree behind it."""
    if relative.startswith("template/"):
        return source / "benchmark" / relative
    if relative.startswith("repo/"):
        return source / relative[len("repo/"):]
    return None


def _hash_checkout_path(source: Path, path: Path) -> str | None:
    """Hash a checkout file or tree exactly as cache_read_input_hashes hashes it.

    Only git-tracked files count: the workspace snapshot is a `git archive`,
    so an untracked file in the checkout (an editor's swap file, a Finder
    .DS_Store) is not something any unit read.
    """
    if path.is_symlink() or not path.exists():
        return None
    listing = subprocess.run(
        ["git", "ls-files", "-z", "--", str(path.relative_to(source))],
        cwd=source, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if listing.returncode != 0:
        return None
    tracked = sorted(
        entry for entry in listing.stdout.decode("utf-8").split("\0") if entry
    )
    if path.is_file():
        return sha256_file(path) if tracked else None
    h = hashlib.sha256()
    for entry in tracked:
        child = source / entry
        if child.is_symlink() or not child.is_file():
            continue
        h.update(child.relative_to(path).as_posix().encode("utf-8"))
        h.update(b"\0")
        h.update(sha256_file(child).encode("ascii"))
        h.update(b"\n")
    return h.hexdigest()


def cache_impact(source: Path) -> dict[str, Any]:
    """Which certified records the checkout's current inputs would no longer match.

    A record's key hashes the readable inputs its unit read, the frozen
    configuration and methodology behind it, the toolchain pins of its
    languages, the declared epoch and, for Quidra, the snapshot's declared
    versions. Most of those are files in this checkout, so a template edit can
    be checked against every record before it is pushed - the check this
    command exists for was missed once by hand, when a one-line edit to the
    workload table re-keyed thirty-six Language Quality records and a scoped
    run paid to measure them again. The exact Task Packet hash is not
    recomputed here (it needs a rendered workspace), so a change to a prompt
    component alone is reported by the synthetic run, not by this command.
    """
    cache_root = source / "benchmark" / "cache" / "v1"
    record_paths = sorted(cache_root.rglob("*.json")) if cache_root.is_dir() else []
    if not record_paths:
        return {
            "schema_version": 1,
            "valid": 0,
            "invalid": [],
            "note": (
                "No certified cache records exist yet; this is a valid cold/first-run "
                "state and requires no invalidation."
            ),
        }
    template = source / "benchmark" / "template"
    pins = (json_load(template / "runtime" / "toolchains.json").get("toolchains") or {})
    policy = json_load(template / "config" / "cache_policy.json")
    declared = policy.get("declared_epochs") or {}
    target_language = str(policy.get("target_language") or "Quidra")
    current_target_execution = quidra_execution_identity_from_git(
        source, "HEAD", quidra_execution_input_paths(policy)
    )
    target_identity_policy = policy.get("quidra_execution_identity") or {}
    legacy_target_baseline = target_identity_policy.get("legacy_baseline") or {}
    legacy_verified_prefixes = {
        str(value)
        for value in (target_identity_policy.get("legacy_verified_commit_prefixes") or [])
    }
    epochs = policy.get("epochs") or {}
    versions: dict[str, str] | None = None
    project = source / "project.toml"
    if project.is_file():
        text = project.read_text(encoding="utf-8")
        found = {
            key: match.group(1)
            for key in ("version", "language_version")
            for match in [re.search(rf'^{key} = "([^"]*)"$', text, re.M)]
            if match
        }
        versions = found if len(found) == 2 else None
    unit_input_files = {
        "benchmark_metadata": template / "config" / "benchmark_metadata.json",
    }
    pin_keys = {
        "Python": ["PYTHON_PIN"],
        "C++": ["CLANG_PIN", "CLANG_MAJOR", "CMAKE_PIN"],
        "Rust": ["RUST_PIN"],
        "Go": ["GO_PIN"],
        "Java": ["JAVA_PIN", "JAVA_BUILD"],
        "TypeScript": ["TYPESCRIPT_PIN", "NODE_PIN"],
        "Kotlin": ["KOTLIN_PIN", "JAVA_PIN", "JAVA_BUILD"],
        "Swift": ["SWIFT_PIN"],
        "Zig": ["ZIG_PIN"],
    }
    hash_cache: dict[str, str | None] = {}
    invalid: list[dict[str, Any]] = []
    superseded: list[dict[str, Any]] = []
    valid = 0
    ecosystem_snapshot_cfg = (
        (policy.get("reuse_conditions") or {}).get(
            "ecosystem_snapshot_recertification"
        )
    )
    ecosystem_snapshot: dict[str, Any] | None = None
    ecosystem_snapshot_path: Path | None = None
    ecosystem_snapshot_common_ok = False
    ecosystem_snapshot_quidra_ok = False
    if isinstance(ecosystem_snapshot_cfg, dict):
        relative = str(ecosystem_snapshot_cfg.get("path") or "")
        rel_path = PurePosixPath(relative)
        if (
            relative
            and not rel_path.is_absolute()
            and not any(part in {"", ".", ".."} for part in rel_path.parts)
        ):
            candidate = source / "benchmark" / "cache" / rel_path
            if candidate.is_file():
                try:
                    ecosystem_snapshot = json_load(candidate)
                    ecosystem_snapshot_path = candidate
                    rubric_asset = json_load(
                        template / "methodology-assets" / "ecosystem" / "rubrics.json"
                    )
                    ecosystem_snapshot_common_ok = bool(
                        ecosystem_snapshot.get("schema_version") == 1
                        and ecosystem_snapshot.get("frozen") is True
                        and ecosystem_snapshot.get("rubric_set_id")
                        == rubric_asset.get("rubric_set_id")
                        and ecosystem_snapshot.get("snapshot_date")
                        == (rubric_asset.get("evidence_policy") or {}).get(
                            "snapshot_date"
                        )
                        and ecosystem_snapshot.get("cache_epoch")
                        == declared.get("ecosystem")
                    )
                    ecosystem_snapshot_quidra_ok = bool(
                        ecosystem_snapshot_common_ok
                        and ecosystem_snapshot.get("quidra_execution_identity")
                        == current_target_execution
                    )
                except (OSError, json.JSONDecodeError, TypeError, ValueError):
                    ecosystem_snapshot = None
                    ecosystem_snapshot_path = None
                    ecosystem_snapshot_common_ok = False
                    ecosystem_snapshot_quidra_ok = False
    for record_path in record_paths:
        try:
            record = json_load(record_path)
        except (OSError, json.JSONDecodeError, TypeError, ValueError) as exc:
            invalid.append({
                "record": record_path.relative_to(source).as_posix(),
                "work_unit_id": None,
                "evaluation": "unknown",
                "changed": [
                    f"record_unreadable_or_corrupt:{type(exc).__name__}:{exc}"
                ],
            })
            # One damaged historical record is a leaf-local invalidation. Keep
            # auditing every other paid record so preflight can still determine
            # the unaffected reuse set.
            continue
        payload = record.get("fingerprint_payload") or {}
        changed: list[str] = []
        for relative, recorded in (payload.get("readable_input_content_hashes") or {}).items():
            target = _checkout_path_for_workspace_relative(source, relative)
            if target is None:
                continue
            key = str(target)
            if key not in hash_cache:
                hash_cache[key] = _hash_checkout_path(source, target)
            if hash_cache[key] != recorded:
                changed.append(relative)
        unit_hashes = payload.get("unit_input_hashes") or {}
        for name, path in unit_input_files.items():
            if name in unit_hashes and path.is_file() and sha256_file(path) != unit_hashes[name]:
                changed.append(name)
        evaluation = str(payload.get("evaluation") or record.get("evaluation") or "")
        if evaluation == "ecosystem" and ecosystem_snapshot_common_ok:
            assigned = [str(value) for value in (payload.get("assigned_languages") or [])]
            snapshot_covers_record = (
                target_language not in assigned or ecosystem_snapshot_quidra_ok
            )
            if snapshot_covers_record:
                superseded.append({
                    "record": record_path.relative_to(source).as_posix(),
                    "work_unit_id": (record.get("provenance") or {}).get("work_unit_id"),
                    "evaluation": evaluation,
                    "reason": "covered by frozen runner-rubric-v2 snapshot recertification",
                    "snapshot": (
                        ecosystem_snapshot_path.relative_to(
                            source / "benchmark" / "cache"
                        ).as_posix()
                        if ecosystem_snapshot_path is not None
                        else None
                    ),
                })
                continue
        if "primary_config" in unit_hashes:
            current_primary = primary_config_projection_from_data(
                json_load(template / "config" / "primary.json"), evaluation
            )
            current_primary_hash = sha256_bytes(
                json.dumps(
                    current_primary,
                    sort_keys=True,
                    separators=(",", ":"),
                    ensure_ascii=False,
                ).encode("utf-8")
            )
            if unit_hashes["primary_config"] != current_primary_hash:
                historical = _primary_config_from_prompt_store(
                    template / "prompts",
                    str(payload.get("exact_task_packet_sha256") or ""),
                )
                if historical is None or primary_config_projection_from_data(
                    historical, evaluation
                ) != current_primary:
                    changed.append("primary_config")
        # The methodology dependency is now scoped to the sections embedded in
        # each Task Packet. Legacy whole-file hashes are intentionally not treated
        # as invalid here: hydrate_certified_cache compares the stored selected
        # component body before accepting one of those records.
        for language, recorded_pins in (payload.get("runtime_toolchain_pins") or {}).items():
            current = {key: pins.get(key) for key in pin_keys.get(language, [])}
            if recorded_pins and current != recorded_pins:
                changed.append(f"runtime_toolchain_pins:{language}")
        epoch_name = "mechanical" if payload.get("result_kind") == "mechanical" else evaluation
        if str(epochs.get(epoch_name, "stable")) == "declared":
            if payload.get("cache_epoch") != declared.get(epoch_name):
                changed.append("cache_epoch")
        for name, recorded in (payload.get("measurement_script_hashes") or {}).items():
            script = template / "scripts" / name
            if not script.is_file() or sha256_file(script) != recorded:
                changed.append(f"measurement_script:{name}")
        if "quidra_target" in payload and versions is not None and payload["quidra_target"] != versions:
            changed.append("quidra_target")
        if target_language in (payload.get("assigned_languages") or []):
            recorded_identity = (
                (record.get("compatibility") or {}).get("quidra_execution_identity")
            )
            if isinstance(recorded_identity, dict):
                if (
                    recorded_identity.get("sha256")
                    != current_target_execution.get("sha256")
                    or recorded_identity.get("git_objects")
                    != current_target_execution.get("git_objects")
                ):
                    changed.append("quidra_execution_identity")
            else:
                prefix = _legacy_cache_run_commit_prefix(record)
                if prefix not in legacy_verified_prefixes:
                    changed.append("quidra_execution_identity:legacy-unverified")
                elif (
                    legacy_target_baseline.get("git_objects")
                    != current_target_execution.get("git_objects")
                ):
                    changed.append("quidra_execution_identity:legacy-baseline-changed")
        entry = {
            "record": record_path.relative_to(source).as_posix(),
            "work_unit_id": (record.get("provenance") or {}).get("work_unit_id"),
            "evaluation": evaluation,
            "changed": changed,
        }
        if changed:
            invalid.append(entry)
        else:
            valid += 1
    return {
        "schema_version": 1,
        "valid": valid,
        "invalid": invalid,
        "superseded": superseded,
        "superseded_count": len(superseded),
        "note": "exact_task_packet_sha256 is not recomputed here; scoped methodology "
                "or assigned-requirement prompt changes show up in the synthetic run, "
                "while legacy scoped records are revalidated during cache hydration. "
                "Historical Ecosystem records covered by the frozen v2 snapshot are "
                "reported as superseded rather than paid misses.",
    }


def cmd_cache_impact(args: argparse.Namespace) -> int:
    source = Path(args.source_repo).resolve()
    if not source.is_dir():
        raise BenchmarkError(f"source repository does not exist: {source}")
    # An empty/missing v1 directory is a valid first-run state, not a benchmark
    # failure.  Report zero reusable records so preflight can still produce an
    # execution plan before the first paid request.
    summary = cache_impact(source)
    by_eval: dict[str, int] = {}
    for entry in summary["invalid"]:
        by_eval[entry["evaluation"]] = by_eval.get(entry["evaluation"], 0) + 1
    summary["invalid_by_evaluation"] = by_eval
    output = getattr(args, "output", None)
    if output:
        destination = Path(output)
        if not destination.is_absolute():
            destination = source / destination
        destination.parent.mkdir(parents=True, exist_ok=True)
        json_dump(destination, summary)
    print(json.dumps(summary, indent=2))
    return 0 if not summary["invalid"] else 3


def cmd_cache_annotate_caps(args: argparse.Namespace) -> int:
    source = Path(args.source_repo).resolve()
    evidence = Path(args.evidence).resolve()
    if not (source / "benchmark" / "cache").is_dir():
        raise BenchmarkError(f"source repository has no certified cache: {source}")
    if not (evidence / "run.json").is_file():
        raise BenchmarkError(f"evidence directory has no run.json: {evidence}")
    summary = annotate_cache_cap_evidence(source, evidence, force=bool(args.force))
    print(json.dumps(summary, indent=2))
    return 0


def mechanical_certification(
    root: Path, unit: dict[str, Any], command_dir: Path, result: dict[str, Any]
) -> dict[str, Any]:
    """What certifies a mechanical measurement: its validator passed, the unit
    completed, and the raw samples it was normalized from are identified by
    hash. The raw files are megabytes of process captures; they stay in the
    run's retained workspace artifact rather than in git."""
    raw_hashes: dict[str, str] = {}
    for child in sorted(command_dir.iterdir()):
        if child.name in ("result.json", "cache_receipt.json"):
            continue
        if child.is_file():
            raw_hashes[child.name] = sha256_file(child)
        elif child.is_dir():
            # One hash per capture tree (the adversarial cells hold thousands
            # of files); the artifact keeps the files themselves.
            raw_hashes[child.name + "/"] = sha256_tree(child)
    return {
        "validator_pass": True,
        "unit_complete": True,
        "primary_complete": False,
        "mechanical": True,
        "runner_action": str(unit.get("runner_action")),
        "raw_evidence_sha256": raw_hashes,
        "raw_evidence_retained_in": "the run's workspace artifact (provenance.run_id)",
        "measured_at_utc": result.get("measured_at_utc"),
    }


def unresolved_semantic_comparability_probes(
    root: Path, manifest: dict[str, Any]
) -> set[str]:
    """Probes a terminal failed comparability audit says must be revalidated."""
    for unit in manifest.get("work_units", []):
        if COMPARABILITY_GATE not in (unit.get("requirement_ids") or []):
            continue
        result_path = (
            root / "work" / "agents" / str(unit.get("assigned_agent_id"))
            / "result.json"
        )
        if not result_path.is_file():
            return set()
        result = json_load(result_path)
        if (result.get("requirements") or {}).get(COMPARABILITY_GATE) is not False:
            return set()
        return set(comparability_revalidation_probes(result))
    return set()


def unresolved_semantic_comparability_pairs(
    root: Path, manifest: dict[str, Any]
) -> set[tuple[str, str]]:
    """Resolve failed blinded audit pairs back to the affected languages.

    A terminal comparability failure can mean the canonical fragment/support
    measurement itself needs to be redone, not merely the cohort adjudication.
    Preserve cache for unaffected languages, but quarantine every Semantic
    metric shard of an affected language so a new run cannot hydrate the same
    suspect fragment and deterministically reproduce the blocker.
    """
    for unit in manifest.get("work_units", []):
        if COMPARABILITY_GATE not in (unit.get("requirement_ids") or []):
            continue
        result_path = (
            root / "work" / "agents" / str(unit.get("assigned_agent_id"))
            / "result.json"
        )
        if not result_path.is_file():
            return set()
        result = json_load(result_path)
        if (result.get("requirements") or {}).get(COMPARABILITY_GATE) is not False:
            return set()
        evidence = result.get("evidence") or {}
        gate = evidence.get("gate_result") or {}
        rows = gate.get("affected_pairs_requiring_revalidation")
        if rows is None:
            rows = evidence.get("affected_pairs_requiring_revalidation")
        if not isinstance(rows, list):
            return set()

        blinding_path = root / COMPARABILITY_BLINDING_RELATIVE
        labels: dict[str, str] = {}
        if blinding_path.is_file():
            raw = (json_load(blinding_path).get("labels") or {})
            labels = {str(label): str(language) for language, label in raw.items()}

        pairs: set[tuple[str, str]] = set()
        unresolved_probes: set[str] = set()
        unknown_label = False
        for row in rows:
            if not isinstance(row, dict):
                continue
            probe = str(row.get("probe_id") or "").upper().strip()
            label = str(row.get("label") or "").strip()
            if not re.fullmatch(r"F\d{2}\.P\d+", probe):
                continue
            unresolved_probes.add(probe)
            language = labels.get(label)
            if language:
                pairs.add((probe, language))
            else:
                unknown_label = True

        if unknown_label and unresolved_probes:
            # Losing the blinded label map must never make a failed measurement
            # look cache-safe. Fall back to remeasuring the affected probes for
            # every language rather than silently preserving suspect shards.
            for probe in unresolved_probes:
                for language in metadata_languages(root):
                    pairs.add((probe, language))
        return pairs
    return set()


def semantic_cache_quarantine_reason(
    unit: dict[str, Any],
    unresolved_probes: set[str],
    unresolved_pairs: set[tuple[str, str]],
) -> str | None:
    """Why one SC unit from a failed comparability run must not be certified."""
    if unit.get("evaluation") != "semantic_compression":
        return None
    adjudicated_probe = support_adjudication_probe(
        [str(value) for value in (unit.get("requirement_ids") or [])]
    )
    if adjudicated_probe in unresolved_probes:
        return (
            "final comparability audit requires this probe to be revalidated; "
            "its support adjudication is not cache-certified"
        )
    affected_languages = {language for _, language in unresolved_pairs}
    assigned = list(unit.get("assigned_languages") or [])
    canonical_probe = str(unit.get("canonical_probe_id") or "")
    if (
        len(assigned) == 1
        and canonical_probe
        and (canonical_probe, str(assigned[0])) in unresolved_pairs
    ):
        return (
            "final comparability audit requires this exact probe×language "
            "canonical fragment to be revalidated"
        )
    if (
        len(assigned) == 1
        and str(assigned[0]) in affected_languages
        and str(unit.get("id") or "").startswith("sc-metrics-")
    ):
        return (
            "final comparability audit affects "
            f"{assigned[0]}; its Semantic Compression metric shards are "
            "quarantined so the canonical fragment/support measurement is "
            "revalidated before reuse"
        )
    return None


def cache_record_metadata_refresh_required(
    existing: dict[str, Any], candidate: dict[str, Any]
) -> bool:
    """Whether same-result fresh measurement carries newer trust metadata."""
    if existing.get("result_sha256") != candidate.get("result_sha256"):
        return False
    return (
        (existing.get("compatibility") or {})
        != (candidate.get("compatibility") or {})
        or (existing.get("certification") or {})
        != (candidate.get("certification") or {})
        or (existing.get("migration") or {})
        != (candidate.get("migration") or {})
    )


def cache_record_migration_upgrade_required(
    existing: dict[str, Any], candidate: dict[str, Any]
) -> bool:
    """Allow a validated HIT to ratchet only missing migration provenance."""
    return (
        existing.get("result_sha256") == candidate.get("result_sha256")
        and not isinstance(existing.get("migration"), dict)
        and isinstance(candidate.get("migration"), dict)
        and (existing.get("compatibility") or {})
        == (candidate.get("compatibility") or {})
        and (existing.get("certification") or {})
        == (candidate.get("certification") or {})
    )


def cache_record_legacy_identity_upgrade_required(
    existing: dict[str, Any], candidate: dict[str, Any]
) -> bool:
    """Classify a same-result legacy Quidra record ratchet, not remeasurement."""
    if existing.get("result_sha256") != candidate.get("result_sha256"):
        return False
    old_compat = existing.get("compatibility") or {}
    new_compat = candidate.get("compatibility") or {}
    return (
        "quidra_execution_identity" not in old_compat
        and "quidra_execution_identity" in new_compat
        and (existing.get("certification") or {})
        == (candidate.get("certification") or {})
    )




def promote_certified_cache(source: Path, root: Path) -> dict[str, Any]:
    manifest_path = root / "work" / "root" / "manifest.json"
    ledger_path = root / "work" / "root" / "ledger.json"
    status_path = root / "results" / "primary_status.json"
    policy_path = root / "template" / "config" / "cache_policy.json"
    if not (manifest_path.is_file() and ledger_path.is_file() and policy_path.is_file()):
        return {"promoted": 0, "replaced": 0, "upgraded": 0, "reused": 0, "records": [], "skipped": []}

    promotion_policy = cache_policy(root).get("promotion") or {}
    if promotion_policy.get("require_complete_unit") is not True:
        raise BenchmarkError("certified cache promotion must require a COMPLETE unit")
    if promotion_policy.get("require_current_validator_pass") is not True:
        raise BenchmarkError("certified cache promotion must require current validator PASS")
    require_primary = bool(
        promotion_policy.get("require_complete_primary_evaluation", True)
    )
    primary = (
        json_load(status_path).get("evaluations") or {}
        if status_path.is_file()
        else {}
    )
    if require_primary and not status_path.is_file():
        return {"promoted": 0, "replaced": 0, "upgraded": 0, "reused": 0, "records": [], "skipped": []}

    # Preserve expensive language-scoped SC checkpoints after a failed audit,
    # but certify cohort adjudications/gate verdicts only from COMPLETE SC.
    def unit_requires_complete_primary(unit: dict[str, Any]) -> bool:
        if require_primary:
            return True
        if str(unit.get("evaluation") or "") != "semantic_compression":
            return False
        requirement_ids = [str(rid) for rid in (unit.get("requirement_ids") or [])]
        return support_adjudication_probe(requirement_ids) is not None or COMPARABILITY_GATE in requirement_ids

    manifest = json_load(manifest_path)
    ledger = json_load(ledger_path)
    unresolved_sc_probes = unresolved_semantic_comparability_probes(root, manifest)
    unresolved_sc_pairs = unresolved_semantic_comparability_pairs(root, manifest)
    records: list[dict[str, Any]] = []
    skipped: list[dict[str, Any]] = []
    promoted = 0
    replaced = 0
    upgraded = 0
    reused = 0

    for unit in manifest.get("work_units", []):
        if not cache_eligible_unit(root, unit):
            continue
        state = ledger.get("units", {}).get(unit["id"], {}) or {}
        if state.get("status") != "COMPLETE":
            continue
        if state.get("validation_result") != "PASS":
            continue
        quarantine_reason = semantic_cache_quarantine_reason(
            unit, unresolved_sc_probes, unresolved_sc_pairs
        )
        if quarantine_reason:
            skipped.append({
                "work_unit_id": unit.get("id"),
                "reason": quarantine_reason,
            })
            continue
        if (
            unit_requires_complete_primary(unit)
            and (primary.get(unit.get("evaluation")) or {}).get("status") != "COMPLETE"
        ):
            continue
        mechanical = mechanical_unit(unit)
        if mechanical:
            agent_dir = mechanical_result_path(root, unit).parent
            result_path = agent_dir / "result.json"
            if not result_path.is_file():
                continue
            candidate = json_load(result_path)
            if candidate.get("synthetic_ci") or (candidate.get("evidence") or {}).get("synthetic_ci"):
                # The synthetic CI flow stands in for the measurement scripts;
                # its numbers must never become certified measurements, and a
                # real record at the same key must not be overwritten by them.
                skipped.append({"work_unit_id": unit.get("id"), "reason": "synthetic mechanical result"})
                continue
            task = mechanical_task(unit)
        else:
            agent_dir = root / "work" / "agents" / str(unit["assigned_agent_id"])
            task_path = agent_dir / "task.json"
            result_path = agent_dir / "result.json"
            if not (task_path.is_file() and result_path.is_file()):
                continue
            task = json_load(task_path)
        pair = cache_fingerprint(root, unit, task)
        if pair is None:
            continue
        fingerprint, payload = pair
        result = json_load(result_path)
        result_raw = json.dumps(
            result, sort_keys=True, separators=(",", ":")
        ).encode("utf-8")
        receipt_path = agent_dir / "cache_receipt.json"
        migration_metadata = None
        if receipt_path.is_file():
            receipt = json_load(receipt_path)
            certification = (receipt.get("certification") or {})
            migration_metadata = receipt.get("migration")
            reused += 1
        elif mechanical:
            certification = mechanical_certification(root, unit, agent_dir, result)
        else:
            try:
                certification = cache_certification_for_unit(root, unit, agent_dir)
            except BenchmarkError as exc:
                # One unit that cannot be certified is one record fewer, not a
                # reason to keep every other validated measurement out of the
                # cache.
                skipped.append({"work_unit_id": unit.get("id"), "reason": str(exc)})
                continue
        compatibility: dict[str, Any] = {}
        target = str(cache_policy(root).get("target_language") or "Quidra")
        if target in (payload.get("assigned_languages") or []):
            try:
                execution_identity = current_quidra_execution_identity(root)
            except BenchmarkError as exc:
                # Historical retained workspaces can predate the trusted
                # Quidra execution-identity contract. That makes only their
                # Quidra leaf uncertifiable; it must not discard otherwise
                # certifiable comparison-language paid evidence from the same
                # run. The current-run compatibility layer may still recover a
                # Quidra record later only through an explicitly verified
                # legacy baseline.
                skipped.append({
                    "work_unit_id": unit.get("id"),
                    "reason": (
                        "cannot certify historical Quidra leaf without frozen "
                        f"execution identity: {exc}"
                    ),
                })
                continue
            if execution_identity is None:
                skipped.append({
                    "work_unit_id": unit.get("id"),
                    "reason": "cannot certify Quidra cache without frozen execution identity",
                })
                continue
            compatibility["quidra_execution_identity"] = execution_identity
        record = {
            "schema_version": 1,
            "fingerprint": fingerprint,
            "fingerprint_payload": payload,
            "evaluation": unit.get("evaluation"),
            "assigned_languages": list(unit.get("assigned_languages", [])),
            "result": result,
            "result_sha256": sha256_bytes(result_raw),
            "certification": certification,
            "provenance": {
                "run_id": json_load(root / "run.json").get("run_id"),
                "work_unit_id": unit.get("id"),
                "prompt_sha256": task.get("prompt_sha256"),
            },
        }
        if compatibility:
            record["compatibility"] = compatibility
        if isinstance(migration_metadata, dict):
            record["migration"] = migration_metadata
        relative = cache_record_relative(unit, fingerprint)
        destination = source / "benchmark" / "cache" / relative
        encoded = (json.dumps(record, indent=2, sort_keys=True) + "\n").encode("utf-8")
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists():
            destination.write_bytes(encoded)
            promoted += 1
        else:
            corrupt_existing = False
            try:
                existing_record = json.loads(destination.read_text(encoding="utf-8"))
                corrupt_existing = bool(_cache_record_self_integrity_problem(existing_record))
            except (OSError, json.JSONDecodeError, TypeError, ValueError):
                existing_record = {}
                corrupt_existing = True

            if corrupt_existing:
                # A fresh COMPLETE+PASS measurement repairs only this exact
                # corrupted record. Never let one broken cache file prevent
                # checkpointing every other successful paid leaf.
                if receipt_path.is_file():
                    skipped.append({
                        "work_unit_id": unit.get("id"),
                        "reason": (
                            "hydrated result points at a corrupt certified record; "
                            "fresh execution is required before replacement"
                        ),
                    })
                    continue
                destination.write_bytes(encoded)
                replaced += 1
            else:
                same_result = (
                    existing_record.get("result_sha256") == record["result_sha256"]
                )
                if (
                    same_result
                    and not receipt_path.is_file()
                    and cache_record_metadata_refresh_required(existing_record, record)
                ):
                    # A verified identity-less legacy record can be ratcheted to
                    # the current explicit identity without changing its result.
                    destination.write_bytes(encoded)
                    if cache_record_legacy_identity_upgrade_required(
                        existing_record, record
                    ):
                        upgraded += 1
                    else:
                        replaced += 1
                elif (
                    same_result
                    and receipt_path.is_file()
                    and cache_record_migration_upgrade_required(
                        existing_record, record
                    )
                ):
                    destination.write_bytes(encoded)
                    upgraded += 1
                elif same_result:
                    pass
                elif receipt_path.is_file():
                    # A hydrated result cannot legitimately differ from the record
                    # that produced it. Keep the stored record and report this unit.
                    skipped.append({
                        "work_unit_id": unit.get("id"),
                        "reason": (
                            "hydrated result differs from the certified record it came "
                            f"from: {fingerprint}"
                        ),
                    })
                    continue
                else:
                    # A fresh measurement supersedes the old record at the same
                    # historical fingerprint. Reuse policy is what forced rerun.
                    destination.write_bytes(encoded)
                    replaced += 1
        records.append({
            "work_unit_id": unit.get("id"),
            "fingerprint": fingerprint,
            "path": str(relative.as_posix()),
            "assigned_languages": list(unit.get("assigned_languages", [])),
        })
    return {
        "promoted": promoted,
        "replaced": replaced,
        "upgraded": upgraded,
        "reused": reused,
        "records": records,
        "skipped": skipped,
    }



def partial_paid_store_manifest(store: Path, fingerprint: str) -> Path:
    return store / "v1" / fingerprint[:2] / fingerprint / "manifest.json"


def partial_paid_store_blob(store: Path, digest: str) -> Path:
    return store / "blobs" / digest[:2] / digest


def _partial_paid_file_record(
    store: Path,
    root: Path,
    source: Path,
    restore_relative: str,
) -> dict[str, Any]:
    source = require_under(source, root)
    data = source.read_bytes()
    digest = sha256_bytes(data)
    blob = partial_paid_store_blob(store, digest)
    blob.parent.mkdir(parents=True, exist_ok=True)
    if blob.is_file():
        if sha256_file(blob) != digest:
            raise BenchmarkError(
                f"partial paid checkpoint blob is corrupt: {blob}"
            )
    else:
        tmp = blob.with_name(blob.name + ".tmp")
        tmp.write_bytes(data)
        os.replace(tmp, blob)
    return {
        "restore_relative": restore_relative,
        "sha256": digest,
        "bytes": len(data),
    }


def _partial_paid_call_count(agent_dir: Path, worker_mode: str) -> int:
    if worker_mode == "packet-only":
        return len(_packet_paid_response_records(agent_dir))
    journal = agent_dir / "trial_call_journal.json"
    if not journal.is_file():
        return 0
    try:
        payload = json_load(journal)
    except (OSError, json.JSONDecodeError, TypeError, ValueError):
        return 0
    calls = payload.get("calls")
    return len(calls) if isinstance(calls, list) else 0


def export_partial_paid_checkpoints(
    root: Path,
    store: Path,
    evaluation: str | None = None,
) -> dict[str, Any]:
    """Persist already-paid model-call state independently of leaf completion.

    Each record is keyed by the same full dependency fingerprint used by the
    certified result cache. The store is content-addressed. Production keeps it
    in private Actions artifacts, with Actions cache as a speed mirror, so hidden
    prompts/completions/trial verification never enter public Git. These bytes
    never certify a score by themselves: import restores them only into a PENDING
    exact-fingerprint leaf, after which current parsing, runtime verification and
    validators still apply.
    """
    manifest = json_load(root / "work" / "root" / "manifest.json")
    ledger = json_load(root / "work" / "root" / "ledger.json")
    exported: list[dict[str, Any]] = []
    skipped: list[dict[str, Any]] = []
    updated_unit_count = 0

    for unit in manifest.get("work_units", []):
        if evaluation is not None and unit.get("evaluation") != evaluation:
            continue
        if unit.get("execution_kind", "agent") != "agent":
            continue
        uid = str(unit["id"])
        state = (ledger.get("units", {}).get(uid) or {})
        # Preserve paid inference even after the leaf reaches COMPLETE. The
        # certified result cache stores the validated score/result; this store
        # keeps the paid raw calls needed to re-parse or re-validate that same
        # semantic experiment later without purchasing identical calls again.
        agent_id = str(unit.get("assigned_agent_id") or "")
        agent_dir = root / "work" / "agents" / agent_id
        task_path = agent_dir / "task.json"
        if not task_path.is_file():
            continue
        task = json_load(task_path)
        pair = cache_fingerprint(root, unit, task)
        if pair is None:
            continue
        fingerprint, payload = pair
        worker_mode = str(unit.get("worker_mode") or "packet-only")
        paid_calls = _partial_paid_call_count(agent_dir, worker_mode)
        if paid_calls <= 0:
            continue

        file_specs: list[tuple[Path, str]] = []
        agent_prefix = f"work/agents/{agent_id}"
        if worker_mode == "packet-only":
            paid_dir = agent_dir / "paid_responses"
            for source in sorted(p for p in paid_dir.rglob("*") if p.is_file()):
                rel = source.relative_to(agent_dir).as_posix()
                file_specs.append((source, f"{agent_prefix}/{rel}"))
        elif worker_mode == "sandbox-agent":
            trials = agent_dir / "trials"
            if trials.is_dir():
                for source in sorted(p for p in trials.rglob("*") if p.is_file()):
                    rel = source.relative_to(agent_dir).as_posix()
                    file_specs.append((source, f"{agent_prefix}/{rel}"))
            for name in (
                "trial_call_journal.json",
                "learnability_preflight.json",
                "learnability_leakage.json",
            ):
                source = agent_dir / name
                if source.is_file():
                    file_specs.append((source, f"{agent_prefix}/{name}"))

            trace_source = None
            for name in (
                "agent_trace.partial.json",
                "agent_trace.json",
                "resume_trace.json",
            ):
                candidate = agent_dir / name
                if candidate.is_file():
                    trace_source = candidate
                    break
            if trace_source is not None:
                file_specs.append(
                    (trace_source, f"{agent_prefix}/resume_trace.json")
                )

            # Proficiency session records point at trusted verifier evidence
            # outside the worker directory. Preserve only referenced files.
            verification_paths: set[Path] = set()
            if trials.is_dir():
                for session_path in trials.glob("*/session.json"):
                    try:
                        session = json_load(session_path)
                    except (OSError, json.JSONDecodeError, TypeError, ValueError):
                        continue
                    for call in session.get("calls", []) or []:
                        raw = call.get("verification_path")
                        if not isinstance(raw, str) or not raw:
                            continue
                        candidate = require_under(root / raw, root)
                        if candidate.is_file():
                            verification_paths.add(candidate)
                        elif candidate.is_dir():
                            verification_paths.update(
                                p for p in candidate.rglob("*") if p.is_file()
                            )
            for source in sorted(verification_paths):
                rel = source.relative_to(root).as_posix()
                if not rel.startswith("work/root/proficiency-verification/"):
                    raise BenchmarkError(
                        f"unexpected trusted verification checkpoint path: {rel}"
                    )
                file_specs.append((source, rel))
        else:
            skipped.append({
                "work_unit_id": uid,
                "reason": f"unsupported worker mode {worker_mode!r}",
            })
            continue

        if not file_specs:
            continue
        files = [
            _partial_paid_file_record(store, root, source, restore_relative)
            for source, restore_relative in file_specs
        ]
        record = {
            "schema_version": 1,
            "fingerprint": fingerprint,
            "fingerprint_payload": payload,
            "work_unit_id": uid,
            "evaluation": unit.get("evaluation"),
            "agent_id": agent_id,
            "worker_mode": worker_mode,
            "prompt_sha256": task.get("prompt_sha256"),
            "paid_call_count": paid_calls,
            "files": files,
            "exported_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        }
        destination = partial_paid_store_manifest(store, fingerprint)
        keep_existing = False
        if destination.is_file():
            try:
                existing = json_load(destination)
                existing_calls = int(existing.get("paid_call_count", 0) or 0)
                existing_files = len(existing.get("files", []) or [])
                keep_existing = (
                    existing.get("fingerprint_payload") == payload
                    and (
                        existing_calls > paid_calls
                        or (
                            existing_calls == paid_calls
                            and existing_files >= len(files)
                        )
                    )
                )
            except (OSError, json.JSONDecodeError, TypeError, ValueError):
                keep_existing = False
        if not keep_existing:
            json_dump(destination, record)
            updated_unit_count += 1
        exported.append({
            "work_unit_id": uid,
            "fingerprint": fingerprint,
            "paid_call_count": max(
                paid_calls,
                int(
                    (json_load(destination).get("paid_call_count", 0) or 0)
                    if destination.is_file()
                    else paid_calls
                ),
            ),
            "record": destination.relative_to(store).as_posix(),
        })

    result = {
        "schema_version": 1,
        "exported_units": exported,
        "exported_unit_count": len(exported),
        "updated_unit_count": updated_unit_count,
        "skipped": skipped,
    }
    json_dump(root / "results" / "partial_paid_checkpoint_export.json", result)
    return result


def _partial_paid_restore_allowed(
    root: Path,
    agent_id: str,
    relative: str,
) -> Path:
    rel = PurePosixPath(relative)
    if rel.is_absolute() or any(part in {"", ".", ".."} for part in rel.parts):
        raise BenchmarkError(f"invalid paid checkpoint restore path: {relative!r}")
    agent_prefix = PurePosixPath("work") / "agents" / agent_id
    trusted_prefix = PurePosixPath("work") / "root" / "proficiency-verification"
    allowed_agent = False
    try:
        suffix = rel.relative_to(agent_prefix)
        text = suffix.as_posix()
        allowed_agent = (
            text == "trial_call_journal.json"
            or text == "learnability_preflight.json"
            or text == "learnability_leakage.json"
            or text == "resume_trace.json"
            or text.startswith("trials/")
            or text.startswith("paid_responses/")
        )
    except ValueError:
        pass
    allowed_trusted = False
    try:
        rel.relative_to(trusted_prefix)
        allowed_trusted = True
    except ValueError:
        pass
    if not (allowed_agent or allowed_trusted):
        raise BenchmarkError(
            f"paid checkpoint restore path is outside the allowed state: {relative}"
        )
    return require_under(root.joinpath(*rel.parts), root)


def import_partial_paid_checkpoints(
    root: Path,
    store: Path,
    evaluation: str | None = None,
) -> dict[str, Any]:
    """Restore exact-fingerprint paid state only into still-PENDING leaves.

    A checkpoint may originate from either an incomplete or previously COMPLETE
    leaf. In both cases it is only paid-call recovery material: current task
    execution and validation decide whether the new run becomes COMPLETE.
    """
    manifest = json_load(root / "work" / "root" / "manifest.json")
    ledger = json_load(root / "work" / "root" / "ledger.json")
    imported: list[dict[str, Any]] = []
    rejected: list[dict[str, Any]] = []

    for unit in manifest.get("work_units", []):
        if evaluation is not None and unit.get("evaluation") != evaluation:
            continue
        if unit.get("execution_kind", "agent") != "agent":
            continue
        uid = str(unit["id"])
        state = (ledger.get("units", {}).get(uid) or {})
        if state.get("status", "PENDING") != "PENDING":
            continue
        if not all(
            (ledger.get("units", {}).get(dep) or {}).get("status") == "COMPLETE"
            for dep in unit.get("dependencies", [])
        ):
            continue
        agent_id = str(unit.get("assigned_agent_id") or "")
        agent_dir = root / "work" / "agents" / agent_id
        task_path = agent_dir / "task.json"
        if not task_path.is_file():
            continue
        task = json_load(task_path)
        pair = cache_fingerprint(root, unit, task)
        if pair is None:
            continue
        fingerprint, payload = pair
        record_path = partial_paid_store_manifest(store, fingerprint)
        if not record_path.is_file():
            continue
        try:
            record = json_load(record_path)
        except (OSError, json.JSONDecodeError, TypeError, ValueError) as exc:
            rejected.append({
                "work_unit_id": uid,
                "reason": f"checkpoint manifest unreadable: {exc}",
            })
            continue
        if (
            record.get("fingerprint") != fingerprint
            or record.get("fingerprint_payload") != payload
            or record.get("work_unit_id") != uid
            or record.get("agent_id") != agent_id
            or record.get("prompt_sha256") != task.get("prompt_sha256")
            or record.get("worker_mode") != unit.get("worker_mode")
        ):
            rejected.append({
                "work_unit_id": uid,
                "reason": "checkpoint dependency identity does not exactly match",
            })
            continue

        staged: list[tuple[Path, bytes]] = []
        problem = None
        for file_row in record.get("files", []) or []:
            relative = str(file_row.get("restore_relative") or "")
            digest = str(file_row.get("sha256") or "")
            blob = partial_paid_store_blob(store, digest)
            if not blob.is_file() or sha256_file(blob) != digest:
                problem = f"missing/corrupt paid checkpoint blob for {relative}"
                break
            data = blob.read_bytes()
            if len(data) != int(file_row.get("bytes", -1)):
                problem = f"paid checkpoint byte count mismatch for {relative}"
                break
            try:
                target = _partial_paid_restore_allowed(root, agent_id, relative)
            except BenchmarkError as exc:
                problem = str(exc)
                break
            if target.exists() and target.read_bytes() != data:
                problem = f"current workspace conflicts with checkpoint file {relative}"
                break
            staged.append((target, data))
        if problem:
            rejected.append({"work_unit_id": uid, "reason": problem})
            continue

        for target, data in staged:
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists():
                tmp = target.with_name(target.name + ".partial-paid.tmp")
                tmp.write_bytes(data)
                os.replace(tmp, target)
        imported.append({
            "work_unit_id": uid,
            "fingerprint": fingerprint,
            "worker_mode": record.get("worker_mode"),
            "restored_paid_calls": int(record.get("paid_call_count", 0) or 0),
            "restored_files": len(staged),
            "record": record_path.relative_to(store).as_posix(),
        })

    result = {
        "schema_version": 1,
        "imported_units": imported,
        "imported_unit_count": len(imported),
        "restored_paid_calls": sum(
            int(row.get("restored_paid_calls", 0) or 0) for row in imported
        ),
        "rejected": rejected,
    }
    json_dump(root / "results" / "partial_paid_checkpoint_status.json", result)
    return result


def cmd_partial_paid_export(args: argparse.Namespace) -> int:
    root = workspace(args)
    store = lexical_absolute(Path(args.store))
    store.mkdir(parents=True, exist_ok=True)
    result = export_partial_paid_checkpoints(root, store, args.evaluation)
    print(json.dumps(result, indent=2))
    return 0


def cmd_partial_paid_import(args: argparse.Namespace) -> int:
    root = workspace(args)
    store = lexical_absolute(Path(args.store))
    if not store.is_dir():
        result = {
            "schema_version": 1,
            "imported_units": [],
            "imported_unit_count": 0,
            "restored_paid_calls": 0,
            "rejected": [],
            "note": "no cross-run paid checkpoint cache was available",
        }
        json_dump(root / "results" / "partial_paid_checkpoint_status.json", result)
        print(json.dumps(result, indent=2))
        return 0
    result = import_partial_paid_checkpoints(root, store, args.evaluation)
    print(json.dumps(result, indent=2))
    return 0


def cache_record_prompt_hashes(
    root: Path, promotion: dict[str, Any]
) -> set[str]:
    """Prompt dependencies for exactly the certified records in one checkpoint."""
    promoted_ids = {
        str(row.get("work_unit_id") or "")
        for row in (promotion.get("records") or [])
        if str(row.get("work_unit_id") or "")
    }
    if not promoted_ids:
        return set()
    manifest = json_load(root / "work" / "root" / "manifest.json")
    hashes: set[str] = set()
    for unit in manifest.get("work_units", []):
        if str(unit.get("id") or "") not in promoted_ids:
            continue
        if unit.get("execution_kind", "agent") != "agent":
            continue
        agent_id = str(unit.get("assigned_agent_id") or "")
        task_path = root / "work" / "agents" / agent_id / "task.json"
        if not task_path.is_file():
            continue
        prompt_hash = str(json_load(task_path).get("prompt_sha256") or "")
        if re.fullmatch(r"[0-9a-f]{64}", prompt_hash):
            hashes.add(prompt_hash)
    return hashes


def cmd_cache_checkpoint(args: argparse.Namespace) -> int:
    """Promote independently validated cache records without requiring finalize.

    This command is trusted-host only.  It runs after the scored sandbox has
    exited, so records written into the source checkout cannot become readable
    inputs to the run that produced them.  The workspace is deliberately kept
    intact: a later successful finalize/post-run may still import the compact run.
    """
    source = Path(args.source_repo).resolve()
    if not source.is_dir():
        raise BenchmarkError(f"source repository does not exist: {source}")
    root = host_workspace(source)
    if not root.is_dir():
        raise BenchmarkError(f"benchmark workspace does not exist: {root}")
    run_path = root / "run.json"
    if not run_path.is_file():
        raise BenchmarkError("benchmark workspace is missing run.json")
    run = json_load(run_path)
    validate_host_workspace_sentinel(root, source, run)

    meta = git_metadata(source)
    if meta["working_tree_status"] != "clean":
        raise BenchmarkError("cache checkpoint requires a clean source repository")

    promotion = promote_certified_cache(source, root)
    # A certified result is only maximally reusable if the exact prompt
    # dependencies needed for later compatibility/revalidation survive too.
    # Store only prompts for records this checkpoint actually certified; raw
    # paid calls remain private in the paid-state store.
    prompt_hashes = cache_record_prompt_hashes(root, promotion)
    prompt_promotion = promote_prompt_store(
        source, root, prompt_hashes=prompt_hashes
    )
    result = {
        "schema_version": 1,
        "ok": True,
        "run_id": run.get("run_id"),
        "evaluated_commit_sha": (run.get("evaluated") or {}).get("commit_sha"),
        "prompt_store": prompt_promotion,
        **promotion,
    }
    print(json.dumps(result, indent=2))
    return 0


def requirement_evidence_sources(
    root: Path, evaluation: str
) -> tuple[dict[str, list[dict[str, Any]]], list[dict[str, Any]]]:
    """Which unit measured each requirement, and the evidence that unit wrote.

    `requirement_results_for_evaluation` merges the scores and drops everything
    else, so a published ranking keeps no trace of who measured a cell or why
    they scored it that way.  This collects both.  A source the reconstruction
    cannot reach is skipped rather than raised on: a command unit names its
    result by absolute in-container path, which does not resolve when a finished
    run is reported on again from outside the sandbox.
    """
    manifest = json_load(root / "work" / "root" / "manifest.json")
    by_requirement: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
    records: list[dict[str, Any]] = []
    for unit in manifest.get("work_units", []):
        if unit.get("evaluation") != evaluation or unit.get("phase") == "aggregation":
            continue
        if unit.get("execution_kind", "agent") not in {"agent", "command"}:
            continue
        if unit.get("result_kind") == "audit":
            continue
        agent_id = str(unit.get("assigned_agent_id") or unit["id"])
        candidates = [root / "work" / "agents" / agent_id / "result.json"]
        for raw in unit.get("evidence_paths", []):
            candidates.append(resolve_evidence_path(root, raw))
        result = None
        for candidate in candidates:
            try:
                if candidate.is_file():
                    result = json_load(candidate)
                    break
            except (OSError, json.JSONDecodeError):
                continue
        if result is None:
            continue
        requirement_ids = sorted(
            rid
            for rid in (result.get("requirements") or {})
            if rid.startswith("metric.") or rid.startswith("condition.")
        )
        if not requirement_ids:
            continue
        record = {
            "work_unit_id": str(unit["id"]),
            "agent_id": agent_id,
            "assigned_languages": list(unit.get("assigned_languages") or []),
            "requirement_ids": requirement_ids,
            "requirements": result.get("requirements") or {},
            "evidence": result.get("evidence"),
        }
        if evaluation == "llm_proficiency":
            trusted_path = (
                root / "work" / "agents" / agent_id
                / "proficiency_runtime_verification.json"
            )
            if trusted_path.is_file():
                record["trusted_runtime_verification"] = json_load(trusted_path)
        records.append(record)
        for rid in requirement_ids:
            by_requirement[rid].append(
                {
                    "work_unit_id": str(unit["id"]),
                    "agent_id": agent_id,
                    "assigned_languages": list(unit.get("assigned_languages") or []),
                }
            )
    return by_requirement, records


def requirement_breakdown_rows(
    config: dict[str, Any],
    req: dict[str, Any],
    languages: list[str],
    sources: dict[str, list[dict[str, Any]]],
) -> list[dict[str, Any]]:
    """One row per scored requirement: its cells, its own ranking, its weight."""
    typ = config["type"]
    weights: dict[str, float] = {}
    category_of: dict[str, str] = {}
    if typ == "weighted_mean":
        weights = {rid: float(w) for rid, w in (config.get("weights") or {}).items()}
    elif typ == "semantic_harmonic":
        weights = {
            rid: float(w) for rid, w in (config.get("quality_weights") or {}).items()
        }
    elif typ == "category_mean":
        for name, category in config["categories"].items():
            for rid in category["metrics"]:
                category_of[rid] = name

    ordered = list(weights) + [rid for rid in category_of if rid not in weights]
    ordered += [
        rid
        for rid in sorted(req)
        if (rid.startswith("metric.") or rid.startswith("condition."))
        and rid not in ordered
    ]

    rows = []
    for rid in ordered:
        cells = req.get(rid)
        if not isinstance(cells, dict):
            continue
        scores: dict[str, float] = {}
        not_applicable: dict[str, str] = {}
        for language in languages:
            if language not in cells:
                continue
            value = cells[language]
            score = score_or_na(value)
            if score is None:
                not_applicable[language] = str(value.get("reason") or "").strip()
            else:
                scores[language] = score
        row = {
            "requirement_id": rid,
            "scores": {language: scores[language] for language in languages if language in scores},
            "ranking": deterministic_ranking(scores, languages) if scores else [],
            "not_applicable": not_applicable,
            "measured_by": sources.get(rid, []),
        }
        if rid in weights:
            row["weight"] = weights[rid]
        if rid in category_of:
            row["category"] = category_of[rid]
        if typ == "semantic_harmonic" and rid == config.get("coverage_metric"):
            row["role"] = "coverage"
        rows.append(row)
    return rows


def evaluation_breakdown(
    root: Path,
    evaluation: str,
    config: dict[str, Any],
    req: dict[str, Any],
    languages: list[str],
    published: dict[str, Any],
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Decompose a published Primary score into the cells it was computed from.

    Every figure is read back through the same `weighted_score` /
    `category_score` the aggregate itself used, and `reconstruction` records the
    largest disagreement with the published score, so a reader can tell a real
    decomposition from a parallel calculation that merely looks plausible.
    """
    sources, records = requirement_evidence_sources(root, evaluation)
    rows = requirement_breakdown_rows(config, req, languages, sources)
    typ = config["type"]

    categories: list[dict[str, Any]] = []
    if typ == "category_mean":
        total_weight = sum(
            float(category["weight"]) for category in config["categories"].values()
        )
        for name, category in config["categories"].items():
            weight = float(category["weight"])
            means: dict[str, float] = {}
            for language in languages:
                values = [
                    score
                    for rid in category["metrics"]
                    if isinstance(req.get(rid), dict) and language in req[rid]
                    for score in [score_or_na(req[rid][language])]
                    if score is not None
                ]
                if values:
                    means[language] = sum(values) / len(values)
            categories.append(
                {
                    "category": name,
                    "weight": weight,
                    "metrics": list(category["metrics"]),
                    "means": means,
                    "contribution": {
                        language: weight * value / total_weight
                        for language, value in means.items()
                    },
                    "ranking": deterministic_ranking(means, languages) if means else [],
                }
            )

    recomputed: dict[str, float] = {}
    for language in languages:
        try:
            if typ == "weighted_mean":
                value = weighted_score(config["weights"], req, language)
            elif typ == "category_mean":
                value = category_score(config["categories"], req, language)
            elif typ == "semantic_harmonic":
                quality = weighted_score(config["quality_weights"], req, language)
                coverage = score_or_na(req[config["coverage_metric"]][language])
                if quality is None or coverage is None:
                    value = None
                elif quality + coverage == 0:
                    value = 0.0
                else:
                    value = 2.0 * quality * coverage / (quality + coverage)
            else:
                value = None
        except (BenchmarkError, KeyError, TypeError, ZeroDivisionError):
            # The recomputation is a check on the published score, not a second
            # source of it: an incomplete requirement set (a blocked evaluation,
            # a language a shard never covered) means this evaluation has no
            # reconstruction to report, never a failed import.
            value = None
        if value is not None:
            recomputed[language] = float(value)

    published_scores = published.get("scores") or {}
    deltas = {
        language: abs(recomputed[language] - float(published_scores[language]))
        for language in recomputed
        if language in published_scores
    }
    breakdown = {
        "schema_version": 1,
        "evaluation": evaluation,
        "status": published.get("status"),
        "aggregation": config,
        "languages": list(languages),
        "published": {
            "score": published.get("score"),
            "scores": published_scores or None,
            "ranking": published.get("ranking"),
            "blockers": published.get("blockers") or [],
        },
        "categories": categories,
        "requirements": rows,
        "reconstruction": {
            "recomputed_scores": recomputed,
            "max_abs_error_vs_published": max(deltas.values()) if deltas else None,
            "note": (
                "Recomputed with the same functions the aggregate used. A null "
                "error means the evaluation published no scores to compare against."
            ),
        },
    }
    return breakdown, records

def render_breakdown_markdown(breakdown: dict[str, Any]) -> str:
    """The same decomposition as a table a reader can scan without a JSON tool."""
    languages = list(breakdown["languages"])
    display = PRIMARY_DISPLAY_NAMES.get(breakdown["evaluation"], breakdown["evaluation"])
    lines = [f"# {display}: score breakdown", ""]
    lines.append(f"Status: **{breakdown['status']}**")
    published = breakdown["published"]
    if published.get("ranking"):
        lines += ["", "## Published ranking", "", "| Rank | Language | Score |", "| ---: | --- | ---: |"]
        for entry in published["ranking"]:
            lines.append(f"| {entry['rank']} | {entry['language']} | {entry['score']} |")
    for blocker in published.get("blockers") or []:
        lines += ["", f"> BLOCKED: {blocker.get('reason')} ({blocker.get('work_unit_id')})"]

    if breakdown["categories"]:
        lines += ["", "## Categories", "", "| Category | Weight | " + " | ".join(languages) + " |"]
        lines.append("| --- | ---: |" + " ---: |" * len(languages))
        for category in breakdown["categories"]:
            cells = [
                f"{category['means'][language]:.1f}" if language in category["means"] else "-"
                for language in languages
            ]
            lines.append(
                f"| {category['category']} | {category['weight']} | " + " | ".join(cells) + " |"
            )

    lines += ["", "## Per-requirement scores", ""]
    lines.append("Each row is one measured requirement. `w` is its frozen weight (or its")
    lines.append("category's, for a category mean). A rank in parentheses is the language's")
    lines.append("position on that requirement alone.")
    lines += ["", "| Requirement | w | " + " | ".join(languages) + " |"]
    lines.append("| --- | ---: |" + " ---: |" * len(languages))
    for row in breakdown["requirements"]:
        ranks = {entry["language"]: entry["rank"] for entry in row["ranking"]}
        cells = []
        for language in languages:
            if language in row["scores"]:
                cells.append(f"{row['scores'][language]:g} ({ranks.get(language, '-')})")
            elif language in row["not_applicable"]:
                cells.append("N/A")
            else:
                cells.append("-")
        weight = row.get("weight")
        label = row["requirement_id"]
        if row.get("category"):
            label = f"{label}<br>_{row['category']}_"
        lines.append(f"| {label} | {weight if weight is not None else ''} | " + " | ".join(cells) + " |")

    na_rows = [row for row in breakdown["requirements"] if row["not_applicable"]]
    if na_rows:
        lines += ["", "## N/A cells and their stated reasons", ""]
        for row in na_rows:
            for language, reason in sorted(row["not_applicable"].items()):
                lines.append(f"- **{row['requirement_id']} / {language}** — {reason}")

    lines += ["", "## Who measured what", "", "| Requirement | Work unit | Languages |", "| --- | --- | --- |"]
    for row in breakdown["requirements"]:
        for source in row["measured_by"]:
            langs = ", ".join(source["assigned_languages"]) or "all"
            lines.append(f"| {row['requirement_id']} | `{source['work_unit_id']}` | {langs} |")

    reconstruction = breakdown["reconstruction"]
    error = reconstruction.get("max_abs_error_vs_published")
    lines += [
        "",
        "## Reconstruction check",
        "",
        "These figures were recomputed with the same functions that produced the",
        "published score. Largest disagreement with the published score: "
        + (f"`{error:.2e}`." if error is not None else "not applicable (no published scores)."),
        "",
        "The workers' own reasoning for every cell is under `evidence/` beside this file.",
    ]
    return "\n".join(lines) + "\n"


def write_run_breakdown(staging: Path, root: Path) -> None:
    """Record how each Primary score was reached, next to the score itself.

    A run directory that carries only five aggregate rankings cannot be analysed
    and cannot be audited: the per-requirement cells, the weights that combined
    them and the workers' stated reasoning all lived in the workspace, which
    import discards, and in an artifact that expires.
    """
    aggregation_path = root / "template" / "config" / "aggregation.json"
    if not aggregation_path.is_file():
        # Nothing frozen to decompose a published score against. A workspace
        # without the template was never scored through it, so there is no
        # breakdown to write and no reason to fail the import over one.
        return
    aggregation = json_load(aggregation_path)
    languages = metadata_languages(root)
    skipped: list[dict[str, str]] = []
    for evaluation in PRIMARY_NAMES:
        published_path = root / "results" / "evaluations" / f"{evaluation}.json"
        published = json_load(published_path) if published_path.is_file() else {}
        config = (aggregation.get("evaluations") or {}).get(evaluation)
        if config is None:
            continue
        try:
            req = requirement_results_for_evaluation(root, evaluation)
            if config.get("recompute_from_evidence"):
                req = sc_recomputed_requirements(root, config, req, languages)
        except (BenchmarkError, OSError, json.JSONDecodeError):
            # A blocked evaluation has no complete requirement set, and that is
            # exactly when what *was* measured matters most, so fall back to the
            # cells the finished units recorded. These are unaggregated: no
            # score is published from them.
            req = {}
            _sources, partial = requirement_evidence_sources(root, evaluation)
            for record in partial:
                for rid, value in (record.get("requirements") or {}).items():
                    if isinstance(value, dict) and (
                        rid.startswith("metric.") or rid.startswith("condition.")
                    ):
                        req.setdefault(rid, {}).update(value)
        try:
            breakdown, records = evaluation_breakdown(
                root, evaluation, config, req, languages, published
            )
            json_dump(staging / "breakdown" / f"{evaluation}.json", breakdown)
            (staging / "breakdown" / f"{evaluation}.md").write_text(
                render_breakdown_markdown(breakdown), encoding="utf-8"
            )
            for record in records:
                json_dump(
                    staging / "evidence" / evaluation / f"{record['agent_id']}.json",
                    record,
                )
        except (BenchmarkError, OSError, KeyError, TypeError, ValueError) as exc:
            # The breakdown explains a score; it never withholds one. Record why
            # it could not be written rather than failing the import or leaving
            # the reader to wonder where the file went.
            skipped.append({"evaluation": evaluation, "reason": str(exc)})
    if skipped:
        json_dump(staging / "breakdown" / "skipped.json", {
            "schema_version": 1,
            "skipped": skipped,
        })


def write_semantic_compression_audit_manifest(
    staging: Path, root: Path
) -> str | None:
    """Retain the compact evidence needed to reconstruct the blinded SC audit.

    The full blinded sample and agent traces stay in the 30-day workflow
    artifact because committing them would duplicate large worker payloads.
    The compact run import already retains every scored metric worker's
    evidence.  This manifest adds what that score breakdown otherwise omits:
    the final cohort support records (with fragment hashes rather than duplicate
    source), the comparability verdict, the blinding map, and hashes of the
    transient audit files.  Together those inputs let a future reader rebuild
    and verify the exact sample without keeping the whole workspace in Git.
    """
    manifest_path = root / "work" / "root" / "manifest.json"
    if not manifest_path.is_file():
        return None

    final_support: dict[str, dict[str, Any]] = {}
    for probe_id, rows in sorted(sc_adjudicated_annotations(root).items()):
        compact_rows: dict[str, Any] = {}
        for language, record in sorted(rows.items()):
            fragment = record.get("fragment")
            compact = {
                key: value
                for key, value in record.items()
                if key != "fragment"
            }
            compact["fragment_sha256"] = (
                sha256_bytes(str(fragment).encode("utf-8"))
                if fragment is not None
                else None
            )
            compact_rows[language] = compact
        final_support[probe_id] = compact_rows

    manifest = json_load(manifest_path)
    comparability: dict[str, Any] | None = None
    for unit in manifest.get("work_units", []):
        if COMPARABILITY_GATE not in (unit.get("requirement_ids") or []):
            continue
        result_path = (
            root / "work" / "agents" / str(unit.get("assigned_agent_id"))
            / "result.json"
        )
        if result_path.is_file():
            result = json_load(result_path)
            evidence = result.get("evidence") or {}
            comparability = {
                "work_unit_id": str(unit.get("id") or ""),
                "gate": (result.get("requirements") or {}).get(COMPARABILITY_GATE),
                "gate_result": evidence.get("gate_result"),
            }
        break

    transient_paths = {
        "comparability_sample": root / COMPARABILITY_SAMPLE_RELATIVE,
        "support_reconciliation": (
            root / "work" / "audit" / "semantic-compression"
            / "support_reconciliation.json"
        ),
        "comparability_blinding": root / COMPARABILITY_BLINDING_RELATIVE,
        "comparability_repairs": root / COMPARABILITY_REPAIR_RELATIVE,
        "f20_runtime_facts": root / F20_RUNTIME_FACTS_RELATIVE,
    }
    transient_hashes = {
        name: {
            "workspace_path": path.relative_to(root).as_posix(),
            "sha256": sha256_file(path),
        }
        for name, path in transient_paths.items()
        if path.is_file()
    }
    blinding = (
        json_load(root / COMPARABILITY_BLINDING_RELATIVE)
        if (root / COMPARABILITY_BLINDING_RELATIVE).is_file()
        else None
    )
    repairs = sc_comparability_repairs(root)
    repaired_pairs = [
        {"probe_id": probe_id, "language": language}
        for probe_id, rows in sorted(repairs.items())
        for language in sorted(rows)
    ]

    payload = {
        "schema_version": 1,
        "purpose": (
            "Compact reconstruction manifest for Semantic Compression "
            "support reconciliation and blinded comparability audit."
        ),
        "reconstruction_note": (
            "Scored metric worker evidence is retained under ../evidence/"
            "semantic_compression/. Canonical fragments live there; "
            "fragment_sha256 below avoids duplicating them. Rebuild the sample "
            "with the frozen template and run_id, then verify the transient "
            "hashes recorded here."
        ),
        "final_support_adjudications": final_support,
        "comparability": comparability,
        "repaired_pairs": repaired_pairs,
        "blinding": blinding,
        "transient_audit_sha256": transient_hashes,
    }
    destination = staging / "audit" / "semantic_compression.json"
    json_dump(destination, payload)
    return destination.relative_to(staging).as_posix()


def compact_run_files(
    staging: Path,
    root: Path,
    run: dict[str, Any],
    cache_promotion: dict[str, Any],
    prompt_promotion: dict[str, Any],
) -> dict[str, str]:
    primary = (
        json_load(root / "results" / "primary_status.json")
        if (root / "results" / "primary_status.json").is_file()
        else {"evaluations": {}}
    )
    cache_status = (
        json_load(root / "results" / "cache_status.json")
        if (root / "results" / "cache_status.json").is_file()
        else {"schema_version": 1, "enabled": False, "hits": {}, "misses": {}}
    )
    toolchains = (
        json_load(root / "results" / "toolchains.json")
        if (root / "results" / "toolchains.json").is_file()
        else {"toolchains": {}}
    )
    evaluations = primary.get("evaluations") or {}
    summary = {
        "schema_version": 1,
        "run_id": run.get("run_id"),
        "evaluated": {
            "commit_sha": (run.get("evaluated") or {}).get("commit_sha"),
            "compiler_version": (run.get("evaluated") or {}).get("compiler_version"),
        },
        "primary_evaluations": {
            name: {
                "status": value.get("status"),
                "score": value.get("score"),
                "ranking": value.get("ranking"),
                "ranking_basis": value.get("ranking_basis"),
                "blockers": value.get("blockers", []),
            }
            for name, value in evaluations.items()
        },
        "cache": {
            "hit_count": int(cache_status.get("hit_count", 0) or 0),
            "miss_count": int(cache_status.get("miss_count", 0) or 0),
            "promoted_records": int(cache_promotion.get("promoted", 0) or 0),
        },
        "raw_evidence": {
            "worker_evidence_committed_to_git": True,
            "worker_evidence_path": "evidence/",
            "score_breakdown_path": "breakdown/",
            "agent_traces_committed_to_git": False,
            "workflow_artifact_retention_days": 30,
        },
    }
    rankings = {
        "schema_version": 1,
        "run_id": run.get("run_id"),
        "rankings": {
            name: value.get("ranking")
            for name, value in evaluations.items()
        },
        "ranking_basis": {
            name: value.get("ranking_basis")
            for name, value in evaluations.items()
        },
    }
    provenance = {
        "schema_version": 1,
        "run_id": run.get("run_id"),
        "master_prompt_sha256": run.get("master_prompt_sha256"),
        "primary_config_sha256": run.get("primary_config_sha256"),
        "template_tree_sha256": run.get("template_tree_sha256"),
        "cache_tree_sha256_at_start": run.get("cache_tree_sha256"),
        "inference_identity": run.get("inference_identity"),
        "quidra_execution_identity": (
            (run.get("evaluated") or {}).get("quidra_execution_identity")
        ),
        "toolchains": {
            language: row.get("canonical")
            for language, row in (toolchains.get("toolchains") or {}).items()
            if row.get("canonical")
        },
        "prompt_promotion": prompt_promotion,
        "cache_promotion": cache_promotion,
    }
    files = {
        "summary.json": summary,
        "rankings.json": rankings,
        "cache_usage.json": cache_status,
        "provenance.json": provenance,
    }
    for name, payload in files.items():
        json_dump(staging / name, payload)
    write_run_breakdown(staging, root)
    semantic_audit_path = write_semantic_compression_audit_manifest(staging, root)
    if semantic_audit_path is not None:
        summary["raw_evidence"]["semantic_compression_audit_manifest"] = semantic_audit_path
        json_dump(staging / "summary.json", summary)

    hashes: dict[str, str] = {}
    for file in sorted(p for p in staging.rglob("*") if p.is_file()):
        hashes[file.relative_to(staging).as_posix()] = sha256_file(file)
    return hashes


def cmd_report(args: argparse.Namespace) -> int:
    """Write the score breakdown for a workspace that has already been scored."""
    root = Path(args.workspace).resolve()
    if not (root / "work" / "root" / "manifest.json").is_file():
        raise BenchmarkError(f"not a benchmark workspace: {root}")
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    write_run_breakdown(out, root)
    written = sorted(p.relative_to(out).as_posix() for p in out.rglob("*") if p.is_file())
    print(json.dumps({"schema_version": 1, "out": str(out), "files": len(written)}, indent=2))
    return 0


def cmd_post_run(args: argparse.Namespace) -> int:
    source = Path(args.source_repo).resolve()
    if not source.is_dir():
        raise BenchmarkError(f"source repository does not exist: {source}")
    root = host_workspace(source)
    if not root.is_dir():
        raise BenchmarkError(f"benchmark workspace does not exist: {root}")
    run_path = root / "run.json"
    if not run_path.is_file():
        raise BenchmarkError("benchmark workspace is missing run.json")
    run = json_load(run_path)
    validate_host_workspace_sentinel(root, source, run)

    finalization_path = root / "results" / "finalization.json"
    if not finalization_path.is_file():
        raise BenchmarkError("post-run requires a successful finalize first")
    finalization = json_load(finalization_path)
    if not finalization.get("ok"):
        raise BenchmarkError("post-run requires a successful finalize first")
    if finalization.get("formal_complete") is not True:
        raise BenchmarkError(
            "post-run refuses diagnostic/partial finalization; all required units "
            "and all five Primary rankings must be COMPLETE"
        )

    privacy_rc = cmd_privacy_check(argparse.Namespace(workspace=str(root)))
    if privacy_rc != 0:
        raise BenchmarkError("post-run retention privacy check failed")

    run_id = str(run.get("run_id") or "").strip()
    if not re.fullmatch(r"[A-Za-z0-9._()\-]+", run_id):
        raise BenchmarkError(f"invalid run_id for repository import: {run_id!r}")

    meta = git_metadata(source)
    if meta["working_tree_status"] != "clean":
        raise BenchmarkError("post-run source repository must have a clean working tree")

    root_real = root.resolve()
    source_real = source.resolve()
    try:
        source_real.relative_to(root_real)
    except ValueError:
        pass
    else:
        raise BenchmarkError("source repository may not be inside the benchmark workspace")
    try:
        workspace_relative = root_real.relative_to(source_real)
    except ValueError:
        workspace_relative = None
    if workspace_relative is not None and workspace_relative != HOST_WORKSPACE_RELATIVE:
        raise BenchmarkError(
            "benchmark workspace may be inside the source repository only as ./.quidra-benchmark"
        )

    benchmark_dir = source / "benchmark"
    if not benchmark_dir.is_dir():
        raise BenchmarkError("source repository is missing benchmark/")
    destination = benchmark_dir / run_id
    if destination.exists():
        raise BenchmarkError(f"run destination already exists: {destination}")

    # Promote only after finalization so current-run workers can never observe
    # material produced by the run they are scoring.
    prompt_promotion = promote_prompt_store(source, root)
    cache_promotion = promote_certified_cache(source, root)

    staging = benchmark_dir / f".{run_id}.importing"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    expected_hashes = compact_run_files(
        staging, root, run, cache_promotion, prompt_promotion
    )
    import_manifest = {
        "schema_version": 2,
        "run_id": run_id,
        "evaluated_commit_sha": run.get("evaluated", {}).get("commit_sha"),
        # post-run materializes the compact result in the checkout it was
        # invoked from. Production runs do that on the isolated benchmark
        # branch; reconciliation into the then-current develop happens later
        # under the documented branch-lifecycle procedure. Do not claim a
        # develop commit here before that reconciliation has actually happened.
        "post_run_source_branch": meta["branch"],
        "post_run_source_commit": meta["commit_sha"],
        "retained_file_count": len(expected_hashes),
        "retained_files_sha256": expected_hashes,
        "retention_policy": "compact-summary-only",
    }
    json_dump(staging / "import_manifest.json", import_manifest)
    os.replace(staging, destination)

    observed = {
        p.relative_to(destination).as_posix(): sha256_file(p)
        for p in sorted(destination.rglob("*"))
        if p.is_file() and p.name != "import_manifest.json"
    }
    if observed != expected_hashes:
        raise BenchmarkError(
            f"compact repository import verification failed: {destination}"
        )

    try:
        validate_host_workspace_sentinel(root, source, run)
        delete_host_workspace(root, source)
    except (OSError, BenchmarkError) as exc:
        raise BenchmarkError(
            f"run imported successfully to {destination}, but guarded workspace cleanup failed: {exc}"
        ) from exc

    result = {
        "ok": True,
        "run_id": run_id,
        "destination": str(destination),
        "retained_file_count": len(expected_hashes),
        "cache_promoted": cache_promotion.get("promoted", 0),
        "prompt_components_promoted": prompt_promotion.get("components", 0),
        "workspace_deleted": True,
    }
    print(json.dumps(result, indent=2))
    return 0


def cmd_discard_workspace(args: argparse.Namespace) -> int:
    source = Path(args.source_repo).resolve()
    if not source.is_dir():
        raise BenchmarkError(f"source repository does not exist: {source}")
    root = host_workspace(source)
    marker = validate_host_workspace_guard(source)
    workspace_deleted = delete_host_workspace(root, source)
    result = {
        "ok": True,
        "run_id": marker["run_id"],
        "workspace_deleted": workspace_deleted,
        "sentinel_deleted": True,
    }
    print(json.dumps(result, indent=2))
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Quidra benchmark orchestration CLI")
    sub = p.add_subparsers(dest="command", required=True)

    init = sub.add_parser("init", help="stage <source-repo>/.quidra-benchmark for mapping to /quidra-benchmark inside the sandbox")
    init.add_argument("--source-repo", required=True)
    init.add_argument("--run-id")
    init.add_argument("--provider")
    init.add_argument("--model")
    init.add_argument(
        "--sandbox-mode",
        required=True,
        choices=("container", "chroot", "namespace", "external-sandbox"),
    )
    init.set_defaults(func=cmd_init)

    tcs = sub.add_parser("toolchain-scan", help="record current comparison-language toolchain fingerprints")
    tcs.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    tcs.add_argument("--strict", action="store_true")
    tcs.set_defaults(func=cmd_toolchain_scan)

    rs = sub.add_parser("reuse-status", help="identify reusable assets needing a toolchain currency audit")
    rs.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    rs.add_argument("--strict", action="store_true")
    rs.set_defaults(func=cmd_reuse_status)

    tcb = sub.add_parser(
        "toolchain-blockers",
        help="record missing toolchains as infrastructure blockers without aborting unrelated evaluations",
    )
    tcb.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    tcb.set_defaults(func=cmd_toolchain_blockers)

    pre = sub.add_parser(
        "preflight",
        help="verify the observed sandbox, launcher contract, gateway handshake and "
             "absence of provider credentials",
    )
    pre.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    pre.add_argument("--gateway-timeout", type=float, default=15.0)
    pre.set_defaults(func=cmd_preflight)

    plan = sub.add_parser("plan", help="report frozen primary work and missing manifest coverage")
    plan.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    plan.add_argument("--strict", action="store_true")
    plan.set_defaults(func=cmd_plan)

    dp = sub.add_parser("deterministic-plan", help="generate all five Primary plans without a planning LLM")
    dp.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    dp.set_defaults(func=cmd_deterministic_plan)

    rc = sub.add_parser("result-check", help="validate a standardized leaf-worker result.json")
    rc.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    rc.add_argument("--id", required=True)
    rc.set_defaults(func=cmd_result_check)

    crc = sub.add_parser("command-result-check", help="validate runner-owned requirement result.json")
    crc.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    crc.add_argument("--id", required=True)
    crc.set_defaults(func=cmd_command_result_check)

    ts = sub.add_parser("task-start", help="claim an agent work unit and start its lease")
    ts.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    ts.add_argument("--id", required=True)
    ts.set_defaults(func=cmd_task_start)

    hb = sub.add_parser("heartbeat", help="renew a RUNNING work-unit lease")
    hb.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    hb.add_argument("--id", required=True)
    hb.set_defaults(func=cmd_heartbeat)

    tf = sub.add_parser("task-finish", help="validate a worker result and complete or retry the unit")
    tf.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    tf.add_argument("--id", required=True)
    tf.set_defaults(func=cmd_task_finish)

    reclaim = sub.add_parser("reclaim-stale", help="reclaim stale RUNNING units or block exhausted work")
    reclaim.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    reclaim.set_defaults(func=cmd_reclaim_stale)

    agg = sub.add_parser("aggregate-primary", help="mechanically aggregate one Primary evaluation and ranking")
    agg.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    agg.add_argument("--evaluation", required=True, choices=PRIMARY_NAMES)
    agg.set_defaults(func=cmd_aggregate_primary)

    ac = sub.add_parser("aggregate-check", help="validate a runner-generated Primary aggregate")
    ac.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    ac.add_argument("--evaluation", required=True, choices=PRIMARY_NAMES)
    ac.add_argument("--file", required=True)
    ac.set_defaults(func=cmd_aggregate_check)

    psd = sub.add_parser("primary-status-derive", help="derive Primary statuses from ledger/aggregates")
    psd.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    psd.set_defaults(func=cmd_primary_status_derive)

    adv = sub.add_parser("advance", help="advance the runner state machine and emit the agent dispatch queue")
    adv.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    adv.add_argument("--evaluation", choices=PRIMARY_NAMES)
    adv.set_defaults(func=cmd_advance)

    prep = sub.add_parser("prepare", help="run deterministic pre-dispatch steps and emit the requested Primary queue")
    prep.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    prep.add_argument("--evaluation", choices=PRIMARY_NAMES)
    prep.set_defaults(func=cmd_prepare)

    pcx = sub.add_parser("plan-check", help="validate one planning agent work_plan.json")
    pcx.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    pcx.add_argument("--evaluation", required=True, choices=PRIMARY_NAMES)
    pcx.add_argument("--file", required=True)
    pcx.set_defaults(func=cmd_plan_check)

    mm = sub.add_parser("manifest-merge", help="freeze deterministic Primary plans into manifest and ledger")
    mm.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    mm.set_defaults(func=cmd_manifest_merge)

    lu = sub.add_parser("ledger-update", help="atomically update one work-unit state")
    lu.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    lu.add_argument("--id", required=True)
    lu.add_argument("--status", required=True, choices=("PENDING", "RUNNING", "COMPLETE", "BLOCKED", "INVALID"))
    lu.add_argument("--evidence", action="append", default=[])
    lu.add_argument("--validation-result", choices=("PASS", "FAIL"))
    lu.add_argument("--blocker")
    lu.add_argument(
        "--blocker-class",
        choices=("scientific", "infrastructure", "budget-plan-defect", "ordinary-incomplete"),
    )
    lu.set_defaults(func=cmd_ledger_update)

    tcm = sub.add_parser("tasks-create", help="create child Task Packets from the frozen manifest")
    tcm.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    tcm.add_argument("--evaluation", choices=PRIMARY_NAMES)
    tcm.add_argument("--parent")
    tcm.add_argument("--depth", type=int, default=1)
    tcm.set_defaults(func=cmd_tasks_create)

    tc = sub.add_parser("task-create", help="create and hash a self-contained delegated Task Packet")
    tc.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    tc.add_argument("--id", required=True)
    tc.add_argument("--parent")
    tc.add_argument("--evaluation", choices=PRIMARY_NAMES)
    tc.add_argument("--goal", required=True)
    tc.add_argument("--read", action="append", default=[])
    tc.add_argument("--write")
    tc.add_argument("--output", action="append", default=[])
    tc.add_argument("--validate", required=True)
    tc.add_argument("--network", action="store_true")
    tc.add_argument("--depth", type=int, default=2)
    tc.add_argument("--section", action="append", default=[])
    tc.add_argument("--requirement-id", action="append", default=[])
    tc.add_argument("--language", action="append", default=[])
    tc.add_argument("--worker-mode", choices=("packet-only", "sandbox-agent"), default="packet-only")
    tc.add_argument(
        "--layout", choices=PACKET_LAYOUTS, default="task-first",
        help="component order of the rendered packet; shared-inputs-first puts the "
             "inputs every sibling unit shares before the per-unit header so the "
             "trusted gateway can cache them once",
    )
    tc.set_defaults(func=cmd_task_create)

    ps = sub.add_parser("prompt-save", help="content-address and preserve an exact scored/delegated prompt")
    ps.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    ps.add_argument("--file", required=True)
    ps.set_defaults(func=cmd_prompt_save)

    tr = sub.add_parser("task-render", help="render one self-contained Task Packet to stdout")
    tr.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    tr.add_argument("--id", required=True)
    tr.set_defaults(func=cmd_task_render)

    ta = sub.add_parser("task-apply", help="import a packet-only worker JSON response from stdin")
    ta.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    ta.add_argument("--id", required=True)
    ta.set_defaults(func=cmd_task_apply)

    ti = sub.add_parser(
        "task-infer",
        help="render a packet-only task, infer through the credential-less gateway, and apply",
    )
    ti.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    ti.add_argument("--id", required=True)
    ti.add_argument("--socket")
    # One packet-only request may now run to the 32768-token ceiling, be
    # re-sent once when it comes back empty, and carry web-search
    # continuations; the slowest first-run call produced 15k tokens in 186s.
    ti.add_argument("--timeout", type=float, default=1800.0)
    ti.add_argument("--max-output-tokens", type=int, default=8192)
    ti.set_defaults(func=cmd_task_infer)

    tv = sub.add_parser("task-validate", help="run the exact validator frozen in a Task Packet")
    tv.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    tv.add_argument("--id", required=True)
    tv.set_defaults(func=cmd_task_validate)

    lr = sub.add_parser("ledger-reconcile", help="reconcile manifest state with required evidence paths")
    lr.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    lr.set_defaults(func=cmd_ledger_reconcile)

    ss = sub.add_parser("score-status", help="show scoreability/blockers for all five primary evaluations")
    ss.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    ss.add_argument("--strict", action="store_true")
    ss.set_defaults(func=cmd_score_status)

    pc = sub.add_parser("privacy-check", help="scan retained artifacts for personal/sensitive data")
    pc.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    pc.set_defaults(func=cmd_privacy_check)

    complete_audit = sub.add_parser(
        "completeness-audit",
        help="identify the exact required leaves/commands still preventing formal COMPLETE",
    )
    complete_audit.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    complete_audit.set_defaults(func=cmd_completeness_audit)

    fin = sub.add_parser("finalize", help="audit completeness and enforce score/blocker and privacy gates")
    fin.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    fin.set_defaults(func=cmd_finalize)

    restore_guard = sub.add_parser(
        "restore-workspace-guard",
        help="re-create the trusted host sentinel after a CI workspace handoff",
    )
    restore_guard.add_argument("--source-repo", required=True)
    restore_guard.add_argument("--expected-commit")
    restore_guard.set_defaults(func=cmd_restore_workspace_guard)

    refresh_cache = sub.add_parser(
        "refresh-cache-snapshot",
        help="bind a newly initialized recovery workspace to a newer certified cache snapshot",
    )
    refresh_cache.add_argument("--source-repo", required=True)
    refresh_cache.add_argument("--expected-commit")
    refresh_cache.set_defaults(func=cmd_refresh_cache_snapshot)

    paid_import = sub.add_parser(
        "partial-paid-import",
        help="restore exact-fingerprint paid leaf inference state from a durable/mirrored checkpoint store",
    )
    paid_import.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    paid_import.add_argument("--store", required=True)
    paid_import.add_argument("--evaluation", choices=PRIMARY_NAMES)
    paid_import.set_defaults(func=cmd_partial_paid_import)

    paid_export = sub.add_parser(
        "partial-paid-export",
        help="checkpoint paid leaf inference state into a content-addressed recovery store",
    )
    paid_export.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    paid_export.add_argument("--store", required=True)
    paid_export.add_argument("--evaluation", choices=PRIMARY_NAMES)
    paid_export.set_defaults(func=cmd_partial_paid_export)

    checkpoint = sub.add_parser(
        "checkpoint-cache",
        help="promote COMPLETE+PASS eligible units from an unfinished host staging run",
    )
    checkpoint.add_argument("--source-repo", required=True)
    checkpoint.set_defaults(func=cmd_cache_checkpoint)

    annotate = sub.add_parser(
        "cache-annotate-caps",
        help="write scored-cap evidence from a run's retained agent traces into the "
             "certified trial records that run promoted",
    )
    annotate.add_argument("--source-repo", required=True)
    annotate.add_argument(
        "--evidence", required=True,
        help="extracted workspace evidence: run.json, work/root/manifest.json and "
             "work/agents/*/agent_trace.json",
    )
    annotate.add_argument("--force", action="store_true", help="rewrite existing evidence")
    annotate.set_defaults(func=cmd_cache_annotate_caps)

    promote = sub.add_parser(
        "cache-promote-evidence",
        help="promote the certifiable units of a run that never finalized, from its "
             "retained workspace evidence and the evaluated snapshot",
    )
    promote.add_argument("--source-repo", required=True)
    promote.add_argument("--evidence", required=True, help="extracted workspace-evidence.tgz")
    promote.add_argument("--snapshot", required=True, help="the evaluated commit (git revision)")
    promote.set_defaults(func=cmd_cache_promote_evidence)

    impact = sub.add_parser(
        "cache-impact",
        help="list the certified records the checkout's current template, docs, pins, "
             "epochs and Quidra versions would no longer match (exit 3 when any)",
    )
    impact.add_argument("--source-repo", required=True)
    impact.add_argument(
        "--output",
        help="optional JSON path for persisting the impact report before paid work",
    )
    impact.set_defaults(func=cmd_cache_impact)

    report = sub.add_parser(
        "report",
        help="write the per-requirement score breakdown and worker evidence for a scored workspace",
    )
    report.add_argument("--workspace", required=True)
    report.add_argument("--out", required=True)
    report.set_defaults(func=cmd_report)

    post = sub.add_parser(
        "post-run",
        help="import finalized artifacts from <source-repo>/.quidra-benchmark and delete the host staging workspace",
    )
    post.add_argument("--source-repo", required=True)
    post.set_defaults(func=cmd_post_run)

    discard = sub.add_parser(
        "discard-workspace",
        help="safely discard an abandoned <source-repo>/.quidra-benchmark staging workspace",
    )
    discard.add_argument("--source-repo", required=True)
    discard.set_defaults(func=cmd_discard_workspace)

    return p


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        return int(args.func(args))
    except BenchmarkError as exc:
        eprint(f"benchmark error: {exc}")
        return 2
    except subprocess.CalledProcessError as exc:
        eprint(f"command failed: {exc}")
        if exc.stderr:
            eprint(exc.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
