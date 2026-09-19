# 07 — FROZEN Algorithm Workload Definitions: SVM, GMM, LightGrad

Status: **FROZEN**. Written before any implementation, generation, or measurement.
Benchmark run: `2026-09-17-7677581`
Spec authority: `prompt.md` sections 13 (SVM), 14 (GMM), 15 (LightGrad), 16 (LLM scenarios),
32 (prohibited practices), 6.1.7 (fairness/audit), 33 (completion criteria).

Once frozen, this document may not be changed in response to observed results. Any defect found in it
after measurement begins must be recorded as an erratum with its discovery timestamp, and the affected
measurements must be reported as-is or re-run in full for **all 10 languages**.

**Remediation status.** This document was corrected on 2026-09-18 in response to an adversarial audit
of the frozen methodology. **No score has been computed from this document, no golden file has been
frozen, and no measurement of any kind has been taken for any language**, so these corrections are
pre-registration, not post-result formula selection (spec 25.4, spec 32). Every change is itemised in
the *Remediation changelog* at the end of this file, including, for every Quidra-bias finding, the
direction the correction is expected to move Quidra's score.

---

## 0. Fixed comparison set and fairness contract

The fixed comparison set is exactly these 10 languages, in this fixed column order:

| # | Language | Toolchain (frozen) |
|---|----------|--------------------|
| 1 | Quidra | Quidra 0.2.0 (Native **and** Interpreter, both measured, kept separate in raw results) |
| 2 | Python | Python 3.14.5 |
| 3 | C++ | Apple clang 17, C++20 |
| 4 | Rust | rustc 1.95.0 |
| 5 | Go | go 1.26.3 |
| 6 | Java | OpenJDK 26.0.1 (arm64) |
| 7 | TypeScript | tsc 7.0.2 + node 24.2.0 |
| 8 | Kotlin | kotlinc 2.3.21 |
| 9 | Swift | swiftc 6.2.3 |
| 10 | Zig | zig 0.16.0 |

Build/run recipes are **not** redefined here. They are taken verbatim from
`environment/environment.json` → `frozen_toolchain_recipes`. No workload-specific compiler flag,
optimization hint, pragma, or runtime option may be added for any language.

**Recorded safety-posture asymmetry (disclosure, not an adjustment).** The frozen recipes are each
language's ordinary optimized release configuration, and those configurations do not all carry the
same runtime checking. `zig build-exe -OReleaseFast` disables Zig's bounds and overflow checks;
`rustc -O` keeps bounds checks and drops integer-overflow checks; `clang++ -O2` checks neither; Go,
Java, Kotlin, Swift, TypeScript, Python and Quidra execute these workloads with bounds checking on,
and Quidra has no unchecked mode at all. This asymmetry is **not** corrected for, **not** discounted,
and **not** excused in either direction: it is a real property of each toolchain
(`environment/environment.json` → `_scope_note`). It is recorded here so that Execution Time and Peak
RSS on these three workloads are read with it in view, and it must be reprinted as a footnote under
every Execution Time and Peak RSS table these workloads produce. No language may add or remove a
checking flag to change it.

### 0.1 Fairness rules binding this document

1. Quidra is the language *under evaluation*. No workload, size, constant, output field, tolerance,
   normalization family or scoring rule in this document was derived from Quidra's syntax, operators,
   type system, standard library, or feature set. The workloads were derived from the three reference
   C++ repositories and from the requirement of cross-language determinism.

   **One disclosed overlap, recorded rather than claimed away.** The generator of 1.2 is a 31-bit
   multiplicative generator, chosen so that it needs no 64-bit unsigned wraparound. That constraint is
   independently and unavoidably imposed by Python (arbitrary-precision integers have no wraparound)
   and by TypeScript (all numbers are binary64), so the same generator would have been chosen for
   those two alone. It *also* happens to suit Quidra, whose integer arithmetic is overflow-checked and
   cannot express wraparound (`00_cross_language_constraints.md` §C-1). This document therefore does
   **not** claim that the RNG choice is Quidra-independent, because it is not. Nor is Quidra's
   overflow-check cost hidden by it: that cost is measured directly in `06_micro_workloads.md`
   (MB-02, MB-03, MB-07, MB-11) and probed in the adversarial set, and nothing in these three
   workloads is sized or shaped to avoid it.
2. No capability was removed because Quidra might lack it. In particular the LightGrad subset
   retains dynamic graph construction, higher-order automatic differentiation, polymorphic
   abstraction, explicit resource lifetime, and an error-handling contract, regardless of whether
   any particular language (including Quidra) expresses these easily.
3. Every language is free to score well on something Quidra does badly and vice versa. The workloads
   deliberately mix arithmetic-only kernels (SVM), transcendental/linear-algebra kernels (GMM), and
   allocation- and abstraction-heavy object-graph work (LightGrad), so that no single runtime
   characteristic dominates.
4. Everything below is language-neutral, predeclared, and applied identically to all 10 languages.
   Where a quantity cannot be measured honestly, section 9 defines exactly how it is marked `N/A`
   with a machine-readable reason code. `N/A` is never available to an implementation that simply
   failed: section 9's `FAIL_SCORED_ZERO` requires a failed run to be **scored 0.0** on every bounded
   correctness fraction, so a missing capability never escapes scoring by being labelled `N/A`
   (spec 26).
5. No third-party library, package, or dependency may be used by any implementation. Only the
   language's own standard library is permitted (see 1.9).

### 0.2 Reference repositories and pinned commit SHAs

| Workload | Repository | Pinned commit SHA | Local clone (read-only) |
|----------|-----------|-------------------|--------------------------|
| SVM | `https://github.com/koba-jon/svm_cpp` | `5fa1b0951740b249a7b7bb170031b75470dbf090` | `/private/tmp/claude-501/-Users-koba-Desktop-Quidra/c7dc7758-3bc7-4b38-ac54-4af31eae0682/scratchpad/bench/refs/svm_cpp` |
| GMM | `https://github.com/koba-jon/gmm_cpp` | `f405f0a4d04ad93f04c2969bbd232c956e6b0e2d` | `/private/tmp/claude-501/-Users-koba-Desktop-Quidra/c7dc7758-3bc7-4b38-ac54-4af31eae0682/scratchpad/bench/refs/gmm_cpp` |
| LightGrad | `https://github.com/koba-jon/lightgrad` | `8656ef7a9d00ea84ab5ed2f368e94aa12ba8a8b8` | `/private/tmp/claude-501/-Users-koba-Desktop-Quidra/c7dc7758-3bc7-4b38-ac54-4af31eae0682/scratchpad/bench/refs/lightgrad` |

All three references are MIT-licensed. The reference sources were read in full for the files listed
in each workload section. The reference clones are **read-only inputs**; they are never modified.

### 0.3 Sub-variant selection (spec 13 requires this to be recorded)

`svm_cpp` contains five sub-variants: `HardMargin-SVM`, `SoftMargin-SVM`, `Kernel-SVM`, `OC-SVM`,
`SVDD`.

**Selected sub-variant: `SoftMargin-SVM`** (files read: `src/svm.hpp`, `src/svm.cpp`,
`src/main.cpp`, `README.md`, `scripts/toy.sh`, `datasets/toy/**`).

Recorded reasons (all language-neutral):

- It is the only two-class variant that is both (a) defined on linearly non-separable data, so the
  regularization parameter `C` actually binds, and (b) free of any transcendental function in the
  training loop, so bitwise cross-language reproducibility is attainable in principle.
- It exposes *all* of the quantities spec 13 asks to compare: learned parameters (`alpha`, `w`, `b`),
  predictions, accuracy, an objective/loss value, and intermediate results (per-sweep residual error).
- Its training loop contains **no RNG at all** in the reference, so no randomness had to be removed.

Recorded reasons for **not** selecting the others (recorded so the choice is auditable, not to favor
any language):

- `HardMargin-SVM`: identical loop minus the `C` clamp; strictly less semantic content, and requires
  linearly separable data.
- `Kernel-SVM`, `OC-SVM`, `SVDD`: all place `exp()` (RBF) or `pow()` inside the O(N²) inner loop.
  Transcendental functions are not bit-reproducible across the 10 runtimes (each ships its own libm
  or pure-language implementation), which would force a loose tolerance on the *only* workload that
  can otherwise be compared bit-exactly. GMM already supplies transcendental/linear-algebra coverage.

The excluded variants are **not** excluded because any language handles them badly. They remain
available for future runs.

---

## 1. Universal conventions (apply to all three workloads)

### 1.1 Numeric type

All arithmetic in all three workloads is IEEE-754 **binary64** (`double` / `f64` / `Double` /
`number` / `f64`). No workload uses binary32.

> Deviation from reference, recorded: LightGrad's reference uses `float` (binary32). This is
> deliberately changed to binary64. **Two** of the ten languages — TypeScript (all numbers are
> binary64 under JavaScript semantics) and Python (no native single-precision scalar) — cannot
> express binary32 arithmetic in their standard language core without bit-manipulation shims. The
> other eight, **including Quidra** (`float32` is IEEE-754 binary32), can. Requiring binary32 would
> impose a language-specific handicap on those two, so all arithmetic is binary64. Per the task
> ordering rule, cross-language reproducibility outranks matching the reference exactly.
>
> (Correction, recorded: an earlier draft of this paragraph said "three of the ten languages" while
> naming only two, and named no third. The count was wrong; exactly two qualify. Nothing was dropped
> because Quidra lacks it — Quidra does not lack binary32.)

Integer counters are 64-bit signed unless stated otherwise. No implementation may rely on integer
overflow anywhere in these workloads.

### 1.2 The frozen deterministic RNG — `LCG-PM` (used for data generation only)

All pseudo-random data is produced by the Lehmer / Park–Miller "minimal standard" multiplicative
generator. It was chosen because every intermediate value fits exactly in binary64 (max product
`48271 × 2147483646 ≈ 1.0369e14 < 2^53`), so **every one of the 10 languages can implement it
exactly**, with 64-bit integers *or* with doubles, without needing unsigned 64-bit wraparound,
big integers, or bit tricks. The avoidance of 64-bit wraparound is forced independently by Python and
TypeScript and *also* suits Quidra's overflow-checked integers; that overlap is disclosed in 0.1 item 1
rather than presented as Quidra-independence.

```
state : integer, 1 <= state <= 2147483646
MULT  = 48271
MOD   = 2147483647          (= 2^31 - 1, prime)

next_state():
    state = (MULT * state) mod MOD      // exact integer arithmetic
    return state

next_uniform() -> double:
    return (double)next_state() / 2147483647.0     // in (0, 1)

next_normal() -> double:                            // Irwin-Hall(12) - 6
    t = 0.0
    repeat 12 times:  t = t + next_uniform()
    return t - 6.0
```

This is the same generator that `00_cross_language_constraints.md` §C-1 independently froze for the
whole benchmark (multiplier `48271`, modulus `2147483647`), for the same reason; the two documents
agree. The seeds and the normal transform below are this document's additions.

`next_normal` is an Irwin–Hall approximate standard normal. It is used instead of Box–Muller
**specifically** because it needs no `log`, `sqrt`, `sin`, or `cos`, so the generated dataset is
bit-identical in all 10 languages. Its distributional imperfection is irrelevant: the dataset is a
fixed deterministic constant, not a statistical sample.

Accumulation order inside `next_normal` is ascending and must not be reassociated.

There is exactly **one** RNG stream per workload, seeded once, drawn strictly sequentially, never
reset, never forked. Draw order is pinned in each workload section.

> Erased nondeterminism, recorded: `gmm_cpp/src/gmm.cpp` seeds `std::mt19937 mt(0)` and draws from
> `std::normal_distribution<double>` to initialize the component means. `std::normal_distribution`
> is *not* specified by ISO C++ to produce a particular value sequence, and no other language
> reproduces libc++'s sequence. That RNG is **removed entirely** and replaced by the deterministic
> initialization in 3.4. This is the single largest deviation from a reference and it is mandatory.

### 1.3 Floating-point evaluation rules binding all implementations

1. **Summation order is normative.** Wherever this document says "accumulate over `j` ascending",
   the implementation must accumulate into a single scalar in exactly that index order. Pairwise
   summation, Kahan/Neumaier compensation, vectorized tree reduction, and parallel reduction are
   **forbidden**.
2. **No algebraic reassociation across the pinned order.** An implementation may apply a
   transformation only if it is *exactly* value-preserving in IEEE-754. Two such transformations are
   explicitly permitted and named in 2.3 and are the only permitted ones.
3. **No parallelism.** All three workloads are strictly single-threaded. No threads, no goroutines,
   no SIMD intrinsics, no GPU, no async concurrency.
4. **FMA contraction is permitted** (it cannot be prevented under the frozen build recipes, e.g.
   `clang++ -std=c++20 -O2` on Apple silicon contracts `a*b+c`). This is the principal identified
   source of last-bits divergence between languages and is exactly why the normative match criterion
   in 1.6 is a tolerance, not a textual diff.
5. **Fast-math is forbidden.** No implementation may enable `-ffast-math`, `@setFloatMode(.optimized)`,
   `-Ounchecked`-style FP relaxation, or any equivalent, beyond what the frozen recipe does by default.
6. Denormals, NaN and infinity must be handled by default IEEE rules; no flush-to-zero mode.

### 1.4 Output contract

Each program writes its result **only** to stdout, as ASCII, LF-terminated (`\n`, never `\r\n`),
with a trailing newline on the final line. stderr is used only for the error-handling tests in 4.9.

Line grammar:

```
LINE   := KEY ( SP VALUE )* LF
KEY    := [A-Z][A-Z0-9_]*
VALUE  := INT | F6 | F12
SP     := a single 0x20 space
```

- No leading whitespace, no trailing whitespace, no blank lines, no banner text, no progress output,
  no timing output, no locale-dependent separators (decimal point is always `.`).
- Lines appear in exactly the order listed in the workload's output schema. No extra lines.
- `INT` — an optionally `-`-prefixed decimal integer, no `+`, no leading zeros (except `0` itself).
- `F6` — fixed-point with **exactly 6 digits after the decimal point**, never scientific notation
  (`printf("%.6f")` semantics: round-half-away-from-zero or round-half-to-even are both accepted;
  see 1.6 note 3).
- `F12` — fixed-point with **exactly 12 digits after the decimal point**, never scientific notation.
  Used only for invariant-deviation fields.
- **Negative zero is forbidden in output.** Any value `v` for which `v == 0.0` must be printed as
  `0.000000` / `0.000000000000`. Implementations must normalize `-0.0` to `0.0` before formatting.
- Non-finite values must never appear. Printing `nan`, `inf`, `-inf`, `NaN`, `Infinity`, etc. is an
  automatic **FAIL** (and a Silent Bug if the exit status is 0).

Programs take **no command line arguments** except the two frozen selectors — the error-mode
selector `--error <N>` defined in 4.9 and the timing selector `--warm` defined in 1.10 — read no
files, and read nothing from stdin. Both selectors exist in all three workloads and in all 10
languages, are spelled identically everywhere, and are part of the frozen specification handed to
every implementation. With neither selector the program performs exactly one normal run and prints
exactly the workload's output schema.

Exit status must be `0` on a normal run.

### 1.5 The golden reference output

For each workload there is exactly one normative golden output file:

```
standard/workloads/golden/svm.out
standard/workloads/golden/gmm.out
standard/workloads/golden/lightgrad.out
```

Golden generation procedure (frozen, section 8): the golden file is produced by the **C++ oracle**
(an implementation of this document, built with the frozen C++ recipe) and must be confirmed
field-for-field, within the *strict* band of 1.6, against an **independent Python oracle** written
from this document by a different route. If the two oracles disagree outside the strict band on any
field, the golden file is **not** frozen and the discrepancy must be resolved and recorded before
any language is measured. The oracles are measurement infrastructure; they are **not** benchmark
submissions and are never counted as any language's implementation.

For LightGrad, and for the closed-form invariants of SVM and GMM, the golden values are additionally
checked against implementation-independent closed forms (4.5, 4.6, 2.6, 3.7).

### 1.6 Numerical tolerance (spec 13 requires this to be stated explicitly and saved)

Frozen, saved as `methodology/frozen_tolerance.json` (its normative fields reproduced here so this
document is self-contained; the file additionally carries provenance keys — `frozen_on`,
`benchmark_run_id`, `applies_to`, `applies_to_languages` — and must also carry the normalization
epsilons frozen in 6.1):

```json
{
  "tolerance_id": "ALGO-TOL-1",
  "frozen": true,
  "print_ulp": { "F6": 1e-6, "F12": 1e-12 },
  "numeric_match": { "abs": 1e-9, "rel": 1e-6,
    "predicate": "abs(actual - golden) <= max( abs + rel * abs(golden), print_ulp[kind] )" },
  "strict_match":  { "abs": 1e-12, "rel": 1e-12,
    "predicate": "abs(actual - golden) <= max( abs + rel * abs(golden), print_ulp[kind] )" },
  "discrete_fields": "exact equality required",
  "invariants": {
    "svm_alpha_y_sum_abs_max": 1.0,
    "gmm_gamma_row_dev_max": 1e-9,
    "gmm_pi_sum_dev_max": 1e-9,
    "lightgrad_closed_form_rel": 1e-9
  },
  "rationale": "1e-6 relative absorbs FMA contraction under the frozen build recipes and the per-runtime differences in log/exp/sqrt implementations across 10 toolchains, amplified over 1000 SVM sweeps and 60 EM iterations; it is far tighter than any genuine algorithmic error, which shows up at 1e-2 or larger in every failure mode observed in the reference algorithms. The print_ulp floor exists because the output contract quantizes to a fixed number of decimal places: two correct implementations may legitimately print adjacent last digits, and that difference must never be scored as an error."
}
```

The `print_ulp` floor is load-bearing and is **not** a loosening of the criterion. `F6` output has an
absolute granularity of `1e-6`, so for a field whose golden magnitude is below about `1e-3` the
relative band alone would be narrower than the printing granularity itself and two *correct*
implementations could be scored as disagreeing. The floor removes that artefact. Its consequence is
that individually, small-magnitude fields (notably the 200 `ALPHA` entries, most of which are
`O(1e-2)`) are weak discriminators; this is why every workload also prints aggregate, `O(0.1)`-to-
`O(1e4)` discriminators (`W`, `B`, `OBJECTIVE`, `ALPHA_SUM`, `ALPHA_CHECKSUM`, `LOGLIK`, `COMP`
lines, `TENSOR_*`, `STRESS_LOSS_*`), against which the relative band binds normally.

Relationship to `06_micro_workloads.md` §2.4 (`|Δ| <= max(1e-9·|expected|, 1e-12)`): that band is
correct for single-pass micro kernels printed at 17 significant digits. `ALGO-TOL-1` is deliberately
looser because these three workloads iterate (1000 SVM sweeps, 60 EM iterations, 150 LightGrad
steps), so any last-bit difference is amplified, and because the output contract here quantizes to a
fixed number of decimal places. `00_cross_language_constraints.md` §C-2 explicitly provides that the
tolerance is recorded **per workload**; this is that record for `WL-SVM`, `WL-GMM` and `WL-LG`.

Definitions:

