#!/usr/bin/env python3
"""
The frozen language-neutral tokenizer (methodology 02 section 4.5).

Authoritative for all ten languages. Implements the uniform unit rule: every
lexeme is exactly one token, whatever its length or class. `func` costs one and
`{` costs one; `wrapping_add` costs one and `+%` costs one. No class is
weighted, discounted or exempted -- that is what makes word-heavy and
punctuation-heavy syntaxes comparable without a subjective weighting table.

Deliberate design choice: an unrecognised character is a HARD ERROR naming the
file, line, column and character. There is no silent skip and no catch-all
fallback, because a silent skip would quietly deflate one language's token count
and inflate its Semantic Density.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import unicodedata

D = os.path.dirname(os.path.abspath(__file__))
PROFILE_PATH = os.path.join(D, "token_profiles.json")


class TokenizeError(Exception):
    pass


def _sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def _is_ident_start(ch, spec):
    if ch == "_":
        return True
    if "$" in spec and ch == "$":
        return True
    return ch.isalpha() or unicodedata.category(ch) in ("Lu", "Ll", "Lt", "Lm", "Lo", "Nl")


def _is_ident_continue(ch, spec):
    if ch == "_":
        return True
    if "$" in spec and ch == "$":
        return True
    return ch.isalnum() or unicodedata.category(ch) in (
        "Lu", "Ll", "Lt", "Lm", "Lo", "Nl", "Mn", "Mc", "Nd", "Pc")


class Tokenizer:
    def __init__(self, lang, profile, path="<mem>"):
        self.lang = lang
        self.p = profile
        self.path = path
        # Maximal munch: longest operator/delimiter lexeme first.
        self.ops = sorted(profile.get("operators", []), key=len, reverse=True)
        self.delims = sorted(profile.get("delimiters", []), key=len, reverse=True)
        self.keywords = set(profile.get("keywords", []))
        self.ident_start = profile.get("ident_start", "alpha_")
        self.ident_cont = profile.get("ident_continue", "alnum_")

    # -- position helpers ---------------------------------------------------
    def _lc(self, text, i):
        line = text.count("\n", 0, i) + 1
        col = i - (text.rfind("\n", 0, i) + 1) + 1
        return line, col

    def tokenize(self, text, _depth=0):
        p, toks, i, n = self.p, [], 0, len(text)
        while i < n:
            ch = text[i]

            # 1. whitespace
            if ch.isspace():
                i += 1
                continue

            # 2. line comment
            hit = False
            for lc in (p.get("line_comment") or []):
                if text.startswith(lc, i):
                    j = text.find("\n", i)
                    i = n if j < 0 else j
                    hit = True
                    break
            if hit:
                continue

            # 3. block comment (with optional nesting)
            bc = p.get("block_comment")
            if bc and text.startswith(bc["open"], i):
                o, c = bc["open"], bc["close"]
                if bc.get("nesting"):
                    depth, j = 0, i
                    while j < n:
                        if text.startswith(o, j):
                            depth += 1
                            j += len(o)
                        elif text.startswith(c, j):
                            depth -= 1
                            j += len(c)
                            if depth == 0:
                                break
                        else:
                            j += 1
                    i = j
                else:
                    j = text.find(c, i + len(o))
                    i = n if j < 0 else j + len(c)
                continue

            # 4. Zig multi-line string: each `\\` line is one STR
            ml = p.get("multiline_string")
            if ml and text.startswith(ml["prefix"], i):
                j = text.find("\n", i)
                j = n if j < 0 else j
                toks.append(self._tok("STR", text[i:j], text, i))
                i = j
                continue

            # 5. quoted identifier (Kotlin `x`, Zig @"x y")
            qi_hit = False
            for qi in (p.get("quoted_ident") or []):
                if text.startswith(qi["open"], i):
                    j = text.find(qi["close"], i + len(qi["open"]))
                    if j < 0:
                        raise TokenizeError(self._err(text, i, "unterminated quoted identifier"))
                    end = j + len(qi["close"])
                    toks.append(self._tok("IDENT", text[i:end], text, i))
                    i = end
                    qi_hit = True
                    break
            if qi_hit:
                continue

            # 6. string prefixes (Python f"", Rust r#""#, C++ R"()" , b"" ...)
            consumed = self._try_prefixed_string(text, i, toks, _depth)
            if consumed is not None:
                i = consumed
                continue

            # 7. plain string / char literal
            consumed = self._try_string(text, i, toks, _depth)
            if consumed is not None:
                i = consumed
                continue

            # 8. numeric literal
            consumed = self._try_number(text, i, toks)
            if consumed is not None:
                i = consumed
                continue

            # 9. identifier or keyword
            if _is_ident_start(ch, self.ident_start):
                j = i + 1
                while j < n and _is_ident_continue(text[j], self.ident_cont):
                    j += 1
                word = text[i:j]
                toks.append(self._tok("KW" if word in self.keywords else "IDENT", word, text, i))
                i = j
                continue

            # 10. operator (maximal munch), then delimiter
            m = next((o for o in self.ops if text.startswith(o, i)), None)
            if m:
                toks.append(self._tok("OP", m, text, i))
                i += len(m)
                continue
            m = next((d for d in self.delims if text.startswith(d, i)), None)
            if m:
                toks.append(self._tok("DELIM", m, text, i))
                i += len(m)
                continue

            # 11. no match -> hard error, never a silent skip
            raise TokenizeError(self._err(text, i, f"unrecognised character {ch!r}"))
        return toks

    # -- literal helpers ----------------------------------------------------
    def _err(self, text, i, msg):
        line, col = self._lc(text, i)
        return f"{self.path}:{line}:{col}: {msg} (language={self.lang})"

    def _tok(self, tag, text_, whole, i):
        line, col = self._lc(whole, i)
        return {"tag": tag, "text": text_, "line": line, "col": col}

    def _try_prefixed_string(self, text, i, toks, depth):
        p = self.p
        prefixes = sorted(p.get("string_prefixes", []), key=len, reverse=True)
        for pre in prefixes:
            if not text.startswith(pre, i):
                continue
            k = i + len(pre)
            if k >= len(text):
                continue
            # Rust raw string: r"..." / r#"..."#
            rs = p.get("raw_string")
            if rs and rs["style"] == "rust" and "r" in pre and (text[k] in '"#'):
                h = 0
                while k + h < len(text) and text[k + h] == "#":
                    h += 1
                if k + h < len(text) and text[k + h] == '"':
                    close = '"' + "#" * h
                    j = text.find(close, k + h + 1)
                    if j < 0:
                        raise TokenizeError(self._err(text, i, "unterminated raw string"))
                    end = j + len(close)
                    toks.append(self._tok("STR", text[i:end], text, i))
                    return end
            # C++ raw string: R"delim( ... )delim"
            if rs and rs["style"] == "cpp" and pre.endswith("R") and text[k] == '"':
                m = re.match(r'"([^(\s\\]{0,16})\(', text[k:])
                if m:
                    delim = m.group(1)
                    close = ")" + delim + '"'
                    j = text.find(close, k + m.end())
                    if j < 0:
                        raise TokenizeError(self._err(text, i, "unterminated raw string"))
                    end = j + len(close)
                    toks.append(self._tok("STR", text[i:end], text, i))
                    return end
            if text[k] in "\"'":
                interp = pre in (p.get("interpolating_prefixes") or [])
                return self._read_string(text, i, k, toks, depth, force_interp=interp)
        return None

    def _try_string(self, text, i, toks, depth):
        for spec in (self.p.get("strings") or []) + (self.p.get("chars") or []):
            o = spec["open"]
            if not text.startswith(o, i):
                continue
            # Rust lifetime: 'a not terminated by a closing quote is an IDENT
            if spec.get("lifetime_aware") and o == "'":
                m = re.match(r"'([A-Za-z_][A-Za-z0-9_]*)(?!')", text[i:])
                if m and not re.match(r"'(\\.|[^'\\])'", text[i:]):
                    toks.append(self._tok("IDENT", m.group(0), text, i))
                    return i + m.end()
            return self._read_string(text, i, i, toks, depth, spec=spec)
        return None

    def _read_string(self, text, tok_start, quote_at, toks, depth, spec=None, force_interp=False):
        p = self.p
        if spec is None:
            spec = next((s for s in (p.get("strings") or []) + (p.get("chars") or [])
                         if text.startswith(s["open"], quote_at)), None)
            if spec is None:
                raise TokenizeError(self._err(text, quote_at, "no string form matched"))
        o, c = spec["open"], spec["close"]
        esc = spec.get("escape")
        interp = spec.get("interpolation") if (spec.get("interpolation") and
                                               (force_interp or not p.get("interpolating_prefixes")
                                                or spec.get("interpolation"))) else None
        if force_interp:
            interp = spec.get("interpolation") or {"open": "{", "close": "}"}

        j = quote_at + len(o)
        holes = []
        n = len(text)
        while j < n:
            if esc and text[j] == esc and not (interp and interp.get("open", "").startswith(esc)
                                               and text.startswith(interp["open"], j)):
                j += 2
                continue
            if interp and text.startswith(interp["open"], j):
                k = j + len(interp["open"])
                d, start = 1, k
                while k < n and d:
                    if text[k] == "(" or text[k] == "{":
                        d += 1
                    elif text[k] == ")" or text[k] == "}":
                        d -= 1
                        if d == 0:
                            break
                    k += 1
                # delimited hole: an opening and a closing delimiter were written
                holes.append((text[start:k], False))
                j = k + 1
                continue
            if interp and interp.get("simple") and text[j] == interp["simple"]:
                m = re.match(r"\$([A-Za-z_][A-Za-z0-9_]*)", text[j:])
                if m:
                    # bare sigil hole ($ident): exactly one delimiter was written
                    holes.append((m.group(1), True))
                    j += m.end()
                    continue
            if text.startswith(c, j):
                j += len(c)
                break
            if esc is None and text[j] == "\\":
                j += 1
                continue
            j += 1
        else:
            raise TokenizeError(self._err(text, tok_start, "unterminated string literal"))

        toks.append(self._tok("STR", text[tok_start:j], text, tok_start))
        # A hole pays for the delimiters actually written, exactly as a call form
        # pays for its '(' and ')'. A braced/parenthesised hole ({expr}, ${expr},
        # \(expr)) writes TWO delimiters and costs two tokens; a bare sigil form
        # ($ident) writes ONE and costs one. Charging every hole a flat single
        # token was a class exemption that discounted precisely the five
        # interpolating languages -- Python, TypeScript, Kotlin, Swift and Quidra
        # -- against the five that must spell out a call. See CORRECTIONS D-4.
        for h, simple in holes:
            if simple:
                toks.append(self._tok("HOLE", "<hole>", text, tok_start))
                if depth < 8 and h.strip():
                    toks.extend(self.tokenize(h, _depth=depth + 1))
            else:
                toks.append(self._tok("HOLE_OPEN", "<hole_open>", text, tok_start))
                if depth < 8 and h.strip():
                    toks.extend(self.tokenize(h, _depth=depth + 1))
                toks.append(self._tok("HOLE_CLOSE", "<hole_close>", text, tok_start))
        return j

    def _try_number(self, text, i, toks):
        p, nums = self.p, self.p.get("numbers", {})
        ch = text[i]
        if not (ch.isdigit() or (ch == "." and i + 1 < len(text) and text[i + 1].isdigit())):
            return None
        sep = nums.get("separator") or ""
        j = i
        for pre in sorted(nums.get("prefixes", []), key=len, reverse=True):
            if text.lower().startswith(pre.lower(), i):
                j = i + len(pre)
                break
        seen_dot = False
        n = len(text)
        while j < n:
            c = text[j]
            if c.isalnum() or (sep and c == sep) or c == "_":
                # C++ digit separator ' sits between digits OF THE LITERAL'S BASE,
                # so hex digits (0xFF'FFu) count as digits here. Anything else is
                # a character literal starting, not a separator.
                if sep == "'" and c == "'":
                    if not (j + 1 < n and text[j + 1].isalnum() and text[j - 1].isalnum()):
                        break
                j += 1
                continue
            if c == "." and not seen_dot and j + 1 < n and text[j + 1].isdigit():
                seen_dot = True
                j += 1
                continue
            if c in "+-" and j > i and text[j - 1].lower() in [e.lower() for e in nums.get("exponent", [])]:
                j += 1
                continue
            break
        toks.append(self._tok("NUM", text[i:j], text, i))
        return j


MARKER_BEGIN = re.compile(r"BEGIN PROBE\s+(\S+)")
MARKER_END = re.compile(r"END PROBE\s+(\S+)")


def extract_region(text, probe_id=None):
    """Only tokens strictly between the frozen marker lines are counted (R1).
    A missing or duplicated marker aborts -- never guess a region."""
    begins = [(m.start(), m.group(1)) for m in MARKER_BEGIN.finditer(text)]
    ends = [(m.start(), m.group(1)) for m in MARKER_END.finditer(text)]
    if probe_id:
        begins = [b for b in begins if b[1] == probe_id]
        ends = [e for e in ends if e[1] == probe_id]
    if len(begins) != 1 or len(ends) != 1:
        raise TokenizeError(
            f"expected exactly one BEGIN/END PROBE marker pair"
            f"{f' for {probe_id}' if probe_id else ''}; found {len(begins)} begin / {len(ends)} end")
    b_line_end = text.find("\n", begins[0][0])
    e_line_start = text.rfind("\n", 0, ends[0][0])
    if b_line_end < 0 or e_line_start < b_line_end:
        raise TokenizeError("malformed probe markers")
    return text[b_line_end + 1:e_line_start], begins[0][1]


def apply_r4(tokens, profile):
    """Remove written-but-optional, fact-free lexemes (R4). Kept for audit."""
    removable = {o["lexeme"] for o in profile.get("optional_lexemes", []) if o.get("removable")}
    if not removable:
        return tokens, []
    kept, removed = [], []
    for t in tokens:
        (removed if t["text"] in removable else kept).append(t)
    return kept, removed


def count_file(lang, path, probe_id=None, use_markers=True):
    profiles = json.load(open(PROFILE_PATH))["languages"]
    if lang not in profiles:
        raise TokenizeError(f"no profile for language {lang!r}")
    text = open(path, encoding="utf-8").read()
    pid = probe_id
    if use_markers:
        text, pid = extract_region(text, probe_id)
    tk = Tokenizer(lang, profiles[lang], path)
    toks = tk.tokenize(text)
    kept, removed = apply_r4(toks, profiles[lang])
    hist = {}
    for t in kept:
        hist[t["tag"]] = hist.get(t["tag"], 0) + 1
    return {
        "probe_id": pid,
        "language": lang,
        "count": len(kept),
        "tokens": kept,
        "class_histogram": hist,
        "removed_by_R4": removed,
        "profile_sha256": _sha256(PROFILE_PATH),
        "script_sha256": _sha256(os.path.abspath(__file__)),
    }


def count_source(lang, source):
    profiles = json.load(open(PROFILE_PATH))["languages"]
    toks = Tokenizer(lang, profiles[lang]).tokenize(source)
    kept, _ = apply_r4(toks, profiles[lang])
    return len(kept)


def selftest():
    """Frozen fixtures, including the methodology 02 section 4.4 tables."""
    cases = [
        # interpolation costs the same in every spelling: shell + hole + expr
        ("python", 'f"x={c}"', 4),
        ("typescript", '`x=${c}`', 4),
        ("kotlin", '"x=$c"', 3),   # bare sigil: ONE delimiter
        ("swift", '"x=\\(c)"', 4),
        ("go", 'fmt.Sprintf("x=%d", c)', 8),
        ("cpp", 'std::format("x={}", c)', 8),
        # generic brackets are purely lexical
        ("java", "List<String> xs", 5),
        ("cpp", "std::vector<int> v", 7),
        ("rust", "Vec::<i32>::new()", 9),
        ("zig", "fn f(comptime T: type)", 8),
        # attributes: no exemption, no penalty
        ("java", "@Override", 2),
        ("rust", "#[derive(Clone)]", 7),
        ("cpp", "[[nodiscard]]", 5),
        ("kotlin", '@Suppress("x")', 5),
        ("swift", "@inlinable", 2),
        ("zig", "@intCast(x)", 5),
        # maximal munch
        ("cpp", "vector<vector<int>>", 6),
        # numeric literals are one token each
        ("cpp", "0xFF'FFu", 1),
        ("rust", "3.14e-2f32", 1),
        ("typescript", "100n", 1),
        # qualified names are not one token (R5)
        ("go", "fmt.Println", 3),
        ("cpp", "std::cout", 3),
        # optional terminators removed (R4), mandatory ones kept
        ("go", "x := 1;", 3),
        ("cpp", "int x = 1;", 5),
        ("rust", "let x = 1;", 5),
        # quidra
        ("quidra", 'print("a={v}")', 7),
        ("quidra", "auto x = 1 + 2", 6),
    ]
    fails = []
    for lang, src, expect in cases:
        try:
            got = count_source(lang, src)
        except Exception as e:  # noqa: BLE001
            fails.append((lang, src, expect, f"ERROR: {e}"))
            continue
        if got != expect:
            fails.append((lang, src, expect, got))
    for lang, src, expect, got in fails:
        print(f"  FAIL {lang:11s} {src!r:30s} expected {expect}, got {got}")
    print(f"tokenizer selftest: {len(cases)-len(fails)}/{len(cases)} fixtures pass")
    return not fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lang")
    ap.add_argument("--probe")
    ap.add_argument("--probe-id")
    ap.add_argument("--json-out")
    ap.add_argument("--no-markers", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--source", help="count a literal source string instead of a file")
    a = ap.parse_args()

    if a.selftest:
        sys.exit(0 if selftest() else 1)
    if a.source:
        print(count_source(a.lang, a.source))
        return
    res = count_file(a.lang, a.probe, a.probe_id, use_markers=not a.no_markers)
    text = json.dumps(res, indent=1)
    if a.json_out:
        os.makedirs(os.path.dirname(a.json_out), exist_ok=True)
        open(a.json_out, "w").write(text)
        print(f"{res['language']} {res['probe_id']}: {res['count']} tokens -> {a.json_out}")
    else:
        print(text)


if __name__ == "__main__":
    main()
