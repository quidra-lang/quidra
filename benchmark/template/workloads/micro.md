# 06 — Micro Benchmark Workloads and Measurement Protocol

This document defines the reusable, language-neutral micro workload and measurement protocol for Language Quality. It is template input, not a historical run record.

The scored comparison contains exactly the ten fixed languages. Each language contributes exactly one ordinary program-execution configuration. Quidra uses the compiler/runtime built from the exact evaluated commit and its normal compiled/native path. Quidra source is maintained inside the evaluated snapshot and re-audited against that commit's compiler before measurement; reusable comparison-language source may be reused only through the template currency-audit rules.

No second Quidra execution configuration is defined or aggregated by this suite. Program startup means startup of the ordinary scored program execution path.

**Fixed language column order:**
Quidra, Python, C++, Rust, Go, Java, TypeScript, Kotlin, Swift, Zig.

---

## 1. Execution configurations

Exactly ten scored configurations are used.

| Config id | Language | Build command | Run command | Has build step |
|---|---|---|---|---|
| `quidra_native` | Quidra | evaluated-commit compiler: `quidra build FILE.qui -o BIN` | `./BIN` | yes |
| `python` | Python | — | `python3 FILE.py` | no |
| `cpp` | C++ | `clang++ -std=c++20 -O2 FILE.cpp -o BIN` | `./BIN` | yes |
| `rust` | Rust | `rustc -O FILE.rs -o BIN` | `./BIN` | yes |
| `go` | Go | `go build -o BIN FILE.go` | `./BIN` | yes |
| `java` | Java | `javac -d OUT FILE.java` | `java -cp OUT Main` | yes |
| `typescript` | TypeScript | `tsc FILE.ts` | `node FILE.js` | yes |
| `kotlin` | Kotlin | `kotlinc FILE.kt -include-runtime -d FILE.jar` | `java -jar FILE.jar` | yes |
| `swift` | Swift | `swiftc -O FILE.swift -o BIN` | `./BIN` | yes |
| `zig` | Zig | `zig build-exe -OReleaseFast FILE.zig -femit-bin=BIN` | `./BIN` | yes |

The exact installed toolchain fingerprints are recorded mechanically for every run. Quidra's compiler is built from the evaluated snapshot rather than taken from a historical benchmark artifact.

### 1.1 Declared toolchain confounds

The recipes intentionally use each language's frozen ordinary production-reasonable path; they are not assumed to expose identical optimization or runtime-checking behavior. Bounds checks, overflow behavior, JIT/AOT behavior, runtime bundling, GC, and similar properties are recorded as measured/toolchain facts and are never manually compensated in scoring.

One source program exists per (workload, language). Quidra's source files come from the evaluated snapshot (`tests/benchmark/quidra/micro`) and are never taken from the template or a previous run. The other nine languages may use validated reusable template source, but every run rebuilds, executes, validates, and measures again.

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
* **Fused multiply-add contraction within a single statement is permitted for every language equally as a
  policy, but it is not available to every language in fact.** Contraction is decided by each frozen
  toolchain, not by this document: Apple clang contracts `a*b+c` by default, Go permits it at the language
  level, while the JVM languages (Java, Kotlin) are specified to evaluate `a*b+c` strictly and *cannot*
  contract, and `rustc` does not contract by default. The permission is therefore neutral as a rule and
  **unequal in effect**, and it is recorded as a declared confound rather than described as universal:

  > Before any timed run, the harness compiles a one-line `a*b+c` probe in every configuration with that
  > configuration's frozen recipe, inspects the emitted instructions (`otool -tV` / the toolchain's own
  > disassembly, or the toolchain's documented evaluation rule) and records the boolean
  > `fp_contraction_observed` per configuration in §1.1 and §10. That boolean is published beside every
  > MB-04, MB-05, MB-06 and MB-09 result as a declared confound. It is never compensated for in scoring.

  The correctness tolerance in §2.4 is set wide enough to absorb a single contraction difference per
  operation and narrow enough to catch a genuinely different computation; the margin is stated as a
  derived bound in each workload's correctness note, not as an observation (see §7.3, which records that
  the two provenance implementations both ran **without** contraction, so no contraction spread had been
  observed at freeze time).

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

The generator is chosen because the arithmetic above is representable without wrapping or arbitrary-precision
requirements in every fixed language. No language-specific overflow behaviour is assumed by this template.
Any current-run runtime-checking behavior that affects the measured implementation is recorded as toolchain
evidence and is not compensated in scoring.

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

Binding for every workload and every language.

**These rules are symmetric as rules; the frozen recipes they bind are not symmetric in effect.** Rules 1
and 7 below forbid any language from adding or removing a flag, which means each configuration is
measured at whatever optimisation tier and run-time-checking state its current-run frozen recipe actually
uses. The comparison-language toolchain audit and Quidra's current-commit representation manifest record
those effective settings before timing. Nothing in this section claims that the ten configurations have
equivalent optimization or runtime checking. The asymmetry is disclosed, not compensated
(specification §7).

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
   `-OReleaseFast`, C++ raw indexing), that is the language's normal release behaviour and is allowed —
   and it is **recorded as a current-run declared confound**. The evaluated Quidra commit's own check behavior
   is determined from current documentation/toolchain evidence before timing. Allowed, disclosed, never
   subtracted from anyone's number.
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
that a fast natively compiled language lands in the mandated 0.3–3 s band, and to bound the cost of the
suite. **The second bound is only a bound on the two reference implementations.** CPython's worst case (7 × 46 s ≈ 324 s) is only a reference bound; any scored configuration may be slower, so the pre-registered partial-retention and iteration-reduction rules remain necessary. That gap is closed by the pre-registered
partial-retention and iteration-reduction rules of §5.4, not by an assumption about how fast any
configuration is. These reference timings are not evidence for any metric and must not be reported as
results.

