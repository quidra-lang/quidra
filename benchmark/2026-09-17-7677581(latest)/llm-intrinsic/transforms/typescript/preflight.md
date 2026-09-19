# MANDATORY PRE-FLIGHT VALIDATION — I1 (Keyword Anonymization), `language_id = typescript`

Unit `I1/s20260918`. Seed **20260918**. Host toolchain `tsc 7.0.2` (`/opt/homebrew/bin/tsc`)
and `node v24.2.0` (`/opt/homebrew/bin/node`), the frozen build and run recipes of
`environment.json` (`tsc FILE.ts`, then `node FILE.js`). All three checks of methodology 10
§10.4 are recorded below with the commands actually executed and their captured output. Raw
transcripts:
`preflight/A_fixture_and_oracle.txt`, `preflight/B_roundtrip_and_pipeline.txt`,
`preflight/C_validator_selftest.txt`, `preflight/D_leak_and_determinism.txt`,
`preflight/E_c4_current_api.txt`.

**Gate: `all_pass = true`.** No check failed. No blocking problem is outstanding.

---

## 0. Artefacts

| File | What it is |
|---|---|
| `mapping.json` | the 17-token keyword map, seed + primitives + word-list SHA-256 recorded |
| `gen_mapping.py` | deterministic generator (FNV-1a 32 → Park–Miller → PW-1 CVCVCV) |
| `forward.py` | real → anonymized, whole word tokens only; regex-driven lexer |
| `inverse.py` | anonymized → real, whole word tokens only; independent character-loop lexer |
| `validate.py` | round-trip + gate + submission validator, with `selftest` (positives **and** negatives) |
| `fixture_real.ts` / `fixture_anon.ts` | the worked fixture, real / anonymized (`fixture_anon.ts` is byte-for-byte `forward(fixture_real.ts)`) |
| `fixture_anon.ts.posmap.json` | the §5.5 position map for that forward pass (empty; see §3, case `P4`) |
| `fixture_expected_output.txt` | what the fixture prints, produced by executing it |
| `reference_solution.ts` | the real, non-anonymized implementation of the task; **never shown to a trial** |
| `expected_output.txt` | the oracle, produced by executing `reference_solution.ts` |
| `reference_pack.md` | the Reference Pack shown to the trial |
| `reference_pack.counts.json` | its published line and character counts |
| `task_statement.txt` | the language-neutral task text |
| `preflight_bare_submission_anon.ts`, `preflight_bare_submission_toplevel_anon.ts` | two PF-13-shaped bare-body submissions written from the pack alone |
| `wordlists/` | `en_common.txt`, `prog_terms.txt`, `reserved_union.txt`, `reserved_typescript.txt` (+ SHA-256 in `mapping.json`) |

### The keyword map

Domain = every `BOUND` K-role token this task needs, per §7.1/§7.1a. V-roles
(`console.log`, `push`, `join`, `length`, `String`) keep their real spellings in I1 by
design (§7.1), which is the published anonymity residual of this condition (§2.6 exemption).

| real | pseudo | | real | pseudo | | real | pseudo | | real | pseudo |
|---|---|---|---|---|---|---|---|---|---|---|
| `function` | `vefedi` | | `if` | `memagu` | | `break` | `zeligu` | | `number` | `gebamu` |
| `return` | `vudoru` | | `else` | `ninobi` | | `continue` | `bedara` | | `boolean` | `zisaro` |
| `const` | `vobunu` | | `for` | `padiri` | | `true` | `gerido` | | `string` | `bunivu` |
| `let` | `busopo` | | `of` | `kubuva` | | `false` | `zaluga` | | `void` | `zerinu` |
| | | | `while` | `biremo` | | | | | | |

All 17 pseudo-words: exactly 6 ASCII lowercase characters, CVCVCV, one-to-one, no prefix
relation (equal length + distinct ⇒ impossible), none an English word, a common programming
term, or a reserved word of any of the ten benchmark languages, none containing any such
entry of length ≥ 4 as a substring, none occurring anywhere in the task material. Collision
redraws: `const` 1, `if` 2, `for` 1, `continue` 1, `return` 1, `true` 1 — recorded in
`mapping.json`.

