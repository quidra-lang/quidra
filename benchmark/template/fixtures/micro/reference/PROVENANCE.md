# Golden micro-benchmark output: provenance and validation

`golden_c.txt` is the frozen expected output for all eleven micro workloads. Every one of the ten
languages' implementations is gated against it before any timing run (methodology 06 §5.1).

## How it was produced

Two **independent** reference implementations of the same frozen workload specification:

| Reference | Toolchain | Command |
|---|---|---|
| `ref.c` | Apple clang 17 | `clang -O2 -ffp-contract=off ref.c -o ref_c -lm` |
| `ref.py` | CPython 3.14.5 | `python3 ref.py` |

Neither is one of the eleven measured configurations, so neither can bias a measured language.

## Validation result

The two references were run and their result lines compared:

```
grep '^MB' golden_c.txt  > c_only.txt
grep '^MB' golden_py.txt > py_only.txt
diff c_only.txt py_only.txt   ->  no differences
```

**All eleven workload lines are byte-identical between the two references**, including every
floating-point field printed to 17 significant digits. Agreement is therefore far inside the frozen
tolerance of `|Δ| <= max(1e-9·|expected|, 1e-12)`, and the integer/checksum fields agree exactly.

Two implementations written in different languages, with different compilers, different floating-point
code generation and different integer models (fixed-width `long long` vs arbitrary precision), reaching
identical results is strong evidence that the expected values reflect the *specification* rather than an
artefact of one implementation.

## Why it matters

A single reference implementation cannot distinguish "the specification says X" from "my reference
happens to compute X". Any language later disagreeing with a single reference would face an expectation
that might itself be wrong. With two independent agreeing references, a disagreement is far more likely
to be a genuine defect in the implementation under test — which is what the correctness gate is for.

## Validator soundness

`../../scripts/check_micro.py` was itself verified to be capable of BOTH outcomes (spec 10.4
pre-flight requirement (c)): it passes the golden output against itself, rejects a single-digit integer
perturbation (`MB02 145395` -> `145396`), and accepts a floating-point value nudged by 1e-11 relative,
which is inside tolerance. A validator that cannot fail measures nothing.
