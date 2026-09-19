# 06 — Micro Benchmark Workloads, Measurement Protocol, and Quidra Mode Aggregation

**Status: FROZEN.** Authored before any micro-benchmark measurement was taken and before any
micro-benchmark result was examined.

**Authority.** This document implements specification sections 12 (Micro Benchmarks), 11 (Quidra Native
and Interpreter Evaluation), 7 (Standard Evaluation Fairness), 25 (Scoring Methodology), 26 (N/A Policy)
and 32 (Prohibited Practices) of `prompt.md` for the run `2026-09-17-7677581`.
Where this document is silent, `prompt.md` governs. Where this document is specific, it is binding and
must not be modified after the first timed measurement.

**Fairness declaration.** Nothing in this document was derived from Quidra's syntax, operators, type
system, standard library, or feature set. Every workload is expressible with constructs that predate all
ten languages (integer and floating-point arithmetic, arrays, loops, recursion, functions, text files,
and a general-purpose associative container). No capability was removed because Quidra might lack it; a
language that lacks a capability is handled by the explicit fallback and scoring rules in §9, never by
silently deleting the workload. The author of this document had no knowledge of how any language would
rank on any of these workloads at the time of writing.

**Fixed language column order (all tables everywhere):**
Quidra, Python, C++, Rust, Go, Java, TypeScript, Kotlin, Swift, Zig.

---

## 1. Execution configurations

Eleven measurement configurations. Ten are the fixed comparison languages; Quidra additionally
contributes a second configuration because specification §11 requires Native and Interpreter modes to be
measured and kept separate in raw results.

| Config id | Language | Build command (frozen) | Run command (frozen) | Has build step |
|---|---|---|---|---|
| `quidra_native` | Quidra | `quidra build FILE.qui -o BIN` | `./BIN` | yes |
| `quidra_interpreter` | Quidra | — | `quidra run FILE.qui` | no |
| `python` | Python | — | `python3 FILE.py` | no |
| `cpp` | C++ | `clang++ -std=c++20 -O2 FILE.cpp -o BIN` | `./BIN` | yes |
| `rust` | Rust | `rustc -O FILE.rs -o BIN` | `./BIN` | yes |
| `go` | Go | `go build -o BIN FILE.go` | `./BIN` | yes |
| `java` | Java | `javac -d OUT FILE.java` | `java -cp OUT Main` | yes |
| `typescript` | TypeScript | `tsc FILE.ts` | `node FILE.js` | yes |
| `kotlin` | Kotlin | `kotlinc FILE.kt -include-runtime -d FILE.jar` | `java -jar FILE.jar` | yes |
| `swift` | Swift | `swiftc -O FILE.swift -o BIN` | `./BIN` | yes |
| `zig` | Zig | `zig build-exe -OReleaseFast FILE.zig -femit-bin=BIN` | `./BIN` | yes |

These are exactly the `frozen_toolchain_recipes` of `environment/environment.json`. **No additional flag,
no additional tool, and no alternative invocation may be used for any language**, in either direction.
`JAVA_HOME=/opt/homebrew/opt/openjdk` is exported for every `java`, `javac`, `kotlinc` and
`java -jar` invocation, per `environment.json`.

One source file per (workload, configuration), named `mbNN.<ext>`, stored at
`benchmark/2026-09-17-7677581/micro/src/<config_id>/mbNN.<ext>`.
Quidra Native and Quidra Interpreter **share the same `.qui` source file**; only the invocation differs.
This is required: they must differ as execution modes, not as programs.

---

## 2. Global invariants

These apply to every workload and every language. They exist so that eleven independently written
implementations are forced to compute the same thing by the same route.

### 2.1 Numeric representability rule (portability, not favoritism)

**Every integer value that any workload computes, stores, or prints is guaranteed by construction to lie
in the closed interval `[-2^53, 2^53]` (|v| ≤ 9007199254740992).**

Every workload's parameters were chosen so that this holds for all intermediate products and sums; the
bound is proved per workload in §4 and was verified empirically by the reference implementations (§7.3).
Consequence: every language may use its ordinary 64-bit signed integer type (`long long`, `i64`, `int64`,
`long`, `Int`, `Int64`), and TypeScript may use `number`, without any language being pushed into
arbitrary-precision arithmetic or `BigInt` while others use machine words. No language needs a bignum
library; no language gains an advantage from having one.

Bitwise XOR appears in one workload (MB-03) and both operands are always in `[0, 2^31)`, so the 32-bit
semantics of JavaScript's `^` produce the same non-negative result as 64-bit XOR. This is stated so that
the TypeScript implementation does not need `BigInt`.

Integer division (`idiv`) always has non-negative operands, so truncating and flooring division coincide;
any language's normal integer division operator is correct (`//` in Python, `Math.floor(a/b)` in
TypeScript, `/` elsewhere).

`mod` always has non-negative operands, so remainder and modulo coincide.

### 2.2 Floating-point rules

* All floating-point values are IEEE-754 binary64 (`double` / `f64` / `Double` / `number`).
* Only `+`, `-`, `*`, `/`, comparison, and `sqrt` are used. No transcendental function appears in any
  workload, because `sin`/`cos`/`exp`/`log` are not correctly rounded and differ between platform math
  libraries, which would produce cross-language differences that are not a language property.
* Accumulation order is pinned by the pseudocode and must be preserved exactly. No language may
  reassociate the sums in source.
* No implementation may enable fast-math, unsafe-math, or float-reassociation options. None of the frozen
  recipes enables one.
* **Fused multiply-add contraction within a single statement is permitted for every language equally**
  (Apple clang's C/C++ default, Go's language-level permission, and any other compiler that does it by
  default). It is allowed because it is the toolchain's normal behaviour for normal code, not a
  benchmark-specific intervention, and it is available to all. The correctness tolerance in §2.4 is set
  wide enough to absorb it and narrow enough to catch a genuinely different computation; the margin
  between the two is at least three orders of magnitude for every printed field.

### 2.3 Deterministic input generation (no data files are shipped)

All pseudo-random input is produced by the **Lehmer / MINSTD** generator, identical in every language:

```
P = 2147483647          // 2^31 - 1, prime
A = 48271
state s, initialised to the workload's frozen seed, 1 <= s <= P-1

next_int():   s = (A * s) mod P ;  return s          // result in [1, P-1]
next_unit():  return next_int() / 2147483647.0       // binary64 in (0, 1)
```

Properties that make this choice language-neutral:

* `A * s` is at most `48271 * 2147483646 = 103664505044466` ≈ 1.04e14, far below `2^53`, so the
  multiplication is exact in binary64 as well as in any 64-bit integer type. No language needs 128-bit
  or arbitrary-precision arithmetic to reproduce the stream.
* `next_unit()` is one exact integer-to-double conversion and one division by a power-independent
  constant. Both are correctly rounded by IEEE-754, so every conforming language produces **bit-identical**
  input arrays. No multiply-add pattern occurs, so contraction cannot perturb the inputs.
* Seeds are given per workload in §4. Every seed is in `[1, P-1]`.

The generator is called in exactly the order the pseudocode states (e.g. "fill A completely, then fill B").

### 2.4 Output contract and correctness criterion

In `once` mode (§5.2), a program writes **exactly one line to stdout and nothing else to stdout**, then
exits with status 0. The line is:

```
MBNN <field> <field> ...
```

with single ASCII space separators and a trailing `\n`. Diagnostics, if any, go to stderr.

* **Integer fields** are printed in plain decimal with no thousands separators, no leading zeros, and no
  `+` sign. They must match the frozen expected value **exactly, as a string**.
* **Floating-point fields** are printed with **at least 17 significant decimal digits** (`%.16e`,
  `toExponential(16)`, `{:.16e}`, or an equivalent). Exponent spelling differs legitimately between
  languages (`e+08`, `e8`, `E+08`); therefore floating-point fields are **never compared as strings**.
  The harness parses each such field as a binary64 and applies:

  ```
  PASS  iff  |measured - expected| <= max( 1e-9 * |expected| , 1e-12 )
  ```

  This is the frozen floating-point tolerance for every micro workload and every floating-point field.
  Named fields (`s1=`, `mean=`, …) appear in the format shown in §4 so that the parse is unambiguous.
* A `NaN` or infinite value is always a failure.
* A configuration whose `once` output fails any field is **not** a valid measurement of that workload:
  see §9.

