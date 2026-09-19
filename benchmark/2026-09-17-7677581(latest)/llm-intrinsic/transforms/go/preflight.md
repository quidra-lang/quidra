# MANDATORY PRE-FLIGHT VALIDATION — I1, `language_id = go`

Methodology 10 §10.4 / §10.1. **Verdict: `all_pass = true`.**

Everything claimed here was produced by executing `./preflight_run.sh`; its complete,
unedited transcript is `preflight_evidence_log.txt` and its artifacts are under
`preflight_evidence/`. Nothing below is a summary of a check that was not run.

| Environment | Value |
|---|---|
| Toolchain | `go1.26.3 darwin/arm64` |
| Build recipe (frozen, §0.1) | `go build -o BIN solution.go` |
| Run recipe | `./BIN` |
| Entry file name (harness-supplied) | `solution.go` |
| Transformer host | Python 3.9.6 |
| Seed | `20260918` |
| Tokens anonymized | 9 |

---

## (a) The fixture compiles, runs, and its output matches EXACTLY what the pack claims

| Step | Evidence | Result |
|---|---|---|
| `go build -o …/fixture_real.bin fixture_real.go` | PF-A1 | exit **0** |
| run the binary | PF-A2 | exit **0** |
| `cmp` stdout against `expected_output.txt` | PF-A3 | **IDENTICAL** |
| oracle produced by execution, not by hand | PF-A2/A3 | `expected_output.txt` is the captured stdout of the built binary |
| a second, independent implementation agrees | PF-A4 | **IDENTICAL** — a Python re-derivation of the generator produces the same four lines |
| the pack's worked example is byte-identical to `fixture_anon.go` | PF-A5 | **True** |
| the four lines the pack **claims** equal the four lines the program **prints** | PF-A6 | **True** |

```
SUM 25632
MAX 935
EVENS 24
JOINED 897-558-614-577-405
```

The pack claim and the oracle are compared mechanically (PF-A6 extracts the claimed block
out of `reference_pack.md` and `cmp`s it), so the two cannot drift apart silently.

**Round trip.** `inverse(forward(fixture_real.go))` is **byte-identical** to
`fixture_real.go` (R1), builds (R2), and prints the same four lines with the same exit
status (R3); the forward pass is non-trivial (R4, 25 token substitutions) and touches
nothing outside the bound domain (R5). PF-F.

---

## (b) Every withheld harness convention is satisfied BY THE HARNESS

§9.1: the pack states everything about the *language*; it states nothing about this
benchmark's file system or invocation, because those facts name the toolchain. The harness
carries them.

| Convention | Who supplies it | How it is verified |
|---|---|---|
| entry file name `solution.go` | harness, fixup **H1** | PF-B writes the inverse image to `solution.go` itself; the submission never names a file |
| build command `go build -o BIN solution.go` | harness | PF-B/PF-B2 run it; the trial is never asked for it |
| output binary path / output directory | harness, fixup **H6** | PF-B |
| code extraction from a fenced block | harness, §9.2 | PF-B extracts the last fenced block of a model-shaped output (755 bytes from 1 block) |
| compilation-unit line, module lines, entry-point shape | **the pack** (sections 1 and 8) — *not* the harness | PF-B: **pack-documented fixups applied = 0** |

**Note on anonymity.** The task brief phrases this requirement as "the entry filename
(`solution.go`) and the build command are given to the trial". Handing a trial the literal
strings `solution.go` and `go build` would name the language and break I1 anonymity, which
§2.6 forbids. The fairness requirement behind the phrasing — *a trial is never penalised
for not guessing a convention the prompt withholds* — is met the way §9.1/§9.3 prescribe:
the harness applies H1 and H6 on the trial's behalf, so there is no filename or build
command for a trial to get wrong. The evidence that this is sufficient is PF-B and PF-B2:

* **PF-B** — a bare-body submission containing every element the pack documents and
  nothing the pack withholds: gate **PASS**, build exit **0**, run exit **0**, output
  **byte-identical** to the oracle, **0 pack-documented fixups**.
* **PF-B2** — an *independently written* solution (different identifier names, loop from
  1 to 50 rather than 0 to 49, `<=` and `!=` in place of `<` and `>`), using only
  pack-documented constructs and never copying the worked example: gate **PASS**, build
  exit **0**, output **byte-identical** to the oracle. This is the check that the pack is
  actually sufficient rather than merely self-consistent.

