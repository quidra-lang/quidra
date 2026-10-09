#!/usr/bin/env python3
"""Gate Gm: a move or rename commit only moves code (tests/golden/README.md).

  check_moves.py [--base REV] [--head REV] [--mode lines|tokens]
                 [--rename-map FILE] [--paths PATH...]
                 [--shadow CLASS_HEADER:CLASS ...] [--new-code]
  check_moves.py --self-test

lines   the normalized removed-line multiset must equal the added-line
        multiset, up to glue (includes, namespaces, #pragma once, comments,
        access specifiers, declarations matching moved definitions) and after
        applying the rename map to the removed lines. Linkage words and
        qualifiers are glue only where a move changes them:
          - a leading `static`/`inline` on an unindented line (namespace
            scope) or on a function definition header (`R f(args) {`), never
            on a local variable, whose `static` decides whether its state
            outlives the call;
          - a `Class::` qualifier before the parameter list of an unindented
            line (an out-of-line definition), never at a call site.
        Lone `{` and `}` lines must balance: the added lone `}` lines may
        exceed the removed ones only by the added namespace openers, and
        lone `{` lines may not change at all, so wrapping moved code in a new
        block scope (which moves destructor and RAII timing) is a residual.
tokens  the same over C++ token streams (for mechanical commits whose line
        breaks change); residual tokens are printed with context. The brace
        balance applies too.

The rename map lists `old new` pairs, one per line (whole-word replacement,
applied in order). --shadow reports every local declared in a moved body
whose name is a data member or member function of the destination class
(MSVC C4456-C4459 and silent shadowing). --new-code fails on anti-patterns in
added lines: raw new/delete, std::exit, #define, std::function.

Defaults: --base HEAD^ --head HEAD, all C/C++ sources and headers.
Exit status 1 on residuals. --self-test runs the probes that pin the glue
rules (exit status 1 when one of them is classified wrongly).
"""

from __future__ import annotations

import argparse
import difflib
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

SOURCE = re.compile(r"\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inc|mm)$")
GLUE = [
    re.compile(r"^$"),
    re.compile(r"^#\s*include\s"),
    re.compile(r"^#\s*pragma once$"),
    re.compile(r"^(public|private|protected):$"),
    re.compile(r"^using namespace [\w:]+;$"),
    re.compile(r"^//"),
]
# Lines that open a scope a move may add or remove around moved code.
SCOPE_OPENER = re.compile(r'^(namespace(\s+[\w:]+)?|extern "C(\+\+)?")\s*\{$')
LONE_OPEN = re.compile(r"^\{$")
LONE_CLOSE = re.compile(r"^\}\s*;?\s*(//.*)?$")
LINKAGE = re.compile(r"^(?:(?:static|inline)\s+)+")
# A function definition header: no `=` before the parameter list, which a
# local variable's initializer would have.
DEFINITION_HEADER = re.compile(r"^[^=]*\)\s*(const)?\s*(noexcept)?\s*(override)?\s*\{$")
OUT_OF_LINE = re.compile(r"^([^(]*?)\b(?:[A-Z]\w*::)+(~?\w+\s*\()")
DECLARATION_SPECIFIERS = re.compile(r"^(?:(?:static|inline|virtual|explicit|constexpr|\[\[nodiscard\]\])\s+)+")
ANTI_PATTERNS = [
    (re.compile(r"(?<![\w.])new\s+[\w:<>]+(\s*\[|\s*\(|\s*\{)"), "raw new"),
    (re.compile(r"(?<![\w.])delete(\s*\[\])?\s+\w"), "raw delete"),
    (re.compile(r"\bstd::exit\b"), "std::exit"),
    (re.compile(r"^\s*#\s*define\b"), "#define"),
    (re.compile(r"\bstd::function\b"), "std::function"),
]
TOKEN = re.compile(
    r'R"(?P<delim>[^()\s]*)\((?:.|\n)*?\)(?P=delim)"'  # raw strings
    r'|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    r"|[A-Za-z_]\w*|\d[\w.']*|::|->|<<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||\+\+|--|[-+*/%&|^]=|\S")


def git(*args: str) -> str:
    return subprocess.run(["git", *args], check=True, stdout=subprocess.PIPE,
                          text=True, errors="replace").stdout


def diff_lines(base: str, head: str, paths: list[str]) -> tuple[list[tuple[str, str]], list[tuple[str, str]]]:
    """(file, line) of every removed and added source line, rename detection off
    so that a moved file counts as removed and added."""
    text = git("diff", "--no-renames", "--unified=0", "--no-color", base, head, "--", *paths)
    removed: list[tuple[str, str]] = []
    added: list[tuple[str, str]] = []
    current = ""
    for line in text.split("\n"):
        if line.startswith("+++ ") or line.startswith("--- "):
            name = line[4:]
            if name != "/dev/null":
                current = name[2:]
            continue
        if line.startswith("diff --git"):
            current = line.split(" b/", 1)[-1]
            continue
        if not SOURCE.search(current):
            continue
        if line.startswith("-") and not line.startswith("---"):
            removed.append((current, line[1:]))
        elif line.startswith("+") and not line.startswith("+++"):
            added.append((current, line[1:]))
    return removed, added