The expected values in §4 were produced by the reference implementations described in §7.3 and
independently cross-checked between two implementations written in different languages.

### 2.5 Idiomatic-but-not-cheating rules (specification §7 and §32)

Binding for every workload and every language:

1. **Release builds only, exactly the frozen recipe.** No extra optimisation flag, no `-march=native`,
   no PGO, no LTO beyond what the frozen recipe already implies, no JVM/Node tuning flags, no changed
   garbage collector, no changed heap size. Defaults everywhere.
2. **Single-threaded.** No threads, no goroutines, no async executors, no parallel collections, no SIMD
   thread pools, in any language, in any workload.
3. **No GPU, no Metal, no Accelerate/vDSP, no BLAS, no NumPy, no Eigen, no third-party numeric or
   container library, in any language.** The core computation must be written in the language under test
   using its own standard library.
4. **No hand-written SIMD.** No intrinsics, no `std::simd`, no `@Vector`, no inline assembly.
   *Compiler auto-vectorisation at the frozen flags is allowed for every language* — it is the compiler's
   normal behaviour for normal code, it is not language-specific tuning, and every language's toolchain is
   free to do it.
5. **No memoisation, caching, precomputation, table lookup, or closed-form substitution** for any quantity
   the pseudocode computes by iteration or recursion.
6. **The pinned algorithm and the pinned loop order are mandatory.** No tiling, no blocking, no loop
   interchange, no strength reduction written by hand, no early exit, no restructuring of the arithmetic
   expressions. Whatever restructuring a compiler chooses to do is the compiler's business and is
   permitted for all.
7. **No unsafe escape hatches used to remove checks.** Rust must be `#![forbid(unsafe_code)]`-clean; C++
   uses ordinary indexing; Java uses ordinary arrays; no `Unsafe`, no `unchecked` blocks, no raw pointer
   tricks. Where a language's normal release mode removes bounds checks on its own (Zig
   `-OReleaseFast`), that is the language's normal release behaviour and is allowed.
8. **Normal idiomatic data handling is expected**, per specification §7: pass by reference/slice/span,
   avoid gratuitous copies, use the language's ordinary array/list type. Deliberately pessimising any
   language is prohibited exactly as strongly as optimising one.
9. **No output other than the one result line**, and no I/O other than MB-10's files.
10. **No benchmark-specific hack of any kind**, including constant-folding the answer, reading the
    expected value from this document, dead-code-eliminating the workload by not consuming its result, or
    skipping work that the pseudocode performs.

Each workload adds its own specific notes in §4.

### 2.6 Anti-elimination rule

Several workloads repeat a kernel `R` times over unchanging data. To prevent a compiler in *any* language
from hoisting the repetition (which would silently turn an `R`-round workload into a 1-round workload in
some languages and not others), the pseudocode mutates one input element per round, as written. **This
mutation is part of the frozen algorithm and must be implemented verbatim.** It costs one array store per
round and changes the expected output, which is why the expected values in §4 already include it.

---

## 3. Workload parameter summary

| Id | Category | Pinned kernel | Size parameters | Seed | Rounds `R` | Ref-C wall¹ | Ref-Py wall¹ |
|---|---|---|---|---|---|---|---|
| MB-01 | Fibonacci | naive double recursion | n = 30…37 | — | 1 | 0.55 s | 6.00 s |
| MB-02 | factorial | recompute k! mod M for every k | N = 20000, M = 1000003 | — | 1 | 0.84 s | 17.02 s |
| MB-03 | integer arithmetic | 5-operation integer mix | N = 120000000 | 20263917 | 1 | 0.62 s | 32.94 s |
| MB-04 | floating-point arithmetic | 4-accumulator FP mix | M = 4000 | 20264917 | 75000 | 0.65 s | 46.32 s |
| MB-05 | vector inner product | straight-line dot product | N = 2000000 | 20265917 | 400 | 0.75 s | 26.21 s |
| MB-06 | matrix multiplication | classical `i-j-k`, flat row-major | n = 512 | 20266917 | 3 | 0.38 s | 24.30 s |
| MB-07 | sorting | bottom-up iterative merge sort | N = 2000000 | 20267917 | 4 | 0.43 s | 17.31 s |
| MB-08 | strings | 5 character-level passes | NW = 200000 words | 20268917 | 20 | 0.56 s | 19.12 s |
| MB-09 | statistics | two-pass moments, Pearson, histogram | N = 2000000 | 20269917 | 30 | 0.30 s | 14.95 s |
| MB-10 | file I/O | write + read back 3 text files | N = 1000000 lines | 20270917 | 3 | 0.54 s | 2.08 s |
| MB-11 | collections | hash map + set + dynamic array | N = 1000000 | 20271917 | 3 | 0.05 s² | 1.83 s |

¹ **These are reference-implementation validation timings, not benchmark measurements.** They come from
the two provenance implementations of §7.3 (`clang -O2 -ffp-contract=off` and CPython 3.14.5), neither of
which is one of the eleven measured configurations. They were used for exactly two purposes: to confirm
that a fast natively compiled language lands in the mandated 0.3–3 s band, and to confirm that the slowest
plausible configuration stays far inside the 1800 s per-process timeout (§5.7) even in steady mode
(7 × 46 s ≈ 324 s worst case). They are not evidence for any metric and must not be reported as results.

² The MB-11 reference uses direct-indexed arrays rather than a hash map, because only the *values* it
produces are needed (and their independence from the container implementation is precisely what the
Python cross-check with real `dict`/`set` confirms). It is therefore not a timing reference for MB-11; a
standard hash map at this size is expected to be roughly an order of magnitude slower than the array
reference.

**Size-selection rule and its one documented exception.** Sizes were chosen so that a fast natively
compiled language lands in the 0.3–3 s band, which keeps process startup (≈2–10 ms) and timer resolution
(≈1 µs) at or below 1 % of the measurement while still allowing ≥5 repetitions plus steady-state
iterations on an 8 GiB host. The reference times above confirm the band for MB-01…MB-10. MB-05 is
memory-bandwidth-bound and MB-04 auto-vectorises heavily; both were scaled up until they reached the band
(MB-04 to R = 75000, MB-05 to 8·10⁸ multiply-adds). No workload was sized with reference to Quidra, whose
performance was unknown when these sizes were fixed. Sizes are frozen for all eleven configurations
alike; **no language ever runs a reduced size**.

---

## 4. The eleven workloads

Notation: `=` is assignment; `mod` and `idiv` have non-negative operands (§2.1); arrays are 0-based;
`float` means binary64. Loop bounds are inclusive on the left and exclusive on the right unless written
`a..b` with an explicit "inclusive".

---

### MB-01 — Fibonacci

**Category:** Fibonacci. **Purpose:** function-call and recursion cost.

**Algorithm (pinned):**

```
function fib(n):                      // n is a small non-negative integer
    if n < 2: return n
    return fib(n-1) + fib(n-2)

total = 0
for n = 30 to 37 inclusive:
    total = total + fib(n)
print "MB01 " + total
```

**Input data:** none; the arguments 30…37 are literal. No generator is used.

**Workload size:** exactly `sum(n=30..37) (2*fib(n+1) - 1) = 200311684` calls to `fib`. The eight
distinct arguments exist so that no compiler can common-subexpression-eliminate a repeated call to a pure
function of a constant.

**Representability:** `total = 61899717 < 2^53`. ✔

**Repetitions inside the program:** 1 (the eight-argument loop is the whole kernel).

**Expected output (exact):**

```
MB01 61899717
```

**Correctness criterion:** exact string match of the integer field.

**Workload-specific fairness notes:** the recursion must be the literal double recursion above — no
memoisation, no iterative rewrite, no closed form, no explicit stack, no caching of `fib(n)` between the
eight top-level calls, no `@lru_cache` or equivalent. Whether a compiler inlines or partially unrolls the
recursion is the compiler's normal behaviour and is allowed for all.

---

### MB-02 — Factorial

**Category:** factorial. **Purpose:** tight integer multiply/modulo loop without bignum.

Modular arithmetic is used instead of arbitrary-precision factorials **for a language-neutral reason**:
exact 20000! would force half the comparison set to import or hand-write a bignum library while Python
would use a builtin, which specification §32 forbids as "outsourcing the core computation to an optimized
external native library for only one language".

**Algorithm (pinned):**

