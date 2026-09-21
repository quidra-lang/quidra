#!/usr/bin/env python3
"""Prove that project.toml is the only place each project value is written down.

Two kinds of check live here:

  * derived files must match what scripts/sync_metadata.py would write, so a
    hand-edit of a generated file fails instead of silently drifting;
  * files that cannot be generated - C++ source, CMake, shell, prose - must
    still agree with project.toml.

The point is to stop a value being *defined* twice, not to ban the words
"Quidra" or ".qui" from appearing in prose.
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import toml_subset  # noqa: E402

FAILURES: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def check_generated_files_are_current() -> None:
    result = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "sync_metadata.py"), "--check"],
        capture_output=True,
        text=True,
    )
    check(
        result.returncode == 0,
        "generated files disagree with project.toml:\n"
        + (result.stderr or result.stdout).strip(),
    )


def check_build_dependencies(project: dict) -> None:
    deps = project["dependencies"]
    cmake_text = read(ROOT / "CMakeLists.txt")

    declared = re.search(
        r"cmake_minimum_required\(VERSION ([0-9.]+)\)", cmake_text
    )
    check(declared is not None, "CMakeLists.txt has no cmake_minimum_required")
    if declared:
        check(
            declared.group(1) == deps["cmake_minimum"],
            f"CMakeLists.txt requires CMake {declared.group(1)} but project.toml "
            f"declares {deps['cmake_minimum']}",
        )

    llvm = deps["llvm_minimum"]
    clang = deps["clang_minimum"]
    debian = re.search(r'CPACK_DEBIAN_PACKAGE_DEPENDS "([^"]*)"', cmake_text)
    check(debian is not None, "CMakeLists.txt has no CPACK_DEBIAN_PACKAGE_DEPENDS")
    if debian:
        for package in (f"clang-{clang}", f"llvm-{llvm}"):
            check(
                package in debian.group(1),
                f"the Debian dependency list does not require {package}, which "
                f"project.toml declares",
            )

    readme = read(ROOT / "README.md")
    for label, value in (
        ("CMake", deps["cmake_minimum"]),
        ("LLVM", llvm),
        ("Clang", clang),
    ):
        check(
            f"{label} {value}+" in readme,
            f"README.md does not state '{label} {value}+' as project.toml declares",
        )
    check(
        f"C++{deps['cxx_standard']} compiler" in readme,
        f"README.md does not state 'C++{deps['cxx_standard']} compiler'",
    )


def check_backend_names(project: dict) -> None:
    declared = {
        entry["id"]: entry["display"]
        for entry in project["backends"]
        if entry["kind"] == "gpu"
    }

    source = read(ROOT / "src" / "device_backend.cpp")
    body = re.search(
        r"std::string backend_display_name\(Backend backend\) \{(.*?)\n\}",
        source,
        re.DOTALL,
    )
    check(body is not None, "device_backend.cpp has no backend_display_name")
    if body:
        compiled = {
            member.lower(): display
            for member, display in re.findall(
                r'case Backend::(\w+): return "([^"]+)";', body.group(1)
            )
            # The fake backend exists only under QUIDRA_ENABLE_TEST_GPU_BACKEND
            # and is deliberately not a shipped backend.
            if member != "Test"
        }
        check(
            compiled == declared,
            f"device_backend.cpp maps {compiled} but project.toml declares "
            f"{declared}",
        )

    manifest = json.loads(read(ROOT / "quidra.manifest.json"))
    model = manifest["gpu_backend_model"]
    documented = {key for key, value in model.items() if isinstance(value, dict)}
    check(
        documented == set(declared),
        f"quidra.manifest.json documents backends {sorted(documented)} but "
        f"project.toml declares {sorted(declared)}",
    )


def check_platform_targets(project: dict) -> None:
    source = read(ROOT / "src" / "package_cli.cpp")
    compiled = set(
        re.findall(
            r'return "((?:linux|macos|windows)-(?:x86_64|arm64))";',
            source,
        )
    )
    declared = set(project["platforms"]["targets"])
    check(
        compiled == declared,
        f"package_cli.cpp platform ids {sorted(compiled)} but project.toml "
        f"declares {sorted(declared)}",
    )


def check_source_extension_consumers(project: dict) -> None:
    extension = project["project"]["extension"]
    consumers = {
        "include/quidra/import_path.hpp": read(
            ROOT / "include" / "quidra" / "import_path.hpp"
        ),
        "src/frontend.cpp": read(ROOT / "src" / "frontend.cpp"),
        "src/package_cli.cpp": read(ROOT / "src" / "package_cli.cpp"),
    }
    forbidden = (
        f'extension() != "{extension}"',
        f'extension() == "{extension}"',
        f'+= "{extension}"',
        f'/ "main{extension}"',
    )
    offenders = []
    for name, text in consumers.items():
        for literal in forbidden:
            if literal in text:
                offenders.append(f"{name}: {literal}")
    check(
        not offenders,
        "machine-sensitive source/package paths must derive from project.toml "
        "instead of spelling the extension/entrypoint directly:\n  "
        + "\n  ".join(offenders),
    )


def check_installer_repository(project: dict) -> None:
    expected = project["repos"]["core"].removeprefix("https://github.com/")
    installer = read(ROOT / "scripts" / "install-ubuntu.sh")
    declared = re.search(r'^REPO="([^"]+)"', installer, re.MULTILINE)
    check(declared is not None, "install-ubuntu.sh does not set REPO")
    if declared:
        check(
            declared.group(1) == expected,
            f"install-ubuntu.sh installs from {declared.group(1)} but "
            f"project.toml declares {expected}",
        )


def check_generated_header_placeholders() -> None:
    template = read(ROOT / "include" / "quidra" / "project.hpp.in")
    cmake_text = read(ROOT / "CMakeLists.txt")
    for placeholder in sorted(set(re.findall(r"@(\w+)@", template))):
        if placeholder == "PROJECT_VERSION":
            continue
        check(
            f"{placeholder})" in cmake_text,
            f"project.hpp.in uses @{placeholder}@ but CMakeLists.txt never "
            f"defines it, so the generated header would contain an empty value",
        )


# Where a version literal would be a second definition rather than an example.
# tests/ and docs/ are excluded on purpose: their version strings are fixtures
# and tutorial snippets. benchmark/ records the toolchain a frozen run measured.
VERSION_SCAN_ROOTS = (
    "src",
    "include",
    "scripts",
    ".github",
    "CMakeLists.txt",
    "README.md",
    "RELEASING.md",
    "quidra.manifest.json",
)


def check_no_second_version_definition(project: dict) -> None:
    version = project["project"]["version"]
    # The only lines in generated files that may carry the version, spelled
    # exactly as the generator writes them.
    generated = {
        "quidra.manifest.json": f'  "compiler_version": "{version}",',
        "README.md": f"{project['project']['name']} {version}",
    }

    scanned = []
    for root in VERSION_SCAN_ROOTS:
        path = ROOT / root
        scanned.extend(
            sorted(p for p in path.rglob("*") if p.is_file())
            if path.is_dir()
            else [path]
        )

    offenders = []
    for path in scanned:
        name = path.relative_to(ROOT).as_posix()
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for number, line in enumerate(text.split("\n"), start=1):
            if version not in line:
                continue
            if generated.get(name) == line:
                continue
            offenders.append(f"{name}:{number}: {line.strip()}")
    check(
        not offenders,
        "the version is defined outside project.toml; derive it instead:\n  "
        + "\n  ".join(offenders),
    )


def main() -> int:
    project = toml_subset.load(ROOT / "project.toml")

    check_generated_files_are_current()
    check_build_dependencies(project)
    check_backend_names(project)
    check_platform_targets(project)
    check_source_extension_consumers(project)
    check_installer_repository(project)
    check_generated_header_placeholders()
    check_no_second_version_definition(project)

    if FAILURES:
        for failure in FAILURES:
            print(f"metadata SSOT: {failure}", file=sys.stderr)
        return 1
    print("metadata SSOT: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
