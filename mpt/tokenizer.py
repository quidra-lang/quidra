#!/usr/bin/env python3
"""Tokenize source files and render a minimal line-numbered HTML view.

Usage: python3 tokenizer.py FILE [FILE ...]
Writes a single integer per input to stdout and creates token.html in the
current directory. The lexical rules are independent of file extension.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from html import escape
from pathlib import Path
import re
import sys
import unicodedata

_COMMON_OPERATORS = """
=== !== <=> >>>= <<= >>= &&= ||= ??= **= //= ... ..= ..<
->* <<|= >>|= +%= -%= *%= +|= -|= *|= >>> <<| >>| << >>
== != <= >= => -> :: := ?? ?. ?: !! ** // ++ -- && ||
+= -= *= /= %= &= |= ^= <<= >>= &= |
<: >: :< :> #[ ]# #= =# ##
|> <| .? .: .* .+ .- ./ .% .^ .== .!= .>= .<= .> .< .=
.+= .-= .*= ./= .%= .^= .&& .|| .??
""".split()
# Recognize J symbolic primitives as single lexical operators.
# Symbolic primitives such as *: are single tokens; +/ is two tokens.
_J_SYMBOL_BASE = (
    "+", "-", "*", "%", "<", ">", "=", "~", "|", "{", "}",
    "#", ";", "!", "^", "&", "@", "/", "\\", "[", "]",
    "$", "?", ",", ":", ".",
)
_J_SYMBOL_OPERATORS = {s + e for s in _J_SYMBOL_BASE for e in (".", ":")}
_J_SYMBOL_OPERATORS.update({"&.:", "^::"})
# Julia's broadcast family and Zig's checked/saturating operators.
_BROADCAST_OPS = {
    "." + base for base in (
        "+", "-", "*", "/", "\\", "%", "^", "=", "==", "!=",
        "<", ">", "<=", ">=", "&&", "||", "&", "|", "//",
        "+=", "-=", "*=", "/=", "^=", "%=",
    )
}
_ZIG_OPS = {"+%", "-%", "*%", "+|", "-|", "*|", "+%=", "-%=", "*%=", "+|=", "-|=", "*|="}
OPERATORS = frozenset(_COMMON_OPERATORS) | _J_SYMBOL_OPERATORS | _BROADCAST_OPS | _ZIG_OPS
# Only punctuation operators need longest-match; ordinary single characters
# are accepted as one token even if unknown (including Unicode symbols).
SORTED_OPERATORS = tuple(sorted({s for s in OPERATORS if len(s) > 1}, key=lambda x: (-len(x), x)))
OPERATORS_BY_START = {
    ch: tuple(op for op in SORTED_OPERATORS if op.startswith(ch))
    for ch in {op[0] for op in SORTED_OPERATORS}
}

# J primitive words with attached dot or colon.  In other languages x.method
# stays x + . + method because the suffix is followed by a name character.
_J_PRIMITIVE_WORDS = frozenset(
    "a. a: b. b: C. C: d. d: e. e: f. f: H. i. i: j. j: "
    "L. L: M. NB. o. o: p. p: q: r. r: s: S: t. t: u: v: x: "
    "if. elseif. else. do. end. for. while. whilst. break. continue. "
    "return. assert. select. case. fcase. try. catch. catchd. throw. "
    "label. goto. endtry. endselect."
    .split()
)
SORTED_J_WORDS = tuple(sorted(_J_PRIMITIVE_WORDS, key=lambda x: (-len(x), x)))
J_WORDS_BY_START = {
    ch: tuple(word for word in SORTED_J_WORDS if word.startswith(ch))
    for ch in {word[0] for word in SORTED_J_WORDS}
}
RE_J_FOR = re.compile(r"for_[A-Za-z_]\w*\.")
RE_CPP_RAW = re.compile(r'(?:u8|u|U|L)?R"([^\s()\\]{0,16})\(')
RE_HASH_QUOTE = re.compile(r'(?:br|rb|r)?(#{1,16})("""|")')
RE_STR_PREFIX = re.compile(r'(?i:fr|rf|br|rb|ur|ru|u8|r|f|b|u|l|c|R)?("""|\'\'\'|"|\'|`)')