Determinism (two separate processes, `preflight/D_leak_and_determinism.txt`):

```
$ python3 gen_mapping.py >/dev/null ; shasum -a 256 mapping.json
39a0a8db0e223b514890807d800b8edabbb9ddbeb76d5a7933616d55ce9c574a  mapping.json
$ python3 gen_mapping.py >/dev/null ; shasum -a 256 mapping.json
39a0a8db0e223b514890807d800b8edabbb9ddbeb76d5a7933616d55ce9c574a  mapping.json
```

**Domain completeness, recomputed mechanically** (PF-03 shaped, `D4` in the same transcript).
The reserved words occurring in `fixture_real.ts` and `reference_solution.ts` are
`const else for function if let number of return string void`; every one of them is mapped, so
`unbound_word_tokens` is **empty**. Six further tokens — `boolean break continue false true
while` — are mapped and documented in the pack's word-token table because the pack must
describe conditional iteration, early exit, next-iteration and the truth type (§2.2 P4/P6)
even though this task's fixtures do not exercise them. No user identifier in either file
equals any mapped token (`identifiers that collide with a mapped token: []`), so the §6.1
lexer ambiguity never arises inside the fixtures.

### Reference Pack size

`reference_pack.md`: **250 lines, 10,245 characters** (10,263 bytes; the difference is the
multi-byte `…` glyph). At the top of the 150–250-line target, not over it. It names no
language, no toolchain, no file extension, and contains **zero** occurrences of any of the 17
real keywords — verified mechanically, §3 below.

---

## 1. Check (a) — the fixture compiles, runs, and its output matches the pack EXACTLY

The worked fixture deliberately is **not** the task solution: it demonstrates every construct
the task needs (routine definition with typed parameters and result, a two-branch conditional,
both loop shapes, an accumulator, multiplication and remainder, sequence construction,
appending and indexing, numeric→text conversion, joining with a separator, four output lines)
over different constants with a different output shape. Handing a trial the answer would have
measured nothing.

```
$ tsc fixture_real.ts                              -> exit 0
$ node fixture_real.js
TOTAL 346
BIGGEST 93
ODDS 4
LABEL 33:72:16
                                                   -> exit 0
```

The Reference Pack, §10, claims exactly:

```
TOTAL 346
BIGGEST 93
ODDS 4
LABEL 33:72:16
```

Verified mechanically, not by eye — self-test cases `P6` and `P7`: the worked-example code
block extracted from `reference_pack.md` is **byte-identical** to `fixture_anon.ts`, and the
claimed-output block is **byte-identical** to the bytes `fixture_real.ts` actually writes.
`fixture_anon.ts` is in turn byte-identical to `forward(fixture_real.ts)` — case `P4`.

**The oracle was produced by execution, never by hand:**

```
$ tsc reference_solution.ts                        -> exit 0
$ node reference_solution.js > expected_output.txt  -> exit 0
$ cat expected_output.txt
SUM 25632
MAX 935
EVENS 24
JOINED 897-558-614-577-405
$ shasum -a 256 expected_output.txt
ed254b8ea2e6526c9d3f4e2312a91216a177f6fc0c017f53286a7cfe3a9a8609
```

Cross-checked against two re-computations of the Park–Miller stream that share no code with
the reference solution — an `awk` implementation using double arithmetic (exact here, since
the largest intermediate `48271 × 2147483646 ≈ 1.04e14` is well inside 2^53) and an
arbitrary-precision one. All three agree byte for byte. First term by hand:
`7 × 48271 = 337897`, `337897 mod 2147483647 = 337897`, `337897 mod 1000 = 897` — the first
field of `JOINED`.

`PASS`.

---

## 2. Check (b) — every withheld harness convention is satisfied BY THE HARNESS

| Convention | Who supplies it | How |
|---|---|---|
| entry filename `solution.ts` | **harness** (fixup H1) | the trial submits one source text; the harness writes it to `solution.ts` |
| build command `tsc solution.ts` | **harness** | frozen build recipe, `environment.json` |
| emitted artifact `solution.js` | **harness** (fixup H6) | produced by the build step beside the source |
| run command `node solution.js` | **harness** | frozen run recipe, §0.1 row 7 |
| working directory, stdout capture, 10 s timeout, exit-status check | **harness** | oracle §4.2 |

