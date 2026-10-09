#!/usr/bin/env python3
"""Corpus B: observe the programs the test suites compile (README.md, Inputs).

`tests/ir_golden.sh observe` installs this file as `quidra` in a shim directory
that mirrors the build directory (the other files there are symlinks), with
REAL and STORE below filled in, and passes that path to the suites in place of
the real binary. For every invocation that compiles a program (check, ir,
llvm, inspect, patch, run, build, debug, a direct `quidra [--no-cache] FILE.qui`,
and repl)
it records a snapshot and then execs the real binary with the same arguments,
so the suite observes no difference:

  - the arguments, the working directory and the environment keys the compiler
    and CLI read (QUIDRA_*, HOME), except QUIDRA_CACHE_DIR: where a run is
    cached does not change what it compiles, and every suite points it at a
    directory of its own run;
  - for repl, the whole of stdin (fed to the real binary from a file);
  - the content of every .qui, .toml and quidra.package file below the
    entry's directory and below every QUIDRA_PACKAGE_PATH root, stored by
    sha256 (STORE/blobs); identical snapshots are stored once.

The shim compiles nothing. Afterwards:

  observe_shim.py --materialize STORE ROOT   writes each snapshot's file tree
      under ROOT/<snapshot>/files/<original absolute path> and ROOT/entries.tsv
      in the corpus format, so tests/golden/capture.py captures base and head
  observe_shim.py --check-deps ROOT CAPTURE  every source in every captured
      `deps` view must lie inside its snapshot (the closure was complete)
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import sys
import tempfile
from pathlib import Path

REAL = "@REAL@"
STORE = "@STORE@"

COMPILING = {"check", "ir", "llvm", "inspect", "patch", "run", "build", "debug"}
UNRECORDED_ENVIRONMENT = {"QUIDRA_CACHE_DIR"}
SNAPSHOT_SUFFIXES = (".qui", ".toml")
SNAPSHOT_NAMES = ("quidra.package",)
MAX_FILES = 20000
MAX_BYTES = 4 << 20


def wanted(name: str) -> bool:
    return name.endswith(SNAPSHOT_SUFFIXES) or name in SNAPSHOT_NAMES


def store_blob(store: Path, data: bytes) -> str:
    digest = hashlib.sha256(data).hexdigest()
    target = store / "blobs" / digest[:2] / digest
    if not target.exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as handle:
            handle.write(data)
        os.replace(handle.name, target)
    return digest


def collect(store: Path, roots: list[Path], files: dict[str, str]) -> None:
    for root in roots:
        if not root.is_dir():
            continue
        for directory, subdirectories, names in os.walk(root):
            subdirectories[:] = sorted(d for d in subdirectories
                                       if not d.startswith(".") and d not in ("build", "__pycache__"))
            for name in sorted(names):
                if not wanted(name) or len(files) >= MAX_FILES:
                    continue
                path = Path(directory) / name
                try:
                    if path.stat().st_size > MAX_BYTES:
                        continue
                    files[str(path.resolve())] = store_blob(store, path.read_bytes())
                except OSError:
                    continue


def record(argv: list[str]) -> int | None:
    """Snapshots a compiling invocation; returns a file descriptor for the real
    binary's stdin when the input was teed (repl)."""
    store = Path(STORE)
    arguments = argv[1:]
    if not arguments:
        return None
    command = arguments[0]
    entry = None
    if command.endswith(".qui"):
        entry = command
    elif command == "--no-cache" and len(arguments) > 1 and arguments[1].endswith(".qui"):
        entry = arguments[1]
    elif command in COMPILING:
        entry = next((a for a in arguments[1:] if a.endswith(".qui")), None)
    elif command != "repl":
        return None
    cwd = Path.cwd()
    environment = {k: v for k, v in os.environ.items()
                   if (k.startswith("QUIDRA_") or k == "HOME") and k not in UNRECORDED_ENVIRONMENT}
    snapshot = {"argv": arguments, "cwd": str(cwd), "environment": environment}
    stdin_fd = None
    roots: list[Path] = []
    if command == "repl":
        data = b"" if sys.stdin.isatty() else sys.stdin.buffer.read()
        snapshot["kind"] = "repl-session"
        snapshot["stdin"] = store_blob(store, data)
        roots.append(cwd)
        replay = tempfile.TemporaryFile()
        replay.write(data)
        replay.seek(0)
        stdin_fd = os.dup(replay.fileno())
        replay.close()
    else:
        path = (cwd / entry).resolve()
        if not path.is_file():
            return None
        snapshot["kind"] = "file"
        snapshot["entry"] = str(path)
        roots.append(path.parent)
    separator = ";" if os.name == "nt" else ":"
    for part in environment.get("QUIDRA_PACKAGE_PATH", "").split(separator):
        if part:
            roots.append(Path(part).resolve())
    files: dict[str, str] = {}
    collect(store, roots, files)
    snapshot["files"] = dict(sorted(files.items()))
    text = json.dumps(snapshot, sort_keys=True).encode()
    key = hashlib.sha256(text).hexdigest()[:20]
    target = store / "snapshots" / f"{key}.json"
    if not target.exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as handle:
            handle.write(text)
        os.replace(handle.name, target)
    return stdin_fd