² The MB-11 reference uses direct-indexed arrays rather than a hash map, because only the *values* it
produces are needed (and their independence from the container implementation is precisely what the
Python cross-check with real `dict`/`set` confirms). It is therefore not a timing reference for MB-11; a
standard hash map at this size is expected to be roughly an order of magnitude slower than the array
reference.

**Size-selection rule.** The frozen sizes target roughly the 0.3–3 s range on a representative fast
native implementation so process startup and timer resolution stay small relative to workload time while
still allowing repeated cold and steady measurements on the designated host class. MB-04 and MB-05 use
their explicitly frozen larger repetition counts because their kernels are especially amenable to
vectorization or memory bandwidth. The same size is mandatory for all ten languages; **no language ever
runs a reduced size**, and Quidra performance is not an input to workload sizing.

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
(`|Δ| <= max(1e-9·|expected|, 1e-12)`; i.e. ≈0.3, 0.1, 0.43 and 0.05 absolute respectively).
**Margin, stated as a derived bound rather than as an observation:** a contracted `a*b+c` differs from the
unfused form by at most half an ulp of the product, so over `R·M = 3·10⁸` accumulations into a sum of
magnitude ≈3·10⁸ the accumulated difference is bounded above by `3·10⁸ · ulp(3·10⁸)/2 ≈ 3·10⁸ · 3·10⁻⁸ ≈ 9`
in the worst case of systematic same-sign drift, and by ≈`sqrt(3·10⁸)·3·10⁻⁸ ≈ 5·10⁻⁴` under random-sign
accumulation. The tolerance (≈0.3 on `s1`) therefore covers the realistic case with a margin of ~10³ and
is roughly 10⁻⁷ of any algorithmic mistake (a wrong operator, a wrong accumulation order across
accumulators, or a dropped term changes these sums by ≥1 % of their value). Whether the worst-case
same-sign bound can be reached is exactly what the contraction provenance build of §7.3 is required to
determine before measurement; if a configuration's measured field falls outside the tolerance solely
because of contraction, that is a correctness failure under §2.4 and is handled by §9, not by widening
the tolerance after the fact.

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
`zip`+`sum` replacement that changes the accumulation order, no SIMD intrinsics, no parallelism.
**The array type is not the implementer's choice**: §4.12 pins exactly one contiguous double-precision
array type per configuration, because `list`-of-floats versus `array('d')` in Python, and `number[]`
versus `Float64Array` in TypeScript, change the measured time by a multiple and would make the result
depend on who wrote the program rather than on the language.

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

All text is ASCII, so UTF-8, UTF-16 and byte representations agree. **The representation is pinned per
configuration in §4.12 and is not the implementer's choice**: passes 1–4 operate on that configuration's
pinned mutable byte/character buffer, which is available to every language and changes no semantics for
ASCII.

**Pass 5 operates on the language's own `string` type, deliberately.** Four byte-level passes plus an
untimed build phase would have measured no language's string abstraction at all, while the category is
named "strings"; a workload that never touches the string type silently neutralises any real gap in it.
Pass 5 (word count) is therefore performed on a native string value `S`, constructed fresh each round
from the working buffer (the construction is part of the timed round), using that language's own ordinary
in-order character/scalar access API — a character iteration protocol, an integer index into the string,
or the string type's own scalar-sequence accessor, whichever is the ordinary spelling there — pinned per
configuration in §4.12. Reinterpreting `S` as a byte buffer for pass 5 is prohibited; passes 1–4 already
do that. The pinned APIs are all O(n) per pass in their own language, so no configuration is put into an
accidental quadratic by this rule.

**Published with the results:** for each language, the representation used for passes 1–4, the string API
used for pass 5, and whether the language offers direct character/code-point access to a string value at
all. The Strings row is reported as measuring byte-level scanning (passes 1–4) *and* one native-string
pass (pass 5), and the results note says so in those words.

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
    S = native_string(text)                                     // pass 5: word count, on the
    cnt_w = 1                                                   //   language's own string type
    for i = 0..len-1:                                           //   (§4.12 pins the access API)
        if char_at(S, i) == ' ': cnt_w = cnt_w + 1              //   in-order access, never a byte view
    for v in [h1, h2, h3, cnt_ab, cnt_w]:
        acc = (acc * 31 + v) mod Q
