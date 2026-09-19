# Defect and correction log

Spec 10.4: "Defects found during a run are fixed, the affected cells re-run or re-verified, and both
the defect and the fix recorded. They are not silently corrected."

---

## D-1. Arithmetic slips in methodology 02 §4.4 illustrative tables

**Found:** while implementing the frozen tokenizer against the §4.4 fixture tables.

**Defect.** Two rows of the §4.4 illustrative tables state a total that does not match the lexeme list
written in the same row. The *lexeme lists are correct*; only the totals are off by one.

| Doc 02 row | Lexemes listed in the doc | Stated total | Correct total |
|---|---|---:|---:|
| C++ `std::format("x={}", c)` | `std` `::` `format` `(` `STR` `,` `c` `)` — 8 lexemes | 7 | **8** |
| Kotlin `@Suppress("x")` | `@` `Suppress` `(` `STR` `)` — 5 lexemes | 4 | **5** |

**Why it is only a presentation defect.** Neither total is used by any measurement. The counting *rule*
(§4.3 uniform unit rule, §4.4(a) interpolation rule, §4.4(c) attributes receive no exemption) is what the
tokenizer implements, and the tokenizer reproduces the doc's own lexeme lists exactly. Applying the rule
as written yields 8 and 5.

**Fix.** The tokenizer's frozen fixture set uses the corrected totals 8 and 5. The lexeme lists are
unchanged. No probe measurement used the incorrect totals, because the tokenizer's fixtures were
corrected before any probe was tokenized.

**Impact on scores:** none. No language is advantaged or disadvantaged; the two rows are illustrations,
and the interpolation-cost symmetry they demonstrate (every interpolation spelling costs shell + hole +
expression) is unaffected.

---

## D-2. Tokenizer defect: C++ digit separator between hexadecimal digits

**Found:** by the tokenizer's own frozen fixture `0xFF'FFu` (expected: one `NUM` token).

**Defect.** The first implementation treated the C++ digit separator `'` as a separator only when both
neighbouring characters satisfied `isdigit()`. In `0xFF'FFu` the neighbours are `F` and `F`, which are
hexadecimal digits but not decimal digits, so the scan terminated the numeric literal early and the
following `'` was then mis-lexed as the opening of a character literal, aborting with
"unterminated string literal".

**Why this mattered.** It would have made any C++ probe containing a hex literal with a digit separator
un-tokenizable — a hard abort. Under the design rule that an unrecognised character is a hard error
rather than a silent skip, this would have surfaced loudly rather than quietly corrupting a count, which
is why the hard-error design was chosen.

**Fix.** The separator test now requires both neighbours to be alphanumeric, which is the correct reading
of "digits of the literal's base" and covers hexadecimal, octal and binary digits. Fixture `0xFF'FFu`
now yields 1 token.

**Impact on scores:** none. Found and fixed before any probe was tokenized.

---

## D-3. Two tokenizer fixtures were mis-stated when first written

**Defect.** Two fixtures added while implementing the tokenizer asserted totals that contradict the
frozen rules: C++ `vector<vector<int>>` was asserted as 8 (the maximal-munch rule in 02 §4.3 makes the
closing `>>` a single token, so the correct total is 6) and Quidra `print("a={v}")` was asserted as 7
(the interpolation rule in 02 §4.4(a) gives `print` `(` `STR` `HOLE` `v` `)` = 6).

**Fix.** Both fixtures corrected to match the frozen rules. The tokenizer was not changed; it had been
right in both cases.

**Impact on scores:** none.

---

## D-4. QUIDRA BIAS in the token rule: the one-token interpolation hole (FIXED)

**Found by:** the adversarial methodology audit of `02_fact_taxonomy_and_density.md`, before any
Semantic Density score was computed.

**Defect.** Methodology 02 §4.1 and §4.3 state the neutrality guarantee that makes punctuation-heavy and
word-heavy syntaxes comparable: *"every lexeme is exactly one token… No class is weighted, discounted,
or exempted."* §4.4(a) then contradicted it: *"The hole costs exactly one token regardless of how the
language spells it."*

An interpolation hole is written with **two** delimiters in every language that has the braced form —
Python `{`…`}`, TypeScript `${`…`}`, Kotlin `${`…`}`, Swift `\(`…`)`, Quidra `{`…`}`. Charging one token
for two written lexemes was the only class exemption in §4, and it granted a flat one-token-per-hole
discount to exactly five languages: **Python, TypeScript, Kotlin, Swift and Quidra**. The five without
interpolation — C++, Go, Java, Rust, Zig — must spell out a call form and pay every `(`, `,` and `)`
separately under the same section's own rule.

