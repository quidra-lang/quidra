# MANDATORY PRE-FLIGHT VALIDATION — I1 Keyword Anonymization, `rust` column

Methodology: `methodology/10_intrinsic_design.md` §10.3 (controlled random lexicalization),
§10.4 (reversibility and mandatory pre-flight validation), §5, §6, §7.1/§7.1a, §9;
`methodology/00_cross_language_constraints.md` rule C-4.

- **Language:** `rust` (the real language is never named in `reference_pack.md`).
- **Deterministic seed:** `20260918`.
- **Toolchain:** `rustc 1.95.0 (59807616e 2026-04-14) (Homebrew)`, host `aarch64-apple-darwin`,
  LLVM 22.1.3, `Darwin 25.4.0 arm64` (full output: `preflight_evidence/toolchain.txt`).
- **Build recipe (intrinsic-track, §0.1a checks-enabled form):**
  `rustc -O -C debug-assertions=on -C overflow-checks=on solution.rs -o solution`
- **Run recipe:** `./solution`
- **Gate field: `all_pass = true`.** No check failed. Nothing blocks the trial.

---

## 0. What was built

| Artifact | Purpose | SHA-256 |
|---|---|---|
| `mapping.json` | the keyword map, 9 tokens, drawn from seed 20260918 | `0c921fa89e00994c66ee6b765c19167a981779b2a71a15e0aaf985fbbbb39557` |
| `lexicalize.py` | PW-1 generator (§5.1–§5.3); regenerates `mapping.json` | `a63cdfc0978416e8487b0e4c43de28086beff7d91b0b107154d811ba05195612` |
| `lex10.py` | the single lossless lexer used in both directions (§6.1) | `c3f2fbf68c326dae58c6e2709f18367ab08ae93b8f826347faf61fa17d3eed84` |
| `forward.py` | real source → anonymized source | `e81e4d3ebff435b3bb36fcb9fa14586ad59512be13e66f39e62f6ff1ea7b30f5` |
| `inverse.py` | anonymized source → real source | `157ea939c997ac37ec41d54fe16a8b2fe4257b02fdad63006d2123054b803758` |
| `validate.py` | round-trip + conformance validator (M1, R1–R5, G1, G2) | `d8941ffa6162222ed823c86400f84808867f5096657a7b9e28bc08e698704581` |
| `harness.py` | extraction → gate → inverse → fixups → build → run → oracle | `83b0c524a6527c9d12408bda1f3a564c4621797ee75055523e5c579b71a0ac60` |
| `fixture_real.rs` | the worked fixture, real spelling | `7ee51a197c6c60ff19a42c2515d6faac1886d3816e54e07919f5e2274dbd9745` |
| `fixture_anon.rs` | the same fixture, anonymized spelling | `360d2a0db0f76f65cf322203f869578d6323bf85f14341b7ca8778f87d006849` |
| `fixture_anon.posmap.json` | the §5.5 unit position map (2 recorded insertions) | `27969492adb32703aa28fadc3ac6e5a627220b52270b2ae75b6321bafa09f164` |
| `expected_output.txt` | the oracle, **produced by execution** | `ed254b8ea2e6526c9d3f4e2312a91216a177f6fc0c017f53286a7cfe3a9a8609` |
| `reference_pack.md` | the anonymized-language description shown to the trial | `4b08995dedd676333aeebda01d35bd4f5cf3425993043ad8f59d93d124d6ccf3` |
| `wordlists/en_common.txt` | 27,939 natural-language words of length 4–6 | `54056731d43f793f8d0ea08285d4f736b18438694dc7c6813c1e3acbd1f01a00` |
| `wordlists/prog_terms.txt` | 278 common programming terms | `19489b1e573e8b1afe678ee290ce180b98c4023058ecca787913b2bd22fbe8ad` |
| `wordlists/reserved_union.txt` | 245 reserved words, union of the ten languages | `58805a7eec88eec073d01ec8ea97178f770b4b6569c5eb7b1cb022f1d8d2a38c` |
| `wordlists/reserved_rust.txt` | 52 reserved words of this language (gate input) | `be540f1e6bf8446ba46ef8189234db696cd0ee3fc27d7241034e826beb6cd927` |

