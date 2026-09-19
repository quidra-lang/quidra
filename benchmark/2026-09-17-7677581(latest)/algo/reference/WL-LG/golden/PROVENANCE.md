# WL-LG — Golden Output Provenance

Workload: `WL-LG` — LightGrad Common Subset ("LightGrad-Core", LGC)
Benchmark run: `2026-09-17-7677581`
Frozen spec: `methodology/07_algorithm_workloads.md` §4 (with the universal conventions of §1)
Frozen tolerance: `methodology/frozen_tolerance.json` → `ALGO-TOL-1`
Date of this reconciliation and freeze: **2026-09-17** (UTC timestamp of the final run:
`2026-09-17T11:12:50Z`)

Status: **GOLDEN FROZEN.** The two independent oracles agree **byte-for-byte**; the maximum
absolute difference over all fields is exactly **0.0**.

---

## 1. The two independent implementations

Per spec §1.5, the golden file is produced by a reference oracle and must be confirmed
field-for-field, within the *strict* band, against an independently written oracle. Both oracles are
measurement infrastructure; neither is a benchmark submission and neither counts as any language's
implementation.

| Role | File | SHA-256 |
|---|---|---|
| Oracle 1 (golden producer) | `algo/reference/WL-LG/ref_c.c` | `dd6352806dd0d755565991a78334892ee1afa1b3bf23ab7578a1f600aa705338` |
| Oracle 2 (independent confirmation) | `algo/reference/WL-LG/ref_py.py` | `36dd70b858d6ffc1090ea6d5a2acbd23156a0da8d4a796e7306056e7fdeca564` |

The two differ in every respect that could plausibly manufacture a shared error:

- **Different languages and object models.** C with explicit `struct Tensor` / `struct Function`,
  function-pointer dispatch and a global allocation registry; Python with classes, duck-typed
  method dispatch and reference-counted garbage collection.
- **Different integer models.** C `size_t` (fixed-width, unsigned) for every shape, size and loop
  index; Python arbitrary-precision `int`. The shape/size arithmetic (`product(shape)`,
  `SIZE_MISMATCH` checks, the `(i % 7) + 1` and `(i % 5) + 1` initializers of §4.7) is therefore
  computed under two different integer semantics.
- **Different floating-point code generation.** Apple clang 17 at `-O2` emitting arm64 scalar
  FP with contraction explicitly disabled, versus CPython's interpreted `double` arithmetic through
  `PyFloat` boxing. No shared compiler, no shared runtime, no shared libm path.
- **Different output formatting paths.** C `snprintf("%.6f", ...)`; Python `"%.6f" % v`.

Both implement the autodiff engine from scratch (per `00_cross_language_constraints.md` C-6); neither
imports one, and neither uses any third-party library.

## 2. Toolchains

| Oracle | Toolchain | Version |
|---|---|---|
| C | Apple clang | `Apple clang version 17.0.0 (clang-1700.6.3.2)`, target `arm64-apple-darwin25.4.0` |
| Python | CPython | `Python 3.14.5` |

Host: macOS 26.4.1, `arm64`.

Note on `-ffp-contract=off`: the C oracle disables FMA contraction. Spec §1.3.4 *permits* contraction
for the ten measured languages and is the reason `ALGO-TOL-1` is a tolerance rather than a textual
diff. Disabling it in the oracle is deliberate and strictly conservative: the golden values are the
un-contracted, straightforwardly-rounded ones, so a measured language that does contract is compared
against the arithmetically plainest reference rather than against one implementation's contraction
pattern. It does not narrow the band any language is judged by.

## 3. Exact commands

```sh
cd algo/reference/WL-LG

# build and run oracle 1 (C)
clang -O2 -ffp-contract=off ref_c.c -o REF_c -lm
./REF_c > out_c.txt                       # exit 0

# run oracle 2 (Python)
python3 ref_py.py > out_py.txt            # exit 0

# reconcile
diff out_c.txt out_py.txt                 # no output: byte-identical

# freeze
cp out_c.txt golden/expected_stdout.txt
shasum -a 256 golden/expected_stdout.txt | awk '{print $1}' > golden/SHA256.txt
```

Both oracles were rebuilt and re-run from source during this reconciliation; each reproduced its
previously stored `out_*.txt` byte-for-byte, so the stored outputs are not stale artifacts of an
earlier source revision.

## 4. Comparison result

Comparator: `algo/reference/WL-LG/tolerance_gate.py`, which parses each printed decimal back to a
binary64 and applies the frozen `ALGO-TOL-1` predicate including the `print_ulp` floor, exactly as
spec §1.6 note 3 requires.