print "MB08 " + acc + " " + len + " " + cnt_ab + " " + cnt_w
```

**Workload size:** text length 2200016 characters; 20 rounds × 5 full passes ≈ 2.2·10⁸ character
operations, plus 40 buffer allocations of 2.2 MB and 20 native-string constructions of 2.2 MB (pass 5).
The pass-5 change does not alter any expected field: the text is ASCII and the character sequence is
identical, so `cnt_w` and `acc` are unchanged.

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
(a fresh buffer per round per the pseudocode), no rope/interning tricks. Pass 5's native string `S` is
constructed fresh each round and is not cached, interned or reused, and pass 5 must not be rewritten to
read the byte buffer.

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
which is ≈10⁷ times tighter than a 0.01 % relative deviation. **No cross-implementation contraction spread
had been observed for this field at freeze time** — both provenance implementations of §7.3 ran without
FMA contraction — so no such number is claimed here. The bound that is claimed is derived: `sxy`, `sxx`
and `syy` are each accumulated over 2·10⁶ terms, so a contraction difference of at most half an ulp per
term bounds the relative perturbation of `pearson` by ≈10⁻¹¹ under systematic same-sign drift, which is
inside the 1e-12 absolute floor only because `|pearson|` is itself ≈2·10⁻⁴. The contraction provenance
build required by §7.3 measures the actual per-field delta before any timed run; if it exceeds the
tolerance for this field, the discrepancy is recorded and resolved **before** measurement, and the
tolerance is not widened afterwards.

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
configuration and requires all ten digests to be equal. A digest mismatch is a correctness failure even
if the printed line matches.

**Workload-specific fairness notes:** normal standard-library buffered file I/O only. No `mmap`, no
`O_DIRECT`, no raw `write(2)` loops tuned per language, no writing to `/dev/null`, no pipes, no in-memory
filesystem, no skipping the read-back, no reusing the previous round's file, no binary format. The write
must actually be flushed and the file closed before it is read. Building the whole file content in memory
and issuing a single write is **not** permitted where the configuration's standard library can write
incrementally — the pseudocode writes line by line and relies on the language's buffering.

**"The language's normal buffered writer/reader" is not left to the implementer.** §4.12 names the exact
writer class, reader class and flush call per configuration, and freezes a single buffer size of
**65536 bytes** for every configuration, either by configuring the standard buffered writer to that size
where its API accepts one, or by formatting into a manual 65536-byte buffer flushed with the language's
ordinary write call where the standard library offers no configurable buffered writer (this is Node's
situation with `fs.writeSync`, and Zig 0.16's, whose `std.Io` writer requires an explicit sized buffer).

**Configurations whose standard library cannot write a file incrementally at all.** This is determined
from the current-run capability freeze for every language. For Quidra specifically, the current evaluated
commit is re-audited rather than inheriting any older file-API assumption. A configuration lacking the
incremental facility implements MB-10 with the closest ordinary standard-library whole-file facility while
preserving the same bytes and read-back semantics, and:

* the workload is **not** `N/A` and is **not** skipped: the configuration is scored on what it actually
  has (specification §26);
* its MB-10 row is published in a **clearly marked** form stating that the I/O shape differs (one
  whole-file write instead of ~800 buffered flushes, one whole-file read instead of a streaming reader),
  so that the number is not read as a like-for-like buffered-I/O comparison in either direction;
* the absence of an incremental writer and of a streaming line reader is recorded as a
  **missing-capability result in Capability Coverage** for that language, so that lacking the facility
  cannot be rewarded by whatever timing the substitute produces. This is the same treatment §4.12 and
  MB-11 apply to any other missing standard-library facility, for any of the ten languages.

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
  `std.AutoHashMap`/`std.AutoHashMap` as a set/`std.ArrayList` (Zig), `dict`/`set`/`list` (Python),
  the current-run resolved standard map/set/growable-array choice for Quidra. The exact type and, for
  configurations with an explicit allocator, the exact allocator are pinned per configuration in §4.12 —
  Zig's allocator choice alone (DebugAllocator vs SmpAllocator vs `c_allocator` vs an arena) changes this
  workload by several times at 10⁶ operations, so it is frozen there rather than left to the implementer.
  Java and Kotlin box their keys and values because the JDK has no primitive-keyed map; that is the
  language's real property and must not be worked around with a third-party primitive-collection library
  (Trove, fastutil, Eclipse Collections, …), which would be "outsourcing to an optimized external library
  for only one language".
* **Default construction only.** No capacity hint, no `reserve`, no `withCapacity`, no load-factor
  tuning, no custom hasher (Rust must keep the default SipHash-based `RandomState`; C++ must keep
  `std::hash`), for any language. This is fixed identically for all so that no language is credited for a
  hand-tuned container.
* Fresh containers every round (the pseudocode constructs them inside the loop); no clearing and reusing.
* **Fallback if a configuration has no standard hash map** (applies identically to any of the ten):
  implement the pinned reference container in-language — open addressing, linear probing, power-of-two
  capacity starting at 16, hash `h(k) = (k * 0x9E3779B1) mod 2^32` then mask, load factor 0.75, capacity
  doubling with full rehash, tombstone-free deletion by backward-shift. Record in the results that the
  fallback was used and why. The workload is **not** N/A in that case (specification §26): the
  configuration is scored on what it actually has.
  * Whether this fallback fires is established by the current-run toolchain/capability audit before timing; no prior run's capability result is reused.
  * **The fallback must not be able to reward a deficit.** The prescribed open-addressing map with a
    multiplicative hash and unboxed keys is materially *faster* than the boxed `HashMap<Long,Long>` that
    Java and Kotlin are required to keep and than the SipHash-keyed `HashMap` that Rust is required to
    keep. A configuration scored via this fallback is therefore (a) published in a separate, clearly
    marked row, (b) additionally recorded as a **missing-capability result in Capability Coverage**, and
    (c) never presented as if the language shipped the container. Otherwise §26's intent would be
    inverted: lacking a standard associative container would buy a better Collections number than having
    one.

---

### MB-00 — Startup probe (supporting measurement)

MB-00 is not one of the eleven workload categories. It exists only to measure **Startup Latency** on the same ten scored execution configurations.

**Program:** the minimal conforming program that prints exactly `HELLO` and exits 0.

**Measurement:** whole-process wall time of the ordinary scored run command for that language. Build time is excluded and remains part of Compile / Build Performance. Use the same deterministic cross-language interleaving policy as other timing cells, with exactly the frozen `W` warm-ups and `R` scored runs from `config/primary.json`.

The representative raw startup value is the median of the `R` scored process-start-to-exit measurements. Validate `HELLO` on every warm-up and scored run.

No interactive or alternate execution-path latency is part of this Primary metric.

### 4.12 Pinned data representation and API per configuration (frozen)

Specification §12 requires the algorithm, the input data and the workload size to be identical across
languages. A representation choice left to the implementer defeats that: `list`-of-floats versus
`array('d')`, `number[]` versus `Float64Array`, a Zig `DebugAllocator` versus `c_allocator`, or an
unbuffered versus a 64 KiB-buffered writer each change the measured time by a multiple, so two independent
analysts would produce different numbers for the same language. **There are no "use one and record which"
options anywhere in this suite.** The tables below are binding; a deviation is an implementation defect
under §7.2, not a permitted variant.

**Verification rule.** The nine reusable comparison-language rows below are template pins and are
currency-audited against the current installed toolchains. Quidra is deliberately different: its
representation/API choices are pinned in the evaluated snapshot's `tests/benchmark/quidra/representation.json`,
resolved from that commit's own documentation under the same language-neutral constraints, and the
quidra-audit unit validates that manifest against the snapshot before any timed run. Historical
Quidra representation/API choices are never inputs. Once the run manifest is frozen, none of these choices
may change after observing measurements.

**(a) Contiguous numeric arrays — MB-04, MB-05, MB-06, MB-09 (`float`), MB-07 and MB-11 `keys` (integer)**

| Config | binary64 array | 64-bit integer array |
|---|---|---|
| `quidra_native` | current-run frozen binary64 array from `quidra_representation.json` | current-run frozen 64-bit integer array from `quidra_representation.json` |
| `python` | `array('d')` | `array('q')` |
| `cpp` | `std::vector<double>` | `std::vector<long long>` |
| `rust` | `Vec<f64>` | `Vec<i64>` |
| `go` | `[]float64` | `[]int64` |
| `java` | `double[]` | `long[]` |
| `typescript` | `Float64Array` | `Float64Array` (exact for \|v\| ≤ 2⁵³, §2.1; `Int32Array` is not used because MB-07 elements reach 2147483647) |
| `kotlin` | `DoubleArray` | `LongArray` |
| `swift` | `[Double]` | `[Int]` |
| `zig` | `[]f64` allocated with `std.heap.c_allocator` | `[]i64` allocated with `std.heap.c_allocator` |

**(b) MB-08 — working buffer (passes 1–4) and native string API (pass 5)**

| Config | Mutable working buffer (passes 1–4) | Native string type and pass-5 access API | Does the language offer direct character access to a string value? |
|---|---|---|---|
| `quidra_native` | current-run frozen mutable byte/buffer representation | current-run frozen string type/access API | current-run capability recorded in `quidra_representation.json` |
| `python` | `bytearray` | `str`; `s[i]` | yes |
| `cpp` | `std::vector<unsigned char>` | `std::string`; `s[i]` | yes (byte) |
| `rust` | `Vec<u8>` | `String`; `.chars()` in order | no integer index; iteration only |
| `go` | `[]byte` | `string`; `for _, r := range s` | no integer character index; byte index or rune range |
| `java` | `byte[]` | `String`; `charAt(i)` | yes (UTF-16 code unit) |
| `typescript` | `Uint8Array` | `string`; `charCodeAt(i)` | yes (UTF-16 code unit) |
| `kotlin` | `ByteArray` | `String`; `s[i]` | yes (UTF-16 code unit) |
| `swift` | `[UInt8]` | `String`; `for c in s` | no integer index; `String.Index` iteration only |
| `zig` | `[]u8` (`c_allocator`) | `[]const u8` — Zig has no distinct string type; `s[i]` | byte only; no character abstraction |

The string→buffer conversion of the build phase happens **once, before the timed region**; the pass-5
buffer→string construction happens **inside** each timed round, per the MB-08 pseudocode.

**(c) MB-10 — writer, reader and buffer size.** Frozen buffer size for **every** configuration:
**65536 bytes**.

| Config | Writer | Line reader |
|---|---|---|
| `quidra_native` | current-run frozen ordinary writer or documented whole-file fallback | current-run frozen ordinary line reader or documented whole-file fallback |
| `python` | `open(name, "w", buffering=65536)` + `write` | `open(name, "r", buffering=65536)`, iterate the file object |
| `cpp` | `std::ofstream` with `rdbuf()->pubsetbuf(buf, 65536)` | `std::ifstream` (same `pubsetbuf`) + `std::getline` |
| `rust` | `BufWriter::with_capacity(65536, File::create(..))` | `BufReader::with_capacity(65536, File::open(..)).lines()` |
| `go` | `bufio.NewWriterSize(f, 65536)` | `bufio.NewReaderSize(f, 65536).ReadString('\n')` |
| `java` | `new BufferedWriter(new FileWriter(name), 65536)` | `new BufferedReader(new FileReader(name), 65536).readLine()` |
| `typescript` | manual 65536-byte `Buffer` + `fs.writeSync(fd, …)` (Node has no synchronous buffered line writer; `fs.createWriteStream` accumulates unboundedly, which §2.5 forbids) | manual 65536-byte `Buffer` + `fs.readSync` with carry-over line splitting |
| `kotlin` | `File(name).bufferedWriter(bufferSize = 65536)` | `File(name).bufferedReader(bufferSize = 65536).readLine()` |
| `swift` | manual 65536-byte `[UInt8]` + `FileHandle.write(contentsOf:)` | manual 65536-byte chunks via `FileHandle.read(upToCount:)` with carry-over line splitting |
| `zig` | `std.Io` file writer with an explicit 65536-byte buffer (Zig 0.16 API, `00_cross_language_constraints.md` §C-4) | `std.Io` file reader with an explicit 65536-byte buffer, delimiter `'\n'` |

Any configuration whose current-run capability audit lacks incremental I/O is handled by MB-10's
missing-capability rule: scored on what it has, row marked, gap recorded in Capability Coverage, never `N/A`.

**(d) MB-11 — containers and allocator**

| Config | Map | Set | Growable array | Allocator |
|---|---|---|---|---|
| `quidra_native` | current-run frozen standard map | current-run frozen standard set | current-run frozen growable array | current-run ordinary allocator/management model |
| `python` | `dict` | `set` | `list` | CPython default |
| `cpp` | `std::unordered_map<long long,long long>` with `std::hash` | `std::unordered_set<long long>` | `std::vector<long long>` | default `std::allocator` (libc malloc) |
| `rust` | `HashMap<i64,i64>` with default `RandomState` | `HashSet<i64>` | `Vec<i64>` | default global (libc malloc) |
| `go` | `map[int64]int64` | `map[int64]struct{}` | `[]int64` with `append` | Go runtime |
| `java` | `HashMap<Long,Long>` | `HashSet<Long>` | `ArrayList<Long>` | JVM heap, default GC |
| `typescript` | `Map<number,number>` | `Set<number>` | `Array` | V8 default |
| `kotlin` | `HashMap<Long,Long>` | `HashSet<Long>` | `ArrayList<Long>` | JVM heap, default GC |
| `swift` | `Dictionary<Int,Int>` | `Set<Int>` | `Array<Int>` | Swift runtime (libc malloc) |
| `zig` | `std.AutoHashMap(i64,i64)` | `std.AutoHashMap(i64,void)` | `std.ArrayList(i64)` | **`std.heap.c_allocator`** |

Zig's allocator is pinned to `std.heap.c_allocator` because it is libc `malloc`, which is also what C++'s
`std::unordered_map`, Rust's `HashMap` and Swift's `Dictionary` use by default on this host: the workload
then compares containers rather than allocators. This is a representation pin, not a tuning flag — no
capacity hint, load factor or hasher is changed for any configuration (MB-11's "default construction
only" rule stands).

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
| `steady` | Execute the *entire* workload body `K` times in-process, printing `ITER <k> <elapsed_ns>` (k = 0…K−1) for each iteration to stdout **as each iteration completes, flushed immediately**, then the single result line. `K` is 7 unless §5.4's pre-registered reduction rule lowers it for that cell; the harness passes `K` as `argv[2]`, defaulting to 7 when absent. Immediate flushing is mandatory so that a process killed at the timeout still contributes the iterations it finished (§5.4). |

In `steady` mode each iteration **re-seeds the generator to the frozen seed and regenerates its input**,
so every iteration performs exactly the work that `once` performs and every iteration's result equals the
`once` result. The result line printed at the end therefore still satisfies §2.4 — this is an additional
correctness check and is enforced.

`elapsed_ns` is measured with the language's monotonic clock around the workload body only, excluding
process start, excluding the printing of the result line:
`std::chrono::steady_clock` (C++), `std::time::Instant` (Rust), `time.Now`/`time.Since` (Go),
`System.nanoTime()` (Java, Kotlin), `process.hrtime.bigint()` (TypeScript/node),
`time.perf_counter_ns()` (Python), `ContinuousClock`/`DispatchTime.now().uptimeNanoseconds` (Swift),
`std.time.Instant.now()` (Zig), and the current-run frozen Quidra monotonic-clock API recorded in `quidra_representation.json`.

**Fallback if a configuration has no monotonic clock API:** `steady` mode is omitted for it and its
steady-state raw value is the *proxy* `median(cold once wall time) − median(MB-00 startup)`, flagged
`steady_proxy = true` in the results. The proxy is defined for every language identically and is used only
when the language genuinely cannot time itself. Toolchain capability is checked in the current run before
timing; the evaluated Quidra commit is never assumed to expose the same timing API as an older commit.

### 5.3 Cold measurement (whole-process)

* 1 discarded warm-up run, then **5 timed runs**, per (workload, configuration).
* Each timed run is a fresh process: `once` mode, launched by the harness.
* **Wall time is measured in a dedicated set of 5 unwrapped runs.** It is the harness's own monotonic
  measurement around `subprocess` spawn→exit (`time.perf_counter_ns()` in the Python 3.14 harness), in
  seconds with 6 decimal places. No `/usr/bin/time`, no shell, no wrapper of any kind is in the process
  tree of a wall-time run.
* **CPU time and peak RSS come from a separate set of 5 runs wrapped in `/usr/bin/time -l`, and the wall
  times of those wrapped runs are discarded.** On macOS, BSD `time -l` reports `maximum resident set size`
  **in bytes** (unlike GNU `time`, which reports kilobytes); the value is stored verbatim in bytes and the
  unit is recorded in the results file. `user` + `sys` from the same output is the CPU time.
  `/usr/bin/time -l` writes to stderr; the program writes its result line to stdout, so the two streams
  are captured separately and never merged.
* **MB-00 startup timing is always unwrapped.** The wrapper's own fork+exec can be the same order as the
  fastest configurations' entire startup, so wrapping could reorder them. The harness records current-run
  wrapper and bare-spawn calibration in environment/raw evidence before scored timing; those calibration
  values are diagnostic only and never copied from a historical run.
* Both sets — unwrapped and wrapped — use the same warm-up, the same ordering discipline (§5.7) and the
  same working directory (§5.8), and both are preserved in the raw file with a
  `"wrapped": true|false` field, so no reader has to guess which set a number came from.
* Median of the 5 timed runs is the representative raw value (specification §25.2). All 5 individual
  values are preserved, together with min, max, mean, sample standard deviation, median absolute
  deviation, interquartile range, and coefficient of variation.

**Cold measurement is what an un-warmed JIT looks like.** It deliberately includes process creation,
dynamic linking, class loading, bytecode interpretation before JIT compilation, runtime and GC heap
initialisation, and module resolution. It is never compared against a steady-state number: the two are
reported in separate tables and feed separate Standard metrics (§6).

### 5.4 Steady-state measurement (in-process, warmed)

* **2 processes × `K` in-process iterations** per (workload, configuration), `K = 7` unless the
  pre-registered reduction rule below lowers it.
* The **first 2 iterations of each process are discarded as warm-up**, leaving `K − 2` kept samples per
  process and `2(K − 2)` in total — **10 kept samples** at the default `K = 7`.
* The **median of the kept samples** is the representative steady-state raw value. All `2K` measured
  iteration times (including the 4 discarded ones) are preserved, and dispersion over the kept samples is
  reported exactly as in §5.3, using the §5.9 conventions.
* Rationale for 2 warm-up iterations: HotSpot's default tiered compilation and V8's optimising tiers
  reach steady state for a single hot loop nest within repeated full-workload execution; two full executions are used as the common warm-up margin. The same
  policy is applied to every language, including the AOT-compiled ones, so that no language gets a
  differently shaped measurement. The per-iteration series is preserved so that any residual warm-up
  effect is visible rather than hidden.
* Garbage collection is **not** disabled, tuned, or triggered manually anywhere. Whatever the default
  collector does during the kept iterations is part of the language's steady-state behaviour. Explicit
  `System.gc()`, `global.gc()`, `gc.collect()`, or arena resets between iterations are prohibited.

**A steady process that hits the per-process timeout keeps the data it already produced.** A `steady`
process executes the whole workload 7 times, so at the single 1800 s per-process timeout of §5.7 it is
held to one-seventh of the per-run cost that a `cold_once` run may take — and a process killed at the
timeout has usually already printed complete `ITER <k> <elapsed_ns>` lines to stdout. Discarding valid
measurements and replacing them with a censored constant is not a measurement. The frozen rule, fixed
here before any run:

1. **Partial retention.** A steady process killed at the per-process timeout contributes **every complete
   `ITER` line it emitted**. The first two per process are discarded as warm-up, as always; the remainder
   are kept samples and enter the median exactly like any other kept samples. The record carries
   `steady_partial = true`, the number of iterations completed, and the per-process timeout that stopped
   it.
2. **Censoring only when nothing survives.** Only if a process emitted **fewer than one kept sample**
   (i.e. two or fewer complete `ITER` lines) is that process's contribution recorded as censored at
   `1800.0 s` with `timed_out = true`. If both processes of a (workload, configuration) are in that state,
   the steady raw for that cell is censored at `1800.0 s`, used for normalisation, displayed as `>=1800`,
   and is **not** `N/A`.
3. **Pre-registered iteration reduction, decided by a rule rather than by a person.** Cold measurement
   precedes steady measurement for every cell (§5.1). If a cell's **cold median exceeds 257 s**
   (= 1800/7), its steady process runs `floor(1800 / cold_median)` iterations instead of 7, with a
   **minimum of 3** (2 warm-up + 1 kept) and a maximum of 7. The reduction is applied by the harness from
   the measured cold median, identically for every configuration, is recorded as
   `steady_iterations_planned` in the results, and no cell's iteration count is ever chosen by hand. A
   cell whose cold median exceeds 600 s cannot reach 3 iterations and falls to rule 2.

This rule replaces nothing else: the discarded warm-up count, the kept-sample median and the preserved
per-iteration series are unchanged.

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

5. **`compile_plus_execute` (specification §11, Native mode).** For every (workload, configuration) the
   derived field `compile_plus_execute = median(compile wall) + median(cold_once wall)` is computed and
   published in the raw tables, in seconds. For a configuration with no build step the first term is
   `0.000000`. It is a §11-required disclosure; it feeds no Standard metric and no normalisation, and it
   is never substituted for either of its two components.

For the scored configuration with no build step (`python`), compile time is **exactly
`0.000000` seconds — a legitimate zero, not a missing value**. Per specification §25.1 family C this
requires the predeclared shifted normalisation, and the shift is frozen here:

```
epsilon_compile = 0.01 s
Score_i = 100 * (best_raw + epsilon_compile) / (raw_i + epsilon_compile)
```

**Derivation of `epsilon_compile`.** Immediately before the first compile measurement, the runner
times `/usr/bin/true` 20 times with the same harness timer. Let `spawn_floor` be the median positive
wall time. Freeze

```
epsilon_compile = 10 ^ ceil(log10(spawn_floor))
```

for the remainder of that run and preserve all 20 calibration samples. The value is therefore derived
from the current measurement host before any scored compile timing, not copied from a prior run or chosen
after seeing language results.

### 5.6 Artifact size measurement

Measured once per (workload, configuration), in bytes, after a successful build:

| Configuration | Artifact measured |
|---|---|
| `quidra_native` | the produced executable `BIN` |
| `cpp`, `rust`, `go`, `swift`, `zig` | the produced executable `BIN` |
| `java` | the sum of the sizes of all `.class` files under `OUT/` |
| `kotlin` | the produced `FILE.jar` (which bundles the Kotlin runtime, per the frozen recipe) |
| `typescript` | the emitted `FILE.js` |
| `python` | **0 bytes** — there is no build product (see the build-less rule below) |

**One rule for build-less configurations.** A configuration with no build product has
**artifact size = 0 bytes: a legitimate zero, not a missing value and not a substitute measurement**,
scored with the family-C shifted form:

```
epsilon_artifact = 4096 bytes
Score_i = 100 * (best_raw + epsilon_artifact) / (raw_i + epsilon_artifact)
```

The 4096-byte shift is the frozen allocation quantum for the designated macOS measurement environment and
is verified as part of environment preflight before scored measurement. **Source bytes are never
substituted for artifact bytes**, and a build-less configuration is not `N/A` merely because it emits
no separate build product.

No stripping, no `strip`, no UPX, no size flags — the artifact exactly as the frozen recipe produces it.
Each record also carries a boolean `bundles_runtime` (true for `kotlin`, `go`, and any statically linked
executable) as **metadata only**; the raw number is never adjusted for it, because specification §7
forbids discounting a real property of a language's normal build output. There is no separate
deployment-footprint score; bundled-runtime effects remain visible in the measured artifact and in the
paired diagnostic below.

**Paired diagnostic for the Java/Kotlin recipe gap (published, non-scoring).** Kotlin's frozen recipe
emits a `-include-runtime` fat jar (~5 MB) while Java's emits bare `.class` files (~2 kB): a ~1000×
difference produced by the recipe pair rather than by the two languages. The scored numbers stand as
measured — §7 forbids discounting a real property of a normal build output — but the results must publish
beside them, as a clearly marked non-scoring diagnostic row, `kotlinc -d OUT` class-file bytes and the
bytes of the same Java classes packaged in a jar, and must state in words that **the scored Kotlin number
includes the bundled Kotlin runtime while the scored Java number excludes the JDK**.

### 5.7 Serial execution and host discipline (mandatory)

* **Exactly one timed process runs at a time.** No parallel measurement, no concurrent builds, no agent
  fan-out, no LLM calls, no other benchmark phase, while a timed run is in flight.
* Before each (workload, configuration) batch the harness reads `sysctl -n vm.loadavg` and requires the
  1-minute load average to be **< 2.0**; otherwise it waits and retries, up to 10 minutes, then records
  the batch as `deferred` and moves on. The observed load average is stored with every measurement record.
  **What a deferred batch scores:** it is re-queued and retried after the last scheduled batch, up to
  **3 times**. A batch still deferred after the final retry is recorded with
  `na_reason: "host_contention_unresolved"`, excluded from that metric's workload mean **for that language
  only** (an infrastructure `N/A` under specification §26 — the language's own behaviour was never
  observed, so there is nothing to score), and the exclusion is listed explicitly in the results file with
  the observed load averages. **A deferred batch is never scored 0 and never scored `1800.0`.** This is
  the only infrastructure exclusion in this suite, and it is distinguishable in the raw file from every
  capability-based classification in §9.
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
  **Exception for `steady` processes, per §5.4:** a killed steady process contributes every complete
  `ITER` line it emitted, and is censored at `1800.0 s` only when it produced fewer than one kept sample.
  Valid per-iteration data that has already been printed is never discarded in favour of a censored
  constant.

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
 "timed_out":false,"load_avg_1min":0.42,"started_at_utc":"<ISO-8601 current-run timestamp>"}
```

