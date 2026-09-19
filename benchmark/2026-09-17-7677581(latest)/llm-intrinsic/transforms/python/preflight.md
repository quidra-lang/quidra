# MANDATORY PRE-FLIGHT VALIDATION — I1 (Keyword Anonymization), `language_id = python`

Unit `I1/s20260918`. Seed **20260918**. Host toolchain `Python 3.9.6 (Clang 17.0.0)`,
`/usr/bin/python3`. All three checks of methodology 10 §10.4 are recorded below with the
commands actually executed and their captured output. Raw transcripts:
`preflight/A_fixture_run.txt`, `preflight/B_roundtrip_and_pipeline.txt`,
`preflight/C_validator_selftest.txt`, `preflight/D_leak_and_determinism.txt`.

**Gate: `all_pass = true`.** No check failed. No blocking problem is outstanding.

---

## 0. Artefacts

| File | What it is |
|---|---|
| `mapping.json` | the 20-token keyword map, seed + primitives + word-list SHA-256 recorded |
| `gen_mapping.py` | deterministic generator (FNV-1a 32 → Park–Miller → PW-1 CVCVCV) |
| `forward.py` | real → anonymized, whole word tokens only |
| `inverse.py` | anonymized → real, whole word tokens only, self-contained lexer |
| `validate.py` | round-trip + submission validator, with `selftest` (positives **and** negatives) |
| `fixture_real.py` / `fixture_anon.py` | the worked fixture, real / anonymized (`fixture_anon.py` is byte-for-byte `forward(fixture_real.py)`) |
| `reference_solution.py` | the real, non-anonymized implementation of the task; **never shown to a trial** |
| `expected_output.txt` | the oracle, produced by executing `reference_solution.py` |
| `reference_pack.md` | the Reference Pack shown to the trial |
| `task_statement.txt` | the language-neutral task text |
| `wordlists/` | `en_common.txt`, `prog_terms.txt`, `reserved_union.txt` (+ SHA-256 in `mapping.json`) |

### The keyword map

Domain = every `BOUND` K-role token this task needs, per §7.1/§7.1a. V-roles
(`print`, `range`, `len`, `append`, `join`) keep their real spellings in I1 by design
(§7.1), which is the published anonymity residual of this condition (§2.6 exemption).

| real | pseudo | | real | pseudo | | real | pseudo | | real | pseudo |
|---|---|---|---|---|---|---|---|---|---|---|
| `def` | `tukoki` | | `if` | `muveze` | | `and` | `nonida` | | `int` | `digami` |
| `return` | `lodira` | | `elif` | `lupire` | | `or` | `zevepa` | | `bool` | `sedagi` |
| `for` | `zomagu` | | `else` | `nuragi` | | `not` | `fasami` | | `str` | `galozi` |
| `in` | `kufumo` | | `break` | `zobumo` | | `True` | `nogelu` | | `list` | `vorito` |
| `while` | `vigifo` | | `continue` | `tezumi` | | `False` | `rogido` | | `None` | `tivuni` |

All 20 pseudo-words: exactly 6 ASCII lowercase characters, CVCVCV, one-to-one, no
prefix relation (equal length + distinct ⇒ impossible), none an English word, a common
programming term, a reserved word of any of the ten benchmark languages, none containing
any such entry of length ≥ 4 as a substring, none occurring anywhere in the task material.
Collision redraws: `elif` 2, `continue` 1, `False` 4, `or` 1 — recorded in `mapping.json`.

Determinism (two separate processes, `preflight/D_leak_and_determinism.txt`):

```
$ python3 gen_mapping.py ; shasum -a 256 mapping.json
9e04cfef7146dfca779ec78eea33df3260ccdb3f4535a0cdf825614660e0076c  mapping.json
$ python3 gen_mapping.py ; shasum -a 256 mapping.json
9e04cfef7146dfca779ec78eea33df3260ccdb3f4535a0cdf825614660e0076c  mapping.json
```

### Reference Pack size

`reference_pack.md`: **252 lines, 9,676 characters** (9,686 bytes; the difference is the
three-byte `…` glyphs). Within the 150–250-line target, +2 lines. It names no language, no
toolchain, no file extension, and contains **zero** occurrences of any of the 20 real
keywords — verified mechanically, §3 below.

---

## 1. Check (a) — the fixture compiles, runs, and its output matches the pack EXACTLY

The worked fixture deliberately is **not** the task solution: it demonstrates every construct
the task needs (function definition with typed parameters + result, a two-branch conditional,
bounded iteration, an accumulator, modulus, sequence construction + append + indexing,
integer→text conversion, text join, four `print` lines) over different constants with a
different output shape. Handing a trial the answer would have measured nothing.