def read_renames(path: str | None) -> list[tuple[re.Pattern, str]]:
    renames = []
    if not path:
        return renames
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        old, new = line.split()
        renames.append((re.compile(r"(?<![\w])" + re.escape(old) + r"(?![\w])"), new))
    return renames


def rename(text: str, renames) -> str:
    for pattern, new in renames:
        text = pattern.sub(new, text)
    return text


def canonical(line: str) -> str:
    text = " ".join(line.strip().split())
    unindented = not line[:1].isspace()
    if unindented or DEFINITION_HEADER.match(text):
        text = LINKAGE.sub("", text)
    if unindented:
        # `R Class::member(` -> `R member(`: the qualifier of an out-of-line
        # definition, before its parameter list only.
        text = OUT_OF_LINE.sub(r"\1\2", text, count=1)
    return text


def signature(text: str) -> str | None:
    """The signature of a definition line `R f(args) {` or declaration
    `R f(args);`, without leading declaration specifiers."""
    match = re.match(r"^(.*\))\s*(const)?\s*(noexcept)?\s*(override)?\s*([{;])$", text)
    if not match:
        return None
    return DECLARATION_SPECIFIERS.sub("", " ".join(part for part in match.groups()[:4] if part))


def is_glue(text: str) -> bool:
    return any(pattern.match(text) for pattern in GLUE)


def is_block_brace(text: str) -> bool:
    return bool(LONE_OPEN.match(text) or LONE_CLOSE.match(text) or SCOPE_OPENER.match(text))


def check_braces(left: Counter, right: Counter) -> list[str]:
    """Lone braces balance, up to the closers of added or removed namespace
    (and extern "C") scopes."""
    def count(counter: Counter, pattern: re.Pattern) -> int:
        return sum(n for text, n in counter.items() if pattern.match(text))

    opens = count(right, LONE_OPEN) - count(left, LONE_OPEN)
    closes = count(right, LONE_CLOSE) - count(left, LONE_CLOSE)
    scopes = count(right, SCOPE_OPENER) - count(left, SCOPE_OPENER)
    if opens == 0 and closes == scopes:
        return []
    return [f"block braces: {opens:+d} lone '{{', {closes:+d} lone '}}' against "
            f"{scopes:+d} namespace openers (a new or removed block scope)"]


def check_lines(removed, added, renames) -> list[str]:
    left = Counter(canonical(rename(line, renames)) for _, line in removed)
    right = Counter(canonical(line) for _, line in added)
    definitions = {signature(t) for t in list(left) + list(right) if t.endswith("{")}
    definitions.discard(None)
    residual = check_braces(left, right)
    for text in sorted(set(left) | set(right)):
        if is_glue(text) or is_block_brace(text):
            continue
        if text.endswith(";") and signature(text) in definitions:
            continue  # declaration of a moved definition
        delta = right[text] - left[text]
        if delta:
            residual.append(f"{'+' if delta > 0 else '-'}{abs(delta)}  {text}")
    return residual


def tokens(lines) -> list[str]:
    text = "\n".join(line for line in lines
                     if not is_glue(canonical(line)) and not is_block_brace(canonical(line)))
    text = re.sub(r"//[^\n]*", "", text)
    return [m.group(0) for m in TOKEN.finditer(text)]


def check_tokens(removed, added, renames) -> list[str]:
    removed_lines = [rename(line, renames) for _, line in removed]
    added_lines = [line for _, line in added]
    left = tokens(removed_lines)
    right = tokens(added_lines)
    residual = check_braces(Counter(canonical(line) for line in removed_lines),
                            Counter(canonical(line) for line in added_lines))
    matcher = difflib.SequenceMatcher(a=left, b=right, autojunk=False)
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag == "equal":
            continue
        context = " ".join(left[max(0, i1 - 6):i1])
        residual.append(f"{tag}: -[{' '.join(left[i1:i2])}] +[{' '.join(right[j1:j2])}]  after: {context}")
    return residual


def class_members(header: Path, name: str) -> set[str]:
    """Data members and member functions declared directly in the class body
    (nested bodies, such as inline member function bodies, are skipped)."""
    text = header.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"\b(class|struct)\s+" + re.escape(name) + r"\b[^;{]*\{", text)
    if not match:
        raise SystemExit(f"check_moves.py: class {name} not found in {header}")
    depth, index, top = 1, match.end(), []
    while depth and index < len(text):
        character = text[index]
        if character == "{":
            depth += 1
            if depth == 2:
                top.append("{}")
        elif character == "}":
            depth -= 1
        elif depth == 1:
            top.append(character)
        index += 1
    body = re.sub(r"//[^\n]*", "", "".join(top))
    members = set(re.findall(r"\b(\w+)\s*(?:\{\}|=[^;]*)?;", body))
    members |= set(re.findall(r"\b(~?\w+)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?"
                              r"(?:override\s*)?(?:;|\{\})", body))
    members -= {"if", "for", "while", "switch", "return", "public", "private", "protected", name}
    return members