**Why it mattered.** Tokens are the *denominator* of Semantic Density
(`explicitly recoverable facts / lexical source tokens`), which is 20% of the Semantic Compression
quality score `Q`. Undercounting tokens inflates density. Quidra was inside the favoured group, so the
defect inflated Quidra's score in Primary Evaluation 1 — the benchmark's top-priority evaluation.

**Fix.** A hole now pays for the delimiters actually written, exactly as a call form pays for `(` and `)`:

| Form | Example | Delimiters written | Tokens for the hole |
|---|---|---:|---:|
| Braced / parenthesised | `{c}`, `${c}`, `\(c)` | 2 | `HOLE_OPEN` + `HOLE_CLOSE` = 2 |
| Bare sigil | `$c` (Kotlin) | 1 | `HOLE` = 1 |

Verified counts after the fix:

| Language | Source | Before | After |
|---|---|---:|---:|
| Python | `f"x={c}"` | 3 | **4** |
| TypeScript | `` `x=${c}` `` | 3 | **4** |
| Kotlin | `"x=$c"` | 3 | 3 (one delimiter, correctly unchanged) |
| Kotlin | `"x=${c}"` | 3 | **4** |
| Swift | `"x=\(c)"` | 3 | **4** |
| Quidra | `print("a={v}")` | 6 | **7** |
| Go | `fmt.Sprintf("x=%d", c)` | 8 | 8 (unchanged) |
| C++ | `std::format("x={}", c)` | 8 | 8 (unchanged) |

**Direction of the correction.** It *raises* Quidra's token count and therefore *lowers* Quidra's
Semantic Density. The benchmark's stated purpose is not to make Quidra score highly, and a defect found
in Quidra's favour is corrected in exactly the same way one found against it would be.

**Re-measurement obligation.** Some probe annotations were produced by agents that invoked the tokenizer
before this fix. Token counts recorded in those annotations are therefore not comparable with later ones.
**Every probe file is re-tokenized in one authoritative pass with the fixed tokenizer before Semantic
Density is computed, and the re-tokenized counts — not any count recorded inline in an annotation — are
the values used for scoring.** This is required for all ten languages equally, so that no language's
denominator depends on when its agent happened to run. The tokenizer records its own `script_sha256` in
every output, so the pass is auditable.

**Self-test:** 27/27 frozen fixtures pass after the fix.

---

## D-5. Micro correctness gate falsely FAILED correct single-workload programs (FIXED)

**Found:** by spot-checking a completed C++ implementation against the gate.

**Defect.** Each micro workload is implemented as its own standalone program printing ONE of the eleven
golden lines. `check_micro.py` iterated over all eleven EXPECTED lines and marked every line absent from
the candidate output as `missing from output` -> FAIL. A correct single-workload program therefore always
failed: C++ `mb01` printed `MB01 61899717`, byte-identical to golden, and the gate reported FAIL.

**Why it mattered.** Implementation agents were told to iterate until the gate passed. A gate that
cannot pass a correct program invites an agent to "repair" already-correct code, and would have made the
whole micro suite unmeasurable. This is the mirror image of the spec 10.4(c) hazard: a validator that
cannot PASS is as broken as one that cannot FAIL.

**Fix.** The gate now compares the intersection of expected and actual workload lines, reporting
uncovered lines as `SKIP` rather than failure, with two soundness guards:

1. an output covering NO expected workload line is always a FAIL, so an empty or silent program cannot
   pass vacuously;
2. `--require-all` restores strict all-eleven checking, and is used for the whole-suite verification.

**Re-verified in all four directions after the fix:**

| Input | Expected verdict | Result |
|---|---|---|
| single workload, correct value | PASS | PASS |
| single workload, value off by one | FAIL | FAIL |
| empty output | FAIL | FAIL |
| single workload with `--require-all` | FAIL | FAIL |

**Impact on scores:** none. No timing had been run and no score computed. Implementations produced before
the fix are re-verified against the corrected gate; any that are genuinely wrong are repaired, and any
that were correct all along are confirmed rather than re-written.

---

## D-6. SVM workload: erratum in an informative (non-normative) dataset value

