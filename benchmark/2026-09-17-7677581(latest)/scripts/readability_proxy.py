#!/usr/bin/env python3
"""Reference implementation of the Readability proxy of methodology/05_standard_rubrics.json.

This script is the ONLY admissible implementation of that metric's
measurement_definitions. It is run unchanged for all 10 languages and its output
is the evidence. It contains NO per-language logic: every per-language fact
(file extensions, comment syntax, literal syntax, block model, routine-header
regexes) is read from the frozen_lexing_table inside the methodology document,
so the document and the script cannot diverge.

Outputs, per language:
  code_lines                       number of code lines in the corpus
  line_length_median / _p95        R3
  nesting_depth_median / _p95      R4
  routine_length_median / _p95     R6
  sigil_density                    R5 (measured and reported, NOT scored)

Usage:
  readability_proxy.py --rubrics <05_standard_rubrics.json> \
                       --language <name> \
                       --corpus <dir> [--corpus <dir> ...] \
                       [--json]

Exit status is 0 on success, 2 on a usage or data error.
"""

import argparse
import json
import math
import os
import re
import sys

CODE = 0
COMMENT = 1
LITERAL = 2

WORD_CHARS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_")


# ---------------------------------------------------------------------------
# Lexing: classify every character of every line as CODE, COMMENT or LITERAL.
# ---------------------------------------------------------------------------

def _match_at(line, i, token):
    return token and line.startswith(token, i)


def classify_file(lines, spec):
    """Return a list of per-line class arrays (one entry per character).

    A single left-to-right scan with carried state. At any position the scanner
    is either inside a block comment, inside a multi-line literal, or in code.
    In code, the tokens are tested in a fixed order at each position: line
    comment, block comment, string literal openers (longest first), character
    literal. Because the scan is left to right, a comment token appearing
    inside a literal is consumed as literal and a quote appearing inside a
    comment is consumed as comment, with no special casing.
    """
    line_comments = sorted(spec.get("line_comment") or [], key=len, reverse=True)
    block_comments = spec.get("block_comment") or []
    strings = sorted(
        spec.get("string_literals") or [], key=lambda s: len(s["open"]), reverse=True
    )
    chars = spec.get("char_literals") or []

    state = None  # None | ("block", cfg, depth) | ("lit", cfg)
    out = []

    for line in lines:
        klass = [CODE] * len(line)
        i = 0
        n = len(line)
        while i < n:
            if state is not None and state[0] == "block":
                _, cfg, depth = state
                if cfg.get("nestable") and _match_at(line, i, cfg["open"]):
                    for k in range(i, min(n, i + len(cfg["open"]))):
                        klass[k] = COMMENT
                    i += len(cfg["open"])
                    state = ("block", cfg, depth + 1)
                    continue
                if _match_at(line, i, cfg["close"]):
                    for k in range(i, min(n, i + len(cfg["close"]))):
                        klass[k] = COMMENT
                    i += len(cfg["close"])
                    state = None if depth <= 1 else ("block", cfg, depth - 1)
                    continue
                klass[i] = COMMENT
                i += 1
                continue

            if state is not None and state[0] == "lit":
                cfg = state[1]
                esc = cfg.get("escape")
                if esc and _match_at(line, i, esc):
                    klass[i] = LITERAL
                    if i + 1 < n:
                        klass[i + 1] = LITERAL
                    i += 2
                    continue
                if cfg["close"] != "\n" and _match_at(line, i, cfg["close"]):
                    for k in range(i, min(n, i + len(cfg["close"]))):
                        klass[k] = LITERAL
                    i += len(cfg["close"])
                    state = None
                    continue
                klass[i] = LITERAL
                i += 1
                continue

            # --- in code ---
            hit = False
            for tok in line_comments:
                if _match_at(line, i, tok):
                    for k in range(i, n):
                        klass[k] = COMMENT
                    i = n
                    hit = True
                    break
            if hit:
                continue

            for cfg in block_comments:
                if _match_at(line, i, cfg["open"]):
                    for k in range(i, min(n, i + len(cfg["open"]))):
                        klass[k] = COMMENT
                    i += len(cfg["open"])
                    state = ("block", cfg, 1)
                    hit = True
                    break
            if hit:
                continue

            for cfg in strings:
                if _match_at(line, i, cfg["open"]):
                    for k in range(i, min(n, i + len(cfg["open"]))):
                        klass[k] = LITERAL
                    i += len(cfg["open"])
                    state = ("lit", cfg)
                    hit = True
                    break
            if hit:
                continue

            for cfg in chars:
                if _match_at(line, i, cfg["open"]):
                    for k in range(i, min(n, i + len(cfg["open"]))):
                        klass[k] = LITERAL
                    i += len(cfg["open"])
                    state = ("lit", dict(cfg, multiline=False))
                    hit = True
                    break
            if hit:
                continue

            i += 1

        out.append(klass)
        # A non-multiline literal and a to-end-of-line literal both close here.
        if state is not None and state[0] == "lit":
            cfg = state[1]
            if cfg["close"] == "\n" or not cfg.get("multiline"):
                state = None

    return out


