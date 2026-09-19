# 06 — Micro Benchmark Workloads, Measurement Protocol, and Quidra Mode Aggregation

**Status: FROZEN.** Authored before any micro-benchmark measurement was taken and before any
micro-benchmark result was examined.

**Authority.** This document implements specification sections 12 (Micro Benchmarks), 11 (Quidra Native
and Interpreter Evaluation), 7 (Standard Evaluation Fairness), 25 (Scoring Methodology), 26 (N/A Policy)
and 32 (Prohibited Practices) of `prompt.md` for the run `2026-09-17-7677581`.
Where this document is silent, `prompt.md` governs. Where this document is specific, it is binding and
must not be modified after the first timed measurement.

**Fairness declaration.** One global choice in this document — the MINSTD generator of §2.3 — was made in
the knowledge that Quidra's integer arithmetic is overflow-checked and cannot express 64-bit wraparound
(`00_cross_language_constraints.md` §C-1). The same choice is independently required by Python's
arbitrary-precision integers and TypeScript's binary64 `number`, and Quidra's overflow-check cost remains
fully exposed in MB-02, MB-03, MB-07 and MB-11 and is probed directly in the specification §17 adversarial set. No
workload, size, or scoring rule was chosen with reference to Quidra. Every workload is expressible with
constructs that predate all ten languages (integer and floating-point arithmetic, arrays, loops,
recursion, functions, text files, and a general-purpose associative container). The author of this
document had no knowledge of how any language would rank on any of these workloads at the time of
writing.

**A capability a language lacks is measured, not deleted and not excused.** Where a workload exercises a
facility that some configuration's standard library does not provide — MB-10's incremental writer,
MB-11's associative container, §5.2's monotonic clock, MB-00's REPL — this document (a) names, in the
section that defines the rule, exactly which configurations the rule applies to, verified before
freezing; (b) pins what those configurations do instead; and (c) records the gap as a missing-capability
result in Capability Coverage and in a marked results row. No such gap is ever an `N/A` that escapes
scoring (specification §26), and no fallback is written so that lacking the facility could score better
than having it.

**Remediation status.** This document was corrected in response to an adversarial audit of the frozen
methodology: nineteen audit findings applied on 2026-09-17, and four further defects (three of them
introduced by those corrections, one previously unitemised) found and fixed in a verification pass on
2026-09-18. **No score has been computed from this document and no benchmark measurement of any kind has
been taken**, so both passes are pre-registration, not post-result formula selection. Every change is
itemised in the *Remediation changelog* at the end of this file, including the direction each change is
expected to move Quidra's score.

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

### 1.1 Declared toolchain confounds (published with every Performance and Memory table)

The frozen recipes are **not** at equivalent optimisation or run-time-checking settings, and this document
previously described them as symmetric. They are not. The asymmetry is a real property of the recipe pair,
it is not compensated in scoring (specification §7 forbids discounting a language's real toolchain
property), and it is therefore **disclosed** instead. Every results table for Native Execution
Performance, Long-running Performance, Memory Efficiency and Binary / Artifact Size must reference this
table by name.