**Found by:** the WL-SVM C reference implementation, and independently re-verified by recomputing the
frozen generator directly.

**Defect.** Methodology 07 §2.3 states, as an *informative* illustration, that the first training point
is `x[0] ~= (0.6836, 1.5163, 0.3262, -0.5479)`. A literal implementation of the frozen generator —
`LCG-PM` seeded `1234567`, `state = (48271*state) mod 2147483647` with the post-update value returned,
`next_uniform = state / 2147483647.0`, `next_normal = Irwin-Hall(12) - 6.0`, `x = MU1[d] + 0.8*next_normal()`
with `MU1 = [1.0, 1.0, 0.5, -0.5]` — produces `(0.8992, 0.1125, 0.9713, -0.3644)`.

**Independent verification.** Recomputed directly from the frozen parameters, without reference to any
implementation:

```
recomputed x[0] = [0.8992, 0.1125, 0.9713, -0.3644]
spec 2.3 states = [0.6836, 1.5163, 0.3262, -0.5479]
```

Plausible off-by-one variants were probed and none reproduces the stated value (returning the pre-update
state gives `(0.1696, 0.2995, 0.7783, -0.2156)`; seed `1` gives `(0.4440, 2.8685, 1.6569, -0.5525)`).

**Why the generator is right and the informative line is wrong.** Every *other* pinned figure in §2.6 and
§2.8 matches the reference exactly, including values that could not match under a different dataset:
`ALPHA_CHECKSUM 126.024432` (a function of all 200 alphas in index order), `ALPHA_Y_SUM 0.047792639128`
to twelve decimals, and the whole discrete-robustness table (135 alphas exactly zero, smallest non-zero
alpha 1.551e-3, largest alpha 0.1304, min |f| over test 1.010e-1, min |f| over train 2.767e-2). A wrong
dataset would perturb all of these. Only the §2.3 illustration disagrees.

**Resolution.** Methodology 07 §1.11 already fixes the precedence: *"If a conforming oracle disagrees with
an informative value, the oracle wins and the informative value is corrected as an erratum."* The
generator is normative; §2.3's `x[0]` line is informative. Erratum recorded:

> 07 §2.3 informative `x[0]`: `(0.6836, 1.5163, 0.3262, -0.5479)` -> `(0.8992, 0.1125, 0.9713, -0.3644)`

**Impact on scores:** none. The golden output is produced by the generator, not by the illustration, and
the two independent references agree byte-for-byte
(`sha256 ce23320612111b75a7475b1899bb8bc2e30978b975d5019e200a52499391119d`). No language is affected,
because all ten implement the same normative generator.

---

## D-7. SYSTEMATIC PRO-QUIDRA BIAS in the first-draft methodology (FOUND, REMEDIATED, RE-AUDITED)

This is the most important entry in this log. It records that the benchmark's own methodology, as first
drafted, was biased toward the language under evaluation — and that the bias was found and removed
**before any score was computed**.

### How it was found

One independent adversarial auditor per document, each instructed to hunt for bias toward Quidra,
mechanical irreproducibility, and spec non-compliance, and to report finding none if that was the truth.

**Result: 10 of 10 documents flagged for pro-Quidra bias. 35 blockers, 87 majors, 56 minors.**

### The five most material defects

| # | Defect | Effect |
|---|---|---|
| 1 | **A capability Quidra uniquely lacks was absent from the universe.** No probe required a function value or closure — the construct Quidra rejects with `FUNCTION_NOT_VALUE` — and three probes routed around it. Spec §32 forbids removing a capability because Quidra lacks it. | The one gap the other nine languages don't share was worth **0 of 103 points** |
| 2 | **A false provenance claim.** The universe asserted no Quidra documentation was consulted; probe F15.P1's task is Quidra's README generics example, and the generated Quidra fragment reproduced it verbatim. | Probes shaped by the subject language |
| 3 | **Asymmetric safety posture.** Rust pinned to `-O` (overflow silently wraps) and Zig to `-OReleaseFast` (overflow is UB), while Quidra has only an optimized-AND-checked mode — then overflow probes scored exactly that difference. | Competitors measured checks-off against Quidra checks-on |
| 4 | **Determinacy double-counted.** Support rubric criterion F-5 required facts be "DETERMINABLE", pushing dynamically typed languages to PARTIAL for being dynamic — paying once in Capability Coverage and again in metrics B/D. | Python and TypeScript penalised twice |
| 5 | **One-token interpolation hole** (logged separately as D-4). | Discounted the five interpolating languages, Quidra among them |

