#!/usr/bin/env python3
"""Reusable Quidra benchmark orchestration helpers.

This CLI handles deterministic benchmark bookkeeping. It intentionally does not
implement language-specific scoring logic from the benchmark methodology.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
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
    if branch != "develop":
        raise BenchmarkError(f"benchmark target must be develop, got {branch!r}")
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
        "materialized_reusable_artifacts": len(reused),
        "source_snapshot": snapshot,
        "template_source_snapshot": template_snapshot,
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
    return problems


def assert_template_integrity(root: Path) -> None:
    run_path = root / "run.json"
    if not run_path.is_file():
        raise BenchmarkError("run.json is required before benchmark planning")
    problems = template_integrity_problems(root, json_load(run_path))
    if problems:
        raise BenchmarkError("template integrity failed: " + ", ".join(problems))


def command_version_output(cmd: list[str], cwd: Path) -> str:
    p = subprocess.run(
        cmd,
        cwd=cwd,
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
    for language, commands in TOOLCHAIN_COMMANDS.items():
        outputs: list[str] = []
        try:
            for cmd in commands:
                outputs.append(command_version_output(cmd, root / "repo"))
            results[language] = {
                "canonical": canonical_toolchain_fingerprint(language, outputs),
                "raw": outputs,
                "commands": commands,
            }
        except (BenchmarkError, FileNotFoundError) as exc:
            results[language] = {"error": str(exc), "commands": commands}
            missing.append(language)
    payload = {
        "schema_version": 1,
        "checked_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "toolchains": results,
        "missing": missing,
        "ok": not missing,
    }
    out = root / "results" / "toolchains.json"
    json_dump(out, payload)
    print(json.dumps(payload, indent=2))
    return 0 if (not args.strict or not missing) else 2


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
    }
    for key in ("LANG", "LC_ALL", "LC_CTYPE", "TZ"):
        value = os.environ.get(key)
        if value:
            env[key] = value
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
        unknown_requirements = sorted(set(requirement_ids) - allowed_requirement_ids)
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
        max_attempts = int(raw.get("max_attempts", 3) or 3)
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
            elif runner_action not in {"micro-measure"}:
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
        representation = by_id.get("lq-qudra-representation")
        if representation is None or representation.get("result_kind") != "audit":
            raise BenchmarkError(
                "language_quality: missing Quidra representation audit unit"
            )
        shards = [
            unit for unit in normalized
            if unit["id"].startswith("lq-qudra-micro-authoring-")
        ]
        expected_workloads = {
            "mb00", "mb01", "mb02", "mb03", "mb04", "mb05",
            "mb06", "mb07", "mb08", "mb09", "mb10", "mb11",
        }
        max_per_leaf = int(
            json_load(root / "template" / "config" / "primary.json")
            .get("runner", {})
            .get("max_quidra_workloads_per_authoring_leaf", 3)
        )
        if len(shards) != 4:
            raise BenchmarkError(
                f"language_quality: expected exactly 4 Quidra authoring shards, got {len(shards)}"
            )
        owners: dict[str, list[str]] = {}
        for shard in shards:
            workload_ids = list(shard.get("workload_ids", []))
            if (
                shard.get("result_kind") != "audit"
                or not workload_ids
                or len(workload_ids) > max_per_leaf
            ):
                raise BenchmarkError(
                    f"{shard['id']}: invalid Quidra authoring shard size/result kind"
                )
            if shard.get("dependencies") != ["lq-qudra-representation"]:
                raise BenchmarkError(
                    f"{shard['id']}: must depend only on lq-qudra-representation"
                )
            for workload in workload_ids:
                owners.setdefault(workload, []).append(shard["id"])
        missing_workloads = sorted(expected_workloads - set(owners))
        duplicate_workloads = sorted(
            workload for workload, unit_ids in owners.items()
            if len(unit_ids) != 1
        )
        unknown_workloads = sorted(set(owners) - expected_workloads)
        if missing_workloads or duplicate_workloads or unknown_workloads:
            raise BenchmarkError(
                "language_quality: invalid Quidra authoring workload coverage; "
                f"missing={missing_workloads}, duplicate={duplicate_workloads}, "
                f"unknown={unknown_workloads}"
            )
        mechanical = by_id.get("lq-micro-mechanical")
        required_dependencies = {
            "lq-qudra-representation",
            *(shard["id"] for shard in shards),
        }
        if (
            mechanical is None
            or mechanical.get("execution_kind") != "command"
            or set(mechanical.get("dependencies", [])) != required_dependencies
        ):
            raise BenchmarkError(
                "language_quality: mechanical micro unit must depend on the "
                "representation audit and all four authoring shards"
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


def cmd_deterministic_plan(args: argparse.Namespace) -> int:
    root = workspace(args)
    assert_template_integrity(root)
    require_privacy_pass(root)
    templates = load_work_plan_templates(root)
    primary = json_load(root / "template" / "config" / "primary.json")
    default_max_attempts = int(primary.get("runner", {}).get("max_attempts_per_work_unit", 3))
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
        split_bases = {
            str(raw["id"])
            for raw in spec.get("units", [])
            if bool(raw.get("split_by_language", False))
        }
        fixed_languages = metadata_languages(root)
        for raw in spec.get("units", []):
            base_uid = str(raw["id"])
            execution_kind = str(raw.get("execution_kind", "agent"))
            result_kind = str(raw.get("result_kind", "requirements"))
            split = bool(raw.get("split_by_language", False)) and execution_kind == "agent"
            shards: list[list[str]] = (
                [[language] for language in fixed_languages] if split else [[]]
            )
            for assigned_languages in shards:
                suffix = (
                    f"--{slug_id(assigned_languages[0])}"
                    if assigned_languages
                    else ""
                )
                uid = base_uid + suffix
                agent_id = (
                    f"worker-{uid}" if execution_kind == "agent" else f"system-{uid}"
                )
                deps: list[str] = []
                for dep in [str(x) for x in raw.get("dependencies", [])]:
                    if dep in split_bases and assigned_languages:
                        deps.append(dep + f"--{slug_id(assigned_languages[0])}")
                    elif dep in split_bases:
                        deps.extend(
                            dep + f"--{slug_id(language)}"
                            for language in fixed_languages
                        )
                    else:
                        deps.append(dep)
                if audit_ids:
                    raw_reads = [
                        lexical_absolute(root / p)
                        for p in raw.get("read_paths", [])
                        if (root / p).exists()
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
                max_calls = (
                    (total_calls + len(fixed_languages) - 1) // len(fixed_languages)
                    if split and total_calls
                    else total_calls
                )
                language_text = (
                    f" Assigned language: {assigned_languages[0]}."
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
                    if validator_action == "verify-quidra-representation":
                        validator_command = (
                            f"python3 {root / 'template' / 'scripts' / 'micro_measure.py'} "
                            f"verify-representation --workspace {root} --agent-id {agent_id}"
                        )
                    elif validator_action == "verify-quidra-micro-shard":
                        validator_command = (
                            f"python3 {root / 'template' / 'scripts' / 'micro_measure.py'} "
                            f"verify-shard --workspace {root} --unit-id {uid} --agent-id {agent_id}"
                        )
                    elif validator_action == "verify-quidra-micro":
                        validator_command = (
                            f"python3 {root / 'template' / 'scripts' / 'micro_measure.py'} "
                            f"verify-quidra --workspace {root} --agent-id {agent_id}"
                        )
                    elif validator_action is None:
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
                    "read_paths": [
                        str(require_under(root / p, root))
                        for p in raw.get("read_paths", [])
                        if (root / p).exists()
                    ],
                    "evidence_paths": evidence_paths,
                    "validator_command": validator_command,
                    "network_allowed": bool(raw.get("network_allowed", False)),
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
        ns = argparse.Namespace(
            workspace=str(root),
            id=agent_id,
            parent=parent,
            evaluation=unit["evaluation"],
            goal=goal,
            read=unit.get("read_paths", []),
            write=str(agent_dir),
            output=unit["evidence_paths"],
            validate=unit["validator_command"],
            network=bool(unit.get("network_allowed", False)),
            depth=int(args.depth),
            section=unit.get("prompt_sections", []),
            requirement_id=unit.get("requirement_ids", []),
            language=assigned_languages,
            worker_mode=unit.get("worker_mode", "packet-only"),
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
            max_attempts = int(raw.get("max_attempts", 3) or 3)
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
                "assigned_languages": list(raw.get("assigned_languages", [])),
                "runner_action": raw.get("runner_action"),
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


def cmd_ledger_update(args: argparse.Namespace) -> int:
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
        if not isinstance(content, str):
            raise BenchmarkError(
                f"packet-only output content must be UTF-8 text: {rel_text}"
            )
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
        response = client.complete(
            [
                {
                    "role": "system",
                    "content": (
                        "You are a packet-only benchmark worker. You have no local "
                        "filesystem, shell, process, editor or host-application tools. "
                        "Every permitted input is embedded in the Task Packet below. "
                        "Reply with exactly one JSON Worker Response object and nothing "
                        "else."
                    ),
                },
                {"role": "user", "content": packet},
            ],
            task_id=args.id,
            max_output_tokens=int(args.max_output_tokens),
            network_allowed=bool(meta.get("network_allowed")),
        )
    except client_module.GatewayRefusal as exc:
        raise BenchmarkError(f"inference gateway refused this Task Packet: {exc}") from exc
    except client_module.GatewayClientError as exc:
        raise BenchmarkError(f"inference transport failure: {exc}") from exc

    completion = response["content"]
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
        unknown = sorted(set(requirement_ids) - set(all_requirement_ids))
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
    components = [store_prompt_component(root, packet, "task")]
    for name, section in embedded_sections:
        components.append(store_prompt_component(root, section, f"embedded:{name}"))
    for name, packet_input in packet_input_sections:
        components.append(store_prompt_component(root, packet_input, name))
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
        "rendered_bytes": len(rendered),
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
    result = json_load(result_path)
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
            else:
                raise BenchmarkError(f"unsupported requirement result type: {rid}")

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
        "archived_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
    })
    if reset:
        task = json_load(agent_dir / "task.json")
        shutil.rmtree(agent_dir)
        agent_dir.mkdir(parents=True, exist_ok=True)
        json_dump(agent_dir / "task.json", task)
    return str(archive)


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
    if rc == 0:
        ns = argparse.Namespace(
            workspace=str(root), id=args.id, status="COMPLETE",
            evidence=unit.get("evidence_paths", []), validation_result="PASS",
            blocker=None, blocker_class=None,
        )
        return cmd_ledger_update(ns)

    attempts = int(state.get("attempts", 0))
    max_attempts = int(state.get("max_attempts", 3))
    if attempts < max_attempts:
        archive_attempt(
            root, unit, attempts, "validation-failed-retry", reset=True
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
        root, unit, attempts, "validation-failed-exhausted", reset=False
    )
    ns = argparse.Namespace(
        workspace=str(root), id=args.id, status="BLOCKED",
        evidence=unit.get("evidence_paths", []), validation_result="FAIL",
        blocker=f"validator failed after {attempts} attempts",
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
            result_path = Path(evidence[0])
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


def cmd_aggregate_primary(args: argparse.Namespace) -> int:
    root = workspace(args)
    evaluation = args.evaluation
    req = requirement_results_for_evaluation(root, evaluation)
    aggregation = json_load(root / "template" / "config" / "aggregation.json")
    languages = metadata_languages(root)
    config = aggregation["evaluations"][evaluation]

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
    assert_template_integrity(root)
    cmd_reclaim_stale(argparse.Namespace(workspace=str(root)))
    propagate_dependency_blockers(root)

    progressed = True
    while progressed:
        progressed = False
        manifest = json_load(root / "work" / "root" / "manifest.json")
        ledger = json_load(root / "work" / "root" / "ledger.json")
        for unit in manifest.get("work_units", []):
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
                elif action == "micro-measure":
                    script = root / "template" / "scripts" / "micro_measure.py"
                    p = subprocess.run(
                        [
                            sys.executable, str(script), "measure",
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
                            f"micro measurement failed: {p.stderr or p.stdout}"
                        )
                    check_rc = cmd_command_result_check(
                        argparse.Namespace(workspace=str(root), id=uid)
                    )
                    if check_rc != 0:
                        raise BenchmarkError("micro command result validation failed")
                else:
                    raise BenchmarkError(
                        f"unsupported runner command action for {uid}: {action!r}"
                    )
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
        workspace=str(root), evaluation=None, parent=None, depth=1
    ))

    manifest = json_load(root / "work" / "root" / "manifest.json")
    ledger = json_load(root / "work" / "root" / "ledger.json")
    queue = []
    for unit in manifest.get("work_units", []):
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
    """Run every deterministic pre-dispatch step and emit the first dispatch queue."""
    root = workspace(args)
    manifest_path = root / "work" / "root" / "manifest.json"

    if manifest_path.is_file():
        rc = cmd_plan(argparse.Namespace(workspace=str(root), strict=True))
        if rc != 0:
            return rc
        return cmd_advance(argparse.Namespace(workspace=str(root)))

    steps = [
        ("preflight", cmd_preflight, argparse.Namespace(workspace=str(root))),
        ("toolchain-scan", cmd_toolchain_scan, argparse.Namespace(workspace=str(root), strict=False)),
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

    rc = cmd_advance(argparse.Namespace(workspace=str(root)))
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
    "windows_home": re.compile(r"(?i)\b[A-Z]:\\\\Users\\\\[^\\\s]+\\\\"),
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


def cmd_privacy_check(args: argparse.Namespace) -> int:
    root = workspace(args)
    findings = []
    scan_roots = [*retained_run_roots(root), root / "template"]
    for path in iter_text_files(scan_roots):
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for kind, pattern in PRIVACY_PATTERNS.items():
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

    staging = benchmark_dir / f".{run_id}.importing"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)

    expected_hashes = retained_file_hashes(root)
    copied_hashes = copy_retained_run(root, staging)
    if copied_hashes != expected_hashes:
        shutil.rmtree(staging, ignore_errors=True)
        raise BenchmarkError("retained artifact hash verification failed during import")

    import_manifest = {
        "schema_version": 1,
        "run_id": run_id,
        "evaluated_commit_sha": run.get("evaluated", {}).get("commit_sha"),
        "imported_into_develop_commit": meta["commit_sha"],
        "retained_file_count": len(expected_hashes),
        "retained_files_sha256": expected_hashes,
    }
    json_dump(staging / "import_manifest.json", import_manifest)
    os.replace(staging, destination)

    if retained_file_hashes(destination) != expected_hashes:
        raise BenchmarkError(
            f"repository import exists but post-rename verification failed: {destination}"
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
    adv.set_defaults(func=cmd_advance)

    prep = sub.add_parser("prepare", help="run all deterministic pre-dispatch steps and emit the first queue")
    prep.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
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
    ti.add_argument("--timeout", type=float, default=900.0)
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