The trial is never asked to guess any of these, and is never penalised for not stating them.
The pack's §11 says so in language-neutral prose: *"the harness writes it to the entry file,
builds it, then runs it; you never state a file name, a command, nor any build option."*

**Deliberate design point, recorded rather than hidden.** The literal filename `solution.ts`
is *not* written into the Reference Pack, because the extension alone identifies the language
and would fail the §2.6 identity-leak scan. It lives here, in the harness record, and is
applied by fixup H1. This satisfies §10.4 requirement (b) — the fact is supplied by the
harness, which is exactly what the requirement asks — without breaking anonymity. No fixup in
this unit supplies pack-documented material: the pack documents no module header, no
entry-point declaration and no import, because this language needs none for a single-file
program (`mapping.json → not_lexicalized` records `K14`, `K15`, `K22`, `K23` with reasons), so
the pack-documented fixup column is structurally **zero** for this language.

**Frozen rule C-4 (methodology 00) — applied, with execution evidence**
(`preflight/E_c4_current_api.txt`). C-4 names this toolchain explicitly. Three facts were
checked by running the toolchain, not by recall:

1. **The output spelling the pack states is the current one and it works.** The pack's §7 and
   §9 give exactly one spelling for writing a line, and the identical text compiled and ran
   through the frozen recipes: `tsc solution.ts` exit 0, `node solution.js` printing `SUM 2`,
   exit 0. The pack states it as current and forecloses alternatives — *"This call is the
   current, correct spelling that writes a line in this language; there is no other, and no
   older spelling is accepted."* The task is about the generator arithmetic, not the output
   API, so a trial cannot fail on that spelling.
2. **The drift C-4 actually records for this toolchain is a build option, and the harness owns
   it.** The older, widely-reproduced emission idiom now fails outright:
   `tsc --outFile bundle.js solution.ts` → exit 2, `error TS5102: Option 'outFile' has been
   removed.` A trial never writes a build option (pack §11), so this drift cannot reach it.
3. **A second toolchain fact was found during this pre-flight and closed the same way.** Under
   the frozen recipe the default declaration set reserves a handful of ordinary names at the
   outermost scope: `const name: string = "x";` at top level fails with
   `error TS2451: Cannot redeclare block-scoped variable 'name'`, exit 2. That has nothing to
   do with the task either, so the pack's §1 states the shape that avoids it — *"Names bound at
   the top level share one space with names the surrounding environment already provides, so
   keep your working names inside a routine and call that routine"* — and the worked example
   demonstrates that shape. This is a statement of a real scoping property, not an extra
   explanation: it occupies the P1 "program shape" slot every pack fills.

**Evidence that this is sufficient in practice (a PF-13-shaped test).** Two *bare-body
submissions written from the pack alone*, both deliberately in different styles from the
reference solution, were fed through the real pipeline (gate → inverse → write `solution.ts` →
`tsc` → `node` → byte-compare against `expected_output.txt`):

- `preflight_bare_submission_anon.ts` — conditional iteration (`biremo`) instead of a bounded
  loop, the sequence-walking loop shape, and the interpolation form from pack §2 for the last
  line;
- `preflight_bare_submission_toplevel_anon.ts` — no routine at all, everything at the top
  level, the separator built by hand with a chained alternative instead of the join facility.

```
$ python3 validate.py gate preflight_bare_submission_anon.ts                       CLEAN
$ python3 validate.py submission preflight_bare_submission_anon.ts expected_output.txt
ACCEPTED
$ python3 validate.py gate preflight_bare_submission_toplevel_anon.ts              CLEAN
$ python3 validate.py submission preflight_bare_submission_toplevel_anon.ts expected_output.txt
ACCEPTED
```

and, showing that only inverse-mapped source ever reaches the compiler (§6.5):

```
$ python3 inverse.py preflight_bare_submission_anon.ts /tmp/bare_ts/solution.ts
$ (cd /tmp/bare_ts && tsc solution.ts && node solution.js)
SUM 25632
MAX 935
EVENS 24
JOINED 897-558-614-577-405                        build exit 0, run exit 0
```

