# WL-GMM — Golden Output Provenance

**Workload:** `WL-GMM` — Gaussian Mixture Model via EM
**Benchmark run id:** `2026-09-17-7677581`
**Date frozen:** 2026-09-17
**Golden file:** `expected_stdout.txt`
**SHA-256:** `eb3aaf5f717b86c69baca4ac3b1de8ec1e0faba4254329eefeb6f848bc2999a1` (also in `SHA256.txt`)
**Status:** FROZEN — the two independent reference implementations agree bit-for-bit.

---

## 1. What this file certifies

The golden output was **not** copied from either implementation by preference, and **not**
reconciled by splitting any difference. Two independent implementations, written in different
languages from the frozen specification alone, produced **byte-identical** stdout. The golden is
that common output.

Because the two implementations use different integer models (C `long long` vs Python arbitrary
precision), different floating-point code generation (AArch64 native codegen at `-O2` with FMA
contraction disabled vs CPython's interpreted binary64 dispatch), and structurally different
code for the same specified quantities (see §5), their agreement is evidence that the expected
values reflect **the specification**, not one implementation's quirks.

---

## 2. Implementations reconciled

| | Implementation A | Implementation B |
|---|---|---|
| File | `../ref_c.c` | `../ref_py.py` |
| SHA-256 | `3127b5a29fa8f937d7636a4522f12ce3a8d51abb5fa4fd1d9b060694ab86782b` | `fa15538a124c3cd8111a6b896fa829b3b96768c485aa65a6d89b8061601f86ea` |
| Language | C (C99) | Python 3 (stdlib only: `math`, `sys`; no numpy) |
| Role | Oracle 1 | Oracle 2 |

Both are measurement infrastructure. Neither is one of the ten measured languages; neither is
ever timed.

### Toolchains

| | Implementation A | Implementation B |
|---|---|---|
| Toolchain | Apple clang 17.0.0 (`clang-1700.6.3.2`) | CPython 3.14.5 (Homebrew), built by Clang 21.0.0 (`clang-2100.0.123.102`) |
| Target / interpreter | `arm64-apple-darwin25.4.0` | `/opt/homebrew/opt/python@3.14/bin/python3.14` |
| Float model | IEEE binary64, `-ffp-contract=off` | IEEE binary64 (`mant_dig=53`, `rounds=1`, `float_repr_style=short`) |

**Host:** Darwin 25.4.0, arm64, Apple M2.

### Exact commands

```sh
# Implementation A — build and run
clang -O2 -ffp-contract=off ref_c.c -o REF_c -lm
./REF_c > out_c.txt

# Implementation B — run
python3 ref_py.py > out_py.txt

# Reconciliation (field-by-field, under ALGO-TOL-1)
python3 golden/gate_check.py out_c.txt out_py.txt

# Golden + hash
cp out_c.txt golden/expected_stdout.txt
shasum -a 256 golden/expected_stdout.txt > golden/SHA256.txt
```

Both programs exited `0` and wrote **nothing** to stderr. Both produced exactly the 18 lines of
the §3.8 output schema. Runtime: ≈0.06 s (C), ≈4.6–7.3 s (CPython) — consistent with the
specification's informative estimate of ≈6.5 s in CPython 3.14.

---

## 3. Comparison result

**Tolerance applied:** `ALGO-TOL-1`, from
`methodology/frozen_tolerance.json`
(SHA-256 `b9d8ec9a4621801a11362895bf7ba72329a8ff3e7949a2fcd569e5e22145c687`),
defined in `methodology/07_algorithm_workloads.md` §1.6.

```
numeric:   abs(actual - golden) <= max( 1e-9 + 1e-6 * abs(golden), print_ulp[field_kind] )
print_ulp: F6 -> 1e-6,  F12 -> 1e-12
discrete:  exact equality required
numerical error := max over F6/F12 fields of abs(actual-golden) / (1 + abs(golden))
```

### 3.1 Printed-field comparison (the gate)

| Quantity | Value |
|---|---|
| Schema fields compared | **92** (all F6, F12 and discrete fields of §3.8) |
| Fields outside tolerance | **0** |
| **Maximum absolute difference** | **0** (exactly zero — no field differs in any digit) |
| Numerical error | **0** |
| Byte-identical stdout | **yes** |
| Common SHA-256 | `eb3aaf5f717b86c69baca4ac3b1de8ec1e0faba4254329eefeb6f848bc2999a1` |

**Every field is inside tolerance, with zero margin consumed.** The tolerance band was not
needed: the two implementations did not differ at all at printed resolution.

### 3.2 Raw binary64 comparison (how far they actually agree)

Printed output is quantized to 6 or 12 decimal places, so byte-identical stdout on its own only
proves agreement to within the print quantum. To establish the true depth of agreement, both
implementations were re-run through instrumented copies (which leave stdout untouched — verified
identical) that dump every internal scalar at 17 significant digits:

| Quantity | Value |
|---|---|
| Raw binary64 scalars compared | **59** (`LOGLIK_INIT`, `LOGLIK`, `LOGLIK_PER_POINT`, `DELTA`, and per component `PI`, `MU`×3, `SIGMA`×9, plus the three invariant scalars) |
| **Bit-identical scalars** | **59 / 59** |
| Max abs difference (raw) | **0** |
| **Max ULP distance** | **0** |

The two implementations agree **to the last bit of every binary64 quantity**, not merely to the
printed decimal. This is roughly six orders of magnitude tighter than the F6 print quantum and
about 10^6 times tighter than the `1e-6` relative tolerance the gate allows.

> **Honest caveat on the strength of this evidence.** Bit-identity is expected here and should
> not be over-read. The specification pins every accumulation order and forbids reassociation
> (§1.3), the C build disables FMA contraction, and IEEE-754 `+ - * /` and `sqrt` are
> correctly rounded, so the only operations that could legitimately diverge are `log` and `exp`.
> On this host both implementations ultimately call the same system libm, so those agree
> bit-for-bit too. Across the ten measured toolchains `log`/`exp` **will** differ in the last
> bits — that is precisely what the `1e-6` relative band in ALGO-TOL-1 exists to absorb. What
> bit-identity here does establish is that the two implementations contain no structural,
> ordering or algorithmic disagreement whatsoever: every difference that could have come from
> the *transcription* rather than the *arithmetic library* is zero.

### 3.3 Agreement with the specification's own reference block

The golden was additionally compared against the informative reference values printed in
`07_algorithm_workloads.md` **§3.9**, extracted verbatim from the specification text:

- All 92 fields match. Max abs difference **0**. **Byte-identical.**

Two independent implementations and the specification's own recorded values form a
**three-way agreement**.

### 3.4 Disagreements found

**None.** No field of either implementation fell outside tolerance, so no diagnosis, no
correction to either implementation, and no re-run for reconciliation was required. Neither
implementation was modified.

---

## 4. Specification invariants (§3.7) — all verified on the golden

| Invariant | Required | Observed | Result |
|---|---|---|---|
| `GAMMA_ROW_DEV_MAX` prints `0.000000000000` at F12 (< 5e-13) | yes | `0.000000000000`; raw **4.4408920985006262e-16** | PASS |
| `PI_SUM_DEV` prints `0.000000000000` at F12 | yes | `0.000000000000`; raw 5.2180482157382357e-15 | PASS |
| `MONOTONE` | `1` | `1` | PASS |
| `LOGLIK > LOGLIK_INIT` | yes | −59264.451380 > −78084.196110 (gain 18819.744730) | PASS |
| every `sigma[k]` symmetric to 1e-9 | yes | max asymmetry **0.0** for all k=0..3 | PASS |
| every `sigma[k]` diagonal strictly positive | yes | min diagonal 0.601314 (k=1) | PASS |
| `ASSIGN_MIN_MARGIN` | reported; frozen run `0.176425806929` | `0.176425806929`; raw 0.17642580692908677 | PASS |
| points with margin below 1e-6 | **zero** | **0** (counted explicitly over all N=10000) | PASS |
| `ASSIGN_COUNTS` sum to N | 10000 | 2500+2500+2501+2499 = 10000 | PASS |
| `CONVERGED`, `ITERATIONS` | `1`, `60` | `1`, `60`; raw final increment exactly **0.0** | PASS |
| output line count | 18 | 18 | PASS |

The raw `GAMMA_ROW_DEV_MAX` of `4.44e-16` reproduces the specification's stated frozen-run value
of `4.44e-16` (§3.7) exactly, and the explicit count of points with margin below `1e-6` reproduces
the specification's stated **zero** — independent corroboration of two facts the specification
records but the quantized output cannot show. This is what justifies requiring **exact** equality
of `ASSIGN_COUNTS`, `ASSIGN_CHECKSUM` and `ASSIGN_FIRST_20`: the hard assignments sit ~1.8×10^5
times further from their decision boundary than the tolerance band is wide.

---

## 5. Evidence of implementation independence

The two files are not transliterations of each other. For identical specified quantities they use
materially different code:

| Specified quantity | Implementation A (C) | Implementation B (Python) |
|---|---|---|
| Canonical component sort (§3.6) | hand-written insertion sort over an index permutation | `sorted()` with a lexicographic tuple key |
| Init moments (§3.4) | two separate passes over `n` per dimension (`sum1`, then `sum2`) | one fused pass over `n` accumulating both `x_sum` and `x2_sum` |
| Top-2 gamma scan (§3.7) | argmax, then a second scan for the max over `k != best` | single pass maintaining `(top1, top2)` |
| `ASSIGN_CHECKSUM` | `long long` (64-bit wrapping integer model) | arbitrary-precision `int` |
| Output assembly | per-field `fputs`/`printf` to stdout | list of strings joined and written once |
| Negative-zero suppression (§1.4) | manual scan of the formatted buffer + `memmove` | `str.lstrip("-0.")` test |
| Module structure | prefix-namespaced statics (`data_`, `linalg_`, `gmm_`, `app_`) | classes used as namespaces |

Both were confirmed to compute their output rather than embed it: neither file contains any
literal from the expected output, and each was re-run from source in this session.

---

## 6. Pre-flight requirement §10.4(c) / PF-05 — the gate can both pass and fail

A validator that cannot fail measures nothing. The gate (`gate_check.py`, which implements the
ALGO-TOL-1 predicate above) was exercised against positive fixtures that must pass and **mutated**
negative fixtures that must be rejected. All mutations are recorded here.

### 6.1 Positive fixtures — all PASSED (required)

| Fixture | Mutation | Result |
|---|---|---|
| `P1_self` | none — golden against itself | **PASS** (required) |
| `P2_python_oracle` | none — independent Python output against golden | **PASS** (required) |
| `P3_within_tol` | `LOGLIK` −59264.451380 → −59264.401380 (Δ = 0.05, inside the 5.926e-2 bound) | **PASS** (required) |
| `P4_print_ulp` | `SIGMA[0][0]` 1.022658 → 1.022659 (last-digit F6 wobble, the `print_ulp` floor case) | **PASS** (required) |

P3 and P4 matter: they prove the gate is a genuine **tolerance** gate that admits legitimate
cross-toolchain variation, not an exact-match check wearing a tolerance's clothes.

### 6.2 Negative fixtures — all REJECTED (required)

| Fixture | Mutation | Result |
|---|---|---|
| `N1_gamma_dev` | `GAMMA_ROW_DEV_MAX` → `0.000000002000` (2e-9, just past the 1e-9 bound) | **REJECTED** |
| `N2_checksum` | `ASSIGN_CHECKSUM` 1223898 → 1223899 (discrete, off by one) | **REJECTED** |
| `N3_assign` | 5th label of `ASSIGN_FIRST_20` 0 → 1 | **REJECTED** |
| `N4_loglik` | `LOGLIK` → −59264.511380 (Δ = 0.06, just past the 5.926e-2 bound) | **REJECTED** |
| `N5_monotone` | `MONOTONE` 1 → 0 | **REJECTED** |
| `N6_permuted` | `COMP 0` and `COMP 1` payloads swapped — the "canonical sort omitted" silent bug of §3.10 | **REJECTED** |
| `N7_margin` | `ASSIGN_MIN_MARGIN` +1e-6 (bound is 1.774e-7) | **REJECTED** |
| `N8_truncated` | final line removed (17 lines — output-contract failure) | **REJECTED** |
| `N9_sigma` | `SIGMA[3][0][0]` 2.128444 → 2.128944 (4th-decimal algorithmic error) | **REJECTED** |

**Result: 4/4 positives passed and 9/9 negatives were rejected. PF-05 is satisfied.** The gate
discriminates in both directions, including the `N4`/`P3` pair that straddles the tolerance
boundary from either side.

---

## 7. Reproducibility notes

- Both implementations were re-run from source in this session; each reproduced its committed
  output (`out_c.txt`, `out_py.txt`) byte-for-byte. The Python reference was run four times and
  produced the identical SHA-256 every time.
- `out_c.txt` and `out_py.txt` already carried the same SHA-256 as the golden before this
  reconciliation, and both match the specification's §3.9 block.
- **Recorded environment hazard (not a defect in this workload):** the scratchpad directory used
  for this run is shared with sibling reconciliation jobs. One intermediate scratch file written
  under a generic name was overwritten mid-run by a concurrent **WL-SVM** job, which briefly made
  a Python rerun appear to emit a 19th line. The stray line was traced to
  `algo/reference/WL-SVM/out_*.txt` and is unrelated to WL-GMM. `ref_py.py` contains exactly one
  `sys.stdout.write`, emitting exactly the 18 schema lines. All results above were subsequently
  re-derived in an isolated directory. No WL-GMM artifact was affected. Future reference runs
  should use job-scoped scratch paths.

## 8. Files in this directory

| File | Purpose |
|---|---|
| `expected_stdout.txt` | the frozen golden output (18 lines, 926 bytes, trailing newline) |
| `SHA256.txt` | `shasum -a 256` of the golden, in `shasum -c` format |
| `PROVENANCE.md` | this record |
| `gate_check.py` | the ALGO-TOL-1 field-by-field validator used for §3 and §6; `gate_check.py <actual> <golden>`, exit 0 = pass |
