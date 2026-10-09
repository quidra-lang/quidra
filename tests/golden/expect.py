"""Expected differences in golden comparisons (README.md, Expected differences).

A language change alters outputs on purpose, so a comparison of its head
with its base cannot ask for byte identity everywhere. `compare.py --expect
FILE` takes expectation files instead: every (entry, view) outside them must
be identical, and inside them every differing line must be explained by a
declared rule. What is left is reported and fails, like any difference.

An expectation file is JSON (rules hold regular expressions, which the TOML
subset cannot spell):

  {
    "rules": [
      {
        "id": "short label, printed in the summary",
        "entries": ["examples/*", "probes/lowering/*.qui"],
        "census": ["InitializedCheck > 0"],
        "census_side": "either",
        "views": ["ir.full", "llvm"],
        "classes": ["K1", {"class": "K3", "kinds": ["call", "store"]}],
        "renames": {"ConstantFloat": "ConstantReal64"},
        "permutations": [{"pattern": "VariantTag\\\\{\\\\d+, (?P<tag>\\\\d+)", "map": {"0": "1", "1": "0"}}],
        "string_lengths": true,
        "new_fields": {"BinSlice": [5, 6], "function": ["c_export_symbol"]},
        "shapes": [{"base": ["regex", ...], "head": ["regex", ...]}],
        "any": false
      }
    ],
    "added": ["probes/lang5a/*"],
    "removed": [],
    "status": [{"entry": "probes/x.qui", "views": ["ir"], "from": "ok", "to": "error:CODE"}],
    "column_maps": ["rewrite.columns.tsv"]
  }

Several files may be given; their rules and lists add up (a feature's
expectation is the union of its commits'). The keys of a rule:

- the pairs it covers (kind a): `entries` (fnmatch patterns of entry names;
  every entry when absent) and `census` (predicates over the entry's census
  view, all of which must hold on `census_side`: base, head or either),
  restricted to `views` (every view when absent);
- `classes` (kind b): the line classes K1-K17 below, each a name or an
  object with the class's parameters;
- token rules (kind c): `renames` (whole tokens of the base renamed before
  the comparison), `permutations` (on base lines matching `pattern`, the
  group `tag` mapped through `map`), `string_lengths` (the lengths of
  `[N x i8]` string constants ignored), `new_fields` (an instruction's
  fields added at the given positions of the head's `Name{...}` in ir.full,
  and named lines added to every function header under `function`), and
  `shapes` (a hunk explained when every removed line matches one of `base`
  and every added line one of `head`: a declared instruction-shape rule);
- `any`: every difference in the covered pairs is accepted and listed for
  review (a declared list of semantic changes, such as Grun's).

Source column maps (kind d) translate the columns of rewritten sources: a
rewriter that inserts or deletes characters writes a map, one edit per line,

  FILE<TAB>LINE<TAB>COLUMN<TAB>DELTA

FILE named as in the corpus (relative to its root, like entry names) or by
absolute path; LINE and COLUMN are 1-based positions in the original source;
a positive DELTA inserts that many characters before COLUMN, a negative one
deletes -DELTA characters starting at COLUMN. Maps come from `column_maps`
(paths relative to the expectation file) and from `compare.py --column-map
FILE`; they apply to every entry whose sources (its `deps` view, or the
entry itself) they name, with or without a rule. A pair of lines is then
equal when every number that differs is a column that follows its line
(`L:C`, `L, C`, `i64 L, i64 C`, `line: L, column: C`) and is the translation
of the base's column, and when the byte offsets of a `diagnostics` span
moved by the edits before it. Text inserted exactly at a column may belong
to what starts there or to what precedes it, so both translations count.
These are the line:column fields of `ir`, `ir.full`, `llvm` (location
immediates, `fail_at` operands, debug records) and `diagnostics`. A
rewritten source also has a new revision (the sha256 of its text, which
`deps` lists and source locations carry): for the sources a map names, the
base's revision is renamed to the head's.

A pair is compared after the token rules of every rule that covers it, and
without the positions that ir.full numbers (an instruction's index in its
block, the numbers of functions, blocks, regions, classes and extensions):
an inserted instruction or function renumbers everything after it, and the
order is kept by the comparison itself. Each hunk that remains is explained
line by line: a pair of removed and added
lines by a pair class (K4, K7, K11, K12, K13 or new_fields), a single line by
a line class of its side (removed lines are judged in the base's context,
added lines in the head's), a whole hunk by a shape. Any other line is K9 and
fails the comparison.

Line classes (5a's classifier; each names the forms the change that
introduces them uses):

  K1  a diagnostic constant definition: `@.msg.*`, `@.err.*`, `@.code.*`
      (and the retired `@.fmt.runtime.error`)
  K2  a prelude declaration of a runtime entry: `quidra_runtime_*`,
      `quidra_bin_*`, `quidra_string_slice`, `quidra_file_failure_message`,
      `quidra_error_*`
  K3  a line of a cold block: a block whose terminator is `unreachable`, or
      that only stores (an error tag and its payload) and branches; with
      `kinds`, only those instruction kinds (`call`, `store`, ...)
  K4  added immediate arguments (`i64 7`) at the calls of `callees`
  K5  a line of a function the other side does not define (an exported
      symbol's entry point, appended after the bodies)
  K6  a `diagnostics` line of one of `codes` (and the count line above them)
  K7  `c_export_symbol` added to a function header; integer fields added to
      `BinSlice`/`StringSlice`
  K8  a new entry (with `entries`, only those)
  K10 the source table: `user_source` records, `user_sources` blocks,
      `@.quidra.sources`, and the `quidra_runtime_register_sources` call;
      the stderr of a failing `run`
  K11 the user-statement and provenance calls (`quidra_runtime_*statement*`,
      `*provenance*`); the location immediates (the trailing `i64 L, i64 C`)
      of a call changing value only
  K12 the line and column values of element index sites (ir.full
      `ArrayGet`, `ArraySet`, `AddressElement`, `StringIndex`,
      `StringIndexAsciiCompare`; llvm `quidra_array_slot`,
      `quidra_fixed_array_slot`, `quidra_string_index*`); the stderr of a
      failing `run`
  K13 two integer fields (line, column) added to an ir.full instruction
  K14 call-site debug metadata: `!dbg !N` attachments, `!DI*` nodes, the
      debug module flags and `!llvm.dbg.cu` are removed from both sides
      before the comparison
  K15 error records: `@.quidra.err.N`, `quidra_runtime_last_reason`,
      `quidra_runtime_fail_error`, `quidra_error_*` calls,
      `@.code.unhandled.error`, `error.message`, `error.make`, `error.code`,
      `error.cause`, `error.detail` (ir.full `ErrorMake`, `ErrorMessage`,
      `ErrorCode`, `ErrorCause`, `ErrorDetail`); any line of a block whose
      label names an error path
  K16 tail records: `quidra_tail_log`, `%tail.*`; every line of the
      functions listed in `functions`
  K17 hop recording: `error.hop` (ir.full `ErrorHop`), `quidra_error_hop`,
      `@.quidra.hop.N`; loads, stores and calls in blocks whose label names
      an error path
"""

