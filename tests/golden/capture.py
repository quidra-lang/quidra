#!/usr/bin/env python3
"""Capture the golden views of every corpus entry (README.md, Capture and comparison).

Runs quidra_golden_dump once per entry (in parallel) under a fixed
environment, enforces a per-entry timeout, and writes

  OUT/out/<entry>/<view>[.error]   the views
  OUT/index.tsv                    <entry> <view> <sha256> <bytes> <status>

where status is ok, error:<code>, timeout, signal:<n> or exit:<n>.

--batch compiles every entry in one process per environment group instead
(forward or reverse order); its index must equal the per-process index.
--perturb, --reverse and --permute-views are the selfcheck variations.
--cli QUIDRA captures `QUIDRA ir FILE` / `QUIDRA llvm FILE` as the views ir
and llvm of every file entry instead (the CLI fidelity check).
--run QUIDRA adds the view `run` to the file entries that corpus.toml lists
as [[run]]: each is built with `QUIDRA build` and the program is run (see
run_view).
"""

from __future__ import annotations

import argparse
import hashlib
import os
import random
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / "scripts"))
import toml_subset  # noqa: E402

# Views per entry kind, in their canonical order. quidra_golden_dump must offer
# the same names.
VIEWS = {
    "file": ["ir", "ir.full", "ir.lowered", "llvm", "llvm.lowered", "llvm.lowered.debug",
             "llvm.debug", "repl", "diagnostics", "inspect", "deps", "census", "census.repl"],
    "memory": ["compile.memory", "census"],
    "fixture": ["fixture", "census"],
    "repl-session": ["repl.session", "census"],
}

KIND_OPTION = {"file": [], "memory": ["--memory"], "fixture": ["--fixture"],
               "repl-session": ["--repl-session"]}

# The view that capture.py produces itself, with --run, for the file entries
# listed as [[run]] in corpus.toml. It is not a quidra_golden_dump view.
RUN_VIEW = "run"
# Seconds a program may run when its [[run]] table names no timeout.
DEFAULT_RUN_TIMEOUT = 60


# Variables added to every entry's environment (--env), e.g. LLVM_PROFILE_FILE
# for the coverage gate.
EXTRA_ENVIRONMENT: dict[str, str] = {}


def golden_environment(home: Path, package_path: str, perturb: bool) -> dict[str, str]:
    environment = {
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "HOME": str(home),
        "QUIDRA_PACKAGE_PATH": package_path,
        "LC_ALL": "C",
        "LANG": "C",
        "TZ": "UTC",
    }
    if perturb:
        # macOS libmalloc scribbles freed and new blocks; glibc perturbs both.
        environment["MallocScribble"] = "1"
        environment["MallocPreScribble"] = "1"
        environment["MALLOC_PERTURB_"] = "165"
    if os.name == "nt":
        for key in ("SYSTEMROOT", "TEMP", "TMP", "USERPROFILE"):
            if key in os.environ:
                environment[key] = os.environ[key]
    environment.update(EXTRA_ENVIRONMENT)
    return environment


def read_entries(root: Path, only: list[str] | None) -> list[dict]:
    entries = []
    for line in (root / "entries.tsv").read_text(encoding="utf-8").splitlines():
        if not line:
            continue
        name, kind, path, cwd, package_path = line.split("\t")
        if only and not any(name.startswith(prefix) for prefix in only):
            continue
        entries.append({"name": name, "kind": kind, "path": path, "cwd": cwd,
                        "package_path": package_path})
    return entries


def load_pins() -> dict:
    return toml_subset.load(HERE / "corpus.toml") if (HERE / "corpus.toml").exists() else {}


def timeouts() -> tuple[int, dict[str, int]]:
    pins = load_pins()
    default = pins.get("corpus", {}).get("timeout", 300)
    return default, {t["entry"]: t["seconds"] for t in pins.get("timeout", [])}