`phase` is one of `cold_once`, `steady`, `compile`, or `startup`.
Startup records contain only ordinary scored-program process-start-to-exit timing. Steady records additionally carry `"process_index"`, `"iteration_index"`, `"iteration_ns"` and `"discarded_warmup": true|false`.

Per-(workload, config, phase) summary record (`schema: "micro_summary_v1"`) carries: `samples` (the full
list), `median`, `min`, `max`, `mean`, `stdev`, `mad`, `iqr`, `cv`, `n`, `correct`, `timed_out`,
`steady_proxy`, `steady_partial`, `steady_iterations_planned`, `wrapped`, `fallback_used`,
`fp_contraction_observed`, `compile_plus_execute`, `representation` (the §4.12 cells actually used), and any `na_reason`.

**Frozen statistical conventions.** Several representative statistics are taken over even-sized sample
sets (10 kept steady samples, 10 startup runs), where "median" and "IQR" are ambiguous between libraries.
Fixed here so that two analysts compute the same number:

* `median` = the arithmetic **mean of the two central order statistics** for even `n`, the central value
  for odd `n`. The rule applies regardless of the frozen `R` count; no section assumes a hard-coded startup repetition count.
* Quartiles and `iqr` use the **linear-interpolation** method (`numpy.percentile` default, R type 7).
* `mad` = `median(|x_i − median(x)|)`, using the same median convention at both levels.
* `stdev` = the **sample** standard deviation with `ddof = 1`.
* `cv` = `stdev / mean`, reported as a dimensionless ratio.
* All of these are computed on the raw values in their natural unit, never on normalized scores.

