#!/usr/bin/env python3
"""
Generates token_profiles.json: one frozen lexical profile per language,
implementing methodology 02 section 4.6.

Kept as a generator rather than a hand-edited JSON blob so the keyword and
operator tables are visibly sourced and symmetrical across languages. The
emitted JSON is the frozen artifact; this script records how it was built.
"""

import json
import os

D = os.path.dirname(os.path.abspath(__file__))

COMMON_DELIMS = ["(", ")", "[", "]", "{", "}", ",", ";", ":", ".", "@", "#", "?", "$", "\\"]


def prof(**kw):
    p = {
        "line_comment": ["//"],
        "block_comment": {"open": "/*", "close": "*/", "nesting": False},
        "strings": [{"open": '"', "close": '"', "escape": "\\", "raw": False}],
        "chars": [],
        "ident_start": "alpha_",
        "ident_continue": "alnum_",
        "quoted_ident": [],
        "numbers": {"prefixes": ["0x", "0X", "0o", "0O", "0b", "0B"], "separator": "_",
                    "exponent": ["e", "E", "p", "P"], "suffixes": []},
        "keywords": [],
        "operators": [],
        "delimiters": list(COMMON_DELIMS),
        "optional_lexemes": [],
        "reference_lexer": {"available": False, "reason": "not determined"},
        "fallbacks": [],
    }
    p.update(kw)
    return p


# Operator tables. Longest-match is applied by sorting at load time, so order
# here is irrelevant. `?`, `$`, `#`, `@`, `\` live in delimiters.
C_FAMILY_OPS = [
    "<<=", ">>=", "...", "->*", ".*", "<=>",
    "==", "!=", "<=", ">=", "&&", "||", "++", "--", "+=", "-=", "*=", "/=", "%=",
    "&=", "|=", "^=", "<<", ">>", "->", "::", "=>",
    "+", "-", "*", "/", "%", "=", "<", ">", "!", "~", "&", "|", "^",
]

PROFILES = {}

# --- Quidra --------------------------------------------------------------
# Populated from Quidra 0.2.0's own published grammar/reference by exactly the
# procedure used for the other nine languages (methodology 02 section 4.5).
PROFILES["quidra"] = prof(
    line_comment=["//"],
    block_comment=None,
    strings=[{"open": '"', "close": '"', "escape": None, "raw": True,
              "interpolation": {"open": "{", "close": "}"}}],
    keywords=[
        "if", "elif", "else", "while", "for", "in", "return", "break", "continue",
        "match", "class", "override", "super", "import", "extern", "cli",
        "const", "auto", "and", "or", "not", "none", "error", "true", "false",
        "int", "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64",
        "float", "float32", "float64", "bool", "string", "bytes", "void", "never",
        "tensor", "neural", "try",
    ],
    operators=["==", "!=", "<=", ">=", "+=", "-=", "*=", "/=", "%=",
               "+", "-", "*", "/", "%", "=", "<", ">", "&", "|"],
    numbers={"prefixes": ["0x", "0b", "0o"], "separator": "_", "exponent": ["e", "E"], "suffixes": []},
    reference_lexer={"available": False,
                     "reason": "Quidra 0.2.0 exposes no token-stream dump; `quidra inspect` emits "
                               "typed AST nodes (parse tree), not lexical tokens."},
    fallbacks=["Quidra has no backslash escapes (documented): string literals are treated as raw "
               "with no escape character. Interpolation uses {expr} per the language reference.",
               "Block comments: none documented; profile declares none."],
)