```
M = 1000003            // prime
N = 20000
total = 0
for k = 1 to N inclusive:
    f = 1
    for j = 2 to k inclusive:
        f = (f * j) mod M
    total = (total + f) mod M
print "MB02 " + total
```

**Input data:** none; all values are literal.

**Workload size:** `N*(N-1)/2 = 199990000` multiply+modulo steps.

**Representability:** `f <= M-1 = 1000002`, `j <= 20000`, so `f*j <= 2.00000e10 < 2^53`. ✔

**Repetitions inside the program:** 1.

**Expected output (exact):**

```
MB02 145395
```

**Correctness criterion:** exact string match.

**Workload-specific fairness notes:** the inner loop must restart from `f = 1` for every `k`. Carrying
`f` across iterations of `k` (turning O(N²) into O(N)) is a different algorithm and is prohibited. No
caching of partial factorials, no closed form, no modular exponentiation tricks.

---

### MB-03 — Integer arithmetic

**Category:** integer arithmetic. **Purpose:** mixed integer add / multiply / modulo / xor / divide.

**Algorithm (pinned):**

```
N = 120000000
x = 20263917                 // the seed itself is the initial state
s_add = 0 ; s_xor = 0 ; s_mul = 1 ; s_div = 0
repeat N times:
    x     = (48271 * x) mod 2147483647
    s_add = (s_add + x) mod 2147483647
    s_xor = s_xor XOR x
    s_mul = (s_mul * 33 + (x mod 97)) mod 1000003
    s_div = s_div + idiv(x, 1000)
print "MB03 " + s_add + " " + s_xor + " " + s_mul + " " + s_div
```

**Input data:** generated inline by the frozen generator (§2.3) seeded with 20263917; no arrays.

**Workload size:** 120000000 iterations × 5 statements.

**Representability:** `48271*x <= 1.0367e14`; `s_add < 2^31`; `s_xor < 2^31` (XOR of values `< 2^31`);
`s_mul*33 + 96 <= 3.3e7`; `s_div <= 120000000 * 2147483 = 2.577e14 < 2^53`. ✔

**Repetitions inside the program:** 1.

**Expected output (exact):**

```
MB03 384434300 1778138624 469719 128855350175186
```

Field order: `s_add s_xor s_mul s_div`.

**Correctness criterion:** exact string match of all four integer fields.

**Workload-specific fairness notes:** all five statements must appear, in this order, with these
constants. No hand-written Barrett/Montgomery reduction, no replacing `mod 2147483647` with shift/add
tricks in source, no replacing `idiv(x,1000)` with a multiply-shift in source. Whatever the compiler does
with the constant divisors is the compiler's normal behaviour and is allowed for all. TypeScript uses
`^` directly (both operands are `< 2^31`, so the int32 conversion is exact and the result non-negative)
and `Math.floor(x/1000)` for `idiv`.

---

### MB-04 — Floating-point arithmetic

**Category:** floating-point arithmetic. **Purpose:** mixed FP add/sub/multiply/divide/sqrt throughput.

**Algorithm (pinned):**

```
M = 4000 ; R = 75000
g = generator(seed = 20264917)
float A[M], B[M]
for i = 0..M-1:  A[i] = 0.5 + g.next_unit()        // fill A completely first
for i = 0..M-1:  B[i] = 0.5 + g.next_unit()        // then fill B
s1 = 0.0 ; s2 = 0.0 ; s3 = 0.0 ; s4 = 0.0
for r = 0..R-1:
    A[r mod M] = A[r mod M] + 1.0e-9               // anti-elimination (§2.6)
    for i = 0..M-1:
        a = A[i] ; b = B[i]
        s1 = s1 + a * b
        s2 = s2 + a / (b + 2.0)
        s3 = s3 + sqrt(a*a + b*b)
        s4 = s4 + (a - b) * (a - b)
print "MB04 s1=" + s1 + " s2=" + s2 + " s3=" + s3 + " s4=" + s4
```

All values are in (0.5, 1.5), so no accumulator suffers catastrophic cancellation and every partial sum
is positive and well-scaled. (This is why `s4` squares the difference rather than forming `a²-b²`.)

**Workload size:** 300000000 inner iterations, ≈3.0·10⁹ floating-point operations.

**Repetitions inside the program:** R = 75000 rounds of M = 4000 elements.

**Expected output:**

```
MB04 s1=2.9913088717553896e+08 s2=1.0150768957556836e+08 s3=4.3336262450718403e+08 s4=5.1877487569461450e+07
```

**Correctness criterion:** each of `s1,s2,s3,s4` within the §2.4 tolerance
(`|Δ| <= max(1e-9·|expected|, 1e-12)`; i.e. ≈0.3, 0.1, 0.43 and 0.05 absolute respectively — roughly
10⁶ times wider than the worst-case FMA-contraction difference and roughly 10⁻⁷ times narrower than any
algorithmic mistake).

**Workload-specific fairness notes:** the four accumulators must remain four separate scalars accumulated
in this order; no manual unrolling into partial sums, no Kahan summation, no reciprocal substitution for
the division, no `rsqrt` substitution, no fast-math.

---

### MB-05 — Vector inner product

**Category:** vector inner product. **Purpose:** streaming multiply-accumulate over memory.

**Algorithm (pinned):**

```
N = 2000000 ; R = 400
g = generator(seed = 20265917)
float X[N], Y[N]
for i = 0..N-1: X[i] = 0.5 + g.next_unit()         // X first
for i = 0..N-1: Y[i] = 0.5 + g.next_unit()         // then Y
total = 0.0
for r = 0..R-1:
    X[r] = X[r] + 1.0e-9                           // anti-elimination; R <= N so no wrap
    d = 0.0
    for i = 0..N-1:
        d = d + X[i] * Y[i]
    total = total + d
print "MB05 total=" + total
```

**Workload size:** 8·10⁸ multiply-accumulates; 32 MB of live data; ≈12.8 GB of memory traffic.

**Repetitions inside the program:** R = 400.

**Expected output:**

```
MB05 total=7.9979633822893977e+08
```

**Correctness criterion:** `total` within the §2.4 tolerance (≈0.8 absolute).

**Workload-specific fairness notes:** one accumulator, one straight loop, ascending index order. No
partial-sum unrolling in source, no Kahan summation, no BLAS/`dot`/`np.dot`/Accelerate, no
`zip`+`sum` replacement that changes the accumulation order, no SIMD intrinsics, no parallelism. Each
language may use its ordinary contiguous array type (`std::vector<double>`, `Vec<f64>`, `[]float64`,
`double[]`, `Float64Array`/`number[]`, `[Double]`, `DoubleArray`, Python `list` of floats or
`array('d')` — both are Python-standard and neither is a native accelerator; the implementation must use
one and record which).

---

### MB-06 — Matrix multiplication

**Category:** matrix multiplication. **Purpose:** nested-loop numeric kernel with strided access.

**Algorithm (pinned):** classical O(n³) product, flat row-major storage, `i-j-k` loop order with a scalar
inner accumulator. Flat 1-D storage is pinned so that no language's nested-array representation
(pointer-chasing vs contiguous) changes the algorithm.

```
n = 512 ; R = 3
g = generator(seed = 20266917)
float A[n*n], B[n*n], C[n*n]
for i = 0..n*n-1: A[i] = g.next_unit()             // A first
for i = 0..n*n-1: B[i] = g.next_unit()             // then B
for r = 0..R-1:
    A[r] = A[r] + 1.0e-9                           // anti-elimination
    for i = 0..n-1:
        for j = 0..n-1:
            s = 0.0
            for k = 0..n-1:
                s = s + A[i*n + k] * B[k*n + j]
            C[i*n + j] = s
sumC = 0.0
for i = 0..n*n-1: sumC = sumC + C[i]
print "MB06 sumC=" + sumC + " c_first=" + C[0] + " c_last=" + C[n*n-1]
```

**Workload size:** 3 × 512³ = 402653184 multiply-adds.

**Repetitions inside the program:** R = 3.

**Expected output:**

```
MB06 sumC=3.3557855882315896e+07 c_first=1.2459839683862613e+02 c_last=1.3319448482962017e+02
```

**Correctness criterion:** all three fields within the §2.4 tolerance.

**Workload-specific fairness notes:** the `i-j-k` order is mandatory in source — no `i-k-j`, no tiling,
no blocking, no transposition of `B`, no Strassen, no cache-oblivious recursion, no library matmul, no
parallel loops. All three matrices are flat arrays of length `n*n`.

