# Cross-language constraints discovered during pre-measurement reconnaissance

Frozen before any workload implementation or scoring. Recorded here so the choices below are
auditable rather than buried in a script.

## C-1. The deterministic data generator must not rely on integer wraparound

**Requirement.** Every workload that needs pseudo-random input must produce **bit-identical** data in all
ten languages, without shipping large data files. That forces a generator each language can express
exactly and naturally.

**The problem with the usual choice.** The conventional benchmark generator is a 64-bit LCG or xorshift
that relies on silent modular wraparound. Across the fixed comparison set that behaviour is *not*
uniformly available:

| Language | 64-bit wraparound multiply |
|---|---|
| C++, Go, Java, Kotlin | available (unsigned wrap / two's-complement wrap) |
| Rust, Swift, Zig | available only via explicit opt-in (`wrapping_mul`, `&*`, `%` builtins) |
| Python | integers are arbitrary precision; requires explicit `& 0xFFFFFFFFFFFFFFFF` masking |
| TypeScript | numbers are IEEE-754 doubles; exact 64-bit integer work requires `BigInt` |
| Quidra | integer arithmetic is overflow-checked; wraparound is not expressible |

Verified directly: in Quidra, `state * 6364136223846793005` on a `uint64` raises
`runtime error[INTEGER_OVERFLOW]` rather than wrapping.

A wraparound generator would therefore hand four languages a natural one-liner while forcing awkward,
non-idiomatic workarounds on Python and TypeScript and being inexpressible in Quidra. That is a
benchmark-infrastructure artefact, not a language property, and it would contaminate Source Code Size,
Code Efficiency and Readability for reasons unrelated to the workload.

**Frozen decision.** All workloads use the **Park–Miller minimal-standard LCG**:

```
state_{n+1} = (state_n * 48271) mod 2147483647      (seed = 1 unless a workload states otherwise)
unit_float  = state_{n+1} / 2147483647.0            (in (0,1))
```

**Why this is neutral rather than an accommodation of Quidra.** The largest intermediate value is
`48271 * 2147483646 ≈ 1.037e14`. That is exact in:

- every 64-bit signed integer type (max ≈ 9.22e18), so C++, Rust, Go, Java, Kotlin, Swift, Zig and
  Quidra all compute it directly with no overflow and no opt-in wrapping call;
- IEEE-754 double (exact integers to 2^53 ≈ 9.01e15), so TypeScript computes it exactly with plain
  `number`;
- Python's arbitrary-precision integers with no masking.

So the generator is idiomatic and exact in all ten languages simultaneously. No language needs a
workaround, and no language gets a shortcut the others lack.

**Verified.** Quidra and Python were confirmed to emit identical streams (`48271`, `182605794`,
`1291394886`, …) and identical derived unit floats to 9 decimal places.

**What this does NOT do.** It does not remove integer overflow from evaluation. Overflow behaviour is
still probed directly and deliberately in the adversarial/safety case set (spec section 17), where each
language's real behaviour — checked failure, silent wraparound, or undefined behaviour — is observed and
classified on its merits. Choosing a non-overflowing *data generator* keeps the workload identical; it
does not shield any language from the overflow tests.

## C-2. Floating-point output must be compared at a fixed precision

Languages differ in default float formatting (shortest-round-trip vs fixed digits vs exponent style).
Every workload therefore specifies an explicit output format with a fixed number of decimal places, and
numerical agreement is checked against an explicitly stated tolerance rather than by string equality of
default formatting. The tolerance is recorded per workload.

## C-3. Java must be measured on a native, current JDK

The `java` first on the measurement host's PATH was AdoptOpenJDK 8 — an x86_64 **JRE** with no `javac`,
which would execute under Rosetta translation on this arm64 host. Measuring Java that way would
understate it for reasons unrelated to the language. Java and Kotlin are therefore pinned to the native
arm64 Homebrew OpenJDK 26.0.1 via `JAVA_HOME`, recorded in `environment.json`.

## C-4. Toolchain API drift that is harness responsibility, not language failure

Two toolchains in the frozen environment have current APIs that differ from widely-reproduced older
idioms. These are handled by the harness so that a trial is never scored down for an infrastructure
mismatch (spec 10.4 pre-flight requirement (b)):

- **TypeScript 7.0.2** removed `--outFile`; emission uses `tsc --outDir`.
- **Zig 0.16** replaced the old `std.io` stdout helpers with an explicit `Io` instance
  (`std.Io.Threaded.init_single_threaded`, `std.Io.File.stdout()`).

Where a task's correctness does not depend on knowing the current stdout spelling, the harness supplies
it, and a model is not penalised for writing the older form. Where the task *is* about the current API,
that is stated.

## C-5. Reference-lexer availability for token reconciliation (resolved)

Methodology 02 §4.7 leaves Quidra's reference-lexer status to be determined once, before measurement.
**Resolved: N/A.** Quidra 0.2.0 exposes no token-stream dump. `quidra inspect` emits typed *AST nodes*
(`node_id`, `kind`, `parent_id`, `depth`, span, `inferred_type`), which is a parse-tree view, not a lexical
token stream, and cannot be mapped to token classes without re-lexing.

Quidra therefore joins the majority category — Rust, Java, Kotlin, Swift and Zig — where the frozen
tokenizer is the sole instrument. Reconciliation against a reference lexer is available for Python, Go,
C++ and TypeScript only. Per 02 §4.5 the frozen tokenizer is authoritative for **all** ten languages
precisely so that this uneven availability does not mean some languages are counted by a different
instrument from others.

## C-6. LightGrad: the autodiff engine must be implemented, not imported — in every language

**The hazard.** Methodology 07 §1.9 permits "standard library only". Quidra's standard namespaces
include `neural`, which is a built-in automatic-differentiation facility (`neural.track`, `neural.grad`,
`Parameter`, `Gradients`). Read naively, "standard library only" would let Quidra satisfy the LightGrad
workload by calling its built-in autodiff while Python, C++, Rust, Go, Java, TypeScript, Kotlin, Swift
and Zig — none of which ship an autodiff engine in their standard library — each hand-build a tape.

That would not be a language comparison. It would compare "has autodiff built in" against "does not",
on a workload whose entire purpose (spec §15) is to measure how well each language expresses the
construction of such an engine: multiple modules, tensor processing, forward and backward computation,
data structures, abstraction, memory management, error handling and API design. It would also breach
spec §32's prohibition on "outsourcing the core computation to an optimized external native library for
only one language" — the principle is identical when the library is built into the language.

**Frozen rule.** For the LightGrad common-subset workload (LGC), **every one of the ten languages
implements the autodiff engine itself**, in the single workload source file, using only general-purpose
language facilities. Specifically prohibited in ALL languages, symmetrically:

- Quidra: the `neural` namespace in any form (`neural.track`, `neural.grad`, `neural.Parameter`,
  `neural.State`, `neural.Gradients`, `neural.mean`, the differentiable reductions, `neural.save/load`),
  and the `dnn` package.
- Python: `torch`, `jax`, `autograd`, `tensorflow`, `numpy` (already excluded by §1.9).
- Every other language: any autodiff, tensor or linear-algebra library, first-party or third-party.

General-purpose numeric facilities that are not differentiation — arithmetic, arrays, dynamic
containers, hash maps, string formatting, `sqrt`/`exp` — remain available to all ten equally.

**Why this is not a penalty aimed at Quidra.** The restriction removes a Quidra advantage on ONE
workload, and it is the same restriction every other language is already under. Quidra's built-in
autodiff is a genuine capability and it is still credited where the benchmark measures capability rather
than expression: it counts in Semantic Compression Capability Coverage, and in the Standard
"Functionality / Expressiveness" and "Library Availability" rubrics, each of which is scored from the
frozen rubric criteria and not from LightGrad. What it may not do is substitute for the workload itself.

**Conformance test.** Methodology 07 §4.4 already pins the backward pass so tightly — gradient
accumulation through a graph-building `ops.add`, a plain recursive creator walk with **no** topological
sort and **no** visited set, depth-first input1-before-input2, and `newGrad()` installing a *fresh*
gradient tensor — that a conforming implementation must expose its own tape. Any implementation that
delegates to a built-in engine will produce different higher-order results on the `mul(h, h)` double-visit
case in §4.7 and fails the numerical gate. The rule above states the intent explicitly so that no
implementer has to infer it, and so that a reviewer can check it directly.

## C-7. When a language lacks a primitive a workload requires

**Discovered:** by the MB-03 fairness audit, and independently verified on this host.

**The finding.** Quidra 0.2.0 has **no bitwise operators at all**. Verified directly:

| Expression | Result |
|---|---|
| `a ^ b` | `error[LEX_ERROR] Unexpected character in source` — `^` is not even a token |
| `a & b` | `error[PARSE_ERROR]` (`&` is address-of only, per grammar.ebnf line 99) |
| `a \| b` | `error[PARSE_ERROR]` |
| `a << 1`, `a >> 1` | `error[PARSE_ERROR] Expected expression` |

The language reference confirms it: the grammar defines only `+ - * / %` and comparisons, and there is no
bitwise member in any standard namespace. MB-03's pinned kernel uses XOR.

**Why this needs a frozen rule.** The MB-03 Quidra implementation synthesises XOR arithmetically
(walking bits with `%2` and `/2`, ~31 iterations per call, 120 million calls). The audit measured a
second conforming synthesis and found the choice **worth roughly 12x**. Left unpinned, MB-03's Quidra
figure would measure whichever emulation the implementer happened to pick, not the language.

**Frozen rule (applies to every language symmetrically).**

1. A workload's pinned kernel is never changed, weakened, or removed because a language lacks a
   primitive. Spec §26 is explicit that a missing capability must not escape scoring via `N/A`.
2. Where a language cannot express a required primitive with a documented language feature or its
   normal standard library, the primitive is synthesised by the **canonical minimal definition** of
   that operation, stated in the workload, so that every implementer produces the same synthesis. For
   XOR over non-negative integers the canonical definition is the bitwise identity computed by repeated
   `%2` / `/2` descent with no lookup table, no precomputation, no memoisation, and no algebraic
   shortcut. The synthesis is part of the measured work.
3. **The resulting figure is published with an explicit emulation disclosure** naming the missing
   primitive, the synthesis used, and the fact that the measured time includes emulation cost. The
   figure is a real measurement of what that language must do to perform the workload; it is not a
   measurement of its arithmetic throughput, and it must never be presented as the latter.
4. The missing primitive is **separately** recorded as a capability gap, where it is scored on its
   merits — in Semantic Compression Capability Coverage and in the Standard
   Functionality/Expressiveness rubric — rather than only as a slow number.

**Why this is not a penalty aimed at Quidra, nor a favour to it.** Rule 1 refuses to make the workload
easier because Quidra is weak here. Rules 2 and 3 refuse to let an arbitrary implementation choice,
worth 12x, masquerade as a language measurement. Any of the ten would be treated identically: had a
language lacked, say, floating-point `sqrt`, the same three rules would apply to MB-04.

**Honest statement of what MB-03 then measures for Quidra:** the cost of performing the pinned integer
kernel in a language with no bitwise primitives. That is a genuine and reportable property of Quidra
0.2.0, and it is exactly the kind of result the benchmark exists to surface.

## C-8. Which toolchain recipe set governs which evaluation (authoritative)

Two frozen recipe sets exist in `environment/environment.json`. This item is the single authoritative
statement of which governs what; any other document's wording is subordinate to it.

| Evaluation | Recipe set | Safety posture |
|---|---|---|
| Primary Evaluation 1 — Semantic Compression (methodology 01–04) | `semantic_compression_recipes` | optimized **and CHECKED** wherever the language offers that mode |
| Primary Evaluation 2 — Standard (05–08: micro, algorithm, adversarial, rubrics) | `frozen_toolchain_recipes` | each language's normal optimized **release** configuration |
| Primary Evaluations 3 and 4 — LLM tracks (09–10) | `frozen_toolchain_recipes` | normal release, identical in purpose across languages |

**Why they differ, on neutral grounds.** The two evaluations ask different questions, and the honest
answer to each requires a different control.

*Semantic Compression* asks what the **local surface form** determines. Probes F08.P1, F08.P2 and F11.P1
ask what happens at integer overflow and at an out-of-bounds index. If Rust were built `-O`
(debug-assertions silently off, so `+` wraps) and Zig `-OReleaseFast` (signed overflow and out-of-bounds
both undefined behaviour) while Quidra has only an optimized-and-checked mode, those probes would score a
**build flag** rather than a language's semantics — and would score it against precisely the two
languages that offer the choice. Pinning every language that HAS an optimized checked mode into that
mode removes the confound. C++ has no checked mode; that is recorded as a language fact and is scored by
the determinacy and hidden-cost metrics, not smuggled in through a recipe.

*Standard* asks how strong each language is **as shipped today**. There, the release configuration a
competent developer actually ships is the correct measurement, and Rust's and Zig's ability to disable
checks in release is a genuine capability of those languages. Quidra's lack of any unchecked mode is
likewise genuine, and its cost is measured rather than excused.

**Neither set favours Quidra.** The Semantic Compression set *removes* an artificial advantage Quidra
held in the first draft (competitors compared checks-off against Quidra's checks-on). The Standard set
*declines to remove* a real advantage Rust and Zig have over Quidra. Each choice follows from what its
evaluation measures, and each was fixed before any score was computed.

**Verified on this host** before freezing: `rustc -O -C debug-assertions=on` panics on signed overflow;
`zig build-exe -OReleaseSafe` traps with `integer overflow`; `tsc --strict --target es2022 --module
nodenext` compiles and runs. The two sets differ ONLY in safety posture — never in optimization intent,
never per-language beyond that posture.

**Gate V5** (methodology 03) is satisfied when `semantic_compression_recipes` matches
`01_capability_universe_and_probes.json → toolchain_binding.recipes`; that is the pairing V5 checks.

## C-9. SVM and GMM: the numerical core must be implemented, not delegated (extends C-6)

**The hazard, and why C-6 alone was not enough.** C-6 stops Quidra delegating LightGrad's autodiff to its
built-in `neural` namespace. The re-audit of `09_llm_run_config.json` found the identical structural
hazard left open in **four** other scored tasks: `PE-P1-svm-specA`, `PE-P2-svm-portB`,
`PE-P3-gmm-specA`, `PE-P4-gmm-portB` carried no prohibited-facilities rule at all.

Methodology 07 §1.9 says "standard library only" and names `numpy`, BLAS, Eigen, `nalgebra`, `gonum` and
Accelerate — all **third-party**. But Quidra ships `linear.dot`, `linear.matmul`, `stats.mean`,
`tensor<T>` and the whole `tensor`/`linear`/`stats` family **in its standard library**. Read literally,
§1.9 would let Quidra call `linear.dot` for the SVM kernel and `stats.mean` for GMM's moments while
Python, Rust, Go and the rest hand-write the same loops because their equivalents are third-party. That
is not a language comparison; it is a comparison of what happens to be bundled.

**Frozen rule.** For WL-SVM and WL-GMM, in every one of the ten languages, the numerical core is
implemented from general-purpose language facilities. Specifically prohibited, symmetrically:

- **Quidra:** `linear.*` (including `linear.dot`, `linear.matmul`), `stats.*` (including `stats.mean`),
  `tensor` / `tensor<T>` and its methods, `neural.*`, and the `dnn` / `vision` packages.
- **Python:** `numpy`, `scipy`, `statistics`, `array`-backed BLAS shims, `math.fsum` for the pinned
  accumulations.
- **Every other language:** any linear-algebra, tensor, statistics or BLAS facility, first-party or
  third-party, bundled or installed — including `java.util.stream` reductions used to replace a pinned
  accumulation loop, Swift's Accelerate, and Go's `gonum`.

Remaining available to all ten equally: arithmetic, arrays and dynamic containers, hash maps, string
formatting, and the elementary scalar functions the workload names (`sqrt`, `exp`, `log`), since the
frozen algorithms specify those by name and every language has them.

**Why this is not aimed at Quidra.** It removes a bundling advantage on two workloads and applies the
same words to every language's equivalent facility. Quidra's `linear`/`stats`/`tensor` are real
capabilities and are still credited where the benchmark measures capability — Semantic Compression
Capability Coverage, and the Standard Functionality/Expressiveness and Library Availability rubrics —
each scored from its own frozen criteria. What they may not do is substitute for the workload whose
whole purpose is to measure expressing that computation.

**Scope.** C-6 governs WL-LG. C-9 governs WL-SVM and WL-GMM. Together they cover every scored algorithm
task: PE-P1..PE-P6. The micro suite is unaffected (its §4.12 container table already pins containers
per language, and MB-05/MB-06 already forbid library matmul/dot).

## C-10. Quidra's two execution modes: one rule across all documents (authoritative)

Quidra is the only language in the fixed set with two shipped execution modes, so it is the only one
needing an explicit rule. The re-audit found three documents stating incompatible versions of it. This
item is authoritative; any other wording is subordinate.

| Evidence source | Quidra's scored value |
|---|---|
| Adversarial / safety case set (metrics: Type Safety, Memory Safety, Runtime Safety, Boundary Value Safety, Adversarial Input Robustness, Early Error Detection, Debuggability, Silent Bug Resistance, Implementation Robustness, Diagnostics) | **mean of native and interpreter**, per `08_adversarial_cases.json → toolchain_binding.quidra_mode_rule` |
| Interactive / Interpreter Performance; the P2 (REPL) sub-probe of Startup / REPL Latency | **Interpreter** |
| Compile / Build Performance; Binary / Artifact Size; Deployment Footprint | **Native** (the interpreter has no build step and ships no artifact) |
| Long-running Performance; Memory Efficiency; Runtime Overhead; the P1 sub-probe of Startup Latency | **mean of the two modes**, combined at the raw level per workload before normalization |
| Every other Standard metric | **Native** |

**Why safety evidence takes the mean rather than native alone.** A defect reachable in either shipped
mode is a real defect of the language. Scoring only the mode that hides it would be precisely the
pro-Quidra shape this audit existed to find — and the corrected rule is the one that costs Quidra, since
any interpreter-only defect now lowers its safety scores instead of appearing in a footnote.

**Both modes are always executed and always published**, for every adversarial case and every micro
workload, with a native-versus-interpreter divergence table, regardless of which mode is scored. Nothing
is hidden; a reader who disagrees with a combiner can recompute from the preserved raw rows.

**This is not a handicap either.** Quidra is not penalised for *having* two modes: where a metric is
defined on one mode (build time, artifact size), only that mode is used, exactly as C++ contributes no
REPL measurement and Python contributes no compile time.

**Superseded wording.** `06_micro_workloads.md` §8.1 previously asserted "Native only" for the safety
metrics and supported it by quoting an earlier version of document 08 that no longer exists. That
paragraph is marked superseded in place and now defers to this item.