# --- Python ---------------------------------------------------------------
PROFILES["python"] = prof(
    line_comment=["#"],
    block_comment=None,
    strings=[
        {"open": '"""', "close": '"""', "escape": "\\", "raw": False, "multiline": True},
        {"open": "'''", "close": "'''", "escape": "\\", "raw": False, "multiline": True},
        {"open": '"', "close": '"', "escape": "\\", "raw": False},
        {"open": "'", "close": "'", "escape": "\\", "raw": False},
    ],
    string_prefixes=["r", "b", "f", "u", "rb", "br", "fr", "rf", "R", "B", "F", "U"],
    interpolating_prefixes=["f", "F", "fr", "rf"],
    keywords=[
        "False", "None", "True", "and", "as", "assert", "async", "await", "break",
        "class", "continue", "def", "del", "elif", "else", "except", "finally",
        "for", "from", "global", "if", "import", "in", "is", "lambda", "nonlocal",
        "not", "or", "pass", "raise", "return", "try", "while", "with", "yield",
        "match", "case",
    ],
    operators=["**=", "//=", ">>=", "<<=", "...", "!=", "==", "<=", ">=", ":=",
               "**", "//", "<<", ">>", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "->",
               "+", "-", "*", "/", "%", "=", "<", ">", "~", "&", "|", "^"],
    numbers={"prefixes": ["0x", "0X", "0o", "0O", "0b", "0B"], "separator": "_",
             "exponent": ["e", "E"], "suffixes": ["j", "J"]},
    reference_lexer={"available": True, "command": "python3 -m tokenize",
                     "mapping": "drop NEWLINE/NL/INDENT/DEDENT/ENDMARKER/COMMENT; "
                                "f-strings remapped to shell+hole per 02 4.4"},
)

# --- C++ ------------------------------------------------------------------
PROFILES["cpp"] = prof(
    strings=[{"open": '"', "close": '"', "escape": "\\", "raw": False}],
    chars=[{"open": "'", "close": "'", "escape": "\\"}],
    raw_string={"style": "cpp", "prefix": "R"},
    string_prefixes=["L", "u8", "u", "U", "R", "LR", "u8R", "uR", "UR"],
    keywords=[
        "alignas", "alignof", "asm", "auto", "bool", "break", "case", "catch", "char",
        "char8_t", "char16_t", "char32_t", "class", "concept", "const", "consteval",
        "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return",
        "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast",
        "else", "enum", "explicit", "export", "extern", "false", "float", "for",
        "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new",
        "noexcept", "nullptr", "operator", "private", "protected", "public", "register",
        "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "template", "this",
        "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
        "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while",
    ],
    operators=C_FAMILY_OPS,
    numbers={"prefixes": ["0x", "0X", "0b", "0B"], "separator": "'",
             "exponent": ["e", "E", "p", "P"],
             "suffixes": ["u", "U", "l", "L", "ll", "LL", "ul", "UL", "ull", "ULL",
                          "f", "F", "z", "Z"]},
    reference_lexer={"available": True,
                     "command": "clang++ -std=c++20 -fsyntax-only -Xclang -dump-tokens",
                     "mapping": "drop eof; rejoin 'greater greater' into one >> per maximal munch"},
)

# --- Rust -----------------------------------------------------------------
PROFILES["rust"] = prof(
    block_comment={"open": "/*", "close": "*/", "nesting": True},
    strings=[{"open": '"', "close": '"', "escape": "\\", "raw": False}],
    chars=[{"open": "'", "close": "'", "escape": "\\", "lifetime_aware": True}],
    raw_string={"style": "rust", "prefix": "r"},
    string_prefixes=["b", "r", "br", "rb"],
    keywords=[
        "as", "async", "await", "break", "const", "continue", "crate", "dyn", "else",
        "enum", "extern", "false", "fn", "for", "if", "impl", "in", "let", "loop",
        "match", "mod", "move", "mut", "pub", "ref", "return", "self", "Self", "static",
        "struct", "super", "trait", "true", "type", "unsafe", "use", "where", "while",
    ],
    operators=["<<=", ">>=", "...", "..=", "==", "!=", "<=", ">=", "&&", "||",
               "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>", "->", "=>",
               "::", "..", "+", "-", "*", "/", "%", "=", "<", ">", "!", "&", "|", "^"],
    numbers={"prefixes": ["0x", "0o", "0b"], "separator": "_", "exponent": ["e", "E"],
             "suffixes": ["i8", "i16", "i32", "i64", "i128", "isize",
                          "u8", "u16", "u32", "u64", "u128", "usize", "f32", "f64"]},
    reference_lexer={"available": False,
                     "reason": "no stable token-dump interface in rustc 1.95.0 "
                               "(-Zunpretty requires nightly)"},
)