from __future__ import annotations

import difflib
import fnmatch
import json
import re
from dataclasses import dataclass, field
from pathlib import Path

# ---------------------------------------------------------------------------
# Line context: functions and blocks of LLVM text (and the labels of `ir`)

DEFINE = re.compile(r"^define\b[^@]*@(\"[^\"]*\"|[\w.$-]+)\(")
LABEL = re.compile(r"^([\w.$-]+):(\s|$)")
UNCONDITIONAL = re.compile(r"^\s*(?:br label %[\w.$-]+|jump [\w.$-]+)\s*$")
OPCODE = re.compile(r"^\s*(?:%[\w.$-]+\s*=\s*)?(?:tail\s+|musttail\s+|notail\s+)?([a-z][\w.]*)")


def opcode(line: str) -> str:
    match = OPCODE.match(line)
    return match.group(1) if match else ""


@dataclass
class LineContext:
    function: str | None = None
    label: str | None = None
    cold: bool = False


def contexts(lines: list[str]) -> list[LineContext]:
    """The enclosing function and block of every line, and whether the
    block is cold (K3)."""
    result = [LineContext() for _ in lines]
    blocks: list[tuple[int, int, str | None, str | None]] = []
    closing: dict[int, str] = {}
    function = None
    start = None
    label = None
    for index, line in enumerate(lines):
        define = DEFINE.match(line)
        if define:
            function = define.group(1)
            start, label = index + 1, "entry"
            continue
        if function is not None and line.startswith("}"):
            if start is not None:
                blocks.append((start, index, function, label))
            closing[index] = function
            function, start, label = None, None, None
            continue
        if function is None:
            # The `ir` view: `function NAME(...)` ... `end`, labels as in LLVM.
            if line.startswith("function "):
                function = line.split()[1].split("(")[0]
                start, label = index + 1, None
            continue
        labelled = LABEL.match(line)
        if labelled:
            if start is not None:
                blocks.append((start, index, function, label))
            start, label = index + 1, labelled.group(1)
        elif line == "end":
            if start is not None:
                blocks.append((start, index, function, label))
            closing[index] = function
            function, start, label = None, None, None
    for begin, end, function, label in blocks:
        body = [lines[i] for i in range(begin, end) if lines[i].strip()]
        cold = False
        if body:
            if opcode(body[-1]) == "unreachable":
                cold = True
            elif UNCONDITIONAL.match(body[-1]):
                others = [opcode(line) for line in body[:-1]]
                cold = "store" in others and all(kind in ("store", "getelementptr")
                                                 for kind in others)
        for i in range(begin, end):
            result[i] = LineContext(function, label, cold)
        if begin > 0 and begin - 1 < len(lines) and LABEL.match(lines[begin - 1]):
            result[begin - 1] = LineContext(function, label, cold)
    for index, line in enumerate(lines):
        if DEFINE.match(line):
            result[index] = LineContext(DEFINE.match(line).group(1), None, False)
    for index, function in closing.items():
        result[index] = LineContext(function, None, False)
    return result