- **Numerical Match** (normative, reported per workload per language): every `F6`/`F12` field
  satisfies the `numeric_match` predicate against the golden file **and** every `INT` field is
  exactly equal. Reported as a boolean plus the count of failing fields and the worst observed
  relative deviation.
- **Strict Numerical Match** (diagnostic only, never a pass/fail gate): the same with the
  `strict_match` predicate. Useful for showing which languages are bit-for-bit equivalent.
- **Exact Textual Match** (diagnostic only): byte-identical stdout. Not required: a ±1-ulp difference
  in the last printed digit is a legitimate consequence of differing decimal-conversion rounding and
  must never be scored as an error.
- **Numerical Error** (reported metric): `max over F6/F12 fields of |actual - golden| / (1 + |golden|)`.
- **Prediction Match / Assignment Match** (reported metric): fraction of the discrete prediction
  vector (`PRED` for SVM, `ASSIGN_*` for GMM) equal to golden. Must be `1.000000` to pass.
- **Parameter Match** (reported metric, `WL-GMM`): the fraction of the 52 `COMP` scalar fields
  (4 components × (1 `PI` + 3 `MU` + 9 `SIGMA`)) satisfying `numeric_match` against golden. Must be
  `1.000000` to pass. Reported with the count of failing fields and the worst relative deviation.
- **Likelihood / objective agreement** (reported metric, all three workloads): a boolean —
  `numeric_match` satisfied on `OBJECTIVE` (`WL-SVM`), on `LOGLIK` **and** `LOGLIK_PER_POINT`
  (`WL-GMM`), and on all four `STRESS_LOSS_*` (`WL-LG`) — reported together with the relative
  deviation of each named field, so the boolean is never published without its evidence.
- **Accuracy** (reported metric, `WL-SVM`): a boolean — exact equality of `TEST_CORRECT` **and**
  `numeric_match` on `TRAIN_ACC`, `TEST_ACC`, `TEST_ACC_C1`, `TEST_ACC_C2`. The raw accuracies are
  additionally published in their natural unit as a non-normalized diagnostic; they are **not**
  normalized and **not** scored on their own, because every conforming implementation produces the
  same accuracy by construction and the raw value would discriminate nothing.
- These four definitions, together with Numerical Match and Numerical Error, are **overlapping views
  of the same stdout**: a field such as `OBJECTIVE` is inside Numerical Match, inside Likelihood
  agreement, and inside Numerical Error. They are collected separately because spec 13/14/15 names
  them separately; 6.1 records the overlap so that a downstream weighting does not silently count one
  property three times.

Note 3 on rounding: the `%.6f` tie-breaking rule differs between C (round-half-to-even from the exact
binary value) and some other runtimes. The comparator parses each printed decimal back to a binary64
and applies the tolerance predicate including the `print_ulp` floor, so a one-in-the-last-printed-digit
difference never causes a false failure.

### 1.7 Run outcome taxonomy (spec 13's Silent Bug requirement)

Every (language × workload × scenario × trial) run is classified into **exactly one** outcome. The
classifier is mechanical and is applied in this order:

| Order | Outcome | Condition |
|---|---|---|
| 1 | `COMPILE_FAIL` | The frozen build recipe exits non-zero (for Python and Quidra Interpreter: the parse/import step fails). |
| 2 | `TIMEOUT` | The run exceeds the per-run wall-clock limit of **1800 s**, the globally frozen per-process timeout of `06_micro_workloads.md` §5.7. |
| 3 | `RUNTIME_FAIL` | The process exits non-zero, is killed by a signal, or aborts/panics/throws out of `main`. |
| 4 | `OUTPUT_CONTRACT_FAIL` | Exit status 0 but stdout violates 1.4 (missing/extra/misordered line, wrong field count, malformed number, non-finite token, negative zero, wrong line terminator). |
| 5 | **`SILENT_BUG`** | Exit status 0, stdout is contract-valid, but **any** of: (a) a discrete field differs from golden; (b) an `F6`/`F12` field fails the `numeric_match` predicate; (c) a frozen invariant in 1.6 is violated; (d) the implementation is found to have subverted the workload (see 1.8). |
| 6 | `PASS` | None of the above. |

`SILENT_BUG` is **never** counted as a successful implementation, is never merged into
`PASS`, and is reported as its own column in every results table. `Test Pass Rate` counts only
`PASS`. `Compile / Parse Success` counts everything except `COMPILE_FAIL`. This is the spec 13 and
spec 32 requirement and it is non-negotiable.

A run classified `SILENT_BUG` must additionally record: the first failing field key, the actual
value, the golden value, and the observed relative deviation. This is the auditable evidence.

A `TIMEOUT` run's `Execution Time` raw value is the censored constant **`1800.0` s** with
`timed_out = true` and `censored = true`, and the censored value is used for normalization, exactly as
`06_micro_workloads.md` §5.7 already requires. A timeout is a real performance result; it is never
`N/A`.

**`LATENT_DEFECT` is an orthogonal annotation, not a seventh outcome.** A deviation from the pinned
algorithm that **provably cannot change any printed field at the frozen constants** — the only
instance identified in this document is omitting `S_inside` from the `w` accumulation of 2.5 when
`NS_INSIDE == 0` — is recorded as `latent_defect: true` on the run, with the proof, in the 1.8 review
record, and is published in its own results column. It does **not** change the run's outcome class in
the table above, and it is **not** a subversion under 1.8 (which concerns deviations that make a
non-conforming program *appear* to conform, not deviations that are inert at the frozen constants).
Any deviation for which such a proof cannot be given is a subversion and is `SILENT_BUG`. The same
annotation is available to all 10 languages.

### 1.8 Anti-subversion clause

An implementation is subverting the workload — and is classified `SILENT_BUG` even if its output
matches golden — if it does any of:

- hard-codes any output value, or any part of the golden file, instead of computing it;
- skips iterations, memoizes across the pinned loop, or exits the loop early on a condition not
  defined in this document;
