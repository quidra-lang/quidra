#!/usr/bin/env python3
"""Corpus A of the golden equivalence harness (README.md, Inputs).

Corpus A is static and pinned. tests/golden/corpus.toml records every entry with
its expected status and the sha256 of its source, the package commits, and the
external sources; a status change is a mismatch even when the text is equal.

Commands:
  materialize  write the corpus under ROOT (absolute paths reach the compiler
               output, so base and head captures must use the same ROOT) and
               the entry list ROOT/entries.tsv; entries must match their pins
               unless --unpinned is given
  pin          rewrite corpus.toml from a capture index (statuses, hashes,
               package commits, external hashes); the [[timeout]] and [[run]]
               tables are kept
  census       aggregate the `census` views of a capture per component;
               --matrix ALT prints the type-signature matrix of one alternative,
               --sites KEY the entries and sites a key lists

Sources outside the repository (package clones, external programs) come from a
sources file, one `package NAME PATH` or `external NAME PATH` per line; see
tests/ir_golden.sh for the default location.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO / "tests"))
import doc_blocks  # noqa: E402
import toml_subset  # noqa: E402

CORPUS_TOML = HERE / "corpus.toml"
DEFAULT_TIMEOUT = 300

# Documents whose fenced blocks join the corpus: the ones the documentation test
# checks, plus the package guide.
DOC_DOCUMENTS = [*doc_blocks.DOCUMENTS, "docs/packages.md"]

# The view whose status is an entry's expected status, per entry kind.
PRIMARY_VIEW = {
    "file": "ir",
    "memory": "compile.memory",
    "fixture": "fixture",
    "repl-session": "repl.session",
}

# Component of an entry, from its name.
COMPONENTS = [
    ("examples/", "core-examples"),
    ("benchmarks/", "core-benchmarks"),
    ("heredoc/", "test-heredocs"),
    ("compiler_tests/", "compiler-tests"),
    ("fixtures/optimizer/", "optimizer-fixtures"),
    ("fixtures/", "backend-fixtures"),
    ("docs/", "docs"),
    ("store/", "packages"),
    ("package-heredoc/", "package-heredocs"),
    ("probes/packages/", "probe-packages"),
    ("probes/", "probes"),
    ("external/", "external"),
    ("generated/", "generated"),
]


def component_of(name: str) -> str:
    for prefix, component in COMPONENTS:
        if name.startswith(prefix):
            return component
    return "other"


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fail(message: str) -> None:
    print(f"corpus.py: {message}", file=sys.stderr)
    raise SystemExit(1)


# ---------------------------------------------------------------------------
# Sources outside the repository


def read_sources(path: Path | None) -> dict[tuple[str, str], Path]:
    sources: dict[tuple[str, str], Path] = {}
    if path is None or not path.exists():
        return sources
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 2)
        if len(parts) != 3 or parts[0] not in ("package", "external"):
            fail(f"{path}:{number}: expected `package NAME PATH` or `external NAME PATH`")
        sources[(parts[0], parts[1])] = Path(parts[2]).expanduser()
    return sources


def load_pins() -> dict:
    if not CORPUS_TOML.exists():
        return {}
    return toml_subset.load(CORPUS_TOML)


# ---------------------------------------------------------------------------
# Heredocs of test scripts

HEREDOC = re.compile(
    r"""^\s*cat\s*(>>?)\s*"?\$\{?([A-Za-z_][A-Za-z0-9_]*)\}?/([^"\s$`]+)"?\s*<<(-?)\s*(['"])([A-Za-z_][A-Za-z0-9_]*)\5\s*$"""
)


def heredoc_writes(script: Path):
    """(variable, relative path, append, text) of every quoted heredoc that
    writes below a variable directory, in script order. Unquoted heredocs
    expand variables at run time; corpus B (observe) covers them."""
    lines = script.read_text(encoding="utf-8", errors="replace").split("\n")
    index = 0
    while index < len(lines):
        match = HEREDOC.match(lines[index])
        index += 1
        if not match:
            continue
        append, variable, relative, strip_tabs, _, tag = match.groups()
        body = []
        while index < len(lines):
            line = lines[index]
            index += 1
            if strip_tabs:
                line = line.lstrip("\t")
            if line == tag:
                break
            body.append(line)
        relative_path = Path(relative)
        if relative_path.is_absolute() or ".." in relative_path.parts:
            continue
        if variable != "TMP":
            relative_path = Path(f"_{variable}") / relative_path
        yield relative_path, append == ">>", "\n".join(body) + "\n"


# Heredoc files a test script feeds to `quidra repl`.
REPL_INPUT = re.compile(r"(^|/)(repl[^/]*\.txt|repl[^/]*/input\.txt)$")


def materialize_script(script: Path, destination: Path, prefix: str) -> list[tuple[str, str]]:
    """Replays a script's quoted heredocs into destination. A rewrite of a file
    with different content starts a new generation `<dir>~<n>` holding the
    whole state from then on, so every version of every file is compiled with
    the siblings it had. Returns the corpus names and kinds of the `.qui`
    files (file) and REPL inputs (repl-session) each generation wrote."""
    state: dict[Path, str] = {}
    generations: list[tuple[dict[Path, str], set[Path]]] = []
    written: set[Path] = set()
    for relative, append, text in heredoc_writes(script):
        content = state.get(relative, "") + text if append else text
        if relative in written and state.get(relative) != content and not append:
            generations.append((dict(state), written))
            written = set()
        state[relative] = content
        written.add(relative)
    generations.append((dict(state), written))
    if not any(written for _, written in generations):
        return []

    names: list[tuple[str, str]] = []
    for number, (files, written_here) in enumerate(generations, 1):
        directory = destination if number == 1 else destination.with_name(
            f"{destination.name}~{number}")
        for relative, content in files.items():
            target = directory / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding="utf-8")
        for relative in sorted(written_here):
            name = f"{prefix}{directory.name}/{relative.as_posix()}"
            if relative.suffix == ".qui":
                names.append((name, "file"))
            elif REPL_INPUT.search(relative.as_posix()):
                names.append((name, "repl-session"))
    return names


# ---------------------------------------------------------------------------
# Generated inputs


def generated_programs() -> dict[str, str]:
    def deep_and(terms: int) -> str:
        return "bool value = " + " and ".join(["true"] * terms) + "\nprint(value)\n"

    def nested_ifs(depth: int) -> str:
        lines = ["int value = 0"]
        for level in range(depth):
            lines.append("    " * level + "if value >= 0")
        lines.append("    " * depth + "value = value + 1")
        lines.append("print(value)")
        return "\n".join(lines) + "\n"

    def flat(statements: int) -> str:
        lines = ["int total = 0"]
        lines.extend(f"total = total + {i % 7}" for i in range(statements - 2))
        lines.append("print(total)")
        return "\n".join(lines) + "\n"

    def method_chain(calls: int) -> str:
        return (
            "class Builder\n"
            "    int total\n\n"
            "    construct(int start)\n"
            "        this.total = start\n\n"
            "    Builder add(int amount)\n"
            "        return Builder(this.total + amount)\n\n"
            "Builder result = Builder(0)" + "".join(f".add({i % 5})" for i in range(calls)) +
            "\nprint(result.total)\n"
        )

    return {
        "deep_and_60.qui": deep_and(60),
        "deep_and_70.qui": deep_and(70),
        "nested_if_200.qui": nested_ifs(200),
        "nested_if_300.qui": nested_ifs(300),
        "flat_10000.qui": flat(10000),
        "flat_40000.qui": flat(40000),
        "method_chain_60.qui": method_chain(60),
        "method_chain_600.qui": method_chain(600),
    }


# ---------------------------------------------------------------------------
# materialize


class Corpus:
    def __init__(self, root: Path):
        self.root = root
        self.entries: list[tuple[str, str]] = []  # (name, kind)
        self.environments: dict[str, list[Path]] = {}

    def add(self, name: str, kind: str = "file") -> None:
        self.entries.append((name, kind))

    def copy(self, source: Path, name: str) -> None:
        target = self.root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)

    def package_path(self, name: str) -> str:
        """QUIDRA_PACKAGE_PATH of an entry: its own scenario's packages first,
        then the pinned store; documentation blocks see only the docs' fake
        packages, as in tests/documentation_examples.py."""
        store = self.root / "store"
        top = name.split("/")
        if top[0] == "docs":
            return str(self.root / "docs" / "packages")
        if top[0] in ("heredoc", "package-heredoc"):
            depth = 2 if top[0] == "heredoc" else 3
            scenario = self.root.joinpath(*top[:depth])
            local = scenario / "packages"
            if local.is_dir():
                return os.pathsep.join([str(local), str(store)])
        if top[0] == "probes":
            local = self.root / "probes" / "packages"
            if local.is_dir():
                return os.pathsep.join([str(local), str(store)])
        return str(store)

    def write_entries(self) -> None:
        lines = []
        for name, kind in sorted(self.entries):
            path = self.root / name
            cwd = self.root if kind == "fixture" else path.parent
            lines.append("\t".join([name, kind, str(path), str(cwd), self.package_path(name)]))
        (self.root / "entries.tsv").write_text("\n".join(lines) + "\n", encoding="utf-8")


def extract_package(clone: Path, commit: str, destination: Path) -> None:
    archive = subprocess.run(
        ["git", "-C", str(clone), "archive", "--format=tar", commit],
        check=True, stdout=subprocess.PIPE).stdout
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        if hasattr(tarfile, "data_filter"):
            tar.extractall(destination, filter="data")
        else:
            tar.extractall(destination)


def resolve_commit(clone: Path, revision: str) -> str:
    return subprocess.run(
        ["git", "-C", str(clone), "rev-parse", "--verify", revision + "^{commit}"],
        check=True, stdout=subprocess.PIPE, text=True).stdout.strip()


def materialize(args) -> None:
    root = Path(args.root).resolve()
    sources = read_sources(Path(args.sources) if args.sources else None)
    pins = load_pins()
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    corpus = Corpus(root)
    missing: list[str] = []

    # Core examples and benchmark programs.
    for source in sorted((REPO / "examples").rglob("*.qui")):
        name = "examples/" + source.relative_to(REPO / "examples").as_posix()
        corpus.copy(source, name)
        corpus.add(name)
    for group in ("micro", "adversarial"):
        for source in sorted((REPO / "tests/benchmark/quidra" / group).glob("*.qui")):
            name = f"benchmarks/{group}/{source.name}"
            corpus.copy(source, name)
            corpus.add(name)

    # Quoted heredocs of the Core test scripts.
    scripts = sorted((REPO / "tests").glob("*.sh")) + [REPO / "scripts/native-smoke.sh"]
    for script in scripts:
        for name, kind in materialize_script(script, root / "heredoc" / script.stem, "heredoc/"):
            corpus.add(name, kind)

    # Documentation blocks with their preludes and fixtures.
    docs = root / "docs"
    docs.mkdir()
    doc_blocks.fixtures(docs)
    for document in DOC_DOCUMENTS:
        path = REPO / document
        for index, code in enumerate(doc_blocks.blocks(path.read_text(encoding="utf-8"))):
            name = f"docs/{path.stem}-{index}.qui"
            (root / name).write_text(doc_blocks.prelude(REPO, path, code) + code, encoding="utf-8")
            corpus.add(name)

    # Pinned package store and the packages' own programs and heredocs.
    package_pins = {p["name"]: p for p in pins.get("package", [])}
    package_names = sorted(set(package_pins) | {n for (k, n) in sources if k == "package"})
    for package in package_names:
        clone = sources.get(("package", package))
        if clone is None:
            missing.append(f"package {package}")
            continue
        commit = package_pins.get(package, {}).get("commit") or resolve_commit(clone, "HEAD")
        if args.unpinned and package not in package_pins:
            commit = resolve_commit(clone, "HEAD")
        extract_package(clone, commit, root / "store" / package)
        store = root / "store" / package
        programs = [store / "main.qui"] if (store / "main.qui").exists() else []
        for directory in ("tests", "examples"):
            programs.extend(sorted((store / directory).rglob("*.qui")))
        for program in programs:
            corpus.add(program.relative_to(root).as_posix())
        package_scripts = sorted((store / "tests").glob("*.sh")) + sorted(
            (store / "scripts").glob("*.sh"))
        for script in package_scripts:
            destination = root / "package-heredoc" / package / script.stem
            for name, kind in materialize_script(script, destination,
                                                 f"package-heredoc/{package}/"):
                corpus.add(name, kind)

    # Probes (and their packages and REPL sessions).
    probes = HERE / "probes"
    if probes.is_dir():
        shutil.copytree(probes, root / "probes")
        for source in sorted((root / "probes").rglob("*.qui")):
            relative = source.relative_to(root).as_posix()
            if relative.startswith("probes/packages/"):
                continue
            corpus.add(relative)
        for session in sorted((root / "probes").rglob("*.session")):
            corpus.add(session.relative_to(root).as_posix(), "repl-session")

    # Programs outside the repository (`external NAME PATH` in the sources file).
    external_pins = {e["name"]: e for e in pins.get("external", [])}
    external_names = sorted(set(external_pins) | {n for (k, n) in sources if k == "external"})
    for external in external_names:
        path = sources.get(("external", external))
        if path is None:
            missing.append(f"external {external}")
            continue
        name = f"external/{external}.qui"
        corpus.copy(path, name)
        corpus.add(name)

    # The inline programs of tests/compiler_tests.cpp, written by its capture
    # hook; compiled through quidra::compile (view compile.memory).
    if args.compiler_tests:
        directory = root / "compiler_tests"
        environment = dict(os.environ, QUIDRA_CAPTURE_SOURCES=str(directory))
        result = subprocess.run([args.compiler_tests], env=environment,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode != 0:
            fail(f"{args.compiler_tests} failed while capturing sources:\n{result.stdout[-2000:]}")
        for source in sorted(directory.glob("*.qui")):
            corpus.add(f"compiler_tests/{source.name}", "memory")

    # Hand-built modules the golden tool knows (--fixture).
    if args.tool:
        listing = subprocess.run([args.tool, "--list-fixtures"], check=True,
                                 stdout=subprocess.PIPE, text=True).stdout
        for fixture in listing.split():
            corpus.add(f"fixtures/{fixture}", "fixture")

    # Generated inputs.
    for file_name, text in generated_programs().items():
        name = f"generated/{file_name}"
        (root / "generated").mkdir(exist_ok=True)
        (root / name).write_text(text, encoding="utf-8")
        corpus.add(name)

    corpus.write_entries()
    check_pins(corpus, pins, missing, args)
    print(f"corpus.py: {len(corpus.entries)} entries under {root}"
          + (f"; missing sources: {', '.join(missing)}" if missing else ""))


def entry_hash(root: Path, name: str, kind: str) -> str:
    path = root / name
    if kind == "fixture" or not path.exists():
        return "-"
    return sha256_file(path)


# Keys of a [[run]] table and their types (capture.py, run_view).
RUN_KEYS = {"entry": str, "args": list, "stdin": list, "timeout": int}


def run_problems(pins: dict, present: dict[str, str], skipped: set[str]) -> list[str]:
    """Every [[run]] table names a file entry of the corpus once, with known
    keys of the right types."""
    problems = []
    seen = set()
    for spec in pins.get("run", []):
        name = spec.get("entry")
        unknown = sorted(set(spec) - set(RUN_KEYS))
        wrong = sorted(key for key, kind in RUN_KEYS.items()
                       if key in spec and not isinstance(spec[key], kind))
        if not isinstance(name, str) or unknown or wrong:
            problems.append(f"malformed [[run]] table {spec}")
            continue
        if name in seen:
            problems.append(f"[[run]] {name} listed twice")
        seen.add(name)
        if present.get(name) == "file":
            continue
        if name not in present and any(name.startswith(prefix) for prefix in skipped):
            continue
        problems.append(f"[[run]] {name} is not a file entry of the corpus")
    return problems


def check_pins(corpus: Corpus, pins: dict, missing: list[str], args) -> None:
    if missing and not args.allow_missing:
        fail("missing sources (pass --allow-missing to skip their entries, or list them in the "
             "sources file): " + ", ".join(missing))
    if args.unpinned:
        return
    if not pins:
        fail(f"{CORPUS_TOML} is missing; bootstrap with --unpinned, capture, then `pin`")
    pinned = {e["name"]: e for e in pins.get("entry", [])}
    present = {name: kind for name, kind in corpus.entries}
    problems = []
    for name, kind in sorted(present.items()):
        pin = pinned.get(name)
        if pin is None:
            problems.append(f"unpinned entry {name}")
        elif pin.get("sha256", "-") != entry_hash(corpus.root, name, kind):
            problems.append(f"changed source {name}")
    skipped = set()
    for item in missing:
        kind, label = item.split(" ", 1)
        skipped.add(f"store/{label}/" if kind == "package" else f"external/{label}.qui")
        skipped.add(f"package-heredoc/{label}/")
    for name in sorted(pinned):
        if name in present:
            continue
        if any(name.startswith(prefix) for prefix in skipped):
            continue
        problems.append(f"missing entry {name}")
    for external in pins.get("external", []):
        path = corpus.root / f"external/{external['name']}.qui"
        if path.exists() and sha256_file(path) != external["sha256"]:
            problems.append(f"external {external['name']} differs from its pin")
    problems.extend(run_problems(pins, present, skipped))
    if problems:
        shown = "\n  ".join(problems[:50])
        fail(f"corpus drift against {CORPUS_TOML.name} ({len(problems)}):\n  {shown}\n"
             "Re-pin with `corpus.py pin` when the change is intended.")


# ---------------------------------------------------------------------------
# pin


def read_index(path: Path) -> dict[tuple[str, str], tuple[str, str, str]]:
    index = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line:
            continue
        entry, view, digest, size, status = line.split("\t")
        index[(entry, view)] = (digest, size, status)
    return index


def read_entries(root: Path) -> list[list[str]]:
    rows = []
    for line in (root / "entries.tsv").read_text(encoding="utf-8").splitlines():
        if line:
            rows.append(line.split("\t"))
    return rows


def pin(args) -> None:
    root = Path(args.root).resolve()
    sources = read_sources(Path(args.sources) if args.sources else None)
    index = read_index(Path(args.index))
    old = load_pins()
    timeouts = {t["entry"]: t["seconds"] for t in old.get("timeout", [])}
    out = [
        "# Golden corpus A (tests/golden/README.md, Inputs).",
        "# Written by `tests/golden/corpus.py pin` from a capture of the pinned",
        "# baseline; re-pin it with the same command whenever an entry is added or",
        "# removed, or its source or expected status changes. Each entry records",
        "# the expected status of its primary view (ok or error:CODE) and the",
        "# sha256 of its source. Package commits and external programs are pinned",
        "# the same way.",
        "",
        "[corpus]",
        "schema = 1",
        f"timeout = {DEFAULT_TIMEOUT}",
        "",
    ]
    package_names = sorted({n for (k, n) in sources if k == "package"} |
                           {p["name"] for p in old.get("package", [])})
    old_packages = {p["name"]: p for p in old.get("package", [])}
    for package in package_names:
        clone = sources.get(("package", package))
        commit = (resolve_commit(clone, old_packages[package]["commit"])
                  if package in old_packages and clone and not args.repin_packages
                  else resolve_commit(clone, "HEAD") if clone
                  else old_packages[package]["commit"])
        repository = old_packages.get(package, {}).get(
            "repository", f"https://github.com/quidra-lang/{package}")
        out += ["[[package]]", f'name = "{package}"', f'repository = "{repository}"',
                f'commit = "{commit}"', ""]
    for external in sorted({n for (k, n) in sources if k == "external"} |
                           {e["name"] for e in old.get("external", [])}):
        path = root / f"external/{external}.qui"
        digest = sha256_file(path) if path.exists() else {
            e["name"]: e for e in old.get("external", [])}[external]["sha256"]
        out += ["[[external]]", f'name = "{external}"', f'sha256 = "{digest}"', ""]
    for entry, seconds in sorted(timeouts.items()):
        out += ["[[timeout]]", f'entry = "{entry}"', f"seconds = {seconds}", ""]
    for spec in sorted(old.get("run", []), key=lambda spec: spec["entry"]):
        out += ["[[run]]", f'entry = "{spec["entry"]}"']
        for key in ("args", "stdin"):
            if key in spec:
                out.append(f"{key} = [" + ", ".join(f'"{item}"' for item in spec[key]) + "]")
        if "timeout" in spec:
            out.append(f"timeout = {spec['timeout']}")
        out.append("")
    helper_mismatches = []
    for name, kind, *_ in read_entries(root):
        status = index.get((name, PRIMARY_VIEW[kind]), ("", "", "missing"))[2]
        if name.startswith("compiler_tests/"):
            helper = name.split("/", 1)[1].rsplit("-", 1)[0]
            expects_error = helper in ("bad", "bad_code", "bad_message")
            if expects_error != status.startswith("error:"):
                helper_mismatches.append(f"{name}: {helper} but {status}")
        if '"' in name or "\\" in name:
            fail(f"entry name not representable in the TOML subset: {name}")
        out += ["[[entry]]", f'name = "{name}"', f'status = "{status}"',
                f'sha256 = "{entry_hash(root, name, kind)}"', ""]
    CORPUS_TOML.write_text("\n".join(out), encoding="utf-8")
    print(f"corpus.py: pinned {len(read_entries(root))} entries in {CORPUS_TOML}")
    for mismatch in helper_mismatches:
        print(f"corpus.py: compiler test status does not match its helper: {mismatch}")


# ---------------------------------------------------------------------------
# census


def census(args) -> None:
    captures = Path(args.captures)
    rows = read_entries(Path(args.root).resolve()) if args.root else None
    names = [r[0] for r in rows] if rows else sorted(
        str(p.parent.relative_to(captures)) for p in captures.rglob("census"))
    alternatives: dict[str, dict[str, list[int]]] = defaultdict(lambda: defaultdict(lambda: [0, 0]))
    signatures: dict[tuple[str, str], list[int]] = defaultdict(lambda: [0, 0])
    events: dict[tuple[str, str, str], int] = defaultdict(int)
    counts: dict[str, dict[str, list[int]]] = defaultdict(lambda: defaultdict(lambda: [0, 0]))
    keys: dict[str, dict[str, int]] = defaultdict(lambda: defaultdict(int))
    kinds: dict[str, dict[str, int]] = defaultdict(lambda: defaultdict(int))
    sites: list[tuple[str, str]] = []
    order: list[str] = []
    for name, view in ((n, v) for n in names for v in ("census", "census.repl")):
        path = captures / name / view
        if not path.exists():
            continue
        component = component_of(name)
        for line in path.read_text(encoding="utf-8").splitlines():
            parts = line.split(" ")
            if parts[0] == "alt":
                label = parts[1]
                if label not in order:
                    order.append(label)
                counts = alternatives[label][component]
                counts[0] += int(parts[2])
                counts[1] += int(parts[3])
            elif parts[0] == "sig":
                key = (parts[1], " ".join(parts[2:-2]))
                signatures[key][0] += int(parts[-2])
                signatures[key][1] += int(parts[-1])
            elif parts[0] == "event":
                events[(component, parts[1], " ".join(parts[2:-1]))] += int(parts[-1])
            elif parts[0] == "count" and len(parts) == 4:
                counts[parts[1]][component][0] += int(parts[2])
                counts[parts[1]][component][1] += int(parts[3])
            elif parts[0] == "key" and len(parts) == 3:
                keys[parts[1]][component] += int(parts[2])
            elif parts[0] == "kind" and len(parts) == 3:
                kinds[parts[1]][component] += int(parts[2])
            elif parts[0] == "site" and len(parts) >= 3 and parts[1] == args.sites:
                sites.append((name, " ".join(parts[2:])))
    if args.sites:
        for entry, site in sites:
            print(f"{entry}\t{site}")
        print(f"corpus.py: {len(sites)} sites of {args.sites} in "
              f"{len({entry for entry, _ in sites})} entries")
        return
    if args.matrix:
        for (alternative, key), (lowered, optimized) in sorted(signatures.items()):
            if alternative == args.matrix:
                print(f"{key}\tlowered={lowered}\toptimized={optimized}")
        return
    components = sorted({c for counts in alternatives.values() for c in counts})
    missing_lowered = []
    missing_optimized = []
    print("alternative\t" + "\t".join(components) + "\ttotal(lowered/optimized)")
    for label in order:
        per = alternatives[label]
        lowered = sum(v[0] for v in per.values())
        optimized = sum(v[1] for v in per.values())
        cells = [f"{per[c][0]}/{per[c][1]}" if c in per else "0/0" for c in components]
        print(f"{label}\t" + "\t".join(cells) + f"\t{lowered}/{optimized}")
        if lowered == 0:
            missing_lowered.append(label)
        if optimized == 0:
            missing_optimized.append(label)
    print()
    for (component, kind, key), count in sorted(events.items()):
        print(f"event\t{component}\t{kind}\t{key}\t{count}")
    print()
    for title, table in (("count", counts), ("key", keys), ("kind", kinds)):
        names = sorted({c for per in table.values() for c in per})
        print(f"{title}\t" + "\t".join(names) + "\ttotal")
        for label in sorted(table):
            per = table[label]
            cells = [per.get(c) for c in names]
            if title == "count":
                text = [f"{v[0]}/{v[1]}" if v else "0/0" for v in cells]
                total = f"{sum(v[0] for v in per.values())}/{sum(v[1] for v in per.values())}"
            else:
                text = [str(v or 0) for v in cells]
                total = str(sum(per.values()))
            print(f"{label}\t" + "\t".join(text) + f"\t{total}")
        print()
    print(f"alternatives never lowered ({len(missing_lowered)}): " + " ".join(missing_lowered))
    print(f"alternatives never optimized ({len(missing_optimized)}): " +
          " ".join(missing_optimized))
    if args.require_all and (missing_lowered or missing_optimized):
        raise SystemExit(1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    m = commands.add_parser("materialize")
    m.add_argument("--root", required=True)
    m.add_argument("--sources")
    m.add_argument("--unpinned", action="store_true")
    m.add_argument("--allow-missing", action="store_true")
    m.add_argument("--tool", help="quidra_golden_dump, to list its fixtures")
    m.add_argument("--compiler-tests", help="quidra_tests, to capture its inline programs")
    p = commands.add_parser("pin")
    p.add_argument("--root", required=True)
    p.add_argument("--index", required=True)
    p.add_argument("--sources")
    p.add_argument("--repin-packages", action="store_true")
    c = commands.add_parser("census")
    c.add_argument("--captures", required=True)
    c.add_argument("--root")
    c.add_argument("--matrix")
    c.add_argument("--sites", metavar="KEY")
    c.add_argument("--require-all", action="store_true")
    args = parser.parse_args()
    {"materialize": materialize, "pin": pin, "census": census}[args.command](args)


if __name__ == "__main__":
    main()