| Config | Optimisation tier as invoked | Array bounds checks at run time | Integer-overflow checks at run time | LTO | Is this the language's ordinary production build? |
|---|---|---|---|---|---|
| `quidra_native` | `quidra build` default = optimised native build via the Clang driver (`--debug` is the separate unoptimised tier) | on (always, not removable by a build flag) | on (always; overflow is a runtime error) | link-time dead stripping on by default; no explicit LTO flag | yes — the default build is the shipped build |
| `quidra_interpreter` | none (no build step) | on | on | n/a | yes for direct execution |
| `python` | none (bytecode, default) | on (`IndexError`) | not applicable (arbitrary precision) | n/a | yes |
| `cpp` | `-O2` (not `-O3`) | **off** (raw indexing; out-of-range is UB) | **off** (signed overflow is UB) | off | typical; `-O3` and LTO are also common in production |
| `rust` | `rustc -O` = **opt-level 2** | on | **off** (release wraps; `overflow-checks` default off) | off | **no** — `cargo build --release` is opt-level 3; recorded as a documented handicap (see below) |
| `go` | single tier; no optimisation flag exists | on | **off** (two's-complement wrap) | n/a | yes |
| `java` | `javac` default + HotSpot tiered JIT at run time | on (JVM) | **off** (two's-complement wrap) | n/a | yes |
| `typescript` | `tsc` default emit + V8 JIT | reads out of range yield `undefined` rather than trapping | not applicable (binary64; silent precision loss) | n/a | yes |
| `kotlin` | `kotlinc -include-runtime` + HotSpot tiered JIT | on (JVM) | **off** (wrap) | n/a | yes (fat jar is a normal distribution form) |
| `swift` | `swiftc -O` | on (`-Ounchecked`, which removes them, is **not** used) | on (arithmetic traps unless `&+` is written) | off | yes |
| `zig` | `-OReleaseFast` — the **checks-disabled** tier | **off** | **off** | off | one of two standard release tiers; `-OReleaseSafe` keeps both classes of check |

Three consequences are stated so that no reader has to infer them:

1. **Checked against unchecked.** `quidra_native` and `swift` run every measured workload with bounds and
   overflow checking enabled; `cpp` and `zig` run with both disabled; `rust`, `go`, `java`, `kotlin` run
   with bounds checking on and overflow checking off. Any performance difference on MB-02, MB-03, MB-05,
   MB-06, MB-07 and MB-11 therefore includes a checking-cost component that differs by configuration. This
   is disclosed as a confound; it is **not** subtracted, discounted, or otherwise adjusted for, and no
   language's number is normalised to "checks-equivalent".
2. **`rustc -O` is a handicap relative to Rust's own production profile.** The recipe is owned by
   `environment/environment.json`, which this document may not amend. The obligation on that document's
   owner is recorded in the *Remediation changelog*: either amend the Rust recipe to
   `rustc -C opt-level=3` (the `cargo build --release` equivalent) or record `rustc -O` as a published
   handicap. Until then this table records it as a handicap.
3. **FMA contraction is toolchain-determined, not universal** (§2.2). The per-configuration boolean
   `fp_contraction_observed` is added to this table by the pre-measurement probe described in §2.2 and is
   recorded in §10 before any timed run.

**No recipe was changed by this remediation, in either direction.** Disclosure is the correction; a
compensating change to any recipe would itself be a prohibited intervention under specification §32.

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
* **Fused multiply-add contraction within a single statement is permitted for every language equally as a
  policy, but it is not available to every language in fact.** Contraction is decided by each frozen
  toolchain, not by this document: Apple clang contracts `a*b+c` by default, Go permits it at the language
  level, while the JVM languages (Java, Kotlin) are specified to evaluate `a*b+c` strictly and *cannot*
  contract, and `rustc` does not contract by default. The permission is therefore neutral as a rule and
  **unequal in effect**, and it is recorded as a declared confound rather than described as universal:

  > Before any timed run, the harness compiles a one-line `a*b+c` probe in every configuration with that
  > configuration's frozen recipe, inspects the emitted instructions (`otool -tV` / the toolchain's own
  > disassembly, or the interpreter's documented evaluation rule) and records the boolean
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

**Provenance of this choice (disclosed, not buried).** MINSTD was selected in the knowledge that Quidra's
integer arithmetic is overflow-checked and cannot express the 64-bit wraparound multiply that a
conventional LCG or xorshift generator needs (`00_cross_language_constraints.md` §C-1, which records the
verified behaviour `state * 6364136223846793005` on a `uint64` raising
`runtime error[INTEGER_OVERFLOW]`). The same constraint is independently binding for Python (arbitrary
precision, requiring explicit masking) and TypeScript (binary64 `number`, requiring `BigInt`), and Rust,
Swift and Zig would each need an explicit opt-in wrapping call. The properties listed above are what make
the chosen generator neutral *once chosen*; this paragraph records what triggered the choice, so that an
auditor reading only this file is not misled. Choosing a non-wrapping data generator does not shield any
language from overflow evaluation: overflow behaviour is probed directly in the adversarial set
(specification §17) and the cost of Quidra's always-on overflow checking is paid in full in MB-02, MB-03,
MB-07 and MB-11.

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
measured at whatever optimisation tier and run-time-checking state its frozen recipe already fixes — and
those differ (`-OReleaseFast` with both check classes off for Zig, `-O2` for C++, opt-level 2 for Rust,
always-on bounds and overflow checks for `quidra_native` and `swift`). **§1.1 tabulates that per
configuration and is the governing disclosure; nothing in this section should be read as a claim that the
eleven configurations are at equivalent settings.** The asymmetry is disclosed, not compensated
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
   and it is **recorded in §1.1 as a declared confound**, because it means those configurations are timed
   without the checks that `quidra_native` and `swift` pay for in every measured loop. Allowed, disclosed,
   never subtracted from anyone's number.
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
suite. **The second bound is only a bound on the two reference implementations.** CPython's worst case
(7 × 46 s ≈ 324 s) says nothing about a configuration whose per-run cost is unknown at freeze time —
`quidra_interpreter` above all — and a per-process timeout applied to a 7-iteration steady process bites
at one-seventh of the per-run cost that cold mode tolerates. That gap is closed by the pre-registered
partial-retention and iteration-reduction rules of §5.4, not by an assumption about how fast any
configuration is. These reference timings are not evidence for any metric and must not be reported as
results.

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
configuration and requires all eleven digests to be equal. A digest mismatch is a correctness failure even
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

**Configurations whose standard library cannot write a file incrementally at all.** Verified against the
frozen toolchain before freezing, this applies in this run to `quidra_native` and `quidra_interpreter`
only: Quidra's `file` namespace exposes whole-file `read`/`write`/`read_bytes`/`write_bytes` and no
incremental writer, no append, and no line reader (`quidra/docs/spec/language.md`, *file* namespace). All
nine other configurations have an incremental writer and a line reader, named in §4.12. Such a
configuration implements MB-10 with the facility it actually has — it formats the same 10⁶ lines into a
string/byte buffer and issues the whole-file write, and reads the file back with the whole-file read and
the language's ordinary line split — and:

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
  `map.Map<K,V>` and the corresponding standard set and growable array (Quidra). The exact type and, for
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
* **Fallback if a configuration has no standard hash map** (applies identically to any of the eleven):
  implement the pinned reference container in-language — open addressing, linear probing, power-of-two
  capacity starting at 16, hash `h(k) = (k * 0x9E3779B1) mod 2^32` then mask, load factor 0.75, capacity
  doubling with full rehash, tombstone-free deletion by backward-shift. Record in the results that the
  fallback was used and why. The workload is **not** N/A in that case (specification §26): the
  configuration is scored on what it actually has.
  * **Verified before freezing: this fallback does not fire in this run.** All eleven configurations ship
    a standard hash map, hash set and growable array (Quidra: `map.Map<K,V>`), so the clause is inert
    here. It is stated as a live rule anyway, because a rule that only ever fires for one language is not
    a rule; it is recorded as inert so that no reader mistakes it for a symmetric safeguard that is
    actually doing work.
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

* `P3` — **repeated expression latency** (specification §11, Interpreter mode): the same six
  configurations are fed a frozen script of **20 identical expressions**, one per line
  (`1+1\n` repeated 20 times, then end-of-input), through the same frozen command as `P2`. The REPL's
  per-expression wall time is recorded from the **3rd expression onward** (the first two absorb
  first-response and warm-up cost, which `P2` already measures), and the **median of those 18
  per-expression times** is the representative `P3`. Where the REPL does not emit a per-expression
  timestamp, the harness derives per-expression time by feeding the script through a pty and timestamping
  each prompt-to-prompt interval; the exact driver is recorded with the result. The non-existence rule
  below applies to `P3` exactly as to `P2`.

**Missing and unscriptable REPLs — one rule, fixed before running (specification §26):**

1. **No first-party REPL in the frozen toolchain** (`cpp`, `rust`, `go`, `zig`, and, as a mode,
   `quidra_native`): the configuration has no `P2` and no `P3` value. At the language level this is a
   **missing capability on a metric whose definition is that capability**, so it receives the
   rubric-defined worst sub-score — `S_P2 = 0` and `S_P3 = 0` (§6) — and **not** an exclusion. §26
   forbids a missing capability from escaping scoring by being labelled N/A, and excluding `P2` would
   hand a language with no REPL a metric composed entirely of a ~2 ms `exec`. The absence is published
   per language with the toolchain evidence, and the P1-only value is published beside it as a
   non-scoring diagnostic.
   **The sub-score is assigned at the language level**: a language receives 0 only when **none** of its
   frozen configurations provides a first-party REPL. Quidra's REPL lives in its Interpreter
   configuration exactly as Python's lives in `python3 -i`, so Quidra is measured, not zeroed; and a
   Quidra mode without a REPL contributes no `P2`/`P3` value at all rather than a fast one (§6.1), so
   having two modes neither dilutes nor inflates this sub-probe. `cpp`, `rust`, `go` and `zig` have no
   first-party REPL in any frozen configuration and receive 0.
2. **A REPL that exists but refuses a non-TTY stdin** must be **driven through a pty**
   (`script -q /dev/null <cmd>`, `expect`, or `unbuffer`) and its real latency recorded. Treating such a
   configuration's `P2`/`P3` as non-existent is **prohibited**: it would delete a slow number precisely
   for the languages whose REPL is both slow and unscriptable, which is outcome-dependent censoring.
3. **Pre-verification, before freezing and before any timed run:** the harness executes all six `P2`
   commands and all six `P3` scripts once, records for each the exact command line, the driver used
   (pipe or pty), the exit status and any stderr, and publishes that table in the results. A command that
   cannot be made to run even through a pty is recorded as a **toolchain failure with the evidence**, and
   the configuration is scored under rule 1 (worst sub-score), not silently dropped.
4. A language whose REPL exists only outside the frozen toolchain (a third-party REPL, an online
   playground, an IDE console) has no `P2`/`P3` here, and is scored under rule 1. This is the same
   frozen-toolchain rule that governs every other measurement in this document.

**Repetitions:** 10 timed runs per sub-probe (`P1`, `P2`, `P3`), preceded by 1 discarded warm-up. Median
is representative. `P1`, `P2` and `P3` are always measured **unwrapped** (§5.3).

**How `P1`, `P2` and `P3` become scores** is defined in §6, and only there, so that the formula cannot be
re-derived differently downstream. The previous rule — "the arithmetic mean of the available sub-probe
medians" — is **withdrawn**: averaging raw times means a configuration that has a REPL adds a larger
number into a lower-is-better mean, so having a REPL always worsened the raw, and a two-mode language
carried its `P2` at half the weight every other language carried it at. Sub-probes are now normalised
separately and the *scores* are averaged (§6).

---

### 4.12 Pinned data representation and API per configuration (frozen)

Specification §12 requires the algorithm, the input data and the workload size to be identical across
languages. A representation choice left to the implementer defeats that: `list`-of-floats versus
`array('d')`, `number[]` versus `Float64Array`, a Zig `DebugAllocator` versus `c_allocator`, or an
unbuffered versus a 64 KiB-buffered writer each change the measured time by a multiple, so two independent
analysts would produce different numbers for the same language. **There are no "use one and record which"
options anywhere in this suite.** The tables below are binding; a deviation is an implementation defect
under §7.2, not a permitted variant.

**Verification rule.** Before freezing, the harness compiles and runs a one-line probe per cell against
the frozen toolchain and records that the named type, call and buffer size exist and behave as stated. Any
cell whose exact spelling differs in the installed toolchain version is corrected **in this table, before
any timed run**, and the correction is logged in the freeze record (§10). The table is never changed after
the first timed measurement.

**(a) Contiguous numeric arrays — MB-04, MB-05, MB-06, MB-09 (`float`), MB-07 and MB-11 `keys` (integer)**

| Config | binary64 array | 64-bit integer array |
|---|---|---|
| `quidra_native`, `quidra_interpreter` | `float[]` (runtime-sized) | `int[]` (runtime-sized) |
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
| `quidra_native`, `quidra_interpreter` | `bytes` | `string`; `len(text)` and `text[i]` (returns a one-code-point `string`); built from the working buffer with the language's ordinary string-building operation | yes, by code point; there is no `char` type, so `text[i]` yields a one-code-point `string` |
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
| `quidra_native`, `quidra_interpreter` | **no incremental writer exists**: whole-file `file.write(name, content)`, content accumulated in a `string` (see MB-10's missing-capability rule) | **no streaming line reader exists**: whole-file `file.read(name)` then `split("\n")` |
| `python` | `open(name, "w", buffering=65536)` + `write` | `open(name, "r", buffering=65536)`, iterate the file object |
| `cpp` | `std::ofstream` with `rdbuf()->pubsetbuf(buf, 65536)` | `std::ifstream` (same `pubsetbuf`) + `std::getline` |
| `rust` | `BufWriter::with_capacity(65536, File::create(..))` | `BufReader::with_capacity(65536, File::open(..)).lines()` |
| `go` | `bufio.NewWriterSize(f, 65536)` | `bufio.NewReaderSize(f, 65536).ReadString('\n')` |
| `java` | `new BufferedWriter(new FileWriter(name), 65536)` | `new BufferedReader(new FileReader(name), 65536).readLine()` |
| `typescript` | manual 65536-byte `Buffer` + `fs.writeSync(fd, …)` (Node has no synchronous buffered line writer; `fs.createWriteStream` accumulates unboundedly, which §2.5 forbids) | manual 65536-byte `Buffer` + `fs.readSync` with carry-over line splitting |
| `kotlin` | `File(name).bufferedWriter(bufferSize = 65536)` | `File(name).bufferedReader(bufferSize = 65536).readLine()` |
| `swift` | manual 65536-byte `[UInt8]` + `FileHandle.write(contentsOf:)` | manual 65536-byte chunks via `FileHandle.read(upToCount:)` with carry-over line splitting |
| `zig` | `std.Io` file writer with an explicit 65536-byte buffer (Zig 0.16 API, `00_cross_language_constraints.md` §C-4) | `std.Io` file reader with an explicit 65536-byte buffer, delimiter `'\n'` |

A configuration in the top row is handled by MB-10's missing-capability rule: scored on what it has, row
marked, gap recorded in Capability Coverage, never `N/A`.

**(d) MB-11 — containers and allocator**

| Config | Map | Set | Growable array | Allocator |
|---|---|---|---|---|
| `quidra_native`, `quidra_interpreter` | `map.Map<int,int>` | `set.Set<int>` | `int[]` with `append` | language-managed |
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
`std.time.Instant.now()` (Zig), Quidra's monotonic clock API.

**Fallback if a configuration has no monotonic clock API:** `steady` mode is omitted for it and its
steady-state raw value is the *proxy* `median(cold once wall time) − median(MB-00 P1)`, flagged
`steady_proxy = true` in the results. The proxy is defined for every language identically and is used only
when the language genuinely cannot time itself.
**Verified before freezing: all eleven configurations expose a monotonic clock (Quidra: `time.now()`,
documented as monotonic), so this proxy does not fire in this run.** It is recorded as inert rather than
left to read as a live symmetric safeguard.

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
* **MB-00 `P1`, `P2` and `P3` are always unwrapped.** The wrapper's own fork+exec is a per-run constant of
  the same order as the fastest configurations' entire startup, so wrapping would reorder them. The host's
  wrapper overhead is documented anyway: the harness records the median of
  `/usr/bin/time -l /usr/bin/true` over 20 runs in the environment metadata (measured on the measurement
  host on 2026-09-17: **0.004891 s**, against **0.002992 s** for a bare `/usr/bin/true`, i.e. ≈1.9 ms of
  wrapper cost).
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
  reach steady state for a single hot loop nest well within one full workload execution of ≥0.3 s at
  native speed and ≥3 s at interpreted speed; two full executions is a conservative margin. The same
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

For configurations with no build step (`quidra_interpreter`, `python`), compile time is **exactly
`0.000000` seconds — a legitimate zero, not a missing value**. Per specification §25.1 family C this
requires the predeclared shifted normalisation, and the shift is frozen here:

```
epsilon_compile = 0.01 s
Score_i = 100 * (best_raw + epsilon_compile) / (raw_i + epsilon_compile)
```

**Derivation of `epsilon_compile` (specification §25.1 requires it to come from measurement resolution,
not from assertion).** The smallest quantity this harness can distinguish for a *build* is not the timer's
resolution — `time.perf_counter_ns()` gives microseconds — but the floor cost of spawning and reaping a
process at all, because every build command is a process. That floor was measured on the measurement host
before freezing:

```
median wall time of /usr/bin/true over 20 harness-timed runs  = 0.002992 s   (measured 2026-09-17)
epsilon_compile = that median rounded up to the next power of ten = 0.01 s
```

The harness re-measures this quantity immediately before the first compile measurement and records it in
§10. If the re-measured median rounds to a different power of ten, **the re-measured value governs and is
recorded before any compile measurement is taken**; it is never re-chosen afterwards. The previous value
(`0.001 s`, justified as "the resolution below which the harness does not claim precision") contradicted
§5.3's microsecond wall-time resolution and was not derived from any measurement; the change is decisive
for this metric (a 2 s build scores 0.05 at 1 ms and 0.50 at 10 ms) and is therefore made now, before any
result exists, rather than defended after the fact.

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
| `quidra_interpreter` | **0 bytes** — there is no build product (same rule, applied identically) |

**One rule for build-less configurations.** A configuration with no build product has
**artifact size = 0 bytes: a legitimate zero, not a missing value and not a substitute measurement**,
scored with the family-C shifted form:

```
epsilon_artifact = 4096 bytes      // the APFS allocation block size of the measurement host's volume,
                                   // i.e. the smallest artifact the filesystem can actually hold;
                                   // verified with `diskutil info /` on 2026-09-17
Score_i = 100 * (best_raw + epsilon_artifact) / (raw_i + epsilon_artifact)
```

This replaces two defects that pointed in opposite directions. Python's artifact size was the size of
`FILE.py` — which is the raw value of the **separate** Source Code Size metric, so one metric was being
measured with another's number; specification §8 requires source size, artifact size and deployment
footprint to stay separate metrics, and the substitution double-counted Python's source bytes and would
have handed Python a near-certain 100 here. One row below, the identical situation (no build product) was
resolved the opposite way, as "not applicable". Neither treatment is used now: **source bytes are never
substituted for artifact bytes for any configuration, and a build-less configuration is never `N/A`
here.**

No stripping, no `strip`, no UPX, no size flags — the artifact exactly as the frozen recipe produces it.
Each record also carries a boolean `bundles_runtime` (true for `kotlin`, `go`, and any statically linked
executable) as **metadata only**; the raw number is never adjusted for it, because specification §7
forbids discounting a real property of a language's normal build output. Interpreter/runtime size belongs
to the separate Deployment Footprint metric, not here.

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
 "timed_out":false,"load_avg_1min":0.42,"started_at_utc":"2026-09-17T21:04:11Z"}
```

`phase` is one of `cold_once`, `steady`, `compile`, `startup_p1`, `startup_p2`, `startup_p3`.
A `startup_p2` or `startup_p3` record additionally carries `"repl_driver": "pipe"|"pty"` and, for
`startup_p3`, `"expression_index"` (3…20) and `"expression_ns"`.
Steady records additionally carry `"process_index"`, `"iteration_index"`, `"iteration_ns"` and
`"discarded_warmup": true|false`.

Per-(workload, config, phase) summary record (`schema: "micro_summary_v1"`) carries: `samples` (the full
list), `median`, `min`, `max`, `mean`, `stdev`, `mad`, `iqr`, `cv`, `n`, `correct`, `timed_out`,
`steady_proxy`, `steady_partial`, `steady_iterations_planned`, `wrapped`, `fallback_used`,
`repl_driver`, `fp_contraction_observed`, `compile_plus_execute`, `representation` (the §4.12 cells
actually used), and any `na_reason`.

**Frozen statistical conventions.** Several representative statistics are taken over even-sized sample
sets (10 kept steady samples, 10 startup runs), where "median" and "IQR" are ambiguous between libraries.
Fixed here so that two analysts compute the same number:

* `median` = the arithmetic **mean of the two central order statistics** for even `n`, the central value
  for odd `n`.
* Quartiles and `iqr` use the **linear-interpolation** method (`numpy.percentile` default, R type 7).
* `mad` = `median(|x_i − median(x)|)`, using the same median convention at both levels.
* `stdev` = the **sample** standard deviation with `ddof = 1`.
* `cv` = `stdev / mean`, reported as a dimensionless ratio.
* All of these are computed on the raw values in their natural unit, never on normalized scores.

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
| **Startup / REPL Latency** | MB-00 `startup_p1` and `startup_p2` | each sub-probe normalised separately with family C across all languages, then the two **scores** averaged (§6.1) |
| **Interactive / Interpreter Performance** | MB-00 `startup_p3` **only** (`startup_p2` is already an input to Startup / REPL Latency and is not reused here — see §6.2) | family C across all languages (§6.2) |
| **Memory Efficiency** | `cold_once` **median peak RSS (bytes)**, per workload | family C per workload; unweighted mean over the 11 workloads |
| **Compile / Build Performance** | `compile` median wall time, per workload | family C with `epsilon_compile = 0.01 s` (§5.5, §10) per workload; unweighted mean over the 11 workloads |
| **Binary / Artifact Size** | §5.6 artifact bytes, per workload | family C per workload; unweighted mean over the 11 workloads |

Explicitly **not** fed by this suite: Deployment Footprint, Runtime Overhead, and every non-performance
metric. *Interactive / Interpreter Performance was previously in this list, deferred to "the dedicated
Native-vs-Interpreter evidence of specification §11" — which no document in the methodology set defines,
leaving a scored Standard metric without an owner. This document owns it, in §6.2.*

### 6.1 Startup / REPL Latency — the exact formula

Averaging **raw** sub-probe times, as this document previously did, is not a neutral aggregation on a
lower-is-better metric: `P2` is larger than `P1` by two to three orders of magnitude for every toolchain,
so adding an available `P2` into an arithmetic mean always worsens the raw, and a language contributing
two execution configurations carried its `P2` at **half** the weight every single-configuration language
carried it at. That is a score change produced by the shape of the aggregation rather than by
measurement. Sub-probes are therefore normalised first and the scores averaged:

```
P1(lang) = median of MB-00 P1 over that language's scored execution configuration(s).
           For Quidra: mean( P1(quidra_native), P1(quidra_interpreter) ) — both modes genuinely start
           programs, so both are in scope for this sub-probe (§8.1).
P2(lang) = median of MB-00 P2 for that language's first-party REPL configuration.
           For Quidra: P2(quidra_interpreter), at FULL weight. Native mode contributes no P2, exactly as
           C++ contributes none — a mode without a REPL is not averaged in as if it had an instant one.

S_P1(lang) = 100 * min_over_all_languages(P1) / P1(lang)                       // family C
S_P2(lang) = 100 * min_over_languages_that_have_a_P2(P2) / P2(lang)            // family C
S_P2(lang) = 0    if the language has no first-party REPL in the frozen toolchain (MB-00 rule 1, §26)

Startup / REPL Latency(lang) = ( S_P1(lang) + S_P2(lang) ) / 2
```

Published beside the score, for every language: `P1`, `P2`, whether a pty driver was needed, and the
P1-only value as a non-scoring diagnostic.

### 6.2 Interactive / Interpreter Performance — the exact formula

This metric is about how fast a language's interactive/direct-execution path **executes work once it is
running**, so it is computed from the one sub-probe that measures exactly that: `P3`, repeated-expression
latency (specification §11, Interpreter mode).

**`P2` is deliberately not reused here.** `P2` is spawn-to-first-response, i.e. an interpreter *start-up*
cost, and it is already a scored input to Startup / REPL Latency (§6.1). Feeding it into a second scored
Standard metric would count one measured property twice: a language that ships a fast REPL would be
credited for it in two metrics, and a language that ships no REPL would take the §26 zero twice for the
same single absence. One property, one metric. The split also matches the two metrics' own names —
`P1`/`P2` are latencies to *reach* a running program, `P3` is the speed of the running interactive path.

```
P3(lang) = median of MB-00 P3 for that language's first-party REPL configuration (repeated-expression
           latency, specification §11). For Quidra: P3(quidra_interpreter).

S_P3(lang) = 100 * min_over_languages_that_have_a_P3(P3) / P3(lang)          // family C
S_P3(lang) = 0    for a language whose frozen toolchain provides no first-party interactive execution
                  path (MB-00 rule 1)

Interactive / Interpreter Performance(lang) = S_P3(lang)
```

`P2` is still measured, still published for every configuration, and still carries the pty-driver and
exit-status evidence of MB-00 rule 3; it simply enters exactly one scored metric.

The zero for a language with no REPL is the rubric's worst score, not an `N/A`: the metric's definition
*is* the capability, and specification §26 forbids a missing capability from escaping scoring by being
labelled not-applicable. In this run that is `cpp`, `rust`, `go` and `zig`; the absence is published per
language with the toolchain evidence, so a reader can see that the zero is a capability statement and not
a measurement failure. Quidra's value comes from Interpreter mode, which is the mode this metric is about
(§8.1) — this is the metric where Quidra's interpreter cost is scored at full weight.

**`compile_plus_execute`** (§5.5 item 5) is published per (workload, configuration) as a specification
§11 disclosure. It feeds no metric here; Compile / Build Performance and Native Execution Performance
remain separately measured and separately scored, and no table sums them into a score.

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
recorded in `micro/IMPLEMENTATION_LOG.md` with the reason.

**Budget and stop rule (so that "cannot produce a conforming implementation" is not a judgement call).**
Unlimited repair with no stop rule made the branch that awards a **0** for a workload — in every metric
that workload feeds — depend on when the author gave up. Fixed:

* **At most 3 independent implementation attempts per (workload, configuration)**, each logged in
  `micro/IMPLEMENTATION_LOG.md` with its diff and its failure mode.
* A configuration is declared **non-conforming** (and scored 0 for that workload per §9) **only** when the
  failure is attributable to a **named, cited language or toolchain limitation** — quote the compiler
  diagnostic verbatim, or cite the language reference by section. Implementer difficulty, unfamiliarity or
  time pressure is never such a citation.
* If 3 attempts fail **without** such a citation, the cell is recorded as `implementation_incomplete`,
  excluded from that metric's workload mean for that language with a written reason under specification
  §26, and published in the results as an incomplete implementation — **not** scored 0. A benchmark author
  who cannot write the program has measured the author, not the language.
* The same budget, the same citation requirement and the same disposal apply to all eleven configurations,
  including both Quidra modes.

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

**Third provenance build, required before any timed run: FMA contraction.** Both references above exclude
contraction (`clang -O2 -ffp-contract=off`, and CPython, which does not contract), so **no contraction
spread has been observed by this document's own provenance** — which is why §2.2, MB-04 and MB-09 no
longer claim one. A third reference build, `micro/reference/ref_fma.c` built with
`clang -O2 -ffp-contract=fast -std=c11`, is executed before the first timed run and its per-field deltas
against `ref.c` are recorded for MB-04, MB-05, MB-06 and MB-09, together with the per-configuration
`fp_contraction_observed` probe of §2.2. If any field's delta exceeds the §2.4 tolerance, the discrepancy
is resolved **before** measurement — by fixing the workload or the expected value, never by widening the
tolerance once results exist. The expected values in §4 remain those of `ref.c`.

---

## 8. Quidra Native / Interpreter aggregation into one language row

Specification §11 requires that Quidra be measured in both modes, that raw results keep
`Quidra Native` and `Quidra Interpreter` separate, that the final language-level table show Quidra as a
single language, and that the aggregation rule be defined **before examining benchmark results**.

**This rule was written and frozen before any micro benchmark was executed and before any Quidra timing,
in either mode, had been observed.**

### 8.1 The rule

**Step 1 — mode-role assignment, from a single stated discriminator.**

> **The discriminator.** Quidra's scored value for **every** Standard metric is taken from **Native**
> mode, except metrics whose definition is about interactive, REPL or interpreter execution
> (Interactive / Interpreter Performance, computed from `P3`; and the `P2` sub-probe of Startup / REPL
> Latency), which are taken from **Interpreter** mode. Interpreter results are executed and published
> **in full** as non-scoring rows for every other metric, with a native-versus-interpreter divergence
> table.

**SUPERSEDED BY C-10 — cross-document reconciliation.** The paragraph that stood here claimed this rule
matched `08_adversarial_cases.json`, quoting it as scoring Quidra "from native compiled mode" with the
interpreter as a "NON-SCORING row". **That quotation no longer describes document 08.** Document 08 was
subsequently corrected and now reads: *"Both are executed for every case-variant under identical frozen
conditions and both are SCORED; Quidra's stage score for a row is the mean of the two. DEFECT CORRECTED:
the frozen version of this rule scored Quidra from native mode alone."*

Document 08 is the authority for the adversarial/safety evidence it produces, and its corrected rule is
the stricter and more honest one: a defect reachable in **either** shipped mode is a real defect of the
language, and scoring only the mode that hides it would be exactly the pro-Quidra shape the audit was
looking for. This document therefore follows 08 rather than the reverse:

> **For every Standard metric whose evidence comes from the adversarial case set — Type Safety, Memory
> Safety, Runtime Safety, Boundary Value Safety, Adversarial Input Robustness, Early Error Detection,
> Debuggability, Silent Bug Resistance, Implementation Robustness and Diagnostics — Quidra's scored value
> is the MEAN of its native and interpreter observations, per `08_adversarial_cases.json →
> toolchain_binding.quidra_mode_rule`. Both modes are executed for every case and both are published.**

The performance/resource metrics keep the role-based assignment in the table below, which is unchanged
and was frozen before any timing was observed. The two documents now state one policy rather than two,
and the direction of the reconciliation costs Quidra rather than helping it. The previous table had **no** principled discriminator: it took both
modes for Long-running Performance, Memory Efficiency and Runtime Overhead on the ground that "nothing in
the metric's definition restricts it", while an identically-worded argument for Runtime Safety, Silent Bug
Resistance or Early Error Detection fell through to a default of "Native only" — i.e. the mode-averaging
was applied exactly where it could not expose a defect and withheld exactly where the interpreter could.

| Standard metric | Quidra mode that is **scored** | Reason under the discriminator |
|---|---|---|
| Native Execution Performance | Native | Not about interactive execution. |
| Long-running Performance | Native | Not about interactive execution. Interpreter published, non-scoring. |
| Memory Efficiency | Native | Not about interactive execution. Interpreter published, non-scoring. |
| Runtime Overhead | Native | Not about interactive execution. Interpreter published, non-scoring. |
| Compile / Build Performance | Native | Not about interactive execution; the Interpreter's parse cost is preserved raw as a diagnostic. |
| Binary / Artifact Size | Native | Not about interactive execution; Interpreter's artifact size is a legitimate 0 (§5.6), published non-scoring. |
| Deployment Footprint | Native | Not about interactive execution. |
| Startup / REPL Latency | **Split by sub-probe, not averaged across modes:** `P1` = mean of the two modes' `P1`; `P2` = Interpreter's `P2` at full weight | `P1` is about starting a program, which both modes do; `P2` is about a REPL, which only the Interpreter has. §6.1 gives the formula. |
| Interactive / Interpreter Performance | Interpreter | The metric's definition *is* interactive/interpreter execution. §6.2 gives the formula. |
| Type Safety, Memory Safety, Runtime Safety, Boundary Value Safety, Adversarial Input Robustness, Early Error Detection, Debuggability, Silent Bug Resistance, Compiler/Interpreter Robustness, Diagnostics, Tooling, and **every** other Standard metric | Native | Same discriminator, applied without exception. Interpreter behaviour is executed and published for every case, and every native/interpreter divergence appears in the divergence table required by `08_adversarial_cases.json`. |

**A divergence is not silently dropped.** Where interpreter mode reveals a behaviour that native mode does
not (a defect reachable only when interpreted, or the reverse), it is published in the divergence table
and is a first-class, citable finding in the results narrative; it simply does not re-enter the scored
cell, because every other language is likewise scored in exactly one frozen execution configuration. If
the benchmark's owner prefers the alternative policy — combining both modes on Safety/Robustness by taking
the **worse** of the two, on the ground that a defect reachable in either mode is a real defect — then
that policy must be applied to *all* metrics including Long-running, Memory and Runtime Overhead, and
`08_adversarial_cases.json` must change with it. **The two policies may not be mixed metric by metric.**

**Step 2 — combination at the raw level.** For a metric with **one** mode in scope, Quidra's
language-level raw value *is* that mode's raw value. The only place where the two modes are combined is
the `P1` sub-probe of Startup / REPL Latency, where both modes genuinely perform the measured act
(starting a program):

```
raw_Quidra(P1) = ( P1_native + P1_interpreter ) / 2
```

There is no other cross-mode averaging anywhere in this document. Raw-level averaging of a fast mode with
a slow one is not used for any performance metric, because it changes a language's score through the shape
of the aggregation rather than through measurement.

**Step 3 — normalisation.** The resulting single Quidra raw value enters the fixed comparison set exactly
like any other language's raw value. `best_positive_raw` for family C is computed over the ten
language-level values (Quidra's being the value from Step 2), never over eleven.

**Step 4 — disclosure.** Every final table cell for Quidra carries a footnote naming its mode
composition (`N`, `I`, or — for the `P1` sub-probe only — `N+I mean`). The detailed raw tables continue to
list `Quidra Native` and `Quidra Interpreter` as separate rows, per specification §29, for every metric,
including the ones where Interpreter mode is non-scoring.

**Step 5 — failure handling.** If the **scored** mode for a metric fails a workload's correctness gate,
that failure is Quidra's result for the cell and §9 applies to it. **The other mode is not substituted**:
no other language may swap in a second configuration when its frozen one fails, and allowing Quidra to do
so would let a failure in the scored configuration be repaired by a different execution mode. The
non-scored mode's outcome is published beside it, and any native/interpreter disagreement goes into the
divergence table. Where both modes are involved in a single value (`P1`), a mode that fails contributes
its failure and the sub-probe is recorded as failed, not silently halved.

### 8.2 Why this rule, on neutral grounds

* **One discriminator, applied without exception.** The rule is a single sentence that decides every
  metric — "Native, except where the metric's definition is interactive/interpreter execution" — so there
  is no metric-by-metric choice, and therefore no place for a choice to fall the convenient way. It is
  role-based, not outcome-based: at the time of freezing, and at the time of this remediation, the author
  had no measurement of either Quidra mode on any workload.
* **It treats Quidra the way the other nine languages are already treated.** Every other language is
  scored in exactly one frozen execution configuration and is credited or debited for what that
  configuration does (Python contributes no compile time; C++ contributes no REPL and is scored 0 on the
  REPL sub-probes accordingly). Quidra is scored in one configuration too, with its second mode fully
  published.
* **It matches the already-frozen sibling rule** in `08_adversarial_cases.json`, so interpreter evidence
  is handled the same way in the micro suite and in the adversarial suite, rather than being in scope in
  one document and out of scope in the other.
* **Raw-level mode averaging is withdrawn for every performance metric.** Averaging a fast mode with a
  slow one moves the language's number by an arithmetic operation, not by a measurement, and — being
  applied only where it could not surface a defect — it was doing different work on different metrics.
  Its removal **raises** Quidra's expected score on Long-running Performance, Memory Efficiency and
  Runtime Overhead relative to the previous rule; that is recorded plainly in the changelog, and the
  interpreter's real cost is now scored at full weight in the metric that is about the interpreter (§6.2)
  instead of being diluted across metrics that are not.
* **Nothing is hidden.** Both modes remain visible in every raw table, for every metric, so a reader who
  disagrees with the discriminator can recompute any cell from the preserved raw data.

---

## 9. Failure, N/A, and censoring policy for this suite

Applied identically to all eleven configurations.

| Situation | Classification | Effect on scoring |
|---|---|---|
| Build fails and cannot be made to conform to §2.5, with a cited toolchain/language limitation (§7.2) | **Build failure** | That workload's performance/resource raw values are `FAIL`; the language receives **0** for that workload's normalized score in each affected metric. **That language's raw value for that workload is not eligible to be `best_positive_raw`**, and the workload remains in every other language's per-workload set and in the unweighted mean of §6. Reason recorded. |
| Runs, exits 0, output does not match §4 (after the §7.2 attempt budget) | **Implementation mismatch** | Workload score **0** for that language, raw preserved and marked `INVALID`, recorded as `implementation_silent_mismatch` in `micro/IMPLEMENTATION_LOG.md` and published. It enters the **Silent Bug Resistance** metric **only if** `08_adversarial_cases.json`'s own classification criteria independently classify the observed behaviour as a Silent Bug for that language; otherwise it is an implementation failure of this suite and is **excluded from every Safety/Robustness metric**. These programs are written by the benchmark author, not produced by the language or by an LLM under test, so an author's arithmetic slip must not be scored against a language's safety. Never counted as a success. |
| Crashes, or exits non-zero | **Runtime failure** | Workload score **0**; raw preserved with the exit status and stderr. |
| 3 implementation attempts fail with no cited language/toolchain limitation (§7.2) | **`implementation_incomplete`** | Excluded from that metric's workload mean for that language, with a written reason under §26 and the three logged attempts published. **Not** scored 0 — the author, not the language, is what failed. |
| Exceeds the 1800 s timeout (`cold_once`, `compile`, or a `steady` process with fewer than one kept sample) | **Censored measurement** | Raw `1800.0 s`, `timed_out = true`; used for normalisation; displayed as `>=1800`. **Not** `N/A`. A `steady` process that emitted kept samples before the kill is **not** censored: §5.4 partial retention applies. |
| The language lacks a capability the workload intentionally exercises (no standard hash map, no incremental file writer) | **Capability result, never N/A** | The workload's own stated substitute is implemented and scored (specification §26: the configuration is scored on what it actually has); the substitute is disclosed in a clearly marked row **and** the gap is recorded as a missing-capability result in Capability Coverage, so lacking the facility cannot score better than having it. |
| A sub-probe of a capability-defined metric does not exist (MB-00 `P2`/`P3` for a language with no first-party REPL) | **Missing capability, scored** | Sub-score **0** for that sub-probe (§6.1, §6.2), with the toolchain evidence published. It is **not** an exclusion: on a metric whose definition is the capability, an exclusion would be exactly the `N/A` escape specification §26 forbids, and would leave the metric composed of a ~2 ms `exec`. A REPL that exists but refuses piped stdin is driven through a pty and measured (MB-00 rule 2); it is never treated as non-existent. |
| A configuration cannot time itself in-process (§5.2 fallback) | **Proxy measurement** | Steady-state raw = `median(cold) − median(MB-00 P1)`, `steady_proxy = true`, disclosed in every table that uses it. Verified inert in this run (§5.2). |
| Host contention prevents measurement after 3 retries (§5.7) | **Infrastructure N/A** | `na_reason: "host_contention_unresolved"`; excluded from that metric's workload mean for that language only, published with the observed load averages. Never 0, never `1800.0`. |

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
| Startup repetitions | 1 warm-up + 10 timed; median; `P1`, `P2`, `P3`; always unwrapped |
| Compile repetitions | 1 priming build + 5 timed; median |
| Per-process timeout | 1800 s; `steady` partial retention and iteration reduction per §5.4 |
| `epsilon_compile` | **0.01 s**, derived from the measured `/usr/bin/true` median (0.002992 s, 2026-09-17) rounded up to the next power of ten; re-measured and re-recorded here before the first compile measurement |
| `epsilon_artifact` | **4096 bytes**, the APFS allocation block size of the measurement volume (`diskutil info /`, 2026-09-17) |
| Wrapper overhead reference | `/usr/bin/time -l /usr/bin/true` median 0.004891 s vs bare 0.002992 s (2026-09-17) |
| Statistical conventions | §5.9 (median of even `n` = mean of two central order statistics; IQR = R type 7; `stdev` ddof = 1) |
| Representation pins | §4.12, verified against the frozen toolchain before freezing |
| Quidra aggregation | §8, single discriminator (Native except interactive/interpreter-defined metrics), frozen before any result was seen |
| Pre-measurement probes required | `fp_contraction_observed` per configuration (§2.2); all six `P2` and `P3` commands executed and their exit status recorded (MB-00 rule 3); `ref_fma.c` contraction provenance build (§7.3) |
| Results observed at remediation time | **none** — no micro measurement had been taken and no score computed when this document was corrected |

Any change to this document after the first timed measurement must be registered as a specification change
under `prompt.md` §25.1 and the affected run must publish results under both the old and the new
definition.

---

## Remediation changelog

**Status when these corrections were made: no micro benchmark had been executed, no Quidra timing in
either mode had been observed, and no score had been computed from this or any sibling methodology
document.** These changes are therefore pre-registration, not post-result formula selection
(`prompt.md` §32). Section numbering is unchanged; §1.1, §4.12, §6.1 and §6.2 are new subsections inside
existing sections, so every external cross-reference (`§2.4` and `§5.7` are cited by
`07_algorithm_workloads.md` and `frozen_tolerance.json`) still resolves.

Direction column: the expected effect on **Quidra's** score. "Lower" is the normal and correct outcome of
removing a pro-Quidra defect.

| # | Severity | Defect (one line) | What changed | Direction for Quidra |
|---|---|---|---|---|
| 1 | BLOCKER | Startup/REPL raw was the mean of available sub-probe **times**, so `P2` carried weight 0.25 for Quidra (two modes) against 0.5 for every other REPL-bearing language, and adding a REPL always worsened a lower-is-better raw. | MB-00's mean-of-raw rule withdrawn. §6.1 now normalises `P1` and `P2` separately (family C) and averages the **scores**. Quidra's `P1` = mean of the two modes' `P1`; Quidra's `P2` = Interpreter's `P2` at **full weight**; Native contributes no `P2`, exactly as C++ contributes none. | **LOWER** — Quidra's REPL latency now counts at the same weight as Python's, Java's, Kotlin's and Swift's. |
| 2 | BLOCKER | A missing REPL was an `N/A` excluded from the sub-probe mean (the §26 escape), and a REPL that refused piped stdin was deleted after the fact — outcome-dependent censoring that rewards a slow, unscriptable REPL. | MB-00 rules 1–4 + §9: no first-party REPL in the frozen toolchain ⇒ sub-score **0** for `P2` and `P3`, published with toolchain evidence; a REPL that refuses a non-TTY **must** be driven through a pty and measured; all six `P2`/`P3` commands are executed and their exit status recorded **before** freezing. | **HIGHER on this metric** — `cpp`, `rust`, `go`, `zig` now score 0 on the REPL sub-probes instead of being excluded. Recorded plainly: this is a §26-mandated correction that happens to help Quidra, applied because the rule is symmetric (any language could ship a REPL), and it is the auditor's specified direction. |
| 3 | BLOCKER | The default mode-role rule silently removed Quidra Interpreter from every unnamed metric — the whole 25 %-weighted Safety/Robustness category — while both-mode averaging was kept exactly where it could not expose a defect; the two stated justifications were mutually exclusive. | §8.1 now states **one** discriminator, applied to the whole table with no exception, matching `08_adversarial_cases.json`: scored value = Native, except metrics whose definition is interactive/interpreter execution (Interactive/Interpreter Performance; the `P2`/`P3` sub-probes). Interpreter published in full as non-scoring rows for every other metric with a divergence table. Cross-mode raw averaging survives **only** for the `P1` sub-probe. Step 5 no longer lets the passing mode substitute for the failing scored mode. | **MIXED.** Removing raw averaging **raises** Quidra on Long-running Performance, Memory Efficiency and Runtime Overhead; the interpreter's cost is now scored at full weight in §6.2 instead of diluted; the no-substitution rule in Step 5 **lowers** expected Quidra outcomes on failures. |
| 4 | MAJOR | Container/representation choice was left to the implementer in MB-05, MB-08, MB-10 and MB-11 ("use one and record which"), changing measured time by multiples and making the suite non-reproducible. | New **§4.12** pins, per (workload, configuration): the binary64 and 64-bit integer array types, MB-08's working buffer and pass-5 string API, MB-10's exact writer/reader class with a frozen 65536-byte buffer for every configuration, and MB-11's containers **and Zig's allocator (`std.heap.c_allocator`)**. No "record which" options remain. Every cell is verified against the frozen toolchain before freezing. | Unchanged / not Quidra-specific (reproducibility). |
| 5 | MAJOR | The 1800 s timeout is per **process**, but a steady process runs the workload 7 times, so steady mode bit at one-seventh of the cold per-run budget; complete `ITER` lines already printed were thrown away and replaced by a censored 1800.0 — and the sizing footnote bounded this only against CPython, never against `quidra_interpreter`. | §5.4: a killed steady process contributes **every complete `ITER` line**; censoring only when fewer than one kept sample survives; `ITER` lines must be flushed immediately; pre-registered iteration reduction `K = clamp(floor(1800 / cold_median), 3, 7)` for cells whose cold median exceeds 257 s, applied by the harness from measured cold data. §3 footnote corrected to say the CPython bound is a bound on CPython only. | **MIXED, mostly LOWER** — Quidra Interpreter can no longer have a slow steady measurement replaced by a censored constant that was then averaged into a scored raw; but interpreter steady values are no longer scored at all (finding 3), so the effect is confined to published non-scoring rows and to Interactive/Interpreter reporting. |
| 6 | MAJOR | Two §11-mandated measurements existed nowhere (Compile + Execute, Repeated Expression Latency), and "Interactive / Interpreter Performance" was declared out of scope here and defined nowhere else — a scored Standard metric with no owner. | MB-00 gains sub-probe **`P3`** (20 identical expressions, per-expression wall time from the 3rd on, median of 18). §5.5 item 5 adds the derived, published, non-scoring field `compile_plus_execute = median(compile) + median(cold_once)` per (workload, configuration). **§6.2** defines Interactive / Interpreter Performance from `P3` (see SR-1 below for why not from `P2` as well), with the §26 zero for languages with no interactive path. | **MIXED** — Quidra now scores on a metric it can score well on (it has a REPL), but its interpreter's repeated-expression cost is measured directly and at full weight rather than being absent. |
| 7 | MAJOR | MB-08 is the mandated "strings" category, yet all five measured passes ran on a byte buffer and the language's own string type was exercised only in the untimed build phase, neutralising any real gap in a language's string abstraction while the category name implied otherwise. | Pass 5 now runs on a **native string value**, constructed fresh inside each timed round from the working buffer, using each language's own ordinary in-order character/scalar API, pinned per configuration in §4.12. Published per language: the passes-1–4 representation, the pass-5 API, and whether the language offers direct character access at all. Expected outputs are unchanged (ASCII, identical character sequence). | **LOWER** — Quidra now pays, inside the timed region, for constructing a `string` from its working buffer and for code-point access (`text[i]` returns a one-code-point `string`; there is no `char` type), instead of that cost being invisible. |
| 8 | MAJOR | The fairness declaration claimed nothing in the document was derived from Quidra's feature set; `00_cross_language_constraints.md` §C-1 records that the MINSTD generator was chosen because 64-bit wraparound is inexpressible in Quidra. | Header declaration replaced with the accurate statement (verbatim as the audit specified), and the same cross-reference added inside §2.3 alongside the independent Python/TypeScript justification and a note that Quidra's overflow-check cost is still paid in MB-02, MB-03, MB-07, MB-11 and probed in §17. | Unchanged (disclosure), but it removes a false neutrality claim that flattered Quidra. |
| 9 | MAJOR | §5.6 measured Python's **artifact** size with its **source** bytes — the raw value of a separate metric, double-counting and near-certainly scoring Python 100 — while the identical build-less case one row below was handled as `N/A`. Kotlin's fat jar vs Java's bare `.class` files was a ~1000× recipe artefact with no paired number. | One rule for build-less configurations: artifact size = **0 bytes**, a legitimate zero, family-C shifted with a predeclared `epsilon_artifact = 4096 bytes` (APFS allocation block size, verified). Applied identically to `python` and `quidra_interpreter`; source bytes are never substituted. Kotlin/Java paired non-scoring diagnostic rows added, with an explicit results statement about the bundled Kotlin runtime. | **LOWER, slightly** — Quidra Interpreter's artifact size becomes a scored legitimate zero rather than an exclusion, and Python stops being handed an unearned 100 on a metric where Quidra Native competes. |
| 10 | MAJOR | §5.3 never said whether the 5 timed runs were the `/usr/bin/time -l`-wrapped ones; the wrapper's ~2 ms cost is the same order as the fastest configurations' entire MB-00 `P1`. | §5.3 rewritten: wall time from 5 **unwrapped** runs; CPU time and peak RSS from 5 **separate wrapped** runs whose wall times are discarded; `P1`/`P2`/`P3` always unwrapped; `"wrapped"` recorded per run; host wrapper overhead measured and recorded (0.004891 s vs 0.002992 s). | Unchanged / not Quidra-specific (reproducibility), though it removes noise that would have reordered the fast starters, `quidra_native` among them. |
| 11 | MINOR | "Median" and "IQR" over even-sized sample sets were undefined; libraries differ. | §5.9 fixes median (mean of two central order statistics), quartiles/IQR (linear interpolation, R type 7), MAD, `stdev` (ddof = 1), `cv`. | Unchanged. |
| 12 | MINOR | `epsilon_compile = 0.001 s` was justified as a resolution the harness does not claim, contradicting §5.3's microsecond wall-time resolution; §25.1 requires derivation from measurement resolution, and the value is decisive. | `epsilon_compile = 0.01 s`, derived from the measured floor cost of spawning a process (`/usr/bin/true` median 0.002992 s on the measurement host, 2026-09-17) rounded up to the next power of ten; re-measured and recorded in §10 before the first compile measurement, never re-chosen afterwards. | Unknown / not Quidra-specific — Quidra's compile time was unmeasured when this was set; the larger epsilon compresses the metric for all slow compilers symmetrically. |
| 13 | MINOR | §7.2 allowed unlimited repair with no stop rule, so the branch awarding a **0** depended on when the author gave up. | §7.2: at most 3 logged attempts per (workload, configuration); a 0 requires a **cited** language/toolchain limitation (quoted diagnostic or language-reference citation); otherwise the cell is `implementation_incomplete`, excluded with a written §26 reason, **not** 0. Applies to both Quidra modes identically. | Unchanged / symmetric. |
| 14 | MINOR | A `deferred` batch (host contention) scored nothing defined — an undefined hole in the per-workload mean. | §5.7: re-queued and retried up to 3 times; still deferred ⇒ `na_reason: "host_contention_unresolved"`, excluded for that language only as an infrastructure `N/A` under §26, published with the observed load averages, never 0 and never 1800.0. Added to the §9 table. | Unchanged. |
| 15 | MINOR | §2.2 claimed FMA contraction is "available to all" (Java/Kotlin cannot contract; Rust does not by default), and MB-04/MB-09 cited an "observed" contraction spread that neither provenance implementation could have observed. | §2.2 now states the permission is neutral as policy and unequal in effect, and requires a per-configuration `fp_contraction_observed` probe published as a declared confound. MB-04 and MB-09 margins restated as **derived ulp bounds**, with the words "observed" and "worst-case observed" removed. §7.3 adds the required third provenance build (`clang -O2 -ffp-contract=fast`) with per-field deltas recorded before measurement. | Unchanged / not Quidra-specific. |
| 16 | MINOR | The recipes were presented as symmetric while Zig runs at the checks-disabled `-OReleaseFast`, C++ at `-O2`, `rustc -O` at opt-level 2 (below cargo release), and Quidra's build tier was unstated — i.e. Quidra's always-on checking is compared against other languages' checks-disabled release modes without disclosure. | New **§1.1** publishes a per-configuration confound table (optimisation tier, bounds checks, overflow checks, LTO, whether it is that language's ordinary production build, `fp_contraction_observed`), referenced from every Performance and Memory table. Quidra's tier stated (`quidra build` default optimised; `--debug` is the separate unoptimised tier). `rustc -O` recorded as a documented handicap. **No recipe was changed in either direction.** | **LOWER in interpretation** — Quidra's results can no longer be read as a like-for-like comparison against `cpp`/`zig` numbers produced with checks disabled. |
| 17 | MINOR | Two "symmetric" fallbacks (MB-11 no-hash-map; §5.2 no-monotonic-clock) are inert for all ten incumbents, and the MB-11 fallback's hand-written unboxed open-addressing map would be **faster** than the boxed/SipHash maps other languages are required to keep — rewarding a missing capability. | Both fallbacks now state, in place, that they were verified before freezing and **do not fire in this run** (Quidra has `map.Map<K,V>`, `set.Set<T>` and `time.now()`). The MB-11 fallback is tightened: a configuration scored through it is published in a separate marked row and recorded as a **missing-capability result in Capability Coverage**, never presented as if the language shipped the container. The same tightening is applied to MB-10's newly disclosed writer gap. | **LOWER** — MB-10's rule now records Quidra's lack of an incremental file writer and streaming line reader (whole-file `file.read`/`file.write` only) as a missing capability instead of leaving it unstated; whatever timing the whole-file substitute produces cannot be read as buffered-I/O parity. |
| 18 | MINOR | §9 routed any author-written micro implementation that exited 0 with wrong output into the scored Silent Bug Resistance metric, contaminating a 25 %-weighted category with the benchmark author's own arithmetic slips. | §9 row rewritten: recorded as `implementation_silent_mismatch` and published; it enters Silent Bug Resistance **only if** `08_adversarial_cases.json`'s own criteria independently classify the behaviour as a Silent Bug for that language; otherwise excluded from every Safety/Robustness metric. | Unchanged / symmetric (it protects every language equally, Quidra included). |
| 19 | MINOR | §9 said a failed workload "is excluded from `best_positive_raw`", which is not a defined operation and could be read as dropping the workload for every language. | Rewritten: "that language's raw value for that workload is not eligible to be `best_positive_raw`, and the workload remains in every other language's per-workload set and in the unweighted mean of §6." | Unchanged. |

**Findings rejected: none.** All 19 findings were applied in the direction the audit specified; where a
finding offered two acceptable dispositions (findings 3 and 7) the stronger-measurement option was taken,
and the choice is argued above rather than asserted.

### Self-review pass (defects found while verifying the 19 corrections)

Applying the audit's fixes introduced three consistency defects of its own, and one hazard the audit did
not itemise. All four are corrected here, still before any measurement.

| # | Severity | Defect (one line) | What changed | Direction for Quidra |
|---|---|---|---|---|
| SR-1 | MAJOR | **One property scored twice.** Finding 6's fix put `P2` into Interactive / Interpreter Performance (§6.2) while finding 1's fix kept `P2` in Startup / REPL Latency (§6.1), so a single measured fact — how fast a language's REPL answers its first expression — was credited or debited in **two** scored Standard metrics. A language shipping a fast REPL was rewarded twice for it; a language shipping none (`cpp`, `rust`, `go`, `zig`) took the §26 zero twice for the same single absence, once as half of §6.1 and once as half of §6.2. That is a score change produced by reusing an input, not by measuring anything more. | §6.2 is now `S_P3` alone: Interactive / Interpreter Performance = the speed of the interactive path once running. `P2` (spawn-to-first-response, a start-up latency) stays in §6.1 and enters exactly one scored metric. `P2` is still measured, still published for all six REPL configurations with its pty-driver and exit-status evidence. The §6 mapping table, the §8.1 discriminator and §5.9's `phase` enum were updated to match. | **LOWER** — Quidra has a REPL in Interpreter mode, so it was one of the languages collecting the doubled reward. It now banks `P2` once. The four no-REPL languages likewise take the §26 zero once, which raises them relative to Quidra on the pair of metrics. The rule is language-neutral: any of the ten could ship a REPL and would be scored the same way. |
| SR-2 | MAJOR | **Dangling cross-reference with a scoring consequence.** Finding 12 moved `epsilon_compile` from `0.001 s` to `0.01 s` in §5.5 and §10, but §6's mapping table still read `0.001 s`. Two frozen values for one normalisation constant, in one document, differing by 10× on a metric where the constant is decisive. | §6's Compile / Build Performance row now reads `epsilon_compile = 0.01 s (§5.5, §10)`. §5.5 and §10 are the only places the value is derived. | Unchanged / not Quidra-specific — it removes an ambiguity, and `quidra_native`'s compile time was unmeasured when the value was set. |
| SR-3 | MINOR | Finding 6 added sub-probe `P3` but §5.9's `phase` enumeration still listed only `startup_p1`/`startup_p2`, so the raw schema had nowhere to put `P3` records and the pty-driver evidence MB-00 rule 2 and §6.1 both require published had no field. | §5.9 adds `startup_p3` to `phase`, adds `"repl_driver": "pipe"\|"pty"` to every `startup_p2`/`startup_p3` record and `"expression_index"`/`"expression_ns"` to `startup_p3`, and adds `repl_driver` to the summary record. | Unchanged. |
| SR-4 | MINOR | **Neutral-sounding rule contradicted by a specific exemption.** §2.5's preamble and item 8 presented the idiomatic-use rules as symmetric, while item 7 specifically blesses Zig's `-OReleaseFast` removal of bounds and overflow checks — the exact shape the audit warned about (a rule stated as neutral in one section, exempted in another), and the exact comparison in which `quidra_native`'s always-on checking is timed against another language's checks-disabled release mode. Finding 16's §1.1 table disclosed it, but §2.5 did not point at it. | §2.5 now opens by stating that the rules are symmetric while the recipes they bind are not, naming §1.1 as the governing disclosure; item 7 names C++ raw indexing alongside Zig and cross-references §1.1. **No recipe was changed** — disclosure is the correction, per specification §7 and §32. | **LOWER in interpretation**, same direction as finding 16: a reader of §2.5 alone can no longer take the configurations to be at equivalent checking settings when comparing Quidra's numbers with `cpp`'s and `zig`'s. |

**Status when SR-1…SR-4 were made: unchanged from the above — no micro measurement had been taken and no
score computed.**

### Counterpart obligations recorded for sibling documents

These are changes this document cannot make on its own side. Each is stated here so the sibling
document's owner can make the matching change before measurement.

1. **`environment/environment.json`** (owner of `frozen_toolchain_recipes`): either amend the Rust recipe
   to `rustc -C opt-level=3` — the `cargo build --release` equivalent, so that Rust is not measured a tier
   below its own production profile — or record `rustc -O` (opt-level 2) as a published handicap. §1.1
   currently records it as a handicap. Additionally, record in that file the per-configuration optimisation
   tier and run-time-checking state that §1.1 tabulates, so the two documents do not drift.
2. **`08_adversarial_cases.json`**: no change required if it keeps its frozen rule (Quidra scored from
   native mode, interpreter published in full as non-scoring rows plus a divergence table) — §8.1 has been
   aligned **to** it. If its owner instead adopts the worse-of-two-modes policy for Safety/Robustness,
   §8.1 of this document must change with it, because the two policies may not be mixed metric by metric.
3. **The Capability Coverage owner** (`01_capability_universe_and_probes.json`): two missing-capability
   results are now generated by this suite and must be receivable there — (a) a configuration with no
   incremental file writer / streaming line reader (MB-10; in this run, both Quidra modes), and (b) a
   configuration with no standard hash map (MB-11; inert in this run). They exist so that a substitute
   implementation cannot score *better* than the facility it replaces without the gap being recorded
   somewhere.
4. **The Standard-metric owner** (`05_standard_rubrics.json`) and the results assembler: Interactive /
   Interpreter Performance is now owned and defined by §6.2 of this document, and Startup / REPL Latency
   by §6.1. Neither may be re-derived elsewhere; a second definition of either would reintroduce exactly
   the aggregation ambiguity finding 1 identified. **Per SR-1, the two metrics share no input:** `P1` and
   `P2` feed Startup / REPL Latency, `P3` feeds Interactive / Interpreter Performance, and no assembler
   may reuse `P2` in the second metric. If that document's own rubric text for Interactive / Interpreter
   Performance describes it in terms of first-response latency, its owner must align it to
   repeated-expression latency (`P3`) so that one measured property is not scored in two metrics.
5. **The results assembler**: Source Code Size and Binary / Artifact Size must draw from different
   measurements (§5.6); a build-less configuration's artifact size is a scored `0` with
   `epsilon_artifact = 4096`, never its source bytes.