def run_specs() -> dict[str, dict]:
    """The [[run]] tables of corpus.toml by entry: `args` and `stdin` (lists
    of strings; stdin is the lines, each followed by a newline) and
    `timeout` (seconds), all optional."""
    return {spec["entry"]: spec for spec in load_pins().get("run", [])}


def view_order(kind: str, permute: int | None, name: str, requested: list[str] | None) -> list[str]:
    views = [v for v in VIEWS[kind] if not requested or v in requested]
    if permute is not None:
        rng = random.Random(f"{permute}:{name}")
        rng.shuffle(views)
    return views


def index_entry(name: str, views: list[str], directory: Path, process_status: str) -> list[str]:
    statuses = {}
    status_file = directory / "status.tsv"
    if status_file.exists():
        for line in status_file.read_text(encoding="utf-8").splitlines():
            view, status = line.split("\t")
            statuses[view] = status
    rows = []
    if process_status != "ok":
        # A process that timed out, crashed or exited early may have written
        # some views, the last one possibly truncated; which ones depends on
        # timing, so none of them counts.
        return [f"{name}\t{view}\t-\t0\t{process_status}" for view in sorted(views)]
    for view in sorted(views):
        for candidate in (directory / view, directory / f"{view}.error"):
            if candidate.exists():
                data = candidate.read_bytes()
                status = statuses.get(view, process_status if process_status != "ok" else "ok")
                rows.append(f"{name}\t{view}\t{hashlib.sha256(data).hexdigest()}\t{len(data)}\t{status}")
                break
        else:
            status = process_status if process_status != "ok" else "missing"
            rows.append(f"{name}\t{view}\t-\t0\t{status}")
    return rows


