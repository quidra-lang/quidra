# WL-SVM — Golden Output Provenance

**Workload:** `WL-SVM` (Soft Margin SVM), benchmark run `2026-09-17-7677581`
**Frozen on:** 2026-09-17
**Status:** FROZEN — two independent oracles agree **bit-for-bit**; golden gate verified to both
accept and reject (PF-05).

---

## 1. Summary

| | |
|---|---|
| Golden file | `golden/expected_stdout.txt` (2425 bytes, 21 lines, LF, pure ASCII) |
| SHA-256 | `ce23320612111b75a7475b1899bb8bc2e30978b975d5019e200a52499391119d` |
| Max abs difference, C oracle vs Python oracle | **0** (exactly zero — byte-identical output) |
| Numerical error `max \|a−g\|/(1+\|g\|)` | **0** |
| Frozen tolerance applied | `ALGO-TOL-1` (`abs=1e-9`, `rel=1e-6`, `print_ulp` F6 `1e-6` / F12 `1e-12`) |
| Every field inside tolerance | **Yes** — and inside the *strict* diagnostic band (`1e-12`/`1e-12`) as well, trivially, since the difference is zero |
| Section 2.6 invariants | All pass |
| Gate can pass **and** fail | Verified — 4 positive fixtures accepted, 10 mutated negatives rejected |

---

## 2. The two independent implementations

Both were written from `methodology/07_algorithm_workloads.md` section 2 only. They are measurement
infrastructure, not benchmark submissions, and are not counted as any measured language.

| | Oracle 1 | Oracle 2 |
|---|---|---|
| Source | `../ref_c.c` | `../ref_py.py` |
| SHA-256 | `d288be2f94fa19ea1a63315c9f8dc7c9e0fc3e13634ba69d79ece913e7b14f62` | `1995f8e8584e22f6bf948adde76a9a1fdfd247374091b3f855d25c88ac4e5a17` |
| Size | 12846 bytes | 14141 bytes |
| Language | C (C99) | Python 3 (standard library only, no numpy) |
| Toolchain | Apple clang 17.0.0 (`clang-1700.6.3.2`), target `arm64-apple-darwin25.4.0` | CPython 3.14.5 |
| Integer model | `int64_t` — LCG state in fixed-width 64-bit signed arithmetic | arbitrary-precision `int` — LCG state in bignum arithmetic |
| Float codegen | native arm64 scalar FP, contraction explicitly disabled | CPython eval-loop `double` ops, no contraction available |
| Data layout | static C arrays, `y` held as `double` | nested `list`s, `y` held as `int`, converted per use with `float()` |
| Structure | free functions with `data_` / `svm_` / `app_` prefixes | classes `data` / `svm` / `app` used as namespaces (permitted by 07 §1.9) |
| Host | macOS 26.4.1, arm64 (Apple silicon) | same host |

The two differ in integer model, float code generation path, memory layout, sign handling for `y`,
and program structure. They share no code and no intermediate files. Agreement between them is
therefore evidence about the **specification**, not about one implementation's quirks.

### How far they agreed

Not "within tolerance" — **identical**. All 21 lines, all 324 fields
(215 F6 + 1 F12 + 108 discrete INT), byte-for-byte:

```
ce23320612111b75a7475b1899bb8bc2e30978b975d5019e200a52499391119d  out_c.txt
ce23320612111b75a7475b1899bb8bc2e30978b975d5019e200a52499391119d  out_py.txt
ce23320612111b75a7475b1899bb8bc2e30978b975d5019e200a52499391119d  golden/expected_stdout.txt
```

The maximum absolute difference over every F6 and F12 field is **exactly 0.0**, which is nine orders
of magnitude inside the `1e-9` absolute floor of `ALGO-TOL-1` and also inside the strict diagnostic
band of §1.6. No field required the tolerance at all.

This is a stronger result than §1.5 demands (§1.5 requires agreement within the *strict* band). It
is attainable here because `WL-SVM` is arithmetic-only: §1.2's Irwin–Hall RNG deliberately avoids
`log`/`sqrt`/`sin`/`cos`, the workload uses no transcendental functions anywhere, the accumulation
order is pinned by §1.3.1, and the C build disables FMA contraction. Every operation is therefore a
correctly-rounded binary64 `+`, `-`, `*` or `/` on both sides, in the same order. `WL-GMM` (which
uses `exp`/`log`) should **not** be expected to reproduce this.