### What was done

All ten documents remediated, then independently re-audited by agents instructed to verify against the
document text and explicitly NOT to trust the changelogs. Every remediation agent that reported a
direction reported **`quidra_score_direction: lower`**.

Headline structural changes:

- **Four counterpart probes added** for capabilities Quidra lacks: `F05.P3` (function/closure value
  constructed and passed), `F04.P3` (heap object identity), `F14.P3` (third alternative added from a
  separate unit), `F15.P3` (runtime dispatch through an abstract element type). Universe: 40 → **44
  probes**, capability points re-derived to a uniform 2 per probe (**denominator 88**).
- **Safety posture equalised** for Primary Evaluation 1: Rust `-O -C debug-assertions=on`, Zig
  `-OReleaseSafe`, TypeScript `--strict` — each verified on this host to actually trap
  (see `00_cross_language_constraints.md` C-8).
- **Criterion F-5 deleted** and 18 "a reader must determine" clauses marked non-normative, ending the
  determinacy double-count.
- **The false provenance claim withdrawn** and README-derived identifiers and constants renamed.
- **Quidra's execution-mode exemption removed**; safety evidence now scores the mean of both modes
  (C-10), so an interpreter-only defect lowers Quidra's safety scores.

### Residual blockers, fixed by hand after the re-audit

Three survived the automated round; all three were cross-document consistency holes and all three are
fixed and recorded as C-8, C-9 and C-10. C-9 is notable: it found that the LightGrad delegation hazard
(C-6) existed unaddressed in **four more scored tasks** — SVM and GMM carried no prohibited-facilities
rule, so Quidra could have used `linear.dot`/`stats.mean` from its own standard library while Python was
barred from `numpy`.

### Why correcting a "frozen" document here is legitimate

Spec §25.4 forbids changing formulas, weights or definitions **after observing results**. At the time of
these corrections **no probe had been scored, no workload timed, and no LLM trial run**. The single
partial probe pass that had begun was stopped and discarded
(`semantic-compression/_superseded_run1/`). The pre-remediation text of every document is preserved with
SHA-256 checksums in `methodology/_pre_remediation_snapshot/`, and the complete findings in
`methodology/_audit_findings/`, so the distinction is auditable rather than asserted.

### Honest statement of what this means for the result

A benchmark that a language's own authors commission, on their own language, is exactly where
motivated reasoning is expected. It appeared here — not as fraud, but as a hundred small defaults each
leaning the same way. It was caught by adversarial review rather than by good intentions. Readers should
weight this log accordingly: the corrections all moved Quidra's expected scores **down**.

---

## D-8. Semantic Compression is MEASURED but NOT CROSS-LANGUAGE COMPARABLE (structural finding)

This entry records the most consequential methodological result of the run: after two rounds of
adversarial audit and a full correction pass, **Primary Evaluation 1 cannot honestly publish a ranking.**

### What was done

1. 44 probes implemented and annotated in all 10 languages, every fragment compiled and run.
2. A cross-language consistency audit, one agent per probe reading all ten languages side by side:
   **44 blockers against Quidra, 6-17 against each other language; not one probe judged consistent.**
3. A correction pass applying those findings in both directions (Quidra: 41 blockers + 70 majors
   applied, 8 rejected with cited evidence).
4. Four spotcheck agents, one per contested metric, re-examining all ten languages.

### The verdict

All four spotchecks returned **not comparable**, and all four found residual bias **still favouring
Quidra**. The root cause, named by the Semantic Density spotcheck:

> the ledgers contain essentially only COUNT rows (1.1-3.8% non-recoverable), so **annotation effort,
> not language semantics, sets `fact_count`**

That is a defect in the metric as operationalised, not in any annotator. Semantic Density divides
recovered facts by source tokens. The denominator is sound — the frozen tokenizer is mechanical and was
re-derived by hand for two probes. The numerator is a human/agent enumeration whose depth varies, and
the frozen rule fixes which rows *exist* but not how exhaustively an annotator walks the site matrix.
Quidra's own correction agent disclosed this against its own interest:

> metric A on these probes measures annotation depth in Quidra's favour until the nine peers are re-swept

### What the correction DID establish

