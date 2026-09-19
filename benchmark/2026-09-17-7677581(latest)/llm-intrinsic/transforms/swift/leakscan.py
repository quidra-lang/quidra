#!/usr/bin/env python3
"""Identity-leak scan of the Reference Pack (methodology 10, section 2.6).

Three domains, because one blanket rule would be either useless or dishonest:

  A. IDENTITY TERMS -- the real language's name, its toolchain and tool names, its
     vendor, its ecosystem names, its file extension.  Scanned over the WHOLE pack,
     case-insensitively.  Any hit is a leak.

  B. DISTINCTIVE RESERVED WORDS -- every reserved word of the real language that is
     NOT an ordinary English word.  Scanned whole-word over the WHOLE pack.  Any hit
     is a leak: prose has no reason to contain `func`, `nil`, `typealias` or
     `fileprivate`.

  C. THE FULL RESERVED SURFACE + every token this condition transforms -- scanned
     whole-word, but only INSIDE fenced code blocks.  Code is where a real spelling
     would actually teach the model the real language; any hit is a leak.

The words exempted from (B) are listed in EXEMPT_ENGLISH below and printed with their
counts.  They are exempted because they are ordinary English words -- `in`, `for`,
`is`, `as`, `case`, `where`, `return`, `true`, `false`, `do`, `set`, `get`, `open`,
`line`, `file`, `some`, `any`, `none`, `left`, `right`, `final`, `copy`, `package`,
`required`, `optional`, `class`, `import`, `operator`, `super`, `static`, `default`,
`continue`, `break`, `try`, `catch`, `throw` -- and a pack written in English cannot
avoid several of them.  An English function word identifies no language: every one of
the ten packs contains `in` and `for` in prose, so a reader learns nothing from them.
The exemption is a claim about *prose* only; domain (C) still forbids every one of
these words inside a code block, which is where a spelling would actually be a leak.

The V-role spelling `print` is exempt in condition I1 by the section 2.6 exemption
(section 7.1 does not transform standard-library names in I1); it is listed, not
counted as a hit.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PACK = os.path.join(HERE, "reference_pack.md")
text = open(PACK, encoding="utf-8").read()

reserved = [w.strip() for w in open(os.path.join(HERE, "wordlists/reserved_swift.txt"))
            if w.strip()]
mapping = json.load(open(os.path.join(HERE, "mapping.json")))
transformed = [e["real"] for e in mapping["mapping"].values()]

IDENTITY = [
    "swift", "swiftc", "xcode", "apple", "cocoa", "swiftui", "objective-c",
    "objc", "llvm", "foundation", "playground", "appkit", "uikit", "nsstring",
    "darwin", "swiftpm", "sourcekit", "chris lattner",
]

EXEMPT_ENGLISH = {
    "any", "as", "at", "break", "case", "catch", "class", "column", "continue",
    "copy", "default", "discard", "do", "dynamic", "file", "final", "for", "get",
    "if", "import", "in", "indirect", "infix", "is", "isolated", "lazy", "left",
    "let", "line", "macro", "none", "open", "operator", "optional", "package",
    "postfix", "precedence", "prefix", "private", "protocol", "public",
    "repeat", "required", "return", "right", "self", "set", "some", "static",
    "super", "throw", "true", "false", "try", "type", "var", "weak", "where",
    "while", "actual", "async", "await", "actor", "each", "function", "init",
    "available", "consume", "distributed", "mutating", "override", "internal",
    "extension", "subscript", "borrowing", "consuming", "unowned", "convenience",
}

EXEMPT_VROLE = {"print"}


def code_blocks(md):
    return re.findall(r"```[a-zA-Z0-9_+-]*\n(.*?)```", md, re.S)


def whole_word_hits(needle, hay, case=True):
    flags = 0 if case else re.IGNORECASE
    return list(re.finditer(r"(?<![A-Za-z0-9_])" + re.escape(needle)
                            + r"(?![A-Za-z0-9_])", hay, flags))


hits = []
lines_of = text.splitlines()

# ---- domain A: identity terms, whole pack, case-insensitive
for term in IDENTITY:
    for m in re.finditer(re.escape(term), text, re.IGNORECASE):
        ln = text.count("\n", 0, m.start()) + 1
        hits.append(("A/identity", term, ln, lines_of[ln - 1].strip()[:70]))

# ---- domain B: distinctive reserved words, whole pack
exempt_seen = {}
for w in sorted(set(reserved) | set(transformed)):
    if w in EXEMPT_VROLE:
        continue
    if w.lower() in EXEMPT_ENGLISH:
        n = len(whole_word_hits(w, text))
        if n:
            exempt_seen[w] = n
        continue
    for m in whole_word_hits(w, text):
        ln = text.count("\n", 0, m.start()) + 1
        hits.append(("B/distinctive", w, ln, lines_of[ln - 1].strip()[:70]))

# ---- domain C: the full reserved surface, inside code blocks only
blocks = code_blocks(text)
for w in sorted(set(reserved) | set(transformed)):
    if w in EXEMPT_VROLE:
        continue
    for b in blocks:
        for m in whole_word_hits(w, b):
            ctx = b[max(0, m.start() - 25):m.start() + 25].replace("\n", " / ")
            hits.append(("C/in-code", w, -1, ctx))

nlines = text.count("\n") + (0 if text.endswith("\n") else 1)
print("reference_pack.md: %d lines, %d characters" % (nlines, len(text)))
print("code blocks scanned: %d" % len(blocks))
print("exempt V-role spellings present by design (section 2.6 I1 exemption): %s"
      % ", ".join(sorted(s for s in EXEMPT_VROLE
                         if whole_word_hits(s, text))))
print("ordinary-English words from the reserved list that occur in PROSE "
      "(exempt from domain B, still forbidden in domain C):")
for w in sorted(exempt_seen):
    print("    %-12s x%d" % (w, exempt_seen[w]))
if hits:
    print("LEAK SCAN: %d HIT(S)" % len(hits))
    for dom, w, ln, ctx in hits:
        print("  %-14s line %-4s %-14s | %s" % (dom, ln if ln > 0 else "-", w, ctx))
    sys.exit(1)
print("LEAK SCAN: 0 hits")