---

### MB-07 — Sorting

**Category:** sorting. **Purpose:** comparison sort with heavy memory movement.

**Why not the standard-library sort:** every language ships a *different* sort algorithm (timsort,
introsort, pdqsort, dual-pivot quicksort, …). Comparing them would violate specification §32's
prohibition on "using a different algorithm for only one language". A single pinned algorithm is
therefore mandatory. Standard-library sorts are not measured anywhere in this suite.

**Algorithm (pinned): bottom-up (iterative) merge sort, ascending, stable, ping-pong buffers.**

```
function msort(a[0..n-1], buf[0..n-1], n):
    src = a ; dst = buf
    width = 1
    while width < n:
        lo = 0
        while lo < n:
            mid = min(lo + width, n)
            hi  = min(lo + 2*width, n)
            i = lo ; j = mid ; k = lo
            while i < mid and j < hi:
                if src[i] <= src[j]: dst[k] = src[i] ; i = i+1
                else:                dst[k] = src[j] ; j = j+1
                k = k+1
            while i < mid: dst[k] = src[i] ; i = i+1 ; k = k+1
            while j < hi:  dst[k] = src[j] ; j = j+1 ; k = k+1
            lo = lo + 2*width
        swap the roles of src and dst
        width = width * 2
    if src is not a: copy src[0..n-1] into a[0..n-1]

N = 2000000 ; R = 4
g = generator(seed = 20267917)
int src[N] ; for i = 0..N-1: src[i] = g.next_int()
int buf[N]
total = 0 ; ssum = 0 ; inv = 0
for r = 0..R-1:
    src[r] = src[r] + 1                            // anti-elimination
    a = copy of src                                // full element-wise copy, counted
    msort(a, buf, N)
    chk = 0
    for i = 0..N-1: chk = (chk * 31 + (a[i] mod 1000003)) mod 1000003
    total = (total * 7 + chk) mod 1000003
    ssum = 0
    for i = 0..N-1: ssum = ssum + a[i]
    for i = 1..N-1: if a[i-1] > a[i]: inv = inv + 1
print "MB07 " + total + " " + ssum + " " + inv
```

`chk` is order-sensitive (it detects a wrong permutation), `ssum` is order-insensitive (it detects lost or
corrupted values), and `inv` must be 0 (it proves the output is sorted).

**Workload size:** 4 sorts of 2·10⁶ 64-bit integers, 21 merge passes each.

**Representability:** elements `<= 2147483647`; `ssum <= 2·10⁶ · 2147483647 = 4.295e15 < 2^53`;
`chk*31 + 1000002 < 3.2e7`. ✔

**Expected output (exact):**

```
MB07 897576 2146922325626459 0
```

Field order: `total ssum inv`. `ssum` is the value from the **final** round.

**Correctness criterion:** exact string match of all three fields; `inv` must be `0`.

**Workload-specific fairness notes:** the pinned merge sort only. No `std::sort`, `sort.Slice`,
`Arrays.sort`, `sorted()`, `Array.prototype.sort`, `sortUnstable`, radix sort, insertion-sort cut-off,
parallel merge, or in-place rotation variant. The per-round copy from `src` into `a` is part of the
workload and must not be elided.

---

### MB-08 — Strings

**Category:** strings. **Purpose:** text construction plus character-level scanning and transformation.

All text is ASCII, so UTF-8, UTF-16 and byte representations agree; each language may operate on its
natural character sequence (`std::string`, `Vec<u8>`, `[]byte`, `char[]`, `string`+`charCodeAt`,
`[UInt8]` from `String.utf8`, `ByteArray`, `bytearray`). Using a byte/char array rather than repeatedly
indexing an opaque grapheme-clustered string is explicitly allowed and is the idiomatic choice in the
languages that need it; it is not a benchmark hack because it is available to all and changes no
semantics for ASCII.

**Algorithm (pinned):**

```
NW = 200000 ; R = 20 ; Q = 1000000007
g = generator(seed = 20268917)

// build: words separated by exactly one space, no leading/trailing space
words = []
for w = 0..NW-1:
    L = 4 + (g.next_int() mod 13)                  // length 4..16
    build a word of L characters, each = 'a' + (g.next_int() mod 26)
text = join(words, " ")                            // language's normal join / string builder
len  = length(text)

function rhash(seq, len):                          // rolling hash, order-sensitive
    h = 0
    for i = 0..len-1: h = (h * 131 + code(seq[i])) mod 1000000007
    return h

acc = 0 ; cnt_ab = 0 ; cnt_w = 0
for r = 0..R-1:
    p = 7*r + 11                                   // anti-elimination; p <= 144 < len
    if text[p] == ' ': text[p] = 'x'
    else:              text[p] = 'a' + ((code(text[p]) - code('a') + 1) mod 26)

    h1 = rhash(text, len)                                       // pass 1
    U  = new buffer of len                                      // pass 2: upper-case
    for i = 0..len-1:
        c = code(text[i])
        U[i] = (c >= 97 and c <= 122) ? (c - 32) : c
    h2 = rhash(U, len)
    V  = new buffer of len                                      // pass 3: reverse
    for i = 0..len-1: V[i] = text[len-1-i]
    h3 = rhash(V, len)
    cnt_ab = 0                                                  // pass 4: naive search
    for i = 0..len-2:
        if text[i] == 'a' and text[i+1] == 'b': cnt_ab = cnt_ab + 1
    cnt_w = 1                                                   // pass 5: word count
    for i = 0..len-1:
        if text[i] == ' ': cnt_w = cnt_w + 1
    for v in [h1, h2, h3, cnt_ab, cnt_w]:
        acc = (acc * 31 + v) mod Q
print "MB08 " + acc + " " + len + " " + cnt_ab + " " + cnt_w
```

**Workload size:** text length 2200016 characters; 20 rounds × 5 full passes ≈ 2.2·10⁸ character
operations, plus 40 buffer allocations of 2.2 MB.

**Representability:** `h*131 + 122 <= 1.31e11`; `acc*31 + 10⁹ <= 3.2e10`. ✔

**Expected output (exact):**

```
MB08 288479539 2200016 2643 199999
```

Field order: `acc len cnt_ab cnt_w`, where `cnt_ab` and `cnt_w` are the **final-round** values.
(`cnt_w` is 199999 rather than 200000 because the anti-elimination mutation converts one space to `'x'`.)

**Correctness criterion:** exact string match of all four integer fields.

**Workload-specific fairness notes:** the join in the build phase uses the language's normal string
builder or join function (this is normal idiom in every language and is the same operation everywhere).
Every one of the five per-round passes must be the explicit character loop written above: no
`toUpperCase`, no `reverse()`, no `count`/`indexOf`/`find`/`memmem`/regex for `cnt_ab`, no `split()` for
`cnt_w`, no caching of `h1`/`h2`/`h3` between rounds, no reuse of the `U` and `V` buffers between rounds
(a fresh buffer per round per the pseudocode), no rope/interning tricks.

---

### MB-09 — Statistics

**Category:** statistics. **Purpose:** numerically careful multi-pass reductions.

**Algorithm (pinned):** two-pass (stable) formulas throughout. The naive
`N·Σxy − Σx·Σy` correlation form is explicitly **not** used, because its catastrophic cancellation would
make cross-language agreement an artefact of rounding rather than of correctness.

```
N = 2000000 ; R = 30
g = generator(seed = 20269917)
float X[N], Y[N]
for i = 0..N-1: X[i] = g.next_unit() * 100.0       // X first
for i = 0..N-1: Y[i] = g.next_unit() * 100.0       // then Y
for r = 0..R-1:
    X[r] = X[r] + 1.0e-9                           // anti-elimination
    s = 0.0 ; mn = X[0] ; mx = X[0]                             // pass 1
    for i = 0..N-1:
        v = X[i] ; s = s + v
        if v < mn: mn = v
        if v > mx: mx = v
    mean = s / N
    sq = 0.0 ; ad = 0.0                                          // pass 2
    for i = 0..N-1:
        d = X[i] - mean
        sq = sq + d*d
        ad = ad + (d < 0 ? -d : d)
    var = sq / N ; sd = sqrt(var) ; mad = ad / N
    sy = 0.0                                                     // pass 3
    for i = 0..N-1: sy = sy + Y[i]
    meany = sy / N
    sxy = 0.0 ; sxx = 0.0 ; syy = 0.0                            // pass 4
    for i = 0..N-1:
        dx = X[i] - mean ; dy = Y[i] - meany
        sxy = sxy + dx*dy ; sxx = sxx + dx*dx ; syy = syy + dy*dy
    pearson = sxy / sqrt(sxx * syy)
    hist[0..63] = 0                                              // pass 5
    for i = 0..N-1:
        b = floor_to_int(X[i] * 0.64)
        if b < 0: b = 0
        if b > 63: b = 63
        hist[b] = hist[b] + 1
    hist_chk = 0
    for b = 0..63: hist_chk = hist_chk + (b+1) * hist[b]
print "MB09 mean=" + mean + " var=" + var + " sd=" + sd + " min=" + mn + " max=" + mx
      + " mad=" + mad + " pearson=" + pearson + " hist_chk=" + hist_chk
```