### The mapping (9 tokens, `anonymized_token_count = 9`)

| Role | Real token | Pseudo-word | Job |
|---|---|---|---|
| K01 | `fn` | `sobini` | opens a function definition |
| K22 | `main` | `doginu` | the fixed entry-point name |
| K02 | `let` | `gutati` | introduces a named local binding |
| K24 | `mut` | `zefegi` | marks a binding reassignable |
| K21a | `i64` | `zikeza` | the 64-bit signed integer kind name |
| K21c | `String` | `kegudu` | the growable text kind name |
| K04 | `if` | `tiroro` | opens a conditional branch |
| K05 | `else` | `mozome` | opens the alternative branch |
| K08 | `while` | `mavenu` | opens conditional iteration |

Every pseudo-word is exactly 6 lowercase ASCII characters in the CVCVCV pattern, so
character length is matched exactly (point mass at 6, zero residual) and **no pseudo-word can
be a prefix of another**. Collision redraws recorded in `mapping.json`:
`K04:2, K08:2, K21c:1, K22:1, K24:1`. Rejections fired by filter: rule 6 (banned substring of
length ≥ 4) 7 times; the other filters were not reached for this small a domain, which is
recorded rather than papered over.

Standard-library names are **not** transformed in I1 by §7.1: `println`, `format` and `new`
keep their real spellings and are listed in `mapping.json → untransformed_by_design`.

---

## (a) The fixture compiles, runs, and its output matches EXACTLY what the pack claims

### (a.1) The real fixture and the oracle

```
$ rustc -O -C debug-assertions=on -C overflow-checks=on fixture_real.rs -o build/fixture_real
BUILD EXIT: 0          (stderr empty — preflight_evidence/pf01_build_stderr.txt)
$ ./build/fixture_real > expected_output.txt
RUN EXIT: 0
$ cat expected_output.txt
SUM 25632
MAX 935
EVENS 24
JOINED 897-558-614-577-405
```

Byte-level confirmation of the trailing newline (`xxd` tail):
`...4a4f 494e 4544 2038 3937 2d35 3538 2d36 3134 2d35 3737 2d34 3035 0a` — 54 bytes, 4 lines,
final `0a` present.

`expected_output.txt` was written by **redirecting the compiled program's stdout**, never by
hand. An independent generator written in a different language reproduces it exactly:

```
$ python3 -c "s=7; ... "
SUM 25632
MAX 935
EVENS 24
JOINED 897-558-614-577-405
```

### (a.2) The pack's worked example

The pack's P12 example is a different program from the fixture and makes an explicit output
claim. It was extracted from `reference_pack.md`, inverse-mapped, built and run
(`preflight_evidence/pack_example_run.txt`):

```
build exit: 0   (stderr empty)
run exit:   0
actual stdout:   'TOTAL 84\nLARGEST 49\nLISTED 0-1-4\n'
claimed in pack: 'TOTAL 84\nLARGEST 49\nLISTED 0-1-4\n'
MATCH: True
```

### (a.3) Round trip, and the source the toolchain actually sees

Only inverse-mapped source is ever compiled (§6.5). `validate.py` on the correct fixture:

```
  M1a  PASS   one-to-one: 9 pseudo-words, 9 distinct
  M1b  PASS   shape ^[a-z]{6}$: all conform
  M1c  PASS   no prefix relation: none
  M1d  PASS   no pseudo-word collides with a real token: none
  M1e  PASS   comparable length: character lengths = [6]
  G1   PASS   conformance gate H_REAL events: none
  G2   PASS   inverse ambiguity: none
  R1   PASS   byte identity of inverse(forward(F)) against F: identical
  R4   PASS   non-triviality: 29 tokens substituted
  R5   PASS   domain containment: every changed token is a bound keyword
  R2   PASS   build+run of the inverse-mapped source: exit=0
  R3   PASS   stdout byte-identical to the oracle: yes
VERDICT: ACCEPT
```

