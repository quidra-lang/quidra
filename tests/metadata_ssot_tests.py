#!/usr/bin/env python3
"""Prove that project.toml is the only place each project value is written down.

Two kinds of check live here:

  * derived files must match what scripts/sync_metadata.py would write, so a
    hand-edit of a generated file fails instead of silently drifting;
  * files that cannot be generated - C++ source, CMake, shell, prose - must
    still agree with project.toml.

The same idea covers the process environment: C and C++ code reads it only
through src/platform/environment.hpp, and the variables it reads are listed
once, in the docs/development.md table, which also gives each variable the
class src/toolchain/compile_environment.hpp gives it for the run cache's key.
It also covers the runtime's entry
points: src/llvm_backend/runtime_abi.hpp names each one once, and the
runtime prelude declares each of them exactly once. And it covers the test
suites' run cache: every suite that runs programs through the CLI's run forms
gives them a cache directory of its own run. And it covers what a compilation
reads: the code a compilation runs reaches the file system only through the
recording helpers of src/compile_inputs.cpp.

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
    source = read(ROOT / "src" / "package_manifest.cpp")
    compiled = set(
        re.findall(
            r'return "((?:linux|macos|windows)-(?:x86_64|arm64))";',
            source,
        )
    )
    declared = set(project["platforms"]["targets"])
    check(
        compiled == declared,
        f"package_manifest.cpp platform ids {sorted(compiled)} but project.toml "
        f"declares {sorted(declared)}",
    )


def check_source_extension_consumers(project: dict) -> None:
    extension = project["project"]["extension"]
    consumers = {
        "include/quidra/import_path.hpp": read(
            ROOT / "include" / "quidra" / "import_path.hpp"
        ),
        "src/import_path.cpp": read(ROOT / "src" / "import_path.cpp"),
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


def check_lockfile_schema_ownership(project: dict) -> None:
    schema = project["compat"]["lockfile_schema"]
    current = f"quidra-lock-v{schema}"
    roots = ("src", "include", "tests", ".github")
    current_offenders = []
    for root in roots:
        base = ROOT / root
        for path in sorted(p for p in base.rglob("*") if p.is_file()):
            try:
                text = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            if current in text:
                current_offenders.append(path.relative_to(ROOT).as_posix())
    check(
        not current_offenders,
        f"current lockfile header {current!r} is handwritten instead of "
        "derived from project.toml: " + ", ".join(current_offenders),
    )

    allowed_legacy = {
        "src/package_lock.cpp",
        "tests/package_lock_tests.cpp",
    }
    legacy_offenders = []
    for version in range(1, schema):
        legacy = f"quidra-lock-v{version}"
        for root in roots:
            base = ROOT / root
            for path in sorted(p for p in base.rglob("*") if p.is_file()):
                name = path.relative_to(ROOT).as_posix()
                if name in allowed_legacy:
                    continue
                try:
                    text = path.read_text(encoding="utf-8")
                except (UnicodeDecodeError, OSError):
                    continue
                if legacy in text:
                    legacy_offenders.append(f"{name}: {legacy}")
    check(
        not legacy_offenders,
        "legacy lockfile headers belong only in the compatibility parser/test: "
        + ", ".join(legacy_offenders),
    )


def check_package_store_ownership(project: dict) -> None:
    store = project["package_manager"]["store_relative"]
    offenders = []
    for root in ("src", "include"):
        base = ROOT / root
        for path in sorted(p for p in base.rglob("*") if p.is_file()):
            try:
                text = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            if store in text:
                offenders.append(path.relative_to(ROOT).as_posix())
    check(
        not offenders,
        f"package store {store!r} is handwritten in C++ instead of deriving "
        "from project.toml: " + ", ".join(offenders),
    )


def check_no_source_extension_literals(project: dict) -> None:
    extension = re.escape(project["project"]["extension"])
    pattern = re.compile(
        r'"[^"\n]*' + extension + r'(?![A-Za-z0-9_])[^"\n]*"'
    )
    offenders = []
    for root in ("src", "include"):
        base = ROOT / root
        for path in sorted(p for p in base.rglob("*") if p.is_file()):
            try:
                text = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            for number, line in enumerate(text.splitlines(), start=1):
                if pattern.search(line):
                    offenders.append(
                        f"{path.relative_to(ROOT).as_posix()}:{number}: "
                        f"{line.strip()}"
                    )
    check(
        not offenders,
        "source extension is handwritten in implementation string literals; "
        "derive it from project.toml/source_extension instead:\n  "
        + "\n  ".join(offenders),
    )


def check_first_party_repositories(project: dict) -> None:
    repos = project["repos"]
    expected_names = {"core", "math", "nn", "vision", "video", "dnn"}
    check(
        set(repos) == expected_names,
        "project.toml [repos] must list exactly the first-party semantic package graph: "
        + ", ".join(sorted(expected_names)),
    )
    base = project["package_manager"]["official_repository_base"]
    for name in sorted(expected_names):
        repository_name = "quidra" if name == "core" else name
        expected = base + repository_name
        check(
            repos.get(name) == expected,
            f"project.toml [repos].{name} must be {expected}",
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
        "quidra.manifest.json": f'  "version": "{version}",',
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


ENVIRONMENT_HELPER = "src/platform/environment.hpp"
NATIVE_SOURCE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inc", ".m", ".mm"
}
# What C, C++ and Objective-C++ code can use to read the process environment:
# the C, POSIX and MSVC CRT readers, the Win32 readers, the environ arrays
# (POSIX, MSVC, glibc, macOS) and NSProcessInfo's environment. A reader counts
# when it is named, called or not (`auto read = &std::getenv;` reads the
# environment as surely as a call). Longer identifiers (<cfenv>'s fegetenv)
# and members (`options.getenv`, `process->environ`) do not count.
ENVIRONMENT_READERS = (
    "getenv", "secure_getenv", "_wgetenv", "getenv_s", "_wgetenv_s",
    "_dupenv_s", "_wdupenv_s",
    "GetEnvironmentVariable", "GetEnvironmentVariableA", "GetEnvironmentVariableW",
    "GetEnvironmentStrings", "GetEnvironmentStringsA", "GetEnvironmentStringsW",
    "environ", "_environ", "__environ", "_wenviron",
    "_get_environ", "_get_wenviron", "_NSGetEnviron",
)
ENVIRONMENT_READ = re.compile(
    r"(?<![\w$.])(?<!->)(?:" + "|".join(ENVIRONMENT_READERS) + r")(?![\w$])"
    r"|\bprocessInfo\s*(?:\]|\(\s*\))?\s*\.?\s*environment\b"
)
LEXICAL_START = re.compile(r"//|/\*|[\"']")
RAW_STRING_PREFIXES = {"R", "u8R", "uR", "UR", "LR"}


def blank(text: str) -> str:
    return "".join("\n" if character == "\n" else " " for character in text)


def quoted_end(text: str, start: int, quote: str) -> int:
    """End of the literal opened at `start`; an unterminated one ends at its line."""
    index = start + 1
    while index < len(text):
        character = text[index]
        if character == "\\":
            index += 2
        elif character == quote:
            return index + 1
        elif character == "\n":
            return index
        else:
            index += 1
    return len(text)


def code_without_comments_and_literals(text: str) -> str:
    """`text` with comments and string and character literals blanked out.

    Newlines stay, so line numbers stay true. Raw strings (R"delim(...)delim")
    and C++14 digit separators (1'000) are recognized.
    """
    pieces: list[str] = []
    index = 0
    while True:
        match = LEXICAL_START.search(text, index)
        if not match:
            pieces.append(text[index:])
            return "".join(pieces)
        start = match.start()
        pieces.append(text[index:start])
        token = match.group(0)
        prefix_start = start
        while prefix_start > 0 and (text[prefix_start - 1].isalnum() or text[prefix_start - 1] == "_"):
            prefix_start -= 1
        prefix = text[prefix_start:start]
        if token == "//":
            end = start
            while end < len(text) and text[end] != "\n":
                # A backslash-newline continues a line comment.
                end += 2 if text.startswith("\\\n", end) else 1
        elif token == "/*":
            end = text.find("*/", start + 2)
            end = len(text) if end < 0 else end + 2
        elif token == "'" and prefix[:1].isdigit():
            end = start + 1  # digit separator
            pieces.append("'")
            index = end
            continue
        elif token == '"' and prefix in RAW_STRING_PREFIXES and (
            raw := re.match(r'"([^()\\\s"]{0,16})\(', text[start:start + 19])
        ):
            closing = ")" + raw.group(1) + '"'
            end = text.find(closing, start + raw.end())
            end = len(text) if end < 0 else end + len(closing)
        else:
            end = quoted_end(text, start, token)
        pieces.append(blank(text[start:end]))
        index = end


def environment_reads(code: str) -> list[tuple[int, str]]:
    """(line number, source line) of every environment read in `code`."""
    stripped = code_without_comments_and_literals(code)
    lines = code.split("\n")
    reads = []
    for match in ENVIRONMENT_READ.finditer(stripped):
        number = stripped.count("\n", 0, match.start()) + 1
        reads.append((number, lines[number - 1].strip()))
    return reads


# Source snippets and whether each one reads the environment. They pin what
# check_environment_reads catches and what it leaves alone.
ENVIRONMENT_READ_SAMPLES = {
    'const char* home = std::getenv("HOME");': True,
    'auto read = &std::getenv;': True,
    'if (::getenv ("HOME")) {}': True,
    'auto home = std ::\n    getenv("HOME");': True,
    'extern char** environ;\nchar* first = environ[0];': True,
    'char** entries = _environ;': True,
    '_dupenv_s(&raw, &size, name);': True,
    'GetEnvironmentVariableW(L"PATH", buffer, 32);': True,
    'wchar_t* block = GetEnvironmentStringsW();': True,
    'char*** entries = _NSGetEnviron();': True,
    'NSDictionary* all = [[NSProcessInfo processInfo] environment];': True,
    "int bytes = 1'000; auto home = getenv(\"HOME\");": True,
    'auto text = R"(getenv)"; auto home = getenv("HOME");': True,
    'fegetenv(&state);': False,
    'options.getenv(name);': False,
    'process->environ = nullptr;': False,
    'auto value = platform::environment_value("HOME");': False,
    '// std::getenv("HOME")': False,
    '/* getenv("HOME")\n   environ */ int x = 0;': False,
    'emit("declare ptr @getenv(ptr)");': False,
    'auto ir = R"ir(call ptr @getenv(ptr "x")\n  environ)ir";': False,
    "char quote = '\"'; int getenvironment_count = 0;": False,
    'auto text = "say \\"getenv\\" twice";': False,
}


def check_environment_read_detector() -> None:
    for snippet, reads in ENVIRONMENT_READ_SAMPLES.items():
        found = bool(environment_reads(snippet))
        check(
            found == reads,
            f"environment read detector {'missed' if reads else 'misreported'}: "
            f"{snippet!r}",
        )


def native_sources(*roots: str) -> list[Path]:
    return [
        path
        for root in roots
        for path in sorted((ROOT / root).rglob("*"))
        if path.is_file() and path.suffix in NATIVE_SOURCE_SUFFIXES
    ]


def check_environment_reads() -> None:
    """platform::environment is the only code that reads the environment."""
    offenders = []
    for path in native_sources("src", "include", "tests"):
        name = path.relative_to(ROOT).as_posix()
        if name == ENVIRONMENT_HELPER:
            continue
        for number, line in environment_reads(read(path)):
            offenders.append(f"{name}:{number}: {line}")
    check(
        not offenders,
        f"read environment variables through {ENVIRONMENT_HELPER} "
        "(quidra::platform::environment_value / environment_has), not "
        "directly; raw getenv is also an MSVC /WX error (C4996):\n  "
        + "\n  ".join(offenders),
    )


def check_environment_variables_documented() -> None:
    """docs/development.md lists exactly the QUIDRA_* variables Core reads."""
    text = read(ROOT / "docs" / "development.md")
    heading = "## Environment variables\n"
    check(heading in text, "docs/development.md has no Environment variables section")
    if heading not in text:
        return
    section = text.split(heading, 1)[1].split("\n## ", 1)[0]
    documented = set(re.findall(r"`(QUIDRA_[A-Z0-9_]+)`", section))
    read_by_core: dict[str, str] = {}
    for path in native_sources("src", "include"):
        for name in re.findall(r'"(QUIDRA_[A-Z0-9_]+)"', read(path)):
            read_by_core.setdefault(name, path.relative_to(ROOT).as_posix())
    missing = sorted(set(read_by_core) - documented)
    stale = sorted(documented - set(read_by_core))
    check(
        not missing,
        "environment variables missing from the docs/development.md table: "
        + ", ".join(f"{name} ({read_by_core[name]})" for name in missing),
    )
    check(
        not stale,
        "docs/development.md documents environment variables no source reads: "
        + ", ".join(stale),
    )


# The code a compilation runs: module loading and import resolution, the
# package manifest and lock readers, and every stage after them. It reads the
# file system only through the helpers of src/compile_inputs.cpp, which record
# each read (quidra/compile_inputs.hpp), so that a run cache key misses no
# input.
COMPILE_INPUT_HELPER = "src/compile_inputs.cpp"
COMPILE_INPUT_FILES = (
    "src/frontend.cpp", "src/compiler.cpp", "src/import_path.cpp",
    "src/package_manifest.cpp", "src/package_lock.cpp", "src/lexer.cpp",
    "src/parser.cpp", "src/checker.cpp", "include/quidra/import_path.hpp",
    "include/quidra/frontend.hpp", "include/quidra/compiler.hpp",
)
COMPILE_INPUT_DIRECTORIES = (
    "src/ir", "src/lowering", "src/optimizer", "src/llvm_backend", "src/llvm_text",
)
# Functions whose reads are recorded by their result: the compilation records
# the digest package_tree_sha256 returns as a package_tree input.
RECORDED_READERS = {"src/package_lock.cpp": ("package_tree_sha256",)}
# File system reads: streams that read, the C and POSIX readers, and the
# std::filesystem queries that look at what a path holds. Building a path
# (absolute, lexically_normal, current_path) reads nothing.
COMPILE_INPUT_READ = re.compile(
    r"(?<![\w$.<])(?<!->)(?:std::)?(?:basic_)?(?:i|w?i)?fstream\b"
    r"|(?<![\w$.])(?<!->)(?:::)?(?:fopen|_wfopen|fopen_s|_wfopen_s|freopen|open|openat"
    r"|stat|lstat|fstatat|access|faccessat|opendir|readlink|realpath"
    r"|CreateFileA|CreateFileW|GetFileAttributesW|GetFileAttributesA)\s*\("
    r"|\b(?:std::)?(?:filesystem|fs)::(?:exists|is_regular_file|is_directory|is_symlink"
    r"|is_empty|is_other|is_block_file|is_character_file|is_fifo|is_socket|status"
    r"|symlink_status|file_size|hard_link_count|last_write_time|canonical"
    r"|weakly_canonical|equivalent|read_symlink|directory_iterator"
    r"|recursive_directory_iterator|space)\b"
    r"|(?<![\w$.])try_read_toml_subset\b"
)

# Source snippets and whether each one reads the file system. They pin what
# check_compile_input_reads catches and what it leaves alone.
COMPILE_INPUT_READ_SAMPLES = {
    'std::ifstream in(path, std::ios::binary);': True,
    'std::fstream stream(path);': True,
    '#include <fstream>': False,
    'FILE* file = fopen(name, "rb");': True,
    'if (::stat(path.c_str(), &status) == 0) {}': True,
    'if (fs::exists(path)) {}': True,
    'auto ok = std::filesystem::is_regular_file(path, error);': True,
    'for (const auto& item : fs::recursive_directory_iterator(root)) {}': True,
    'const auto real = fs::weakly_canonical(path);': True,
    'const auto document = try_read_toml_subset(path);': True,
    'auto text = read_input_file(path, InputFileKind::source, inputs_);': False,
    'auto state = probe_input_path(path, inputs);': False,
    'std::ofstream out(path);': False,
    'auto cwd = fs::absolute(path).lexically_normal();': False,
    'std::ostringstream out; out << value;': False,
    'auto file = stream.open(path);': False,
    'const auto status = entry.status();': False,
    '// std::ifstream in(path);': False,
    'auto text = "fs::exists(path)";': False,
    'int restat(int x); restat(1);': False,
}


def compile_input_reads(code: str) -> list[tuple[int, str]]:
    """(line number, source line) of every file system read in `code`."""
    stripped = code_without_comments_and_literals(code)
    lines = code.split("\n")
    return [
        (stripped.count("\n", 0, match.start()) + 1,
         lines[stripped.count("\n", 0, match.start())].strip())
        for match in COMPILE_INPUT_READ.finditer(stripped)
    ]


def function_body_lines(code: str, name: str) -> set[int]:
    """Line numbers of the body of the definition of function `name`."""
    stripped = code_without_comments_and_literals(code)
    lines: set[int] = set()
    for match in re.finditer(r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*(?:const\s*)?\{",
                             stripped):
        depth = 0
        index = match.end() - 1
        while index < len(stripped):
            if stripped[index] == "{":
                depth += 1
            elif stripped[index] == "}":
                depth -= 1
                if depth == 0:
                    break
            index += 1
        first = stripped.count("\n", 0, match.start()) + 1
        last = stripped.count("\n", 0, index) + 1
        lines.update(range(first, last + 1))
    return lines


def check_compile_input_reads() -> None:
    """The code a compilation runs reads files only through the recorder."""
    for snippet, reads in COMPILE_INPUT_READ_SAMPLES.items():
        check(
            bool(compile_input_reads(snippet)) == reads,
            f"compile input read detector {'missed' if reads else 'misreported'}: "
            f"{snippet!r}",
        )
    files = [ROOT / name for name in COMPILE_INPUT_FILES]
    files += native_sources(*COMPILE_INPUT_DIRECTORIES)
    offenders = []
    for path in files:
        name = path.relative_to(ROOT).as_posix()
        check(path.exists(), f"check_compile_input_reads: {name} does not exist")
        if not path.exists():
            continue
        code = read(path)
        exempt: set[int] = set()
        for function in RECORDED_READERS.get(name, ()):
            body = function_body_lines(code, function)
            check(bool(body), f"check_compile_input_reads: {name} defines no {function}")
            exempt |= body
        for number, line in compile_input_reads(code):
            if number not in exempt:
                offenders.append(f"{name}:{number}: {line}")
    check(
        (ROOT / COMPILE_INPUT_HELPER).exists(),
        f"check_compile_input_reads: {COMPILE_INPUT_HELPER} does not exist",
    )
    check(
        not offenders,
        "read compile inputs through the recording helpers of "
        f"{COMPILE_INPUT_HELPER} (read_input_file, probe_input_path, "
        "input_path_state), so that the run cache sees every input:\n  "
        + "\n  ".join(offenders),
    )


COMPILE_ENVIRONMENT = "src/toolchain/compile_environment.hpp"
CACHE_KEY_CLASSES = {"value", "effect", "no"}


def environment_table_rows() -> list[list[str]]:
    """The cells of each row of the docs/development.md environment table."""
    text = read(ROOT / "docs" / "development.md")
    heading = "## Environment variables\n"
    if heading not in text:
        return []
    section = text.split(heading, 1)[1].split("\n## ", 1)[0]
    rows = []
    for line in section.splitlines():
        if not line.startswith("|") or line.startswith("|---"):
            continue
        rows.append([cell.strip() for cell in line.strip().strip("|").split(" | ")])
    return rows


def check_cache_key_environment() -> None:
    """The docs table's "Run cache key" column and compile_environment.hpp
    classify the same variables the same way."""
    rows = environment_table_rows()
    check(bool(rows), "docs/development.md has no environment table")
    if not rows:
        return
    check(rows[0][-1] == "Run cache key",
          "the docs/development.md environment table needs a last column 'Run cache key'")
    documented: dict[str, str] = {}
    for cells in rows[1:]:
        key = cells[-1].strip("`")
        check(key in CACHE_KEY_CLASSES,
              f"docs/development.md: run cache key class {cells[-1]!r} of {cells[0]!r} "
              f"is not one of {sorted(CACHE_KEY_CLASSES)}")
        for name in re.findall(r"`([A-Za-z_][A-Za-z0-9_]*)`", cells[0]):
            check(name not in documented,
                  f"docs/development.md lists {name} in two environment table rows")
            documented[name] = key
    table: dict[str, str] = {}
    for name, key in re.findall(r'\{"([A-Za-z_][A-Za-z0-9_]*)",\s*CacheKeyClass::(\w+)\}',
                                read(ROOT / COMPILE_ENVIRONMENT)):
        check(name not in table, f"{COMPILE_ENVIRONMENT} classifies {name} twice")
        table[name] = key
    check(bool(table), f"{COMPILE_ENVIRONMENT}: no variable found")
    missing = sorted(set(documented) - set(table))
    unlisted = sorted(set(table) - set(documented))
    differing = sorted(name for name in set(documented) & set(table)
                       if documented[name] != table[name])
    check(not missing,
          f"environment variables documented but not classified in {COMPILE_ENVIRONMENT}: "
          + ", ".join(missing))
    check(not unlisted,
          f"environment variables classified in {COMPILE_ENVIRONMENT} but missing from the "
          "docs/development.md table: " + ", ".join(unlisted))
    check(not differing,
          "environment variables whose run cache key class differs between "
          f"docs/development.md and {COMPILE_ENVIRONMENT}: "
          + ", ".join(f"{name} ({documented[name]} / {table[name]})" for name in differing))


# The CLI's run forms, `quidra FILE.qui ...` and `quidra run ...`, in the test
# suites: the shell suites call the CLI through a QUIDRA... variable, the
# Python drivers pass "run" right after it.
SHELL_RUN_FORM = re.compile(
    r'"\$\{?QUIDRA\w*\}?"\s+(?:run\b|"[^"\n]*\.qui"|[^\s"|;&<>()]*\.qui\b)')
PYTHON_RUN_FORM = re.compile(r'(?i)(?:quidra\w*\)?\s*,\s*|\brun_tool\(\s*)"run"')
CACHE_DIRECTORY_SET = re.compile(r'QUIDRA_CACHE_DIR"?\]?\s*=')

# Lines of a suite and whether each one uses a run form. They pin what
# check_test_cache_isolation counts as running a program.
RUN_FORM_SAMPLES = {
    ".sh": {
        '"$QUIDRA" run "$TMP/main.qui"': True,
        'output="$("$QUIDRA" "$TMP/main.qui")"': True,
        'HOME="$TMP/home" "$QUIDRA" main.qui >"$TMP/out"': True,
        '"$QUIDRA_TIMING" "$TMP/run-lease/main.qui" &': True,
        '"$QUIDRA" build "$TMP/main.qui" -o "$TMP/main"': False,
        '"$QUIDRA" check "$TMP/main.qui" --json': False,
        '"$QUIDRA" repl < "$TMP/main.qui"': False,
        '"$QUIDRA" package install "$TMP/source"': False,
    },
    ".py": {
        '[QUIDRA, "run", source]': True,
        '[str(quidra), "run", str(source)]': True,
        'run_tool("run", str(source), cwd=tmp)': True,
        '[str(quidra), "build", str(source), "-o", str(binary)]': False,
        'data=json.dumps({"mode": "run", "source": text})': False,
    },
}


def uses_run_form(suffix: str, text: str) -> bool:
    pattern = SHELL_RUN_FORM if suffix == ".sh" else PYTHON_RUN_FORM
    return any(pattern.search(line) for line in text.splitlines()
               if not line.lstrip().startswith("#"))


def check_test_cache_isolation() -> None:
    """Every suite that runs programs through the CLI's run forms sets
    QUIDRA_CACHE_DIR, so that it never uses the user's run cache and every
    run of it sees the same cache hits and misses."""
    for suffix, samples in RUN_FORM_SAMPLES.items():
        for line, runs in samples.items():
            check(
                uses_run_form(suffix, line) == runs,
                f"run form detector {'missed' if runs else 'misreported'}: {line!r}",
            )
    suites = [
        path
        for pattern in ("tests/*.sh", "tests/*.py", "tests/golden/*.sh",
                        "tests/golden/*.py", "scripts/*.sh")
        for path in sorted(ROOT.glob(pattern))
        if path != Path(__file__).resolve()
    ]
    offenders = []
    for path in suites:
        text = read(path)
        if not uses_run_form(path.suffix, text):
            continue
        sets_cache = any(CACHE_DIRECTORY_SET.search(line) for line in text.splitlines()
                         if not line.lstrip().startswith("#"))
        if not sets_cache:
            offenders.append(path.relative_to(ROOT).as_posix())
    check(
        not offenders,
        "suites that run programs through `quidra FILE.qui` or `quidra run` "
        "without a QUIDRA_CACHE_DIR of their own run: " + ", ".join(offenders),
    )


# The families of runtime_abi.hpp that are not runtime entry points: the
# prelude defines its helpers in LLVM IR and writes the C library
# declarations as they are.
RUNTIME_ABI_VERBATIM_FAMILIES = {"prelude", "c_library"}


def runtime_abi_entry_points(text: str) -> list[str]:
    """family::name of every LlvmCallee runtime_abi.hpp defines outside the
    verbatim families."""
    entries = []
    family = None
    for line in text.splitlines():
        if match := re.match(r"namespace (\w+) \{$", line):
            family = match.group(1)
        elif re.match(r"\} // namespace \w+$", line):
            family = None
        elif match := re.match(r"inline constexpr LlvmCallee (\w+) =", line):
            if family is not None and family not in RUNTIME_ABI_VERBATIM_FAMILIES:
                entries.append(f"{family}::{match.group(1)}")
    return entries


def check_runtime_entry_points_declared() -> None:
    """The prelude declares every runtime entry point of runtime_abi.hpp
    exactly once, and nothing else through declare(); the entry points of
    the exact real support block (family exact_real_support) and of the
    bare integer support block (family bare_integer_support) are declared by
    those blocks (small_rational.cpp, bare_integer.cpp) instead."""
    backend = ROOT / "src" / "llvm_backend"
    entries = runtime_abi_entry_points(read(backend / "runtime_abi.hpp"))
    declared = re.findall(r"declare\(runtime_abi::(\w+::\w+)\)",
                          read(backend / "runtime_prelude.cpp"))
    support = re.findall(r"&runtime_abi::(exact_real_support::\w+)",
                         read(backend / "small_rational.cpp"))
    check(bool(support), "small_rational.cpp: no support block entry point found")
    check(all(not entry.startswith("exact_real_support::") for entry in declared),
          "the prelude declares an entry point of the exact real support block")
    integers = re.findall(r"&runtime_abi::(bare_integer_support::\w+)",
                          read(backend / "bare_integer.cpp"))
    check(bool(integers), "bare_integer.cpp: no support block entry point found")
    check(all(not entry.startswith("bare_integer_support::") for entry in declared),
          "the prelude declares an entry point of the bare integer support block")
    declared = declared + support + integers
    check(bool(entries), "runtime_abi.hpp: no runtime entry point found")
    missing = sorted(set(entries) - set(declared))
    unknown = sorted(set(declared) - set(entries))
    repeated = sorted({entry for entry in declared if declared.count(entry) > 1})
    check(
        not missing,
        "runtime entry points the runtime prelude does not declare "
        "(add a declare() item to runtime_prelude.cpp): " + ", ".join(missing),
    )
    check(
        not unknown,
        "runtime prelude declare() items that are no runtime entry point of "
        "runtime_abi.hpp: " + ", ".join(unknown),
    )
    check(not repeated, "runtime entry points the prelude declares twice: " + ", ".join(repeated))


# The checker functions that may look a name up among the fields of the class
# being checked. Every other check asks receiver_field(), so a bare name never
# turns into a receiver field again (fields are written `this.NAME`).
RECEIVER_FIELD_LOOKUPS = ("receiver_field", "check_this_field", "reject_bare_field")


def check_receiver_field_resolution() -> None:
    """Receiver fields are resolved only by the checker's receiver-field
    helpers: `find_field(current_class_` appears nowhere else."""
    text = read(ROOT / "src" / "checker.cpp")
    allowed = 0
    for name in RECEIVER_FIELD_LOOKUPS:
        match = re.search(r"\n[^\n]*\bChecker::" + name + r"\([^)]*\)[^{]*\{", text)
        check(match is not None, f"src/checker.cpp: Checker::{name} not found")
        if match is None:
            continue
        end = text.find("\n}\n", match.end())
        allowed += text[match.end():end].count("find_field(current_class_")
    total = text.count("find_field(current_class_")
    check(
        total == allowed,
        "src/checker.cpp: find_field(current_class_, ...) outside the receiver-field "
        "helpers; resolve receiver fields through receiver_field()",
    )


def main() -> int:
    project = toml_subset.load(ROOT / "project.toml")

    check_generated_files_are_current()
    check_build_dependencies(project)
    check_backend_names(project)
    check_platform_targets(project)
    check_source_extension_consumers(project)
    check_lockfile_schema_ownership(project)
    check_package_store_ownership(project)
    check_no_source_extension_literals(project)
    check_first_party_repositories(project)
    check_installer_repository(project)
    check_generated_header_placeholders()
    check_no_second_version_definition(project)
    check_environment_read_detector()
    check_environment_reads()
    check_environment_variables_documented()
    check_cache_key_environment()
    check_runtime_entry_points_declared()
    check_test_cache_isolation()
    check_compile_input_reads()
    check_receiver_field_resolution()

    if FAILURES:
        for failure in FAILURES:
            print(f"metadata SSOT: {failure}", file=sys.stderr)
        return 1
    print("metadata SSOT: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