def error_label(label: str | None) -> bool:
    return bool(label) and "error" in label


# ---------------------------------------------------------------------------
# Fields of an ir.full instruction line: `    <i> Name{a, b, T{...}, "s"}`

IR_FULL_INSTRUCTION = re.compile(r"^(\s*(?:\d+ )?)([A-Z][A-Za-z0-9]*)\{(.*)\}$")


def split_fields(body: str) -> list[str] | None:
    fields: list[str] = []
    depth = 0
    quoted = False
    current = []
    index = 0
    while index < len(body):
        char = body[index]
        if quoted:
            current.append(char)
            if char == "\\" and index + 1 < len(body):
                current.append(body[index + 1])
                index += 1
            elif char == '"':
                quoted = False
        elif char == '"':
            quoted = True
            current.append(char)
        elif char in "{[(":
            depth += 1
            current.append(char)
        elif char in "}])":
            depth -= 1
            if depth < 0:
                return None
            current.append(char)
        elif char == "," and depth == 0:
            fields.append("".join(current).strip())
            current = []
        else:
            current.append(char)
        index += 1
    if depth != 0 or quoted:
        return None
    if current or fields:
        fields.append("".join(current).strip())
    return fields


def instruction_fields(line: str) -> tuple[str, str, list[str]] | None:
    match = IR_FULL_INSTRUCTION.match(line)
    if not match:
        return None
    fields = split_fields(match.group(3))
    if fields is None:
        return None
    return match.group(1), match.group(2), fields


INTEGER = re.compile(r"^-?\d+$")


def fields_added(base: str, head: str, names: set[str] | None, count: int | None,
                 positions: list[int] | None = None) -> bool:
    """head is base with integer fields inserted (at `positions` of the head
    when given, else `count` adjacent ones anywhere), for instructions in
    `names` (any when None)."""
    a = instruction_fields(base)
    b = instruction_fields(head)
    if not a or not b or a[1] != b[1] or a[0].strip() != b[0].strip():
        return False
    if names is not None and a[1] not in names:
        return False
    if positions is not None:
        if len(b[2]) != len(a[2]) + len(positions) or any(p >= len(b[2]) for p in positions):
            return False
        kept = [f for i, f in enumerate(b[2]) if i not in set(positions)]
        return kept == a[2]
    extra = len(b[2]) - len(a[2])
    if extra <= 0 or (count is not None and extra != count):
        return False
    for at in range(len(b[2]) - extra + 1):
        inserted = b[2][at:at + extra]
        if all(INTEGER.match(f) for f in inserted) and b[2][:at] + b[2][at + extra:] == a[2]:
            return True
    return False


# ---------------------------------------------------------------------------
# Value-only changes and call arguments

NUMBER_SPLIT = re.compile(r"(-?\d+)")
TRAILING_LOCATION = re.compile(r"^(.*\bcall\b.*), i64 (-?\d+), i64 (-?\d+)\)(.*)$")


def numbers_only(base: str, head: str) -> bool:
    a = NUMBER_SPLIT.split(base)
    b = NUMBER_SPLIT.split(head)
    return len(a) == len(b) and all(x == y for x, y in zip(a[0::2], b[0::2]))


def location_immediates_only(base: str, head: str) -> bool:
    a = TRAILING_LOCATION.match(base)
    b = TRAILING_LOCATION.match(head)
    return bool(a and b) and a.group(1) == b.group(1) and a.group(4) == b.group(4)


CALL = re.compile(r"^(.*\bcall\b[^@]*@)([\w.$-]+)\((.*)\)(.*)$")
IMMEDIATE = re.compile(r"^i\d+ -?\d+$")


def immediates_added(base: str, head: str, callees: set[str] | None) -> bool:
    a = CALL.match(base)
    b = CALL.match(head)
    if not a or not b or a.group(1) != b.group(1) or a.group(2) != b.group(2) or \
            a.group(4) != b.group(4):
        return False
    if callees is not None and a.group(2) not in callees:
        return False
    old = split_fields(a.group(3)) or []
    new = split_fields(b.group(3)) or []
    if len(new) <= len(old):
        return False
    position = 0
    for argument in new:
        if position < len(old) and argument == old[position]:
            position += 1
        elif not IMMEDIATE.match(argument):
            return False
    return position == len(old)


# ---------------------------------------------------------------------------
# Source column maps