`R1` is **plain byte identity**, not a canonicalized comparison: the two whitespace characters
the §5.5 emission rule inserts (at offsets 13 and 204 of the transformed text, after `doginu`
and after `kegudu`) are recorded in `fixture_anon.posmap.json` and deleted by the inverse.

**Literal and comment safety** (`preflight_evidence/literal_safety.txt`): a fixture whose
string literals, raw string, character literal and comments all contain the real keywords was
transformed and inverted. The literals and comments came through untouched in both directions
and the round trip was byte-identical. This is what keeps program output unchanged (§1
consequence 2).

**Determinism** (`preflight_evidence/determinism.txt`): `lexicalize.py` was run in two separate
processes; both outputs and the committed `mapping.json` share SHA-256
`0c921fa8…bbb39557`.

---

## (b) Every withheld harness convention is satisfied BY THE HARNESS

### (b.1) The conventions, and who supplies them

| Convention | Value | Supplied by |
|---|---|---|
| entry filename | `solution.rs` | harness, fixup H1 |
| build command | `rustc -O -C debug-assertions=on -C overflow-checks=on solution.rs -o solution` | harness |
| run command | `./solution` | harness |
| working directory | fresh temporary directory per trial | harness |
| compilation-unit / module declaration | none required | n/a |
| imports | none required (the facilities are in the automatic prelude) | n/a |
| error-propagation marker on the output path (`K27`) | none required | n/a |

None of these is stated in `reference_pack.md`, and that is deliberate: the file extension
alone would name the language, which §2.6 forbids. §9.1 resolves this — the harness carries
every fact the prompt withholds, so the trial is never marked down for not guessing it.
Fixups H2–H6 are **not** available for pack-documented material (§9.3): the entry-point shape
*is* documented in P1, so a submission that omits it is a model failure, not a rescue case.

### (b.2) PF-13-style sufficiency evidence

A **bare-body submission** was written from `reference_pack.md` alone — it contains every
element the pack documents, in anonymized spelling, uses different identifier names from the
fixture, and omits only the unstated conventions. It was deliberately wrapped in prose with
**two** fenced blocks so the §9.2 extraction rule (take the last block) is exercised too.
Run through the real pipeline (`preflight_evidence/pf13_pipeline.txt`):

```
PIPELINE for bare_body_submission.txt
  extract      ok    2 fenced block(s); took the last one, 870 bytes
  gate         ok    H_REAL events: none
  ambiguity    ok    INVERSE_AMBIGUOUS: none
  inverse      ok    788 bytes of real source produced
  fixup H1     ok    wrote the submission to solution.rs (unstated convention)
  fixups H2-H6 ok    none applied; 0 pack-documented fixups
  build        ok    exit=0 stderr=''
  run          ok    exit=0 stderr=''
  oracle       ok    stdout byte-identical to expected_output.txt: yes
RESULT: PASS
```

**Pack-documented fixups applied: 0. Unstated-convention fixups applied: 1 (H1).**

### (b.3) Frozen rule C-4 — current stdout API versus the widely-reproduced older idiom

C-4 names two toolchains whose current stdout API differs from the idiom most reproduced in
training data (TypeScript 7's removal of `--outFile`; Zig 0.16's explicit `Io` instance), and
requires the Reference Pack to state the **current** spelling where that is true.

