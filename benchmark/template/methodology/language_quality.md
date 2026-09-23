# Language Quality Specification

This is the authoritative detailed specification for **Primary Evaluation 3 — Language Quality** in new benchmark runs.

## Required runner coverage IDs

The deterministic runner assigns these IDs to frozen work units before measurement. Across non-aggregation work units, every ID below must be covered by the current run plan. These IDs are orchestration metadata; they do not change the scoring definition.

- `coverage.all_10_languages`
- `gate.quidra_programs_current`
- `metric.native_execution_performance`
- `metric.long_running_performance`
- `metric.compile_build_performance`
- `metric.startup_latency`
- `metric.memory_efficiency`
- `metric.source_code_size`
- `metric.binary_artifact_size`
- `metric.runtime_overhead`
- `metric.code_efficiency_conciseness`
- `metric.readability`
- `metric.functionality_expressiveness`
- `metric.diagnostics`
- `metric.dependency_simplicity`
- `metric.portability_design_platform_neutrality`
- `metric.ffi_interoperability_design`
- `metric.concurrency`
- `metric.type_safety`
- `metric.memory_safety`
- `metric.runtime_safety`
- `metric.boundary_value_safety`
- `metric.adversarial_input_robustness`
- `metric.early_error_detection`
- `metric.debuggability`
- `metric.silent_bug_resistance`
- `metric.implementation_robustness`

The deterministic plan is validated mechanically before `manifest-merge`; missing or unknown requirement IDs are fatal pre-measurement errors.


Ordinary workers receive only the compact worker rules, frozen Primary configuration, assigned requirement IDs, and selected sections of this specification needed for those requirements. They do not need the root prompt, root conversation, historical runs, sibling outputs, or other evaluation specifications.

The frozen Primary configuration overrides only replication/execution counts. Evaluation meaning, language-neutral fairness rules, metric definitions, formulas and capability universes below remain binding unless explicitly changed before the run.

# 7. Language Quality Evaluation Fairness

For Language Quality Evaluation, use normal, idiomatic, production-reasonable best practices for each language.

The goal is to compare:

**the same algorithm, input, output, numerical requirements, and workload implemented appropriately in each language.**

Do not intentionally write inefficient code in one language.

For example, do not penalize C++ by unnecessarily copying large `std::vector` objects when normal code would use:

- references
- `const&`
- `std::span`

Likewise, use normal idioms such as:

- Rust borrowing and slices
- Go slices
- Java reference semantics
- appropriate native data structures
- avoidance of unnecessary copies
- release / optimized builds
- normal compiler optimizations

However, the following are prohibited:

- using a different algorithm for only one language
- GPU acceleration for only one language
- handwritten SIMD for only one language
- benchmark-specific hacks for only one language
- outsourcing the core computation to an optimized external native library for only one language
- deliberately weakening one language
- giving one language disproportionate manual optimization

The principle is:

**Language Quality Score = the intrinsic quality of the language and its implementation under comparable workloads, excluding maturity/network-effect advantages that primarily accumulate with age and adoption.**

Language Quality must not be tuned to Quidra's design philosophy. Established languages must receive full credit for genuinely better language semantics, implementation quality, performance, diagnostics, safety, concurrency, portability design, interoperability design, and other intrinsic properties.

However, do **not** reduce Language Quality merely because a language is young and therefore has fewer surrounding tools, supported integrations, mature editor/debugger workflows, years of documentation, release history, package infrastructure, or implemented target platforms. Those present-day disadvantages are real, but they belong to Primary Evaluation 4 — Ecosystem.

Mixed concepts must be split instead of double-counted:

- language-level portability / platform neutrality -> Language Quality;
- realized supported-platform breadth -> Ecosystem;
- FFI / interoperability design quality -> Language Quality;
- breadth of working external integrations -> Ecosystem;
- dependency semantics / dependency simplicity -> Language Quality;
- package-manager maturity, package corpus, and maintenance activity -> Ecosystem;
- diagnostics and code-level debuggability -> Language Quality;
- debugger/profiler product maturity -> Ecosystem.

This boundary is deliberate: Language Quality asks how good the language and implementation are; Ecosystem asks how much usable surrounding infrastructure and real-world evidence currently exists.

---

# 8. Language Quality Score Metrics

Evaluate the following metrics.

All final normalized metric scores must range from 0 to 100, where 100 is best.

## Performance

- Native Execution Performance
- Long-running Performance
- Compile / Build Performance
- Startup Latency

## Resource / Distribution