def run_entry(tool: Path, home: Path, out: Path, entry: dict, views: list[str], timeout: int,
              perturb: bool) -> list[str]:
    directory = out / "out" / entry["name"]
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    target = entry["name"].split("/", 1)[1] if entry["kind"] == "fixture" else entry["path"]
    command = [str(tool), "--out", str(directory), "--views", ",".join(views),
               *KIND_OPTION[entry["kind"]], target]
    try:
        result = subprocess.run(
            command, cwd=entry["cwd"],
            env=golden_environment(home, entry["package_path"], perturb),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        if result.returncode == 0:
            status = "ok"
        elif result.returncode < 0:
            status = f"signal:{-result.returncode}"
        else:
            status = f"exit:{result.returncode}"
        if result.returncode != 0:
            (directory / "process.stderr").write_bytes(result.stderr)
    except subprocess.TimeoutExpired:
        status = "timeout"
    return index_entry(entry["name"], views, directory, status)


CLI_ERROR = re.compile(rb"error\[([A-Z0-9_]+)\]")


def run_view(quidra: Path, home: Path, out: Path, entry: dict, spec: dict) -> str:
    """The `run` view of a file entry: build it with `QUIDRA build` in the
    entry's directory, as the compile views do, then run the program with
    the spec's arguments and stdin, in an empty working directory, under the
    capture environment and a run cache of the capture's own. The view is

      exit <status>            (or `signal <n>`)
      stdout <bytes> bytes
      <stdout>
      stderr <bytes> bytes
      <stderr>

    with status ok; a build that fails writes its stderr to run.error with
    status error:<code>, a program that outlives its timeout has status
    timeout. The program and its working directory live under HOME/run,
    the same path for every capture under one golden home, so a path the
    program prints is equal in base and head captures."""
    name = entry["name"]
    directory = out / "out" / name
    for stale in (directory / RUN_VIEW, directory / f"{RUN_VIEW}.error"):
        if stale.exists():
            stale.unlink()
    work = home / "run" / name
    if work.exists():
        shutil.rmtree(work)
    cwd = work / "cwd"
    cwd.mkdir(parents=True)
    program = work / ("program.exe" if os.name == "nt" else "program")
    environment = golden_environment(home, entry["package_path"], False)
    environment["QUIDRA_CACHE_DIR"] = str(out / "run-cache")
    build = subprocess.run([str(quidra), "build", entry["path"], "-o", str(program)],
                           cwd=entry["cwd"], env=environment,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if build.returncode != 0:
        data = build.stderr
        code = CLI_ERROR.search(data)
        (directory / f"{RUN_VIEW}.error").write_bytes(data)
        status = f"error:{code.group(1).decode() if code else 'build'}"
    else:
        stdin = "".join(line + "\n" for line in spec.get("stdin", [])).encode("utf-8")
        try:
            result = subprocess.run([str(program), *spec.get("args", [])], cwd=cwd,
                                    env=environment, input=stdin, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE,
                                    timeout=spec.get("timeout", DEFAULT_RUN_TIMEOUT))
        except subprocess.TimeoutExpired:
            shutil.rmtree(work, ignore_errors=True)
            return f"{name}\t{RUN_VIEW}\t-\t0\ttimeout"
        ending = (f"signal {-result.returncode}" if result.returncode < 0
                  else f"exit {result.returncode}")
        data = (f"{ending}\nstdout {len(result.stdout)} bytes\n".encode("utf-8") + result.stdout
                + f"\nstderr {len(result.stderr)} bytes\n".encode("utf-8") + result.stderr)
        (directory / RUN_VIEW).write_bytes(data)
        status = "ok"
    shutil.rmtree(work, ignore_errors=True)
    return f"{name}\t{RUN_VIEW}\t{hashlib.sha256(data).hexdigest()}\t{len(data)}\t{status}"


def run_rows(quidra: Path | None, home: Path, out: Path, entries: list[dict],
             requested: list[str] | None, jobs: int) -> list[str]:
    """The `run` view of every listed file entry (with --run)."""
    if quidra is None or (requested and RUN_VIEW not in requested):
        return []
    specs = run_specs()
    runnable = [e for e in entries if e["kind"] == "file" and e["name"] in specs]
    for entry in runnable:
        (out / "out" / entry["name"]).mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(run_view, quidra, home, out, entry, specs[entry["name"]])
                   for entry in runnable]
        return [future.result() for future in futures]


def run_cli(quidra: Path, home: Path, out: Path, entry: dict, views: list[str], timeout: int,
            perturb: bool) -> list[str]:
    """`quidra ir FILE` and `quidra llvm FILE` as the CLI runs them."""
    directory = out / "out" / entry["name"]
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    statuses = []
    for view in views:
        try:
            result = subprocess.run(
                [str(quidra), view, entry["path"]], cwd=entry["cwd"],
                env=golden_environment(home, entry["package_path"], perturb),
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        except subprocess.TimeoutExpired:
            statuses.append(f"{view}\ttimeout")
            continue
        if result.returncode == 0:
            (directory / view).write_bytes(result.stdout)
            statuses.append(f"{view}\tok")
        else:
            (directory / f"{view}.error").write_bytes(result.stderr)
            code = CLI_ERROR.search(result.stderr)
            statuses.append(f"{view}\terror:{code.group(1).decode() if code else 'cli'}")
    (directory / "status.tsv").write_text("\n".join(sorted(statuses)) + "\n", encoding="utf-8")
    return index_entry(entry["name"], views, directory, "ok")


def run_batch(tool: Path, home: Path, out: Path, entries: list[dict], order: str,
              requested: list[str] | None, perturb: bool) -> list[str]:
    """One process per environment group and entry kind (a batch shares one
    view list), every entry of the group in that process."""
    groups: dict[tuple[str, str], list[dict]] = {}
    for entry in entries:
        groups.setdefault((entry["package_path"], entry["kind"]), []).append(entry)
    rows = []
    batches = out / "batches"
    batches.mkdir(parents=True, exist_ok=True)
    for number, ((package_path, kind), members) in enumerate(sorted(groups.items())):
        views = view_order(kind, None, "", requested)
        listing = batches / f"batch-{number}.tsv"
        lines = []
        for entry in members:
            target = entry["name"].split("/", 1)[1] if kind == "fixture" else entry["path"]
            lines.append("\t".join([entry["name"], kind, target, entry["cwd"]]))
        listing.write_text("\n".join(lines) + "\n", encoding="utf-8")
        result = subprocess.run(
            [str(tool), "--batch", str(listing), "--order", order, "--out", str(out / "out"),
             "--views", ",".join(views)],
            env=golden_environment(home, package_path, perturb),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        status = "ok" if result.returncode == 0 else (
            f"signal:{-result.returncode}" if result.returncode < 0 else f"exit:{result.returncode}")
        if result.returncode != 0:
            (batches / f"batch-{number}.stderr").write_bytes(result.stderr)
        for entry in members:
            rows.extend(index_entry(entry["name"], views, out / "out" / entry["name"], status))
    return rows


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tool", required=True)
    parser.add_argument("--root", required=True, help="materialized corpus root")
    parser.add_argument("--home", required=True, help="empty HOME for the compiler")
    parser.add_argument("--out", required=True)
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--views", help="comma-separated subset of views")
    parser.add_argument("--only", action="append", help="entry name prefix (repeatable)")
    parser.add_argument("--reverse", action="store_true", help="schedule entries in reverse")
    parser.add_argument("--permute-views", type=int, help="seed for a per-entry view order")
    parser.add_argument("--perturb", action="store_true", help="scribble freed memory")
    parser.add_argument("--batch", choices=["forward", "reverse"])
    parser.add_argument("--cli", help="capture the ir/llvm views through this quidra CLI")
    parser.add_argument("--run", help="add the run view of the [[run]] entries, built with "
                                      "this quidra CLI")
    parser.add_argument("--env", action="append", default=[], metavar="KEY=VALUE",
                        help="add a variable to every entry's environment")
    args = parser.parse_args()
    for assignment in args.env:
        key, _, value = assignment.partition("=")
        EXTRA_ENVIRONMENT[key] = value

    tool = Path(args.tool).resolve()
    root = Path(args.root).resolve()
    home = Path(args.home).resolve()
    out = Path(args.out).resolve()
    home.mkdir(parents=True, exist_ok=True)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    requested = args.views.split(",") if args.views else None
    entries = read_entries(root, args.only)
    run_quidra = Path(args.run).resolve() if args.run and not args.cli else None
    all_entries = entries
    if requested:
        entries = [e for e in entries if view_order(e["kind"], None, "", requested)]
    default_timeout, overrides = timeouts()

    if args.cli:
        entries = [e for e in entries if e["kind"] == "file"]
        cli = Path(args.cli).resolve()
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = [
                pool.submit(run_cli, cli, home, out, entry,
                            [v for v in ("ir", "llvm") if not requested or v in requested],
                            overrides.get(entry["name"], default_timeout), args.perturb)
                for entry in entries
            ]
            rows = [row for future in futures for row in future.result()]
    elif args.batch:
        rows = run_batch(tool, home, out, entries, args.batch, requested, args.perturb)
    else:
        schedule = list(reversed(entries)) if args.reverse else entries
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = [
                pool.submit(run_entry, tool, home, out, entry,
                            view_order(entry["kind"], args.permute_views, entry["name"], requested),
                            overrides.get(entry["name"], default_timeout), args.perturb)
                for entry in schedule
            ]
            rows = [row for future in futures for row in future.result()]
    rows.extend(run_rows(run_quidra, home, out, all_entries, requested, args.jobs))
    rows.sort()
    (out / "index.tsv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    failed = [r for r in rows if r.split("\t")[4] in ("timeout", "missing") or
              r.split("\t")[4].startswith(("signal:", "exit:"))]
    print(f"capture.py: {len(entries)} entries, {len(rows)} views -> {out / 'index.tsv'}"
          + (f"; {len(failed)} views without output (timeout/crash)" if failed else ""))


if __name__ == "__main__":
    main()