**Checked for this language, and it is not true here.** The line-writing facility this task
uses is spelled the same today as in every widely-reproduced older form: it is the same
name, the same trailing exclamation mark, the same `{}` hole syntax, and it needs no writer
object, no handle, no buffer, no allocator and no error-propagation marker. Verified by
building and running under the pinned toolchain (`rustc 1.95.0`) with the frozen recipe: the
fixture, the pack's P12 example and the bare-body submission all compile with **empty stderr**
and exit 0. Nothing about the output path is version-sensitive, so there is nothing for the
pack to warn about and `K27` is correctly `NOT_LEXICALIZED`.

The pack nevertheless states the spelling completely and mechanically in P10 — the literal,
the `{}` holes, the comma-separated values, that the line terminator is appended by the
facility itself, and that the trailing exclamation mark is part of the name — so a trial
cannot fail on the output API regardless of what it remembers.

---

## (c) The round-trip validator BOTH accepts a correct input AND rejects a corrupted one

Full transcript: `preflight_evidence/validator_runs.txt`. Every run below is real output.

| # | Input | Expected | Actual | Caught by |
|---|---|---|---|---|
| P | `fixture_anon.rs` (correct) | ACCEPT | **ACCEPT**, exit 0 | — |
| N1 | correct **real** source, ignoring the transformation entirely | REJECT | **REJECT**, exit 1 | `G1` |
| N2 | one pseudo-word swapped for another valid pseudo-word | REJECT | **REJECT**, exit 1 | `R2`, `R3` |
| N3 | a pseudo-word mistyped into an unmapped word | REJECT | **REJECT**, exit 1 | `R2`, `R3` |
| N4 | a corruption that **builds cleanly** and prints the wrong answer | REJECT | **REJECT**, exit 1 | `R3` |
| N5 | a `mapping.json` that is not one-to-one | REJECT | **REJECT**, exit 1 | `M1a`, `R2`, `R3` |
| N6 | an identifier spelled as a pseudo-word | REJECT | **REJECT**, exit 1 | `G2` |

Key excerpts:

```
NEGATIVE N1  (PF-05(b): correct real source that ignores the transformation)
  G1   FAIL   conformance gate H_REAL events:
              ['String', 'else', 'fn', 'i64', 'if', 'let', 'main', 'mut', 'while']
  VERDICT: REJECT

NEGATIVE N2
  R2   FAIL   build+run of the inverse-mapped source: exit=1
              error: expected expression, found keyword `mut`
  VERDICT: REJECT

NEGATIVE N4  (the silent-bug shape: builds, exits 0, wrong answer)
  R2   PASS   build+run of the inverse-mapped source: exit=0
  R3   FAIL   stdout byte-identical to the oracle:
              NO -> 'SUM 25632\nMAX 935\nEVENS 24\nJOINED 558-614-577-405\n'
  VERDICT: REJECT

NEGATIVE N6  (INVERSE_AMBIGUOUS: the program still builds and still prints the right answer)
  R2   PASS   ...exit=0
  R3   PASS   stdout byte-identical to the oracle: yes
  G2   FAIL   inverse ambiguity: ['kegudu']
  VERDICT: REJECT
```

The same negative was also driven through the full trial pipeline, so the rejection is shown
at the place a scored trial would meet it, not only inside the validator:

```
PIPELINE for negative_submission_real_source.txt
  extract      ok    1 fenced block(s); took the last one, 787 bytes
  gate         FAIL  H_REAL events: ['String','else','fn','i64','if','let','main','mut','while']
RESULT: FAIL
```

### On §10.4's vacuity warning