The correction was not futile; it removed a real artefact and it moved the ranking:

| | Pre-correction | Post-correction |
|---|---|---|
| Quidra Hidden Semantic Cost (raw) | 0.242 events/probe (**15x** better than Python) | 1.152 (**3.1x**) |
| Rank 1 | Quidra 82.25 | **Go 86.32** |
| Quidra | 1st (82.25) | 2nd (81.44) |

Quidra is charged on seven of the nine hidden-behaviour checklist items after correction, and on
Determinacy it is now second to Zig and charged *more* than Python on three probes — which the spotcheck
called "the signature of a metric being measured rather than steered".

### How this is reported

The corrected score table is published as **raw evidence with the comparability verdict attached**, and
the ranking is recorded as **NOT ESTABLISHED**. Specifically:

- The Go / Quidra / Zig ordering (86.32 / 81.44 / 81.08) is **not resolved** by this measurement. The
  spread is smaller than the residual annotation-depth effect.
- No Semantic Compression Ranking is published as a result. Spec §4 forbids scoring from anything but
  measured, reproducible evidence, and §32 forbids reporting an unexecuted or unsound measurement as
  executed and sound.
- The full verdicts are preserved verbatim in `semantic-compression/COMPARABILITY_VERDICT.md`;
  pre-correction annotations and scores are preserved under
  `semantic-compression/raw/_pre_audit_annotations/` (with SHA-256) and
  `semantic-compression/scores/PRE_CORRECTION_scores.*`.

### What would fix it

A single annotator (or one fixed script) performing the site-matrix walk to identical depth across all
ten languages, with the per-site row set fixed mechanically before any language is annotated, so that
`fact_count` cannot vary with effort. That is a re-run of the annotation phase under a stricter
protocol, not a re-scoring of the existing data.

### Honest summary

The benchmark did its job here. It caught a pro-Quidra tilt in its own methodology (D-7), corrected it,
measured, caught a *second* tilt in the measurement itself, corrected that too — and then reported that
the remaining number still is not trustworthy enough to rank on. Publishing "Quidra first" from the
uncorrected data, or "Go first" from the corrected data, would both have been more satisfying and less
true.

---

## D-9. Zig's Correct@1 failure was a HARNESS defect, not a language failure (CORRECTED)

**Found by:** the independent adjudicator for the Zig trial, which flagged a conflict with frozen rule
C-4 rather than accepting the trial's own self-report.

**What was recorded initially.** Zig was the only one of ten languages to fail Correct@1: `gen_00` failed
to build, and a single repair produced a passing program. On the raw numbers that is
Correct@1 = 9/10, with Zig the sole failure.

**What the artifacts actually show.** `gen_00`'s only defect was the stdout call:

```
solution.zig:364:28: error: root source file struct 'posix' has no member named 'write'
        const n = std.posix.write(1, outbuf[off..outlen]) catch break;
```

The diff between `gen_00.zig` and the repaired `gen_01.zig` is **8 lines, all of them output plumbing**:

```
<     const n = std.posix.write(1, outbuf[off..outlen]) catch break;
---
>     std.Io.File.stdout().writeStreamingAll(io, outbuf[0..outlen]) catch {};
```

**Not one line of the SVM algorithm changed.** The data generation, the Gram precompute, the
Gauss-Seidel sweep, the clamping chain, the model recovery and the evaluation were all correct on the
first attempt.

**Why this is a harness defect.** Frozen rule C-4 in `00_cross_language_constraints.md` states:

> **Zig 0.16** replaced the old `std.io` stdout helpers with an explicit `Io` instance. Where a task's
> correctness does not depend on knowing the current stdout spelling, the harness supplies it, and a
> model is not penalised for writing the older form.

The task specification says only that the program "prints the specified lines to stdout in the specified
order". It deliberately names no language, type, operator or standard-library symbol — that is what makes
it fair to all ten. It therefore never told the model which of Zig 0.16's stdout spellings to use, while
nine other languages' stdout spellings did not change under them. Spec §10.4(b) is explicit that a fact
the prompt withholds may not be demanded of the model.

**Correction applied.** Zig's Correct@1 is recorded as **PASS with a harness-supplied stdout
correction**, and the build failure is recorded separately as an infrastructure event, not a language or
model failure — the same treatment spec §6.2 requires for provider/infrastructure failures, which "must
be recorded separately from language/model failures and must not be silently converted into incorrect
generations".