A trial that uses only what the pack states, and states no convention of its own, passes —
in two quite different styles, with zero pack-documented fixups applied.

`PASS`.

---

## 3. Check (c) — the round-trip validator BOTH accepts a correct input AND rejects a corrupt one

`python3 validate.py selftest` (full transcript: `preflight/C_validator_selftest.txt`, exit 0).

**Positives — all ACCEPTED:**

| case | what it checks |
|---|---|
| `P1` | `inverse(forward(fixture_real.ts))` byte-identical (R1), transform non-trivial (R4, 11 tokens changed), every changed token inside the bound domain (R5) |
| `P2` | the same three, on `reference_solution.ts` (10 tokens changed) |
| `P3` | `fixture_anon.ts` through gate → inverse → build → run → byte-compare |
| `P4` | `fixture_anon.ts` == `forward(fixture_real.ts)`, with **0** inserted whitespace characters |
| `P5` | `forward(reference_solution.ts)` through the same full pipeline |
| `P6` | the pack's worked example == `fixture_anon.ts`, byte for byte |
| `P7` | the pack's claimed output == what `fixture_real.ts` prints, byte for byte |

**Negatives — all REJECTED, each with the reason printed:**

| case | corruption | rejected with |
|---|---|---|
| `N1` | one pseudo-word swapped back to the real keyword it stands for | `GATE_H_REAL:if` |
| `N2` | **correct real source that ignores the transformation entirely** (§4.2a / PF-05 b) | `GATE_H_REAL:const,for,function,if,let,number,of,return,string,void` |
| `N3` | a pseudo-word mistyped into an undefined name | `COMPILE_FAILED:rc=2:...error TS1434` |
| `N4` | source using a pseudo-word as a user identifier (injectivity) | `FORWARD_COLLISION` |
| `N4b` | a submission naming a variable with a pseudo-word: the `INVERSE_AMBIGUOUS` path of §6.1 | `COMPILE_FAILED:rc=2:...error TS1440` |
| `N5` | token-clean, gate-clean, compiles, runs — arithmetic altered (`48271` → `48273`) | `OUTPUT_MISMATCH` |
| `N6` | a transform that reached inside a text literal | `R1_BYTE_IDENTITY_FAILED` |
| `N7` | correct four lines plus one extra output line | `OUTPUT_MISMATCH` |

`N5` and `N7` are the cases that answer §10.4's warning directly. A *"did any transformed token
survive?"* test is vacuous when the mapping is a permutation of the language's own vocabulary.
It is not vacuous here — the pseudo-words are provably not tokens of this language, filter 5 of
`accept()` guarantees it — but it is still weak on its own, so the validator does not stop at
tokens: it requires the inverse image to compile, to run to exit 0 with empty standard error,
and to emit the exact expected bytes. `N5` and `N7` pass every token-level check and are still
rejected, which is what shows the token test is not carrying the verdict alone. `N2` is the
§4.2a gate's mandated negative: a fully correct, familiar-looking program that ignores the
anonymization is rejected, so pretraining recall cannot substitute for reading the pack.
`N6` shows what a literal-reaching transformer would cost: the text `"for the record"` comes
back as `"padiri the record"`, and R1 catches it.

The gate's surface is `reserved_typescript.txt ∪ {the 17 mapped tokens}` minus the V-role names
the pack documents untransformed. `reserved_typescript.txt` is a per-language list built for
this unit because the frozen `reserved_union.txt` is the union across all ten languages and
does not carry this language's contextual grammar words (`of`, `boolean`). It deliberately
**omits** words that this language reserves only in narrow positions and that models commonly
use as ordinary names (`get`, `set`, `from`, `is`, `out`, `module`, `global`, `object`,
`symbol`, `constructor`, `require`, `assert`, `using`, `accessor`): including them would reject
correct submissions for naming a variable `out`, which is a false gate failure and would be
scored as a language failure. The omission is recorded here, not implied.

**Rejection-filter liveness.** The self-test calls the *actual* `accept()` of `gen_mapping.py`
on one probe per filter and on one control:

