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
    "work/root/manifest.json",
    "work/root/ledger.json",
    "work/root/plans",
    "work/root/commands",
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

    run = {
        "schema_version": 1,
        "run_id": run_id,
        "evaluated": {
            **meta,
            "compiler_version": manifest_version(root / "repo"),
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
    if lexical_absolute(root) == lexical_absolute(CANONICAL_WORKSPACE):
        semantic_ffi_smoke = run_f20_runtime_baselines(root)
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


def validate_work_plan_data(root: Path, evaluation: str, plan: dict[str, Any]) -> dict[str, Any]:
    if evaluation not in PRIMARY_NAMES:
        raise BenchmarkError(f"unknown Primary evaluation: {evaluation}")
    if plan.get("evaluation") != evaluation:
        raise BenchmarkError(f"work plan evaluation mismatch: {plan.get('evaluation')!r}")
    requirements_path, required_requirement_ids = load_evaluation_requirements(root, evaluation)
    allowed_requirement_ids = set(required_requirement_ids)
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
        unknown_requirements = sorted(
            rid for rid in set(requirement_ids) - allowed_requirement_ids
            if not str(rid).startswith(SUPPORT_ADJUDICATION_PREFIX)
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
    runner section: primary.json is embedded in every Task Packet and hashed
    into every certified-cache key, so the limit is raised where it re-keys
    nothing. A unit that declares its own max_attempts keeps it.
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
) -> list[str]:
    """Resolve task reads, narrowing reusable programs to assigned comparison languages."""
    comparison_only = bool(assigned_languages) and "Quidra" not in assigned_languages
    expanded: list[str] = []
    catalog: dict[str, Any] | None = None

    for value in raw_paths:
        if comparison_only and value == "repo/docs":
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
            units.append({
                "id": uid,
                "evaluation": evaluation,
                "phase": "readiness",
                "execution_kind": "agent",
                "worker_mode": "packet-only",
                "result_kind": "audit",
                "goal": (
                    f"Audit reusable artifact {artifact_id} against the current toolchain/capability "
                    "contract. Confirm whether the source/harness remains semantically valid; do not "
                    "reuse old measurements."
                ),
                "assigned_agent_id": agent_id,
                "dependencies": [],
                "input_hashes": {"artifact_git_object": artifact.get("content_git_object_sha1")},
                "reuse_audit_for": [artifact_id],
                "requirement_ids": [],
                "read_paths": [str(root / "template" / str(artifact["content_destination"]))],
                "evidence_paths": [str(root / "work" / "agents" / agent_id / "result.json")],
                "validator_command": (
                    f"python3 {root / 'template' / 'scripts' / 'benchmark.py'} "
                    f"result-check --workspace {root} --id {agent_id}"
                ),
                "network_allowed": True,
                "prompt_sections": [],
                "max_attempts": default_max_attempts,
                "max_llm_calls": 0,
                "estimated_input_tokens_per_call": 0,
                "max_output_tokens_per_call": 0,
            })

        regular_ids: list[str] = []
        split_modes = {
            str(raw["id"]): (
                "language"
                if bool(raw.get("split_by_language", False))
                else str(raw.get("split_mode") or "")
            )
            for raw in spec.get("units", [])
            if bool(raw.get("split_by_language", False)) or raw.get("split_mode")
        }
        fixed_languages = metadata_languages(root)
        for raw in spec.get("units", []):
            base_uid = str(raw["id"])
            execution_kind = str(raw.get("execution_kind", "agent"))
            result_kind = str(raw.get("result_kind", "requirements"))
            split_mode = (
                "language"
                if bool(raw.get("split_by_language", False))
                else str(raw.get("split_mode") or "")
            )
            if execution_kind != "agent":
                split_mode = ""
            if split_mode == "language":
                shards: list[list[str]] = [[language] for language in fixed_languages]
            elif split_mode == "target_vs_comparison":
                target = str(
                    load_benchmark_metadata(root / "template").get(
                        "evaluated_target_language", "Quidra"
                    )
                )
                comparison = [language for language in fixed_languages if language != target]
                shards = [[target], comparison]
            elif split_mode:
                raise BenchmarkError(
                    f"{base_uid}: unsupported split_mode {split_mode!r}"
                )
            else:
                shards = [[]]
            for assigned_languages in shards:
                suffix = ""
                if assigned_languages:
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
                    + ", ".join(raw.get("requirement_ids", []))
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
                    "runner_action": raw.get("runner_action"),
                    "goal": goal,
                    "assigned_agent_id": agent_id,
                    "assigned_languages": assigned_languages,
                    "dependencies": deps,
                    "input_hashes": {
                        "primary_config": sha256_file(
                            root / "template" / "config" / "primary.json"
                        ),
                        "benchmark_metadata": sha256_file(
                            root / "template" / BENCHMARK_METADATA_RELATIVE
                        ),
                        "evaluation_spec": sha256_file(
                            root / "template" / "methodology"
                            / EVALUATION_SPEC_FILES[evaluation]
                        ),
                    },
                    "reuse_audit_for": [],
                    "requirement_ids": list(raw.get("requirement_ids", [])),
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
    """Compact trusted build/run evidence for one canonical Semantic probe.

    Canonical-fragment validation executes in the pinned runtime after the
    authoring worker returns. Support adjudicators must see that trusted fact;
    otherwise they can re-invent stale toolchain assumptions after the exact
    frozen recipe has already succeeded.
    """
    path = (
        root / "work" / "audit" / "semantic-compression"
        / f"canonical_verification_{slug_id(language)}.json"
    )
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

    def process(value: Any) -> dict[str, Any] | None:
        if not isinstance(value, dict):
            return None
        return {
            "argv": list(value.get("argv") or []),
            "exit_code": value.get("exit_code"),
            "stdout": clip_annotation_text(str(value.get("stdout") or ""), 500),
            "stderr": clip_annotation_text(str(value.get("stderr") or ""), 500),
        }

    compact: dict[str, Any] = {
        "verified": True,
        "source": "trusted canonical-fragment validator in the pinned runtime",
        "mode": raw.get("mode"),
        "run_count": raw.get("run_count"),
        "build": process(raw.get("build")),
        "symbol_add2_defined": raw.get("symbol_add2_defined"),
    }
    runs = [
        item for item in (process(value) for value in (raw.get("runs") or []))
        if item is not None
    ]
    if runs:
        compact["runs"] = runs
    nm = process(raw.get("nm"))
    if nm is not None:
        compact["nm"] = nm
    return compact


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
    probe = next(
        (entry for entry in comparability_sample_probes(root)
         if str(entry.get("probe_id")) == probe_id),
        None,
    )
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
            "runner evidence from the pinned runtime using the exact frozen recipe; "
            "when present, do not contradict its build/run facts or invent a P-b "
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
        "comparability_policy": policy,
        "mechanical_verification": mechanical_verification,
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
        for probe_id, fields in collected.items():
            if fields:
                sc_add_annotation_source(
                    annotations.setdefault(probe_id, {}),
                    str(source.get("id") or dependency),
                    fields,
                )
        if support_owner_id in (source.get("requirement_ids") or []):
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



def canonical_fragment_catalog(
    root: Path, result: dict[str, Any]
) -> dict[str, dict[str, Any]]:
    """Validate and normalize the one-fragment-per-probe catalog."""
    evidence = result.get("evidence") or {}
    raw = evidence.get("canonical_fragments")
    if not isinstance(raw, dict):
        raise BenchmarkError(
            "canonical fragment owner must write evidence.canonical_fragments"
        )
    matrix = json_load(
        root / "template" / "methodology-assets" / "semantic_compression"
        / "semantic_site_matrix.json"
    )
    expected = {str(probe.get("probe_id")) for probe in matrix.get("probes", [])}
    if set(raw) != expected:
        raise BenchmarkError(
            "canonical fragment catalog must contain exactly the frozen probe set; "
            f"missing={sorted(expected-set(raw))}, extra={sorted(set(raw)-expected)}"
        )
    normalized: dict[str, dict[str, Any]] = {}
    for probe_id in sorted(expected):
        record = sc_adjudicated_record(raw[probe_id])
        if record is None:
            raise BenchmarkError(
                f"canonical fragment {probe_id} must be a complete support record"
            )
        normalized[probe_id] = record
    return normalized


def canonical_fragment_input_for_unit(
    root: Path, unit: dict[str, Any], manifest: dict[str, Any]
) -> tuple[Path, str]:
    """Materialize the completed owner catalog for one language as task input."""
    source_requirement = str(
        unit.get("canonical_fragment_source_requirement") or ""
    )
    assigned = list(unit.get("assigned_languages") or [])
    if not source_requirement or len(assigned) != 1:
        raise BenchmarkError(
            f"{unit.get('id')}: canonical fragment consumer must own one language"
        )
    language = str(assigned[0])
    units = {str(item.get("id")): item for item in manifest.get("work_units", [])}
    candidates: list[dict[str, Any]] = []
    for dep in unit.get("dependencies", []):
        source = units.get(str(dep))
        if source is None:
            continue
        if source_requirement not in (source.get("requirement_ids") or []):
            continue
        if list(source.get("assigned_languages") or []) != [language]:
            continue
        candidates.append(source)
    if len(candidates) != 1:
        raise BenchmarkError(
            f"{unit.get('id')}: expected one completed canonical fragment owner "
            f"for {language}, found {len(candidates)}"
        )
    source = candidates[0]
    result_path = (
        root / "work" / "agents" / str(source.get("assigned_agent_id"))
        / "result.json"
    )
    if not result_path.is_file():
        raise BenchmarkError(
            f"{unit.get('id')}: canonical fragment owner result is missing"
        )
    catalog = canonical_fragment_catalog(root, json_load(result_path))
    payload = {
        "schema_version": 1,
        "language": language,
        "source_work_unit_id": source.get("id"),
        "source_requirement_id": source_requirement,
        "rule": (
            "Use exactly these fragments for every downstream Semantic "
            "Compression metric. FULL/PARTIAL entries must be measured verbatim; "
            "NONE entries have no fragment and must not receive a numeric "
            "per-probe A/B/C/D measurement."
        ),
        "canonical_fragments": catalog,
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
            f"canonical fragment input changed after owner completion: {language}"
        )
    destination.write_bytes(encoded)
    return destination, sha256_bytes(encoded)


def canonical_owner_record_for_probe(
    root: Path, language: str, probe_id: str
) -> dict[str, Any]:
    """Read the fragment/support record frozen by Capability Coverage."""
    manifest = json_load(root / "work" / "root" / "manifest.json")
    matches = [
        unit for unit in manifest.get("work_units", [])
        if "metric.capability_coverage" in (unit.get("requirement_ids") or [])
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
    catalog = canonical_fragment_catalog(root, json_load(result_path))
    if probe_id not in catalog:
        raise BenchmarkError(
            f"{probe_id}: canonical fragment owner omitted {language}"
        )
    return catalog[probe_id]


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


F20_RUNTIME_FIXTURES_RELATIVE = Path("template/runtime/f20_interop_fixtures.json")


def _render_f20_runtime_recipe(
    recipe: str, source: Path, work: Path
) -> list[str]:
    replacements = {
        "FILE.py": str(source),
        "FILE.go": str(source),
        "FILE.java": str(source),
        "FILE.kt": str(source),
        "FILE.jar": str(work / "program.jar"),
        "./BIN": str(work / "program"),
        "BIN": str(work / "program"),
        "OUT": str(work / "out"),
    }
    argv: list[str] = []
    for token in shlex.split(recipe):
        rendered = token
        for key in sorted(replacements, key=len, reverse=True):
            rendered = rendered.replace(key, replacements[key])
        argv.append(rendered)
    return argv


def run_f20_runtime_baselines(root: Path) -> dict[str, Any]:
    """Verify selected F20.P1 mechanisms under the exact frozen recipes."""
    fixtures = json_load(root / F20_RUNTIME_FIXTURES_RELATIVE)
    if fixtures.get("schema_version") != 1 or fixtures.get("probe_id") != "F20.P1":
        raise BenchmarkError("invalid F20 runtime baseline fixture contract")
    environment = json_load(root / "template" / "environment" / "environment.json")
    frozen = environment.get("frozen_toolchain_recipes") or {}
    work_root = root / "work" / "root" / "commands" / "f20-runtime-baselines"
    shutil.rmtree(work_root, ignore_errors=True)
    work_root.mkdir(parents=True, exist_ok=True)
    report: dict[str, Any] = {
        "schema_version": 1,
        "probe_id": "F20.P1",
        "passed": False,
        "languages": {},
    }
    for language, fixture in sorted((fixtures.get("languages") or {}).items()):
        recipe = frozen.get(language) or {}
        if fixture.get("build") != recipe.get("build") or fixture.get("run") != recipe.get("run"):
            raise BenchmarkError(
                f"F20 baseline recipe drift for {language}: fixture must equal frozen recipe"
            )
        work = work_root / slug_id(language)
        work.mkdir(parents=True, exist_ok=True)
        (work / "out").mkdir(exist_ok=True)
        source = work / str(fixture["filename"])
        source.write_text(str(fixture["source"]), encoding="utf-8")
        evidence: dict[str, Any] = {
            "recipe_build": fixture.get("build"),
            "recipe_run": fixture.get("run"),
        }
        if fixture.get("build"):
            build_argv = _render_f20_runtime_recipe(
                str(fixture["build"]), source, work
            )
            evidence["build"] = _semantic_run_process(
                root, work, build_argv, label=f"{language} F20.P1 baseline build"
            )
        run_argv = _render_f20_runtime_recipe(str(fixture["run"]), source, work)
        evidence["run"] = _semantic_run_process(
            root, work, run_argv, label=f"{language} F20.P1 baseline run"
        )
        stdout = str(evidence["run"].get("stdout") or "").strip()
        if stdout != "3":
            raise BenchmarkError(
                f"{language} F20.P1 baseline produced {stdout!r}, expected '3'"
            )
        evidence["verified_without_extra_flags"] = True
        evidence["observed_stdout"] = "3"
        report["languages"][language] = evidence
    report["passed"] = True
    return report


def f20_runtime_baseline_data(root: Path) -> dict[str, Any] | None:
    path = root / "results" / "toolchains.json"
    if not path.is_file():
        return None
    data = (json_load(path).get("semantic_ffi_smoke") or {})
    if not data:
        return None
    if data.get("probe_id") != "F20.P1" or data.get("passed") is not True:
        raise BenchmarkError("F20 runtime baseline evidence is incomplete or failed")
    return data


def validate_f20_record_against_runtime_baseline(
    root: Path, language: str, record: dict[str, Any]
) -> None:
    fixtures = json_load(root / F20_RUNTIME_FIXTURES_RELATIVE)
    verified_languages = set((fixtures.get("languages") or {}).keys())
    if language not in verified_languages:
        return
    data = f20_runtime_baseline_data(root)
    if data is None:
        if lexical_absolute(root) == lexical_absolute(CANONICAL_WORKSPACE):
            raise BenchmarkError(
                f"F20.P1: trusted runtime baseline evidence is missing for {language}"
            )
        return
    if language not in (data.get("languages") or {}):
        raise BenchmarkError(
            f"F20.P1: trusted runtime baseline omitted configured language {language}"
        )
    if record["level"] == "NONE":
        raise BenchmarkError(
            f"F20.P1: {language} cannot be NONE: the pinned runtime baseline "
            "delivers the numbered task under the exact frozen recipe"
        )
    if "P-b" in record["partial_reasons"]:
        raise BenchmarkError(
            f"F20.P1: {language} cannot cite P-b: the pinned runtime baseline "
            "uses no compiler/runtime flag beyond the frozen recipe"
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
    synthetic = bool(evidence.get("synthetic"))
    audit_path = (
        root / "work" / "audit" / "semantic-compression"
        / f"canonical_verification_{slug_id(language)}.json"
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
            runs = []
            for index in range(row["run_count"]):
                runs.append(_semantic_run_process(
                    root, probe_dir, run_argv,
                    label=(
                        f"{language} {probe_id} frozen run recipe"
                        + (f" #{index + 1}" if row["run_count"] > 1 else "")
                    ),
                ))
            probe_report["runs"] = runs
        report["probes"][probe_id] = probe_report

    report["verified_probe_count"] = len(report["probes"])
    json_dump(audit_path, report)
    return report


def validate_canonical_fragment_owner_result(
    root: Path, task: dict[str, Any], result: dict[str, Any]
) -> None:
    """The owner fixes fragments/support before any A/B/C/D/E measurement."""
    assigned = list(task.get("assigned_languages") or [])
    if len(assigned) != 1:
        raise BenchmarkError("canonical fragment owner must be language-sharded")
    language = str(assigned[0])
    catalog = canonical_fragment_catalog(root, result)
    if not bool((result.get("evidence") or {}).get("synthetic")):
        validate_f20_record_against_runtime_baseline(
            root, language, catalog["F20.P1"]
        )
    validate_canonical_fragment_verification(
        root, language, catalog, result.get("evidence") or {}
    )
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
    total = 0.0
    awarded = 0.0
    for probe in matrix.get("probes", []):
        probe_id = str(probe.get("probe_id"))
        points = float(probe.get("capability_denominator") or 0)
        total += points
        awarded += points * factors.get(catalog[probe_id]["level"], 0.0)
    if total <= 0:
        raise BenchmarkError("canonical fragment catalog has no capability denominator")
    expected = round(100.0 * awarded / total, 6)
    req = result.get("requirements") or {}
    coverage = req.get("metric.capability_coverage") or {}
    actual = coverage.get(language)
    if not isinstance(actual, (int, float)) or isinstance(actual, bool):
        raise BenchmarkError(
            "canonical fragment owner must report numeric metric.capability_coverage"
        )
    if abs(float(actual) - expected) > 0.02:
        raise BenchmarkError(
            f"canonical fragment owner coverage mismatch for {language}: "
            f"reported={actual}, derived={expected}"
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
        # The micro suite and the adversarial set measure every language on the
        # pinned image; the audit measures only the snapshot's own programs.
        # No model is involved, so provider, model and sampling stay out of the
        # key, and the measurement scripts themselves enter it.
        assigned = (
            [target] if unit.get("runner_action") == "quidra-audit"
            else list(metadata_languages(root))
        )
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
        "validator_contract": str(unit.get("validator_command") or ""),
        "worker_mode": unit.get("worker_mode"),
        "network_allowed": bool(unit.get("network_allowed")),
        "runtime_toolchain_pins": selected_pins,
        "cache_epoch": cache_epoch(
            root, "mechanical" if mechanical else str(unit.get("evaluation"))
        ),
    }
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
    record: dict[str, Any], unit: dict[str, Any]
) -> str | None:
    """Why a certified trial record cannot stand in for a run at the current cap.

    Same cap: same experiment, reusable. A different cap: reusable only when the
    cap demonstrably never mattered - no call was cut off, and no completion
    was larger than the cap that applies now. A record that predates this
    evidence is not reusable until `cache-annotate-caps` has read the run's
    agent traces and written it in.
    """
    if str(unit.get("evaluation") or "") not in TRIAL_EVALUATIONS:
        return None
    current = int(unit.get("max_output_tokens_per_call", 0) or 0)
    if current <= 0:
        return None
    certification = record.get("certification") or {}
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
        return json_load(path)
    return {
        "schema_version": 1,
        "enabled": bool((json_load(root / "run.json").get("inference_identity") or {}).get("model")),
        "hits": {},
        "misses": {},
    }


def _write_cache_status(root: Path, status: dict[str, Any]) -> None:
    status["hit_count"] = len(status.get("hits", {}))
    status["miss_count"] = len(status.get("misses", {}))
    json_dump(root / "results" / "cache_status.json", status)


def hydrate_certified_cache(
    root: Path, evaluation: str | None = None, *, mechanical_only: bool = False
) -> int:
    """Complete every PENDING cacheable unit that has a certified record.

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
        if not cache_path.is_file():
            status["misses"][uid] = {
                "fingerprint": fingerprint,
                "scope": cache_scope(unit),
                "reason": "no certified record",
            }
            continue
        record = json_load(cache_path)
        if (
            record.get("schema_version") != 1
            or record.get("fingerprint") != fingerprint
            or record.get("fingerprint_payload") != payload
            or record.get("result_sha256")
            != sha256_bytes(
                json.dumps(
                    record.get("result"), sort_keys=True, separators=(",", ":")
                ).encode("utf-8")
            )
        ):
            raise BenchmarkError(f"{uid}: certified cache record failed integrity checks")
        cap_problem = cache_cap_reuse_problem(record, unit)
        if cap_problem:
            status["misses"][uid] = {
                "fingerprint": fingerprint,
                "scope": cache_scope(unit),
                "reason": cap_problem,
            }
            continue
        result_path = agent_dir / "result.json"
        agent_dir.mkdir(parents=True, exist_ok=True)
        json_dump(result_path, record["result"])
        json_dump(agent_dir / "cache_receipt.json", {
            "schema_version": 1,
            "status": "HIT",
            "fingerprint": fingerprint,
            "record": str(rel.as_posix()),
            "certification": record.get("certification") or {},
            "fingerprint_payload": payload,
        })
        if mechanical:
            # The record holds the result the measurement scripts wrote; the
            # raw samples stay in the retained evidence of the run that measured.
            check_rc = cmd_command_result_check(argparse.Namespace(workspace=str(root), id=uid))
        else:
            check_rc = cmd_result_check(argparse.Namespace(workspace=str(root), id=unit["assigned_agent_id"]))
        if check_rc != 0:
            raise BenchmarkError(f"{uid}: cached result failed the current validator")
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
            "record": rel.as_posix(),
            "assigned_languages": list(unit.get("assigned_languages", [])),
        }
        status["misses"].pop(uid, None)
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
            raise BenchmarkError(f"packet-only output already exists: {dest}")
        seen.add(rel_text)
        staged.append((dest, data, rel_text))
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


def cmd_task_infer(args: argparse.Namespace) -> int:
    """Render, infer through the credential-less gateway, and apply - in one step.

    This is the packet-only half of the same contract the sandbox-agent runtime
    uses. The outer runner no longer needs provider credentials of its own: it
    renders the frozen packet, asks the trusted gateway over the shared socket,
    and hands the reply straight to the same importer `task-apply` uses. The
    meaning of render -> model -> apply is unchanged; only the credential
    location is.
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
        feedback = previous_attempt_feedback(root, args.id)
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

    completion = response["content"]
    incomplete = client_module.completion_problem(response)
    if incomplete:
        raise BenchmarkError(f"packet-only worker response is incomplete: {incomplete}")
    try:
        worker_response = client_module.parse_model_json(completion)
    except client_module.GatewayClientError as exc:
        raise BenchmarkError(f"packet-only worker response is unusable: {exc}") from exc

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
        eval_content = extract_markdown_sections(eval_source, prompt_sections)
        eval_content = render_workspace_paths(eval_content, root)
        config_content = render_workspace_paths(config_path.read_text(encoding="utf-8"), root)
        metadata_content = render_workspace_paths(
            metadata_path.read_text(encoding="utf-8"), root
        )

        for name, source_path, content in (
            ("worker_core.md", core_path, core_content),
            (eval_path.name, eval_path, eval_content),
            ("primary.json", config_path, config_content),
            ("benchmark_metadata.json", metadata_path, metadata_content),
        ):
            digest = sha256_file(source_path)
            embedded_inputs.append({"path": str(source_path), "sha256": digest})
            embedded_sections.append((
                name,
                "\n\n---\n\n"
                f"## Embedded input: {name}\n"
                f"Source SHA-256: `{digest}`\n\n"
                + content
            ))

        requirements_path, all_requirement_ids = load_evaluation_requirements(root, args.evaluation)
        unknown = sorted(
            rid for rid in set(requirement_ids) - set(all_requirement_ids)
            if not str(rid).startswith(SUPPORT_ADJUDICATION_PREFIX)
        )
        if unknown:
            raise BenchmarkError(
                f"Task Packet names unknown requirement IDs: {', '.join(unknown)}"
            )
        requirements_source_sha = sha256_file(requirements_path)
        requirements_content = json.dumps(
            {"schema_version": 1, "evaluation": args.evaluation, "assigned": requirement_ids},
            indent=2, sort_keys=True,
        ) + "\n"
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
        isolation_block = f"""- Worker mode: packet-only
- Local filesystem, shell, process, editor, IDE, and host-application tools: forbidden
- All permitted local source inputs are embedded in this packet.
- Provider-level network retrieval: {'allowed' if args.network else 'disabled'}
- Return exactly one JSON Worker Response; do not write files directly.
- Worker Response schema: {{"schema_version":1,"task_id":"{args.id}","files":[{{"path":"result.json","content":"<UTF-8 text>"}}]}}"""
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
        # Everything identical across the units that share these inputs comes
        # first, so the trusted gateway can cache it once for all of them; the
        # per-unit header and its requirement list follow. Section 8 of the
        # master prompt records why: a semantic-compression packet carries
        # about 180k tokens of the same methodology assets for every language,
        # and with the language-specific header first none of it was ever read
        # from the cache. The rendered bytes are a permutation of the
        # task-first layout; nothing is added or removed.
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
    for rid in requirement_ids:
        value = req[rid]
        if rid.startswith("gate.") or rid.startswith("coverage."):
            if not isinstance(value, bool):
                raise BenchmarkError(f"{rid}: command gate/coverage result must be boolean")
        elif rid.startswith("metric.") or rid.startswith("condition."):
            if not isinstance(value, dict) or set(value) != set(languages):
                raise BenchmarkError(f"{rid}: command score must contain all fixed languages")
            for language in languages:
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
        shutil.rmtree(agent_dir)
        agent_dir.mkdir(parents=True, exist_ok=True)
        json_dump(agent_dir / "task.json", task)
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


def learnability_toolchain_evidence_problems(
    unit: dict[str, Any], trace: dict[str, Any]
) -> list[str]:
    """Require a real, successful toolchain invocation per assigned language.

    The runtime lets a learnability agent start trials only after it attests
    fixtures_compile_and_run=true. In the first paid run the Rust agents, whose
    rustc could not run at all, attested it anyway and scored trials with a
    string matcher; nothing downstream noticed. The trace records every `run`
    with its exit code, so the attestation is checkable.
    """
    problems: list[str] = []
    actions = list(trace.get("trace", []))
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
                "appears in the agent trace, so fixtures_compile_and_run is unsupported"
            )
    return problems


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


def proficiency_required_trial_ids(root: Path) -> list[str]:
    """The exact fresh-session IDs required by the frozen Primary allocation."""
    cfg = json_load(root / "template" / "config" / "primary.json")["llm_proficiency"]
    workloads = [str(value) for value in cfg["primary_workloads"]]
    scenarios = [str(value) for value in cfg["primary_scenarios"]]
    replications = int(cfg["independent_trials_per_replicated_cell"])
    if replications < 1:
        raise BenchmarkError("LLM Proficiency requires at least one Primary replication")
    trial_ids = [
        f"{slug_id(workload)}--{slug_id(scenario)}--t{replication}"
        for workload in workloads
        for scenario in scenarios
        for replication in range(1, replications + 1)
    ]
    if len(trial_ids) != len(set(trial_ids)):
        raise BenchmarkError(
            "LLM Proficiency workload/scenario names collide after trial-ID normalization"
        )
    return trial_ids


def proficiency_primary_trial_set_sha256(root: Path) -> str:
    payload = json.dumps(
        proficiency_required_trial_ids(root),
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")
    return sha256_bytes(payload)


def proficiency_trial_coverage_problems(
    root: Path, trace: dict[str, Any]
) -> list[str]:
    """Require exactly the predeclared Primary cells and replications."""
    expected = set(proficiency_required_trial_ids(root))
    trials = ((trace.get("trials") or {}).get("trials") or {})
    if not isinstance(trials, dict):
        return ["scored trial records are not an object"]
    observed = {str(trial_id) for trial_id in trials}
    missing = sorted(expected - observed)
    extra = sorted(observed - expected)
    problems: list[str] = []
    if missing:
        problems.append(
            "missing required Primary trials: " + ", ".join(missing)
        )
    if extra:
        problems.append(
            "unexpected Primary trial IDs: " + ", ".join(extra)
        )
    if len(observed) != len(expected):
        problems.append(
            f"Primary trial count is {len(observed)}, expected exactly {len(expected)}"
        )
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
        problems.extend(proficiency_trial_coverage_problems(root, trace))
        problems.extend(_preserved_trial_problems(agent_dir, trace))
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
    order_index = {name: i for i, name in enumerate(language_order)}
    ordered = sorted(scores.items(), key=lambda kv: (-kv[1], order_index[kv[0]]))
    ranking = []
    previous_score = None
    previous_rank = 0
    for index, (language, score) in enumerate(ordered, start=1):
        rank = previous_rank if previous_score is not None and score == previous_score else index
        ranking.append({"rank": rank, "language": language, "score": round(score, 2)})
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


def sc_language_ratio(evidence: Any, spec: dict[str, Any]) -> float | None:
    """A metric the methodology defines over the whole probe universe, not per probe."""
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
                value = sc_language_ratio(evidence, rule)
                if value is None:
                    raise BenchmarkError(
                        f"{metric}: {language} recorded no raw value to recompute from"
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
    owner = config.get("support_level_owner") or {}
    rule = config.get("coverage_from_support") or {}
    owner_id = str(owner.get("requirement_id") or "")
    if not owner_id or not rule:
        return None
    fields = [str(name) for name in (owner.get("fields") or ["support"])]
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
    if not probes:
        return None
    adjudicated = sc_adjudicated_levels(root)
    manifest = json_load(root / "work" / "root" / "manifest.json")
    coverage: dict[str, float] = {}
    for unit in manifest.get("work_units", []):
        if owner_id not in (unit.get("requirement_ids") or []):
            continue
        assigned = list(unit.get("assigned_languages") or [])
        if len(assigned) != 1:
            continue
        language = str(assigned[0])
        result_path = (
            root / "work" / "agents" / str(unit.get("assigned_agent_id"))
            / "result.json"
        )
        if not result_path.is_file():
            return None
        rows = probe_annotation_fields(json_load(result_path), set(probes))
        awarded = 0.0
        total = 0.0
        for probe_id, points in probes.items():
            settled = (adjudicated.get(probe_id) or {}).get(language)
            if settled:
                level = settled
            else:
                found = sc_owner_support_levels(rows.get(probe_id) or {}, fields)
                if len(found) != 1:
                    return None
                level = found.pop()
            total += points
            awarded += points * factors.get(level, 0.0)
        if total <= 0:
            return None
        coverage[language] = 100.0 * awarded / total
    if sorted(coverage) != sorted(languages):
        return None
    return coverage


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
                and "interpreter_run" not in (recipes.get("Quidra") or {})
            )
            requirements[rid] = ok
            evidence.update({
                "probe_count": len(probes),
                "family_count": len(families),
                "semantic_fact_kind_count": len(facts),
                "unknown_semantic_fact_refs": sorted(fact_refs - facts),
                "recipe_languages": sorted(recipes),
                "toolchain_recipe_copies_match": recipes == frozen_recipes,
                "quidra_native_only_recipe": set((recipes.get("Quidra") or {}).keys()) == {"build", "run"},
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
        elif rid in {"gate.objective_rubrics_frozen", "gate.evidence_window_frozen"}:
            methodology = (
                root / "template" / "methodology" / "ecosystem.md"
            ).read_text(encoding="utf-8")
            rubric_markers = (
                "define and freeze an objective rubric or proxy before scoring any language",
                "thresholds, and 0–100 conversion must be applied unchanged to all 10 languages",
                "Do not alter these category weights after measurements begin",
            )
            window_markers = (
                "### Predeclared sampling",
                "freeze a **named target or deterministic external selection rule before evidence collection**",
                "Record the full candidate universe or query needed to reproduce the selection",
                "Use the same number of retrieval routes and the same examination depth for every language",
            )
            if rid == "gate.objective_rubrics_frozen":
                missing = [x for x in rubric_markers if x not in methodology]
            else:
                missing = [x for x in window_markers if x not in methodology]
            requirements[rid] = not missing and fixed_10
            evidence.update({
                f"{rid}.missing_policy_markers": missing,
                f"{rid}.fixed_language_set": fixed_10,
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
            expected_trials = proficiency_primary_trial_set_sha256(root)
            if certification.get("proficiency_primary_trial_set_sha256") != expected_trials:
                problems.append(
                    f"{uid}: cache record was not certified against the current "
                    "complete Primary trial set"
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
            for p in proficiency_trial_coverage_problems(root, trace)
        )
        problems.extend(f"{uid}: {p}" for p in _preserved_trial_problems(agent_dir, trace))
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
    result = {"ok": not problems, "problems": problems, "checked_at_utc": dt.datetime.now(dt.timezone.utc).isoformat()}
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
    if reconcile_rc != 0:
        raise BenchmarkError("ledger reconciliation failed")

    errors, summary = validate_primary_status(json_load(status_path))
    if errors:
        raise BenchmarkError("score status invalid: " + "; ".join(errors))

    privacy_rc = cmd_privacy_check(args)
    if privacy_rc != 0:
        raise BenchmarkError("privacy check failed")

    result = {
        "ok": True,
        "finalized_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "primary_evaluations": summary,
        "note": (
            "Use post-run from the trusted outer runner to import compact retained "
            "artifacts into the local repository and then delete the workspace."
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


def promote_prompt_store(source: Path, root: Path) -> dict[str, Any]:
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

    for manifest_path in sorted(manifests_root.glob("*.json")):
        manifest = json_load(manifest_path)
        prompt_hash = str(manifest.get("prompt_sha256") or "")
        if not re.fullmatch(r"[0-9a-f]{64}", prompt_hash):
            raise BenchmarkError(f"invalid prompt SHA-256 in {manifest_path}")
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
        problems.extend(proficiency_trial_coverage_problems(root, trace))
        problems.extend(_preserved_trial_problems(agent_dir, trace))
        if problems:
            raise BenchmarkError(
                f"{unit['id']}: proficiency cache promotion failed integrity: "
                + "; ".join(problems)
            )
        certification["proficiency_integrity"] = True
        certification["proficiency_primary_trial_set_sha256"] = (
            proficiency_primary_trial_set_sha256(root)
        )
        certification["proficiency_primary_trial_count"] = len(
            proficiency_required_trial_ids(root)
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
    summary = promote_certified_cache(source, evidence)
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
    template = source / "benchmark" / "template"
    pins = (json_load(template / "runtime" / "toolchains.json").get("toolchains") or {})
    policy = json_load(template / "config" / "cache_policy.json")
    declared = policy.get("declared_epochs") or {}
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
        "primary_config": template / "config" / "primary.json",
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
    valid = 0
    for record_path in sorted(cache_root.rglob("*.json")):
        record = json_load(record_path)
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
        spec = template / "methodology" / f"{evaluation}.md"
        if "evaluation_spec" in unit_hashes and spec.is_file() and sha256_file(spec) != unit_hashes["evaluation_spec"]:
            changed.append("evaluation_spec")
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
        "note": "exact_task_packet_sha256 is not recomputed here; a prompt-component "
                "change shows up in the synthetic run, not in this report",
    }


def cmd_cache_impact(args: argparse.Namespace) -> int:
    source = Path(args.source_repo).resolve()
    if not (source / "benchmark" / "cache" / "v1").is_dir():
        raise BenchmarkError(f"source repository has no certified cache: {source}")
    summary = cache_impact(source)
    by_eval: dict[str, int] = {}
    for entry in summary["invalid"]:
        by_eval[entry["evaluation"]] = by_eval.get(entry["evaluation"], 0) + 1
    summary["invalid_by_evaluation"] = by_eval
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


def promote_certified_cache(source: Path, root: Path) -> dict[str, Any]:
    manifest_path = root / "work" / "root" / "manifest.json"
    ledger_path = root / "work" / "root" / "ledger.json"
    status_path = root / "results" / "primary_status.json"
    policy_path = root / "template" / "config" / "cache_policy.json"
    if not (manifest_path.is_file() and ledger_path.is_file() and policy_path.is_file()):
        return {"promoted": 0, "replaced": 0, "reused": 0, "records": []}

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
        return {"promoted": 0, "replaced": 0, "reused": 0, "records": []}

    manifest = json_load(manifest_path)
    ledger = json_load(ledger_path)
    records: list[dict[str, Any]] = []
    skipped: list[dict[str, Any]] = []
    promoted = 0
    replaced = 0
    reused = 0

    for unit in manifest.get("work_units", []):
        if not cache_eligible_unit(root, unit):
            continue
        state = ledger.get("units", {}).get(unit["id"], {}) or {}
        if state.get("status") != "COMPLETE":
            continue
        if state.get("validation_result") != "PASS":
            continue
        if (
            require_primary
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
        if receipt_path.is_file():
            certification = (json_load(receipt_path).get("certification") or {})
            reused += 1
        elif mechanical:
            certification = mechanical_certification(root, unit, agent_dir, result)
        else:
            try:
                certification = cache_certification_for_unit(root, unit, agent_dir)
            except BenchmarkError as exc:
                # One unit that cannot be certified is one record fewer, not a
                # reason to keep every other validated measurement out of the
                # cache. The first paid run lost eighty-seven promotions to a
                # single Rust unit whose compiler had never run.
                skipped.append({"work_unit_id": unit.get("id"), "reason": str(exc)})
                continue
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
        relative = cache_record_relative(unit, fingerprint)
        destination = source / "benchmark" / "cache" / relative
        encoded = (json.dumps(record, indent=2, sort_keys=True) + "\n").encode("utf-8")
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists():
            destination.write_bytes(encoded)
            promoted += 1
        elif json.loads(destination.read_text(encoding="utf-8")).get(
            "result_sha256"
        ) == record["result_sha256"]:
            pass
        elif receipt_path.is_file():
            # The unit was hydrated from this very record, so its result cannot
            # legitimately differ from it. Report the mismatch and leave the
            # stored record alone: one suspect record is no more a reason to
            # keep a paid run out of the cache than one uncertifiable unit is.
            skipped.append({
                "work_unit_id": unit.get("id"),
                "reason": (
                    "hydrated result differs from the certified record it came "
                    f"from: {fingerprint}"
                ),
            })
            continue
        else:
            # A record the run measured again although one already sat at this
            # key: the reuse rules refused the stored one. A trial cut off by an
            # older output cap is that case, and it is invisible to the key,
            # because the cap is deliberately outside it. The fresh measurement
            # is the one those rules accept, so it replaces the stale record.
            # Keeping the old one would make this unit a collision, and a
            # re-measurement, in every later run.
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
        "reused": reused,
        "records": records,
        "skipped": skipped,
    }


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
    result = {
        "schema_version": 1,
        "ok": True,
        "run_id": run.get("run_id"),
        "evaluated_commit_sha": (run.get("evaluated") or {}).get("commit_sha"),
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
        records.append(
            {
                "work_unit_id": str(unit["id"]),
                "agent_id": agent_id,
                "assigned_languages": list(unit.get("assigned_languages") or []),
                "requirement_ids": requirement_ids,
                "requirements": result.get("requirements") or {},
                "evidence": result.get("evidence"),
            }
        )
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
    }
    provenance = {
        "schema_version": 1,
        "run_id": run.get("run_id"),
        "master_prompt_sha256": run.get("master_prompt_sha256"),
        "primary_config_sha256": run.get("primary_config_sha256"),
        "template_tree_sha256": run.get("template_tree_sha256"),
        "cache_tree_sha256_at_start": run.get("cache_tree_sha256"),
        "inference_identity": run.get("inference_identity"),
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
    if not finalization_path.is_file() or not json_load(finalization_path).get("ok"):
        raise BenchmarkError("post-run requires a successful finalize first")

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
        "schema_version": 1,
        "run_id": run_id,
        "evaluated_commit_sha": run.get("evaluated", {}).get("commit_sha"),
        "imported_into_develop_commit": meta["commit_sha"],
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

    fin = sub.add_parser("finalize", help="enforce score/blocker and privacy gates")
    fin.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    fin.set_defaults(func=cmd_finalize)

    restore_guard = sub.add_parser(
        "restore-workspace-guard",
        help="re-create the trusted host sentinel after a CI workspace handoff",
    )
    restore_guard.add_argument("--source-repo", required=True)
    restore_guard.add_argument("--expected-commit")
    restore_guard.set_defaults(func=cmd_restore_workspace_guard)

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