- Memory Efficiency
- Source Code Size
- Binary / Artifact Size
- Runtime Overhead

Source size and artifact size must remain separate metrics.

## Language / Development

- Code Efficiency / Conciseness
- Readability
- Functionality / Expressiveness
- Diagnostics
- Dependency Simplicity
- Portability Design / Platform Neutrality
- FFI / Interoperability Design
- Concurrency

These eight **Language / Development** metrics use the frozen
`methodology-assets/language_quality/design_rubrics.json` contract. Each metric
has exactly five equally weighted components and fixed levels 0..4. The
language-scoped worker performs only the semantic judgment: it assigns one
frozen level per component and cites existing frozen `repo/...` or
`template/...` evidence. The trusted runner owns all arithmetic, maps levels
to 0/5/10/15/20 points, sums the five components to the normalized 0–100
metric score, and overwrites any worker-supplied numeric score. The rubric set
is mechanically validated by `gate.language_quality_design_rubrics_frozen`
before any of these judgment workers may run. External ecosystem maturity,
popularity and live web evidence are forbidden here and belong exclusively to
Primary Evaluation 4.

## Safety / Robustness

- Type Safety
- Memory Safety
- Runtime Safety
- Boundary Value Safety
- Adversarial Input Robustness
- Early Error Detection
- Debuggability
- Silent Bug Resistance
- Implementation Robustness

Semantic Compression-specific concepts such as Semantic Regularity, Rule Exception Density, context-sensitive semantic branching, and hidden behavior are intentionally excluded from the Language Quality score to avoid double counting. Their direct evaluation belongs to Primary Evaluation 1.

Language Quality intentionally excludes ecosystem maturity and network-effect metrics. Do not raise or lower it because a language has more users, more third-party packages, more community content, more production deployments, more years of release history, broader editor/debugger integration, broader implemented platform coverage, or a more mature surrounding toolchain. Score those factors in Ecosystem instead.

Use these metrics to calculate the:

**Language Quality Score**

Do not fabricate human-study results for metrics such as Readability.

If an objective proxy is used, define it explicitly.

---

## 8.1 Fixed Language Quality Weighting

The Language Quality Score uses fixed category weights. Do not choose or tune these weights during a benchmark run.

| Language Quality category | Weight |
|---|---:|
| Performance | 25% |
| Resource / Distribution | 18.75% |
| Language / Development | 25% |
| Safety / Robustness | 31.25% |

Within each category, every listed normalized metric has equal weight unless this specification explicitly defines a more specific sub-metric aggregation.

Calculate each category score as the arithmetic mean of its applicable normalized metrics, then calculate:

**Language Quality Score = 0.25*Performance + 0.1875*Resource + 0.25*LanguageDevelopment + 0.3125*SafetyRobustness**

Apply the N/A policy in Section 26 within the affected category first. If an entire category is genuinely N/A, renormalize the remaining category weights proportionally and document the reason. A missing capability intentionally tested by a category is not N/A.

Do not change category weights, metric membership, or within-category equal weighting after measurements begin.

A genuinely inapplicable metric may follow Section 26. An applicable metric that was not executed is **not** `N/A` and its weight may not be silently redistributed.

If any applicable Language Quality metric required by the fixed score is `Not Executed`, or if an entire applicable category lacks the evidence required by this specification, mark Language Quality `PARTIAL` and do **not** calculate or publish Language Quality Score or Language Quality Ranking. Partial category and metric measurements may still be reported as diagnostics.

## 8.2 Runner-owned resource metric definitions

The following resource metrics are mechanically derived and must not be reinterpreted by a leaf worker:

- **Memory Efficiency** = the family-C lower-is-better score from peak RSS measured on MB-01 through MB-11, normalized per workload and then averaged across applicable workloads.
- **Runtime Overhead** = the family-C lower-is-better score from the median peak RSS of MB-00, the minimal ordinary program that prints `HELLO` and exits successfully. This measures baseline runtime/process memory overhead rather than workload memory demand.
- **Source Code Size** = the family-C lower-is-better score from source bytes for MB-01 through MB-11, normalized per workload and averaged. MB-00 is excluded.
- **Binary / Artifact Size** = the shifted family-C score from the frozen per-workload build artifact definition.
- **Startup Latency** = the family-C lower-is-better score from MB-00 ordinary-program process-start-to-exit wall time.

Runtime Overhead and Memory Efficiency are deliberately separate: MB-00 isolates the baseline cost of bringing the language/runtime process to life, while MB-01 through MB-11 measure memory under real benchmark work. Do not substitute one for the other.

