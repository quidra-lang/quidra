#!/usr/bin/env python3
"""Check documented Quidra examples and machine-facing tooling contracts."""

from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

QUIDRA = Path(sys.argv[1]).resolve()
ROOT = Path(sys.argv[2]).resolve()
DOCS = [
    ROOT / "README.md",
    ROOT / "docs/spec/language.md",
    ROOT / "docs/spec/llm-guide.md",
]

POINT = """class Point
    float x
    float y

"""
CONFIG = """class Config
    int retries = 3
    float timeout = 5.0
    string endpoint

"""
GENERIC = """class Box<T>
    T value

T first<T>(T[] values)
    return values[0]

class Convert
    T identity<T>(T value)
        return value

"""
LOOKUP = """int | none | error lookup(int id)
    if id < 0
        return error("invalid id")
    if id == 0
        return none
    return id

"""
PARENTS = """class Parent
    int value

class Child : Parent
    int extra

"""


def prelude(path: Path, code: str) -> str:
    rel = path.relative_to(ROOT).as_posix()
    if rel == "README.md":
        if code.lstrip().startswith("int | none | error doubled("):
            return LOOKUP
        if code.strip() == "Parent | Child value":
            return PARENTS
    if rel == "docs/spec/language.md":
        stripped = code.lstrip()
        if stripped.startswith("Box<int> box ="):
            return GENERIC
        if stripped.startswith("Point complete ="):
            return POINT + CONFIG
        if stripped.startswith("Point a =") or "Point point = Point(" in code:
            return POINT
    return ""


def fixtures(directory: Path) -> None:
    (directory / "geometry.qui").write_text(
        "class Point\n    int x\n    int y\n", encoding="utf-8"
    )
    (directory / "local.qui").write_text(
        "int local_value()\n    return 1\n", encoding="utf-8"
    )
    (directory / "shared.qui").write_text(
        "int shared_value()\n    return 1\n", encoding="utf-8"
    )
    shared = directory / "shared"
    shared.mkdir()
    (shared / "root.qui").write_text(
        "int root_value()\n    return 1\n", encoding="utf-8"
    )


def run_tool(*args: str, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(QUIDRA), *args],
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def verify_machine_tooling(tmp: Path, failures: list[str]) -> None:
    grammar = run_tool("describe", "grammar", cwd=tmp)
    expected_grammar = (ROOT / "docs/spec/grammar.ebnf").read_text(encoding="utf-8")
    if grammar.returncode != 0 or grammar.stdout.rstrip("\n") != expected_grammar.rstrip("\n"):
        failures.append(
            "describe grammar must emit docs/spec/grammar.ebnf exactly; "
            f"exit code {grammar.returncode}\n{grammar.stdout}{grammar.stderr}"
        )

    patch_schema = run_tool("describe", "patch-schema", cwd=tmp)
    try:
        schema = json.loads(patch_schema.stdout)
        assert patch_schema.returncode == 0
        assert schema["$schema"] == "https://json-schema.org/draft/2020-12/schema"
        assert schema["x-quidra-supported-versions"] == [1, 2]
        assert schema["x-quidra-preferred-version"] == 2
    except (AssertionError, KeyError, json.JSONDecodeError) as exc:
        failures.append(
            "describe patch-schema must emit the supported machine-readable patch schema; "
            f"{exc}\n{patch_schema.stdout}{patch_schema.stderr}"
        )

    llm = run_tool("describe", "llm", cwd=tmp)
    try:
        interface = json.loads(llm.stdout)
        assert llm.returncode == 0
        assert interface["schema_version"] == 1
        assert interface["language"] == "Quidra"
        assert interface["generation"]["grammar_command"] == "quidra describe grammar"
        assert interface["repair"]["schema_command"] == "quidra describe patch-schema"
        assert interface["repair"]["preferred_schema_version"] == 2
        assert interface["repair"]["apply_command"] == "quidra patch FILE.qui PATCH.json --write"
        assert interface["validation_sequence"] == [
            "quidra fmt FILE.qui --check",
            "quidra check FILE.qui --json",
        ]
    except (AssertionError, KeyError, json.JSONDecodeError) as exc:
        failures.append(
            "describe llm must emit the compact machine workflow contract; "
            f"{exc}\n{llm.stdout}{llm.stderr}"
        )

    source = tmp / "patch-v2.qui"
    source.write_text(
        'int answer = 40\nprint("drop")\nprint(answer)\n', encoding="utf-8"
    )
    inspection_result = run_tool("inspect", str(source), cwd=tmp)
    try:
        inspection = json.loads(inspection_result.stdout)
        integer = next(
            node
            for node in inspection["nodes"]
            if node["kind"] == "integer" and node.get("source") == "40"
        )
        statements = [
            node
            for node in inspection["nodes"]
            if node["kind"] == "expression_statement" and node.get("source", "").startswith("print(")
        ]
        drop_statement, answer_statement = statements
    except (json.JSONDecodeError, KeyError, StopIteration, ValueError) as exc:
        failures.append(f"cannot prepare patch v2 fixture: {exc}\n{inspection_result.stdout}{inspection_result.stderr}")
        return

    patch = {
        "schema_version": 2,
        "base_revision": inspection["revision"],
        "operations": [
            {
                "op": "replace_node",
                "node_id": integer["node_id"],
                "expected_hash": integer["source_hash"],
                "expected_kind": integer["kind"],
                "replacement": "42",
            },
            {
                "op": "delete_node",
                "node_id": drop_statement["node_id"],
                "expected_hash": drop_statement["source_hash"],
                "expected_kind": drop_statement["kind"],
            },
            {
                "op": "insert_before",
                "node_id": answer_statement["node_id"],
                "expected_hash": answer_statement["source_hash"],
                "expected_kind": answer_statement["kind"],
                "replacement": 'print("inserted")\n',
            },
        ],
    }
    patch_file = tmp / "patch-v2.json"
    patch_file.write_text(json.dumps(patch), encoding="utf-8")
    applied = run_tool("patch", str(source), str(patch_file), "--write", cwd=tmp)
    executed = run_tool("run", str(source), cwd=tmp) if applied.returncode == 0 else None
    if (
        applied.returncode != 0
        or executed is None
        or executed.returncode != 0
        or executed.stdout.rstrip("\n") != "inserted\n42"
    ):
        details = applied.stdout + applied.stderr
        if executed is not None:
            details += executed.stdout + executed.stderr
        failures.append(f"patch schema v2 structural operations failed\n{details}")

    kind_source = tmp / "patch-kind.qui"
    kind_source.write_text("int value = 1\nprint(value)\n", encoding="utf-8")
    kind_inspection_result = run_tool("inspect", str(kind_source), cwd=tmp)
    try:
        kind_inspection = json.loads(kind_inspection_result.stdout)
        kind_integer = next(node for node in kind_inspection["nodes"] if node["kind"] == "integer")
    except (json.JSONDecodeError, KeyError, StopIteration) as exc:
        failures.append(f"cannot prepare patch kind fixture: {exc}")
        return
    bad_patch = {
        "schema_version": 2,
        "base_revision": kind_inspection["revision"],
        "operations": [
            {
                "op": "replace_node",
                "node_id": kind_integer["node_id"],
                "expected_hash": kind_integer["source_hash"],
                "expected_kind": "string",
                "replacement": "2",
            }
        ],
    }
    bad_patch_file = tmp / "patch-kind.json"
    bad_patch_file.write_text(json.dumps(bad_patch), encoding="utf-8")
    rejected = run_tool("patch", str(kind_source), str(bad_patch_file), cwd=tmp)
    if rejected.returncode == 0 or "PATCH_KIND_MISMATCH" not in rejected.stderr:
        failures.append(
            "patch schema v2 must reject stale structural kinds with PATCH_KIND_MISMATCH\n"
            + rejected.stdout
            + rejected.stderr
        )


