# MANDATORY PRE-FLIGHT VALIDATION — I1, `language_id = swift`

Methodology 10 §10.4 / §10.1. **Verdict: `all_pass = true`.**

Everything claimed here was produced by executing `./preflight_run.sh`; its complete,
unedited transcript is `preflight_evidence_log.txt` and its artifacts are under
`preflight_evidence/`. Nothing below is a summary of a check that was not run.

| Environment | Value |
|---|---|
| Toolchain | `Apple Swift version 6.2.3 (swiftlang-6.2.3.3.21 clang-1700.6.3.2)`, target `arm64-apple-macosx26.0` |
| Build recipe (frozen intrinsic track, §0.1/§0.1a) | `swiftc -O solution.swift -o BIN` |
| Run recipe | `./BIN` |
| Entry file name (harness-supplied) | `solution.swift` |
| Transformer host | Python 3.9.6 |
| Seed | `20260918` |
| Tokens anonymized | 7 |
| Reference Pack size | 236 lines, 8496 characters |

---

## (a) The fixture compiles, runs, and its output matches EXACTLY what the pack claims

| Step | Evidence | Result |
|---|---|---|
| `swiftc -O fixture_real.swift -o …/fixture_real.bin` | PF-A1 | exit **0** |
| run the binary | PF-A2 | exit **0** |
| `cmp` stdout against `expected_output.txt` | PF-A3 | **IDENTICAL** |
| oracle produced by execution, not by hand | PF-A2/A3 | `expected_output.txt` is the captured stdout of the built binary |
| a second, independent implementation agrees | PF-A4 | **IDENTICAL** — a Python re-derivation of the generator produces the same four lines |
| the pack's worked example is byte-identical to `fixture_anon.swift` | PF-A5 | **True** |
| the four lines the pack **claims** equal the four lines the program **prints** | PF-A6 | **True** |

```
SUM 25632
MAX 935
EVENS 24
JOINED 897-558-614-577-405
```

The pack claim and the oracle are compared mechanically (PF-A6 extracts the claimed block
out of `reference_pack.md` and `cmp`s it), so the two cannot drift apart silently.

**Round trip.** `inverse(forward(fixture_real.swift))` is **byte-identical** to
`fixture_real.swift` (R1), builds (R2), and prints the same four lines with the same exit
status (R3); the forward pass is non-trivial (R4, 18 token substitutions) and touches
nothing outside the bound domain (R5). PF-F.

**The whitespace-insertion path is exercised on purpose (PF-C section A2).** The shipped
fixture happens to need **zero** inserted characters, because none of its bound tokens sits
hard against a delimiter. §6.4 warns that a fixture author who never writes a keyword
against a parenthesis can hide a transformer defect behind a style choice, so the validator
additionally round-trips an auxiliary source written in exactly that style
(`n:<kind> = 0`, `<cond-word>(n < 1){`). The emission rule inserts **3** characters there,
the inverse deletes exactly those 3, R1 is byte-identical, and the inverse image builds and
prints what the real source prints.

**The pack's own factual claims are executed, not asserted (PF-G).** Seven properties the
pack states about the language were each compiled and run:

| Pack claim | Observed |
|---|---|
| symmetric operator spacing `a + b` builds | build-ok, run exit 0 |
| asymmetric spacing `a +b` does **not** build | build **FAIL**, as claimed |
| `0..<3` excludes the upper end (sum 0+1+2) | prints `3` |
| assigning to a fixed binding does **not** build | build **FAIL**, as claimed |
| an unread binding is a warning, not an error | build-ok, run exit 0 |
| whole-number arithmetic is checked, not wrapping | run exit **133** (trap), as claimed |
| a `{` on the next line builds (both for the branch and the loop header) | build-ok, run exit 0 |

The last row is a **defect found and fixed during pre-flight**, recorded rather than
quietly corrected (§10.4). An earlier draft of the pack carried a "the `{` must sit on the
same line" rule, carried over from a sibling language where it is true. PF-G executed it,
found it false here, and the pack now states the truth. A pack claim that is false teaches
every trial in this language something wrong; that is precisely the class of defect this
check exists to catch.