# --- Go -------------------------------------------------------------------
PROFILES["go"] = prof(
    strings=[{"open": '"', "close": '"', "escape": "\\", "raw": False},
             {"open": "`", "close": "`", "escape": None, "raw": True}],
    chars=[{"open": "'", "close": "'", "escape": "\\"}],
    keywords=[
        "break", "case", "chan", "const", "continue", "default", "defer", "else",
        "fallthrough", "for", "func", "go", "goto", "if", "import", "interface",
        "map", "package", "range", "return", "select", "struct", "switch", "type", "var",
    ],
    operators=["<<=", ">>=", "&^=", "...", "==", "!=", "<=", ">=", "&&", "||", "<-",
               "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>",
               "&^", ":=", "+", "-", "*", "/", "%", "=", "<", ">", "!", "&", "|", "^"],
    numbers={"prefixes": ["0x", "0X", "0o", "0O", "0b", "0B"], "separator": "_",
             "exponent": ["e", "E", "p", "P"], "suffixes": ["i"]},
    optional_lexemes=[{"lexeme": ";", "removable": True,
                       "reason": "grammar permits omission (auto-insertion); carries no fact value"}],
    reference_lexer={"available": True, "command": "go/scanner driver",
                     "mapping": "drop auto-inserted semicolons (literal '\\n'), drop EOF"},
)

# --- Java -----------------------------------------------------------------
PROFILES["java"] = prof(
    strings=[{"open": '"""', "close": '"""', "escape": "\\", "raw": False, "multiline": True},
             {"open": '"', "close": '"', "escape": "\\", "raw": False}],
    chars=[{"open": "'", "close": "'", "escape": "\\"}],
    ident_start="alpha_$",
    ident_continue="alnum_$",
    keywords=[
        "abstract", "assert", "boolean", "break", "byte", "case", "catch", "char",
        "class", "const", "continue", "default", "do", "double", "else", "enum",
        "extends", "final", "finally", "float", "for", "goto", "if", "implements",
        "import", "instanceof", "int", "interface", "long", "native", "new", "package",
        "private", "protected", "public", "return", "short", "static", "strictfp",
        "super", "switch", "synchronized", "this", "throw", "throws", "transient",
        "try", "void", "volatile", "while", "var", "record", "sealed", "permits",
        "yield", "true", "false", "null",
    ],
    operators=[">>>=", "<<=", ">>=", ">>>", "...", "==", "!=", "<=", ">=", "&&", "||",
               "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>",
               "->", "::", "+", "-", "*", "/", "%", "=", "<", ">", "!", "~", "&", "|", "^"],
    numbers={"prefixes": ["0x", "0X", "0b", "0B"], "separator": "_",
             "exponent": ["e", "E", "p", "P"], "suffixes": ["l", "L", "f", "F", "d", "D"]},
    reference_lexer={"available": False,
                     "reason": "com.sun.tools.javac.parser is internal/unsupported in OpenJDK 26.0.1"},
)