# Shared numeric lexeme rule, including common base, exponent, and suffix forms.
RE_NUMBER = re.compile(r"""
    (?:
        0[xX][0-9a-fA-F_']+(?:\.[0-9a-fA-F_']+)?(?:[pP][+-]?[0-9_']+)?
      | 0[bB][01_']+
      | 0[oO][0-7_']+
      | (?:[0-9][0-9_']*(?:\.(?!\.)[0-9_']*)?|\.[0-9][0-9_']*)
        (?:[eEfF](?:[+-]|_)?[0-9][0-9_']*)?
    )
    (?:
      usize|isize|[ui](?:8|16|32|64|128)|[fF](?:16|32|64|128)
      |[rR][0-9_]+|[jJ][0-9_]+|im|[xXnNjJ]
      |(?:[uUlL]{1,3})
      |(?:_[A-Za-z][A-Za-z_0-9]*)
    )?
""", re.VERBOSE)
RE_J_NEGATIVE = re.compile(r"_[0-9][0-9_']*(?:\.[0-9_']+)?(?:[eE](?:[+-]|_)?[0-9_']+)?(?:[xrj][0-9_]*)?")


@dataclass(frozen=True, slots=True)
class Token:
    kind: str
    text: str
    line: int     # 1-indexed
    column: int   # 1-indexed, Unicode code point columns
    offset: int   # 0-indexed, character offset in normalized source


def _id_start(ch: str) -> bool:
    return ch == "_" or ch.isalpha() or unicodedata.category(ch) == "Nl"


def _id_continue(ch: str) -> bool:
    return _id_start(ch) or ch.isdecimal() or unicodedata.category(ch) in {"Mn", "Mc", "Me", "Pc", "Cf"}


def _quoted_end(source: str, quote_start: int, opener: str, closer: str | None = None) -> int | None:
    """Return index after a quoted literal; supports escapes and J doubled '."""
    closer = closer or opener
    pos = quote_start + len(opener)
    multiline = len(opener) == 3 or opener == "`"  # TS template/Go raw/Julia command strings
    while pos < len(source):
        if source.startswith(closer, pos):
            if opener == "'" and source.startswith("''", pos):
                # In J, a doubled single quote is an escaped apostrophe.
                # Only skip it if some later quote can close this literal.
                later = source.find("'", pos + 2)
                eol = source.find("\n", pos + 2)
                if later != -1 and (eol == -1 or later < eol):
                    pos += 2
                    continue
            return pos + len(closer)
        if source[pos] == "\\":
            pos += 2   # C/Python/JS and friends; J/Quidra harmless convention
            continue
        if source[pos] == "\n" and not multiline:
            return None
        pos += 1
    return None


def _literal_end(source: str, i: int) -> int | None:
    """Language-neutral union of widely-used literal spellings."""
    m = RE_CPP_RAW.match(source, i)
    if m:
        closer = ")" + m.group(1) + '"'
        end = source.find(closer, m.end())
        return end + len(closer) if end != -1 else None

    m = RE_HASH_QUOTE.match(source, i)
    if m:
        closer = m.group(2) + m.group(1)
        end = source.find(closer, m.end())
        return end + len(closer) if end != -1 else None

    m = RE_STR_PREFIX.match(source, i)
    if not m:
        return None
    quote = m.group(1)
    prefix_len = m.end() - i - len(quote)
    # Julia's postfix conjugate transpose is not a quoted character literal.
    if prefix_len == 0 and quote == "'" and i > 0 and (source[i-1].isalnum() or source[i-1] in ")]}"):
        return None
    return _quoted_end(source, i + prefix_len, quote)


