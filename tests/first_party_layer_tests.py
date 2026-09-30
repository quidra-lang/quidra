#!/usr/bin/env python3
"""Enforce dependency, compatibility, and release consistency across first-party packages."""

from __future__ import annotations

from pathlib import Path
import re
import sys

LAYERS = {
    "math": 2,
    "nn": 3,
    "vision": 3,
    "video": 3,
    "dnn": 4,
}
FIRST_PARTY = frozenset(LAYERS)
LEGACY_NAME_RE = re.compile(r"^name\s*=\s*([^\s#]+)\s*$")
LEGACY_VERSION_RE = re.compile(r"^version\s*=\s*([^\s#]+)\s*$")
LEGACY_REQUIRE_RE = re.compile(
    r"^requires\.([A-Za-z0-9_-]+)\s*=\s*(.*?)\s*$"
)
PACKAGE_IMPORT_RE = re.compile(
    r"^\s*(?:public\s+)?import\s+"
    r"(?:[A-Za-z_][A-Za-z0-9_]*\s*=\s*)?"
    r"([A-Za-z_][A-Za-z0-9_-]*)\s*$"
)
TEST_PATH_RE = re.compile(
    r"(?<![A-Za-z0-9_.-])((?:quidra/)?tests/[A-Za-z0-9_./-]+)"
)
SEMVER_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")
REQUIREMENT_TERM_RE = re.compile(r"^(<=|>=|=|<|>)(\d+\.\d+\.\d+)$")


def unquote(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] == '"':
        return value[1:-1]
    return value


def parse_semver(value: str) -> tuple[int, int, int]:
    match = SEMVER_RE.fullmatch(value)
    if not match:
        raise ValueError(f"invalid MAJOR.MINOR.PATCH version {value!r}")
    return tuple(int(part) for part in match.groups())


def version_satisfies(version: str, requirement: str) -> bool:
    actual = parse_semver(version)
    terms = requirement.split()
    if not terms:
        raise ValueError("empty version requirement")
    for term in terms:
        match = REQUIREMENT_TERM_RE.fullmatch(term)
        if not match:
            raise ValueError(f"unsupported version requirement term {term!r}")
        operator, expected_text = match.groups()
        expected = parse_semver(expected_text)
        if operator == "=" and actual != expected:
            return False
        if operator == "<" and not actual < expected:
            return False
        if operator == "<=" and not actual <= expected:
            return False
        if operator == ">" and not actual > expected:
            return False
        if operator == ">=" and not actual >= expected:
            return False
    return True


def legacy_metadata(root: Path) -> tuple[str, str, dict[str, str]]:
    manifest = root / "quidra.package"
    if not manifest.is_file():
        raise ValueError(f"{root}: missing quidra.package")

    name: str | None = None
    version: str | None = None
    requirements: dict[str, str] = {}
    for raw in manifest.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if name is None:
            match = LEGACY_NAME_RE.match(line)
            if match:
                name = unquote(match.group(1))
                continue
        if version is None:
            match = LEGACY_VERSION_RE.match(line)
            if match:
                version = unquote(match.group(1))
                continue
        match = LEGACY_REQUIRE_RE.match(line)
        if match:
            requirements[match.group(1)] = unquote(match.group(2))

    if not name:
        raise ValueError(f"{manifest}: missing package name")
    if not version:
        raise ValueError(f"{manifest}: missing package version")
    parse_semver(version)
    return name, version, requirements


def project_metadata(
    root: Path,
) -> tuple[str, int | None, dict[str, str]]:
    project = root / "project.toml"
    if not project.is_file():
        raise ValueError(f"{root}: missing project.toml")

    version: str | None = None
    abi: int | None = None
    requirements: dict[str, str] = {}
    section = ""
    for raw in project.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line
            continue
        if "=" not in line:
            continue
        key, raw_value = (part.strip() for part in line.split("=", 1))
        value = unquote(raw_value)
        if section == "[package]" and key == "version":
            version = value
        elif section == "[requires]":
            if key == "abi":
                try:
                    abi = int(value)
                except ValueError as error:
                    raise ValueError(
                        f"{project}: requires.abi must be an integer"
                    ) from error
            else:
                requirements[key] = value

    if not version:
        raise ValueError(f"{project}: missing package.version")
    parse_semver(version)
    return version, abi, requirements


