# Common Benchmark Rules

The runner uses these rules as the authoritative common scoring contract. Leaf workers receive the compact `worker_core.md` plus only the evaluation sections needed for their assigned requirement IDs.

For root orchestration, `execution_policy.md` governs Task Packet compilation, retries, recovery, and deterministic aggregation. Ordinary workers do **not** need to open it.

Within a worker Task Packet, precedence is:
1. the frozen Primary configuration embedded from `template/config/primary.json` for replication/execution counts;
2. this common specification;
3. the embedded evaluation-specific specification;
4. the concrete Task Packet instructions where they narrow paths/outputs without changing evaluation semantics.

The split specifications are complete for new runs. New runs must not read historical run directories as input, and workers must not depend on the root conversation.

The evaluated source visible to workers is the immutable snapshot at `./.quidra-benchmark/repo`. Use only the fixed 10-language comparison set. Never change a condition selectively for Quidra.

# 3. Fixed Comparison Languages

The comparison set is fixed to exactly these 10 languages:

1. Quidra
2. Python
3. C++
4. Rust
5. Go
6. Java
7. TypeScript
8. Kotlin
9. Swift
10. Zig

Do not add, remove, replace, or substitute languages during the benchmark.

Evaluate **TypeScript**, not JavaScript.

If a metric cannot legitimately be measured for one language, keep the language in the comparison and mark only that metric as `N/A`, with the reason documented.

---

# 4. General Evaluation Principle

Do not stop after proposing an evaluation methodology.

Actually perform:

**implementation → build / compile / parse → execution → correctness verification → measurement → raw-data preservation → normalization → scoring → comparison**

Do not assign scores based only on:

- reputation
- general language knowledge
- subjective impressions
- conventional wisdom

Every score must be supported by one or more of:

- measured raw data
- reproducible tests
- explicitly defined objective criteria
- explicitly documented scoring formulas

If a value cannot be measured honestly, do not fabricate it.

Use `N/A` or `Not Executed` and explain why.


## 4.1 Mandatory comparability and publication gates

A benchmark result is publishable only if the evidence being compared has the same meaning across all 10 languages.

This is a **precondition**, not a post-result disclaimer.

Before any primary evaluation is scored, define and preserve its comparability contract:

- the exact units being counted or measured;
- the exact row / site / task universe;
- inclusion and exclusion rules;
- multiplicity rules;
- missing-capability handling;
- validation rules; and
- the mechanical checks that prove all 10 languages were evaluated against the same contract.

The run must assign each primary evaluation exactly one status:

- `COMPLETE` — all required evidence and comparability gates passed;
- `PARTIAL` — useful measurements exist, but one or more required measurements, repetitions, or gates are incomplete;
- `WITHDRAWN` — measurements were produced but a validity defect was discovered;
- `NOT EXECUTED` — the evaluation was not run.

**Only a `COMPLETE` primary evaluation may publish its Overall Score or Ranking.**

For `PARTIAL`, `WITHDRAWN`, or `NOT EXECUTED` evaluations:

- preserve all valid raw evidence;
- clearly identify provisional or diagnostic observations;
- do not emit a numeric primary Overall Score;
- do not rank the languages for that primary evaluation;
- do not place provisional values into fields named `Overall Score` or `Ranking`; and
- do not allow report-generation scripts to infer a ranking from raw intermediate values.

A failed comparability gate is an infrastructure failure. It is never converted into a language score.

`N/A`, `Not Executed`, `PARTIAL`, and `WITHDRAWN` are distinct states and must never be treated as interchangeable.

# 5. Universal Score Direction

All normalized scores use the same direction:

**100 = best**  
**0 = worst**

Every score in every final comparison table must follow:

**higher score = better result**

This rule is absolute.

For raw metrics where lower values are better, reverse the direction during normalization.

Examples:

- lower execution time → higher Performance Score
- lower memory usage → higher Memory Efficiency Score
- lower compile time → higher Compile / Build Performance Score
- lower startup latency → higher Startup Score
- smaller source size → higher Source Code Size Score
- fewer source tokens → higher Token Efficiency Score
- fewer repair iterations → higher Repair Efficiency Score
- fewer silent bugs → higher Silent Bug Resistance Score
- fewer semantic branches → higher Semantic Determinacy Score
- fewer required external semantic lookups → higher Semantic Locality Score
- fewer hidden semantic behaviors → higher Hidden Semantic Cost Score
- lower semantic complexity per capability → higher Capability Efficiency Score