def mask(line, klass, drop):
    """Return line with every character whose class is in `drop` replaced by a space."""
    return "".join(" " if k in drop else c for c, k in zip(line, klass))


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------

def median(values):
    if not values:
        return None
    v = sorted(values)
    n = len(v)
    if n % 2:
        return float(v[n // 2])
    return (v[n // 2 - 1] + v[n // 2]) / 2.0


def p95(values):
    """Nearest-rank 95th percentile: sort ascending, take index ceil(0.95*n)."""
    if not values:
        return None
    v = sorted(values)
    idx = math.ceil(0.95 * len(v))
    idx = max(1, min(idx, len(v)))
    return float(v[idx - 1])


# ---------------------------------------------------------------------------
# Routine extraction
# ---------------------------------------------------------------------------

def indent_width(line):
    w = 0
    for ch in line:
        if ch == " ":
            w += 1
        elif ch == "\t":
            w += 1
        else:
            break
    return w


def routines_braces(lines, code_masked, header_re, exclude_re):
    """Yield (start, end, depth) inclusive line indices for a braces block model."""
    n = len(lines)
    consumed_to = -1
    for i in range(n):
        if i <= consumed_to:
            continue
        m = code_masked[i]
        if not header_re.search(m):
            continue
        if exclude_re is not None and exclude_re.search(m):
            continue
        # Find the opening brace of the block this header introduces.
        start_col = None
        j = i
        while j < n and j < i + 8:
            col = code_masked[j].find("{", (m.start() if j == i and False else 0))
            if col != -1:
                start_col = col
                break
            j += 1
        if start_col is None:
            continue
        depth = 0
        maxdepth = 0
        end = None
        k = j
        col = start_col
        while k < n:
            row = code_masked[k]
            c = col
            while c < len(row):
                if row[c] == "{":
                    depth += 1
                    maxdepth = max(maxdepth, depth)
                elif row[c] == "}":
                    depth -= 1
                    if depth == 0:
                        end = k
                        break
                c += 1
            if end is not None:
                break
            k += 1
            col = 0
        if end is None:
            continue
        consumed_to = end
        yield (i, end, maxdepth)


def routines_indentation(lines, code_masked, header_re, exclude_re):
    """Yield (start, end, depth) inclusive line indices for an indentation block model."""
    n = len(lines)
    consumed_to = -1
    for i in range(n):
        if i <= consumed_to:
            continue
        m = code_masked[i]
        if not header_re.search(m):
            continue
        if exclude_re is not None and exclude_re.search(m):
            continue
        base = indent_width(lines[i])
        end = i
        widths = set()
        k = i + 1
        while k < n:
            if not code_masked[k].strip():
                k += 1
                continue
            w = indent_width(lines[k])
            if w <= base:
                break
            widths.add(w)
            end = k
            k += 1
        # Frozen definition: 1 plus the maximum number of indentation steps by
        # which any code line inside the extent is indented beyond the header.
        # The maximum step count equals the number of distinct indentation
        # widths greater than the header's.
        depth = 1 + len(widths)
        consumed_to = end
        yield (i, end, depth)


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def collect_files(roots, extensions):
    found = []
    for root in roots:
        if os.path.isfile(root):
            if any(root.endswith(e) for e in extensions):
                found.append(root)
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames.sort()
            for fn in sorted(filenames):
                if any(fn.endswith(e) for e in extensions):
                    found.append(os.path.join(dirpath, fn))
    return sorted(set(found))


def measure(language, spec, files):
    line_lengths = []
    routine_lengths = []
    nesting_depths = []
    sigil_total = 0
    sigil_nonword = 0
    code_line_count = 0

    header_re = re.compile(spec["routine_header_regex"])
    excl = spec.get("routine_header_exclusion_regex")
    exclude_re = re.compile(excl) if excl else None
    block_model = spec["block_model"]

    for path in files:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = [ln.rstrip("\n").rstrip("\r") for ln in fh]
        klasses = classify_file(lines, spec)

        no_comment = [mask(ln, kl, {COMMENT}) for ln, kl in zip(lines, klasses)]
        code_masked = [mask(ln, kl, {COMMENT, LITERAL}) for ln, kl in zip(lines, klasses)]

        is_code = []
        for ln, nc in zip(lines, no_comment):
            code = bool(nc.strip())
            is_code.append(code)
            if code:
                code_line_count += 1
                line_lengths.append(len(ln.rstrip()))

        for cm in code_masked:
            for ch in cm:
                if ch.isspace():
                    continue
                sigil_total += 1
                if ch not in WORD_CHARS:
                    sigil_nonword += 1

        gen = routines_braces if block_model == "braces" else routines_indentation
        for (start, end, depth) in gen(lines, code_masked, header_re, exclude_re):
            length = sum(1 for k in range(start, end + 1) if is_code[k])
            routine_lengths.append(length)
            nesting_depths.append(depth)

    return {
        "language": language,
        "files_measured": len(files),
        "code_lines": code_line_count,
        "routines": len(routine_lengths),
        "line_length_median": median(line_lengths),
        "line_length_p95": p95(line_lengths),
        "nesting_depth_median": median(nesting_depths),
        "nesting_depth_p95": p95(nesting_depths),
        "routine_length_median": median(routine_lengths),
        "routine_length_p95": p95(routine_lengths),
        "sigil_density": (sigil_nonword / sigil_total) if sigil_total else None,
        "block_model": block_model,
        "R3_median_le_80": None,
        "R3_p95_le_120": None,
        "R4_median_le_4": None,
        "R4_p95_le_5": None,
        "R6_median_le_30": None,
        "R6_p95_le_60": None,
        "R5_sigil_le_0_30_measured_not_scored": None,
    }


def apply_thresholds(r):
    def le(v, t):
        return None if v is None else bool(v <= t)

    r["R3_median_le_80"] = le(r["line_length_median"], 80)
    r["R3_p95_le_120"] = le(r["line_length_p95"], 120)
    r["R4_median_le_4"] = le(r["nesting_depth_median"], 4)
    r["R4_p95_le_5"] = le(r["nesting_depth_p95"], 5)
    r["R6_median_le_30"] = le(r["routine_length_median"], 30)
    r["R6_p95_le_60"] = le(r["routine_length_p95"], 60)
    r["R5_sigil_le_0_30_measured_not_scored"] = le(r["sigil_density"], 0.30)
    r["R3_satisfied"] = bool(r["R3_median_le_80"] and r["R3_p95_le_120"])
    r["R4_satisfied"] = bool(r["R4_median_le_4"] and r["R4_p95_le_5"])
    r["R6_satisfied"] = bool(r["R6_median_le_30"] and r["R6_p95_le_60"])
    return r


def main(argv):
    ap = argparse.ArgumentParser(description="Readability proxy measurement.")
    ap.add_argument("--rubrics", required=True,
                    help="path to methodology/05_standard_rubrics.json")
    ap.add_argument("--language", required=True,
                    help="language name exactly as it appears in frozen_lexing_table")
    ap.add_argument("--corpus", required=True, action="append",
                    help="corpus file or directory; repeatable")
    ap.add_argument("--json", action="store_true", help="emit JSON only")
    args = ap.parse_args(argv[1:])

    with open(args.rubrics, "r", encoding="utf-8") as fh:
        doc = json.load(fh)

    table = None
    for metric in doc.get("metrics", []):
        if metric.get("metric") == "Readability":
            table = metric.get("frozen_lexing_table")
            break
    if table is None:
        sys.stderr.write("no frozen_lexing_table in the Readability metric\n")
        return 2
    if args.language not in table:
        sys.stderr.write(
            "language %r is not in frozen_lexing_table; known: %s\n"
            % (args.language, ", ".join(k for k in table if not k.startswith("_")))
        )
        return 2

    spec = table[args.language]
    files = collect_files(args.corpus, spec["file_extensions"])
    if not files:
        sys.stderr.write("no corpus files with extensions %s under %s\n"
                         % (spec["file_extensions"], args.corpus))
        return 2

    result = apply_thresholds(measure(args.language, spec, files))
    result["corpus_files"] = files

    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