---

# 11. Quidra Current-Commit Evaluation

Quidra is evaluated through its ordinary compiled/native execution path, using the compiler and runtime built from the exact evaluated commit.

Measure:

- Execution Time
- CPU Time
- Peak RSS
- Compile Time
- Compile + Execute Time
- Startup Time
- Binary Size

Quidra contributes exactly one scored execution configuration: the compiled/native program produced from the evaluated commit. Quidra's program sources are maintained inside the evaluated snapshot (`tests/benchmark/quidra`), together with the compiler that runs them, and the deterministic runner re-audits them against that commit's compiler before any measurement (`gate.quidra_programs_current`): every program must build, run, match the frozen oracle and honour the frozen representation pins. A stale or missing program is an authoring/infrastructure blocker, never a language score. Repeatable build, correctness, timing, memory, and size measurements are runner-owned.

---

# 12. Micro Benchmarks

Run the following benchmark categories for all 10 languages:

- Fibonacci
- factorial
- integer arithmetic
- floating-point arithmetic
- vector inner product
- matrix multiplication
- sorting
- strings
- statistics
- file I/O
- collections

For every language, keep fixed:

- algorithm
- input data
- workload size
- expected output
- correctness criteria
- iteration count

Save these conditions before performance measurement begins.

Account fairly for runtime characteristics such as:

- warm-up
- JIT compilation
- garbage collection
- runtime initialization

Do not compare an un-warmed JIT workload against a steady-state native workload without clearly separating those measurements.

### Mandatory repeated-measurement protocol

For every primary timing or resource-measurement cell, unless a workload section explicitly freezes a larger count **before any language is measured for that workload**:

1. execute exactly **W unscored warm-up runs**, where `W = timing.warmups_per_cell` in `primary.json`;
2. execute exactly **R scored measurement runs**, where `R = timing.measured_runs_per_cell` in `primary.json`;
3. validate expected output on every warm-up and measured run;
4. preserve all `R` raw scored measurements in execution order;
5. use the **median** of the R scored runs as the representative raw value; and
6. preserve dispersion at minimum as min, max, median, and MAD or IQR.

### Deterministic cross-language measurement interleaving

For every workload where multiple languages or execution modes are compared by primary timing or resource measurements, do not measure all scored repetitions of one language and then move to the next.

Before the first timing/resource sample for that workload:

1. define the complete comparison-cell set for the workload;
2. derive and record one deterministic `measurement_order_seed` from the immutable run identity and workload ID, or freeze an explicit seed in the manifest before measurement;
3. materialize and preserve the complete warm-up and scored execution schedule before observing any timing result; and
4. use that schedule unchanged unless the recovery policy requires a documented restart.

Execute warm-ups in rounds: each round gives at most one warm-up run to every cell that still requires warm-up, with the within-round cell order deterministically shuffled from the frozen seed and round index. Then execute scored measurements in **R scored rounds**, each containing exactly one scored run from every applicable comparison cell, again using a deterministic per-round shuffle derived from the same frozen seed. Thus each cell still receives exactly the required number of samples, but machine drift, thermal state, scheduler load, and other time-correlated effects are distributed across languages instead of being coupled to presentation order.

The interleaving schedule is part of the raw evidence. Preserve the seed, generated order, actual start order, and any deviation/recovery record. Never choose or modify the order after observing timing results. Do not execute all scored samples for one language consecutively unless the comparison group contains only one applicable cell or a genuine platform constraint makes interleaving impossible; in that case document the constraint before scoring and treat any resulting comparability limitation explicitly.

Use exactly **W warm-ups per cell**, where `W = timing.warmups_per_cell` in `primary.json`. Do not add language-specific warm-ups during the Primary run. Startup/cold-start measurements remain separate and must not be replaced by steady-state values.

A measured run affected by a verified harness/runner/transport failure is **invalid evidence**, not an outlier to silently discard. Preserve the failed attempt and restart the entire affected measurement cell from its configured warm-ups so the final cell still contains exactly R valid scored runs under one uninterrupted frozen configuration.

Do not trim, winsorize, cherry-pick, or discard a slow but valid scored run. Statistical outlier status alone is never a reason to remove valid evidence.

Before the first timing cell, perform one unscored timing-harness self-test and one peak-memory self-test with known finite programs. Confirm that the harness returns the expected number of samples, preserves units, distinguishes nonzero runtime from startup/measurement overhead where applicable, and reports failed commands as failures rather than as numeric zeros.