**Frozen rule C-4 (methodology 00) — applied, and the finding recorded.**
C-4 names TypeScript 7 (`--outFile` removed) and Zig 0.16 (`std.io` helpers replaced by an
explicit `Io` instance) as the two toolchains whose current stdout/emission API differs
from the widely-reproduced older idiom. **This language is not one of them.** Its one-line
output facility, `fmt.Println`, is both the current spelling under `go1.26.3` and the
spelling reproduced in essentially all published material for this toolchain; there is no
older idiom a trial could write instead, and therefore no drift for the harness to absorb.
C-4 is nevertheless honoured in the stronger form the brief asks for: **the Reference Pack
states the current spelling outright** (section 7, verbatim: "`fmt.Println` is the current
spelling of the one-line output facility in this toolchain; there is no older or
alternative spelling to consider"), together with the exact spelling of the
integer-to-text facility `strconv.FormatInt(v, 10)` and its base argument. A trial cannot
fail on the stdout API here, because the pack hands it the API.

Two further facts that are language properties rather than benchmark conventions, and that
a trial could not derive from anywhere else, are also stated in the pack rather than left
to be guessed (sections 1): the opening `{` must be on the same line as its header, and an
unused binding or an unused module is a build **error**. Both are silent-failure traps that
have nothing to do with what this task measures.

---

## (c) The validator can BOTH accept a correct input AND reject a corrupted one

`python3 validate.py selftest` — full transcript in `preflight_evidence_log.txt`, section
PF-C. Every line below was executed.

### Accepted (must pass)

```
[PASS] R1_byte_identity on the shipped fixture
[PASS] R2_build_identity on the shipped fixture
[PASS] R3_behavioural_identity on the shipped fixture
[PASS] R4_non_triviality on the shipped fixture
[PASS] R5_domain_containment on the shipped fixture
[PASS] inverse image reproduces expected_output.txt
[PASS] C1 anonymized fixture passes the gate (0 H_REAL events)
[PASS] C4 real keywords inside a text literal do not trip the gate
```

### Rejected (must fail, and did)

```
[PASS] B1 one-character corruption of a pseudo-word rejected (R1)
[PASS] B1 one-character corruption of a pseudo-word rejected (R2 build)
[PASS] B2 swapped pseudo-words rejected (R1)
[PASS] B2 swapped pseudo-words rejected (R2 build)
[PASS] B3 substitution inside a text literal rejected (R1)
[PASS] B3 substitution inside a text literal rejected (R3 behaviour)
[PASS] B3 output actually differs from the oracle
[PASS] B4 truncated submission rejected (R2 build)
[PASS] C2 correct REAL source that ignores the transformation is rejected
       H_REAL events found in the real source: 25 (first five: package, main, import, import, func)
[PASS] C3 anonymized submission leaking ONE real keyword is rejected
```

The mutations are, in order: one character of `feguzi` altered; `feguzi` and `lunuso`
exchanged; a substitution reaching **inside** the text literal `"SUM "` (the corruption
that would silently change program output — caught behaviourally as well as byte-wise, and
the resulting stdout provably differs from the oracle); a truncated submission; correct
real source submitted unchanged; and an otherwise-conformant submission in which a single
real keyword was written from memory.

**§4.2a negative fixture, as PF-05(b) requires.** C2 is exactly the fixture the methodology
demands: *correct real source that ignores the transformation entirely.* It builds, it
produces the right answer, and the gate rejects it with 25 `H_REAL` events. Without this
gate that submission would score as a pass on pretraining recall alone.

**§10.4's vacuity warning.** §10.4 / PF-05(a) warns that a "did any transformed token
survive?" test is vacuous when the mapping is a permutation of the language's own
vocabulary — the I6 situation. I1's mapping is **not** a permutation: its image is nine
invented CVCVCV words that are provably not tokens of this or any of the ten languages
(filters 3–6 of §5.3). Survivor-counting is therefore meaningful here rather than vacuous.
It is nonetheless not relied on alone: the validator's substantive checks are byte-identity
of the round trip (R1), build and behavioural identity of the inverse image (R2, R3),
domain containment of the diff (R5) and the §4.2a gate — each exercised with a positive and
a negative case above.

### Lexicalizer and mapping self-tests

* **Determinism** — the mapping regenerated in two fresh processes is byte-identical to the
  shipped `mapping.json`. PF-C section E.
* **Mapping invariants** (PF-E) — 9 tokens; one-to-one; every pseudo-word exactly 6 ASCII
  lowercase characters (so character length is matched exactly, not approximately); **no
  pseudo-word is a prefix of another**; no pseudo-word collides with any identifier or
  literal in the fixture.
* **Rejection filters** (PF-C section D) — rather than hand-picking a witness per filter,
  the check walks the **entire 343,000-word PW-1 output space** and records which filter
  rejects each word. `shape`, `used`, `en_common` (844 words) and `substring` (103,206
  words) fire; 238,950 words (69.7%) survive every filter. `prog_terms`, `reserved_union`
  and `task_material` are reported **SUBSUMED**: for this language and this task every word
  they would reject is already rejected by an earlier filter (a PW-1 word always ends in a
  vowel, and the CVCVCV-shaped entries of those lists — `module`, `native` — are ordinary
  English words caught by `en_common` first). That is published as a fact, not papered
  over, and each subsumed predicate is separately shown to be live code by an isolated
  membership test.
* **Lexer losslessness** — `"".join(token texts) == source` for both fixtures, so no
  substitution can reach inside a string, rune, number or comment token by construction.

### Identity-leak scan (PF-D)

`reference_pack.md`: **247 lines, 8394 characters**, **0 leak hits**. Scanned for the
language's name, its toolchain and tool names, its file extension, and every whole-word
occurrence of any of its 51 reserved and predeclared words plus all 9 transformed tokens.

---

## Published residuals

These are stated, not corrected.

1. **V-role surface is real by design.** §7.1 does not transform standard-library names in
   I1, so `fmt`, `Println`, `strconv` and `FormatInt` appear in the pack in their real
   spelling. §2.6 exempts them explicitly; the residual anonymity exposure is that a reader
   who already knows this toolchain will recognize it from those four names. Recorded here
   as an anonymity residual, per §13.2.
2. **`anonymized_token_count = 9`**, published rather than quota-matched (§7.1). This is
   the mechanically derived §7.1a domain for *this task*: the intersection of the language's
   reserved/predeclared surface with the word tokens that actually occur in the fixture.
   The entry-point name and the compilation-unit name are toolchain-fixed rather than
   user-chosen, so they are bound (`K22`) and anonymized; ordinary user identifiers
   (`state`, `sum`, `largest`, `evens`, `joined`, `i`, `term`) are **not** renamed, per
   §10.5 I1.
3. **Three rejection filters are subsumed** for this language and task (above). Reported,
   not engineered around.
4. **Two inverse modes.** With the forward pass's position map the inverse is byte-exact
   (R1). Model output has no forward pass and no map; in that mode the inverse deletes no
   whitespace, which is safe because this language's grammar is whitespace-insensitive at
   every boundary a word substitution can touch. The only observable difference on this
   fixture is one space in `mizufi lemuma ()`; both images build and produce identical
   output (PF-B, PF-B2).
5. **Integer width.** The fixture and the pack use the explicitly 64-bit whole-number kind
   rather than the platform-width one, so no value in this task depends on the host's word
   size. Every intermediate (`2147483646 * 48271 ≈ 1.04e14`) is exact in 64 bits
   (methodology 00, C-1).

---

## Files

| File | What it is |
|---|---|
| `mapping.json` | the 9-token keyword map, the seed, the word-list SHA-256 values, redraw counts |
| `gen_mapping.py` | PW-1 generator (§5.1–5.3); regenerates `mapping.json` deterministically |
| `lex10.py` | the lossless lexer (§6.1) with this language's profile |
| `forward.py` | real source → anonymized source, plus the position map |
| `inverse.py` | anonymized source → real source (map mode and no-map mode) |
| `validate.py` | R1–R5, the §4.2a gate, the filter census, determinism; `selftest` runs positives and negatives |
| `leakscan.py` | §2.6 identity-leak scan of the pack |
| `fixture_real.go` | the real program; the oracle's source |
| `fixture_anon.go` | its anonymized image (+ `.posmap.json`) |
| `expected_output.txt` | the oracle, captured from execution |
| `reference_pack.md` | the Reference Pack shown to the trial |
| `preflight_run.sh` | produces every claim in this document |
| `preflight_evidence_log.txt` | the unedited transcript |
| `preflight_evidence/` | binaries, extracted submissions, built sources, captured stdout |
| `wordlists/` | `en_common`, `prog_terms`, `reserved_union`, `reserved_go` |