**Corrected Correct@1: 10/10.** Both figures are published: the raw observation (9/10 built on the first
attempt) and the C-4-corrected result (10/10 algorithmically correct on the first attempt), with this
entry as the reason.

**Direction of this correction:** it *removes* the only blemish from a competitor language, and it
slightly *reduces* Quidra's relative standing, since Quidra no longer shares a distinction with nine
others but with all nine. It is applied because the frozen rule requires it, not because of who it helps.

**Note on what this does NOT excuse.** C-4 covers only the mechanical stdout-API drift. Had Zig's
algorithm been wrong, or had it failed the oracle after building, that would have been a genuine
Correct@1 failure and would have been recorded as one.

---

## D-10. I1 is INVALID — answer-in-prompt contamination in 5 of 10 languages (MY PROMPT'S DEFECT)

**Found by:** the independent I1 adjudicators for Java, Swift and Zig, which refused to certify a
Correct@1 they could not attribute to the model. Confirmed mechanically across all ten.

**The defect.** For five languages the Reference Pack's "worked example" — shown to the trial — was
byte-identical to the task's own solution, and the output it claimed was byte-identical to
`expected_output.txt`. The Zig adjudicator found three files sharing one sha256
(`05eff0a3f81230a3262eb29c094503add2f79fb51ec21161ba50bf1bc4f9432e`): the pack's worked example, the
fixture, and the answer.

Mechanical check — does the expected output appear inside the Reference Pack?

| Language | Result |
|---|---|
| Go, Java, Kotlin, Swift, Zig | **CONTAMINATED** — answer present in the pack |
| C++, Python, Quidra, Rust, TypeScript | clean |

**Whose fault this is.** Mine. My prep prompt said: *"build your fixture and pack to cover exactly the
constructs it needs, and no more"*, immediately followed by the task statement. Five agents reasonably
read that as "make the fixture demonstrate this task", and a worked example that demonstrates the task IS
the answer. The instruction needed to say that the fixture must exercise the same constructs on a
*different* problem, and a disjointness check belonged in the §10.4 pre-flight.

**Why the pre-flight did not catch it.** §10.4's three mandatory checks are: the fixture builds and its
output matches; withheld harness conventions are supplied; and validators can both pass and fail. All
three passed — correctly. **None of them asks whether the pack leaks the answer.** That is a real gap in
the frozen pre-flight, not just in my prompt, and it is recorded as such.

**Consequence: the I1 scores are withdrawn.** Correct@1 = 10/10 across all ten languages is not evidence
of specification-grounded learnability, because for five languages the trial could have transcribed the
pack, and the five clean languages cannot be compared against five contaminated ones. No I1 score, no
LLM Intrinsic Learnability Score, and no §28.1 ranking is published.

**The infrastructure is not wasted.** The transformers, mappings, round-trip validation and pre-flight
evidence are sound and preserved — all ten passed round-trip byte-identity, fixture execution, and the
both-directions validator test, including a non-vacuity case that rejects a corrupted literal no
token-survival test would catch. A future run needs only to re-author the fixtures on a disjoint problem
and add a leak check.

**Second, independent reason I1 would not have been publishable:** see
`llm-intrinsic/I1_ANONYMITY_RESIDUAL.md`. Nine of ten trials correctly identified the underlying real
language from structure alone after keyword anonymization; only Quidra came back "unknown". The
familiarity control that I1 exists to impose did not hold for nine of the ten.

**What is preserved as a real observation:** every trial produced a program that inverse-mapped, built and
ran on the real toolchain, with zero real-keyword leakage. That says the transformation machinery works.
It does not say the models learned the language from the pack.

**Required additions to a future §10.4 pre-flight**, in order of importance:
1. **Answer-disjointness:** assert that the pack's worked example is not the task solution, and that
   `expected_output.txt` does not appear anywhere in the pack.
