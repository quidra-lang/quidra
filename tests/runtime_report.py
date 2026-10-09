#!/usr/bin/env python3
"""Split a Quidra runtime failure report into its fields and check them.

The report (docs/spec/diagnostics.md, "Runtime failure format"):

    Quidra runtime error[CODE] at FILE:L:C
                                      (empty line; snippet only)
    L | <source line L>               (snippet only)
      | <padding>^                    (snippet only)
      | <message line>                (one per line of the message)

Without the snippet the message follows the header in a zero-width gutter
("| message"). A failure with no site names the file alone ("at FILE"); an
uncoded runtime-library failure has no "[CODE]"; a test assertion's header is
"Quidra test assertion failed at FILE:L:C" and it has no message; only a
failure before any source table is registered keeps the one-line
"Quidra runtime error[CODE]: message".

Usage as a check: runtime_report.py STDERR_FILE [field=value ...]
Fields: kind (runtime_error, test_assertion), code ("" for none), file,
line, column, snippet (yes/no), source (the snippet's source line),
caret (the byte column the caret stands under), message (the message, its
lines joined with LF), provenance (the bracketed provenance suffix that ends
the last line, if any). A value given as /regex/ is matched as
a regular expression with re.search.
"""

import re
import sys
import unicodedata

HEADER = re.compile(
    r"^Quidra runtime error(?:\[(?P<code>[A-Z][A-Z0-9_]*)\])? at (?P<file>.+?):(?P<line>[0-9]+):(?P<column>[0-9]+)$")
HEADER_NO_SITE = re.compile(r"^Quidra runtime error(?:\[(?P<code>[A-Z][A-Z0-9_]*)\])? at (?P<file>.+)$")
HEADER_ONE_LINE = re.compile(r"^Quidra runtime error(?:\[(?P<code>[A-Z][A-Z0-9_]*)\])?: (?P<message>.*)$")
HEADER_ASSERTION = re.compile(
    r"^Quidra test assertion failed at (?P<file>.+?):(?P<line>[0-9]+):(?P<column>[0-9]+)(?P<rest>( \[.*)?)$")
GUTTER = re.compile(r"^(?P<number>[0-9]+) \| (?P<source>.*)$")
CARET = re.compile(r"^(?P<gutter> +) \| (?P<padding>[ \t]*)\^(?P<rest>.*)$")


def display_width(text):
    """Display columns of `text` by the rule the runtime places the caret with."""
    width = 0
    for ch in text:
        cp = ord(ch)
        if cp == 0xAD:
            width += 1
        elif 0x1160 <= cp <= 0x11FF or cp == 0x200B or unicodedata.category(ch) in ("Mn", "Me", "Cf"):
            pass
        elif unicodedata.east_asian_width(ch) in ("W", "F"):
            width += 2
        else:
            width += 1
    return width


def caret_column(source, padding):
    """The 1-based byte column whose bytes before it the padding covers."""
    # A tab is copied as a tab: one column on both lines.
    target = padding.replace("\t", " ")
    columns = 0
    byte = 0
    for ch in source:
        if columns >= len(target):
            break
        columns += 1 if ch == "\t" else display_width(ch)
        byte += len(ch.encode("utf-8"))
    return byte + 1 + max(0, len(target) - columns)


SUFFIX = re.compile(r" (\[source_revision=[^\]]*\])$")


def split_provenance(report):
    """Moves the provenance suffix that ends the last line into its field."""
    for field in ("message", "file"):
        match = SUFFIX.search(report[field])
        if match:
            report["provenance"] = match.group(1)
            report[field] = report[field][:match.start()]
            return


def parse(text):
    """The fields of the first report in `text` (None without one)."""
    lines = text.split("\n")
    for index, line in enumerate(lines):
        if line.startswith("Quidra runtime error") or line.startswith("Quidra test assertion failed"):
            break
    else:
        return None
    report = {"kind": "runtime_error", "code": "", "file": "", "line": "", "column": "",
              "snippet": "no", "source": "", "caret": "", "message": "", "provenance": ""}
    header = lines[index]
    rest = lines[index + 1:]
    assertion = HEADER_ASSERTION.match(header)
    if assertion:
        report.update(kind="test_assertion", file=assertion["file"], line=assertion["line"],
                      column=assertion["column"])
        if SUFFIX.search(assertion["rest"]):
            report["provenance"] = SUFFIX.search(assertion["rest"]).group(1)
    elif (match := HEADER.match(header)):
        report.update(code=match["code"] or "", file=match["file"], line=match["line"],
                      column=match["column"])
    elif (match := HEADER_ONE_LINE.match(header)):
        report.update(code=match["code"] or "", message=match["message"])
        return report
    elif (match := HEADER_NO_SITE.match(header)):
        report.update(code=match["code"] or "", file=match["file"])
    else:
        return None
    gutter = 0
    if len(rest) >= 3 and rest[0] == "" and GUTTER.match(rest[1]) and CARET.match(rest[2]):
        number = GUTTER.match(rest[1])
        caret = CARET.match(rest[2])
        if number["number"] == report["line"] and len(caret["gutter"]) == len(number["number"]):
            report["snippet"] = "yes"
            report["source"] = number["source"]
            report["caret"] = str(caret_column(number["source"], caret["padding"]))
            if SUFFIX.search(caret["rest"]):
                report["provenance"] = SUFFIX.search(caret["rest"]).group(1)
            gutter = len(caret["gutter"]) + 1
            rest = rest[3:]
    body = re.compile(r"^" + " " * gutter + r"\|(?: (?P<text>.*))?$")
    message = []
    for line in rest:
        match = body.match(line)
        if not match:
            break
        if match["text"] is None:
            break
        message.append(match["text"])
    report["message"] = "\n".join(message)
    split_provenance(report)
    return report


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    text = open(argv[1], encoding="utf-8", errors="replace").read()
    report = parse(text)
    if report is None:
        print(f"runtime_report: no report in {argv[1]}:\n{text}", file=sys.stderr)
        return 1
    failed = False
    for item in argv[2:]:
        name, _, expected = item.partition("=")
        actual = report.get(name)
        if actual is None:
            print(f"runtime_report: unknown field {name}", file=sys.stderr)
            return 2
        if len(expected) >= 2 and expected.startswith("/") and expected.endswith("/"):
            ok = re.search(expected[1:-1], actual) is not None
        else:
            ok = actual == expected
        if not ok:
            print(f"runtime_report: {name} is {actual!r}, expected {expected!r}", file=sys.stderr)
            failed = True
    if failed:
        print(text, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