def core_contract() -> tuple[str, int, set[str]]:
    project = Path(__file__).resolve().parents[1] / "project.toml"
    section = ""
    version: str | None = None
    abi: int | None = None
    repositories: set[str] = set()
    for raw in project.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line
            continue
        if "=" not in line:
            continue
        key, raw_value = (part.strip() for part in line.split("=", 1))
        value = unquote(raw_value)
        if section == "[project]" and key == "version":
            version = value
        elif section == "[compat]" and key == "abi":
            try:
                abi = int(value)
            except ValueError as error:
                raise ValueError(
                    f"{project}: compat.abi must be an integer"
                ) from error
        elif section == "[repos]" and key != "core":
            repositories.add(key)

    if not version:
        raise ValueError(f"{project}: missing project.version")
    if abi is None:
        raise ValueError(f"{project}: missing compat.abi")
    parse_semver(version)
    return version, abi, repositories


def source_first_party_imports(root: Path) -> set[str]:
    dependencies: set[str] = set()
    for source in sorted(root.rglob("*.qui")):
        relative = source.relative_to(root)
        if relative.parts and relative.parts[0] in {"tests", "examples", "build"}:
            continue
        for raw in source.read_text(encoding="utf-8").splitlines():
            match = PACKAGE_IMPORT_RE.match(raw)
            if match and match.group(1) in FIRST_PARTY:
                dependencies.add(match.group(1))
    return dependencies


def release_contract_failures(
    root: Path, name: str, project_requires: dict[str, str]
) -> list[str]:
    failures: list[str] = []
    development = root / "docs" / "development.md"
    if not development.is_file():
        failures.append(f"{name}: missing canonical docs/development.md")

    workflow = root / ".github" / "workflows" / "release.yml"
    if not workflow.is_file():
        failures.append(f"{name}: missing .github/workflows/release.yml")
        return failures

    text = workflow.read_text(encoding="utf-8")
    if "ref: develop" in text:
        failures.append(
            f"{name}/release.yml: release dependencies must use immutable tags, not develop"
        )

    workflow_root = root / ".github" / "workflows"
    ci_workflow = workflow_root / "ci.yml"
    if not ci_workflow.is_file():
        failures.append(f"{name}: missing .github/workflows/ci.yml")
    else:
        validation_tests: set[str] = set()
        validation_workflows = sorted(
            list(workflow_root.glob("*.yml")) + list(workflow_root.glob("*.yaml"))
        )
        for validation_workflow in validation_workflows:
            if validation_workflow.name == "release.yml":
                continue
            validation_text = validation_workflow.read_text(encoding="utf-8")
            validation_tests.update(TEST_PATH_RE.findall(validation_text))

        release_tests = set(TEST_PATH_RE.findall(text))
        missing_release_tests = sorted(validation_tests - release_tests)
        if missing_release_tests:
            failures.append(
                f"{name}/release.yml: CI test contracts missing from release validation: "
                f"{missing_release_tests}"
            )

    release_dependencies = set(project_requires) & (FIRST_PARTY | {"quidra"})
    for dependency in sorted(release_dependencies):
        marker = f"repository: quidra-lang/{dependency}"
        if marker not in text:
            failures.append(
                f"{name}/release.yml: declared dependency {dependency!r} is not checked out "
                "for release validation"
            )
            continue
        same_version_marker = (
            f"repository: quidra-lang/{dependency}\n"
            "          ref: v${{ steps.package.outputs.version }}"
        )
        if same_version_marker not in text:
            failures.append(
                f"{name}/release.yml: dependency {dependency!r} must be checked out "
                "at the package's same-version release tag"
            )
    return failures