class FileEdits:
    """The edits of one rewritten file, by line: (column, delta) in the
    original's coordinates."""

    def __init__(self) -> None:
        self.lines: dict[int, list[tuple[int, int]]] = {}

    def add(self, line: int, column: int, delta: int) -> None:
        self.lines.setdefault(line, []).append((column, delta))

    def translate(self, line: int, column: int, after_insertion: bool) -> int:
        """The column in the rewritten source; text inserted exactly at
        `column` counts before it when `after_insertion`."""
        shift = 0
        for at, delta in sorted(self.lines.get(line, [])):
            if delta > 0:
                if at < column or (at == column and after_insertion):
                    shift += delta
            elif column >= at - delta:
                shift += delta
            elif column >= at:
                shift += at - column
        return column + shift

    def columns(self, line: int, column: int) -> set[int]:
        if line not in self.lines:
            return {column}
        return {self.translate(line, column, False), self.translate(line, column, True)}

    def offsets(self, line: int, column: int, offset: int) -> set[int]:
        before = sum(delta for number, edits in self.lines.items() if number < line
                     for _, delta in edits)
        return {offset + before + translated - column
                for translated in self.columns(line, column)}


class ColumnMaps:
    def __init__(self, files: list[str]):
        self.files: dict[str, FileEdits] = {}
        for name in files:
            for number, raw in enumerate(Path(name).read_text(encoding="utf-8").splitlines(), 1):
                if not raw.strip() or raw.startswith("#"):
                    continue
                parts = raw.split("\t")
                if len(parts) != 4 or not all(INTEGER.match(p) for p in parts[1:]):
                    raise ValueError(f"{name}:{number}: expected FILE<TAB>LINE<TAB>COLUMN<TAB>DELTA")
                line, column, delta = (int(p) for p in parts[1:])
                if line < 1 or column < 1 or delta == 0:
                    raise ValueError(f"{name}:{number}: LINE and COLUMN start at 1, DELTA is not 0")
                self.files.setdefault(parts[0], FileEdits()).add(line, column, delta)

    def names(self, entry: str, path: str) -> bool:
        return any(path == key or path.endswith("/" + key) or (key == entry and path == entry)
                   for key in self.files)

    def for_entry(self, entry: str, sources: list[str]) -> list[FileEdits]:
        found = []
        for key, edits in self.files.items():
            if key == entry or any(path == key or path.endswith("/" + key) for path in sources):
                found.append(edits)
        return found


DEPS_SOURCE = re.compile(r'^source "((?:[^"\\]|\\.)*)" \d+ (\S+)$')
LOCATED = re.compile(r"(?<![\w.])(-?\d+)(?![\w.])")
LOCATION_SEPARATOR = re.compile(r"^(?::|, |, i64 |, column: )$")
DIAGNOSTIC = re.compile(r"^(\S+) (\d+):(\d+)-(\d+):(\d+) @(\d+)-(\d+) (.*)$")


def columns_moved(base: str, head: str, tables: list[FileEdits]) -> bool:
    """head is base with its columns (and diagnostic offsets) translated."""
    a = DIAGNOSTIC.match(base)
    b = DIAGNOSTIC.match(head)
    if a and b:
        if a.group(1) != b.group(1) or a.group(8) != b.group(8):
            return False
        for line, column, offset in ((2, 3, 6), (4, 5, 7)):
            if a.group(line) != b.group(line):
                return False
            l, c, o = int(a.group(line)), int(a.group(column)), int(a.group(offset))
            c2, o2 = int(b.group(column)), int(b.group(offset))
            if (c, o) != (c2, o2) and not any(c2 in t.columns(l, c) and o2 in t.offsets(l, c, o)
                                              for t in tables):
                return False
        return True
    x = LOCATED.split(base)
    y = LOCATED.split(head)
    if len(x) != len(y) or x[0::2] != y[0::2]:
        return False
    numbers_a = x[1::2]
    numbers_b = y[1::2]
    for k, (old, new) in enumerate(zip(numbers_a, numbers_b)):
        if old == new:
            continue
        if k == 0 or numbers_a[k - 1] != numbers_b[k - 1] or \
                not LOCATION_SEPARATOR.match(x[2 * k]):
            return False
        line = int(numbers_a[k - 1])
        if not any(int(new) in t.columns(line, int(old)) for t in tables):
            return False
    return True


# ---------------------------------------------------------------------------
# Classes

K1 = re.compile(r"^@\.(?:msg|err|code|fmt\.runtime)\b[\w.$-]* = ")
K2 = re.compile(r"^declare\b.*@(?:quidra_runtime_\w+|quidra_bin_\w+|quidra_string_slice|"
                r"quidra_file_failure_message|quidra_error_\w+)\(")
K10 = re.compile(r"\buser_sources?\b|@\.quidra\.sources\b|@quidra_runtime_register_sources\b")
K11 = re.compile(r"@quidra_runtime_\w*(?:statement|provenance)\w*\b")
K12_IR = re.compile(r"^\s*(?:\d+ )?(?:ArrayGet|ArraySet|AddressElement|StringIndex|"
                    r"StringIndexAsciiCompare)\{")
K12_LLVM = re.compile(r"@(?:quidra_array_slot|quidra_fixed_array_slot|quidra_string_index\w*)\(")
K14_ATTACHMENT = re.compile(r",? !dbg !\d+")
K14_LINE = re.compile(r"^!llvm\.dbg\.cu\s*=|^!\d+ = (?:distinct )?!DI|"
                      r"^!\d+ = !\{i32 \d+, !\"(?:Debug Info Version|Dwarf Version)\"")
