"""Parser for the TOML subset used by Quidra project metadata.

The subset is deliberately tiny and strictly enforced: `src/toml_subset.cpp`
parses the same grammar in the compiler, and keeping both readers to one small
grammar is what makes a `project.toml` written for one readable by the other.

Accepted:
    # full-line comment
    [table]
    [[array_of_tables]]
    key = "string"
    key = 12
    key = true
    key = ["a", "b"]

Everything else - inline comments, nested tables, floats, dates, multi-line
strings, bare keys on the right-hand side - is a parse error rather than a
silent misreading.
"""

from __future__ import annotations


class TomlSubsetError(ValueError):
    pass


def _fail(line_number: int, message: str) -> "TomlSubsetError":
    return TomlSubsetError(f"line {line_number}: {message}")


def _parse_string(text: str, line_number: int) -> str:
    if len(text) < 2 or not text.startswith('"') or not text.endswith('"'):
        raise _fail(line_number, f"expected a double-quoted string: {text}")
    body = text[1:-1]
    if '"' in body or "\\" in body:
        raise _fail(line_number, "strings may not contain quotes or backslashes")
    return body


def _split_array(body: str, line_number: int) -> list[str]:
    if not body.strip():
        return []
    items = [item.strip() for item in body.split(",")]
    if items and items[-1] == "":
        items.pop()
    if any(item == "" for item in items):
        raise _fail(line_number, "array has an empty element")
    return items


def _parse_value(text: str, line_number: int):
    if text.startswith("["):
        if not text.endswith("]"):
            raise _fail(line_number, "array must close on the same line")
        return [
            _parse_string(item, line_number)
            for item in _split_array(text[1:-1], line_number)
        ]
    if text.startswith('"'):
        return _parse_string(text, line_number)
    if text in ("true", "false"):
        return text == "true"
    if text.isdigit() or (text.startswith("-") and text[1:].isdigit()):
        return int(text)
    raise _fail(line_number, f"unsupported value: {text}")


def loads(text: str) -> dict:
    document: dict = {}
    table: dict = document
    for line_number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue

        if line.startswith("[["):
            if not line.endswith("]]"):
                raise _fail(line_number, "unterminated [[array of tables]] header")
            name = line[2:-2].strip()
            if not name:
                raise _fail(line_number, "empty [[array of tables]] header")
            table = {}
            document.setdefault(name, []).append(table)
            continue

        if line.startswith("["):
            if not line.endswith("]"):
                raise _fail(line_number, "unterminated [table] header")
            name = line[1:-1].strip()
            if not name:
                raise _fail(line_number, "empty [table] header")
            if name in document:
                raise _fail(line_number, f"duplicate table: {name}")
            table = {}
            document[name] = table
            continue

        key, separator, value = line.partition("=")
        if not separator:
            raise _fail(line_number, "expected 'key = value'")
        key = key.strip()
        if not key:
            raise _fail(line_number, "empty key")
        if table is document:
            raise _fail(line_number, f"key outside any table: {key}")
        if key in table:
            raise _fail(line_number, f"duplicate key: {key}")
        table[key] = _parse_value(value.strip(), line_number)

    return document


def load(path) -> dict:
    with open(path, encoding="utf-8") as handle:
        return loads(handle.read())