def main() -> int:
    total = 0
    failures: list[str] = []
    fence = re.compile(r"\x60\x60\x60quidra\n(.*?)\x60\x60\x60", re.S)

    with tempfile.TemporaryDirectory(prefix="quidra-docs-") as raw_tmp:
        tmp = Path(raw_tmp)
        fixtures(tmp)

        for document in DOCS:
            text = document.read_text(encoding="utf-8")
            for index, code in enumerate(fence.findall(text)):
                total += 1
                source = tmp / f"{document.stem}-{index}.qui"
                source.write_text(prelude(document, code) + code, encoding="utf-8")
                result = subprocess.run(
                    [str(QUIDRA), "check", str(source)],
                    cwd=tmp,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                )
                expected_failure = "// compile-time error" in code
                passed = result.returncode != 0 if expected_failure else result.returncode == 0
                if not passed:
                    mode = "expected rejection" if expected_failure else "expected acceptance"
                    details = (result.stdout + result.stderr).strip()
                    failures.append(
                        f"{document.relative_to(ROOT)} fence #{index}: {mode}; "
                        f"exit code {result.returncode}\n{details}"
                    )
                    continue

                expected_output = re.findall(r"// output:\s?(.*)$", code, re.M)
                if expected_output and not expected_failure:
                    native = subprocess.run(
                        [str(QUIDRA), "run", str(source)],
                        cwd=tmp,
                        text=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        check=False,
                        timeout=15,
                    )
                    actual = native.stdout.rstrip("\n")
                    expected = "\n".join(expected_output)
                    if native.returncode != 0 or actual != expected:
                        details = (native.stdout + native.stderr).strip()
                        failures.append(
                            f"{document.relative_to(ROOT)} fence #{index}: "
                            f"native output mismatch; exit code {native.returncode}; "
                            f"expected {expected!r}, got {actual!r}\n{details}"
                        )

        verify_machine_tooling(tmp, failures)

    if failures:
        print("\n\n".join(failures), file=sys.stderr)
        print(
            f"documentation/tooling verification: {len(failures)} failure(s) across {total} fences",
            file=sys.stderr,
        )
        return 1

    print(f"documentation/tooling verification: {total} fences and machine contracts verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())