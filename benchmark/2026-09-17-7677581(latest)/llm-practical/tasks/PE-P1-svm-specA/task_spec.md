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


---

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