(The printed values are those of the final round. `var` is the population variance, divisor `N`.)

**Workload size:** 30 rounds × 5 passes over 2·10⁶ elements ≈ 3.6·10⁸ element operations.
`float` working set 32 MB.

**Representability:** `hist_chk <= 64 · 2·10⁶ = 1.28e8 < 2^53`. ✔

**Median is deliberately excluded** from this workload: computing it is dominated by sorting, which MB-07
already measures, and including it would double-weight sorting inside the micro suite. This exclusion is
language-neutral.

**Expected output:**

```
MB09 mean=4.9997218497631785e+01 var=8.3236798529694795e+02 sd=2.8850788295936525e+01 min=1.6116537161225703e-04 max=9.9999983608722673e+01 mad=2.4981874162937615e+01 pearson=-2.0535570587736901e-04 hist_chk=64996215
```

**Correctness criterion:** the seven floating-point fields within the §2.4 tolerance; `hist_chk` exact
string match. Note that `pearson` is small (≈−2.05e-4) and is compared with an absolute floor of 1e-12,
which is ≈10⁷ times tighter than a 0.01 % relative deviation; the observed cross-implementation spread
from FMA contraction on this field is below 1e-18.

**Workload-specific fairness notes:** no statistics library (`statistics`, `numpy`, Apache Commons,
Swift Numerics, …); no single-pass/Welford substitution; no combining of the passes; the histogram bin
computation must be `floor(x * 0.64)` with the two clamps, not a division or a lookup table.

---

### MB-10 — File I/O

**Category:** file I/O. **Purpose:** buffered text write, read-back, and integer formatting/parsing.

**Algorithm (pinned):**

```
N = 1000000 ; R = 3
g = generator(seed = 20270917)                    // the stream continues across rounds
sum_v = 0 ; chk = 0 ; nbytes = 0 ; lines = 0
for r = 0..R-1:
    name = "mb10_round_" + r + ".txt"             // in the process's current working directory
    open name for writing, text, ASCII, using the language's normal BUFFERED writer
    for i = 0..N-1:
        v = g.next_int()
        line = decimal(i) + " " + decimal(v) + "\n"
        write line
        nbytes = nbytes + byte_length(line)
    flush and close
    open name for reading, using the language's normal BUFFERED line reader
    idx = 0
    for each line read:
        split the line at its single space into two decimal fields
        a = parse_int(field0) ; v = parse_int(field1)
        if a != idx: fail the run
        idx = idx + 1 ; lines = lines + 1
        sum_v = (sum_v + v) mod 1000000007
        chk   = (chk * 31 + (v mod 1000003)) mod 1000003
    close
print "MB10 " + sum_v + " " + chk + " " + nbytes + " " + lines
```

The program does **not** delete the files; the harness deletes the scratch directory after the run (§5.8).

**Workload size:** 3 files, 3·10⁶ lines, 52112726 bytes written and read back per run.

**Representability:** all accumulators are modular or small. ✔

**Expected output (exact):**

```
MB10 686900499 941388 52112726 3000000
```

Field order: `sum_v chk nbytes lines`.

**Cross-language byte-identity check (additional correctness criterion):** because the line format and
the generator are pinned, `mb10_round_0.txt` must be **byte-identical for every configuration**. During
the correctness-verification run the harness records `shasum -a 256 mb10_round_0.txt` for each
configuration and requires all eleven digests to be equal. A digest mismatch is a correctness failure even
if the printed line matches.

**Workload-specific fairness notes:** normal standard-library buffered file I/O only. No `mmap`, no
`O_DIRECT`, no raw `write(2)` loops tuned per language, no writing to `/dev/null`, no pipes, no in-memory
filesystem, no skipping the read-back, no reusing the previous round's file, no binary format. The write
must actually be flushed and the file closed before it is read. Building the whole file content in memory
and issuing a single write is **not** permitted — the pseudocode writes line by line and relies on the
language's buffering.

---

### MB-11 — Collections

**Category:** collections. **Purpose:** the language's standard associative container, set, and dynamic
array under insert / update / lookup / delete / iterate.

This is the one workload whose point is the standard library's own general-purpose containers. Using each
language's own hash map is therefore *required*, not a violation of the "same algorithm" rule: the
algorithm — the sequence of container operations — is identical, and every language performs it with the
container its own standard library provides.

**Algorithm (pinned):**

```
N = 1000000 ; R = 3 ; KM = 500009 ; SM = 100003 ; Q = 1000000007
g = generator(seed = 20271917)
int keys[N] ; for i = 0..N-1: keys[i] = g.next_int()
acc = 0
for r = 0..R-1:
    keys[r] = keys[r] + 1000000                   // anti-elimination; stays < 2^31
    m = new empty map from integer to integer     // DEFAULT capacity, no reserve/presize
    for i = 0..N-1:
        k = keys[i] mod KM
        m[k] = (m contains k ? m[k] : 0) + 1
    size1 = number of entries in m
    found = 0 ; vsum = 0
    for i = 0..N-1:
        k = (keys[i] + 7) mod KM
        if m contains k: found = found + 1 ; vsum = vsum + m[k]
    mchk = 0
    for each (k, v) in m:                         // ANY iteration order
        mchk = (mchk + (k mod 1000003) * v) mod 1000003
    for i = 0, 2, 4, ... while i < N:
        k = keys[i] mod KM
        if m contains k: remove k from m
    size2 = number of entries in m
    st = new empty set of integers                // DEFAULT capacity
    for i = 0..N-1: add (keys[i] mod SM) to st
    size3 = number of elements in st
    lst = new empty dynamic array                 // DEFAULT capacity
    for i = 0..N-1: append (keys[i] mod 1000) to lst
    lsum = 0
    for v in lst: lsum = lsum + v
    for v in [size1, found, vsum, mchk, size2, size3, lsum]:
        acc = (acc * 31 + v) mod Q
print "MB11 " + acc + " " + size1 + " " + found + " " + vsum + " " + mchk + " " + size2
      + " " + size3 + " " + lsum
```

`mchk` uses only commutative accumulation (`+` under a modulus), so it is **independent of map iteration
order**. This is deliberate: Go randomises map iteration, Rust's `HashMap` order is unspecified, Python's
`dict` is insertion-ordered. An order-sensitive checksum would have been unimplementable across the
comparison set.

**Workload size:** per round, 10⁶ insert-or-update, 10⁶ lookups, 5·10⁵ deletes, 10⁶ set insertions, 10⁶
array appends, and one full map iteration; 3 rounds.

**Representability:** `(k mod 1000003) * v <= 1000002 * ~20 ≈ 2e7`; `lsum <= 10⁹`; `acc*31 + 10⁹ ≈ 3.2e10`. ✔

**Expected output (exact):**

```
MB11 563800404 431998 864363 1999889 595180 116056 100002 499371314
```

Field order: `acc size1 found vsum mchk size2 size3 lsum`, final-round values for all but `acc`.

**Correctness criterion:** exact string match of all eight integer fields.

**Workload-specific fairness notes:**