def tokenize(source: str) -> list[Token]:
    """Split source into lexical tokens, including whitespace.

    A run of four leading ASCII spaces is one INDENT token. Eight is two.
    Any remaining leading spaces count one each. Every other space/tab is one.
    LF (also normalized CRLF/CR) is one token, including blank lines.
    One quoted literal counts as one token, regardless of its length.
    Comments are counted as source text, not interpreted.
    """
    source = source.replace("\r\n", "\n").replace("\r", "\n")
    if source.startswith("\ufeff"):
        source = source[1:]
    i, line, column = 0, 1, 1
    at_line_start = True
    n = len(source)
    tokens: list[Token] = []

    def emit(kind: str, end: int) -> None:
        nonlocal i, line, column, at_line_start
        segment = source[i:end]
        tokens.append(Token(kind, segment, line, column, i))
        if "\n" in segment:
            count = segment.count("\n")
            line += count
            column = len(segment) - segment.rfind("\n")
            at_line_start = segment.endswith("\n")
        else:
            column += len(segment)
            if segment.strip(" \t"):
                at_line_start = False
        i = end

    while i < n:
        ch = source[i]
        if ch == "\n":
            emit("newline", i + 1)
            continue
        if at_line_start and ch == " ":
            if source.startswith("    ", i):
                emit("indent4", i + 4)
            else:
                emit("space", i + 1)
            continue
        if ch == " " or ch == "\t" or (ch.isspace() and ch != "\n"):
            emit("space", i + 1)
            continue

        end = _literal_end(source, i)
        if end is not None:
            emit("literal", end)
            continue

        # J's for_i. and i. / NB. / if. are global lexical atoms.
        fm = RE_J_FOR.match(source, i)
        if fm and (fm.end() == n or not _id_start(source[fm.end()])):
            emit("operator_word", fm.end())
            continue
        matched = False
        for word in J_WORDS_BY_START.get(ch, ()):
            if source.startswith(word, i):
                after = i + len(word)
                if after == n or not _id_start(source[after]):
                    emit("operator_word", after)
                    matched = True
                    break
        if matched:
            continue

        if ch == "_" and i + 1 < n and source[i+1].isdigit():
            m = RE_J_NEGATIVE.match(source, i)
            if m:
                emit("number", m.end())
                continue
        if ch.isdigit() or (ch == "." and i + 1 < n and source[i + 1].isdigit()):
            m = RE_NUMBER.match(source, i)
            if m:
                emit("number", m.end())
                continue

        if _id_start(ch):
            end = i + 1
            while end < n and _id_continue(source[end]):
                end += 1
            emit("identifier", end)
            continue

        # Julia permits broadcast of a Unicode operator, e.g. .⊗ .√ .∈.
        # The same lexical rule is applied to *all* supported languages.
        if ch == "." and i + 1 < n:
            next_cat = unicodedata.category(source[i + 1])
            if next_cat.startswith("S") and not _id_start(source[i + 1]):
                end = i + 2
                while end < n and unicodedata.category(source[end]).startswith("S"):
                    end += 1
                emit("operator", end)
                continue

        for op in OPERATORS_BY_START.get(ch, ()):
            if source.startswith(op, i):
                emit("operator", i + len(op))
                matched = True
                break
        if matched:
            continue
        # Unicode math operators, punctuation, standalone delimiter, etc.
        emit("symbol", i + 1)

    return tokens


# Pastel backgrounds distinguish adjacent tokens without changing their text.
COLORS = (
    "#ffedb5", "#c9f1d9", "#cdeaff", "#f3d8ff",
    "#ffd7d7", "#dce5bd", "#ffe2c0", "#cdddf7",
    "#d4f3ef", "#f7e6c5", "#e0d9ff", "#dff0c5",
)

CSS = """
*{box-sizing:border-box}
body{margin:16px;background:#f6f7f9;color:#1c2430}
.program{overflow-x:auto;margin-bottom:20px;padding:10px 0;background:#fff;
         border:1px solid #e5e7eb}
.line{display:flex;min-height:1.65em;min-width:max-content;
      font:14px/1.65 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
.number{position:sticky;left:0;display:inline-block;flex:0 0 3.5em;
        padding:0 .8em 0 .3em;text-align:right;user-select:none;
        background:#fff;color:#8b919c;border-right:1px solid #e5e7eb}
.code{padding:0 .75em;white-space:pre}
.token{white-space:pre;border-radius:1px}
"""


def render_html(programs: list[list[Token]]) -> str:
    colors = "\n".join(
        f".c{i}{{background:{color}}}" for i, color in enumerate(COLORS)
    )
    blocks = []
    for tokens in programs:
        lines: list[list[str]] = [[]]
        for index, tok in enumerate(tokens):
            color = index % len(COLORS)
            for part_number, part in enumerate(tok.text.split("\n")):
                if part_number:
                    lines.append([])
                if part:
                    lines[-1].append(
                        f'<span class="token c{color}">{escape(part)}</span>'
                    )
        rows = "".join(
            f'<div class="line"><span class="number">{number}</span>'
            f'<span class="code">{"".join(parts)}</span></div>'
            for number, parts in enumerate(lines, start=1)
        )
        blocks.append(f'<div class="program">{rows}</div>')
    return (
        '<!doctype html><html lang="en"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        f'<style>{CSS}{colors}</style></head><body>'
        + "".join(blocks) + '</body></html>\n'
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Count source tokens and create a minimal token.html view."
    )
    parser.add_argument(
        "files", nargs="*", metavar="FILE",
        help="Source files, or '-' for standard input (default: standard input)",
    )
    args = parser.parse_args(argv)
    inputs = args.files or ["-"]
    if inputs.count("-") > 1:
        parser.error("standard input can be used only once")

    all_tokens: list[list[Token]] = []
    try:
        for item in inputs:
            source = sys.stdin.read() if item == "-" else Path(item).read_text(encoding="utf-8-sig")
            all_tokens.append(tokenize(source))
        Path("token.html").write_text(render_html(all_tokens), encoding="utf-8")
    except (OSError, UnicodeError) as error:
        parser.exit(2, f"error: {error}\n")

    for tokens in all_tokens:
        print(len(tokens))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