```
$ python3 -m py_compile fixture_real.py          -> exit 0
$ python3 fixture_real.py
TOTAL 346
BIGGEST 93
ODDS 4
LABEL 33:72:16
                                                  exit 0
```

The Reference Pack, §10, claims exactly:

```
TOTAL 346
BIGGEST 93
ODDS 4
LABEL 33:72:16
```

Verified mechanically, not by eye: the worked-example code block extracted from
`reference_pack.md` is **byte-identical** to `fixture_anon.py`, and the claimed-output block
is **byte-identical** to the bytes `fixture_real.py` actually writes. `fixture_anon.py` is in
turn byte-identical to `forward(fixture_real.py)` — self-test case `P4`.

**The oracle was produced by execution, never by hand:**

```
$ python3 reference_solution.py > expected_output.txt
$ cat expected_output.txt
SUM 25632
MAX 935
EVENS 24
JOINED 897-558-614-577-405
```

Cross-checked against two independent re-computations of the Park–Miller stream (a separate
one-line implementation, and an `awk` implementation using double arithmetic — exact here,
since the largest intermediate `48271 × 2147483646 ≈ 1.04e14` is well inside 2^53). All three
agree byte for byte. First term check by hand: `7 × 48271 = 337897`, `337897 mod 2147483647 =
337897`, `337897 mod 1000 = 897` — the first field of `JOINED`.

`PASS`.

---

## 2. Check (b) — every withheld harness convention is satisfied BY THE HARNESS

| Convention | Who supplies it | How |
|---|---|---|
| entry filename `solution.py` | **harness** (fixup H1) | the trial submits one source text; the harness writes it to `solution.py` |
| build step | **harness** | `python3 -m py_compile solution.py` (§9.4 parse/compile check) |
| run command `python3 solution.py` | **harness** | frozen run recipe, §0.1 row 2 |
| working directory, stdout capture, 10 s timeout, exit-status check | **harness** | oracle §4.2 |

The trial is never asked to guess any of these, and is never penalised for not stating them.
The pack's §11 says so in language-neutral prose: *"the harness writes it to the entry file,
builds it, then runs it; you never state a file name, a command, nor any build option."*

**Deliberate design point, recorded rather than hidden.** The literal filename `solution.py`
is *not* written into the Reference Pack, because the extension alone identifies the language
and would fail the §2.6 identity-leak scan. It lives here, in the harness record, and is
applied by fixup H1. This satisfies §10.4 requirement (b) — the fact is supplied by the
harness, which is exactly what the requirement asks — without breaking anonymity. No fixup in
this unit supplies pack-documented material: the pack documents no module header, no
entry-point declaration and no import, because this language needs none (`mapping.json →
not_lexicalized` records `K14`, `K15`, `K22`, `K23` with reasons), so the pack-documented
fixup column is structurally **zero** for this language.

**Frozen rule C-4 (methodology 00) — applied, and it does bite here.** C-4 covers a language
whose current stdout API differs from the widely-reproduced older idiom. For this language the
older, heavily-reproduced idiom is the *statement* form of writing a line; the current spelling
is a **call with parentheses**. The pack therefore states the current spelling explicitly and
uses nothing else:

- §7: `` `print(text)` | writes `text` to standard output, then one line terminator. This is
  the current, correct spelling of writing a line; there is no other. ``
- §9 repeats it, adding that the terminator is produced by the call itself.
- Every example in the pack uses the call form; the statement form appears nowhere.

The task is about the generator arithmetic, not about the output API, so a trial cannot fail on
that spelling.

**Evidence that this is sufficient in practice (a PF-13-shaped test).** A *bare-body submission
written from the pack alone* — `preflight_bare_submission_anon.py`, deliberately in a different
style from the reference solution: a conditional loop `vigifo` instead of bounded iteration,
`+=`, brace interpolation, no helper function — was fed through the real pipeline
(gate → inverse → write `solution.py` → compile → run → byte-compare):

```
$ python3 validate.py submission preflight_bare_submission_anon.py expected_output.txt
ACCEPTED
$ python3 inverse.py preflight_bare_submission_anon.py > /tmp/solution.py
$ python3 /tmp/solution.py | diff - expected_output.txt      -> no differences, exit 0
```

A trial that uses only what the pack states, and states no convention of its own, passes.

`PASS`.

---

## 3. Check (c) — the round-trip validator BOTH accepts a correct input AND rejects a corrupt one

`validate.py selftest` (full transcript: `preflight/C_validator_selftest.txt`, exit 0).

**Positives — all ACCEPTED:**