* Use the language's **standard, general-purpose** hash map, hash set, and growable array
  (`std::unordered_map`/`std::unordered_set`/`std::vector`, `HashMap`/`HashSet`/`Vec`,
  `map[int64]int64`/`map[int64]struct{}`/slice, `HashMap<Long,Long>`/`HashSet<Long>`/`ArrayList<Long>`,
  `Map`/`Set`/`Array`, `HashMap`/`HashSet`/`ArrayList` (Kotlin), `Dictionary`/`Set`/`Array` (Swift),
  `std.AutoHashMap`/`std.AutoHashMap` as a set/`std.ArrayList` (Zig), `dict`/`set`/`list` (Python).
  Java and Kotlin box their keys and values because the JDK has no primitive-keyed map; that is the
  language's real property and must not be worked around with a third-party primitive-collection library
  (Trove, fastutil, Eclipse Collections, …), which would be "outsourcing to an optimized external library
  for only one language".
* **Default construction only.** No capacity hint, no `reserve`, no `withCapacity`, no load-factor
  tuning, no custom hasher (Rust must keep the default SipHash-based `RandomState`; C++ must keep
  `std::hash`), for any language. This is fixed identically for all so that no language is credited for a
  hand-tuned container.
* Fresh containers every round (the pseudocode constructs them inside the loop); no clearing and reusing.
* **Fallback if a language has no standard hash map** (applies identically to any of the ten): implement
  the pinned reference container in-language — open addressing, linear probing, power-of-two capacity
  starting at 16, hash `h(k) = (k * 0x9E3779B1) mod 2^32` then mask, load factor 0.75, capacity doubling
  with full rehash, tombstone-free deletion by backward-shift. Record in the results that the fallback was
  used and why. The workload is **not** N/A in that case (specification §26): the language is scored on
  what it actually has.

---

### MB-00 — Startup probe (supporting measurement)

Not one of the eleven categories; defined here because §6 needs it and because it is the only honest way
to separate runtime initialisation from the workload.

**Program:** the minimal conforming program that prints exactly `HELLO` and exits 0. One per
configuration, at `micro/src/<config_id>/mb00.<ext>`, built and run with the same frozen recipe.

**Sub-probes:**

* `P1` — **program startup**: whole-process wall time of the frozen run command. For `quidra_native` this
  is `./BIN`; for `quidra_interpreter` it is `quidra run mb00.qui`; for `python` it is
  `python3 mb00.py`; for the others it is their frozen run command.
* `P2` — **REPL first-response latency**: for configurations whose frozen toolchain provides a
  first-party REPL, the whole-process wall time of feeding a single expression on stdin and letting the
  REPL reach end-of-input and exit. Timed from process spawn to process exit. The frozen commands are:

  | Configuration | Frozen `P2` command |
  |---|---|
  | `quidra_interpreter` | `printf '1+1\n' \| quidra repl` |
  | `python` | `printf '1+1\n' \| python3 -i` |
  | `typescript` | `printf '1+1\n' \| node -i` |
  | `java` | `printf '1+1\n' \| jshell -` |
  | `kotlin` | `printf '1+1\n' \| kotlinc` |
  | `swift` | `printf '1+1\n' \| swift repl` |

  Where a first-party REPL does not exist in the frozen toolchain (`cpp`, `rust`, `go`, `zig`,
  `quidra_native`), `P2` simply does not exist for that configuration; it is **not** scored as a failure
  and **not** substituted. If a listed command cannot be executed non-interactively on this host, the
  exact command, exit status and stderr are recorded and `P2` is treated as non-existent for that
  configuration, with the reason published (specification §26).

**Repetitions:** 10 timed runs per sub-probe, preceded by 1 discarded warm-up. Median is representative.

**Raw value for the Startup / REPL Latency metric:** the arithmetic mean of the available sub-probe
medians for that language. A language with no REPL is represented by `P1` alone. This rule is uniform:
it neither rewards nor punishes having a REPL, it only measures the ones that exist.

---

## 5. Measurement protocol

### 5.1 Ordering: correctness gates timing

For every (workload, configuration):

1. Build (if the configuration has a build step). A build failure is recorded and the workload is scored
   per §9.
2. Run once in `once` mode and verify the output against §4. **No timing measurement is accepted until
   the correctness gate passes.**
3. Only then perform the timed runs.

### 5.2 Program modes

Every micro program accepts one optional command-line argument:

| `argv[1]` | Meaning |
|---|---|
| absent, or `once` | Execute the workload exactly once and print the single result line. |
| `steady` | Execute the *entire* workload body 7 times in-process, printing `ITER <k> <elapsed_ns>` (k = 0…6) for each iteration to stdout, then the single result line. |

In `steady` mode each iteration **re-seeds the generator to the frozen seed and regenerates its input**,
so every iteration performs exactly the work that `once` performs and every iteration's result equals the
`once` result. The result line printed at the end therefore still satisfies §2.4 — this is an additional
correctness check and is enforced.

`elapsed_ns` is measured with the language's monotonic clock around the workload body only, excluding
process start, excluding the printing of the result line:
`std::chrono::steady_clock` (C++), `std::time::Instant` (Rust), `time.Now`/`time.Since` (Go),
`System.nanoTime()` (Java, Kotlin), `process.hrtime.bigint()` (TypeScript/node),
`time.perf_counter_ns()` (Python), `ContinuousClock`/`DispatchTime.now().uptimeNanoseconds` (Swift),
`std.time.Instant.now()` (Zig), Quidra's monotonic clock API.

**Fallback if a configuration has no monotonic clock API:** `steady` mode is omitted for it and its
steady-state raw value is the *proxy* `median(cold once wall time) − median(MB-00 P1)`, flagged
`steady_proxy = true` in the results. The proxy is defined for every language identically and is used only
when the language genuinely cannot time itself.

### 5.3 Cold measurement (whole-process)

* 1 discarded warm-up run, then **5 timed runs**, per (workload, configuration).
* Each timed run is a fresh process: `once` mode, launched by the harness.
* **Wall time** is the harness's own monotonic measurement around `subprocess` spawn→exit
  (`time.perf_counter_ns()` in the Python 3.14 harness), in seconds with 6 decimal places.
* **CPU time** and **peak RSS** come from wrapping the run as
  `/usr/bin/time -l <run command>`. On macOS, BSD `time -l` reports `maximum resident set size` **in
  bytes** (unlike GNU `time`, which reports kilobytes); the value is stored verbatim in bytes and the unit
  is recorded in the results file. `user` + `sys` from the same output is the CPU time.
  `/usr/bin/time -l` writes to stderr; the program writes its result line to stdout, so the two streams
  are captured separately and never merged.
* Median of the 5 timed runs is the representative raw value (specification §25.2). All 5 individual
  values are preserved, together with min, max, mean, sample standard deviation, median absolute
  deviation, interquartile range, and coefficient of variation.

**Cold measurement is what an un-warmed JIT looks like.** It deliberately includes process creation,
dynamic linking, class loading, bytecode interpretation before JIT compilation, runtime and GC heap
initialisation, and module resolution. It is never compared against a steady-state number: the two are
reported in separate tables and feed separate Standard metrics (§6).

### 5.4 Steady-state measurement (in-process, warmed)

* **2 processes × 7 in-process iterations** per (workload, configuration).
* The **first 2 iterations of each process are discarded as warm-up**, leaving 5 kept samples per process
  and **10 kept samples** in total.
* The **median of the 10 kept samples** is the representative steady-state raw value. All 14 measured
  iteration times (including the 4 discarded ones) are preserved, and dispersion over the 10 kept samples
  is reported exactly as in §5.3.
* Rationale for 2 warm-up iterations: HotSpot's default tiered compilation and V8's optimising tiers
  reach steady state for a single hot loop nest well within one full workload execution of ≥0.3 s at
  native speed and ≥3 s at interpreted speed; two full executions is a conservative margin. The same
  policy is applied to every language, including the AOT-compiled ones, so that no language gets a
  differently shaped measurement. The per-iteration series is preserved so that any residual warm-up
  effect is visible rather than hidden.
* Garbage collection is **not** disabled, tuned, or triggered manually anywhere. Whatever the default
  collector does during the kept iterations is part of the language's steady-state behaviour. Explicit
  `System.gc()`, `global.gc()`, `gc.collect()`, or arena resets between iterations are prohibited.

### 5.5 Compile / build measurement

For every (workload, configuration) with a build step:

1. Perform one **priming build** (not timed) so that the toolchain's own caches (Go build cache, Rust's
   incremental state, JVM class loading of `javac`/`kotlinc`) are in their ordinary warm, everyday state.
   Do not purge `~/.cache`, `GOCACHE`, or any toolchain cache: the everyday incremental rebuild is the
   thing being measured, and it is measured identically for all.
2. Delete the produced artifacts (`BIN`, `OUT/`, `FILE.js`, `FILE.jar`).
3. Run the frozen build command under the harness's monotonic timer, wrapped in `/usr/bin/time -l`.
4. Repeat steps 2–3 **5 times**; the **median** is the representative compile time. All runs and
   dispersion preserved. A build must exit 0; warnings are recorded but do not fail the build.