Raw tables are published in natural units (seconds, bytes) and are never rewritten into the
100-is-best direction (specification §29).

---

## 6. Which measurement feeds which Standard metric

Normalisation uses the frozen lower-is-better family-C rule per workload where applicable, then the unweighted arithmetic mean of per-workload normalized scores.

| Standard metric | Micro-suite input | Aggregation |
|---|---|---|
| **Native Execution Performance** | `cold_once` median wall time, per workload | family C per workload; unweighted mean over the 11 workloads |
| **Long-running Performance** | `steady` median in-process iteration time, per workload | family C per workload; unweighted mean over the 11 workloads |
| **Compile / Build Performance** | compile median wall time, per workload | shifted family C using the frozen compile epsilon; unweighted mean over the 11 workloads |
| **Startup Latency** | MB-00 ordinary-program startup median | family C across the fixed ten languages |
| **Memory Efficiency** | `cold_once` median peak RSS, per MB-01…MB-11 workload | family C per workload; unweighted mean over the 11 workloads |
| **Runtime Overhead** | MB-00 ordinary-program median peak RSS | family C across the fixed ten languages |
| **Source Code Size** | source bytes, per workload | family C per workload; unweighted mean over the 11 workloads |
| **Binary / Artifact Size** | artifact bytes, per workload | shifted family C; unweighted mean over the 11 workloads |