| case | what it checks |
|---|---|
| `P1` | `inverse(forward(fixture_real.py))` byte-identical (R1), transform non-trivial (R4), every changed token inside the bound domain (R5) |
| `P2` | the same three, on `reference_solution.py` |
| `P3` | `fixture_anon.py` through gate → inverse → compile → run → byte-compare |
| `P4` | `fixture_anon.py` == `forward(fixture_real.py)` |

**Negatives — all REJECTED, each with the reason printed:**

| case | corruption | rejected with |
|---|---|---|
| `N1` | one pseudo-word swapped back to the real keyword it stands for | `GATE_H_REAL:if` |
| `N2` | **correct real source that ignores the transformation entirely** (§4.2a / PF-05 b) | `GATE_H_REAL:None,def,else,for,if,in,int,list,return,str` |
| `N3` | a pseudo-word mistyped into an undefined name | `COMPILE_FAILED:rc=1:SyntaxError` |
| `N4` | source using a pseudo-word as a user identifier (injectivity) | `FORWARD_COLLISION` |
| `N5` | token-clean, gate-clean, compiles, runs — arithmetic altered | `OUTPUT_MISMATCH` |
| `N6` | a transform that reached inside a text literal | `R1_BYTE_IDENTITY_FAILED` |

`N5` is the case that answers §10.4's warning directly. A *"did any transformed token survive?"*
test is vacuous when the mapping is a permutation of the language's own vocabulary. It is not
vacuous here — the pseudo-words are provably not tokens of this language — but it is still weak
on its own, so the validator does not stop at tokens: it requires the inverse image to compile,
to run to exit 0, and to emit the exact expected bytes. `N5` passes every token-level check and
is still rejected, which is what shows the token test is not carrying the verdict alone. `N2` is
the §4.2a gate's mandated negative: a fully correct, familiar-looking program that ignores the
anonymization is rejected, so pretraining recall cannot substitute for reading the pack.

The gate's real surface is `keyword.kwlist ∪ {the 20 mapped tokens}`, so it also catches the
type names (`int`, `str`, `list`, `bool`), which are builtins rather than reserved words and
which a bare reserved-word list would have missed.

**Rejection-filter liveness.** The self-test calls the *actual* `accept()` of `gen_mapping.py`
on one probe per filter and on one control:

```
[PASS] filter1 shape      ab3de    accept()=False   (not ^[a-z]{6}$)
[PASS] filter2 collision  digami   accept()=False   (already used)
[PASS] filter3 english    banana   accept()=False   (in en_common.txt)
[PASS] filter4 progterm   buffer   accept()=False   (in prog_terms.txt)
[PASS] filter5 reserved   static   accept()=False   (in reserved_union.txt)
[PASS] filter6 substring  zolist   accept()=False   (contains 'list')
[PASS] filter7 material   biggest  accept()=False   (occurs in the fixture material)
[PASS] control accepted   gidopu   accept()=True    (trips no filter)
```

During the actual draw only filter 6 fired (8 rejections): for candidates of length 6 it
subsumes filters 3–5, since an exact list hit is also a substring hit. That is why the liveness
probes above are run explicitly — otherwise four of the seven filters would never be observed
to work.

**Identity-leak scan** (`preflight/leakscan.py`, `preflight/D_leak_and_determinism.txt`):
252 lines scanned, 20 real keywords and 47 identity terms checked (language names, toolchains,
file extensions, ecosystem markers) — **zero hits**.

`PASS`.

---

## 4. Residuals, published rather than corrected

1. **V-role surface is real in I1 by design** (§7.1): `print`, `range`, `len`, `append`, `join`
   keep their true spellings, so the pack remains recognisable to a reader who already knows
   this language. That is the condition's known anonymity residual (§2.6 exemption, §13.2), not
   a defect in this unit.
2. **Interpolation (`V18`) is `NOT_TRANSFORMABLE`** (§3.2): its syntax lies inside a text
   literal, which §1 consequence 2 never transforms. The pack documents it, and adds the one
   clause that keeps it safe under the inverse transformer — *"a word token of section 3 never
   appears inside a literal of any kind"* — so a trial cannot bury a pseudo-word where the
   inverse pass will not reach it.
3. **`anonymized_token_count = 20`** for this language and task. Published, not quota-matched
   (§7.1).
4. **`str` carries two roles** in this language: the text type name (`K21c`) and the
   integer→text facility (`V10`). One token, one pseudo-word, `galozi`, documented for both
   jobs in one table row — no extra explanatory sentence (§2.2).
5. **Pack length 252 lines**, +2 over the 250-line target; content is slot-faithful, so it ships
   as-is rather than being trimmed below what the slots need (§2.4 overage rule).