For configurations with no build step (`quidra_interpreter`, `python`), compile time is **exactly
`0.000000` seconds — a legitimate zero, not a missing value**. Per specification §25.1 family C this
requires the predeclared shifted normalisation, and the shift is frozen here:

```
epsilon_compile = 0.001 s        // 1 ms: the resolution below which the harness does not claim precision
Score_i = 100 * (best_raw + epsilon_compile) / (raw_i + epsilon_compile)
```

### 5.6 Artifact size measurement

Measured once per (workload, configuration), in bytes, after a successful build:

| Configuration | Artifact measured |
|---|---|
| `quidra_native` | the produced executable `BIN` |
| `cpp`, `rust`, `go`, `swift`, `zig` | the produced executable `BIN` |
| `java` | the sum of the sizes of all `.class` files under `OUT/` |
| `kotlin` | the produced `FILE.jar` (which bundles the Kotlin runtime, per the frozen recipe) |
| `typescript` | the emitted `FILE.js` |
| `python` | the `FILE.py` source (there is no build product) |
| `quidra_interpreter` | not applicable as an artifact (see §8 mode-role table) |

No stripping, no `strip`, no UPX, no size flags — the artifact exactly as the frozen recipe produces it.
Each record also carries a boolean `bundles_runtime` (true for `kotlin`, `go`, and any statically linked
executable) as **metadata only**; the raw number is never adjusted for it, because specification §7
forbids discounting a real property of a language's normal build output. Interpreter/runtime size belongs
to the separate Deployment Footprint metric, not here.

### 5.7 Serial execution and host discipline (mandatory)

* **Exactly one timed process runs at a time.** No parallel measurement, no concurrent builds, no agent
  fan-out, no LLM calls, no other benchmark phase, while a timed run is in flight.
* Before each (workload, configuration) batch the harness reads `sysctl -n vm.loadavg` and requires the
  1-minute load average to be **< 2.0**; otherwise it waits and retries, up to 10 minutes, then records
  the batch as `deferred` and moves on. The observed load average is stored with every measurement record.
* A **2-second idle pause** separates consecutive timed runs, to limit thermal coupling between runs on a
  fanless M2.
* The whole suite runs under `caffeinate -i` so the host cannot sleep mid-measurement.
* Frozen environment for every run: `LC_ALL=C`, `LANG=C`, `TZ=UTC`,
  `JAVA_HOME=/opt/homebrew/opt/openjdk`, the PATH recorded in `environment.json`, and a fixed working
  directory (§5.8). No `nice`, no `taskpolicy`, no QoS manipulation, no CPU pinning — none of which is
  available uniformly on macOS and any of which would advantage whichever language it was applied to.
* Per-process **timeout: 1800 s**. A process that exceeds it is killed; its raw value is recorded as
  `1800.0` with `timed_out = true` and `censored = true`, the ratio tables display it as `>=1800`, and the
  censored value is used for normalisation. A timeout is a real performance result, not an `N/A`.

### 5.8 Working directory and scratch data

Every run's working directory is `micro/work/<config_id>/<workload>/`, created empty before the run and
deleted after the run's outputs have been read (except during the correctness-verification run of MB-10,
where `mb10_round_0.txt` is hashed before deletion). This keeps MB-10's ~52 MB per run on the same local
APFS volume for every language.

### 5.9 Preserved raw data (specification §12, §29, §30)

Results are written as JSON lines to `results/micro/micro_raw.jsonl`, plus a derived
`results/micro/micro_summary.json` and a human-readable `results/micro/micro_summary.md`.

Per-run record (`schema: "micro_run_v1"`):

```json
{"schema":"micro_run_v1","workload":"MB-03","config":"rust","phase":"cold_once",
 "run_index":0,"wall_seconds":0.612345,"cpu_user_seconds":0.601,"cpu_sys_seconds":0.008,
 "peak_rss_bytes":2129920,"rss_unit":"bytes","exit_code":0,
 "stdout_line":"MB03 384434300 1778138624 469719 128855350175186","correct":true,
 "timed_out":false,"load_avg_1min":0.42,"started_at_utc":"2026-09-17T21:04:11Z"}
```

`phase` is one of `cold_once`, `steady`, `compile`, `startup_p1`, `startup_p2`.
Steady records additionally carry `"process_index"`, `"iteration_index"`, `"iteration_ns"` and
`"discarded_warmup": true|false`.

Per-(workload, config, phase) summary record (`schema: "micro_summary_v1"`) carries: `samples` (the full
list), `median`, `min`, `max`, `mean`, `stdev`, `mad`, `iqr`, `cv`, `n`, `correct`, `timed_out`,
`steady_proxy`, `fallback_used`, and any `na_reason`.

Raw tables are published in natural units (seconds, bytes) and are never rewritten into the
100-is-best direction (specification §29).

---

## 6. Which measurement feeds which Standard metric

Frozen mapping. Normalisation is specification §25.1 **family C** (positive, lower-is-better) applied
**per workload** across the fixed comparison set, then combined across workloads by the **unweighted
arithmetic mean** of the per-workload normalized scores (specification §25.2).

| Standard metric (§8) | Micro-suite input | Aggregation |
|---|---|---|
| **Native Execution Performance** | `cold_once` **median wall time**, per workload | family C per workload; unweighted mean over the 11 workloads |
| **Long-running Performance** | `steady` **median in-process iteration time**, per workload | family C per workload; unweighted mean over the 11 workloads |
| **Startup / REPL Latency** | MB-00 (`startup_p1`, and `startup_p2` where a REPL exists) | mean of available sub-probe medians → one raw value per language → family C |
| **Memory Efficiency** | `cold_once` **median peak RSS (bytes)**, per workload | family C per workload; unweighted mean over the 11 workloads |
| **Compile / Build Performance** | `compile` median wall time, per workload | family C with `epsilon_compile = 0.001 s` (§5.5) per workload; unweighted mean over the 11 workloads |
| **Binary / Artifact Size** | §5.6 artifact bytes, per workload | family C per workload; unweighted mean over the 11 workloads |

Explicitly **not** fed by this suite: Interactive / Interpreter Performance (fed by the dedicated
Native-vs-Interpreter evidence of specification §11), Deployment Footprint, Runtime Overhead, and every
non-performance metric.

**The cold/steady separation required by specification §12 is structural, not advisory:** Native
Execution Performance is computed *only* from whole-process cold runs and Long-running Performance *only*
from warmed in-process iterations. No table ever places a cold number and a steady number in the same
column, and every published table names which of the two it contains.

**Family-C compression reporting (specification §25.1).** Micro-benchmark wall times across ten languages
are expected to span two or more orders of magnitude. Wherever a workload's applicable raw values span a
factor of 100 or more, the results must publish, beside the normalized score: the raw value in seconds for
every language, the ratio `raw_i / best_positive_raw` for every language, and the fixed note that the
normalized score is compressed and that the ratios carry the comparison between the non-leading
languages. This is required, not optional, and it does not change the formula.

---

## 7. Implementation, verification, and provenance

### 7.1 Who writes the implementations

The eleven micro programs per workload are **hand-written benchmark implementations**, not LLM trial
artifacts. They are not evidence for Primary Evaluation 4 and their authorship does not enter any LLM
metric. They must be reviewed against §2.5 and the per-workload notes before the correctness gate.

### 7.2 Implementation-bug policy

If an implementation fails the correctness gate, it is repaired and re-verified before any timing run.
This is legitimate for hand-written benchmark code and is *not* the prohibited practice of §32 (which
concerns manually repairing LLM-generated code and counting it as an LLM success). Every repair is
recorded in `micro/IMPLEMENTATION_LOG.md` with the reason. If a language **cannot** produce a conforming
implementation under §2.5 and the workload's pinned algorithm, that is a real capability result: see §9.

### 7.3 Provenance of the expected values

The expected values in §4 were produced by a reference implementation in C
(`micro/reference/ref.c`, built with `clang -O2 -ffp-contract=off -std=c11`) and independently
cross-checked by a second reference implementation written separately in Python
(`micro/reference/ref.py`, CPython 3.14.5), which additionally asserts at runtime that no integer value
anywhere in any workload exceeds `2^53` (§2.1). Both reference files are preserved in the run directory.
Neither reference is used for timing, and neither is one of the eleven measured configurations.