---

## (b) Every withheld harness convention is satisfied BY THE HARNESS

§9.1: the pack states everything about the *language*; it states nothing about this
benchmark's file system or invocation, because those facts name the toolchain. The harness
carries them.

| Convention | Who supplies it | How it is verified |
|---|---|---|
| entry file name `solution.swift` | harness, fixup **H1** | PF-B writes the inverse image to `solution.swift` itself; the submission never names a file |
| build command `swiftc -O solution.swift -o BIN` | harness | PF-B/PF-B2 run it; the trial is never asked for it |
| output binary path | harness, fixup **H6** | PF-B |
| code extraction from a fenced block | harness, §9.2 | PF-B and PF-B3: zero, one and three fenced blocks all extract the program |
| entry-point shape, compilation-unit declaration, imports | **the pack** (sections 1 and 8) — *not* the harness | PF-B: **pack-documented fixups applied = 0** |

This language needs no H2/H3/H4/H5 scaffolding at all: its outermost statements *are* the
program (PF-B builds a submission with no entry-point declaration and it runs), no
compilation-unit line exists, this task imports nothing, and its output facility propagates
no error. H1 and H6 are the only fixups, and both are pure unstated-convention fixups.

**Note on anonymity.** The task brief phrases this requirement as "the entry filename
(`solution.swift`) and the build command are given to the trial". Handing a trial the
literal strings `solution.swift` and `swiftc` would name the language and break I1
anonymity, which §2.6 forbids. The fairness requirement behind the phrasing — *a trial is
never penalised for not guessing a convention the prompt withholds* — is met the way
§9.1/§9.3 prescribe: the harness applies H1 and H6 on the trial's behalf, so there is no
filename and no build command for a trial to get wrong. The evidence that this is
sufficient is PF-B and PF-B2:

* **PF-B** — a bare-body submission containing every element the pack documents and
  nothing the pack withholds: gate **PASS**, build exit **0**, run exit **0**, output
  **byte-identical** to the oracle, **0 pack-documented fixups**.
* **PF-B2** — an *independently written* solution (different identifier names, the run
  walked from 1 to 50 rather than 0 to 49, `<=` and `!=` in place of `<` and `>`, and the
  generator step written **without** its parentheses so that the pack's precedence table is
  actually relied on rather than copied around): gate **PASS**, build exit **0**, output
  **byte-identical** to the oracle. This is the check that the pack is actually sufficient
  rather than merely self-consistent.

