#!/usr/bin/env python3
"""Reject native packages that depend on Quidra Core implementation details."""

from __future__ import annotations

from pathlib import Path
import re
import sys

SOURCE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
    ".cu", ".mm", ".s", ".S"
}
PUBLIC_QUIDRA_HEADERS = {"quidra/native_extension.h"}
PRIVATE_IDENTIFIERS = ("TensorValue", "TensorStorage")
PRIVATE_SYMBOL_PREFIXES = (
    "quidra_tensor_",
    "quidra_autograd_",
    "quidra_device_",
    "quidra_gpu_",
)
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
NATIVE_SOURCE_RE = re.compile(
    r"^native\.source\.[A-Za-z0-9_-]+(?:\.[A-Za-z0-9_-]+)?\s*=\s*(.+?)\s*$"
)


def unquote(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] == '"':
        return value[1:-1]
    return value


def declared_sources(root: Path) -> list[Path]:
    manifest = root / "quidra.package"
    if not manifest.is_file():
        raise ValueError(f"{root}: missing quidra.package")
    result: list[Path] = []
    for raw in manifest.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = NATIVE_SOURCE_RE.match(line)
        if not match:
            continue
        relative = Path(unquote(match.group(1)))
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"{root}: unsafe native source path {relative}")
        source = (root / relative).resolve()
        try:
            source.relative_to(root.resolve())
        except ValueError as error:
            raise ValueError(
                f"{root}: native source escapes package root: {relative}"
            ) from error
        if not source.is_file():
            raise ValueError(f"{root}: declared native source is missing: {relative}")
        result.append(source)
    return result


def scan_files(root: Path) -> list[Path]:
    files = set(declared_sources(root))
    native = root / "native"
    if native.is_dir():
        files.update(
            path.resolve()
            for path in native.rglob("*")
            if path.is_file() and path.suffix in SOURCE_SUFFIXES
        )
    return sorted(files)


def scan(root: Path) -> list[str]:
    failures: list[str] = []
    for path in scan_files(root):
        text = path.read_text(encoding="utf-8", errors="strict")
        relative = path.relative_to(root.resolve())
        for identifier in PRIVATE_IDENTIFIERS:
            if re.search(rf"\b{re.escape(identifier)}\b", text):
                failures.append(
                    f"{root.name}/{relative}: Core private type {identifier!r}"
                )
        for prefix in PRIVATE_SYMBOL_PREFIXES:
            if prefix in text:
                failures.append(
                    f"{root.name}/{relative}: Core private symbol prefix {prefix!r}"
                )
        for include in INCLUDE_RE.findall(text):
            normalized = include.replace("\\", "/")
            if (
                normalized.startswith("quidra/")
                and normalized not in PUBLIC_QUIDRA_HEADERS
            ):
                failures.append(
                    f"{root.name}/{relative}: non-ABI Quidra header {include!r}"
                )
            if "quidra/src/" in normalized:
                failures.append(
                    f"{root.name}/{relative}: Core implementation include {include!r}"
                )
    return failures


def main() -> int:
    if len(sys.argv) < 2:
        print(
            "usage: package_native_boundary_tests.py PACKAGE_ROOT [...]",
            file=sys.stderr,
        )
        return 2

    failures: list[str] = []
    for argument in sys.argv[1:]:
        root = Path(argument).resolve()
        if not root.is_dir():
            failures.append(f"package root does not exist: {argument}")
            continue
        try:
            failures.extend(scan(root))
        except (OSError, UnicodeError, ValueError) as error:
            failures.append(str(error))

    if failures:
        print("package/native Core ABI boundary failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print("package/native Core ABI boundary: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
