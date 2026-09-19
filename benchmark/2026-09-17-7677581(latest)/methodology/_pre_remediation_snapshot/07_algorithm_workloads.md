# 07 — FROZEN Algorithm Workload Definitions: SVM, GMM, LightGrad

Status: **FROZEN**. Written before any implementation, generation, or measurement.
Benchmark run: `2026-09-17-7677581`
Spec authority: `prompt.md` sections 13 (SVM), 14 (GMM), 15 (LightGrad), 16 (LLM scenarios),
32 (prohibited practices), 6.1.7 (fairness/audit), 33 (completion criteria).

Once frozen, this document may not be changed in response to observed results. Any defect found in it
after measurement begins must be recorded as an erratum with its discovery timestamp, and the affected
measurements must be reported as-is or re-run in full for **all 10 languages**.

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

### 0.1 Fairness rules binding this document

1. Quidra is the language *under evaluation*. Nothing in these workloads was derived from Quidra's
   syntax, operators, type system, standard library, or feature set. The workloads were derived
   **only** from the three reference C++ repositories and from the requirement of cross-language
   determinism.
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
   with a machine-readable reason code.
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
> deliberately changed to binary64 because three of the ten languages cannot express binary32
> arithmetic in their standard language core without bit-manipulation shims (notably JavaScript
> `number` semantics behind TypeScript, and Python's lack of a native single-precision scalar).
> Requiring binary32 would impose an artificial, language-specific handicap. Per the task ordering
> rule, cross-language reproducibility outranks matching the reference exactly.

Integer counters are 64-bit signed unless stated otherwise. No implementation may rely on integer
overflow anywhere in these workloads.

### 1.2 The frozen deterministic RNG — `LCG-PM` (used for data generation only)

All pseudo-random data is produced by the Lehmer / Park–Miller "minimal standard" multiplicative
generator. It was chosen because every intermediate value fits exactly in binary64 (max product
`48271 × 2147483646 ≈ 1.0369e14 < 2^53`), so **every one of the 10 languages can implement it
exactly**, with 64-bit integers *or* with doubles, without needing unsigned 64-bit wraparound,
big integers, or bit tricks.

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

Programs take **no command line arguments** except the error-mode selector defined in 4.9, read no
files, and read nothing from stdin.

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

Frozen, saved as `methodology/frozen_tolerance.json` (content reproduced here verbatim so this
document is self-contained):

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

This check is a mandatory manual source review of every submitted implementation, recorded as
`subversion_review: pass | fail` with a reviewer note. It is applied identically to all 10 languages.

### 1.9 Structural requirements (identical for all 10 languages)

- **One source file per workload per language**, because the frozen build recipes compile a single
  file. "Multiple modules" therefore means **logical** modules: the language's own namespacing
  construct (`namespace`, `mod`, `package`-like object, `object`/`enum` namespace, `class` used as a
  module, or top-level grouping with a documented name), not separate files. The required module
  names are given per workload and must appear, spelled identically (case may follow the language's
  convention), in all 10 implementations.
- Standard library only. No third-party packages, no `numpy`, no BLAS, no `Eigen`, no `nalgebra`,
  no `gonum`, no `Accelerate`, no `java.util.stream` parallel operations.
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
- Measurement protocol per (language × workload): 1 untimed warm-up run + **5** timed runs; report
  min, median, mean, sample standard deviation; the **median** is the headline figure.
- Peak RSS is taken from the maximum resident set size of the timed runs (median of 5).
- These workloads are sized (2.2, 3.2, 4.7) so that the slowest expected runtime finishes well inside
  the 1800 s cap on the measurement host while the fastest is still comfortably above timer noise.

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

Informative: the first training point is `x[0] ≈ (0.6836, 1.5163, 0.3262, -0.5479)`; the dataset is
linearly non-separable (the frozen run misclassifies 2 of 200 training points).

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

Checked by the comparator on every run, independently of golden:

- `|ALPHA_Y_SUM| <= 1.0` (the dual feasibility residual; the frozen run gives `0.047792639128`).
- `0.0 <= alpha[i] <= C` for every printed `alpha[i]`.
- `NS_MARGIN + NS_INSIDE <= N` and `NS_MARGIN >= 1`.
- `TEST_ACC * 100` is an integer count divided by 100, i.e. `TEST_ACC ∈ {0.00, 0.01, ..., 1.00}`.
- `PRED` has exactly `N_TEST_C1 + N_TEST_C2 = 100` entries, each exactly `1` or `-1`.

Discrete-decision robustness (measured, recorded here so that exact comparison of the discrete fields
is justified rather than assumed):

| Quantity | Measured margin from its decision boundary |
|---|---|
| smallest non-zero `alpha[i]` vs `EPS_SV = 1e-7` | `1.551e-3` (15 514× above) |
| largest `alpha[i]` vs `C - EPS_SV = 10.0` | `0.1304` (76× below) |
| number of `alpha[i]` exactly `== 0.0` (bitwise) | 135 (clamped, so the comparison is exact) |
| smallest `|f(p)|` over the 100 test points | `1.010e-1` |
| smallest `|f(p)|` over the 200 training points | `2.767e-2` |

Every discrete decision in `WL-SVM` is therefore separated from its boundary by at least ten orders
of magnitude more than the tolerance band. Exact equality of `PRED`, `NS_MARGIN`, `NS_INSIDE`,
`CONVERGED` and the accuracy counts is required.

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
- **FAIL** — any of `COMPILE_FAIL`, `TIMEOUT`, `RUNTIME_FAIL`, `OUTPUT_CONTRACT_FAIL`.
- **SILENT BUG** — exits 0 with contract-valid output but a wrong answer. Concretely, and these are
  the failure modes actually reachable in this workload, each of which must be classified
  `SILENT_BUG` and never as a success:
  - Jacobi instead of Gauss–Seidel `alpha` update (a whole-sweep copy of `alpha`) — changes `W`, `B`,
    `OBJECTIVE`, typically by `>1e-2`.
  - Fixing the reference's `if/elif` chain so that clamped multipliers also set `judge`/`error` —
    changes `CONVERGED` and `ERROR_LAST`.
  - Updating `beta` inside the `i` loop instead of once per sweep.
  - Omitting `S_inside` from the `w` accumulation (harmless at these hyperparameters — `NS_INSIDE`
    is 0 — but still a wrong program; it is caught only by review, so 1.8's manual review is what
    detects it, and it is recorded as a latent defect rather than a `SILENT_BUG` if the output is
    identical).
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

Run **exactly `ITERS = 60` iterations**, with no early exit. The log-likelihood is computed once
before the loop (`LOGLIK_INIT`) and once after every iteration.

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

- `GAMMA_ROW_DEV_MAX = max over n of |(sum over k of gamma[n][k]) - 1.0|` must print as
  `0.000000000000` at `F12` (i.e. `< 5e-13`). Frozen run: `4.44e-16`.
- `PI_SUM_DEV = |(sum over k of pi[k]) - 1.0|` must print as `0.000000000000` at `F12`.
- `MONOTONE` must be `1`.
- `LOGLIK > LOGLIK_INIT`.
- Every `sigma[k]` printed must be symmetric to within `1e-9` and have strictly positive diagonal.
- `ASSIGN_MIN_MARGIN` (min over `n` of `top1(gamma[n]) - top2(gamma[n])`) is reported. Frozen run:
  `0.176425806929`, and **zero** points have a margin below `1e-6`. The hard assignments are
  therefore separated from their decision boundary by ~14 orders of magnitude more than the
  tolerance band, which is what justifies requiring exact equality of `ASSIGN_COUNTS`,
  `ASSIGN_CHECKSUM` and `ASSIGN_FIRST_20`.

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

Dropped, with reasons (each dropped item is dropped for **all 10** languages):

- `clone()` / `clone_pre()` / `clone_post()` deep graph cloning — pure C++-object-graph plumbing;
  `detach()` is kept because it has real autodiff semantics.
- `operator[]` / `Subscript` node, `operator<<` streaming, `operator+=`/`*=` in-place operator
  overloads, `from_array(const float*, ...)` raw-pointer overload — syntax-surface only, and
  operator overloading is not available in several of the 10 languages, so requiring it would
  advantage a subset of languages for a non-semantic reason.
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
`∂loss/∂P₀ = 0.282227`, `∂loss/∂Q₀ = 0.033203`. Both are reported so that a single backward step is
verifiable against the closed form without running the whole loop.

Each step constructs a fresh forward graph *and* a fresh backward graph (because backward builds
nodes, 4.4) — roughly 30 tensors of 4 096 doubles per step, ~4 500 tensors over the run. A program
that never releases them holds ~150 MB; a correct one holds a few MB. This is the memory-management
signal and it is why `Peak RSS` on `WL-LG` is a scored metric.

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

34 lines exactly. `NODE_TYPES` is the number of distinct concrete `Function` subtypes the
implementation defines; it must be exactly `6` (`Identity`, `View`, `Sum`, `Expand`, `Addition`,
`Multiplication`). It is a cheap machine-checkable proxy for the abstraction requirement, verified
against source during the 1.8 review.

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
run and contributes one point to `Test Pass Rate`.

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
4. The **constraints**: 1.1, 1.3, 1.9 (single file, standard library only, single-threaded, no
   third-party libraries), and the frozen build/run recipe for the target language.
5. The **correctness requirements**: 1.6 tolerance, the workload's invariants, and its PASS criteria.
6. The **tests**: for LightGrad, the closed-form expected values of 4.5/4.6/4.7 and the 4.9 error
   contract; for SVM and GMM, the invariant list only.

The prompt **must not** contain:

- any source file from `svm_cpp`, `gmm_cpp`, or `lightgrad`, in whole or in part;
- the golden output files;
- the informative reference values of 2.8, 3.9 and 4.8 for SVM/GMM/LightGrad *stress* fields.
  (The LightGrad closed-form tables of 4.5 and 4.6 **are** included, because they are mathematical
  facts stated as tests, exactly as spec 16.A permits "tests" to be provided.)

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
| Compile Time | ✔ | ✔ | ✔ | frozen build recipe, 5 runs, median |
| Source Bytes / LOC / Source Tokens | ✔ | ✔ | ✔ | static analysis of the submitted single file |
| LLM Generation Success | ✔ | ✔ | ✔ | 5.3, per scenario |
| Repair Count | ✔ | ✔ | ✔ | 5.3, per scenario |
| Silent Bug occurrence | ✔ | ✔ | ✔ | 1.7 |

Quidra Native and Quidra Interpreter are measured and stored as two separate rows in the raw results
for every cell above, and collapsed into one `Quidra` column only in the final language-level table,
using the aggregation rule frozen elsewhere in the methodology.

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
| `NA_NOT_PRODUCED` | The implementation never produced output (compile fail / crash / timeout), so output-derived metrics are undefined. | `Numerical Match`, `Numerical Error`, `Prediction Match`, `Forward/Backward Correctness` on a `COMPILE_FAIL`, `RUNTIME_FAIL` or `TIMEOUT` run. |
| `NA_METRIC_NOT_DEFINED` | The metric is not defined for this workload. | e.g. `Accuracy` on `WL-GMM` and `WL-LG`; `Parameter Match` on `WL-SVM` and `WL-LG`. |
| `NA_MEASUREMENT_UNAVAILABLE` | The host or toolchain cannot report the quantity honestly. | e.g. peak RSS for a runtime that reports only its own heap; must carry a free-text note naming the limitation. |
| `NA_TRIAL_VOID` | The trial was invalidated (operator intervention, harness fault, environment change mid-run). | LLM trials only; the void trial is still listed with its reason. |

Any `N/A` in a published table must be traceable to a row in `results/na_log.json` carrying the
reason code, the timestamp, and a free-text note.

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