2. **Identification-rate check:** record whether trials can name the underlying language; a high rate
   invalidates the familiarity control (this is the I3 condition's job, and I1 alone is insufficient).

---

## D-11. Two adversarial-set decisions escalated to the operator, and how they were decided

The adversarial agents refused to decide two questions themselves and escalated both. That was correct
behaviour and both are recorded here with the evidence and the reasoning.

### Decision 1 — the diagnostic lexicon under-measures precise diagnostics. NOT CORRECTED.

**The problem.** Six Quidra rows are scored **Crash (20)** when the program in fact produced a precise,
diagnosed, non-crashing runtime error:

```
Quidra runtime error[INDEX_BOUNDS] at 4:17: index -1 outside length 5
Quidra runtime error[INDEX_BOUNDS] at 12:21: index 5 outside length 5
Quidra runtime error[CALL_DEPTH_LIMIT] at 10:16: call depth limit
```

Hazard code, source line, column, offending index **and** the length. By the definitions in methodology
08 that is *Runtime Safe Detection* (75), not *Crash* (20). It is demoted solely because the frozen
lexicon matches `index out of range` / `out of bounds` and Quidra writes `outside length`. The penalty is
**55 points on each of 6 rows**.

**It is systemic, not Quidra-specific.** Rows whose diagnostics failed to match the frozen lexicon:

| Quidra | Go | Java | Zig | Kotlin | Rust | Swift | TS | Python | C++ |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 | 7 | 7 | 7 | 6 | 6 | 3 | 3 | 2 | 1 |

**Decision: the frozen lexicon stands. The scores are published as computed.**

Spec §25.4 forbids selecting or adjusting a formula after observing results. Relaxing the lexicon now —
after seeing that it costs the language under evaluation 330 points across six rows — is precisely the
move that rule exists to prevent, and the fact that the correction would **help Quidra** is the strongest
reason not to make it here. The same restraint was applied to the family-C compression disclosure and to
the binary-size metric, both of which distort in other directions.

**What is published instead:** the unmatched-lexicon count per language (table above), the six Quidra
diagnostics verbatim, and this statement: *Early Error Detection under-measures any language whose
diagnostic wording differs from the frozen fragments, Quidra most of all. Quidra's 64.44 is therefore a
floor, not an estimate, and correcting the lexicon would increase its already-first-place lead.*

**Required for a future run:** the lexicon must match on semantic content (a hazard code plus a source
location) rather than on English phrasing, and must be validated against every language's real
diagnostics before measurement, not after.

### Decision 2 — the Quidra type-binding amendment was authored late. SCORED, WITH DISCLOSURE.

**The problem.** Methodology 08 requires `quidra_type_binding_amendment.json` to be published and
checksummed **before** the first Quidra adversarial program is written, with an `if_not_produced` clause
making every Quidra row `na_authoring_defect` otherwise. The file did not exist when the measurement
began. The chunk-1 agent authored it at measurement time, **from Quidra documentation only and before
compiling or running any `.qui` file**, then flagged its own violation in a `PROVENANCE_WARNING` and
escalated rather than deciding.

**Decision: the rows are scored, and the deviation is published.**

Reasoning, stated so it can be disagreed with:

1. The rule's purpose is to prevent type bindings being *chosen after seeing which ones score well*. The
   evidence shows that did not happen: authored from documentation, before any execution, and the agent
   disclosed the violation against its own interest.
2. Voiding the rows would discard genuine measurements — including Quidra's three silent bugs and its six
   demoted-but-precise diagnostics — for a timing technicality.
3. **This decision helps Quidra**, which currently ranks first on Early Error Detection. That is exactly
   why it is stated explicitly rather than applied quietly. A reader who disagrees can discount every
   Quidra adversarial row; the counterfactual is that Quidra's Early Error Detection, Runtime Safety,
   Boundary Value Safety, Silent Bug Resistance and Debuggability all become N/A, and no Standard
   Overall Score is computable for Quidra at all.
4. The amendment claims `TM3a` (prevented-by-construction) **nowhere** — the agent confirmed Quidra has a
   concrete construct for every binding, so no Quidra row scored 100 for lacking a feature.

**Amendment sha256:** `ed1a45bdbede6aac78788861add36d4181bcefecce9f075a833c10891d167d74`

### A third observation, not a decision

`ADV-03/C` is recorded Compile-time Detection (100) but carries the flag `hazard_not_reached`: Quidra
rejected the program at `int base = -9223372036854775808` with `error[INTEGER_RANGE] Integer literal
exceeds int`, because it lexes that as unary minus applied to a literal above signed-int max. **Quidra
cannot write the minimum value of its own default integer type as a negated decimal literal.** The
underflow the case exists to test was never reached, so that 100 is not a detection of underflow.
`ADV-03/R` (75) is the row that actually measures the hazard. This is a real language finding and is
reported as one.