**Cross-check result (executed 2026-09-17, before any benchmark measurement):** for all eleven workloads,
every integer field is identical between the two references, and every floating-point field is identical
to all 17 printed significant digits — i.e. agreement far inside the §2.4 tolerance, between a
C implementation with FMA contraction disabled and a CPython implementation. The Python reference also
reports the largest integer magnitude reached anywhere in the suite:
`2146922325626459` (MB-07 `ssum`), which is 0.238 × 2⁵³, confirming the §2.1 representability rule
empirically as well as by construction. The two references differ structurally in MB-11 (arrays vs real
`dict`/`set`), which independently confirms that MB-11's expected output is container-implementation
independent, including its order-independent `mchk`.

---

## 8. Quidra Native / Interpreter aggregation into one language row

Specification §11 requires that Quidra be measured in both modes, that raw results keep
`Quidra Native` and `Quidra Interpreter` separate, that the final language-level table show Quidra as a
single language, and that the aggregation rule be defined **before examining benchmark results**.

**This rule was written and frozen before any micro benchmark was executed and before any Quidra timing,
in either mode, had been observed.**

### 8.1 The rule

**Step 1 — mode-role assignment.** Each Standard metric is *about* one or more execution modes. The
in-scope Quidra mode(s) per metric are fixed by this table:

| Standard metric | Quidra modes in scope | Reason |
|---|---|---|
| Native Execution Performance | Native only | The metric is defined on each language's compiled/primary execution configuration. |
| Interactive / Interpreter Performance | Interpreter only | The metric is defined on interactive/direct execution. |
| Long-running Performance | Native **and** Interpreter | A long-running Quidra program may plausibly be run either way; nothing in the metric's definition restricts it. |
| Startup / REPL Latency | Native **and** Interpreter | The metric's own name spans both, and §MB-00 already defines a sub-probe mean. |
| Compile / Build Performance | Native only | The Interpreter mode has no build step; its parse cost is preserved raw as a diagnostic. |
| Binary / Artifact Size | Native only | The Interpreter mode produces no artifact. |
| Memory Efficiency | Native **and** Interpreter | Memory use is a property of running the program, which Quidra can do in either mode. |
| Runtime Overhead | Native **and** Interpreter | Same reasoning. |
| Deployment Footprint | Native only | The shipped unit for a compiled Quidra program is the native artifact. |
| **Any other Standard metric** | Native only, **unless** the metric's definition in `prompt.md` explicitly concerns interactive, REPL, or interpreter behaviour, in which case Interpreter only | Default rule, so the table is total. |

**Step 2 — combination at the raw level.** For a metric with **one** mode in scope, Quidra's
language-level raw value *is* that mode's raw value. For a metric with **both** modes in scope, Quidra's
language-level raw value is the **unweighted arithmetic mean of the two modes' raw values in their natural
unit**, computed **per workload, before normalisation**:

```
raw_Quidra(metric, workload) = ( raw_native(metric, workload) + raw_interpreter(metric, workload) ) / 2
```

**Step 3 — normalisation.** The resulting single Quidra raw value enters the fixed comparison set exactly
like any other language's raw value. `best_positive_raw` for family C is computed over the ten
language-level values (Quidra's being the value from Step 2), never over eleven.

**Step 4 — disclosure.** Every final table cell for Quidra carries a footnote naming its mode
composition (`N`, `I`, or `N+I mean`). The detailed raw tables continue to list `Quidra Native` and
`Quidra Interpreter` as separate rows, per specification §29.

**Step 5 — failure handling.** If one Quidra mode fails a workload's correctness gate while the other
passes, the failing mode contributes its failure per §9 and the metric's Quidra raw for that workload is
taken from the passing mode alone, with the substitution recorded. If both fail, §9 applies.

### 8.2 Why this rule, on neutral grounds

* **It is role-based, not outcome-based.** Every assignment follows from what the metric is defined to
  measure in `prompt.md` §8, not from which mode is expected to do better. At the time of freezing, the
  author had no measurement of either Quidra mode on any workload.
* **It treats Quidra the way the other nine languages are already treated.** Each of the other languages
  contributes the execution configuration its frozen recipe defines, and that configuration is used for
  whichever metrics it is capable of (Python contributes no compile time; C++ contributes no REPL). Quidra
  simply has two configurations instead of one, so the same principle needs an explicit table.
* **The arithmetic mean is the neutral combiner.** Where both modes are genuinely in scope, the
  arithmetic mean of the raw values is the expected cost under the pre-registered, symmetric assumption
  that a Quidra user is equally likely to run either mode. It is symmetric in the two modes, it is
  monotone in both, and it cannot be gamed after the fact: it penalises Quidra exactly where the
  interpreter is slow and credits it exactly where the interpreter is fast. Alternatives were rejected on
  neutral grounds: **min** would let Quidra cherry-pick its better mode on every metric, which §32
  forbids; **max** would punish Quidra for offering a second mode at all; a **geometric mean** would
  compress a large native/interpreter gap and would thereby hide exactly the information specification
  §11 wants preserved; **weighted** means would require a usage-share estimate that no measurement in
  this benchmark supplies.
* **Nothing is hidden.** Both modes remain visible in every raw table, so a reader who disagrees with the
  combiner can recompute any cell from the preserved raw data.

---

## 9. Failure, N/A, and censoring policy for this suite

Applied identically to all eleven configurations.

| Situation | Classification | Effect on scoring |
|---|---|---|
| Build fails and cannot be made to conform to §2.5 | **Build failure** | That workload's performance/resource raw values are `FAIL`; the language receives **0** for that workload's normalized score in each affected metric, and the workload is excluded from `best_positive_raw`. Reason recorded. |
| Runs, exits 0, output does not match §4 (after §7.2 repair attempts) | **Silent Bug** (specification §13 wording) | Same as build failure: workload score **0**, raw preserved and marked `INVALID`, and the occurrence is recorded for the Silent Bug Resistance metric owned by the adversarial methodology. Never counted as a success. |
| Crashes, or exits non-zero | **Runtime failure** | Workload score **0**; raw preserved with the exit status and stderr. |
| Exceeds the 1800 s timeout | **Censored measurement** | Raw `1800.0 s`, `timed_out = true`; used for normalisation; displayed as `>=1800`. **Not** `N/A`. |
| The language lacks a capability the workload intentionally exercises (e.g. no standard hash map) | **Capability result, not N/A** | The §MB-11 fallback (or the workload's own stated fallback) is implemented and scored normally; the fallback is disclosed. Specification §26 forbids escaping the metric via `N/A` here. |
| A sub-probe genuinely does not exist (e.g. MB-00 `P2` for a language with no REPL) | **Not applicable** | Excluded from that language's sub-probe mean only; the metric itself is still scored from the remaining sub-probe. Reason recorded. |
| A configuration cannot time itself in-process (§5.2 fallback) | **Proxy measurement** | Steady-state raw = `median(cold) − median(MB-00 P1)`, `steady_proxy = true`, disclosed in every table that uses it. |

Every `N/A` in this suite carries a written reason in the results file (specification §26).

---

## 10. Freeze record

| Item | Value |
|---|---|
| Frozen on | 2026-09-17, before any micro-benchmark execution |
| Run id | 2026-09-17-7677581 |
| Quidra HEAD | 7677581aa8167b8f8a817a919732909fef14041d |
| Workloads | MB-01 … MB-11 (the eleven categories of specification §12) + MB-00 startup probe |
| Configurations | 11 (10 languages + Quidra Interpreter) |
| Generator | Lehmer MINSTD, `A = 48271`, `P = 2147483647`, per-workload seeds in §3 |
| Floating-point tolerance | `\|Δ\| <= max(1e-9·\|expected\|, 1e-12)` |
| Cold repetitions | 1 warm-up (discarded) + 5 timed; median representative |
| Steady repetitions | 2 processes × 7 iterations; first 2 of each discarded; median of 10 |
| Startup repetitions | 1 warm-up + 10 timed; median |
| Compile repetitions | 1 priming build + 5 timed; median |
| Per-process timeout | 1800 s |
| `epsilon_compile` | 0.001 s |
| Quidra aggregation | §8, frozen before any result was seen |

Any change to this document after the first timed measurement must be registered as a specification change
under `prompt.md` §25.1 and the affected run must publish results under both the old and the new
definition.