Non-performance metrics are outside this micro measurement command. Source Code Size uses only MB-01…MB-11 source bytes; MB-00 is a supporting startup/runtime-overhead probe and never enters source-size scoring.

### 6.1 Startup Latency

For each language:

```
P_start(lang) = median ordinary-program startup wall time from MB-00
StartupLatency(lang) = 100 * min_positive(P_start) / P_start(lang)
```

All ten languages contribute one value from the same conceptual operation: launch the scored program through its ordinary run path and wait for successful exit. There is no cross-mode averaging.

### 6.2 Runtime Overhead

For each language, run MB-00 under the same wrapped peak-RSS measurement mechanism used for workload memory measurement. Use exactly the frozen warm-up and scored-run counts, validate `HELLO` on every run, and take the median scored peak RSS:

```
P_runtime(lang) = median peak RSS bytes of MB-00
RuntimeOverhead(lang) = 100 * min_positive(P_runtime) / P_runtime(lang)
```

This is baseline process/runtime memory overhead. It does not replace Memory Efficiency, which remains based only on MB-01…MB-11 workload RSS.

`compile_plus_execute = median(compile wall) + median(cold_once wall)` may be published as a non-scoring diagnostic; it is never substituted for either scored metric.

Cold and steady measurements remain structurally separate. A cold number and a warmed in-process number must never be combined into one scored raw value.