| Quantity | Result |
|---|---|
| Lines | 34 in both (schema §4.8 requires exactly 34) |
| Fields compared | **144** — 141 `F6` values + 3 `INT` values (`LIGHTGRAD_VERSION`, `STRESS_STEPS`, `NODE_TYPES`) |
| Discrete (`INT`) mismatches | **0** |
| **Maximum absolute difference over all `F6` fields** | **`0.0` (exactly zero)** |
| Numerical Error, `max |actual−golden| / (1 + |golden|)` | **`0.0`** |
| Fields failing `numeric_match` (abs `1e-9`, rel `1e-6`, floor `1e-6`) | **0 of 141** |
| Fields failing `strict_match` (abs `1e-12`, rel `1e-12`) — diagnostic | **0 of 141** |
| Exact Textual Match — diagnostic | **true** (byte-identical stdout) |

**How far the two agreed:** completely. Not "within tolerance" — *identical*. The C and Python
oracles produce the same 34 lines, the same 144 fields, the same bytes. The agreement is therefore
not resting on the 1e-6 band at all; the band was never consumed. Two implementations with different
integer models, different floating-point code generation, different memory models and different
decimal-formatting paths landing on identical bytes is strong evidence that these values are
determined by the **specification**, not by either implementation's quirks.

No reconciliation, correction, or re-run was required: **neither implementation was found to be
wrong, and neither was changed.** Nothing was split, averaged, or arbitrarily chosen.

## 5. Independent closed-form verification

Byte-identity between two oracles would still be worthless if both encoded the same misreading of the
spec. Spec §1.5 and §4.10 therefore require the LightGrad values to be checked against
implementation-independent closed forms. This was done with a third, separate checker
(`scratchpad/wllg/closedform.py`) that contains **no tensors, no graph and no autodiff at all** — it
evaluates the analytic formulas of §4.5, §4.6 and §4.7 directly, and re-runs the §4.7 training loop as
a plain numeric SGD using the closed-form gradients `∂loss/∂Pᵢ = 2Pᵢ(Qᵢ+1)²`,
`∂loss/∂Qᵢ = 2Pᵢ²(Qᵢ+1)`.

| Quantity | Result |
|---|---|
| Closed-form values checked | **141 of 141** (all `F6` fields: 56 forward + 85 backward, matching §4.10) |
| Violations of `invariants.lightgrad_closed_form_rel = 1e-9` (floored at `print_ulp` `1e-6`) | **0** |
| Worst `|actual − closed form| / (1 + |closed form|)` | `3.412e-07`, on `STRESS_GRAD_P0` |
| Discrete checks | `STRESS_STEPS = 150` ✓, `NODE_TYPES = 6` ✓, `LIGHTGRAD_VERSION = 1` ✓ |

The worst deviation is a pure `F6` printing artifact, not a numerical disagreement: the exact
gradient is `0.2822265625`, which the output contract quantizes to `0.282227`. The residual is
`4.375e-7`, below the `print_ulp` floor the contract exists to accommodate. Every other closed-form
value agrees to better than that.

The three strongest discriminators in the workload all hold in the golden file:

- `SCALAR_D2Y_DX1DX2 = 72.000000` — the mixed partial. Obtainable only if the backward pass genuinely
  builds a differentiable graph (§4.5). An implementation accumulating gradients as raw numbers
  returns `0.000000` here while getting the first-order results right.
- `GRAD_A3 = 0` (12×) — the `detach()` discriminator (§4.6).
- `GRAD_A4 = 653.000000` — the `expand`-backward (`sum`-of-gradient) discriminator (§4.6).

Both oracles were additionally confirmed against the §4.9 error contract. All five error cases, in
both oracles, print nothing to stdout, emit exactly `ERROR: <CODE>` on stderr, and exit `2`:
`SHAPE_MISMATCH`, `SIZE_MISMATCH`, `EXPAND_NOT_SCALAR`, `EMPTY_TENSOR`, `GRAD_NOT_ENABLED`.

Both sources were read in full and reviewed against the §1.8 anti-subversion clause: no output value
is hard-coded, no loop is skipped or memoized, no `visited` set or topological sort is present in
`backward` (the `mul(h, h)` double-path contribution is genuinely counted twice), `newGrad()`
installs a fresh zero tensor rather than zeroing in place, gradient accumulation goes through
`ops.add`, and nothing is threaded, vectorized or library-backed. `subversion_review: pass` for both.

## 6. Tolerance applied

`ALGO-TOL-1`, from `methodology/frozen_tolerance.json`, unmodified:

```
numeric_match (normative gate): |actual − golden| <= max( 1e-9 + 1e-6·|golden|, print_ulp[kind] )
strict_match  (diagnostic)    : |actual − golden| <= max( 1e-12 + 1e-12·|golden|, print_ulp[kind] )
print_ulp                     : F6 = 1e-6,  F12 = 1e-12
discrete fields               : exact equality
```

`WL-LG` uses `F6` and `INT` only; no `F12` field appears in the §4.8 schema.

## 7. Pre-flight: the gate can both pass and fail (spec §10.4 requirement (c))

A validator that cannot fail measures nothing. Both directions were demonstrated against
`tolerance_gate.py` (exit `0` = PASS, `1` = REJECT/`SILENT_BUG`, `2` = `OUTPUT_CONTRACT_FAIL`).

**Must pass:**

| # | Input | Result |
|---|---|---|
| 1 | `golden/expected_stdout.txt` against itself | **PASS**, exit `0`, Numerical Error `0`, 0 failing fields |
| 2 | Python oracle output against golden | **PASS**, exit `0`, Numerical Error `0`, 0 failing fields |

**Must fail** — each perturbation is a single deliberate edit to a copy of the golden file:

| # | Perturbation | Result |
|---|---|---|
| 3 | `STRESS_LOSS_0` `1814.574524` → `1814.584524` (Δ `1e-2`; band at that magnitude is `1.8e-3`) | **REJECT**, exit `1`, field `STRESS_LOSS_0` reported |
| 4 | `NODE_TYPES` `6` → `5` (discrete field) | **REJECT**, exit `1`, "discrete field differs" |
| 5 | `SCALAR_D2Y_DX1DX2` `72.000000` → `0.000000` (the raw-number-gradient Silent Bug of §4.10) | **REJECT**, exit `1` |
| 6 | `GRAD_A4` `653.000000` → `2.000000` (the `expand`-backward-as-identity Silent Bug of §4.10) | **REJECT**, exit `1` |
| 7 | One schema line deleted | **`OUTPUT_CONTRACT_FAIL`**, exit `2` |

The gate therefore discriminates in both directions, and cases 5 and 6 confirm it catches the two
named Silent Bug modes that this workload exists to detect. Each rejection names the first failing
field, its actual value, its golden value and the observed deviation, as §1.7 requires as auditable
evidence.

## 8. Recorded observation — `print_ulp` boundary artifact (erratum candidate, **not** applied)

While exercising the gate, one further probe was run: perturbing a field by ±1 in its last printed
digit, which spec §1.6 note 3 states "never causes a false failure". Under the frozen predicate as
literally written, **4 of 268** such adjacent-digit perturbations across the 141 golden `F6` values
are rejected:

```
golden=0.200000  adjacent=0.200001  |d|=1.0000000000010001e-06  band=1e-06
golden=0.200000  adjacent=0.199999  |d|=1.0000000000010001e-06  band=1e-06
golden=0.282227  adjacent=0.282226  |d|=1.0000000000287557e-06  band=1e-06
golden=0.033203  adjacent=0.033202  |d|=1.0000000000010001e-06  band=1e-06
```

Cause: the floor is the exact constant `1e-6`, but the difference between two adjacent `F6` decimals,
once each is parsed back to binary64, is a hair *above* `1e-6` (`1.0000000000010001e-06`). The
comparison is `<=`, so it fails by roughly one part in `1e12`. It bites only on small-magnitude
fields, where the relative band lies below the floor — precisely the fields the floor was written to
protect (here: `NEW_A1[0]`, `STRESS_GRAD_P0`, `STRESS_GRAD_Q0`). 264 of 268 adjacent-digit
perturbations are accepted as intended.

**No change was made.** `methodology/07_algorithm_workloads.md` (lines 8–10) forbids changing a frozen
document in response to observed results, and `frozen_tolerance.json` carries `"frozen": true`. The
predicate is therefore implemented literally, exactly as frozen, and this is filed here as an erratum
candidate with its discovery timestamp (`2026-09-17T11:12:50Z`) for the benchmark owner to rule on
before the ten languages are measured.

**This does not affect the golden freeze.** The two oracles agree byte-for-byte, so no boundary case
arises anywhere in the golden file itself. It is recorded because it could affect a *measured*
language whose decimal-conversion rounding differs in the last digit on one of those three
small-magnitude fields, which is the exact scenario §1.6 note 3 promises will not be scored as an
error.

## 9. Frozen artifacts

| File | SHA-256 |
|---|---|
| `golden/expected_stdout.txt` (34 lines, LF, trailing newline) | `7010f373e668c76ca123c30e74800f7103b2b3dde77c98ccf72f5bdd1f6e8edd` |

Recorded in `golden/SHA256.txt`.