def shim() -> None:
    stdin_fd = None
    try:
        stdin_fd = record(sys.argv)
    except Exception as error:  # observation must never change the suite's result
        try:
            with open(Path(STORE) / "shim-errors.log", "a", encoding="utf-8") as log:
                log.write(f"{sys.argv!r}: {error!r}\n")
        except OSError:
            pass
    if stdin_fd is not None:
        os.dup2(stdin_fd, 0)
        os.close(stdin_fd)
    os.execv(REAL, [REAL, *sys.argv[1:]])


def materialize(store: Path, root: Path) -> None:
    import shutil
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    rows = []
    for path in sorted((store / "snapshots").glob("*.json")):
        snapshot = json.loads(path.read_text(encoding="utf-8"))
        key = path.stem
        base = root / key / "files"

        def mapped(original: str) -> Path:
            return base / original.lstrip("/").replace(":", "_")

        for original, digest in snapshot["files"].items():
            target = mapped(original)
            target.parent.mkdir(parents=True, exist_ok=True)
            blob = store / "blobs" / digest[:2] / digest
            try:
                os.link(blob, target)
            except OSError:
                shutil.copyfile(blob, target)
        cwd = mapped(snapshot["cwd"])
        cwd.mkdir(parents=True, exist_ok=True)
        separator = ";" if os.name == "nt" else ":"
        package_path = separator.join(
            str(mapped(str(Path(p).resolve()))) for p in
            snapshot["environment"].get("QUIDRA_PACKAGE_PATH", "").split(separator) if p)
        if snapshot["kind"] == "repl-session":
            session = root / key / "session.input"
            shutil.copyfile(store / "blobs" / snapshot["stdin"][:2] / snapshot["stdin"], session)
            rows.append([f"{key}/session.input", "repl-session", str(session), str(cwd), package_path])
        else:
            entry = mapped(snapshot["entry"])
            rows.append([f"{key}/{entry.name}", "file", str(entry), str(cwd), package_path])
    (root / "entries.tsv").write_text("".join("\t".join(r) + "\n" for r in rows), encoding="utf-8")
    print(f"observe_shim.py: {len(rows)} snapshots under {root}")


def check_deps(root: Path, capture: Path) -> int:
    incomplete = 0
    for line in (root / "entries.tsv").read_text(encoding="utf-8").splitlines():
        name = line.split("\t")[0]
        deps = capture / "out" / name / "deps"
        if not deps.exists():
            continue
        snapshot = (root / name.split("/")[0]).resolve()
        for row in deps.read_text(encoding="utf-8").splitlines():
            match = re.match(r'^source "((?:[^"\\]|\\.)*)"', row)
            if match:
                source = re.sub(r"\\(.)", r"\1", match.group(1))
                if not str(Path(source).resolve()).startswith(str(snapshot)):
                    print(f"observe_shim.py: {name}: source outside its snapshot: {source}")
                    incomplete += 1
    print(f"observe_shim.py: deps closure {'complete' if not incomplete else 'INCOMPLETE'}")
    return 1 if incomplete else 0


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--materialize":
        materialize(Path(sys.argv[2]), Path(sys.argv[3]))
    elif len(sys.argv) == 4 and sys.argv[1] == "--check-deps":
        raise SystemExit(check_deps(Path(sys.argv[2]), Path(sys.argv[3])))
    else:
        shim()