# --- TypeScript -----------------------------------------------------------
PROFILES["typescript"] = prof(
    strings=[{"open": '"', "close": '"', "escape": "\\", "raw": False},
             {"open": "'", "close": "'", "escape": "\\", "raw": False},
             {"open": "`", "close": "`", "escape": "\\", "raw": False,
              "interpolation": {"open": "${", "close": "}"}}],
    ident_start="alpha_$",
    ident_continue="alnum_$",
    keywords=[
        "any", "as", "async", "await", "boolean", "break", "case", "catch", "class",
        "const", "continue", "debugger", "declare", "default", "delete", "do", "else",
        "enum", "export", "extends", "false", "finally", "for", "from", "function",
        "get", "if", "implements", "import", "in", "infer", "instanceof", "interface",
        "is", "keyof", "let", "namespace", "never", "new", "null", "number", "object",
        "of", "package", "private", "protected", "public", "readonly", "return", "set",
        "static", "string", "super", "switch", "symbol", "this", "throw", "true", "try",
        "type", "typeof", "undefined", "unique", "unknown", "var", "void", "while",
        "with", "yield", "satisfies",
    ],
    operators=[">>>=", "**=", "&&=", "||=", "??=", "...", "===", "!==", ">>>",
               "<<=", ">>=", "==", "!=", "<=", ">=", "&&", "||", "??", "?.", "**",
               "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>",
               "=>", "+", "-", "*", "/", "%", "=", "<", ">", "!", "~", "&", "|", "^"],
    numbers={"prefixes": ["0x", "0X", "0o", "0O", "0b", "0B"], "separator": "_",
             "exponent": ["e", "E"], "suffixes": ["n"]},
    optional_lexemes=[{"lexeme": ";", "removable": True,
                       "reason": "ASI permits omission; carries no fact value"}],
    reference_lexer={"available": True, "command": "ts.createScanner via node",
                     "mapping": "drop NewLineTrivia/WhitespaceTrivia/comment trivia; "
                                "template spans remapped to shell+hole"},
)

# --- Kotlin ---------------------------------------------------------------
PROFILES["kotlin"] = prof(
    block_comment={"open": "/*", "close": "*/", "nesting": True},
    strings=[{"open": '"""', "close": '"""', "escape": None, "raw": True, "multiline": True,
              "interpolation": {"open": "${", "close": "}", "simple": "$"}},
             {"open": '"', "close": '"', "escape": "\\", "raw": False,
              "interpolation": {"open": "${", "close": "}", "simple": "$"}}],
    chars=[{"open": "'", "close": "'", "escape": "\\"}],
    quoted_ident=[{"open": "`", "close": "`"}],
    keywords=[
        "as", "break", "class", "continue", "do", "else", "false", "for", "fun", "if",
        "in", "interface", "is", "null", "object", "package", "return", "super", "this",
        "throw", "true", "try", "typealias", "typeof", "val", "var", "when", "while",
        "by", "catch", "constructor", "delegate", "dynamic", "field", "file", "finally",
        "get", "import", "init", "param", "property", "receiver", "set", "setparam",
        "where", "abstract", "actual", "annotation", "companion", "const", "crossinline",
        "data", "enum", "expect", "external", "final", "infix", "inline", "inner",
        "internal", "lateinit", "noinline", "open", "operator", "out", "override",
        "private", "protected", "public", "reified", "sealed", "suspend", "tailrec",
        "vararg", "it",
    ],
    operators=["===", "!==", "?:", "!!", "==", "!=", "<=", ">=", "&&", "||", "?.",
               "++", "--", "+=", "-=", "*=", "/=", "%=", "->", "..", "::",
               "+", "-", "*", "/", "%", "=", "<", ">", "!"],
    numbers={"prefixes": ["0x", "0X", "0b", "0B"], "separator": "_",
             "exponent": ["e", "E"], "suffixes": ["L", "f", "F", "u", "U", "uL", "UL"]},
    optional_lexemes=[{"lexeme": ";", "removable": True,
                       "reason": "grammar permits omission; carries no fact value"}],
    reference_lexer={"available": False, "reason": "kotlinc 2.3.21 exposes no token dump"},
)