Where a workload's raw times span at least 100×, publish the raw values and `raw_i / best_positive_raw` ratios beside the normalized scores so compression by the 0–100 scale remains visible.

---

## 7. Implementation, verification, and provenance

### 7.1 Implementation ownership

The nine established comparison-language implementations are reusable template assets. They are admitted
only through the frozen reuse/currency audit and are rebuilt, executed, validated and measured again in
every run.

Quidra is different because it is the changing target. Its twelve programs and its representation manifest are maintained inside the evaluated snapshot, next to the compiler, and change only through ordinary reviewed commits. The deterministic runner's quidra-audit unit re-verifies them against the compiler built from the evaluated commit before any timed run. A run may report that a program looks non-idiomatic or violates a pin; that report is advice for a later commit, never an edit made by the run, and Quidra source is never promoted into the reusable program catalog.

These programs are benchmark fixtures, not scored LLM-generation trials. Their authorship therefore does
not enter LLM Learnability or LLM Proficiency.

### 7.2 Implementation validation and retry policy

Correctness gates timing. Comparison-language reusable programs must pass their current toolchain-currency
audit and the frozen oracle before timing. Quidra authoring is validator-gated against the compiler built
from the evaluated commit, including MB-00 and all eleven micro workloads.

The deterministic runner owns the attempt budget. A failed authoring/audit attempt is preserved and retried
up to the frozen maximum; implementer difficulty is never converted into a language defect. A workload may
receive a language/toolchain failure classification only when the preserved evidence identifies a concrete
language or toolchain limitation under the frozen rules. Exhausting the authoring budget without such
evidence is an explicit authoring/infrastructure blocker, not a score of zero.