Raw values themselves must still be preserved in their natural units.

Do not alter the meaning of raw measurements simply to make larger values look better.

Only the normalized **0–100 score** must always use:

**higher = better.**

`N/A` must remain `N/A` and must not be converted to zero automatically.

---

# 25. Scoring Methodology

The scoring pipeline inside each primary evaluation must be:

**Raw Data → Normalization → Metric Score → Evaluation Score**

Never assign a 0–100 score before preserving the underlying evidence.

For every metric, preserve:

- raw unit
- whether higher or lower raw values are better
- normalization formula used
- clipping policy
- aggregation method
- weight
- N/A policy

The normalization family is fixed by the metric type below. Do not choose a different family after seeing results.

### 25.1 Fixed normalization families

**A. Rates, probabilities, and bounded success fractions where 1.0 is ideal**

Examples: compile success rate, test pass rate, Correct@1, repair success rate, capability coverage represented as a fraction.

`Score = 100 * clamp(raw_fraction, 0, 1)`

**B. Bounded error/failure fractions where 0.0 is ideal**

Examples: silent bug rate, hallucination rate, failure rate, rule-exception density when represented as a fraction.

`Score = 100 * (1 - clamp(raw_fraction, 0, 1))`

**C. Positive lower-is-better quantities**

Examples: execution time, compile time, startup latency, memory, source bytes, binary size, deployment footprint, source tokens, total tokens, semantic branching, required semantic lookups, hidden semantic counts, and semantic complexity units per supported capability.

For the same metric and workload across the fixed comparison set:

`Score_i = 100 * best_positive_raw / raw_i`

where `best_positive_raw` is the smallest valid positive raw value among applicable languages.

If a legitimate exact zero is possible for that metric, use the predeclared shifted form:

`Score_i = 100 * (best_raw + epsilon) / (raw_i + epsilon)`

and define `epsilon` from measurement resolution before running the benchmark.

Do not introduce logarithmic scaling unless this specification explicitly requires it for that metric. Semantic Determinacy is the explicit exception: its raw aggregate is `mean(log2(B_i))` before lower-is-better normalization.

**Known property of family C, and the reporting it requires.**

Family C is a hyperbola, so it compresses hard. A language twice as slow as the best scores 50; ten times slower scores 10; fifty times slower scores 2. On metrics whose raw values span two or more orders of magnitude across the fixed comparison set, this places most languages inside a few points of zero.

This is a property of the specified formula, not a defect to be worked around at scoring time. Do not switch families to recover the lost resolution.

Whenever a family-C metric's applicable raw values span a factor of 100 or more, publish, beside the normalized score:

- the raw value in its natural unit for every language;
- the ratio `raw_i / best_positive_raw` for every language;
- an explicit note that the normalized score for this metric is compressed and that the ratios, not the scores, carry the comparison between the non-leading languages.

**D. Positive higher-is-better quantities without a natural 0-1 bound**

Examples include Semantic Density when represented as semantic facts per source token.

`Score_i = 100 * raw_i / best_raw`

where `best_raw` is the largest valid applicable raw value.

**E. Repair-iteration count with the fixed three-repair budget**

For successful trials:

`Repair Efficiency = 100 * (1 - repair_turns / 3)`

clipped to `[0, 100]`.

A Correct@1 success has zero repair turns and therefore receives 100 for repair efficiency. A trial still incorrect after the third repair receives 0 for repair efficiency and is also a repair failure.

When aggregating trials, average trial-level Repair Efficiency scores. Keep Repair Success as a separate metric.

**F. Ordered defect-detection stage**

Use the fixed stage scores:

| Earliest observable stage | Score |
|---|---:|
| Compile-time Detection | 100 |
| Static Checking Before Execution | 90 |
| Runtime Safe Detection | 75 |
| Test Failure | 55 |
| Output Verification | 40 |
| Crash | 20 |
| Undefined Behavior | 5 |
| Silent Bug | 0 |

Average over the fixed adversarial case set.

**G. Objective rubric/proxy metrics**