```
[PASS] filter1 shape      ab3de    accept()=False   (not ^[a-z]{6}$)
[PASS] filter2 collision  gebamu   accept()=False   (already used)
[PASS] filter3 english    banana   accept()=False   (in en_common.txt)
[PASS] filter4 progterm   buffer   accept()=False   (in prog_terms.txt)
[PASS] filter5 reserved   static   accept()=False   (in reserved_union.txt)
[PASS] filter6 substring  zovoid   accept()=False   (contains 'void')
[PASS] filter7 material   bigger   accept()=False   (occurs in the fixture material)
[PASS] control accepted   gidopu   accept()=True    (trips no filter)
```

During the actual draw only filter 6 fired (7 rejections): for candidates of length 6 it
subsumes filters 3–5, since an exact list hit is also a substring hit. That is why the liveness
probes above are run explicitly — otherwise four of the seven filters would never be observed
to work.

**Identity-leak scan** (`validate.py leakscan`, transcript `D5`): 250 lines scanned, 17 real
keywords and 75 identity terms checked (language names, toolchains, file extensions, ecosystem
markers) — **zero hits**. The keyword scan runs twice, once case-sensitively (the language is
case-sensitive, so `String` is not the type keyword `string`) and once ignoring case with the
pack-documented V-role names exempted, so a capitalised keyword could not slip through.

`PASS`.

---

## 4. Residuals, published rather than corrected

1. **V-role surface is real in I1 by design** (§7.1): `console.log`, `push`, `join`, `length`
   and `String` keep their true spellings, so the pack remains recognisable to a reader who
   already knows this language. That is the condition's known anonymity residual (§2.6
   exemption, §13.2), not a defect in this unit.
2. **Interpolation (`V18`) is `NOT_TRANSFORMABLE`** (§3.2): its syntax lies inside a text
   literal, which §1 consequence 2 never transforms. The pack documents it, and adds the one
   clause that keeps it safe under the inverse transformer — *"a word token from section 3
   never appears inside a literal in any form"* — so a trial cannot bury a pseudo-word where
   the inverse pass will not reach it. `preflight_bare_submission_anon.ts` exercises the
   interpolation path end to end and is `ACCEPTED`.
3. **The frozen task statement contains two of this language's reserved words as ordinary
   English** — `for` and `of` in `task_statement.txt` (`D5`, 2 hits). The statement is
   language-neutral prose, byte-identical for all ten languages, so it carries no identity
   information whatever; rewording the frozen statement to suit one language would itself be
   the fairness violation. Recorded here rather than removed. The Reference Pack, which is the
   only language description a trial receives, is clean at 0 hits — reaching that required
   writing its prose without the English words `for`, `of`, `if`, `else`, `while`, `break`,
   `continue`, `return`, `true`, `false`, `number`, `string`, `boolean`, `void`, `const`,
   `let` and `function` anywhere, in any capitalisation.
4. **`anonymized_token_count = 17`** for this language and task. Published, not quota-matched
   (§7.1). Six of the 17 (`boolean break continue false true while`) are documented in the
   pack but unexercised by the fixtures, for the §2.2 slot reasons given in §0 above.
5. **`K21d` (dynamic-sequence type name) is `NOT_WORD_TOKEN` here**: this language spells the
   sequence type as a punctuation suffix on the element type, so it consumes no I1 budget. The
   pack documents the punctuation form in §4. Likewise `K18`/`K19`/`K20` (logical conjunction,
   disjunction, negation) and `K25` (type-annotation introducer) are punctuation, and `K06`
   (chained alternative) is spelled as the `K05` token followed by the `K04` token. All are
   recorded in `mapping.json → not_lexicalized` with reasons.
6. **Regular-expression literals are lexed heuristically.** Both lexers decide `/` by the
   preceding significant token, the standard rule. No task construct needs a regular
   expression and the pack documents none, so the path is never taken by a conforming
   submission; the heuristic is present so that a non-conforming one cannot corrupt the
   inverse image silently.
7. **The §5.5 position map is empty for every fixture in this unit** (`P4`). In this language
   two word tokens can never be adjacent without whitespace already between them, so the
   emission rule never has to insert anything. The recording machinery is implemented and
   exercised anyway, because a transformer that *cannot* record an insertion would hide the
   defect rather than prove it absent.