K15 = re.compile(r"@\.quidra\.err\.\d+|\bquidra_runtime_last_reason\b|"
                 r"\bquidra_runtime_fail_error\b|@quidra_error_\w+|@\.code\.unhandled\.error\b|"
                 r"\berror\.(?:message|make|code|cause|detail)\b|"
                 r"\bError(?:Make|Message|Code|Cause|Detail)\{")
K16 = re.compile(r"\bquidra_tail_log\b|%tail\.")
K17 = re.compile(r"\berror\.hop\b|\bErrorHop\{|\bquidra_error_hop\b|@\.quidra\.hop\.\d+")
IR_FULL_POSITION = re.compile(r"^(\s+)\d+ (?=[A-Z][A-Za-z0-9]*\{)|"
                              r"^(\s*(?:function|class|extension|block|region) #)\d+(?= )")
STRING_LENGTH = re.compile(r"\[\d+ x i8\]")
CLASS_NAMES = {f"K{n}" for n in range(1, 18)} - {"K9"}
DIAGNOSTIC_HEADER = re.compile(r"^(?:CompileErrors count=\d+ truncated=[01]|CompileError)$")
RUN_STDERR = re.compile(r"^stderr \d+ bytes$")


@dataclass
class Side:
    """One side of a differing pair: its lines and their context."""
    lines: list[str]
    contexts: list[LineContext]
    functions: set[str]
    view: str
    stderr_from: int | None
    failing_run: bool

    @staticmethod
    def of(lines: list[str], view: str) -> "Side":
        stderr_from = None
        failing = False
        if view == "run":
            for index, line in enumerate(lines):
                if RUN_STDERR.match(line):
                    stderr_from = index
                    break
            failing = bool(lines) and lines[0] != "exit 0"
        found = contexts(lines)
        return Side(lines, found, {c.function for c in found if c.function}, view,
                    stderr_from, failing)


# ---------------------------------------------------------------------------
# Rules


def as_list(value) -> list:
    if value is None:
        return []
    return value if isinstance(value, list) else [value]


@dataclass
class Rule:
    id: str
    entries: list[str] | None
    census: list[str]
    census_side: str
    views: set[str] | None
    classes: dict[str, dict]
    renames: dict[str, str]
    permutations: list[tuple[re.Pattern, dict[str, str]]]
    string_lengths: bool
    new_fields: dict[str, list]
    shapes: list[tuple[list[re.Pattern], list[re.Pattern]]]
    any: bool

    @staticmethod
    def parse(data: dict, origin: str) -> "Rule":
        known = {"id", "entries", "census", "census_side", "views", "classes", "renames",
                 "permutations", "string_lengths", "new_fields", "shapes", "any"}
        unknown = sorted(set(data) - known)
        if unknown:
            raise ValueError(f"{origin}: unknown rule keys {unknown}")
        classes: dict[str, dict] = {}
        for item in as_list(data.get("classes")):
            name, parameters = (item, {}) if isinstance(item, str) else (item.get("class"), item)
            if name not in CLASS_NAMES:
                raise ValueError(f"{origin}: unknown class {name!r}")
            classes[name] = {k: v for k, v in parameters.items() if k != "class"}
        side = data.get("census_side", "either")
        if side not in ("base", "head", "either"):
            raise ValueError(f"{origin}: census_side must be base, head or either")
        return Rule(
            id=str(data.get("id", origin)),
            entries=as_list(data["entries"]) if "entries" in data else None,
            census=as_list(data.get("census")),
            census_side=side,
            views=set(as_list(data["views"])) if "views" in data else None,
            classes=classes,
            renames=dict(data.get("renames", {})),
            permutations=[(re.compile(p["pattern"]), {str(k): str(v) for k, v in p["map"].items()})
                          for p in as_list(data.get("permutations"))],
            string_lengths=bool(data.get("string_lengths", False)),
            new_fields=dict(data.get("new_fields", {})),
            shapes=[([re.compile(r) for r in as_list(s.get("base"))],
                     [re.compile(r) for r in as_list(s.get("head"))])
                    for s in as_list(data.get("shapes"))],
            any=bool(data.get("any", False)),
        )


CENSUS_PREDICATE = re.compile(r"^\s*([\w.$:-]+)\s*(>=|<=|==|!=|>|<)\s*(-?\d+)\s*$")


def census_values(text: str) -> dict[str, int]:
    """Named counts of a census view: an alternative's optimized count by
    its name and the lowered count as NAME.lowered, the sum of each event
    kind as event.KIND, and `KIND KEY ... COUNT` rows as KIND.KEY."""
    values: dict[str, int] = {}
    for line in text.splitlines():
        parts = line.split(" ")
        if parts[0] == "alt" and len(parts) == 4:
            name = parts[1].split(":", 1)[-1]
            values[name + ".lowered"] = int(parts[2])
            values[name] = int(parts[3])
        elif parts[0] == "event" and len(parts) >= 3 and INTEGER.match(parts[-1]):
            values[f"event.{parts[1]}"] = values.get(f"event.{parts[1]}", 0) + int(parts[-1])
        elif parts[0] not in ("sig", "stages") and len(parts) >= 3 and INTEGER.match(parts[-1]):
            key = f"{parts[0]}.{parts[1]}"
            values[key] = values.get(key, 0) + int(parts[-1])
    return values