# --- Swift ----------------------------------------------------------------
PROFILES["swift"] = prof(
    block_comment={"open": "/*", "close": "*/", "nesting": True},
    strings=[{"open": '"""', "close": '"""', "escape": "\\", "raw": False, "multiline": True,
              "interpolation": {"open": "\\(", "close": ")"}},
             {"open": '"', "close": '"', "escape": "\\", "raw": False,
              "interpolation": {"open": "\\(", "close": ")"}}],
    quoted_ident=[{"open": "`", "close": "`"}],
    keywords=[
        "associatedtype", "class", "deinit", "enum", "extension", "fileprivate", "func",
        "import", "init", "inout", "internal", "let", "open", "operator", "private",
        "precedencegroup", "protocol", "public", "rethrows", "static", "struct",
        "subscript", "typealias", "var", "break", "case", "catch", "continue",
        "default", "defer", "do", "else", "fallthrough", "for", "guard", "if", "in",
        "repeat", "return", "throw", "switch", "where", "while", "Any", "as", "await",
        "false", "is", "nil", "self", "Self", "super", "throws", "true", "try",
        "async", "some", "any", "consuming", "borrowing", "nonisolated", "actor",
        "mutating", "nonmutating", "lazy", "final", "indirect", "required", "optional",
        "override", "convenience", "dynamic", "weak", "unowned", "willSet", "didSet",
        "get", "set",
    ],
    operators=["===", "!==", "...", "..<", "??", "==", "!=", "<=", ">=", "&&", "||",
               "+=", "-=", "*=", "/=", "%=", "&+", "&-", "&*", "->", "?.",
               "+", "-", "*", "/", "%", "=", "<", ">", "!", "&", "|", "^", "~"],
    numbers={"prefixes": ["0x", "0o", "0b"], "separator": "_",
             "exponent": ["e", "E", "p", "P"], "suffixes": []},
    reference_lexer={"available": False, "reason": "swift-syntax not installed as a CLI on this host"},
)

# --- Zig ------------------------------------------------------------------
PROFILES["zig"] = prof(
    line_comment=["//"],
    block_comment=None,
    strings=[{"open": '"', "close": '"', "escape": "\\", "raw": False}],
    chars=[{"open": "'", "close": "'", "escape": "\\"}],
    multiline_string={"prefix": "\\\\"},
    quoted_ident=[{"open": '@"', "close": '"'}],
    keywords=[
        "addrspace", "align", "allowzero", "and", "anyframe", "anytype", "asm", "async",
        "await", "break", "callconv", "catch", "comptime", "const", "continue", "defer",
        "else", "enum", "errdefer", "error", "export", "extern", "fn", "for", "if",
        "inline", "linksection", "noalias", "noinline", "nosuspend", "opaque", "or",
        "orelse", "packed", "pub", "resume", "return", "struct", "suspend", "switch",
        "test", "threadlocal", "try", "union", "unreachable", "usingnamespace", "var",
        "volatile", "while", "true", "false", "null", "undefined",
    ],
    operators=["<<=", ">>=", "*%=", "+%=", "-%=", "*|=", "+|=", "-|=", "<<|=",
               "==", "!=", "<=", ">=", "++", "**", "+%", "-%", "*%", "+|", "-|", "*|",
               "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>", "->", "=>",
               "||", "..", ".*", ".?",
               "+", "-", "*", "/", "%", "=", "<", ">", "!", "&", "|", "^", "~"],
    numbers={"prefixes": ["0x", "0o", "0b"], "separator": "_",
             "exponent": ["e", "E", "p", "P"], "suffixes": []},
    reference_lexer={"available": False,
                     "reason": "zig 0.16.0 exposes no token dump CLI (ast-check reports diagnostics)"},
    fallbacks=["Zig has no block comments; profile declares none.",
               "@builtin: '@' is a DELIM and the following name an IDENT, so @intCast is 2 tokens."],
)


def main():
    out = os.path.join(D, "token_profiles.json")
    doc = {
        "_schema": "methodology 02 section 4.6",
        "_frozen": True,
        "_generated_by": "make_token_profiles.py",
        "_note": ("Authoritative for all 10 languages. Reference lexers, where available, are audit "
                  "instruments only (02 section 4.5), so that languages lacking one are not counted "
                  "by a different instrument."),
        "languages": PROFILES,
    }
    with open(out, "w") as f:
        json.dump(doc, f, indent=1, sort_keys=False)
    print(f"wrote {out}")
    for k, v in PROFILES.items():
        print(f"  {k:12s} kw={len(v['keywords']):3d} ops={len(v['operators']):3d} "
              f"reflexer={'yes' if v['reference_lexer'].get('available') else 'N/A'}")


if __name__ == "__main__":
    main()