---

## 3. Exact commands

Run from this directory's parent (`algo/reference/WL-SVM/`), on macOS 26.4.1 arm64:

```sh
# Oracle 1 — C
clang -O2 -ffp-contract=off ref_c.c -o REF_c -lm
./REF_c > out_c.txt                       # 0.06 s user

# Oracle 2 — Python
python3 ref_py.py > out_py.txt            # 4.90 s user

# Reconciliation
diff out_c.txt out_py.txt                 # no output: identical
shasum -a 256 out_c.txt out_py.txt        # same digest

# Field-by-field comparison under ALGO-TOL-1 (not just diff)
python3 compare_svm.py out_c.txt out_py.txt

# Freeze
cp out_c.txt golden/expected_stdout.txt
printf '%s  expected_stdout.txt\n' \
  "$(shasum -a 256 golden/expected_stdout.txt | cut -d' ' -f1)" > golden/SHA256.txt
( cd golden && shasum -a 256 -c SHA256.txt )    # expected_stdout.txt: OK
```

`-ffp-contract=off` is used for the **oracle** so that oracle 1 and oracle 2 evaluate the identical
expression tree. This is a property of the oracle, not a constraint on the measured languages:
§1.3.4 permits FMA contraction in submissions, which is exactly why the gate in §2.9 is a tolerance
rather than a textual diff. The golden values are contraction-free, and `ALGO-TOL-1`'s `1e-6`
relative band is what absorbs contraction in the ten measured implementations.

Both oracles were rebuilt and re-run from source during this reconciliation and reproduced the
stored `out_c.txt` / `out_py.txt` exactly.

---

## 4. Comparison result, field by field

Comparator: `compare_svm.py` (kept beside this file). It parses the §2.7 schema, applies the
`ALGO-TOL-1` predicate per field kind, requires exact equality on discrete fields, and checks the
§2.6 invariants. It is not a text diff.

```
max abs difference over all F6/F12 fields : 0
numerical error  max |a-g|/(1+|g|)        : 0
tolerance ALGO-TOL-1 : abs=1e-09 rel=1e-06, print_ulp F6=1e-06 F12=1e-12

section 2.6 invariants:
  [ok] |ALPHA_Y_SUM| <= 1.0                                 0.047792639128
  [ok] 0 <= alpha[i] <= C for all i                         min=0.000000 max=0.130354
  [ok] NS_MARGIN+NS_INSIDE <= N and NS_MARGIN >= 1          65+0=65, N=200
  [ok] TEST_ACC*100 is an integer count                     0.990000 -> 99
  [ok] PRED has 100 entries, each +1 or -1                  n=100, set=[-1, 1]
  [ok] NS_MARGIN/NS_INSIDE consistent with printed ALPHA    recomputed 65/0 vs printed 65/0
  [ok] ALPHA_SUM consistent with printed ALPHA vector       sum(printed alpha)=2.581095 vs ALPHA_SUM=2.581096
  [ok] ALPHA_CHECKSUM consistent with printed ALPHA vector  recomputed=126.024486 vs printed=126.024432
  [ok] TEST_CORRECT consistent with PRED                    from PRED 49 50 99 vs printed [49, 50, 99]

VERDICT: PASS
```

(The last two consistency checks recompute from the F6-rounded printed `ALPHA`, so they carry a
~1e-4 rounding budget by construction; they catch permuted or truncated vectors, not last-digit
drift.)

### Output contract conformance (§1.4, §2.7)

21 lines; LF only (0 CR bytes); trailing newline present; pure ASCII; no leading, trailing or double
spaces; no blank lines; no scientific notation; no `nan`/`inf`; no negative zero.

---

## 5. Independent cross-checks against the specification

Neither oracle was used to produce the values the spec records, so these are genuine checks that the
golden reflects the **specification** rather than a shared implementation habit. Diagnostics were
emitted at 17 significant digits from an instrumented build of oracle 1 whose stdout was confirmed
byte-identical to the golden.