def holds(predicate: str, values: dict[str, int]) -> bool:
    match = CENSUS_PREDICATE.match(predicate)
    if not match:
        raise ValueError(f"census predicate {predicate!r}: expected `NAME OP INTEGER`")
    name, op, number = match.group(1), match.group(2), int(match.group(3))
    value = values.get(name, 0)
    return {">": value > number, ">=": value >= number, "<": value < number,
            "<=": value <= number, "==": value == number, "!=": value != number}[op]


@dataclass
class Verdict:
    explained: bool
    classes: dict[str, int] = field(default_factory=dict)
    rules: list[str] = field(default_factory=list)
    residual: list[str] = field(default_factory=list)
    accepted_any: bool = False


class Expectations:
    def __init__(self, files: list[str], column_maps: list[str] | None = None):
        self.rules: list[Rule] = []
        self.added: list[str] = []
        self.removed: list[str] = []
        self.status: list[dict] = []
        maps = list(column_maps or [])
        for name in files:
            data = json.loads(Path(name).read_text(encoding="utf-8"))
            known = {"rules", "added", "removed", "status", "column_maps"}
            unknown = sorted(set(data) - known)
            if unknown:
                raise ValueError(f"{name}: unknown keys {unknown}")
            for number, rule in enumerate(as_list(data.get("rules"))):
                self.rules.append(Rule.parse(rule, f"{name}#{number}"))
            self.added += as_list(data.get("added"))
            self.removed += as_list(data.get("removed"))
            for item in as_list(data.get("status")):
                if "entry" not in item or set(item) - {"entry", "views", "from", "to"}:
                    raise ValueError(f"{name}: a status item has `entry` and optional "
                                     f"`views`, `from`, `to`: {item}")
                self.status.append(item)
            maps += [str(Path(name).parent / path) for path in as_list(data.get("column_maps"))]
        self.column_maps = ColumnMaps(maps) if maps else None
        self.census_cache: dict[tuple[str, str], dict[str, int]] = {}

    # -- coverage ------------------------------------------------------------

    def census(self, capture: Path, entry: str) -> dict[str, int]:
        key = (str(capture), entry)
        if key not in self.census_cache:
            path = capture / "out" / entry / "census"
            self.census_cache[key] = census_values(path.read_text(encoding="utf-8")) \
                if path.exists() else {}
        return self.census_cache[key]

    def covering(self, entry: str, view: str, base: Path, head: Path) -> list[Rule]:
        rules = []
        for rule in self.rules:
            if rule.views is not None and view not in rule.views:
                continue
            if rule.entries is not None and not any(fnmatch.fnmatchcase(entry, pattern)
                                                    for pattern in rule.entries):
                continue
            if rule.census:
                sides = {"base": [base], "head": [head], "either": [base, head]}[rule.census_side]
                if not any(all(holds(p, self.census(side, entry)) for p in rule.census)
                           for side in sides):
                    continue
            rules.append(rule)
        return rules

    def entry_added(self, entry: str, view: str, base: Path, head: Path) -> bool:
        if any(fnmatch.fnmatchcase(entry, pattern) for pattern in self.added):
            return True
        for rule in self.covering(entry, view, base, head):
            if "K8" in rule.classes:
                patterns = as_list(rule.classes["K8"].get("entries")) or ["*"]
                if any(fnmatch.fnmatchcase(entry, pattern) for pattern in patterns):
                    return True
        return False

    def entry_removed(self, entry: str) -> bool:
        return any(fnmatch.fnmatchcase(entry, pattern) for pattern in self.removed)

    def status_expected(self, entry: str, view: str, before: str, after: str) -> bool:
        for item in self.status:
            if not fnmatch.fnmatchcase(entry, item["entry"]):
                continue
            if "views" in item and view not in as_list(item["views"]):
                continue
            if "from" in item and item["from"] != before:
                continue
            if "to" in item and item["to"] != after:
                continue
            return True
        return False

    # -- comparison ----------------------------------------------------------

    def sources(self, capture: Path, entry: str) -> dict[str, str]:
        """The entry's sources and their sha256, from its deps view."""
        path = capture / "out" / entry / "deps"
        if not path.exists():
            return {}
        found = {}
        for line in path.read_text(encoding="utf-8").splitlines():
            match = DEPS_SOURCE.match(line)
            if match:
                name = match.group(1).replace('\\"', '"').replace("\\\\", "\\")
                found[name] = match.group(2)
        return found

    def check(self, entry: str, view: str, base_text: str, head_text: str,
              base: Path, head: Path) -> Verdict:
        rules = self.covering(entry, view, base, head)
        tables: list[FileEdits] = []
        revisions: dict[str, str] = {}
        if self.column_maps:
            head_sources = self.sources(head, entry)
            tables = self.column_maps.for_entry(entry, list(head_sources))
            for name, digest in self.sources(base, entry).items():
                if head_sources.get(name, digest) != digest and \
                        self.column_maps.names(entry, name):
                    revisions[digest] = head_sources[name]
        if not rules and not tables:
            return Verdict(False, residual=["(no rule covers this entry and view)"])
        ids = [rule.id for rule in rules] + (["column map"] if tables else [])
        if any(rule.any for rule in rules):
            return Verdict(True, {"any": 1}, ids, accepted_any=True)
        merged = merge(rules, revisions)
        merged.columns = tables
        a = unnumbered(base_text.splitlines())
        b = unnumbered(head_text.splitlines())
        if "K14" in merged.classes:
            a, b = strip_debug(a), strip_debug(b)
        a = [merged.transform_base(line) for line in a]
        if merged.string_lengths:
            a = [STRING_LENGTH.sub("[N x i8]", line) for line in a]
            b = [STRING_LENGTH.sub("[N x i8]", line) for line in b]
        verdict = Verdict(True, {}, ids)
        if a == b:
            verdict.classes["token rules"] = 1
            return verdict
        left = Side.of(a, view)
        right = Side.of(b, view)
        matcher = difflib.SequenceMatcher(None, a, b, autojunk=False)
        for tag, i1, i2, j1, j2 in matcher.get_opcodes():
            if tag == "equal":
                continue
            merged.explain_hunk(left, right, list(range(i1, i2)), list(range(j1, j2)), verdict)
        verdict.explained = not verdict.residual
        return verdict