Metrics such as Readability, Diagnostics, Portability Design / Platform Neutrality, FFI / Interoperability Design, Concurrency, Core Tooling Availability / Quality, Package / Dependency Management Quality, IDE / Editor Support Quality, Debugger / Profiler Support, Build / Test Integration, Documentation Quality, Installation / Distribution Experience, Toolchain Stability / Release Maturity, Implemented Platform Coverage, External Integration Coverage, Third-party Library Availability / Domain Coverage, Package Ecosystem Activity / Maintenance, Third-party Tool Availability, Production Adoption / Deployment Evidence, and Community / Public Knowledge Availability that cannot be reduced honestly to one direct physical quantity must use a published objective rubric or proxy with explicit observable criteria.

The rubric, its levels, and its conversion to 0-100 must be stored before any language is scored. The same rubric must be applied unchanged to all languages. Unsupported capabilities intentionally covered by the rubric receive the rubric-defined low score rather than N/A.

### 25.2 Cross-workload aggregation

Normalize at the lowest comparable workload level first.

When a metric has multiple benchmark workloads, aggregate language scores using the **unweighted arithmetic mean** across the fixed applicable workload set unless this specification explicitly defines a different aggregation.

Do not weight a workload more heavily because it favors or disfavors a language.

For repeated timing/resource measurements within one workload, use the **median** as the representative raw value and preserve all individual runs plus dispersion.

### 25.3 Fixed clipping and precision

All normalized scores are clipped to `[0, 100]`.

Retain raw calculations at full available precision. Report normalized metric and evaluation scores to **two decimal places**. Ranking comparisons use the unrounded values; displayed rounding must not decide ties.

### 25.4 No post-result formula selection

All final normalized scores must satisfy:

**100 = best**  
**0 = worst**

The formulas above are the defaults and are part of this specification. A metric-specific formula may override them only when that formula is already explicitly written in this file before benchmark execution.

Do not select min-max scaling, logarithmic scaling, percentile scaling, winsorization, clipping thresholds, alternative weights, or other transformations after observing results.

The normalization families of §25.1 are fixed. Any change to one of them must be registered before the run it first applies to, and that run must publish its results under both the old and the new formula, so that the effect of the change is visible rather than absorbed into the scores.

Do not modify formulas, weights, metric definitions, trial counts, or aggregation rules after observing results in order to benefit any language.

Do not create a normalization formula whose only purpose is to improve Quidra's relative position.

---

# 26. N/A Policy

Do not automatically convert `N/A` into a score of zero.

If a metric is genuinely not applicable, exclude that metric from the applicable-weight denominator and renormalize the remaining weights.

However, distinguish:

**not applicable**

from:

**the language lacks a capability that the metric is intentionally testing.**

A missing capability must not automatically escape scoring by being labeled `N/A`.

For Semantic Compression Capability Coverage, an unsupported capability in the fixed universe remains in the denominator and therefore lowers coverage rather than becoming N/A.

Document the reason for every `N/A`.

`Not Executed` is never `N/A`. Lack of time, compute budget, tool availability during the run, unfinished harness work, or an aborted measurement does not make an applicable metric inapplicable.

Do not renormalize weights around `Not Executed` evidence. If a required applicable metric is Not Executed, apply the primary-evaluation publication gate in Section 4.1.

---

# 32. Prohibited Practices

Do not:

- alter conditions to make Quidra score higher
- derive Semantic Compression probes from Quidra's current syntax or feature set
- remove a capability from Semantic Compression because Quidra lacks it
- create workloads deliberately favorable to one language
- create workloads deliberately unfavorable to one language
- modify weights after seeing results
- fabricate measurements
- report unexecuted benchmarks as executed
- hide failed trials
- hide inconvenient raw data
- classify a Silent Bug as success
- manually repair LLM-generated code and count it as LLM success
- mix previous benchmark results with current results
- modify Quidra during benchmarking
- commit or push benchmark results
- create a cross-evaluation weighted overall score or overall ranking
- publish a primary Overall Score or Ranking from a `PARTIAL`, `WITHDRAWN`, or `NOT EXECUTED` evaluation
- compare Semantic Compression fact counts produced from different semantic-site row sets or annotation depths
- begin Learnability scored trials before the Reference Pack leakage audit passes

---


## Runner-owned aggregation and ranking

Leaf workers produce validated requirement-level evidence only. Primary score formulas and language rankings are calculated mechanically by `benchmark.py aggregate-primary` from the frozen aggregation configuration. A COMPLETE, scientifically scoreable evaluation therefore cannot omit its ranking. Rankings are withheld only when a required gate fails or applicable work is incomplete/blocked.