def check_shadowing(added, specs: list[str]) -> list[str]:
    """Locals declared inside moved bodies (indented lines) whose name is a
    member of the destination class."""
    reports = []
    declaration = re.compile(
        r"^\s+(?:const\s+)?(?:auto|[\w:<>,]+(?:\s*<[^;=(){}]*>)?)\s*[&*]?\s+(\w+)\s*(?:=|\{|;|\((?![^)]*\)\s*(?:const\s*)?\{))")
    for spec in specs:
        header, name = spec.rsplit(":", 1)
        members = class_members(Path(header), name)
        for file, line in added:
            match = declaration.match(line)
            if match and match.group(1) in members and not line.strip().startswith("return"):
                reports.append(f"{file}: local '{match.group(1)}' shadows {name}::{match.group(1)}: {line.strip()}")
    return reports


def check_new_code(added) -> list[str]:
    reports = []
    for file, line in added:
        for pattern, label in ANTI_PATTERNS:
            if pattern.search(line):
                reports.append(f"{file}: {label}: {line.strip()}")
    return reports


# (name, removed lines, added lines, expected to be a pure move). The lines
# are diff lines of two files: `a.cpp` loses them, `b.hpp` gains them.
SELF_TEST = [
    ("a function-local static becomes a plain local (rebuilt every call)",
     ["    static const std::unordered_set<std::string> symbols{"],
     ["    const std::unordered_set<std::string> symbols{"], False),
    ("a local becomes static (state outlives the call)",
     ["        std::size_t start = 0;"],
     ["        static std::size_t start = 0;"], False),
    ("a static local initialized by a call loses static",
     ["    static const auto table = make_table();"],
     ["    const auto table = make_table();"], False),
    ("a call site loses its class qualifier",
     ["    auto name = TypeHelperSet::clone_name(t);"],
     ["    auto name = clone_name(t);"], False),
    ("moved code is wrapped in a new block scope",
     ["    emit(n);", "    finish(n);"],
     ["    {", "    emit(n);", "    finish(n);", "    }"], False),
    ("a block scope around moved code is removed",
     ["    {", "    emit(n);", "    }"],
     ["    emit(n);"], False),
    ("a namespace-scope static function moves into a header as inline",
     ["static std::string helper(int x) {", "    return std::to_string(x);", "}"],
     ["#pragma once", "", "#include <string>", "", "namespace quidra {", "",
      "inline std::string helper(int x) {", "    return std::to_string(x);", "}", "",
      "} // namespace quidra"], True),
    ("an out-of-line member definition moves into its class body",
     ["std::string TypeHelperSet::clone_name(const Type& t) {", "    return name(t);", "}"],
     ["    std::string clone_name(const Type& t) {", "        return name(t);", "    }"], True),
    ("a free static function becomes a static member, declared in its class",
     ["static bool is_small(int x) {", "    return x < 4;", "}"],
     ["    static bool is_small(int x);",
      "bool Limits::is_small(int x) {", "    return x < 4;", "}"], True),
    ("a free function becomes a static member defined in the class",
     ["static bool is_small(int x) {", "    return x < 4;", "}"],
     ["    static bool is_small(int x) {", "        return x < 4;", "    }"], True),
    ("a moved definition gains its declaration in a header",
     ["std::string helper(int x) {", "    return std::to_string(x);", "}"],
     ["std::string helper(int x);", "std::string helper(int x) {",
      "    return std::to_string(x);", "}"], True),
]


def self_test() -> int:
    failures = 0
    for name, removed, added, pure in SELF_TEST:
        removed_lines = [("a.cpp", line) for line in removed]
        added_lines = [("b.hpp", line) for line in added]
        residual = check_lines(removed_lines, added_lines, [])
        if bool(residual) == pure:
            failures += 1
            print(f"check_moves.py: self-test FAILED: {name}: "
                  f"{'pure move' if not residual else residual}")
    print(f"check_moves.py: self-test: {len(SELF_TEST) - failures}/{len(SELF_TEST)} probes "
          "classified as expected")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", default="HEAD^")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--mode", choices=["lines", "tokens"], default="lines")
    parser.add_argument("--rename-map")
    parser.add_argument("--paths", nargs="*", default=[])
    parser.add_argument("--shadow", action="append", default=[], metavar="HEADER:CLASS")
    parser.add_argument("--new-code", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    removed, added = diff_lines(args.base, args.head, args.paths)
    renames = read_renames(args.rename_map)
    residual = (check_lines if args.mode == "lines" else check_tokens)(removed, added, renames)
    shadows = check_shadowing(added, args.shadow)
    anti = check_new_code(added) if args.new_code else []
    print(f"check_moves.py: {len(removed)} removed, {len(added)} added source lines "
          f"({args.mode} mode, {len(renames)} renames)")
    for title, items in (("residual", residual), ("shadowing", shadows), ("anti-pattern", anti)):
        if items:
            print(f"{title} ({len(items)}):")
            for item in items[:400]:
                print("  " + item)
    if residual or shadows or anti:
        return 1
    print("check_moves.py: pure move")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