Preserve:

- all warm-up measurements;
- all `R` individual scored measurements;
- representative median;
- variability / dispersion;
- timeout/retry evidence, if any; and
- exact command lines and environment variables used.

Do not rely only on a single timing measurement.

---

# 13. Algorithm Benchmark: SVM

Reference repository:

https://github.com/koba-jon/svm_cpp

Record the exact reference commit SHA used.

Inspect the reference implementation, algorithm, inputs, outputs, and expected numerical behavior.

Create equivalent implementations for all 10 languages.

Measure at least:

- Compile / Parse Success
- Test Pass Rate
- Numerical Match
- Prediction Match
- Accuracy
- Numerical Error
- Execution Time
- Memory Usage
- Compile Time
- Source Bytes
- LOC
- Source Tokens
- LLM Generation Success
- Repair Count
- Silent Bug occurrence

Where applicable, compare:

- learned parameters
- predictions
- accuracy
- losses
- intermediate results
- final results

Use an explicit floating-point tolerance and save that tolerance.

A program that compiles and runs but silently returns an incorrect result must be classified as a:

**Silent Bug**

not as a successful implementation.

---

# 14. Algorithm Benchmark: GMM

Reference repository:

https://github.com/koba-jon/gmm_cpp

Record the exact reference commit SHA used.

Follow the same fairness, correctness, numerical comparison, performance measurement, LLM evaluation, and Silent Bug rules used for the SVM benchmark.

Measure at least:

- Compile / Parse Success
- Test Pass Rate
- Numerical Match
- Parameter Match
- Prediction / Assignment Match where applicable
- Likelihood or objective-value agreement
- Numerical Error
- Execution Time
- Memory Usage
- Compile Time
- Source Bytes
- LOC
- Source Tokens
- LLM Generation Success
- Repair Count
- Silent Bug occurrence

---

# 15. Real-world Benchmark: LightGrad

Reference repository:

https://github.com/koba-jon/lightgrad

Record the exact reference commit SHA used.

Inspect the design, implementation, API, and major functionality.

Define a common workload containing at least:

- multiple functions
- multiple modules
- tensor / numerical processing
- automatic differentiation
- forward computation
- backward computation
- data structures
- abstraction
- memory management
- error handling
- API design

Measure:

- Forward Correctness
- Backward Correctness
- Execution Performance
- Memory Usage
- Compile Performance
- Source Size
- Source Token Count
- LLM Generation Success
- Repair Count
- Silent Bug occurrence

If completely porting LightGrad to all 10 languages is not practical, define a **common subset before implementation begins**.

Use exactly the same subset for all 10 languages.

Do not select an easier subset for one language and a harder subset for another.

---

# 16. LLM Implementation Scenarios

For SVM, GMM, LightGrad, and other substantial tasks, evaluate at least these two independent scenarios.

## A. Specification → Implementation

Do not show the reference source code.

Provide only:

- algorithm specification
- API specification
- input/output requirements
- constraints
- correctness requirements
- tests

Then ask the LLM to implement the program.

## B. Reference → Porting

Provide the reference implementation and ask the LLM to port it to the target language with equivalent behavior.

Keep Scenario A and Scenario B results separate.

Do not merge them into a single raw dataset.

---

# 17. Adversarial / Edge-case Testing

## Pre-freeze diagnostic and locale calibration

Before freezing the adversarial diagnostic lexicon, run unscored calibration probes for every hazard/toolchain whose outcome classification depends on diagnostic text. Collect the actual diagnostic wording or structured diagnostic key from all ten languages and verify that the frozen classifier recognizes equivalent safe detections symmetrically. A structured diagnostic key may be used directly; prose-only runtimes must not lose points merely because the lexicon omitted their ordinary wording.

Do not expand or repair the lexicon after scored outcomes are visible.

For diagnostics/rubric evidence, freeze the diagnostic locale at the level each toolchain actually obeys and verify the pin with an unscored deliberate-error probe before collection. Environment variables that a toolchain ignores do not count as a locale pin. For JVM compiler/runtime commands, use explicit JVM language/country flags when required rather than assuming `LC_ALL` or `LANG` controls diagnostics.


Do not test only valid inputs.

Actively attempt to expose unsafe semantics, hidden behavior, crashes, undefined behavior, and silent bugs.

Include at least:

- very large `int64` → `int32` narrowing
- integer overflow
- integer underflow
- signed / unsigned boundaries
- out-of-range float → integer conversion
- NaN
- Infinity
- divide by zero
- array lower-bound violation
- array upper-bound violation
- huge allocation
- uninitialized value
- invalid cast
- type mismatch
- invalid mutation
- mutation of immutable value
- null / none misuse
- missing return
- invalid program state
- infinite recursion
- excessive recursion depth
- extreme parser nesting
- malformed source code
- malformed UTF-8
- huge numeric literal
- invalid external input

Pay particular attention to cases where:

**an invalid value is silently transformed into another apparently valid value and execution continues.**

Classify each case as one of:

1. Compile-time Detection
2. Runtime Safe Detection
3. Test Failure
4. Output Verification
5. Crash
6. Undefined Behavior
7. Silent Bug

Also record the earliest stage at which the defect became observable.

### Diagnostic classification evidence precedence

Do not classify outcomes primarily by matching English compiler/runtime prose when stronger machine evidence exists. For every adversarial case, preserve and classify evidence in this order:

1. structured diagnostics such as JSON, stable diagnostic/error codes, and explicit compiler phase/stage fields;
2. process exit status plus the compiler/runtime stage that produced it;
3. machine-verifiable test result and expected-output verification;
4. natural-language stdout/stderr only as supporting evidence or, when no structured signal exists, as a documented fallback.

If a language/tool exposes structured diagnostics, its code/category/stage is the primary classification key; localized wording must not change the class. A text regex must not override contradictory structured evidence. When text matching is the only available signal, preserve the raw message, locale/tool version, matching rule, and an auditable sample of classifications. `Crash`, `Undefined Behavior`, and `Silent Bug` must be established from execution/verification evidence rather than guessed from message wording.

---

# 18. Early Error Detection

Measure how early each language detects invalid programs or dangerous states.

Prefer, in descending order:

1. compile-time rejection
2. static checking before execution
3. explicit runtime safety failure
4. test-detected error
5. output-verification failure
6. crash
7. undefined behavior
8. silent incorrect behavior

Use this evidence when calculating:

- Early Error Detection
- Runtime Safety
- Boundary Value Safety
- Silent Bug Resistance
- Debuggability

---

# 19. Debuggability

Do not evaluate only whether a bug exists.

Evaluate how easily the bug can be:

**noticed → diagnosed → repaired**

Record where measurable:

- Detection Stage
- Time to Detection
- Time to Root Cause
- Diagnosis Turns
- Repair Turns
- Tokens to Diagnosis
- Tokens to Repair
- Misdiagnosis Count

Do not invent human debugging times.

If no real human participant study was performed, do not claim that a human would need a specific number of minutes.

LLM or agent wall-clock time may be recorded when actually measured, but also record turns and token usage.

---

## 28.1 Primary Evaluation 3 — Language Quality Final Comparison Table

Produce a Language Quality table with the following fixed columns:

| Metric | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Native Performance | | | | | | | | | | |
| Long-running Performance | | | | | | | | | | |
| Compile / Build Performance | | | | | | | | | | |
| Startup Latency | | | | | | | | | | |
| Memory Efficiency | | | | | | | | | | |
| Source Code Size | | | | | | | | | | |
| Binary / Artifact Size | | | | | | | | | | |
| Runtime Overhead | | | | | | | | | | |
| Code Efficiency / Conciseness | | | | | | | | | | |
| Readability | | | | | | | | | | |
| Functionality / Expressiveness | | | | | | | | | | |
| Diagnostics | | | | | | | | | | |
| Dependency Simplicity | | | | | | | | | | |
| Portability Design / Platform Neutrality | | | | | | | | | | |
| FFI / Interoperability Design | | | | | | | | | | |
| Concurrency | | | | | | | | | | |
| Type Safety | | | | | | | | | | |
| Memory Safety | | | | | | | | | | |
| Runtime Safety | | | | | | | | | | |
| Boundary Value Safety | | | | | | | | | | |
| Adversarial Input Robustness | | | | | | | | | | |
| Early Error Detection | | | | | | | | | | |
| Debuggability | | | | | | | | | | |
| Silent Bug Resistance | | | | | | | | | | |
| Implementation Robustness | | | | | | | | | | |
| **Language Quality Score** | | | | | | | | | | |

Every value in this table that is a normalized score must follow:

**higher = better.**

Create an independent:

**Language Quality Ranking**

based on Language Quality Score only when Language Quality status is `COMPLETE`. `Not Executed` applicable metrics prevent publication of the Language Quality Score and Ranking.