§10.4 and PF-05(a) warn that a *"did any transformed token survive?"* test is vacuous when the
mapping is a permutation of the language's own vocabulary — the I6 case. **That warning does
not apply to this mapping** (the codomain is nine invented CVCVCV strings, disjoint from the
language's vocabulary), but the survival test alone would still be weak here, because it
cannot distinguish a wrong-but-still-pseudo-worded program from a right one. The validator
therefore does not rely on it: N2, N3 and N4 contain **no** real token at all and pass the
survival test trivially, yet are rejected — N2/N3 because the inverse-mapped program fails to
build, N4 because it builds and runs cleanly and prints the wrong bytes. N6 passes the
survival test, builds, runs and prints the *correct* bytes, and is still rejected. The
validator's rejecting power comes from behaviour and from role position, not from token
census.

---

## Residuals, published rather than corrected

1. **Anonymity residual (§2.6 exemption, §13.2).** I1 transforms grammar words only, so the
   standard-library spellings `println!`, `format!`, `::new()` and the `{}` interpolation
   holes stay real in the pack. A reader who already knows the language can still recognize
   it from those. This is a designed property of I1, not a defect, and it is recorded here
   rather than fixed by transforming V-roles (which would make this I2).
2. **Leak scan.** A whole-word, case-insensitive scan of `reference_pack.md` against the 52
   reserved words of the real language, against the three bound non-reserved tokens
   (`i64`, `String`, `main`) and against 20 language/tool/extension names returns **2 hits**,
   both the English word "in" inside the frozen §2.2 sentence
   `(this language has no construct in this category)`. That sentence is frozen verbatim and
   identical in all ten packs, so it carries no language signal. Zero hits otherwise.
   The stricter reading of §2.6 — scanning prose against the *union* reserved list of all ten
   languages — is not realizable in English prose (that list contains `in`, `is`, `of`, `it`,
   `any`, `some`, `from`, `with`, `set`, `get`, `type`, `use`, `where`, `when`, `data`,
   `value`, `range`, `record`, `test`), so the realized criterion is: zero occurrences of any
   reserved word of the **real** language anywhere in the pack, and zero union-list words
   inside code blocks. Recorded here so the criterion actually applied is auditable.
3. **Model-tokenizer residual (§5.4).** `model_tokenizer_available: false`. Pseudo-word
   matching is exact in characters (all 6) and in frozen lexical tokens (all 1); how the
   benchmark model's BPE segments a given six-letter string is unknown and unmeasurable here.
4. **Emission-rule cosmetics.** The §5.5 rule inserts a single space where a substituted token
   had no adjacent whitespace, so the anonymized fixture reads `doginu ()` and
   `kegudu ::new()`. Both are legal; P1 of the pack states that whitespace between tokens is
   free, so the spacing teaches nothing false. The insertions are recorded and exactly
   reversed, which is what lets R1 stay a plain byte-identity test (§6.4).
5. **Scope.** This deliverable covers the single LCG task stated in `task_statement.txt`, not
   the five-task CORE set, so the pack carries one worked example rather than the six E1–E6
   slots of §2.3, and the §3.1 roles the task does not need (`K07` bounded iteration, `K11`
   result, `K12`–`K13` aggregates, `K14`–`K15` imports/modules, `K16`–`K20` booleans and word
   operators, `K21b`/`K21d`/`K21e`, `K23`, `K25`–`K27`) are recorded `NOT_LEXICALIZED` with a
   reason in `mapping.json` rather than bound. Extending to the full CORE set requires
   re-running `lexicalize.py` with the enlarged role set and re-running this pre-flight in
   full (§10.2: partial re-validation is not permitted).

---

## Verdict

| Check | Result |
|---|---|
| (a) fixture compiles, runs, output matches the pack exactly | **PASS** |
| (a) pack's worked example compiles, runs, output matches its claim exactly | **PASS** |
| (a) `expected_output.txt` produced by execution, cross-checked independently | **PASS** |
| (a) round trip real → forward → inverse is byte-identical (R1), non-trivial (R4), domain-contained (R5) | **PASS** |
| (b) entry filename and build command supplied by the harness; 0 pack-documented fixups | **PASS** |
| (b) frozen rule C-4 examined; no stdout API drift for this language; spelling stated in P10 anyway | **PASS** |
| (c) validator accepts the correct input | **PASS** |
| (c) validator rejects 6 distinct corruptions, including one that builds and runs cleanly | **PASS** |

**`all_pass: true`.** Nothing blocks the trial.