### 7.3 Provenance of the expected values

The expected values in §4 were produced by a reference implementation in C
(`micro/reference/ref.c`, built with `clang -O2 -ffp-contract=off -std=c11`) and independently
cross-checked by a second reference implementation written separately in Python
(`micro/reference/ref.py`, CPython 3.14.5), which additionally asserts at runtime that no integer value
anywhere in any workload exceeds `2^53` (§2.1). Both reference files are preserved in the run directory.
Neither reference is used for timing, and neither is one of the ten scored configurations.

**Current-run cross-check gate.** Before scored timing, execute both preserved reference
implementations and require agreement on every exact integer field and agreement within the frozen
floating-point tolerance on every floating field. Preserve their stdout, toolchain fingerprints and
comparison result in current-run evidence. The Python reference also verifies the representability bound
used by §2.1. A failed reference cross-check blocks timing until the template/workload defect is resolved;
historical reference outputs are never accepted as current evidence.

**FMA-contraction provenance gate.** The non-contracting references do not establish the effect of contraction. A third reference build, `micro/reference/ref_fma.c` built with
`clang -O2 -ffp-contract=fast -std=c11`, is executed before the first timed run and its per-field deltas
against `ref.c` are recorded for MB-04, MB-05, MB-06 and MB-09, together with the per-configuration
`fp_contraction_observed` probe of §2.2. If any field's delta exceeds the §2.4 tolerance, the discrepancy
is resolved **before** measurement — by fixing the workload or the expected value, never by widening the
tolerance once results exist. The expected values in §4 remain those of `ref.c`.

---

## 8. Single-configuration comparison rule

Every language contributes exactly one scored execution configuration to this suite.

For Quidra, that configuration is the compiled/native program produced by the compiler built from the evaluated commit. No second Quidra mode enters raw tables, normalization, scoring, safety aggregation, or ranking, so there is no cross-mode averaging or substitution rule.

The same principle applies to all languages: a failed scored configuration is not replaced by a different execution route after results are observed.

---

## 9. Failure, N/A, and censoring policy for this suite

Applied identically to all ten scored configurations.

| Situation | Classification | Effect on scoring |
|---|---|---|
| Build fails and cannot be made conforming after the frozen attempt budget, with a cited language/toolchain limitation | **Build failure** | That workload receives 0 in affected normalized performance/resource metrics; reason and diagnostics are preserved. |
| Runs and exits 0 but output fails the frozen oracle after the attempt budget | **Implementation mismatch** | That workload receives 0 in affected metrics; raw output is preserved and marked invalid. It is not automatically a language Silent Bug unless the separate adversarial methodology independently classifies it as one. |
| Crashes or exits non-zero | **Runtime failure** | That workload receives 0; exit status and stderr are preserved. |
| Attempt budget is exhausted without a cited language/toolchain limitation | **implementation_incomplete** | Infrastructure/authoring incompleteness; preserve all attempts and do not misclassify implementer difficulty as a language defect. |
| Exceeds the frozen process timeout | **Censored measurement** | Use the frozen censoring rule; never relabel a genuine slow execution as N/A. |
| The language lacks a capability intentionally exercised by a workload | **Capability result** | Apply the workload's predeclared substitute/failure rule symmetrically; never invent a post-result escape. |
| A configuration cannot self-time in-process | **Proxy measurement** | Use the predeclared steady proxy from `median(cold) - median(MB-00 startup)`, marked explicitly. |
| Host contention remains unresolved after the frozen retry budget | **Infrastructure N/A** | Exclude only the affected cell from that language's metric workload mean and preserve every observed load/retry record. |

Every N/A requires an explicit machine-readable reason.

---

## 10. Template invariants frozen before each run

Before the manifest is frozen, the runner records the exact target commit, toolchain fingerprints, measurement host, timing counts, timeout, normalization constants, representation pins, and deterministic measurement schedule.

The reusable invariants are:

- 10 fixed languages and exactly one scored execution configuration per language;
- MB-01 … MB-11 as the frozen workload categories;
- MB-00 as ordinary-program startup only;
- Quidra source taken from the evaluated snapshot and re-audited against its compiler;
- comparison-language reusable source subject to currency audit;
- correctness before timing;
- deterministic cross-language measurement interleaving;
- preserved raw samples and no post-result metric/formula changes.

Run-specific dates, commits, measured constants, and correction histories belong in run output, not in this reusable template.