- uses a different algorithm that happens to agree (e.g. replacing SVM's O(N²) dual sweep with a
  closed-form QP solve, or replacing GMM's EM with k-means);
- uses multiple threads, SIMD intrinsics, or a numerical library;
- reads the golden file, the reference repositories, or any network resource at run time.

This check is a mandatory source review of every submitted implementation. It is **not** a free-text
judgement: it is the closed checklist below, answered `yes`/`no` per submission, each answer carrying
a source line citation (file line numbers of the submitted single file). The checklist is identical
for all 10 languages and is answered by the same reviewer procedure for all of them.

| # | Question (answered per submission, with a source line citation) | `yes` ⇒ |
|---|---|---|
| S1 | Is any printed output field, or any part of the golden file, written as a literal instead of computed from the workload's inputs? | `SILENT_BUG` |
| S2 | Does any pinned loop execute a number of trips other than its frozen constant (`SWEEPS = 1000`, `ITERS = 60`, `STEPS = 150`, `N`, `K`, `D`, `order`)? | `SILENT_BUG` |
| S3 | Does control leave a pinned loop, or skip a pinned iteration, on any condition not defined in this document? | `SILENT_BUG` |
| S4 | Is any result memoized, cached or reused across a pinned loop in a way this document does not define? | `SILENT_BUG` |
| S5 | Is a different algorithm substituted for a pinned one (closed-form QP for the SVM dual sweep; k-means for GMM's EM; a topological sort, visited set or reverse-mode tape reordering for 4.4's recursive backward; an analytic gradient for an autodiff one)? | `SILENT_BUG` |
| S6 | Does the implementation use threads, goroutines, async concurrency, SIMD intrinsics, a GPU, or any facility on the 1.9 prohibition list — including the entries named there for the implementation's own language? | `SILENT_BUG` |
| S7 | Does the program read the golden file, a reference repository, any other file, or any network resource at run time? | `SILENT_BUG` |
| S8 | For `WL-LG` only: is the number of distinct concrete realizations of the `Function` abstraction in the source anything other than 6, or is the printed `NODE_TYPES` value not derived from that set (4.8)? | `SILENT_BUG` |
| S9 | Is there any other deviation from the pinned algorithm? If yes, is it accompanied by a proof that it cannot change any printed field at the frozen constants? | proof given ⇒ `latent_defect: true` (1.7), outcome unchanged; **no proof ⇒ `SILENT_BUG`** |

`subversion_review` is `fail` if any of S1–S8 is `yes`, or if S9 is `yes` without a proof. A `fail`
yields `SILENT_BUG` **regardless of whether the output matches golden**. The completed checklist,
including every citation and every S9 proof, is preserved per submission in
`results/algo/subversion_review/<language>/<workload>/<scenario>/<trial>.json` and is published.

### 1.9 Structural requirements (identical for all 10 languages)

- **One source file per workload per language**, because the frozen build recipes compile a single
  file. "Multiple modules" therefore means **logical** modules: the language's own namespacing
  construct (`namespace`, `mod`, `package`-like object, `object`/`enum` namespace, `class` used as a
  module, or top-level grouping with a documented name), not separate files. The required module
  names are given per workload and must appear, spelled identically (case may follow the language's
  convention), in all 10 implementations.
- **Standard library only, and the numeric kernels are written, not imported — in every language.**
  In all three workloads every language implements the numeric kernels itself, and in `WL-LG` every
  language implements the autodiff engine itself, from general-purpose facilities only: scalar
  arithmetic, arrays, dynamic containers, maps, string formatting, and the scalar
  `sqrt`/`exp`/`log` of the language's own math facility. Prohibited in all ten languages
  symmetrically, whether the facility is a third-party package, a first-party package, or a namespace
  built into the language itself:

  | Language | Prohibited in all three workloads |
  |---|---|
  | **Quidra** | the `linear` namespace in any form (`linear.dot`, `linear.matmul`, and any other validated linear-algebra fast path); the `neural` namespace in any form (`neural.track`, `neural.grad`, `neural.Parameter`, `neural.State`, `neural.Gradients`, `neural.mean`, the differentiable reductions, `neural.save`/`neural.load`); the `dnn` package |
  | Python | `numpy`, `torch`, `jax`, `autograd`, `tensorflow`, `array`-backed BLAS shims |
  | C++ | Eigen, any BLAS/LAPACK, `std::valarray` vendor kernels, `<execution>` parallel policies |
  | Rust | `nalgebra`, `ndarray`, `packed_simd`, `std::simd`, `rayon` |
  | Go | `gonum`, `golang.org/x/exp` numeric kernels |
  | Java | any linear-algebra or autodiff library, the Vector API, parallel `java.util.stream` operations |
  | Kotlin | the same as Java, plus any multiplatform numeric library and any coroutine-parallel reduction |
  | TypeScript | any tensor, BLAS or autodiff package (`tfjs`, `ndarray`, `mathjs`, …), typed-array vendor kernels |
  | Swift | `Accelerate`, `simd`, `BLAS`, `Numerics` matrix kernels |
  | Zig | any tensor/autodiff/BLAS package; `@Vector` SIMD kernels in the pinned loops |

  The list names the language under evaluation first and covers all ten. A facility being *built into
  the language* rather than *imported as a library* makes no difference: the workloads measure how
  each language expresses the construction of these kernels, and outsourcing the core computation to
  a native fast path for one language would breach spec 32 exactly as an external library would. This
  bullet is the in-document statement of `00_cross_language_constraints.md` §C-6 and governs whether
  or not that document is at hand.
- The program's entry point runs the whole workload and terminates.

### 1.10 Performance measurement hooks

Timing, memory, and compile-time measurement are defined in the environment/measurement methodology,
not here. This document only fixes what makes those measurements comparable:

- Each workload is a **whole-program** run; process-level wall time therefore includes runtime
  startup and JIT warm-up for Java, Kotlin, TypeScript/node, Python and Quidra Interpreter. That is
  intentional and pre-declared. To keep the comparison honest, the same harness must also record
  each language's `STARTUP_TIME` from a no-op program built with the identical recipe, and report it
  alongside, so startup can be separated from algorithm time by the reader. Startup time is never
  silently subtracted from a reported figure.
- **Baseline RSS, the memory counterpart of `STARTUP_TIME`.** The harness records `BASELINE_RSS` for
  each language from the *same* no-op program built with the identical recipe, and publishes it beside
  every Peak RSS figure, together with the ratio `peak_rss_i / best_positive_peak_rss` required by
  6.1. Baseline RSS is **never silently subtracted** from a reported figure. No workload-specific
  heap, GC, arena or nursery flag may be added for any language (section 0), so the baseline is the
  honest floor of that runtime and it is *disclosed* rather than corrected for. Peak RSS on `WL-SVM`
  and `WL-GMM`, whose working sets are ≈ 320 KB and ≈ 1.6 MB, is dominated by that floor for every
  language and is reported for completeness rather than as a discriminator (6.1). Only on `WL-LG`
  does the workload's own allocation behaviour exceed the floor for every language, and 4.7 states how
  that signal is read.
- Measurement protocol per (language × workload): 1 untimed warm-up run + **5** timed runs; report
  min, median, mean, sample standard deviation; the **median** is the headline figure.
- Peak RSS is taken from the maximum resident set size of the timed runs (median of 5).
- **Cold and warm execution are measured separately and are never mixed in one column** (spec 12).
  `EXEC_COLD` is the median of the 5 whole-process runs above. `EXEC_WARM` is measured in a second
  frozen configuration, selected by the frozen `--warm` flag of 1.4, in which the entry point executes
  the whole workload body **twice in one process**, prints nothing on the first pass, prints the
  normal output schema on the second, and the harness times **only the second execution** using the
  language's own monotonic clock around the second body. The configuration is identical for all 10
  languages and all three workloads; it is excluded from Peak RSS and from Compile Time. Every
  published table names which of the two it contains, and no table places a cold figure and a warm
  figure in the same column. `EXEC_WARM` exists because a no-op program's startup does not contain the
  workload's JIT warm-up, which is what spec 12's separation rule is about; `06_micro_workloads.md`
  §5.4 solves the same problem structurally for the micro suite.
- **Compile / Parse Time uses the frozen cache discipline of `06_micro_workloads.md` §5.5,
  restated here because leaving it unstated made the metric irreproducible.** Per (language ×
  workload) with a build step: (1) one **priming build**, not timed, so the toolchain's own caches
  (`GOCACHE`, Rust's incremental state, the `javac`/`kotlinc` JVM class loading, the Swift and clang
  module caches, `tsc`'s `.tsbuildinfo`, Zig's local and global caches, Quidra's build cache) are in
  their ordinary warm, everyday state; (2) delete the produced artifacts only (`BIN`, `OUT/`,
  `FILE.js`, `FILE.jar`, `__pycache__`) — **do not** purge any toolchain cache; (3) run the frozen
  build command under the harness's monotonic timer; (4) repeat (2)–(3) **5 times** and take the
  median. Artifact deletion is outside the timed window. The identical sequence is used for all 10
  languages, which is what makes the number reproducible by an independent analyst; whether the
  discipline is warm or cold matters far less than that it is pinned, and it is pinned to whatever
  `06` §5.5 says so that the two documents can never disagree. `Parse Time` for Python and Quidra
  Interpreter is measured under the same discipline with the commands named in section 9.

### 1.11 Informative expected values

Every "informative reference value" quoted in sections 2, 3 and 4 was produced by a conforming
prototype of this specification on the measurement host and is reproduced to help implementers
self-check. **Informative values are not normative.** The normative values are the golden files of
1.5. If a conforming oracle disagrees with an informative value, the oracle wins and the informative
value is corrected as an erratum.

---

## 2. Workload `WL-SVM` — Soft Margin SVM

Derived from `svm_cpp @ 5fa1b0951740b249a7b7bb170031b75470dbf090`, sub-variant `SoftMargin-SVM`
(`src/svm.cpp`, `src/svm.hpp`, `src/main.cpp`).

### 2.1 Required logical modules

`data`, `svm`, `app` — three logical modules, named identically in all 10 implementations.

- `data` — the LCG-PM generator and the dataset builder.
- `svm` — the model: training, support-vector extraction, `w`/`b` recovery, `f`, `g`, evaluation.
- `app` — entry point, output formatting.

### 2.2 Frozen constants

| Name | Value | Meaning |
|------|-------|---------|
| `D` | `4` | feature dimension |
| `N_TRAIN_C1` | `100` | class +1 training points |
| `N_TRAIN_C2` | `100` | class −1 training points |
| `N_TEST_C1` | `50` | class +1 test points |
| `N_TEST_C2` | `50` | class −1 test points |
| `MU1` | `[1.0, 1.0, 0.5, -0.5]` | class +1 mean |
| `MU2` | `[-1.0, -1.0, -0.5, 0.5]` | class −1 mean |
| `SIGMA` | `0.8` | per-dimension scale, both classes |
| `SEED` | `1234567` | LCG-PM seed |
| `C` | `10.0` | regularization bound (matches `scripts/toy.sh`) |
| `LR` | `0.0001` | dual ascent step (matches `scripts/toy.sh`) |
| `LIMIT` | `0.0001` | KKT residual threshold (reference default) |
| `SWEEPS` | `1000` | **fixed** number of full sweeps; see 2.4 |
| `EPS_SV` | `0.0000001` | support-vector membership epsilon (reference `eps`) |

Total training size `N = 200`. Complexity is `SWEEPS × 2N²` inner accumulations ≈ 8.0e7 flops, plus a
one-off `N² × D` Gram build. This sizes to roughly 13 s in CPython and a few tens of milliseconds in
optimized native code on the measurement host: a wide, informative dynamic range that fits inside the
1800 s cap for the slowest plausible entrant.

### 2.3 Data generation (exact draw order)

One `LCG-PM` stream, seeded with `SEED = 1234567`. The four blocks are drawn in this order and no
other; within a block, points ascending; within a point, dimensions ascending:

```
1. train class +1 : for n = 0..99 : for d = 0..3 : x = MU1[d] + SIGMA * next_normal()
2. train class -1 : for n = 0..99 : for d = 0..3 : x = MU2[d] + SIGMA * next_normal()
3. test  class +1 : for n = 0..49 : for d = 0..3 : x = MU1[d] + SIGMA * next_normal()
4. test  class -1 : for n = 0..49 : for d = 0..3 : x = MU2[d] + SIGMA * next_normal()
```

The training arrays are then concatenated **class +1 first, class −1 second**, giving
`x[0..199]` and `y[0..199]` with `y[i] = +1` for `i < 100` and `y[i] = -1` for `i >= 100`.
This matches the reference's `(1.1)` then `(1.2)` ordering in `SoftMargin_SVM::train`.

Informative self-check (non-normative, see 1.11): the first two training points are

```
x[0] = ( 0.899197,  0.112540,  0.971311, -0.364383)
x[1] = ( 0.858706,  1.140231,  1.266498, -1.292345)
```

printed at `F6`; the dataset is linearly non-separable (the frozen run misclassifies 2 of 200 training
points). Two points are given rather than one so that an implementation which reuses one RNG value for
all `D` dimensions, or resets the stream per point, is caught by the check itself.

> **Erratum ALGO-E-001, discovered 2026-09-18 during the pre-measurement audit, before any golden
> file was frozen and before any language was measured.** This line previously read
> `x[0] ≈ (0.6836, 1.5163, 0.3262, -0.5479)`. That value does not reproduce from the frozen `LCG-PM`
> (multiplier `48271`, modulus `2147483647`, seed `1234567`), the Irwin–Hall(12) normal of 1.2, and
> the draw order above; it was a stale constant from a superseded draft. The corrected values above
> were re-derived from the frozen definitions. This constant is inside the text that 5.1 extracts into
> every Scenario A and Scenario B prompt, so a wrong value here would have cost every language repair
> turns against a correct generator — the correction therefore had to be made before any measurement,
> and was. **Every remaining informative constant in 2.3, 2.6.1, 2.8, 3.9 and 4.8 must be re-derived
> from the section 8 oracles and re-stamped with the oracle build that produced it before the golden
> files are frozen**; any that does not reproduce is corrected as a further numbered erratum under
> this same procedure.

### 2.4 Training (exact)

Precompute the Gram matrix once:

```
for i = 0..N-1:
  for j = 0..N-1:
    G[i][j] = sum over d = 0..D-1 ascending of  x[i][d] * x[j][d]
```

Precomputing `G` is **mandatory**, not optional. `G[i][j]` is bit-identical to the reference's
per-iteration `dot(x[i], x[j])`, so this changes no value; it is fixed so that every language does
exactly the same amount of work.

Initialize `alpha[i] = 0.0` for all `i`, and `beta = 1.0`.

Then run **exactly `SWEEPS = 1000` sweeps**, with no early exit:

```
for sweep = 0 .. SWEEPS-1:

    judge = false
    error = 0.0
    max_abs_delta = 0.0

    # (3.1) update alpha, in ascending i, in place (alpha[j] for j < i is already updated)
    for i = 0 .. N-1:

        item1 = 0.0
        for j = 0 .. N-1 ascending:
            item1 = item1 + alpha[j] * y[i] * y[j] * G[i][j]

        item2 = 0.0
        for j = 0 .. N-1 ascending:
            item2 = item2 + alpha[j] * y[i] * y[j]

        delta = 1.0 - item1 - beta * item2

        if abs(delta) > max_abs_delta: max_abs_delta = abs(delta)

        alpha[i] = alpha[i] + LR * delta
        if   alpha[i] < 0.0 : alpha[i] = 0.0
        elif alpha[i] > C   : alpha[i] = C
        elif abs(delta) > LIMIT:
            judge = true
            error = error + (abs(delta) - LIMIT)

    # (3.2) update beta
    s = 0.0
    for i = 0 .. N-1 ascending:  s = s + alpha[i] * y[i]
    beta = beta + s * s / 2.0
```

Notes that are part of the specification, not commentary:

- The `if / elif / elif` chain is exactly the reference's. In particular, when `alpha[i]` is clamped
  to `0.0` or to `C`, the convergence flag is **not** set and `error` is **not** accumulated, even if
  `|delta| > LIMIT`. Implementations that "fix" this are wrong.
- `alpha` is updated **in place** during the sweep (Gauss–Seidel, not Jacobi). `item1`/`item2` for
  index `i` see the already-updated `alpha[0..i-1]`.
- `beta` is updated once per sweep, after the whole `i` loop.
- Permitted exactly-value-preserving rewrites (the **only** two allowed, per 1.3.2):
  1. `item2` may be computed as `y[i] * (sum over j ascending of alpha[j] * y[j])`. Since
     `y ∈ {+1,-1}` and IEEE negation is exact, this is bitwise identical. The inner sum must still be
     recomputed for every `i` in ascending `j` order (it changes as `alpha` is updated in place).
  2. `alpha[j] * y[i] * y[j] * G[i][j]` may be evaluated with the sign folded in any order, since
     multiplication by ±1 is exact.
  Factoring `item1` through a running weight vector (`y[i] * dot(x[i], w_cur)`) is **forbidden**: it
  is mathematically equal but not bitwise equal.

Why `SWEEPS` is fixed rather than a `do/while(judge)` loop: the reference's loop termination is a
threshold test on floating-point residuals and would make the iteration count itself sensitive to
FMA contraction, which differs between the 10 toolchains under the frozen recipes. With the frozen
hyperparameters the predicate is nowhere near its threshold — the measured residual at sweep 1000 is
`max_abs_delta = 3.246166`, i.e. **32 461×** the `LIMIT` of `1e-4` — so `judge` is robustly `true`
and the reference loop would not have terminated before the cap anyway. The convergence predicate is
still computed and reported (`CONVERGED`, `ERROR_LAST`, `MAX_ABS_DELTA`); it simply does not control
the loop. This removes control-flow divergence without removing the criterion.

### 2.5 Model recovery and evaluation (exact)

```
S_margin = [ i in 0..N-1 ascending : EPS_SV < alpha[i] and alpha[i] < C - EPS_SV ]
S_inside = [ i in 0..N-1 ascending : alpha[i] >= C - EPS_SV ]

w[d] = 0.0 for all d
for d = 0..D-1:
    for i in S_margin (ascending):  w[d] += alpha[i] * y[i] * x[i][d]
    for i in S_inside (ascending):  w[d] += alpha[i] * y[i] * x[i][d]

b = 0.0
for i in S_margin (ascending):
    dp = 0.0 ; for d = 0..D-1 ascending: dp += w[d] * x[i][d]
    b += y[i] - dp
b = b / (double)|S_margin|          # if |S_margin| == 0 this is a FAIL, see 2.7

f(p) = ( sum over d ascending of w[d]*p[d] ) + b
g(p) = +1 if f(p) >= 0.0 else -1
```

Accuracies: `TRAIN_ACC` over the 200 training points (class +1 block then class −1 block),
`TEST_ACC` over the 100 test points, plus per-class test accuracies. Correct means `g(p)` equals the
point's true label.

Dual objective (reported as `OBJECTIVE`):

```
OBJECTIVE = ( sum over i ascending of alpha[i] )
          - 0.5 * ( sum over i ascending of ( sum over j ascending of alpha[i]*alpha[j]*y[i]*y[j]*G[i][j] ) )
```

Ordering-sensitive parameter checksum (catches permuted or partially-correct `alpha` vectors that a
sum alone would hide):

```
ALPHA_CHECKSUM = sum over i = 0..N-1 ascending of  alpha[i] * (double)((i mod 97) + 1)
```

### 2.6 Implementation-independent invariants

**Bounds only. Every statement in this subsection is a bound or a shape constraint; no measured output
value appears here**, so the subsection can be extracted into a Scenario A prompt without leaking a
golden field (5.1). The comparator checks all of them on every run, independently of golden:

- `|ALPHA_Y_SUM| <= 1.0` — the dual feasibility residual.
- `0.0 <= alpha[i] <= C` for every printed `alpha[i]`.
- `NS_MARGIN + NS_INSIDE <= N` and `NS_MARGIN >= 1`.
- `TEST_ACC * 100` is an integer count divided by 100, i.e. `TEST_ACC ∈ {0.00, 0.01, ..., 1.00}`.
- `PRED` has exactly `N_TEST_C1 + N_TEST_C2 = 100` entries, each exactly `1` or `-1`.
- Exact equality against golden of `PRED`, `NS_MARGIN`, `NS_INSIDE`, `CONVERGED` and the accuracy
  counts is required; 2.6.1 records the measured evidence that justifies requiring it.

#### 2.6.1 Discrete-decision robustness — MEASURED, NOT EXTRACTED

> **Not extracted into any prompt.** This subsection carries measured quantities of the frozen run and
> is handled exactly like the informative blocks of 2.8 / 3.9 / 4.8: the 5.1 extraction script must
> exclude it, and must fail loudly if any numeric literal it emits also appears as a field value in
> that workload's golden file.

Recorded so that exact comparison of the discrete fields is justified rather than assumed:

| Quantity | Measured margin from its decision boundary |
|---|---|
| smallest non-zero `alpha[i]` vs `EPS_SV = 1e-7` | `1.551e-3` (15 514× above) |
| largest `alpha[i]` vs `C - EPS_SV = 10.0` | `0.1304` (76× below) |
| number of `alpha[i]` exactly `== 0.0` (bitwise) | 135 (clamped, so the comparison is exact) |
| smallest `\|f(p)\|` over the 100 test points | `1.010e-1` |
| smallest `\|f(p)\|` over the 200 training points | `2.767e-2` |
| `ALPHA_Y_SUM` at sweep 1000 | `0.047792639128` (bound is `1.0`) |

Every discrete decision in `WL-SVM` is therefore separated from its boundary by at least ten orders
of magnitude more than the tolerance band.

### 2.7 Output schema (exact, in this order)

```
SVM_VERSION 1
SWEEPS <INT>
CONVERGED <INT 0|1>                # 0 iff judge was true on the final sweep
ERROR_LAST <F6>                    # 'error' accumulated on the final sweep
MAX_ABS_DELTA <F6>                 # max |delta| over the final sweep
BETA <F6>
NS_MARGIN <INT>
NS_INSIDE <INT>
W <F6> <F6> <F6> <F6>              # D = 4 values
B <F6>
OBJECTIVE <F6>
ALPHA_SUM <F6>
ALPHA_Y_SUM <F12>
ALPHA_CHECKSUM <F6>
TRAIN_ACC <F6>
TEST_ACC <F6>
TEST_ACC_C1 <F6>
TEST_ACC_C2 <F6>
TEST_CORRECT <INT> <INT> <INT>     # correct_c1, correct_c2, total
PRED <INT> x100                    # test class+1 block (50) then test class-1 block (50)
ALPHA <F6> x200                    # full learned dual variables, index order
```

21 lines exactly.

### 2.8 Informative reference values (non-normative, see 1.11)

```
SVM_VERSION 1
SWEEPS 1000
CONVERGED 0
ERROR_LAST 19.782045
MAX_ABS_DELTA 3.246166
BETA 1.534149
NS_MARGIN 65
NS_INSIDE 0
W 0.742352 0.806731 0.048922 -0.232132
B 0.071352
OBJECTIVE 1.952006
ALPHA_SUM 2.581096
ALPHA_Y_SUM 0.047792639128
ALPHA_CHECKSUM 126.024432
TRAIN_ACC 0.990000
TEST_ACC 0.990000
TEST_ACC_C1 0.980000
TEST_ACC_C2 1.000000
TEST_CORRECT 49 50 99
PRED   (first 10) 1 1 1 1 1 1 -1 1 1 1     (entries 51..60) -1 -1 -1 -1 -1 -1 -1 -1 -1 -1
ALPHA  (first 10) 0.021140 0.000000 0.027812 0.000000 0.000000 0.000000 0.000000 0.000000 0.000000 0.002114
```

Informative runtime on the measurement host: ≈ 12.6 s for a straightforward CPython 3.14
implementation. Expect roughly 30–80 ms for optimized native code.

### 2.9 Correctness criteria

- **PASS** — outcome `PASS` per 1.7: builds, exits 0, contract-valid output, every `INT` field equal
  to golden, every `F6`/`F12` field within `ALGO-TOL-1.numeric_match`, all 2.6 invariants hold,
  and `subversion_review: pass`.
- **`Test Pass Rate`(`WL-SVM`) = 1 if the run is `PASS`, else 0 — denominator 1.** `WL-SVM` has one
  scored run, so its Test Pass Rate is a boolean expressed as a fraction; 6.1 normalizes it with
  family A and averages it with the other two workloads' rates unweighted (spec 25.2).
- **FAIL** — any of `COMPILE_FAIL`, `TIMEOUT`, `RUNTIME_FAIL`, `OUTPUT_CONTRACT_FAIL`.
- **SILENT BUG** — exits 0 with contract-valid output but a wrong answer. Concretely, and these are
  the failure modes actually reachable in this workload, each of which must be classified
  `SILENT_BUG` and never as a success:
  - Jacobi instead of Gauss–Seidel `alpha` update (a whole-sweep copy of `alpha`) — changes `W`, `B`,
    `OBJECTIVE`, typically by `>1e-2`.
  - Fixing the reference's `if/elif` chain so that clamped multipliers also set `judge`/`error` —
    changes `CONVERGED` and `ERROR_LAST`.
  - Updating `beta` inside the `i` loop instead of once per sweep.
  - Omitting `S_inside` from the `w` accumulation. At the frozen constants `NS_INSIDE == 0`, so this
    provably cannot change any printed field; it is therefore detected by question **S9** of the 1.8
    checklist, requires the S9 proof, and is recorded as `latent_defect: true` per 1.7 with the
    outcome class unchanged. Without that proof it is a subversion and is `SILENT_BUG`. (1.7, 1.8 and
    this bullet state one rule; the earlier wording, under which 1.8 said "`SILENT_BUG` even if its
    output matches golden" while this bullet said "latent defect", left two analysts free to classify
    the same source differently.)
  - `>=` vs `>` in `g(p)` at `f == 0` (not reachable here; margin `1.0e-1`).
  - Integer division when computing accuracies (`correct / total` in integer arithmetic) — a
    classic cross-language trap; yields `0.000000` or `1.000000`.
  - Reusing one RNG value for all `D` dimensions, or resetting the stream per block — changes the
    entire dataset.
  - Off-by-one in the LCG (`state` starting at 0, or returning the pre-update state).

---

## 3. Workload `WL-GMM` — Gaussian Mixture Model via EM

Derived from `gmm_cpp @ f405f0a4d04ad93f04c2969bbd232c956e6b0e2d`
(`src/gmm.cpp`, `src/gmm.hpp`, `src/parameter.hpp`, `src/main.cpp`, `scripts/toy.sh`).

### 3.1 Required logical modules

`data`, `linalg`, `gmm`, `app` — four logical modules, named identically in all 10 implementations.

- `data` — LCG-PM generator and dataset builder.
- `linalg` — Cholesky factorization, forward substitution, log-determinant.
- `gmm` — initialization, E-step, M-step, log-likelihood, assignment.
- `app` — entry point, canonical component ordering, output formatting.

### 3.2 Frozen constants

| Name | Value | Meaning |
|------|-------|---------|
| `N` | `10000` | number of data points |
| `D` | `3` | dimension (matches `scripts/toy.sh --D 3`) |
| `K` | `4` | number of mixture components |
| `K_TRUE` | `4` | number of generating clusters |
| `SEED` | `20260917` | LCG-PM seed |
| `CENTERS` | `[[-3,-3,-3],[3,-3,3],[-3,3,3],[3,3,-3]]` | generating means |
| `SCALES` | `[1.0, 1.5, 0.8, 1.2]` | per-cluster isotropic scale |
| `ITERS` | `60` | **fixed** number of EM iterations; see 3.5 |
| `EPS_CONV` | `0.000001` | convergence threshold on the log-likelihood increment |
| `REG` | `0.000001` | covariance ridge added to the diagonal each M-step |
| `LOG_2PI` | `1.8378770664093453` | `ln(2π)`, given as an exact decimal literal so no language derives it differently |

Complexity ≈ `ITERS × N × K × O(D²)` ≈ 3e7 flops plus 2.4e6 `exp` and 6.6e5 `log` calls. Informative
runtime: ≈ 6.5 s in CPython 3.14; expect ~100 ms optimized native.

> Deviation from reference, recorded: the reference's `scripts/toy.sh` uses `K = 8` on a 1000-point
> toy set. `K = 4` with `K_TRUE = 4` is used instead because `K > K_TRUE` makes EM's fixed point
> non-identifiable (components split arbitrarily and can empty out, producing `Nk[k] → 0` and a
> division by zero), which would make cross-language agreement depend on luck rather than on
> correctness. `N` is raised from 1000 to 10000 so the workload is large enough to time.

### 3.3 Data generation (exact draw order)

One `LCG-PM` stream, seeded `20260917`. Clusters are assigned **round-robin** so that the
deterministic initialization of 3.4 picks one point from each cluster:

```
for n = 0 .. N-1:
    c = n mod K_TRUE
    for d = 0 .. D-1:
        x[n][d] = CENTERS[c][d] + SCALES[c] * next_normal()
```

Clusters are well separated (center separation `6*sqrt(2) ≈ 8.49`, largest scale `1.5`), which is
what makes the EM fixed point unique and the workload reproducible.

### 3.4 Initialization (exact, fully deterministic, no RNG)

This **replaces** the reference's `std::mt19937` + `std::normal_distribution` initialization
entirely (see 1.2).

```
# global per-dimension moments, accumulated over n ascending
mean[d] = ( sum over n ascending of x[n][d] ) / (double)N
var[d]  = ( sum over n ascending of x[n][d]*x[n][d] ) / (double)N  -  mean[d]*mean[d]

pi[k]        = 1.0 / (double)K                       for all k
mu[k][d]     = x[k][d]                               for k = 0..K-1, d = 0..D-1
sigma[k][a][b] = var[b] if a == b else 0.0           for all k
```

`mu[k] = x[k]` is a deterministic Forgy-style seeding: the first `K` data points, which by 3.3 come
one from each generating cluster. `var[d]` is exactly the reference's `sigma_gauss` (the second
central moment computed as `E[x²] - E[x]²`, in that form, with that accumulation order).

### 3.5 EM iteration (exact)

Run **exactly `ITERS = 60` iterations**, with no early exit. **Each iteration is E-step then M-step.
The log-likelihood reported for iteration `t` is evaluated *after* that iteration's M-step, with the
updated parameters — it is not the `L_total` produced by that iteration's E-step, which belongs to the
parameters at the *start* of the iteration.** `LOGLIK_INIT` is the same evaluation applied to the
initial parameters of 3.4. There are therefore **61** log-likelihood evaluations in total, and
`LOGLIK` is the value after iteration 60. (The two readings differ by one EM step and would change
`DELTA_LOGLIK_LAST`, `MONOTONE` and `CONVERGED`'s input, as well as ≈ 1/60 of the workload's runtime,
so the choice is pinned rather than left to the implementer.)

Per-component factorization, computed once per E-step (not once per point):

```
cholesky(S) -> L        # lower triangular, L*L^T = S, D x D
for i = 0..D-1:
    for j = 0..i:
        s = S[i][j]
        for k = 0..j-1 ascending:  s = s - L[i][k]*L[j][k]
        if i == j:
            if s <= 0.0:  ERROR  (see 3.8)
            L[i][i] = sqrt(s)
        else:
            L[i][j] = s / L[j][j]

logdet[k] = 2.0 * ( sum over i = 0..D-1 ascending of log(L[i][i]) )
```

E-step, for `n` ascending, `k` ascending:

```
# Mahalanobis distance by forward substitution: solve L z = (x[n] - mu[k])
q = 0.0
for i = 0..D-1:
    s = x[n][i] - mu[k][i]
    for j = 0..i-1 ascending:  s = s - L[i][j]*z[j]
    z[i] = s / L[i][i]
    q = q + z[i]*z[i]

logp[k] = log(pi[k]) - 0.5 * ( (double)D * LOG_2PI + logdet[k] + q )

m = max over k of logp[k]                       # scan k ascending, strict > to update
ssum = 0.0
for k = 0..K-1 ascending:  ssum = ssum + exp(logp[k] - m)
loglik_n = m + log(ssum)
for k = 0..K-1:  gamma[n][k] = exp(logp[k] - m) / ssum

L_total = sum over n ascending of loglik_n
```

Log-domain evaluation with max-subtraction is **mandatory** (the reference evaluates the density
directly and is therefore prone to underflow; the log-sum-exp form is numerically stable, fully
specified, and identical for every language).

M-step, for `k` ascending:

```
Nk[k] = sum over n ascending of gamma[n][k]
pi[k] = Nk[k] / (double)N

for d = 0..D-1:
    mu[k][d] = ( sum over n ascending of gamma[n][k]*x[n][d] ) / Nk[k]

# covariance uses the JUST-UPDATED mu[k]  (this matches the reference's (2.2)-then-(2.3) order)
Snew[a][b] = 0.0
for n = 0..N-1 ascending:
    dev[d] = x[n][d] - mu[k][d]
    for a = 0..D-1: for b = 0..D-1:
        Snew[a][b] = Snew[a][b] + gamma[n][k]*dev[a]*dev[b]
for a = 0..D-1:
    for b = 0..D-1: Snew[a][b] = Snew[a][b] / Nk[k]
    Snew[a][a] = Snew[a][a] + REG
sigma[k] = Snew
```

`REG` is added **after** the division by `Nk[k]`, on the diagonal only, every iteration. It has no
counterpart in the reference and is added deliberately: it guarantees positive definiteness so that
Cholesky never fails and so that no language diverges into a degenerate component.

Why `ITERS` is fixed at 60 rather than `while (L - L_old > eps)`: with the frozen constants the EM
increment crosses `EPS_CONV = 1e-6` around iteration 8, and the crossing margin there is only ~1.4×,
which is *not* robust to FMA contraction — different toolchains could legitimately stop one
iteration apart. By iteration 60 the increment is exactly `0.0` at `F12` resolution and the parameters
are at a stable fixed point, so 60 fixed iterations is both deterministic and fully converged. The
convergence predicate is still evaluated and reported (`CONVERGED`, `DELTA_LOGLIK_LAST`) with an
enormous margin (`0.0` vs `1e-6`), it simply does not control the loop.

`MONOTONE` is reported as `1` iff no iteration decreased the log-likelihood by more than `1e-9`.
EM guarantees monotone non-decreasing likelihood, so `MONOTONE = 0` is an implementation-independent
proof of a bug.

### 3.6 Canonical component ordering and assignment

Component indices are permutation-free only if pinned. Before output, sort the `K` components by

```
key(k) = ( mu[k][0], mu[k][1], mu[k][2] )   ascending, lexicographic
```

and renumber `0..K-1` in that order. All printed `COMP` lines and all assignments use the sorted
numbering. (The frozen run's four means differ in their first coordinate by ≈ 6.0, so the sort is
unambiguous.)

Hard assignment: `assign[n] = argmax over k ascending of gamma[n][k]` (strict `>` to replace the
incumbent, so the lowest index wins ties), expressed in the **sorted** numbering.

```
ASSIGN_COUNTS[k] = number of n with assign[n] == k
ASSIGN_CHECKSUM  = sum over n = 0..N-1 of (assign[n] + 1) * ((n mod 97) + 1)      # exact integer
```

### 3.7 Implementation-independent invariants

**Bounds only. No measured output value appears in this subsection**, so it can be extracted into a
Scenario A prompt without leaking a golden field (5.1):

- `GAMMA_ROW_DEV_MAX = max over n of |(sum over k of gamma[n][k]) - 1.0|` must print as
  `0.000000000000` at `F12` (i.e. `< 5e-13`).
- `PI_SUM_DEV = |(sum over k of pi[k]) - 1.0|` must print as `0.000000000000` at `F12`.
- `MONOTONE` must be `1`.
- `LOGLIK > LOGLIK_INIT`.
- Every `sigma[k]` printed must be symmetric to within `1e-9` and have strictly positive diagonal.
- `ASSIGN_MIN_MARGIN` (min over `n` of `top1(gamma[n]) - top2(gamma[n])`) is reported and must be
  `> 1e-6`.
- Exact equality against golden of `ASSIGN_COUNTS`, `ASSIGN_CHECKSUM` and `ASSIGN_FIRST_20` is
  required; 3.7.1 records the measured evidence that justifies requiring it.

#### 3.7.1 Assignment-margin evidence — MEASURED, NOT EXTRACTED

> **Not extracted into any prompt**, on the same terms as 2.6.1 and the informative blocks.

Frozen run: `GAMMA_ROW_DEV_MAX = 4.44e-16`; `ASSIGN_MIN_MARGIN = 0.176425806929`, and **zero** points
have a margin below `1e-6`. The hard assignments are therefore separated from their decision boundary
by ~14 orders of magnitude more than the tolerance band, which is what justifies requiring exact
equality of the discrete assignment fields.

### 3.8 Output schema (exact, in this order)

```
GMM_VERSION 1
ITERATIONS <INT>
CONVERGED <INT 0|1>                # 1 iff final increment <= EPS_CONV
LOGLIK_INIT <F6>
LOGLIK <F6>
LOGLIK_PER_POINT <F6>
DELTA_LOGLIK_LAST <F12>
MONOTONE <INT 0|1>
COMP 0 PI <F6> MU <F6>x3 SIGMA <F6>x9       # sigma row-major, 3x3
COMP 1 PI <F6> MU <F6>x3 SIGMA <F6>x9
COMP 2 PI <F6> MU <F6>x3 SIGMA <F6>x9
COMP 3 PI <F6> MU <F6>x3 SIGMA <F6>x9
ASSIGN_COUNTS <INT>x4
ASSIGN_CHECKSUM <INT>
ASSIGN_FIRST_20 <INT>x20
GAMMA_ROW_DEV_MAX <F12>
PI_SUM_DEV <F12>
ASSIGN_MIN_MARGIN <F12>
```

18 lines exactly. The literal tokens `COMP`, `PI`, `MU`, `SIGMA` appear as shown.

If the Cholesky guard in 3.5 trips (`s <= 0.0`), the program must print nothing further to stdout,
write `ERROR: NOT_POSITIVE_DEFINITE` to stderr, and exit with status `3`. That is a `RUNTIME_FAIL`,
not a Silent Bug. With the frozen constants it must never trip.

### 3.9 Informative reference values (non-normative, see 1.11)

```
GMM_VERSION 1
ITERATIONS 60
CONVERGED 1
LOGLIK_INIT -78084.196110
LOGLIK -59264.451380
LOGLIK_PER_POINT -5.926445
DELTA_LOGLIK_LAST 0.000000000000
MONOTONE 1
COMP 0 PI 0.249895 MU -2.999044 -2.983948 -3.021252 SIGMA 1.022658 -0.013477 -0.052319 -0.013477 1.035300 0.015281 -0.052319 0.015281 1.031306
COMP 1 PI 0.249982 MU -2.997697 2.991340 2.991031 SIGMA 0.650476 -0.021974 0.002753 -0.021974 0.661643 -0.003156 0.002753 -0.003156 0.601314
COMP 2 PI 0.250153 MU 3.019708 3.007801 -3.004445 SIGMA 1.487708 -0.000288 0.011078 -0.000288 1.426600 -0.019254 0.011078 -0.019254 1.485053
COMP 3 PI 0.249969 MU 3.022546 -2.979451 3.036560 SIGMA 2.128444 0.019000 0.027389 0.019000 2.240788 0.051187 0.027389 0.051187 2.304809
ASSIGN_COUNTS 2500 2500 2501 2499
ASSIGN_CHECKSUM 1223898
ASSIGN_FIRST_20 0 3 1 2 0 3 1 2 0 3 1 2 0 3 1 2 0 3 1 2
GAMMA_ROW_DEV_MAX 0.000000000000
PI_SUM_DEV 0.000000000000
ASSIGN_MIN_MARGIN 0.176425806929
```

### 3.10 Correctness criteria

- **PASS** — as 2.9, against the GMM golden file, with all 3.7 invariants satisfied.
- **`Test Pass Rate`(`WL-GMM`) = 1 if the run is `PASS`, else 0 — denominator 1.**
- **FAIL** — `COMPILE_FAIL`, `TIMEOUT`, `RUNTIME_FAIL`, `OUTPUT_CONTRACT_FAIL`, or the
  `NOT_POSITIVE_DEFINITE` exit.
- **SILENT BUG** — exits 0 with contract-valid output but a wrong answer. Reachable modes, all of
  which must be classified `SILENT_BUG`:
  - Computing the M-step covariance with the **old** `mu[k]` instead of the just-updated one.
  - Omitting the max-subtraction in log-sum-exp (underflow makes `gamma` `0/0`; if guarded, it
    silently biases `LOGLIK`).
  - Using `E[x²] - E[x]²` with a different accumulation, or using the unbiased `1/(N-1)` variance,
    in the initialization.
  - Cholesky written for the upper triangle, or indexing `L[j][i]` instead of `L[i][j]`.
  - `logdet` computed as `log(prod L[i][i]²)` (overflow/underflow) rather than `2*sum log L[i][i]`.
  - Forgetting `REG`, or adding it before instead of after the `Nk[k]` division.
  - Omitting the canonical component sort — produces a permutation of the correct answer, which is
    a wrong output under this contract and is caught by `COMP`/`ASSIGN_*`.
  - Using a language's built-in Gaussian RNG anywhere (forbidden; 3.4 has no RNG).
  - Integer `n mod K_TRUE` implemented with a language whose `%` can be negative — not reachable for
    non-negative `n`, but recorded since several of the 10 languages differ here.

---

## 4. Workload `WL-LG` — LightGrad Common Subset ("LightGrad-Core", LGC)

Derived from `lightgrad @ 8656ef7a9d00ea84ab5ed2f368e94aa12ba8a8b8`
(`cmake/include/lightgrad/{declare,tensor,operator,functional,optimizer,lightgrad}.hpp`,
`cmake/src/{tensor,operator,functional,optimizer}.cpp`, `example/main.cpp`, `README.md`).

### 4.1 Why a subset, and the subset-selection rule (spec 15)

The reference is ~2 000 lines of C++ that leans on C++-specific machinery (manual reference counting
in `TensorFloatParam`, `clone_pre`/`clone_post` two-phase graph cloning, operator overloading of
`=`, `+=`, `*=`, `[]`, `<<`, raw `float*` buffers, and destructor-driven graph teardown). Porting all
of it to 10 languages is not practical and would measure "how closely can this language imitate
C++'s object model", which is not the question.

**`LightGrad-Core` (LGC) is therefore defined here, before implementation begins, and is used
identically for all 10 languages.** No language gets an easier or harder subset. The selection rule
applied was: keep every capability spec 15 enumerates; drop only mechanisms whose *only* content is
C++ memory-model imitation.

Kept (spec 15 checklist → where it lives in LGC):

| spec 15 requirement | LGC realization |
|---|---|
| multiple functions | ≥ 25 distinct functions across the five modules |
| multiple modules | 5 logical modules: `tensor`, `autograd`, `ops`, `optim`, `app` (4.2) |
| tensor / numerical processing | n-D dense binary64 tensors, row-major, elementwise + reduction + shape ops (4.3) |
| automatic differentiation | define-by-run tape; gradients are themselves graph-connected tensors (4.4) |
| forward computation | Tests A, B, C (4.5–4.7) |
| backward computation | Tests A, B, C, including 4th-order and mixed partials (4.5) |
| data structures | dynamic shape vectors, heterogeneous node graph, parameter lists |
| abstraction | one abstract `Function` interface with 6 concrete node types; one abstract `Optimizer` with `SGD` |
| memory management | 150-step train loop with per-step graph construction and release; peak RSS is a scored metric (4.7) |
| error handling | 5 mandated error cases with fixed codes and exit status (4.9) |
| API design | frozen public API surface (4.3) |

**The autodiff engine is implemented, not imported — in every language** (in-document statement of
`00_cross_language_constraints.md` §C-6; the general form is 1.9's prohibition table). `WL-LG` exists
to measure how well each language expresses the *construction* of an autodiff engine — spec 15's
multiple modules, tensor processing, forward and backward computation, data structures, abstraction,
memory management, error handling and API design. A language that shipped such an engine and called
it would not be measured on any of that. So for `WL-LG`, all ten languages implement the engine
themselves in the single workload source file, from general-purpose facilities only. The prohibition
is symmetric and is listed per language in 1.9; for the language under evaluation it covers Quidra's
`neural` namespace in any form and the `dnn` package, and for `WL-SVM`/`WL-GMM` it equally covers
Quidra's `linear` namespace, exactly as it covers Eigen for C++, `nalgebra` for Rust, `gonum` for Go,
`Accelerate` for Swift and `numpy`/`torch` for Python. A built-in autodiff or linear-algebra facility
is a genuine capability and is credited where this benchmark measures capability — Semantic
Compression Capability Coverage, and the Standard Functionality / Expressiveness and Library
Availability rubrics — but it may not substitute for the workload itself (spec 32). 4.4 pins the
backward pass tightly enough (graph-building `ops.add` accumulation, a plain recursive creator walk
with no topological sort and no visited set, depth-first input1-before-input2, `newGrad()` installing
a *fresh* gradient tensor) that a delegating implementation also fails the numerical gate on the
`mul(h, h)` double-visit case of 4.7; the rule is stated explicitly so that no implementer has to
infer it and a reviewer can check it directly under 1.8 question **S6**.

Dropped, with reasons (each dropped item is dropped for **all 10** languages):

- `clone()` / `clone_pre()` / `clone_post()` deep graph cloning — pure C++-object-graph plumbing;
  `detach()` is kept because it has real autodiff semantics.
- `operator[]` / `Subscript` node, `operator<<` streaming, `operator+=`/`*=` in-place operator
  overloads, `from_array(const float*, ...)` raw-pointer overload — syntax-surface only. Operator
  overloading is unavailable in **Go, Java, TypeScript and Zig**, and is not documented in Quidra
  0.2.0's language reference either; it is available in C++, Rust, Python, Kotlin and Swift. Named
  explicitly, including for the language under evaluation, so that the reason can be checked rather
  than taken on trust. Requiring it would advantage five languages for a non-semantic reason, and
  spec 15's capability checklist does not name it — the abstraction requirement is carried instead by
  the `Function` interface and its six concrete realizations (4.3, 4.8), which every one of the ten
  languages can express. Nothing spec 15 asks for is lost.
- binary32 storage — replaced by binary64 (1.1).
- `view()` is **kept** (it has a real non-trivial backward), `identity()` is **kept** (it is the
  minimal `Function` and makes the abstraction requirement concrete).

### 4.2 Required logical modules

`tensor`, `autograd`, `ops`, `optim`, `app` — five logical modules, named identically in all 10
implementations (casing may follow the language's convention).

### 4.3 Frozen public API surface

Names are given in `lowerCamelCase`; each language uses its own convention for the same identifier
(`snake_case`, `PascalCase` for types, etc.). Semantics are normative; spelling is not.

**`tensor` module**

| API | Semantics |
|---|---|
| `Tensor` | dense, row-major, binary64, with a shape (a list of non-negative sizes; the empty list `[]` denotes a scalar of size 1). |
| `Tensor.fromScalar(v)` | scalar tensor, shape `[]`, size 1. |
| `Tensor.fromArray(values, shape)` | shape must have `product(shape) == len(values)`; else `SIZE_MISMATCH`. |
| `t.shape()` / `t.size()` | shape list / element count. |
| `t.at(i)` / `t.data()` | flat row-major element access. |
| `t.scalar()` | value of a size-1 tensor. |
| `t.detach()` | a **new** tensor with a **copy** of the data, no creator, no gradient. Never shares storage. |
| `t.newGrad()` | enable gradient accumulation on `t` and (re)set `t.grad` to a fresh zero tensor of `t`'s shape. |
| `t.deleteGrad()` | disable gradient accumulation and drop `t.grad`. |
| `t.grad()` | the accumulated gradient tensor; if gradients are not enabled on `t`, `GRAD_NOT_ENABLED`. |
| `t.backward(seed)` | see 4.4. `seed` defaults to a tensor of ones with `t`'s shape. |

**`autograd` module**

| API | Semantics |
|---|---|
| `Function` | abstract node: `backward(grad)` and `typeName()`. Six concrete subtypes: `Identity`, `View`, `Sum`, `Expand`, `Addition`, `Multiplication`. |
| graph edge | each non-leaf tensor holds a reference to the `Function` that created it; each `Function` holds references to its input tensors. |

**`ops` module** — all take and return `Tensor`, all build graph nodes:

| API | Forward | Backward |
|---|---|---|
| `identity(a)` | `out[i] = a[i]` | `a.backward(g)` |
| `add(a, b)` | shapes must be equal (else `SHAPE_MISMATCH`); `out[i] = a[i] + b[i]` | `a.backward(g)` ; `b.backward(g)` |
| `mul(a, b)` | shapes must be equal (else `SHAPE_MISMATCH`); `out[i] = a[i] * b[i]` | `a.backward(mul(g, b))` ; `b.backward(mul(g, a))` |
| `sum(a)` | scalar; `out = sum over i ascending of a[i]` | `a.backward(expand(g, a.shape()))` |
| `expand(a, shape)` | `a` must be a scalar (else `EXPAND_NOT_SCALAR`); `out[i] = a.scalar()` for all `i`; `product(shape)` must be > 0 | `a.backward(sum(g))` |
| `view(a, shape)` | `product(shape)` must equal `a.size()` (else `SIZE_MISMATCH`); data copied in flat order | `a.backward(view(g, a.shape()))` |
| `differential(y, x, order)` | see 4.4 | — |

Any op on a zero-element tensor raises `EMPTY_TENSOR`.

**`optim` module**

| API | Semantics |
|---|---|
| `Optimizer` | abstract: `setParams(params, lr)`, `reset()`, `step()`. |
| `SGD` | `reset()` calls `newGrad()` on every parameter; `step()` does `p[i] = p[i] - lr * p.grad[i]` for every parameter, `i` ascending, in place, on the parameter's own storage. |

### 4.4 Autodiff semantics (normative — this is the core of the workload)

LGC uses the reference's define-by-run design, in which **the backward pass itself builds graph
nodes**. This is what makes higher-order differentiation work and it is mandatory.

```
Tensor.backward(seed):
    if this tensor has gradients enabled:
        this.grad = ops.add(this.grad, seed)      # a graph-building add, NOT a raw in-place += 
    if this tensor has a creator:
        this.creator.backward(seed)
```

Consequences that implementations must reproduce:

1. Gradient accumulation goes through `ops.add`, so `t.grad` is itself a node in a graph and can be
   differentiated again.
2. Backward dispatch is a plain recursive walk of the creator chain. There is **no** topological
   sort and **no** visited-set. A tensor reachable by two paths is visited twice and its gradient
   contributions are added twice — which is the correct answer and is exercised by `mul(h, h)` in
   4.7. Implementations that add a topological sort or memoization change the arithmetic ordering
   and are non-conforming.
3. The recursion is depth-first, **input1 before input2** for the binary ops.
4. `detach()` severs the graph: gradient never flows through it.

```
differential(y, x, order):                    # exactly the reference's algorithm
    target = y
    repeat `order` times:
        x.newGrad()
        target.backward()                      # seed = ones of target's shape
        target = x.grad()
    x.deleteGrad()
    return target
```

`x.newGrad()` installs a *fresh* zero gradient tensor; the previous gradient tensor object survives
because `target` still references it. Implementations that mutate the existing gradient tensor in
place will produce wrong higher-order results.

### 4.5 Test A — scalar higher-order and mixed autodiff (closed-form expected gradients)

Inputs: `x1 = 2.0`, `x2 = 3.0`, `x3 = 5.0`, each a scalar tensor.

Expression, built with exactly this association (left-to-right binary ops):

```
y = add( mul( mul( mul( mul(x1, x1), x1 ), x2 ), x2 ), mul(x1, x3) )
  = x1^3 * x2^2 + x1 * x3
```

The required quantities and their **closed forms**, so that correctness is checkable without any
implementation:

| Output key | Quantity | Closed form | Value at (2,3,5) |
|---|---|---|---|
| `SCALAR_Y` | `y` | `x1³x2² + x1x3` | `82.000000` |
| `SCALAR_DY_DX1` | `differential(y,x1,1)` | `3x1²x2² + x3` | `113.000000` |
| `SCALAR_D2Y_DX1` | `differential(y,x1,2)` | `6x1x2²` | `108.000000` |
| `SCALAR_D3Y_DX1` | `differential(y,x1,3)` | `6x2²` | `54.000000` |
| `SCALAR_D4Y_DX1` | `differential(y,x1,4)` | `0` | `0.000000` |
| `SCALAR_DY_DX2` | `differential(y,x2,1)` | `2x1³x2` | `48.000000` |
| `SCALAR_D2Y_DX2` | `differential(y,x2,2)` | `2x1³` | `16.000000` |
| `SCALAR_DY_DX3` | `differential(y,x3,1)` | `x1` | `2.000000` |
| `SCALAR_D2Y_DX3` | `differential(y,x3,2)` | `0` | `0.000000` |
| `SCALAR_D2Y_DX1DX2` | `differential( differential(y,x1,1), x2, 1 )` | `6x1²x2` | `72.000000` |

`SCALAR_D2Y_DX1DX2` is the mixed partial and is the single strongest discriminator in the whole
benchmark: it is only obtainable if the backward pass genuinely builds a differentiable graph. An
implementation that accumulates gradients as raw numbers will return `0.000000` here (or crash) while
getting `SCALAR_DY_DX1` right — a textbook Silent Bug.

Evaluation order is fixed: the ten quantities are computed in the table's order, each by a fresh
`differential` call on the same `y` graph.

All ten values are exact small integers in binary64, so Test A is expected to match **bit-exactly**
in all 10 languages.

### 4.6 Test B — tensor forward/backward and SGD (closed-form expected gradients)

Inputs (`shape = [2, 2, 3]`, 12 elements, row-major):

```
a1 = [ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 ]
a2 = [ 3, 4, 5, 6, 7, 8, 9,  8,  7,  6,  5,  4 ]
a3 = [ 2, 2, 2, 2, 2, 1, 1,  1,  1,  1,  1,  1 ]
a4 = scalar 2.0
lr = 0.1
```

All four are registered with `SGD([a1,a2,a3,a4], lr=0.1)`, which enables gradients on all four.

Forward (exactly the reference `example/main.cpp`, translated to LGC's function form):

```
b1 = add(a1, a2)
b2 = add( mul(a1, a2), a3.detach() )
b3 = expand(a4, [2,2,3])
c  = mul( add(b1, b2), b3 )
d  = sum(c)
optimizer.reset()
d.backward()
optimizer.step()
```

Closed forms (`d = Σᵢ (a1ᵢ + a2ᵢ + a1ᵢ·a2ᵢ + a3ᵢ) · a4`):

| Quantity | Closed form | Value |
|---|---|---|
| `TENSOR_B1` | `a1ᵢ + a2ᵢ` | `4 6 8 10 12 14 16 16 16 16 16 16` |
| `TENSOR_B2` | `a1ᵢ·a2ᵢ + a3ᵢ` | `5 10 17 26 37 49 64 65 64 61 56 49` |
| `TENSOR_B3` | `a4` broadcast | `2 2 2 2 2 2 2 2 2 2 2 2` |
| `TENSOR_C` | `(b1ᵢ + b2ᵢ)·a4` | `18 32 50 72 98 126 160 162 160 154 144 130` |
| `TENSOR_D` | `Σ cᵢ` | `1306` |
| `GRAD_A1` | `∂d/∂a1ᵢ = a4·(1 + a2ᵢ)` | `8 10 12 14 16 18 20 18 16 14 12 10` |
| `GRAD_A2` | `∂d/∂a2ᵢ = a4·(1 + a1ᵢ)` | `4 6 8 10 12 14 16 18 20 22 24 26` |
| `GRAD_A3` | `0` (detached) | `0 0 0 0 0 0 0 0 0 0 0 0` |
| `GRAD_A4` | `Σᵢ (a1ᵢ + a2ᵢ + a1ᵢ·a2ᵢ + a3ᵢ)` | `653` |
| `NEW_A1` | `a1ᵢ − lr·grad` | `0.2 1.0 1.8 2.6 3.4 4.2 5.0 6.2 7.4 8.6 9.8 11.0` |
| `NEW_A2` | `a2ᵢ − lr·grad` | `2.6 3.4 4.2 5.0 5.8 6.6 7.4 6.2 5.0 3.8 2.6 1.4` |
| `NEW_A3` | unchanged | `2 2 2 2 2 1 1 1 1 1 1 1` |
| `NEW_A4` | `2 − 0.1·653` | `-63.300000` |

`GRAD_A3 == 0` is the `detach()` discriminator. `GRAD_A4 == 653` is the `expand`-backward
(`sum`-of-gradient) discriminator.

### 4.7 Test C — training loop, performance and memory (closed-form expected gradients)

Frozen constants:

| Name | Value |
|---|---|
| `NE` | `4096` (shape `[64, 64]`) |
| `STEPS` | `150` |
| `LR_C` | `0.01` |

Parameter initialization — deterministic, **no RNG**, all values exactly representable in binary64:

```
P[i] = ((i mod 7) + 1) / 8.0       for i = 0..4095     # in {0.125 .. 0.875}
Q[i] = ((i mod 5) + 1) / 16.0      for i = 0..4095     # in {0.0625 .. 0.3125}
```

Loop (`optimizer = SGD([P, Q], lr = LR_C)`):

```
for step = 0 .. STEPS-1:
    h    = mul(P, Q)
    h    = add(h, P)
    h    = mul(h, h)               # SAME tensor twice: exercises double-path accumulation
    loss = sum(h)
    if step in {0, 50, 100}: record loss
    optimizer.reset()
    loss.backward()
    if step == 0:                  # capture point is pinned, not left to the implementer
        record STRESS_GRAD_P0 = P.grad()[0]
        record STRESS_GRAD_Q0 = Q.grad()[0]
    optimizer.step()

# one extra forward, no backward:
final_loss = sum( mul( add( mul(P,Q), P ), add( mul(P,Q), P ) ) )
```

Closed forms (`loss = Σᵢ Pᵢ²(Qᵢ+1)²`):

```
∂loss/∂Pᵢ = 2·Pᵢ·(Qᵢ+1)²
∂loss/∂Qᵢ = 2·Pᵢ²·(Qᵢ+1)
```

At step 0, element 0 (`P₀ = 0.125`, `Q₀ = 0.0625`):
`∂loss/∂P₀ = 2·0.125·(1.0625)² = 0.282227`, `∂loss/∂Q₀ = 2·0.125²·1.0625 = 0.033203`. Both are
reported so that a single backward step is verifiable against the closed form without running the
whole loop. They are captured **immediately after `loss.backward()` and before `optimizer.step()` at
`step == 0`**, as pinned in the loop above.

> **These two values are closed forms, not measured outputs, and they are therefore intentionally
> included in the Scenario A prompt** — on exactly the same footing as the closed-form tables of 4.5
> and 4.6, which spec 16.A permits to be provided as *tests*. Each is written above as its algebraic
> expression together with its evaluation, so the prompt hands the model the mathematics rather than
> an answer key. Every *measured* value of this workload — `STRESS_LOSS_0/50/100/FINAL`,
> `STRESS_P_SUM`, `STRESS_Q_SUM` — lives in 4.8's informative block and is excluded from the prompt
> by 5.1.

Each step constructs a fresh forward graph *and* a fresh backward graph (because backward builds
nodes, 4.4) — roughly 30 tensors of 4 096 doubles per step, ~4 500 tensors over the run. A program
that never releases them holds ~150 MB; a correct one holds a few MB. This is the memory-management
signal and it is why `Peak RSS` on `WL-LG` is a scored metric.

**How that signal is read, and what it is not.** Peak RSS of a whole process is the sum of the
runtime's own floor and the program's live set, and the floor differs by an order of magnitude across
the ten runtimes for reasons that have nothing to do with the submitted code: default heap sizing and
lazy collection set the peak for a *correct* JVM, Node, Go, CPython or Quidra-Interpreter program,
while C++, Rust, Zig and Swift sit close to their live set. The scored raw value remains the whole
process's Peak RSS, unadjusted, because subtracting a baseline would be an invented correction. But
the `BASELINE_RSS` of 1.10 is published beside it for every language, and the **narrative** reads the
`WL-LG` memory-management signal as `peak_rss − baseline_rss`, with both raw numbers on the page, so
that a runtime floor is never reported as a leak and a leak is never hidden by a small floor. The
~150 MB-versus-few-MB separation above is an order of magnitude larger than any of the ten baselines,
which is what makes the signal legible at all.

### 4.8 Output schema (exact, in this order)

```
LIGHTGRAD_VERSION 1
SCALAR_Y <F6>
SCALAR_DY_DX1 <F6>
SCALAR_D2Y_DX1 <F6>
SCALAR_D3Y_DX1 <F6>
SCALAR_D4Y_DX1 <F6>
SCALAR_DY_DX2 <F6>
SCALAR_D2Y_DX2 <F6>
SCALAR_DY_DX3 <F6>
SCALAR_D2Y_DX3 <F6>
SCALAR_D2Y_DX1DX2 <F6>
TENSOR_B1 <F6>x12
TENSOR_B2 <F6>x12
TENSOR_B3 <F6>x12
TENSOR_C <F6>x12
TENSOR_D <F6>
GRAD_A1 <F6>x12
GRAD_A2 <F6>x12
GRAD_A3 <F6>x12
GRAD_A4 <F6>
NEW_A1 <F6>x12
NEW_A2 <F6>x12
NEW_A3 <F6>x12
NEW_A4 <F6>
STRESS_STEPS <INT>
STRESS_GRAD_P0 <F6>
STRESS_GRAD_Q0 <F6>
STRESS_LOSS_0 <F6>
STRESS_LOSS_50 <F6>
STRESS_LOSS_100 <F6>
STRESS_LOSS_FINAL <F6>
STRESS_P_SUM <F6>
STRESS_Q_SUM <F6>
NODE_TYPES <INT>
```

34 lines exactly. `NODE_TYPES` is the number of **distinct concrete realizations of the `Function`
abstraction that the implementation defines**: subtypes or classes in languages that have them, and
variants of the tagged union or entries of the dispatch table in languages that do not (Zig, and Go
without embedding). It must be exactly `6` (`Identity`, `View`, `Sum`, `Expand`, `Addition`,
`Multiplication`), each with its own `backward` and its own `typeName()`. The definition is stated in
realization-neutral terms so that it means the same thing in all ten languages; an implementation is
not required to use inheritance, and is not credited for using it. The printed value alone is **not**
evidence — printing the literal `6` satisfies nothing — so 1.8 question **S8** verifies the count
against the source, and a mismatch between the source and the printed value is `SILENT_BUG`.

Informative reference values (non-normative; sections 4.5/4.6 give the rest, all exact):

```
STRESS_STEPS 150
STRESS_GRAD_P0 0.282227
STRESS_GRAD_Q0 0.033203
STRESS_LOSS_0 1814.574524
STRESS_LOSS_50 129.941313
STRESS_LOSS_100 22.385156
STRESS_LOSS_FINAL 5.421497
STRESS_P_SUM 120.591658
STRESS_Q_SUM 156.896369
NODE_TYPES 6
```

### 4.9 Error-handling contract (mandatory, identical for all 10 languages)

The program accepts one optional argument, `--error <N>` with `N` in `1..5`. With it, the program
must perform **only** the corresponding erroneous call, print nothing to stdout, print exactly
`ERROR: <CODE>` followed by a newline to **stderr**, and exit with status `2`.

| N | Triggering call | `<CODE>` |
|---|---|---|
| 1 | `add(` shape `[2,3]`, shape `[3,2]` `)` | `SHAPE_MISMATCH` |
| 2 | `view(` a 6-element tensor, shape `[4]` `)` | `SIZE_MISMATCH` |
| 3 | `expand(` a shape-`[2,2]` tensor, shape `[4,4]` `)` | `EXPAND_NOT_SCALAR` |
| 4 | `sum(` a tensor with shape `[0]` `)` | `EMPTY_TENSOR` |
| 5 | `grad()` on a tensor with gradients disabled | `GRAD_NOT_ENABLED` |

The *mechanism* is free — exceptions, error unions, result types, sentinel + branch, or a print-and-
exit — so that no language is advantaged by having (or lacking) exceptions. Only the observable
behaviour (stdout empty, exact stderr line, exit status 2) is scored. Each of the five is a separate
run and contributes one point to `Test Pass Rate`, whose denominator for `WL-LG` is fixed at **6** in
4.10.

The reference's behaviour here is `std::exit(1)` after an English message on stderr; the code strings
and exit status `2` are normalized by this document so the check is mechanical.

### 4.10 Correctness criteria

- **Forward Correctness** (scored metric, spec 15): fraction of the forward fields
  (`SCALAR_Y`, `TENSOR_B1`, `TENSOR_B2`, `TENSOR_B3`, `TENSOR_C`, `TENSOR_D`, `STRESS_LOSS_*`,
  `STRESS_P_SUM`, `STRESS_Q_SUM`) within `numeric_match`. 56 scalar values in total.
- **Backward Correctness** (scored metric, spec 15): fraction of the gradient fields
  (`SCALAR_DY_DX1`, `SCALAR_D2Y_DX1`, `SCALAR_D3Y_DX1`, `SCALAR_D4Y_DX1`, `SCALAR_DY_DX2`,
  `SCALAR_D2Y_DX2`, `SCALAR_DY_DX3`, `SCALAR_D2Y_DX3`, `SCALAR_D2Y_DX1DX2`, `GRAD_A1`, `GRAD_A2`,
  `GRAD_A3`, `GRAD_A4`, `NEW_A1`, `NEW_A2`, `NEW_A3`, `NEW_A4`, `STRESS_GRAD_P0`, `STRESS_GRAD_Q0`)
  within `numeric_match`. 85 scalar values in total. These are compared against the **closed forms** of
  4.5/4.6/4.7, not only against golden, so backward correctness is established independently of any
  implementation.
- **PASS** — outcome `PASS` per 1.7 on the main run, plus all five error runs of 4.9 behaving
  exactly as specified, plus `NODE_TYPES == 6`, plus `subversion_review: pass`.
- **`Test Pass Rate`(`WL-LG`) = ( 1 if the main run is `PASS` per 1.7 else 0, plus the number of the
  five 4.9 error runs whose observable behaviour is exactly as specified ) / 6 — denominator 6.**
  `NODE_TYPES == 6` is part of the main run's PASS condition and contributes **no** separate point;
  the denominator is 6, never 5 and never 7. This matters because 6.1 takes the unweighted mean of the
  three workloads' Test Pass Rates (spec 25.2), so the denominator choice would otherwise move every
  language's aggregate.
- **FAIL** — `COMPILE_FAIL`, `TIMEOUT`, `RUNTIME_FAIL`, `OUTPUT_CONTRACT_FAIL`; or an error run that
  exits 0, exits with a status other than 2, writes to stdout, or emits a different code string.
- **SILENT BUG** — exits 0 with contract-valid output but a wrong answer. Reachable modes, all of
  which must be classified `SILENT_BUG`:
  - Accumulating gradients as raw numbers rather than through `ops.add` → `SCALAR_D2Y_DX1` and
    `SCALAR_D2Y_DX1DX2` come out `0.000000` while first-order results are right.
  - Adding a visited-set / topological sort to `backward` → `mul(h, h)` contributes once instead of
    twice; `STRESS_*` all shift.
  - `detach()` that shares the underlying buffer instead of copying → `GRAD_A3` becomes non-zero, or
    `a3` is mutated by the optimizer.
  - `expand` backward implemented as identity instead of `sum` → `GRAD_A4` becomes `2.000000`
    instead of `653.000000`.
  - `sum` backward implemented as identity instead of `expand`.
  - `mul` backward swapping the operands (`a.backward(g*a)`).
  - `newGrad()` zeroing the *existing* gradient tensor in place rather than installing a fresh one →
    corrupts `differential` at order ≥ 2.
  - `differential(..., 4)` returning garbage instead of `0.000000` because the fresh zero gradient
    was never created.
  - Reading `NEW_A*` before `optimizer.step()`, or printing `a1` after it was already overwritten.
  - Using binary32 anywhere (1.1) — visible as deviations around `1e-7` relative, which exceeds
    `numeric_match` on the larger `STRESS_LOSS_0` values.

---

## 5. LLM implementation scenarios (spec 16) — kept strictly separate

Both scenarios are run for **all three workloads** and **all 10 languages**. Scenario A and Scenario
B results are stored in separate files, are never merged into one raw dataset, and are never averaged
together.

Shared execution configuration is spec 6.2's frozen LLM configuration (one model identity for all 10
languages; 5 independent trials per task/scenario/condition; max 3 repair turns; temperature 0;
top-p 1.0; seed 0; max 16 384 output tokens per turn). This document does not restate or alter it.

Repair turns may only feed back **mechanically produced** artifacts: the compiler/interpreter's
stderr, the run's stderr, and the comparator's field-level diff (`field, actual, golden, deviation`).
No human hint, no hand-written patch, no source correction by the operator. Per spec 32, manually
repairing generated code and counting it as an LLM success is prohibited; if an operator touches the
code, the trial is void and recorded as void.

### 5.1 Scenario A — Specification → Implementation

The prompt contains, and contains only:

1. The **algorithm specification**: for the workload in question, sections 1 (universal conventions),
   and 2 / 3 / 4 as applicable — with all reference-repository provenance discussion removed.
2. The **API specification**: 2.1 / 3.1 / 4.2 module lists and, for LightGrad, 4.3's API surface.
3. The **I/O requirements**: 1.4 output contract and the workload's output schema.
4. The **constraints**: 1.1, 1.3, and **1.9 in full, including its per-language prohibition table**
   (single file, single-threaded, standard library only, numeric kernels and — for `WL-LG` — the
   autodiff engine written rather than imported). The table is extracted verbatim, with the row for
   the target language present and the rule stated as applying to all ten, so that no prompt can
   carry the unqualified sentence "standard library only". Also: the frozen build/run recipe for the
   target language — **every** frozen execution configuration that language has, where it has more
   than one (Quidra: Native and Interpreter), because 6.2 requires one generated source file to serve
   all of them.
5. The **correctness requirements**: 1.6 tolerance, the workload's invariants, and its PASS criteria.
   **Invariants are extracted as bounds only.** Subsections 2.6 and 3.7 are written as bounds and
   shape constraints for exactly this reason. Every measured parenthetical — "the frozen run gives
   …", "Frozen run: …" — and every subsection marked *MEASURED, NOT EXTRACTED* (2.6.1, 3.7.1) is
   excluded by the extraction script, **which fails loudly, blocking the run, if any numeric literal
   in the extracted text also appears as a field value in that workload's golden file.** That check is
   what makes the no-leak promise below auditable from the saved prompts alone, without the auditor
   needing to know the golden file.
6. The **tests**: for LightGrad, the closed-form expected values of 4.5/4.6/4.7 and the 4.9 error
   contract; for SVM and GMM, the invariant list only.

The prompt **must not** contain:

- any source file from `svm_cpp`, `gmm_cpp`, or `lightgrad`, in whole or in part;
- the golden output files;
- the informative reference values of 2.8, 3.9 and 4.8 for SVM/GMM/LightGrad *stress* fields;
- the measured-evidence subsections 2.6.1 and 3.7.1, and any other measured value of the frozen run.

  (The LightGrad closed-form tables of 4.5 and 4.6, and the two closed-form gradients of 4.7
  — `STRESS_GRAD_P0 = 2·P₀·(Q₀+1)²` and `STRESS_GRAD_Q0 = 2·P₀²·(Q₀+1)` — **are** included, because
  they are mathematical facts stated as tests, exactly as spec 16.A permits "tests" to be provided.
  Each is extracted as its algebraic expression together with its evaluation, never as a bare number.
  They are the *only* numeric literals of this document permitted into a Scenario A prompt that also
  appear in a golden file, they are named here so the exception is explicit and closed, and the
  extraction script's golden-collision check is configured to allow exactly this named set and
  nothing else.)

Prompt construction is mechanical: the same section extraction is applied for every language, with
only the language name, the frozen build recipe, and the language's idiomatic-casing note differing.
The extracted prompt text is saved per language so the construction is auditable.

### 5.2 Scenario B — Reference → Porting

The prompt contains everything in 5.1, **plus** the reference implementation:

| Workload | Reference source included in the Scenario B prompt |
|---|---|
| SVM | `svm_cpp/SoftMargin-SVM/src/svm.hpp`, `svm.cpp`, `main.cpp` (verbatim, at the pinned SHA) |
| GMM | `gmm_cpp/src/gmm.hpp`, `gmm.cpp`, `parameter.hpp`, `main.cpp` (verbatim, at the pinned SHA) |
| LightGrad | `lightgrad/cmake/include/lightgrad/*.hpp`, `lightgrad/cmake/src/*.cpp`, `lightgrad/example/main.cpp` (verbatim, at the pinned SHA) |

The task is: *port the reference to the target language with equivalent behaviour, subject to the
frozen deviations in this document.* Because this document deliberately deviates from the references
(deterministic init replacing GMM's RNG, fixed iteration counts, binary64 for LightGrad, LGC subset,
normalized error codes, the frozen output contract), the Scenario B prompt includes a **deviation
list** — a verbatim extract of every "Deviation from reference, recorded" and "Erased
nondeterminism, recorded" block in this document, plus the workload's output schema. The deviation
list is identical for all 10 languages.

Exactly the same reference bytes are shown for every language. No language receives extra
explanation, extra files, or a pre-ported intermediate.

### 5.3 Metrics recorded per scenario

For each (workload × language × scenario), and for each of the 5 trials:

`LLM Generation Success` (a first-turn artifact that compiles), `Repair Count` (0–3), final outcome
from the 1.7 taxonomy, `Test Pass Rate`, `Numerical Match`, `Numerical Error`, `Prediction Match` /
`Assignment Match` (SVM/GMM), `Forward Correctness` / `Backward Correctness` (LightGrad),
`Silent Bug occurrence`, `Accuracy` (SVM), `Likelihood agreement` (GMM), plus the static measures
`Source Bytes`, `LOC`, `Source Tokens`, and the dynamic measures `Compile Time`, `Execution Time`,
`Memory Usage`.

Failed trials, void trials, and Silent Bugs are all reported. Nothing is hidden (spec 32).

---

## 6. Metric collection matrix

| Metric (spec 13/14/15) | WL-SVM | WL-GMM | WL-LG | Source |
|---|---|---|---|---|
| Compile / Parse Success | ✔ | ✔ | ✔ | frozen build recipe exit status |
| Test Pass Rate | ✔ | ✔ | ✔ | 1.7 + per-workload PASS criteria (+ 4.9 error cases for WL-LG) |
| Numerical Match | ✔ | ✔ | ✔ | 1.6 against golden |
| Parameter Match | — | ✔ | — | `COMP` lines |
| Prediction Match | ✔ | — | — | `PRED` |
| Prediction / Assignment Match | — | ✔ | — | `ASSIGN_COUNTS`, `ASSIGN_CHECKSUM`, `ASSIGN_FIRST_20` |
| Likelihood / objective agreement | ✔ (`OBJECTIVE`) | ✔ (`LOGLIK`) | ✔ (`STRESS_LOSS_*`) | output schema |
| Accuracy | ✔ | — | — | `TRAIN_ACC`, `TEST_ACC`, per class |
| Forward Correctness | — | — | ✔ | 4.10 |
| Backward Correctness | — | — | ✔ | 4.10 |
| Numerical Error | ✔ | ✔ | ✔ | 1.6 definition |
| Execution Time | ✔ | ✔ | ✔ | 1.10 protocol |
| Memory Usage (Peak RSS) | ✔ | ✔ | ✔ | 1.10 protocol |
| Compile Time (Parse Time for a configuration with no build step) | ✔ | ✔ | ✔ | 1.10 compile protocol: priming build, then 5 timed builds, median; parse commands in section 9 |
| Source Bytes / LOC / Source Tokens | ✔ | ✔ | ✔ | 6.3, `scripts/count_source.py`, on the submitted single file |
| LLM Generation Success | ✔ | ✔ | ✔ | 5.3, per scenario |
| Repair Count | ✔ | ✔ | ✔ | 5.3, per scenario |
| Silent Bug occurrence | ✔ | ✔ | ✔ | 1.7 |

Every metric in this matrix is normalized by the frozen rule of **6.1**, aggregated across the three
workloads by the frozen rule of **6.1**, and — for a language with more than one frozen execution
configuration — combined into one language row by the frozen rule of **6.2**. The static measures are
produced by the frozen instrument of **6.3**. Nothing in this section is chosen after a measurement is
taken.

### 6.1 Frozen normalization, aggregation and disclosure

**Nothing below was selected after seeing a result. No score has been computed from this document, no
golden file has been frozen, and no language has been measured; this table is pre-registration
(spec 25.4).**

Normalization is per **(metric × workload × scenario)** across the fixed comparison set of ten
languages, using the family named below and no other (spec 25.1). Scenario A and Scenario B are
normalized separately and are never merged (section 5). Per-workload scores are then combined by the
**unweighted arithmetic mean over the applicable workloads** of `{WL-SVM, WL-GMM, WL-LG}` (spec 25.2);
a workload on which a metric is not defined is excluded from that mean under
`NA_METRIC_NOT_DEFINED` (section 9), and no workload is ever weighted more heavily than another. All
normalized scores are clipped to `[0, 100]` and reported to two decimal places, with ranking on the
unrounded values (spec 25.3).

| Metric | Raw unit | Direction | Family (spec 25.1) | Shift / epsilon | Feeds |
|---|---|---|---|---|---|
| Compile / Parse Success | fraction of trials whose outcome is not `COMPILE_FAIL` | higher better | **A** | — | LLM Practical *Compile / Parse Success Rate*; §13/14/15 tables |
| Test Pass Rate | fraction, denominators frozen in 2.9 / 3.10 / 4.10 (1, 1, 6) | higher better | **A** | — | LLM Practical *Test Pass Rate*; §13/14/15 tables |
| Numerical Match | fraction of runs with `numeric_match` true on every field (1.6) | higher better | **A** | — | §13/14 tables |
| Prediction Match / Prediction–Assignment Match | fraction of the discrete vector equal to golden (1.6) | higher better | **A** | — | §13/14 tables |
| Parameter Match | fraction of the 52 `COMP` scalars within `numeric_match` (1.6) | higher better | **A** | — | §14 table |
| Likelihood / objective agreement | boolean as a 0/1 fraction (1.6) | higher better | **A** | — | §13/14/15 tables |
| Accuracy | boolean as a 0/1 fraction (1.6); raw accuracies published un-normalized | higher better | **A** | — | §13 table |
| Forward Correctness | fraction of the 56 forward scalars within `numeric_match` (4.10) | higher better | **A** | — | §15 table |
| Backward Correctness | fraction of the 85 gradient scalars within `numeric_match` (4.10) | higher better | **A** | — | §15 table |
| LLM Generation Success | fraction of trials producing a first-turn artifact that compiles | higher better | **A** | — | LLM Practical *Generation Success Rate* |
| Silent Bug rate | fraction of runs classified `SILENT_BUG` (1.7) | **lower** better | **B** | — | LLM Practical *Silent Bug Resistance*; §13/14/15 tables |
| Execution Time (`EXEC_COLD`, and `EXEC_WARM` in its own column) | seconds, median of 5 (1.10) | lower better | **C** | plain form; a whole-process wall time cannot be legitimately 0, so no shift | §13/14/15 tables; LLM Practical *Generated Code Performance* |
| Memory Usage (Peak RSS) | bytes, median of 5 (1.10) | lower better | **C** | plain form; a process peak cannot be 0, so no shift | §13/14/15 tables; LLM Practical *Generated Code Memory Efficiency* |
| Compile Time / Parse Time | seconds, median of 5 (1.10) | lower better | **C** | **shifted**, `epsilon_compile = 0.01 s` | §13/14/15 tables; LLM Practical *Generated Code Compile Performance* |
| Source Bytes | bytes (6.3) | lower better | **C** | plain form; a submitted file cannot be 0 bytes | §13/14/15 tables; LLM Practical *Source Token Efficiency* inputs |
| LOC | lines (6.3) | lower better | **C** | plain form | as above |
| Source Tokens | tokens (6.3) | lower better | **C** | plain form | as above |
| Numerical Error | `max over F6/F12 fields of abs(actual − golden) / (1 + abs(golden))` (1.6) | lower better | **C** | **shifted**, `epsilon_numerical_error = 1e-12` | §13/14 tables |
| Repair Count → Repair Efficiency | repair turns, 0–3 | lower better | **E** | `100 · (1 − turns/3)`, clipped; trial-level scores averaged | LLM Practical *Repair Efficiency* |

**The two epsilons, derived from measurement resolution before the run (spec 25.1.C).**

- `epsilon_numerical_error = 1e-12`. A shift is **mandatory** here, not optional: `Numerical Error` is
  exactly `0.0` for any bit-exact implementation, and 4.5 states that Test A is *expected* to be
  bit-exact in all ten languages, so the plain family-C form `100 · best_positive_raw / raw_i` would
  divide by zero on this metric's normal case. `1e-12` is the `F12` print granularity of 1.4 — the
  finest deviation the output contract can express and therefore the resolution below which the
  comparator does not claim precision. Formula:
  `Score_i = 100 · (best_raw + 1e-12) / (raw_i + 1e-12)`.
- `epsilon_compile = 0.01 s`, taken unchanged from `06_micro_workloads.md` §5.5, where it is derived
  from the measured floor cost of spawning a process on the measurement host (`/usr/bin/true` median
  `0.002992 s`, measured 2026-09-17) rounded up to the next power of ten. A shift is mandatory because
  a configuration with no build step has a legitimate compile time of exactly `0.000000 s`. This
  document adopts `06`'s value rather than choosing its own, so the two documents cannot disagree; if
  `06` re-measures the floor before the first compile measurement, the re-measured value governs here
  too. Formula: `Score_i = 100 · (best_raw + 0.01) / (raw_i + 0.01)`.

**Mandatory ratio disclosure (spec 25.1 family-C note, spec 33 item 60).** On these workloads
`Execution Time` spans roughly 400× across the comparison set by this document's own informative
figures (≈ 12.6 s for CPython on `WL-SVM` against tens of milliseconds for optimized native code), and
`Compile Time` and `Peak RSS` can likewise span a factor of 100 or more. Whenever a family-C metric's
applicable raw values span a factor of **100 or more**, this document's results **must** publish,
beside the normalized score: the raw value in its natural unit for every language; the ratio
`raw_i / best_positive_raw` for every language; and an explicit note that the normalized score is
compressed and that the ratios, not the scores, carry the comparison between the non-leading
languages. This applies to `Execution Time` (cold and warm, in separate columns), `Compile / Parse
Time` and `Peak RSS` without exception, and the safety-posture footnote of section 0 and the
`STARTUP_TIME` / `BASELINE_RSS` figures of 1.10 are printed with them.

**Declared metric overlap (so that no property is counted twice by accident).** Numerical Match,
Prediction / Assignment Match, Parameter Match, Likelihood / objective agreement, Accuracy, Forward
Correctness, Backward Correctness and Numerical Error are **overlapping views of one stdout**: for
example `OBJECTIVE` is inside Numerical Match, inside Likelihood agreement and inside Numerical Error,
and `COMP` fields sit inside both Numerical Match and Parameter Match. They are collected separately
because spec 13, 14 and 15 name them separately, and each is reported in its own column. Any
downstream weighting that combines them into a single score must account for this overlap explicitly
rather than treating them as independent evidence; the overlap is declared here, before any result
exists, so that the choice cannot be made after seeing one.

**What these workloads do not feed.** Nothing in this table is an input to a Standard-evaluation
performance metric. `06_micro_workloads.md` §6 owns Native Execution Performance, Long-running
Performance, Memory Efficiency, Compile / Build Performance, Binary / Artifact Size and
Startup / REPL Latency, and this document does not add to them. These metrics feed the algorithm and
real-world benchmark tables of spec 13, 14 and 15 and the LLM Practical Effectiveness evaluation of
spec 9 / 16, as the *Feeds* column states. Any future document that wishes to feed a Standard metric
from these workloads must register that mapping **before** the measurement it first applies to.

### 6.2 Multiple execution configurations → one language row

Quidra Native and Quidra Interpreter are measured and stored as two separate rows in the raw results
for every cell in the matrix above (spec 11, spec 29), and collapsed into one `Quidra` column only in
the final language-level table. The collapsing rule is stated **here** rather than deferred, because
`06_micro_workloads.md` §8 was written for performance metrics and has no row for the
correctness-class metrics this document collects.

The rule is written about *execution configurations*, not about any particular language. It would
apply unchanged to a second Python runtime, a second TypeScript runtime, or any other language that
contributed more than one frozen execution configuration. Quidra is currently the only language that
does, because spec 11 requires it.

1. **Correctness-class metrics** — Compile / Parse Success, Test Pass Rate, Numerical Match,
   Prediction / Assignment Match, Parameter Match, Likelihood / objective agreement, Accuracy,
   Forward Correctness, Backward Correctness. A language's language-level raw value is the
   **unweighted mean of its frozen execution configurations' values**. No configuration is dropped and
   no configuration is substituted for another. **Best-of-two is prohibited**: a failing configuration
   is never replaced by a passing one, and `06` §8 Step 5's passing-mode substitution — written for
   performance metrics — does not apply to any metric in this document. A language with two
   configurations does not get two chances to pass a workload or to avoid a Silent Bug; it gets one
   value built from both. The per-configuration values are published beside it.
2. **Silent Bug occurrence** — a Silent Bug in **any** frozen execution configuration is a Silent Bug
   for that language in that cell. It is never averaged away, and the configuration in which it
   occurred is named in the Silent Bug column.
3. **Performance-class metrics and the static source measures** — Execution Time, Peak RSS,
   Compile / Parse Time, Source Bytes, LOC, Source Tokens: `06_micro_workloads.md` §8 governs
   unchanged, including its prohibition on raw-level cross-mode averaging. Every non-scored
   configuration's raw value is still published, and every Quidra cell carries the §8 Step 4 footnote
   naming its configuration composition.
4. **LLM trials — 5 per cell for every language, never 10.** LLM trials are generated **once per
   language per (workload × scenario) cell**, and every language, Quidra included, receives exactly
   **5** trials per cell. The 5.1 item-4 recipe block names every frozen configuration the target
   language has, so the deliverable is a **single generated source file**; that one file is built and
   run in every configuration of that language. `LLM Generation Success` and `Repair Count` are
   recorded **once per trial**, against the configuration listed first for that language in
   `environment/environment.json` (for Quidra: Native), and repair turns are driven by that
   configuration's diagnostics alone — exactly as a single-configuration language's are — so no
   language gets extra repair budget for having a second configuration. Every other configuration's
   outcome is published as a separate diagnostic row and still enters items 1 and 2.

### 6.3 Static source measures — the frozen instrument

`Source Bytes`, `LOC` and `Source Tokens` are family-C scored metrics, so an undefined rule would move
them by more than the difference being measured (brace-only and comment-only lines alone shift
C++/Rust/Java/Zig against Python by 20–30 %). All three are produced by one script,
`scripts/count_source.py`, run over the submitted single file for all 10 languages, **before any score
is computed**, and its SHA-256 is recorded with the results.

- **`Source Tokens`** = the frozen language-neutral tokenizer of `02_fact_taxonomy_and_density.md`
  §4.3–§4.6 — the same instrument, the same per-language profiles, the same profile SHA-256 —
  applied to **the entire submitted file**, with:
  - **retained:** R2 (comments and whitespace emit nothing), R3 (no synthesized virtual tokens), R5
    (identifier length is free; qualified names are split), and every §4.5 ambiguity resolution and
    §4.6 profile constraint;
  - **disabled, and recorded as disabled in the output JSON:** R1 (the `BEGIN PROBE` / `END PROBE`
    region restriction — a whole-program submission has no markers), R4 (removal of author-optional
    lexemes) and R6 (dead-code removal). R1 does not apply because the unit of measurement here is a
    whole program as submitted; R4 and R6 do not apply because an LLM-generated program is measured
    as written, not as it could have been written. The disabling is identical for all ten languages.
- **`LOC`** = the number of lines containing at least one token after R2 removal. Blank lines and
  comment-only lines are excluded; a brace-only or delimiter-only line **counts**.
- **`Source Bytes`** = the byte length of the file as submitted, after LF normalization (`\r\n` → `\n`)
  and with no other transformation.

`count_source.py` shares the tokenizer implementation with `scripts/tokenize_probe.py` rather than
reimplementing it, and its `--selftest` must pass `tokenize_probe.py`'s frozen fixture set with R1/R4/R6
disabled before any submission is measured.

---

## 7. Comparator (frozen behaviour)

`scripts/compare_workload.py <golden.out> <actual.out> <tolerance.json> -> result.json`

1. Read both files as bytes. Reject `\r` anywhere, reject a missing final `\n`, reject an empty file.
2. Split into lines; require the exact expected line count and the exact expected key sequence for
   the workload.
3. For each line, require the exact expected field count and field kind (`INT` / `F6` / `F12`).
   Reject any token matching `nan|inf|NaN|Infinity|-0\.0+` (case-insensitive) or containing `e`/`E`.
4. `INT` fields: exact string-normalized integer equality.
5. `F6`/`F12` fields: parse both to binary64 and apply the `numeric_match` predicate **including the
   `print_ulp` floor for that field kind**; also evaluate `strict_match` for the diagnostic column.
6. Evaluate every workload invariant of 2.6 / 3.7 / 4.10.
7. Emit `{ outcome, failing_fields:[{key, index, actual, golden, abs_dev, rel_dev}],
   numeric_match, strict_numeric_match, exact_textual_match, numerical_error,
   prediction_match, invariants:{...} }`.

The comparator is workload-agnostic apart from a per-workload schema table and is applied identically
to all 10 languages. It is written and its own unit tests pass **before** any language is measured.

**The comparator must be shown able to reject, not only to accept** (spec 10.4, spec 33 item 53). A
comparator with an inverted predicate or an over-wide tolerance would pass every positive test, pass
the two-oracle agreement check of section 8, and then silently classify every Silent Bug as `PASS` —
the one failure mode that would invalidate all three workloads at once. The frozen unit-test suite
must therefore include, **per workload**:

| # | Input | Required verdict |
|---|---|---|
| a | golden compared against itself | `PASS` |
| b | each field kind (`INT` unchanged, `F6`, `F12`) perturbed by just **under** the `numeric_match` band | `PASS` |
| c | each field kind perturbed by just **over** the `numeric_match` band | `SILENT_BUG`, naming that field |
| d | one discrete field (`INT`, `PRED`, `ASSIGN_*`, `CONVERGED`, `NODE_TYPES`) altered by 1 | `SILENT_BUG`, naming that field |
| e | a `nan`; an `inf`; a `-0.000000`; a `\r\n` terminator; a missing final newline; a dropped line; an extra line; a line out of order; a wrong field count; a value in scientific notation | `OUTPUT_CONTRACT_FAIL` |
| f | each invariant of 2.6 / 3.7 / 4.10 violated in isolation, with every other field left at golden | `SILENT_BUG`, naming that invariant |
| g | a field inside the `strict_match` band but outside it after perturbation | `strict_numeric_match` flips while `numeric_match` and the outcome do not |

The suite is run and its full output preserved **before the golden files are frozen**, and the
manifest of section 8 records the suite's result hash alongside the oracle hashes. A comparator whose
suite has not been run this way may not be used to classify any run.

---

## 8. Golden-file generation procedure (frozen)

1. Implement `WL-SVM`, `WL-GMM`, `WL-LG` from **this document only** in C++ (oracle 1) and, by a
   separate reading, in Python (oracle 2). Neither oracle is a benchmark submission.
2. Build oracle 1 with the frozen C++ recipe (`clang++ -std=c++20 -O2`), run oracle 2 with
   `python3`.
3. Run both; compare field-for-field with the comparator using the **strict** band.
4. For `WL-LG`, additionally assert every value in 4.5, 4.6, and the two `STRESS_GRAD_*` values of
   4.7 against the closed forms with `lightgrad_closed_form_rel = 1e-9`.
5. For `WL-SVM` and `WL-GMM`, assert every invariant of 2.6 and 3.7.
6. If and only if 3–5 all pass, write oracle 1's stdout to `standard/workloads/golden/<wl>.out`,
   record both oracles' SHA-256, this document's SHA-256, and the pinned reference SHAs into
   `standard/workloads/golden/manifest.json`, and mark the golden files frozen.
7. If 3–5 fail, do **not** freeze. Resolve, record the discrepancy and its cause in the manifest, and
   repeat. No language may be measured before the golden files are frozen.

---

## 9. N/A rules (spec's "do not fabricate" requirement)

A cell is recorded as `"N/A"` with a machine-readable `reason` from this closed list. It is never
left blank, never guessed, never interpolated, and never replaced by a neighbouring language's value.
An `N/A` never counts as a zero and never counts as a success.

| Reason code | Meaning | Where it can legitimately occur |
|---|---|---|
| `NA_NO_COMPILE_STEP` | The language has no separate compile step under its frozen recipe, so `Compile Time` does not exist. | `Compile Time` for Python and Quidra Interpreter. `Parse Time` is measured instead and reported in its own column; the two are never averaged together. |
| `NA_NOT_PRODUCED` | The implementation never produced output, so an **unbounded magnitude** derived from that output genuinely does not exist. | **`Numerical Error` only**, on a `COMPILE_FAIL`, `RUNTIME_FAIL`, `TIMEOUT` or `OUTPUT_CONTRACT_FAIL` run. It may not be used for any bounded correctness fraction — see `FAIL_SCORED_ZERO`. |
| `FAIL_SCORED_ZERO` | The run produced no valid output **because the implementation failed**. This is a measured failure of the capability the metric is testing, not an inapplicability (spec 26). | On a `COMPILE_FAIL`, `RUNTIME_FAIL`, `TIMEOUT` or `OUTPUT_CONTRACT_FAIL` run, **every bounded correctness fraction is scored `0.0`, never `N/A`**: `Numerical Match`, `Prediction Match`, `Prediction / Assignment Match`, `Parameter Match`, `Likelihood / objective agreement`, `Accuracy`, `Forward Correctness`, `Backward Correctness`, `Test Pass Rate`. This is a **scored zero and not an `N/A` at all**; it is listed in this table only so that the reason is machine-readable and appears in `results/algo/na_log.json` as a non-`N/A` disposition. |
| `NA_METRIC_NOT_DEFINED` | The metric is not defined for this workload. | e.g. `Accuracy` on `WL-GMM` and `WL-LG`; `Parameter Match` on `WL-SVM` and `WL-LG`. |
| `NA_MEASUREMENT_UNAVAILABLE` | The host or toolchain cannot report the quantity honestly. | e.g. peak RSS for a runtime that reports only its own heap; must carry a free-text note naming the limitation. |
| `NA_TRIAL_VOID` | The trial was invalidated (operator intervention, harness fault, environment change mid-run). | LLM trials only; the void trial is still listed with its reason. |

Why `FAIL_SCORED_ZERO` exists: spec 26 requires "not applicable" to be distinguished from "the
language lacks a capability that the metric is intentionally testing", and forbids a missing
capability from escaping scoring by being labelled `N/A`. A program that does not compile has failed
the exact thing `Numerical Match` tests. Because spec 26 drops `N/A` cells from the applicable-weight
denominator and renormalizes, treating a failed run as `N/A` would score a language that failed
`WL-GMM` outright but matched on `WL-SVM` and `WL-LG` **identically to a language that matched on all
three** — the failure would be visible only in Test Pass Rate, while spec 13 and 15 list these as
separate scored metrics. `FAIL_SCORED_ZERO` closes that escape, and it is applied identically to all
ten languages.

A `TIMEOUT`'s `Execution Time` is the censored raw value `1800.0` s with `timed_out = true` and
`censored = true` (1.7, `06_micro_workloads.md` §5.7) — a real performance result, never `N/A`.

Any `N/A` in a published table must be traceable to a row in `results/algo/na_log.json` carrying the
reason code, the timestamp, and a free-text note.

### 9.1 `Compile / Parse Success` and `Parse Time` for configurations with no build step

`NA_NO_COMPILE_STEP` says `Parse Time` "is measured instead" for Python and Quidra Interpreter, and
1.7 classifies `COMPILE_FAIL` for those two as "the parse/import step fails". Neither is measurable
until the command is named, and `environment/environment.json` → `frozen_toolchain_recipes` currently
gives `python` only `run` and `quidra_interpreter` only `run` and `repl`. The commands are therefore
frozen here and **must be added to `environment.json` as `parse_check` entries before any measurement**:

| Configuration | Frozen parse-check command | `COMPILE_FAIL` condition |
|---|---|---|
| `python` | `python3 -m py_compile FILE.py` | non-zero exit |
| `quidra_interpreter` | the toolchain's non-executing check command — `quidra check FILE.qui` if the frozen toolchain provides it, otherwise the parse-only mode recorded in `environment.json` | non-zero exit |

If the Quidra toolchain provides neither a check command nor a parse-only mode, then `Parse Time` for
`quidra_interpreter` is `N/A` with `NA_MEASUREMENT_UNAVAILABLE` and a note naming the limitation, and
`COMPILE_FAIL` is determined from a run that exits with the interpreter's parse-error status **before
producing any stdout**. That fallback is a measurement limitation of one toolchain, recorded as such;
it is never a better outcome than having the command, and it does not exempt the configuration from
`FAIL_SCORED_ZERO`. The parse step runs under the same 1.10 timing discipline as a compile, and its
result feeds the `Compile / Parse Success` gate that separates `COMPILE_FAIL` from `RUNTIME_FAIL` for
these two configurations exactly as a build exit status does for the other nine.

---

## 10. Freeze record

| Item | Value |
|---|---|
| Document | `methodology/07_algorithm_workloads.md` |
| Status | FROZEN |
| Frozen on | 2026-09-17 |
| Benchmark run | `2026-09-17-7677581` |
| Tolerance id | `ALGO-TOL-1` (abs `1e-9`, rel `1e-6`, floored at the printing granularity `1e-6`/`1e-12`; strict diagnostic band `1e-12`), saved at `methodology/frozen_tolerance.json` |
| Workload ids | `WL-SVM`, `WL-GMM`, `WL-LG` |
| SVM sub-variant | `SoftMargin-SVM` |
| LightGrad subset | `LightGrad-Core` (LGC), defined in section 4, before any implementation |
| RNG | `LCG-PM` (Lehmer, mult `48271`, mod `2147483647`), Irwin-Hall(12) normal; seeds `1234567` (SVM), `20260917` (GMM); `WL-LG` uses no RNG |
| All library RNG removed | yes — `std::mt19937` + `std::normal_distribution` in `gmm_cpp` replaced by 3.4 |
| svm_cpp SHA | `5fa1b0951740b249a7b7bb170031b75470dbf090` |
| gmm_cpp SHA | `f405f0a4d04ad93f04c2969bbd232c956e6b0e2d` |
| lightgrad SHA | `8656ef7a9d00ea84ab5ed2f368e94aa12ba8a8b8` |
| Languages | Quidra, Python, C++, Rust, Go, Java, TypeScript, Kotlin, Swift, Zig (fixed column order) |
| Scenarios | A (Specification → Implementation) and B (Reference → Porting), kept separate, never merged |
| Per-run timeout | 1800 s (inherited from `06_micro_workloads.md` §5.7) |
| Silent Bug policy | a compiling, running, exit-0 program with a wrong answer is `SILENT_BUG`, never a success |
| Remediation | corrected 2026-09-18 after an adversarial audit, before any golden file was frozen and before any measurement; see the *Remediation changelog* |
| Normalization | frozen in 6.1: families A/B/C/E per metric, per-workload then unweighted mean over the three workloads, Scenario A and B normalized separately, clipped to `[0, 100]` |
| Normalization epsilons | `epsilon_numerical_error = 1e-12` (the `F12` print granularity); `epsilon_compile = 0.01 s` (adopted unchanged from `06_micro_workloads.md` §5.5). Both must also be recorded in `methodology/frozen_tolerance.json` |
| Family-C ratio disclosure | mandatory for Execution Time, Compile / Parse Time and Peak RSS (6.1) |
| Test Pass Rate denominators | `WL-SVM` 1, `WL-GMM` 1, `WL-LG` 6 (2.9, 3.10, 4.10) |
| Execution configurations | cold `EXEC_COLD` and warm `EXEC_WARM` (`--warm`, 1.10), never in the same column |
| Static source instrument | `scripts/count_source.py`, reusing `02_fact_taxonomy_and_density.md` §4 with R1/R4/R6 disabled (6.3) |
| Parse-check commands | `python3 -m py_compile` (Python); `quidra check` or the recorded parse-only mode (Quidra Interpreter) — 9.1 |
| Multi-configuration collapsing | 6.2: unweighted mean of configurations for correctness metrics, worst-case for Silent Bug, `06` §8 for performance; best-of-two prohibited; 5 LLM trials per cell for every language |
| Kernels written, not imported | 1.9 prohibition table, all 10 languages symmetrically, Quidra's `linear` and `neural` namespaces named first |
| Errata | ALGO-E-001 (2.3 first training point), 2026-09-18, pre-measurement |

---

## Remediation changelog

Applied 2026-09-18, in response to the adversarial audit of the frozen methodology.

**No results have been observed.** No golden file has been frozen, no comparator has classified a run,
no oracle output has been compared, and no language — Quidra included — has been measured on any of
these three workloads. These corrections are therefore legitimate pre-registration under spec 25.4,
not post-result formula selection. Section numbering is unchanged throughout; the new material is
carried in new subsections (2.6.1, 3.7.1, 6.1, 6.2, 6.3, 9.1) so that no existing cross-reference
moves.

**Symmetry test applied to every change below:** each rule was re-read with "Quidra" replaced by
"Zig" and by "Python". Any rule that only made sense because of something Quidra specifically does or
lacks was rewritten until it read the same for all ten.

| # | Severity | Defect | What changed | Direction for Quidra's expected score |
|---|---|---|---|---|
| 1 | **BLOCKER** | QUIDRA BIAS. §1.9's "standard library only" named the hazard for six of the nine comparison languages (`numpy`, BLAS, Eigen, `nalgebra`, `gonum`, `Accelerate`, parallel streams) and named nothing belonging to the language under evaluation, while Quidra's `linear` namespace (native linear-algebra fast path) and `neural` namespace (built-in reverse-mode autodiff) are language namespaces, not libraries. Read literally it let Quidra build `WL-SVM`'s Gram matrix with a native kernel and satisfy `WL-LG` by calling built-in autodiff while the other nine hand-wrote both. `00_cross_language_constraints.md` §C-6 patched only LightGrad, and §5.1 did not extract that document into any prompt. | §1.9's second bullet is now a **per-language prohibition table covering all ten languages, with Quidra listed first** (`linear` in any form, `neural` in any form, the `dnn` package), stating that a facility built into the language is prohibited exactly as an imported library is, and applying to **all three** workloads rather than only `WL-LG`. §4.1 carries the §C-6 rule in-document. §5.1 item 4 now extracts §1.9 **in full, including the table**, so no prompt can carry the unqualified sentence. §1.8 question **S6** makes it reviewable. | **LOWER.** Quidra must now hand-write the Gram/dot kernels for `WL-SVM`/`WL-GMM` and the whole autodiff tape for `WL-LG`, on the same terms as the other nine, and loses a native fast path it could previously have called. |
| 2 | **BLOCKER** | SPEC COMPLIANCE. §6 listed 17 metrics with no normalization family, no epsilon, no clipping, no aggregation and no mapping to any score. `Numerical Error` is exactly `0.0` for a bit-exact implementation — which §4.5 *expects* in all ten — so plain family C divides by zero and §25.1.C's shifted form with a predeclared epsilon was mandatory and absent. Execution Time spans ~400×, triggering §25.1's and §33 item 60's mandatory raw-and-ratio publication, which was never stated. | New **§6.1**: family per metric (A / B / C / E), `epsilon_numerical_error = 1e-12` (the `F12` print granularity), `epsilon_compile = 0.01 s` (adopted unchanged from `06` §5.5 rather than invented), per-workload normalization then unweighted arithmetic mean over the three workloads, Scenario A and B normalized separately, clipping and precision per §25.3, the *Feeds* column for every row, and the mandatory family-C ratio disclosure for Execution Time, Compile / Parse Time and Peak RSS. | Not a Quidra-bias finding. Direction **unknown and unpredictable** — no raw value exists for any language. The epsilons were chosen from measurement resolution and from a sibling document's already-derived value, not from any Quidra figure. |
| 3 | MAJOR | SPEC COMPLIANCE. §9's `NA_NOT_PRODUCED` marked the bounded correctness fractions `N/A` on any failed run. Since §26 drops `N/A` from the applicable-weight denominator and renormalizes, a language that failed one workload outright would have been scored on Numerical Match exactly like a language that matched all three. | `NA_NOT_PRODUCED` is now restricted to **`Numerical Error` alone** (the one genuinely unbounded magnitude). New `FAIL_SCORED_ZERO`: every bounded correctness fraction — Numerical Match, Prediction / Assignment Match, Parameter Match, Likelihood agreement, Accuracy, Forward / Backward Correctness, Test Pass Rate — is **scored `0.0`**, never `N/A`, on `COMPILE_FAIL` / `RUNTIME_FAIL` / `TIMEOUT` / `OUTPUT_CONTRACT_FAIL`. A `TIMEOUT`'s Execution Time is the censored `1800.0` s. | Applies identically to all ten. **Lower for any language that fails a workload**, Quidra included; no language can now convert a failure into an exemption. |
| 4 | MAJOR | NO FABRICATED VALUES. §2.3's self-check constant `x[0] ≈ (0.6836, 1.5163, 0.3262, -0.5479)` does not reproduce from the frozen RNG, seed and draw order. It sits in the text §5.1 extracts into **every** Scenario A and B prompt for all 10 languages, so it would have burned repair turns against correct generators or induced a distorted generator and a Silent Bug. | Re-derived from the frozen `LCG-PM`, Irwin–Hall(12) normal and §2.3 draw order and replaced with `x[0] = (0.899197, 0.112540, 0.971311, -0.364383)`; `x[1] = (0.858706, 1.140231, 1.266498, -1.292345)` added so the check also catches a stream reset or a reused draw. Recorded as **erratum ALGO-E-001** with its discovery date and the fact that it predates any measurement. Every remaining informative constant must be re-derived from the §8 oracles and stamped with the oracle build before the goldens are frozen. *(The auditor's suggested `x[1]` was quoted to 4 decimals; the `F6` values above are this document's own re-derivation and supersede it.)* | Uniform across all ten languages; **unchanged** for Quidra relative to the others. It removes a uniform inflation of Repair Count. |
| 5 | MAJOR | PROMPT LEAK. §5.1 promised Scenario A withholds the golden output, then put "the workload's invariants" in the prompt — and the invariant sections carried golden field values verbatim (`ALPHA_Y_SUM 0.047792639128`, `ASSIGN_MIN_MARGIN 0.176425806929`, the printed `GAMMA_ROW_DEV_MAX` / `PI_SUM_DEV` strings, and §4.7's two gradients), some to full `F12` precision, invisibly unless the auditor knew the golden file. | §2.6 and §3.7 are rewritten as **bounds only**; every measured quantity moved to new **§2.6.1 / §3.7.1**, both marked *MEASURED, NOT EXTRACTED*. §5.1 now requires the extraction script to strip every measured parenthetical and to **fail loudly, blocking the run, if any numeric literal in the extracted text is also a field value in that workload's golden file**. §4.7's two gradients are kept — they are closed forms, not measured outputs — but are now written as their algebraic expressions with their evaluations, declared as the single named exception under spec §16.A, and the collision check is configured to allow exactly that set and nothing else. | Applies identically to all ten languages. **Unchanged** for Quidra; it removes an equal-for-all leak and makes the no-leak claim auditable from the saved prompts. |
| 6 | MAJOR | REPRODUCIBILITY. `Compile Time` was "frozen build recipe, 5 runs, median" with the build-cache state between runs unspecified — worth one to two orders of magnitude for `go`, `rustc`, `javac`, `kotlinc` and `swiftc`, and decisive for the median that is the headline figure. | §1.10 now restates a pinned discipline in full: one untimed priming build, then 5 × (delete artifacts only → timed build), median; toolchain caches are **not** purged; artifact deletion is outside the timed window; identical sequence for all 10. **Direction of the discipline taken from `06_micro_workloads.md` §5.5 rather than invented here** — see the rejection note below. | **Unchanged.** The discipline is the same for all ten and no Quidra figure was consulted. |
| 7 | MAJOR | REPRODUCIBILITY. `Source Bytes / LOC / Source Tokens` were "static analysis of the submitted single file" — no tokenizer, no LOC rule — while all three feed family-C scored metrics; the LOC rule alone moves C++/Rust/Java/Zig against Python by 20–30 %. | New **§6.3**: `Source Tokens` = the frozen tokenizer of `02_fact_taxonomy_and_density.md` §4.3–§4.6 over the whole submitted file, retaining R2/R3/R5 and every §4.5/§4.6 constraint, with R1 (probe region), R4 (optional-lexeme removal) and R6 (dead-code removal) explicitly disabled and recorded as disabled. `LOC` = lines with ≥ 1 token after R2 (blank and comment-only excluded; a brace-only line counts). `Source Bytes` = LF-normalized byte length. One script, `scripts/count_source.py`, run for all ten before any score is computed, SHA recorded. | **Unchanged.** Quidra's profile is built by the same §4.5 procedure as the other nine, with its fallbacks logged. |
| 8 | MAJOR | REPRODUCIBILITY + INTERNAL CONTRADICTION. §1.8's anti-subversion check was a free-text `pass` / `fail` review with no rubric, and it can flip a run from PASS to SILENT_BUG. §1.8 ("SILENT_BUG even if its output matches golden") and §2.9 ("recorded as a latent defect … if the output is identical") gave opposite answers on a case §2.9 itself raised. | §1.8 is now a **closed checklist S1–S9**, each answered yes/no with a source line citation, preserved per submission and published; `fail` on S1–S8 yields `SILENT_BUG` regardless of output. §1.7 adds `LATENT_DEFECT` as an orthogonal annotation with a required proof of inertness at the frozen constants; §2.9's `S_inside` bullet now routes through S9 and states the same rule as §1.8. | **LOWER.** S6 makes the §1.9 prohibition list — whose first row is Quidra's `linear` and `neural` namespaces — mechanically checkable rather than dependent on a reviewer's opinion, and a `linear.dot` that reproduced golden bit-exactly would previously have escaped. |
| 9 | MAJOR | QUIDRA BIAS + REPRODUCIBILITY. §6 deferred Quidra's two-mode collapsing to "the aggregation rule frozen elsewhere", i.e. `06` §8 — a rule written for performance metrics with no row for any correctness metric collected here, and whose Step 5 substitutes the passing mode. Applied to Test Pass Rate, Numerical Match or Silent Bug occurrence that is **best-of-two**: two chances to pass each workload and to avoid a Silent Bug, against one for the other nine. Separately, with two recipes in the prompt it was undefined whether Quidra ran 5 LLM trials per cell or 10. | New **§6.2**, written about *execution configurations* rather than about Quidra: for every correctness-class metric a language's value is the **unweighted mean of its frozen configurations**, **best-of-two is prohibited**, and `06` §8 Step 5's substitution explicitly does not apply to any metric in this document; a Silent Bug in **any** configuration is a Silent Bug for the language, named; performance and static metrics stay with `06` §8; and **LLM trials are 5 per cell for every language, never 10** — one generated source file built and run in every configuration, `LLM Generation Success` and `Repair Count` recorded once against the first-listed configuration, with repair turns driven by that configuration's diagnostics alone so no language gains repair budget from a second configuration. | **LOWER.** Quidra loses two independent chances to pass each workload and to avoid a Silent Bug; an Interpreter-only failure now enters the scored mean instead of being dropped, and an Interpreter-only Silent Bug is now reported as Quidra's. |
| 10 | MAJOR | FAIRNESS ACROSS ALL TEN. §1.10 published a no-op `STARTUP_TIME` so wall time could be read honestly, and gave memory no equivalent — while §4.7 makes Peak RSS a deliberately scored signal. Peak RSS is dominated by each runtime's floor and GC policy, a multiple-× family-C penalty on Go, Java, Kotlin, Node and CPython for a property of the runtime rather than the submitted code; on `WL-SVM` and `WL-GMM` (≈ 320 KB and ≈ 1.6 MB working sets) it measures essentially nothing else. | §1.10 adds **`BASELINE_RSS`** from the same no-op program under the identical recipe, published beside every Peak RSS figure with the `peak_rss_i / best_positive_peak_rss` ratio, never silently subtracted, with no workload-specific heap/GC/arena flag permitted. §4.7 states that the `WL-LG` memory-management signal is read as `peak_rss − baseline_rss` **in the narrative**, both raw numbers published, the scored raw staying unadjusted. §6.1 records that Peak RSS on `WL-SVM`/`WL-GMM` is floor-dominated and reported for completeness rather than as a discriminator. | **Slightly lower / disclosure.** The scored raws are unchanged for every language, but Quidra Interpreter's own runtime floor becomes visible and quotable beside Quidra Native's, where previously only the combined peak appeared. |
| 11 | MINOR | `Test Pass Rate` for `WL-LG` had no denominator (5, 6 or 7 were all readable), while `WL-SVM`/`WL-GMM` were booleans — and §25.2 averages the three unweighted, so the choice moved every language's aggregate. | §4.10 fixes it at **6** (main run + five §4.9 error runs; `NODE_TYPES == 6` is part of the main run's PASS and scores no separate point). §2.9 and §3.10 state denominator **1** each. | **Unchanged.** Identical for all ten. |
| 12 | MINOR | `Parameter Match`, `Likelihood / objective agreement` and `Accuracy` were collected but never numerically defined; raw `Accuracy` would have tied every conforming implementation at 0.99 and discriminated nothing. | All three are defined in §1.6 beside `Prediction Match`: Parameter Match = fraction of the 52 `COMP` scalars within `numeric_match`; Likelihood agreement = a boolean over the named fields, published with each field's relative deviation; Accuracy = a boolean over `TEST_CORRECT` and the four accuracy fields, with the raw accuracies published un-normalized and unscored. The overlap between these and Numerical Match is declared in §6.1. | **Unchanged.** Identical for all ten. |
| 13 | MINOR | FAIRNESS ACROSS ALL TEN. §1.10 produced cold whole-process timings only and mitigated with a no-op `STARTUP_TIME`, which contains no JIT warm-up — while the hot loops of `WL-SVM` and `WL-LG` are interpreted for thousands of iterations in Java, Kotlin and Node. `06` §5.4 solves this structurally for the micro suite; this document had no counterpart. | §1.10 adds **`EXEC_WARM`** beside `EXEC_COLD`: a second frozen configuration, selected by the frozen `--warm` flag now named in §1.4, in which the entry point runs the whole workload body twice in one process, prints nothing on the first pass and is timed only on the second. Identical for all 10 languages and all 3 workloads, excluded from Peak RSS and Compile Time; no table mixes a cold and a warm figure in one column and each names which it contains. The escape hatch of declaring a warm variant out of scope was **not** taken. | **LOWER (relative).** Quidra Native is AOT-compiled and gains least from warm measurement; Java, Kotlin, TypeScript/node, and to a lesser degree Python and Quidra Interpreter, gain most. This narrows Quidra Native's lead on the warm column. |
| 14 | MINOR | The binary64 deviation was justified by "three of the ten languages cannot express binary32", while naming only two — and only two qualify. An unnamed third in a fairness justification is exactly the shape of "a capability was dropped because the language under evaluation lacks it". | §1.1 now says **two**, names them (TypeScript, Python), states that the other eight **including Quidra (`float32`) can** express binary32, and records the miscount as a correction. | **Unchanged.** The justification is now checkable and confirms nothing was dropped for Quidra's benefit — Quidra has `float32`. |
| 15 | MINOR | `COMPILE_FAIL` for Python and Quidra Interpreter was "the parse/import step fails" and `Parse Time` was "measured instead", but no parse command exists in the frozen recipes — leaving the Compile/Parse Success gate undefined for two of the eleven measured configurations. | New **§9.1** freezes `python3 -m py_compile FILE.py` and, for Quidra Interpreter, `quidra check FILE.qui` or the recorded parse-only mode, with a named fallback (`NA_MEASUREMENT_UNAVAILABLE` plus parse-error-status detection) that is explicitly never a better outcome than having the command and never an exemption from `FAIL_SCORED_ZERO`. Both must be added to `environment.json` before measurement. | **Unchanged to slightly lower.** If the Quidra toolchain has no check command, Quidra Interpreter's `Parse Time` becomes a disclosed `N/A` with a named limitation rather than an unstated advantage. |
| 16 | MINOR | SPEC COMPLIANCE (§10.4, §33 item 53). §7 and §8 required the comparator to *pass* correct input; nothing required it to *reject*. An inverted predicate or an over-wide tolerance would have passed every positive test and the two-oracle check, then classified every Silent Bug as PASS — the one failure that invalidates all three workloads at once. | §7 adds the mandatory negative suite (a)–(g): golden-vs-golden, just-under and just-over the band per field kind, a discrete field off by 1, the full output-contract corpus (`nan`, `inf`, `-0.000000`, `\r\n`, missing final newline, dropped/extra/misordered line, wrong field count, scientific notation), each invariant violated in isolation, and a strict-band flip. Run with its output preserved **before** the goldens are frozen; its result hash goes into the §8 manifest. | **LOWER for any language whose Silent Bugs would otherwise be missed**, symmetrically — including Quidra, whose `WL-LG` autodiff Silent Bug modes (§4.10) are precisely the ones a lax comparator would absorb. |
| 17 | MINOR | Three undefined points: (1) whether the per-iteration log-likelihood is the E-step's `L_total` or a post-M-step evaluation — the two differ by one EM step and change `DELTA_LOGLIK_LAST`, `MONOTONE` and `CONVERGED`'s input; (2) where `STRESS_GRAD_P0/Q0` are captured; (3) `NODE_TYPES` defined as "`Function` subtypes", which has no meaning in Zig or Go-without-embedding and is trivially satisfied by printing `6`. | (1) §3.5 pins the **post-M-step** evaluation, 61 evaluations total, `LOGLIK` = the value after iteration 60. (2) §4.7's pseudocode captures both gradients immediately after `loss.backward()` and before `optimizer.step()` at `step == 0`. (3) §4.8 redefines `NODE_TYPES` as distinct concrete **realizations** of the `Function` abstraction — subtypes where the language has them, tagged-union variants or dispatch-table entries where it does not (Zig, Go) — each with its own `backward` and `typeName()`, with §1.8 question **S8** verifying the count against source and the printed value explicitly not counting as evidence. | **Unchanged**, except that (3) removes a definition that read as inheritance-shaped and now reads identically for all ten, and makes the printed `6` uncheatable in every language. |

### Self-review findings (not itemised in the brief; found by applying the same rules to the rest of the document)

| # | Severity | Defect | What changed | Direction for Quidra |
|---|---|---|---|---|
| SR-1 | MAJOR | CLAIM OF QUIDRA-INDEPENDENCE CONTRADICTED BY THE DOCUMENT'S OWN CONTENT. §0.1 item 1 claimed nothing in these workloads was derived from Quidra's feature set, while §1.2 chose a 31-bit generator specifically so that no implementation needs 64-bit unsigned wraparound — which `00_cross_language_constraints.md` §C-1 and `06_micro_workloads.md`'s own fairness declaration both record as a constraint Quidra's overflow-checked integers cannot meet. The blanket claim was therefore false as written. | §0.1 item 1 now records the overlap explicitly instead of claiming it away: the constraint is independently and unavoidably imposed by Python and TypeScript, it *also* suits Quidra, and this document does **not** claim the choice is Quidra-independent. §1.2 cross-references the disclosure. Quidra's overflow-check cost is named as measured elsewhere (MB-02/03/07/11, adversarial set) and nothing here is shaped to avoid it. | **Unchanged in score, lower in claimed fairness.** An unearned neutrality claim is withdrawn; no constant or workload changed. |
| SR-2 | MINOR | The frozen recipes compare Quidra's always-on checking against `zig build-exe -OReleaseFast` (bounds and overflow checks off) and `rustc -O` (overflow checks off), and the document said nothing about it. Left unstated, a reader cannot tell whether the asymmetry was noticed. | §0 adds a recorded safety-posture disclosure naming which of the ten run these workloads with checking on and which with it off, states that the asymmetry is **not** corrected for and **not** excused in either direction, and requires it to be reprinted as a footnote under every Execution Time and Peak RSS table these workloads produce. | **Unchanged.** The asymmetry currently runs *against* Quidra; it is disclosed, not compensated. Adding a compensating adjustment would itself have been a prohibited change. |
| SR-3 | MINOR | §4.1 justified dropping operator overloading with "not available in several of the 10 languages" — the same unnamed-count shape as finding 14, and Quidra 0.2.0 documents no operator overloading either, so the drop happens to suit the language under evaluation. | The reason now names them: unavailable in **Go, Java, TypeScript and Zig**, and undocumented in Quidra 0.2.0; available in C++, Rust, Python, Kotlin and Swift. It also records that spec §15's capability checklist does not name operator overloading and that the abstraction requirement is carried instead by the `Function` interface and its six realizations, which all ten can express — so nothing spec §15 asks for is lost. | **Unchanged.** The drop is genuinely multi-language (five of ten). Naming Quidra in the list is the point: the reason is now checkable rather than taken on trust. |
| SR-4 | MINOR | Seven of §6's metrics are overlapping views of the same stdout (`OBJECTIVE` sits in Numerical Match, Likelihood agreement and Numerical Error; `COMP` fields sit in Numerical Match and Parameter Match), which would let one property be counted several times by a downstream weighting. | The overlap is declared in §1.6 and again in §6.1, before any result exists, with the requirement that any downstream combination account for it explicitly. The metrics are kept separate because spec §13/14/15 name them separately — none was deleted. | **Unchanged**, and the same for all ten. |

### Findings rejected

None. Every finding in the brief was applied in the direction the auditor specified.

**One direction-of-implementation note, recorded so it is not mistaken for a silent weakening.** Finding
6's *required fix* text proposed a **cold**-cache compile discipline (purge `GOCACHE`, `CARGO_HOME`,
module caches, `.tsbuildinfo`, `__pycache__` before each of the 5 timed builds). The finding's defect —
"the cache state is never fixed, and it is worth one to two orders of magnitude" — is accepted in full
and is fixed in §1.10. The *direction* is taken from `06_micro_workloads.md` §5.5, which, in its own
post-audit remediated text, freezes the opposite discipline in explicit terms ("Perform one priming
build … Do not purge `~/.cache`, `GOCACHE`, or any toolchain cache: the everyday incremental rebuild is
the thing being measured, and it is measured identically for all"). Writing a cold rule here would have
put two frozen documents in direct contradiction over the same measurement on the same host, which is a
worse defect than either discipline. What the finding actually requires — that the state be **pinned,
identical for all ten, and reproducible by an independent analyst** — is delivered either way, and
§1.10 now states the full sequence in-document rather than by reference. The choice is not
Quidra-shaped: no Quidra build time was consulted, and the warm discipline helps whichever toolchains
have build caches (Go, Rust, Java, Kotlin, Swift), not Quidra specifically. Finding 6's *epsilon*
suggestion of `0.001 s` is likewise superseded by `06` §5.5's re-derived `0.01 s` for the same reason,
and §6.1 records that.

### Counterpart obligations (cross-document changes this correction requires elsewhere)

These are fixes on the **other** side of a cross-document boundary. This document has been corrected as
the findings specify; the owners of the documents below must make the matching change before any
measurement.

| Document | Change required |
|---|---|
| `00_cross_language_constraints.md` §C-6 | §C-6 prohibits Quidra's `neural` namespace for LightGrad only. This document's §1.9 extends the same rule, symmetrically and for **all three** workloads, to Quidra's `linear` namespace (`linear.dot`, `linear.matmul`, and any validated linear-algebra fast path), because `WL-SVM`'s Gram matrix and `WL-GMM`'s Cholesky/forward-substitution kernels are exactly the computation those workloads measure. §C-6 (or a new §C-6b) must be widened to match, so the two documents state one rule. |
| `06_micro_workloads.md` §8 | §8's discriminator and its Step 5 are written for performance metrics. §8 must state that it does **not** govern the correctness-class metrics of `07` §6 — Compile / Parse Success, Test Pass Rate, Numerical Match, Prediction / Assignment Match, Parameter Match, Likelihood agreement, Accuracy, Forward / Backward Correctness, Silent Bug occurrence — which `07` §6.2 owns, and that no passing-configuration substitution occurs for any of them. |
| `06_micro_workloads.md` §6 | The Compile / Build Performance row of §6's mapping table still reads `epsilon_compile = 0.001 s` while §5.5 now derives and freezes `0.01 s`. The stale value must be reconciled to `0.01 s`; `07` §6.1 adopts `0.01 s`. |
| `environment/environment.json` → `frozen_toolchain_recipes` | Add (a) `parse_check` commands for `python` and `quidra_interpreter` per `07` §9.1; (b) the compile-measurement sequence of `07` §1.10 / `06` §5.5 (priming build, artifact-only deletion, 5 timed builds) recorded per language; (c) the frozen `--warm` and `--error <N>` invocations in each language's `run` entry, so the two selectors are part of the frozen recipe rather than of the prose. |
| `methodology/frozen_tolerance.json` | Add a `normalization` block carrying `epsilon_numerical_error = 1e-12`, `epsilon_compile = 0.01`, and the family assignments of `07` §6.1, so the epsilons are frozen in a machine-readable artifact and not only in prose. `07` §1.6's reproduction of the file is already re-worded to say so. |
| `02_fact_taxonomy_and_density.md` §4 | Record that `scripts/count_source.py` (`07` §6.3) is a second, declared consumer of the §4 tokenizer and profiles, running with R1, R4 and R6 disabled over whole submitted files, so that any future change to §4's rules is known to affect both instruments. |
| `09_llm_run_config.json` | Record `07` §6.2 item 4: exactly **5** trials per (workload × scenario × language) cell for every language, one generated source file per trial built and run in every frozen configuration of that language, `LLM Generation Success` and `Repair Count` recorded once against the first-listed configuration, and repair turns driven by that configuration's diagnostics alone. |
| `scripts/` (prompt extraction) | Implement `07` §5.1's golden-collision check: the extractor must strip every measured parenthetical and every *MEASURED, NOT EXTRACTED* subsection (§2.6.1, §3.7.1, §2.8, §3.9, §4.8) and **fail, blocking the run**, if any numeric literal in the extracted text is also a field value in that workload's golden file, allowing only the named closed-form exception set of §4.5 / §4.6 / §4.7. |