def unnumbered(lines: list[str]) -> list[str]:
    """ir.full lines without the positions it numbers."""
    return [IR_FULL_POSITION.sub(lambda m: m.group(1) or m.group(2), line) for line in lines]


def strip_debug(lines: list[str]) -> list[str]:
    result = []
    for line in lines:
        if K14_LINE.match(line):
            continue
        result.append(K14_ATTACHMENT.sub("", line))
    return result


@dataclass
class Merged:
    classes: dict[str, dict]
    renames: dict[str, str]
    permutations: list[tuple[re.Pattern, dict[str, str]]]
    string_lengths: bool
    new_fields: dict[str, list]
    shapes: list[tuple[list[re.Pattern], list[re.Pattern]]]
    rename_pattern: re.Pattern | None = None

    def transform_base(self, line: str) -> str:
        if self.rename_pattern is not None:
            line = self.rename_pattern.sub(lambda m: self.renames[m.group(0)], line)
        for pattern, mapping in self.permutations:
            match = pattern.search(line)
            if match and "tag" in pattern.groupindex and match.group("tag") in mapping:
                start, end = match.span("tag")
                line = line[:start] + mapping[match.group("tag")] + line[end:]
        return line

    # -- pairs -----------------------------------------------------------

    def pair_class(self, base: str, head: str, left: Side, i: int, right: Side, j: int) -> str | None:
        if self.columns and columns_moved(base, head, self.columns):
            return "columns"
        for name, positions in self.new_fields.items():
            if name != "function" and fields_added(base, head, {name}, None, list(positions)):
                return "new_fields"
        if "K4" in self.classes:
            callees = self.classes["K4"].get("callees")
            if immediates_added(base, head, set(callees) if callees is not None else None):
                return "K4"
        if "K7" in self.classes and fields_added(base, head, {"BinSlice", "StringSlice"}, None):
            return "K7"
        if "K13" in self.classes and fields_added(base, head, None, 2):
            return "K13"
        if "K12" in self.classes and numbers_only(base, head) and (
                K12_IR.match(base) or K12_LLVM.search(base)):
            return "K12"
        if "K11" in self.classes and location_immediates_only(base, head):
            return "K11"
        for name in ("K10", "K12"):
            if name in self.classes and in_failing_stderr(left, i) and in_failing_stderr(right, j):
                return name
        return None

    def line_class(self, side: Side, index: int, added: bool) -> str | None:
        line = side.lines[index]
        context = side.contexts[index]
        classes = self.classes
        if "K1" in classes and K1.match(line):
            return "K1"
        if "K2" in classes and K2.match(line):
            return "K2"
        if "K10" in classes and (K10.search(line) or in_failing_stderr(side, index)):
            return "K10"
        if "K11" in classes and K11.search(line):
            return "K11"
        if "K12" in classes and in_failing_stderr(side, index):
            return "K12"
        if "K15" in classes and (K15.search(line) or error_label(context.label)):
            return "K15"
        if "K16" in classes and (K16.search(line) or context.function in
                                 set(as_list(classes["K16"].get("functions")))):
            return "K16"
        if "K17" in classes and (K17.search(line) or (
                error_label(context.label) and opcode(line) in ("load", "store", "call"))):
            return "K17"
        if "K6" in classes:
            codes = set(as_list(classes["K6"].get("codes")))
            first = line.split(" ", 1)[0]
            if (first in codes) or DIAGNOSTIC_HEADER.match(line):
                return "K6"
        if "K7" in classes and added and re.match(r"^\s+c_export_symbol\b", line):
            return "K7"
        if added and self.new_fields.get("function"):
            for name in self.new_fields["function"]:
                if re.match(rf"^\s+{re.escape(name)}\b", line):
                    return "new_fields"
        if "K3" in classes and context.cold:
            kinds = classes["K3"].get("kinds")
            if kinds is None or opcode(line) in set(kinds):
                return "K3"
        if "K5" in classes and context.function is not None:
            other = self.other_functions
            if context.function not in other:
                return "K5"
        return None

    other_functions: set[str] = field(default_factory=set)
    columns: list[FileEdits] = field(default_factory=list)

    def explain_hunk(self, left: Side, right: Side, removed: list[int], added: list[int],
                     verdict: Verdict) -> None:
        # A declared shape explains the whole hunk.
        for base_patterns, head_patterns in self.shapes:
            if all(any(p.search(left.lines[i]) for p in base_patterns) for i in removed) and \
                    all(any(p.search(right.lines[j]) for p in head_patterns) for j in added):
                count(verdict, "shape", len(removed) + len(added))
                return
        paired_left: set[int] = set()
        paired_right: set[int] = set()
        for i, j, name in pair_lines(self, left, right, removed, added):
            paired_left.add(i)
            paired_right.add(j)
            count(verdict, name, 2)
        for side, indices, paired, is_added, other in (
                (left, removed, paired_left, False, right),
                (right, added, paired_right, True, left)):
            self.other_functions = other.functions
            for index in indices:
                if index in paired:
                    continue
                name = self.line_class(side, index, is_added)
                if name is None:
                    sign = "+" if is_added else "-"
                    verdict.residual.append(f"{sign} {side.lines[index][:300]}")
                else:
                    count(verdict, name, 1)