def main() -> int:
    if len(sys.argv) < 2:
        print(
            "usage: first_party_layer_tests.py PACKAGE_ROOT [...]",
            file=sys.stderr,
        )
        return 2

    failures: list[str] = []
    seen: set[str] = set()
    package_contracts: dict[
        str, tuple[str, int | None, dict[str, str]]
    ] = {}

    try:
        core_version, core_abi, registered_repositories = core_contract()
    except (OSError, UnicodeError, ValueError) as error:
        print(f"first-party package consistency failed:\n  - {error}", file=sys.stderr)
        return 1

    if registered_repositories != FIRST_PARTY:
        failures.append(
            "Core [repos]/first-party layer registry drift: "
            f"{sorted(registered_repositories)} != {sorted(FIRST_PARTY)}"
        )

    for argument in sys.argv[1:]:
        root = Path(argument).resolve()
        try:
            name, legacy_version, legacy_requires = legacy_metadata(root)
            project_version, project_abi, project_requires = project_metadata(root)
            source_requires = source_first_party_imports(root)
        except (OSError, UnicodeError, ValueError) as error:
            failures.append(str(error))
            continue

        if name not in LAYERS:
            failures.append(f"{root}: unknown first-party package {name!r}")
            continue
        if name in seen:
            failures.append(f"duplicate first-party package root for {name!r}")
            continue
        seen.add(name)
        package_contracts[name] = (project_version, project_abi, project_requires)

        if legacy_version != project_version:
            failures.append(
                f"{name}: quidra.package/project.toml version drift: "
                f"{legacy_version!r} != {project_version!r}"
            )
        if project_version != core_version:
            failures.append(
                f"{name}: first-party package version {project_version} must match "
                f"Core version {core_version}"
            )
        if legacy_requires != project_requires:
            failures.append(
                f"{name}: quidra.package/project.toml requirement drift: "
                f"{legacy_requires!r} != {project_requires!r}"
            )

        project_first_party = set(project_requires) & FIRST_PARTY
        if source_requires != project_first_party:
            failures.append(
                f"{name}: source import/project.toml first-party dependency drift: "
                f"{sorted(source_requires)} != {sorted(project_first_party)}"
            )

        for dependency in sorted(project_first_party):
            if LAYERS[dependency] >= LAYERS[name]:
                failures.append(
                    f"{name}: Layer {LAYERS[name]} package may not depend on "
                    f"Layer {LAYERS[dependency]} package {dependency!r}"
                )

        if project_abi is None:
            failures.append(f"{name}: project.toml must declare requires.abi")
        elif project_abi != core_abi:
            failures.append(
                f"{name}: requires.abi {project_abi} does not match Core ABI {core_abi}"
            )

        core_requirement = project_requires.get("quidra")
        if core_requirement is None:
            failures.append(f"{name}: project.toml must declare requires.quidra")
        else:
            try:
                if not version_satisfies(core_version, core_requirement):
                    failures.append(
                        f"{name}: current Core {core_version} does not satisfy "
                        f"requires.quidra {core_requirement!r}"
                    )
            except ValueError as error:
                failures.append(f"{name}: invalid requires.quidra: {error}")

        failures.extend(release_contract_failures(root, name, project_requires))

    missing_roots = sorted(FIRST_PARTY - seen)
    extra_roots = sorted(seen - FIRST_PARTY)
    if missing_roots:
        failures.append(
            f"first-party package roots missing from consistency check: {missing_roots}"
        )
    if extra_roots:
        failures.append(
            f"unexpected package roots in consistency check: {extra_roots}"
        )

    for name, (version, _abi, requirements) in sorted(package_contracts.items()):
        for dependency in sorted(set(requirements) & FIRST_PARTY):
            target = package_contracts.get(dependency)
            if target is None:
                failures.append(
                    f"{name}: dependency {dependency!r} was not supplied to the "
                    "cross-package consistency check"
                )
                continue
            dependency_version = target[0]
            requirement = requirements[dependency]
            try:
                if not version_satisfies(dependency_version, requirement):
                    failures.append(
                        f"{name}: checked-out {dependency} {dependency_version} does not "
                        f"satisfy requires.{dependency} {requirement!r}"
                    )
            except ValueError as error:
                failures.append(
                    f"{name}: invalid requires.{dependency}: {error}"
                )

    if failures:
        print("first-party package consistency failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(
        f"first-party package consistency: ok "
        f"(Core {core_version}, ABI {core_abi})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
