# Meaning Per Token (MPT)

MPT measures the density of independently verifiable semantic information in a
source program:

```text
MPT = meaning count / token count
```

**Higher is better**, provided the compared programs satisfy the same task.
A shorter program is not automatically more meaningful or safer.

## Files

- `tokenizer.py` — language-neutral token counter and minimal colored HTML renderer.
- `meaning.md` — rubric for the AI-reviewed meaning count.
- `README.md` — this measurement procedure.

The tokenizer knows nothing about meanings or MPT scores. Counting meanings and
computing MPT are separate review steps.

## 1. Count tokens

Requires **Python 3.10+** and no third-party packages. From the repository root:

```sh
python3 mpt/tokenizer.py example.qui example.py example.ijs
```

Standard output has **exactly one integer per input program**, in input order:

```text
128
143
95
```

The same command creates or overwrites **`token.html` in the current working
directory**. It displays only token-colored source code and line numbers on the
left: no score, legend, controls, or additional analysis. With multiple input
programs, the HTML shows separate unlabeled code blocks in argument order.
The generated HTML is an output artifact, not one of the three tracked files.

Read one program from standard input with `-`, or with no file arguments:

```sh
printf 'x + y' | python3 mpt/tokenizer.py -
# stdout: 5
```

The same lexical rules apply to every source file; there is no language-mode
switch. The shared rule set covers the benchmark's Quidra, Python, C++, Rust,
Go, Java, TypeScript, Kotlin, Swift, and Zig, plus J and Julia.

### Counting conventions

| Source unit | Tokens |
| --- | ---: |
| Alphanumeric identifier part (e.g. `real32`, `sample`, `Count`) | 1 |
| Each underscore `_` in an identifier | 1 |
| Camel/Pascal boundary (e.g. `sampleCount`, `HTTPServer`) | Split into word parts |
| Each decimal digit, including in signed and multi-digit numeric literals | 1 per digit |
| Hexadecimal digit (`0`-`9`, `A`-`F`) within a `0x` numeric literal | 1 per digit |
| Numeric punctuation/signs (e.g. `-`, `_`, `.`) | 1 per mark |
| Opening and closing string delimiters | 1 each (also for triple/raw delimiters) |
| String-literal prefix (`r`, `f`, `u8R`, etc.) | 1 |
| Consecutive ASCII letters *inside* a string (e.g. `Hello`) | 1 per run |
| Each digit, punctuation mark, or ordinary space *inside* a string | 1 each |
| Non-ASCII text inside strings | 1 per Unicode code point |
| Single operator or delimiter (`+`, `(`, `]`) | 1 |
| Recognized multi-character operator (`>=`, `*:`, `.^`) | 1 |
| Composed operators such as J's `+/` | 2 (`+` and `/`) |
| Ordinary ASCII space or tab | 1 per character |
| Leading indentation: four ASCII spaces | 1 per group of four |
| Leading indentation: six ASCII spaces | 3 (one group + two spaces) |
| Line ending (LF, CRLF, or CR) | 1 |

Examples: `137` = 3 tokens, `-137` = 4, `"Hello"` = 3,
`"Hello World"` = 5, `"ABC123"` = 6, and `"Hello, World!"` = 7.
The literal contents are counted from their **source spelling**, so backslash
escapes and interpolation punctuation are not interpreted or expanded.
Identifiers **outside strings** are split at every underscore, lowercase-to-
uppercase transition, digit-to-uppercase transition, and acronym-to-word
boundary (e.g. `HTTPServer` -> `HTTP` + `Server`). An alphanumeric part that
starts with a letter stays whole (`real32` = 1 token); numeric-only parts after
underscores still count one digit each. For instance, `sample_count` = 3,
`sampleCount` = 2, `_value` = 2, `value_2` = 3, and `__init__` = 5 tokens.
Inside quoted strings, consecutive ASCII letters remain one token regardless
of case (`"HelloWorld"` = 3); underscores and numbers still split normally.

Whitespace is counted, including blank lines and the final newline if present.
Comments are **not discarded**; for fair comparisons, remove nonessential
comments and apply consistent formatting to all programs. This is a
reproducible cross-language counting convention, not a full parser or an LLM's
model-specific tokenizer.

## 2. Count meanings with AI

Ask an AI reviewer to read each program and follow [`meaning.md`](meaning.md).
Record the visible scope, observation points, semantic basis, and a common set
of questions before scoring. Require a section-by-section ledger with evidence
and a score of 1 for each distinct guaranteed fact, or 0 when it is unknown.
Include relevant types, shapes, mutation authority, and enforced error conditions.
Credit explicit and inferred guarantees equally; a known reference model earns
the same point as a known value model. The reviewer must not invent guarantees
or count the same fact twice.

For example, the Quidra type-and-binding fragment:

```text
tensor<real32>{nat N, N} matrix
```

has **5 meanings** under the worked example in `meaning.md`: one name-binding
fact and four type/shape facts. The shape checker is evidence for a constraint,
not an additional meaning. The fragment has **14 tokens** under `tokenizer.py`
(with no trailing newline).

## 3. Calculate MPT

For a program with 5 meanings and 14 tokens:

```text
MPT = 5 / 14 = 0.357142857...
```

Build one deduplicated fact ledger for the whole program, then allocate each
fact to exactly one logical section. Compute the whole-program MPT from that
ledger and the complete token count, not the average of section ratios:

```text
section MPT = facts allocated to section / section tokens
program MPT = distinct whole-program facts / complete program tokens
```

Summing section meanings gives the program numerator only when the sections
use the same whole-program scope and their allocated facts do not overlap.
Do not sum independently scored local fragments: a larger visible scope can
resolve an unknown, while two fragments can repeat the same guarantee.

Document how newlines between sections are assigned so that section token
counts add up to the complete program. Do not compute an MPT for an empty
source (zero tokens).

Scores are comparable only when the tokenizer rules, meaning rubric, formatter,
input assumptions, and required behavior are the same. Record any
language-specific semantic differences rather than treating unequal programs
as identical.