def in_failing_stderr(side: Side, index: int) -> bool:
    return side.view == "run" and side.failing_run and side.stderr_from is not None and \
        index >= side.stderr_from


def count(verdict: Verdict, name: str, lines: int) -> None:
    verdict.classes[name] = verdict.classes.get(name, 0) + lines


def pair_lines(merged: Merged, left: Side, right: Side, removed: list[int],
               added: list[int]) -> list[tuple[int, int, str]]:
    """The longest in-order pairing of removed and added lines that a pair
    class explains (greedy for very large hunks)."""
    if not removed or not added:
        return []
    if len(removed) * len(added) > 250_000:
        pairs = []
        position = 0
        for i in removed:
            for k in range(position, len(added)):
                j = added[k]
                name = merged.pair_class(left.lines[i], right.lines[j], left, i, right, j)
                if name:
                    pairs.append((i, j, name))
                    position = k + 1
                    break
        return pairs
    names: dict[tuple[int, int], str] = {}
    rows, columns = len(removed), len(added)
    table = [[0] * (columns + 1) for _ in range(rows + 1)]
    for x in range(rows - 1, -1, -1):
        for y in range(columns - 1, -1, -1):
            i, j = removed[x], added[y]
            name = merged.pair_class(left.lines[i], right.lines[j], left, i, right, j)
            best = max(table[x + 1][y], table[x][y + 1])
            if name and table[x + 1][y + 1] + 1 > best:
                names[(x, y)] = name
                best = table[x + 1][y + 1] + 1
            table[x][y] = best
    pairs = []
    x = y = 0
    while x < rows and y < columns:
        if (x, y) in names and table[x][y] == table[x + 1][y + 1] + 1:
            pairs.append((removed[x], added[y], names[(x, y)]))
            x += 1
            y += 1
        elif table[x + 1][y] >= table[x][y + 1]:
            x += 1
        else:
            y += 1
    return pairs


def merge(rules: list[Rule], renamed: dict[str, str] | None = None) -> Merged:
    classes: dict[str, dict] = {}
    renames: dict[str, str] = dict(renamed or {})
    permutations = []
    new_fields: dict[str, list] = {}
    shapes = []
    for rule in rules:
        for name, parameters in rule.classes.items():
            if name not in classes:
                classes[name] = dict(parameters)
                continue
            # Two rules naming one class: their lists add up, and a class
            # without a restriction stays unrestricted.
            for key, value in parameters.items():
                if key not in classes[name]:
                    continue
                classes[name][key] = sorted(set(as_list(classes[name][key])) | set(as_list(value)))
            for key in list(classes[name]):
                if key not in parameters:
                    del classes[name][key]
        renames.update(rule.renames)
        permutations += rule.permutations
        for name, positions in rule.new_fields.items():
            new_fields[name] = list(positions)
        shapes += rule.shapes
    pattern = None
    if renames:
        alternatives = "|".join(re.escape(old) for old in sorted(renames, key=len, reverse=True))
        pattern = re.compile(rf"(?<![A-Za-z0-9_])(?:{alternatives})(?![A-Za-z0-9_])")
    return Merged(classes, renames, permutations, any(r.string_lengths for r in rules),
                  new_fields, shapes, pattern)