**Frozen rule C-4 (methodology 00) — applied, and the finding recorded.**
C-4 names TypeScript 7 (`--outFile` removed) and Zig 0.16 (`std.io` helpers replaced by an
explicit `Io` instance) as the two toolchains whose current stdout/emission API differs from
the widely-reproduced older idiom. **This language is not one of them.** Its one-line output
facility, `print`, is both the current spelling under Swift 6.2.3 and the spelling
reproduced in essentially all published material for this toolchain; there is no older idiom
a trial could write instead, and therefore no drift for the harness to absorb. C-4 is
nevertheless honoured in the stronger form the brief asks for: **the Reference Pack states
the current spelling outright** (section 7, verbatim: "`print` is the current spelling of
the one-line output facility in this toolchain. There is no older or alternative spelling to
consider, no instance or handle to obtain first, and no error to propagate from it"). The
two clauses after the semicolon are deliberate: they tell a trial that the Zig-shaped
"obtain a writer first" and the `K27`-shaped "mark the error path" ceremonies do **not**
apply here, so a model carrying either habit is not left guessing. A trial cannot fail on
the stdout API in this language, because the pack hands it the API.

Three further facts that are language properties rather than benchmark conventions, and
that a trial could not derive from anywhere else, are also stated in the pack rather than
left to be guessed: symmetric operator spacing (section 1), checked whole-number arithmetic
(section 4), and the half-open run operator `a..<b` (section 5). The first is a silent trap
that has nothing to do with what this task measures; the third would otherwise be an
off-by-one waiting to happen. All three were executed at PF-G.

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
[PASS] A2 the emission rule actually inserted whitespace here (3 character(s))
[PASS] A2 R1 byte identity after deleting exactly the inserted characters
[PASS] A2 R2 the inverse image builds
[PASS] A2 R3 it prints what the real source prints
[PASS] C1 anonymized fixture passes the gate (0 H_REAL events)
[PASS] C4 real keywords inside a text literal do not trip the gate
[PASS] C5 a legal identifier that happens to be a context-sensitive word does NOT trip the gate
[PASS] G1 the clean anonymized fixture raises no ambiguity
```

### Rejected (must fail, and did)

```
[PASS] B1 one-character corruption of a pseudo-word rejected (R1)
[PASS] B1 one-character corruption of a pseudo-word rejected (R2 build)
[PASS] B2 swapped pseudo-words rejected (R1)
[PASS] B2 swapped pseudo-words rejected (R3 behaviour)
[PASS] B3 substitution inside a text literal rejected (R1)
[PASS] B3 substitution inside a text literal rejected (R3 behaviour)
[PASS] B3 output actually differs from the oracle
[PASS] B4 truncated submission rejected (R2 build)
[PASS] C2 correct REAL source that ignores the transformation is rejected
       H_REAL events found in the real source: 18 (first five: var, Int, var, Int, var)
[PASS] C3 anonymized submission leaking ONE real keyword is rejected
[PASS] G2 a submission that NAMES A VARIABLE with a pseudo-word is flagged
[PASS] G3 ... and its inverse image indeed fails to build
```

The mutations are, in order: one character of the conditional pseudo-word altered; the two
binding pseudo-words exchanged; a substitution reaching **inside** the text literal
`"SUM \(total)"` (the corruption that would silently change program output — caught
behaviourally as well as byte-wise, and the resulting stdout provably differs from the
oracle); a truncated submission; correct real source submitted unchanged; an
otherwise-conformant submission in which a single real keyword was written from memory; and
a submission that declares a variable **named** with a pseudo-word, which is the
`INVERSE_AMBIGUOUS` path of §6.1.

**§4.2a negative fixture, as PF-05(b) requires.** C2 is exactly the fixture the methodology
demands: *correct real source that ignores the transformation entirely.* It builds, it
produces the right answer, and the gate rejects it with 18 `H_REAL` events. Without this
gate that submission would score as a pass on pretraining recall alone.

**A gate that rejects correct work measures nothing either.** C5 is the counterpart
negative-of-the-negative: this language has many *context-sensitive* words (`get`, `set`,
`some`, `any`, `open`, `line`, `file`, …) that are perfectly legal identifier names. The
gate's vocabulary is therefore the language's **strict** keywords
(`wordlists/reserved_swift_strict.txt`, 54 entries) plus the 7 real spellings this
condition transforms — not the full 128-entry reserved surface. C5 submits a conformant
program whose accumulator is named with one of those context-sensitive words and requires
the gate to pass it. The exclusion list is a file, not a judgement call made per submission.

**§10.4's vacuity warning.** §10.4 / PF-05(a) warns that a "did any transformed token
survive?" test is vacuous when the mapping is a permutation of the language's own
vocabulary — the I6 situation. I1's mapping is **not** a permutation: its image is seven
invented CVCVCV words that are provably not tokens of this or any of the ten languages
(filters 3–6 of §5.3). Survivor-counting is therefore meaningful here rather than vacuous.
It is nonetheless not relied on alone: the validator's substantive checks are byte-identity
of the round trip (R1), build and behavioural identity of the inverse image (R2, R3),
domain containment of the diff (R5), the §4.2a gate in both directions, and the
`INVERSE_AMBIGUOUS` detector in both directions — each exercised with a positive and a
negative case above.

### Lexicalizer and mapping self-tests

* **Determinism** — the mapping regenerated in two fresh processes is byte-identical to the
  shipped `mapping.json`. PF-C section E.
* **Mapping invariants** (PF-E) — 7 tokens; one-to-one; every pseudo-word exactly 6 ASCII
  lowercase characters (so character length is matched exactly, not approximately); **no
  pseudo-word is a prefix of another**; no pseudo-word collides with any identifier or
  literal in the fixture.
* **Rejection filters** (PF-C section D) — rather than hand-picking a witness per filter,
  the check walks the **entire 343,000-word PW-1 output space** and records which filter
  rejects each word. `shape`, `used`, `en_common` (1,090 words) and `substring` (123,810
  words) fire; 218,100 words (63.6%) survive every filter. `prog_terms`, `reserved_union`
  and `task_material` are reported **SUBSUMED**: for this language and this task every word
  they would reject is already rejected by an earlier filter (a PW-1 word always ends in a
  vowel, and the CVCVCV-shaped entries of those lists are ordinary English words that
  `en_common` catches first). That is published as a fact, not papered over, and each
  subsumed predicate is separately shown to be live code by an isolated membership test.
* **Lexer losslessness** — `"".join(token texts) == source` for both fixtures, and also for
  a source using this language's extended (`#"…"#`), multi-line (`"""…"""`) and
  value-embedding (`\(…)`) literal forms. Three further checks prove the substituter cannot
  reach where §1 forbids: a bound word written inside a text literal is not substituted; a
  bound word written inside an **embedded expression** is not substituted either (§3.2
  V18); and a bound word written as a backtick-escaped *name* is not substituted, because
  the backticks say "this is a name, not a grammar word".

### Identity-leak scan (PF-D)

`reference_pack.md`: **236 lines, 8496 characters**, **0 leak hits**. The scan has three
domains, because one blanket rule would be either useless or dishonest:

* **A — identity terms**, whole pack, case-insensitive: the language's name, its compiler
  and tool names, its vendor, its ecosystem and framework names. 0 hits.
* **B — distinctive reserved words**, whole pack, whole-word: every reserved word of the
  real language that is **not** an ordinary English word. 0 hits. Prose has no reason to
  contain the real declaration or type words, and it does not.
* **C — the full 128-word reserved surface plus all 7 transformed real spellings**,
  whole-word, **inside fenced code blocks only** (9 blocks). 0 hits. Code is where a real
  spelling would actually teach the real language.

The words exempted from domain B are ordinary English function words that also happen to be
reserved here (`as`, `do`, `in`, `is`, `line`, `file`, `where`, `operator`, `optional`,
`override`, `each`, `available`). Their realized prose counts are printed in the evidence
log. An English function word identifies no language — every one of the ten packs contains
`in` and `is` in prose — and the exemption is a claim about *prose* only: domain C still
forbids all of them inside a code block. The exemption list is a file-backed constant in
`leakscan.py`, not a per-run judgement. For what it is worth, the five most identifying
candidates (`if`, `for`, `let`, `var`, and the two real type names) occur **zero** times in
the pack, in prose or in code.

---

## Published residuals

These are stated, not corrected.

1. **V-role surface is real by design.** §7.1 does not transform standard-library names in
   I1, so the output facility appears in the pack in its real spelling. §2.6 exempts it
   explicitly; the residual anonymity exposure is that a reader who already knows this
   toolchain will recognize it from that one name **together with** the `\(…)` embedding
   syntax of residual 2. Recorded here as an anonymity residual, per §13.2. It is the
   largest anonymity residual in this language's pack.
2. **Value embedding is untransformable in every condition.** The facility that turns a
   number into text here lives *inside* a text literal (`"\(v)"`). §1 consequence 2 forbids
   any transformation from reaching inside a literal, and §3.2 `V18` makes that permanent
   for all six conditions. So this language's integer-to-text step keeps its real surface in
   I1 **and** would keep it in I2, where a language that spells the same job as a call would
   have it anonymized. That is the published asymmetry of §13.1, not a licence to drop the
   facility: the task needs it, so the pack documents it in the P2 slot.
3. **`anonymized_token_count = 7`**, published rather than quota-matched (§7.1). This is the
   mechanically derived §7.1a domain for *this task*: the intersection of this language's
   reserved surface with the word tokens that actually occur in the fixture. Six are K-roles
   (`K02`, `K03`, `K04`, `K07`, `K21a`, `K21c`); one is a non-role reserved word carrying
   the key `U:in`, anonymized on exactly the same footing. `unbound_word_tokens` is
   **empty**, as §7.1a requires. The count is lower than some sibling languages' because
   this language needs **no** compilation-unit declaration, **no** entry-point declaration
   and **no** import for this task — three whole K-roles (`K14`, `K15`, `K22`) are
   `NOT_LEXICALIZED` here and consume no budget. Ordinary user identifiers (`state`,
   `total`, `largest`, `evens`, `joined`, `i`, `term`) are **not** renamed, per §10.5 I1.
4. **Three rejection filters are subsumed** for this language and task (above). Reported,
   not engineered around.
5. **Two inverse modes.** With the forward pass's position map the inverse is byte-exact
   (R1). Model output has no forward pass and no map; in that mode the inverse inserts and
   deletes **nothing** — it rewrites whole word tokens in place and leaves every whitespace
   character alone. That is not a convenience here but a requirement: this language's
   operator parser is whitespace-sensitive (see PF-G, asymmetric spacing), so a transformer
   that "tidied" spacing on the way back could change a submission's meaning. On this
   fixture the two modes produce identical output, because the forward pass inserted nothing
   (PF-F, PF-B).
6. **Integer width.** The fixture and the pack use this language's signed whole-number kind,
   which is 64-bit on the frozen target. Every intermediate
   (`2147483646 * 48271 ≈ 1.04e14`) is exact in 64 bits (methodology 00, C-1), and the
   arithmetic is **checked**, so a hypothetical overflow would trap loudly rather than wrap
   silently — consistent with §0.1a's checks-enabled rule, and achieved without any flag
   because this toolchain checks by default under `-O`.
7. **Cross-language word-list divergence, reported not resolved.** §5.3 treats
   `en_common.txt`, `prog_terms.txt` and `reserved_union.txt` as *frozen shared* files whose
   SHA-256 is recorded. In the current tree the sibling language directories each hold a
   **different** copy of all three (six distinct `prog_terms` hashes across six languages;
   `en_common` ranges from 1,445 to 32,456 entries). This language's copies are therefore
   built as the **union** of every sibling's copy, plus this language's own reserved words
   folded into `reserved_union`, so that this language's `accept()` filter is at least as
   strict as any sibling's and no pseudo-word here could be a word another language's
   filter would have rejected. The four hashes actually used are recorded in `mapping.json`
   and re-verified in PF-E. **This is a cross-language consistency defect that this
   single-language build cannot fix**: §5.3 requires one frozen list set shared by all ten,
   and PF-08/PF-14 will need to reconcile the ten directories and regenerate every mapping
   against the reconciled lists before any trial is scored. It is recorded here rather than
   silently absorbed.

---

## Files

| File | What it is |
|---|---|
| `mapping.json` | the 7-token keyword map, the seed, the word-list SHA-256 values, redraw counts |
| `gen_mapping.py` | PW-1 generator (§5.1–5.3); regenerates `mapping.json` deterministically |
| `lex10.py` | the lossless lexer (§6.1) with this language's profile |
| `forward.py` | real source → anonymized source, plus the position map |
| `inverse.py` | anonymized source → real source (map mode and no-map mode), `INVERSE_AMBIGUOUS` detector |
| `validate.py` | R1–R5, the §4.2a gate, the filter census, determinism; `selftest` runs positives and negatives |
| `leakscan.py` | §2.6 identity-leak scan of the pack, three domains |
| `fixture_real.swift` | the real program; the oracle's source |
| `fixture_anon.swift` | its anonymized image (+ `.posmap.json`) |
| `expected_output.txt` | the oracle, captured from execution |
| `reference_pack.md` | the Reference Pack shown to the trial |
| `preflight_run.sh` | produces every claim in this document |
| `preflight_evidence_log.txt` | the unedited transcript |
| `preflight_evidence/` | binaries, extracted submissions, built sources, captured stdout, probe sources |
| `wordlists/` | `en_common`, `prog_terms`, `reserved_union`, `reserved_swift`, `reserved_swift_strict` |