| Quantity | Spec §2.6 / §2.4 records | Measured from the golden run | Agrees |
|---|---|---|---|
| `NS_MARGIN` | 65 | 65 | yes |
| smallest non-zero `alpha[i]` | `1.551e-3` (15514× above `EPS_SV`) | `1.5514229605444383e-3` | yes |
| largest `alpha[i]` | `0.1304` (76× below `C`) | `0.13035437682102208` | yes |
| `alpha[i]` exactly `== 0.0` (bitwise) | 135 | 135 | yes |
| smallest `\|f(p)\|` over 100 test points | `1.010e-1` | `0.10103650445204664` | yes |
| smallest `\|f(p)\|` over 200 train points | `2.767e-2` | `0.027672956987356487` | yes |
| `MAX_ABS_DELTA / LIMIT` | 32461× | `32461.661788403741` | yes |
| `ALPHA_Y_SUM` | `0.047792639128` | `0.047792639128424762` | yes |

All 21 lines of the golden also match the §2.8 informative reference block exactly, including the
quoted first-10 `PRED` and first-10 `ALPHA` values.

`WL-SVM` has no closed-form expected values — §1.5 reserves closed-form checking for LightGrad's
gradients and for the closed-form invariants of SVM/GMM, and SVM's are the inequality/consistency
invariants of §2.6, all of which are checked above and all of which hold. The RNG stream was
additionally verified by a **third** route: the first four `next_normal()` draws were recomputed in
exact rational arithmetic (`fractions.Fraction`, one rounding at the end) and agree with the pinned
binary64 ascending accumulation to within 1 ulp, confirming the dataset is not an artifact of
accumulation order.

### Erratum recorded (§1.11)

`07_algorithm_workloads.md` §2.3 contains the informative prose note:

> "the first training point is `x[0] ≈ (0.6836, 1.5163, 0.3262, -0.5479)`"

**This informative value is wrong.** The correct value under §1.2 and §2.3's own normative
pseudocode is:

```
x[0] = (0.89919728706553459, 0.11254049945275346, 0.97131129487944468, -0.36438288905861849)
     ≈ (0.8992, 0.1125, 0.9713, -0.3644)
```

confirmed by three independent routes: oracle 1 (C, `int64_t` LCG), oracle 2 (Python, bignum LCG),
and exact rational arithmetic. The quoted point does not occur anywhere in the frozen dataset
(all 300 generated points were searched; no match within 5e-4).

Per §1.11 — *"If a conforming oracle disagrees with an informative value, the oracle wins and the
informative value is corrected as an erratum"* — the oracles win. **No implementation was changed**,
because neither implementation is wrong: both follow the normative pseudocode, and the §2.8
informative block (the full 21-line output, which is the load-bearing informative value) matches
them exactly. The §2.3 note appears to be a stale artifact of an earlier draft. The frozen spec file
was **not** edited; this erratum is the record. Implementers self-checking against §2.3 should use
the value above.

---

## 6. Gate verification — PF-05, "every validator can both pass and reject"

`10_intrinsic_design.md` §10.1 PF-05 requires ≥1 positive fixture that must pass and ≥1 **mutated**
negative fixture that must be rejected. A validator that cannot fail measures nothing.

**(a) The golden passes against itself:** byte-identical copy → `VERDICT: PASS`, exit 0.
Both `out_c.txt` and `out_py.txt` also pass against the frozen golden.

**(b) Deliberately perturbed copies are rejected.** Full matrix — 4 positives accepted, 10 negatives
rejected, zero unexpected outcomes:

| Fixture | Mutation | Expect | Actual |
|---|---|---|---|
| `M0_identical` | none (positive control) | PASS | PASS |
| `M1_beta_last_digit_INSIDE` | `BETA` +1e-6 (adjacent last printed digit; bound 1.535e-6) | PASS | PASS |
| `M10_alpha_y_sum_5e-10_INSIDE` | `ALPHA_Y_SUM` +5e-10 (bound 4.879e-8) | PASS | PASS |
| `M11_alpha_y_sum_5e-9_INSIDE` | `ALPHA_Y_SUM` +5e-9 (bound 4.879e-8) | PASS | PASS |
| `M2_beta_2ulp_OUTSIDE` | `BETA` +2e-6 | FAIL | FAIL |
| `M3_w0_1e-2_OUTSIDE` | `W[0]` +1e-2 (the §2.9 silent-bug magnitude) | FAIL | FAIL |
| `M12_alpha0_1e-5_OUTSIDE` | `ALPHA[0]` +1e-5 | FAIL | FAIL |
| `M13_alpha_y_sum_2e-7_OUTSIDE` | `ALPHA_Y_SUM` +2e-7 | FAIL | FAIL |
| `M4_pred_flip_DISCRETE` | one `PRED` entry `1` → `-1` | FAIL | FAIL |
| `M5_ns_margin_DISCRETE` | `NS_MARGIN` 65 → 64 | FAIL | FAIL |
| `M6_alpha_y_sum_INVARIANT` | `ALPHA_Y_SUM` → 1.5 (breaks §2.6 `\|·\| ≤ 1`) | FAIL | FAIL |
| `M7_alpha_gt_C_INVARIANT` | one `ALPHA` → 11.0 (breaks `0 ≤ α ≤ C`) | FAIL | FAIL |
| `M8_missing_line_CONTRACT` | one schema line deleted | FAIL | FAIL |
| `M9_alpha_199_CONTRACT` | `ALPHA` given 199 values instead of 200 | FAIL | FAIL |

Sample rejection diagnostics:

```
M3:  W[0]: |0.75235200000000002 - 0.74235200000000001| = 1.000e-02 > bound 1.000e-06
M2:  BETA: |1.534151 - 1.534149| = 2.000e-06 > bound 1.535e-06
M6:  [FAIL] |ALPHA_Y_SUM| <= 1.0    1.500000000000
M8:  OUTPUT_CONTRACT_FAIL (actual): expected 21 lines, got 20
```

The positive set is deliberately not just the identity: `M1`, `M10` and `M11` are *different bytes*
that must still pass, demonstrating the gate implements the `ALGO-TOL-1` band and its `print_ulp`
floor rather than performing a disguised byte comparison. The negative set brackets the band from
above (`M2` at 2e-6 vs `M1` at 1e-6 on the same field), demonstrating the boundary is where the
frozen tolerance puts it.

One label error was found and corrected during this check: a mutant perturbing `ALPHA_Y_SUM` by
5e-9 was initially expected to be rejected. It was accepted, and on recomputation the validator was
right — the bound for that field is `1e-9 + 1e-6 × 0.0478 = 4.879e-8`, so 5e-9 is legitimately
inside. The fixture was relabelled `INSIDE` and a genuine out-of-band mutant (`M13`, +2e-7) added.
Recorded because the expectation, not the gate, was the thing that was wrong.

---

## 7. Files

| File | Role |
|---|---|
| `expected_stdout.txt` | **The normative golden output.** 21 lines, 2425 bytes. |
| `SHA256.txt` | `shasum -a 256 -c`-compatible digest of the golden. |
| `PROVENANCE.md` | This file. |
| `compare_svm.py` | The field-by-field comparator used above (schema + `ALGO-TOL-1` + §2.6 invariants). Exit 0 = PASS. |
| `PF05_gate_matrix.txt` | Raw PF-05 result matrix. |

Reconciliation working directory (mutants and per-fixture logs):
`/private/tmp/claude-501/-Users-koba-Desktop-Quidra/c7dc7758-3bc7-4b38-ac54-4af31eae0682/scratchpad/wlsvm-reconcile/`

---

## 8. Note on the reconciliation environment

The first rebuild/re-run attempt in this session wrote to generic filenames directly in the shared
session scratchpad root and was clobbered mid-run by concurrent sibling reconciliations (`WL-GMM`
output appeared in a file named `rerun_c.txt`). This was caught because the resulting hashes
disagreed with the stored outputs. All results in this document come from the re-run in the isolated
directory `scratchpad/wlsvm-reconcile/`, where the C and Python oracles reproduced the stored
outputs and each other exactly. Recorded because a golden frozen from clobbered intermediates would
be undetectable later.

---

**Frozen:** 2026-09-17
**Tolerance:** `ALGO-TOL-1` (`methodology/frozen_tolerance.json`, `frozen: true`)
**Spec:** `methodology/07_algorithm_workloads.md` §2, §1.2–§1.6, §1.11
**Pre-flight:** `methodology/10_intrinsic_design.md` §10.1 PF-05
