# Quidra Comprehensive Benchmark Specification

This file is the **single authoritative specification** for running the Quidra benchmark suite.

When instructed to run the benchmark using this file, read this document first and execute the benchmark according to it without requiring additional clarification unless execution is genuinely impossible.

The purpose of this benchmark is **not to make Quidra score highly**.

The purpose is to compare Quidra against major programming languages as fairly as possible using **measured, reproducible, and auditable evidence**.

---

# 1. Evaluation Target

Quidra repository:

https://github.com/quidra-lang/quidra

The evaluation target is the state of the **local `develop` branch exactly as it exists when the benchmark begins**.

Do not synchronize Quidra with the remote repository.

Do not perform:

- `git pull`
- `git fetch`
- merge
- rebase
- remote synchronization

Before evaluation, record:

- current branch
- HEAD commit SHA
- working tree status
- operating system
- architecture
- CPU
- RAM
- relevant compiler versions
- relevant runtime versions
- other important benchmark environment information

Treat the local Quidra implementation as a fixed evaluation target.

**Do not modify the Quidra implementation during benchmarking.**

Benchmark-related changes must be limited to the `benchmark/` directory unless absolutely required for measurement infrastructure.

---

# 2. Handling `benchmark/`

This file,

`benchmark/master_prompt.md`

is the benchmark specification itself.

It may be revised between benchmark runs. Once a benchmark run begins, **do not delete, replace, regenerate, or rewrite this file until that run is complete**.

At benchmark start, define the run identity from the benchmark start date and the evaluated Quidra commit:

`YYYY-MM-DD-<Quidra-short-SHA>`

where `<Quidra-short-SHA>` is the output of `git rev-parse --short HEAD` for the evaluated local `develop` HEAD.

Create the active run directory directly under `benchmark/`:

`benchmark/YYYY-MM-DD-<Quidra-short-SHA>/`

Before the first measurement or scored model request, copy the exact benchmark specification used for the run to:

`benchmark/YYYY-MM-DD-<Quidra-short-SHA>/prompt.md`

The copied `prompt.md` is immutable and is the authoritative prompt for that run even if `benchmark/master_prompt.md` is revised later.

Completed benchmark runs are historical measurement evidence and must not be deleted, overwritten, regenerated in place, or mixed with a new run.

Completed run directories live directly under `benchmark/` in one of these two forms:

- `benchmark/YYYY-MM-DD-<Quidra-short-SHA>/`
- `benchmark/YYYY-MM-DD-<Quidra-short-SHA>(latest)/`

The only entries permitted directly under `benchmark/` are:

- `master_prompt.md`
- run directories matching one of the two forms above

Do not place methodology files, task definitions, prompt templates, tests, inputs, expected outputs, measurement or scoring scripts, utilities, scratch directories, result files, symlinks, or any other files or directories directly under `benchmark/`. Everything required to execute, audit, or reproduce a run belongs inside that run's directory.

The literal `(latest)` suffix marks the newest completed run. Exactly one completed run may have this suffix. When a newer run becomes complete, rename the previous latest directory to its unsuffixed name, then rename the new completed directory to add `(latest)`. A run must exist under only one of the two names; do not duplicate it.

Each completed run directory represents one immutable benchmark run and should preserve, as applicable:

- `prompt.md`, copied from the exact `master_prompt.md` used for the run
- results and score tables
- raw measurements and logs
- generated implementations
- LLM trials and complete repair histories
- charts and reports
- environment information
- evaluated Quidra HEAD SHA
- reference SHAs
- any run-specific methodology or configuration required to audit that run

A new benchmark must be executed in a disposable scratch copy of the entire repository at the exact local `develop` HEAD recorded at benchmark start. The scratch copy may exclude `.git`, but it must preserve repository-relative paths and benchmark infrastructure.

Only inside that disposable scratch copy may generated artifacts from a previous scratch run be removed so that the active scratch workspace contains only the new run's generated artifacts.

Do not mix measurements from different runs.

Each run directory must be self-contained. Put the methodology, task definitions, prompt templates, tests, inputs, expected outputs, measurement and scoring scripts, utilities, reproducibility documentation, and generated evidence used for that run inside its directory. Material may be copied forward from an older run only after it is checked against this specification; the copied version then belongs to the new run and does not create shared infrastructure at the `benchmark/` root.

After a benchmark is fully complete, its preserved run directory may be imported from the disposable scratch copy directly under `benchmark/` as a separate post-benchmark operation. Update the literal `(latest)` suffix only after the new run is complete. Do not commit or push while measurements are in progress.

Do not retain in a completed run directory:

- build caches
- unnecessary binaries
- temporary files
- redundant copies
- meaningless intermediate artifacts
- unrelated files

---

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

---

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
- smaller deployment footprint → higher Deployment Footprint Score
- fewer source tokens → higher Token Efficiency Score
- fewer repair iterations → higher Repair Efficiency Score
- fewer silent bugs → higher Silent Bug Resistance Score
- fewer semantic exceptions → higher Semantic Regularity-related Score
- lower Rule Exception Density → higher Rule Exception Density Score

Raw values themselves must still be preserved in their natural units.

Do not alter the meaning of raw measurements simply to make larger values look better.

Only the normalized **0–100 score** must always use:

**higher = better.**

`N/A` must remain `N/A` and must not be converted to zero automatically.

---


# 6. Three Independent Primary Evaluations

The benchmark must report three independent primary evaluations:

## Standard Evaluation

Measures how strong each language is when used appropriately by a competent developer using normal best practices.

## LLM Practical Effectiveness Evaluation

Measures how effectively the selected LLM can use the language as it actually exists today, including any advantage or disadvantage created by pretraining exposure, ecosystem prevalence, familiar syntax, and existing examples.

## LLM Intrinsic Learnability Evaluation

Measures how effectively the selected LLM can learn, apply, compose, and resist misremembering the language's rules when direct lexical and structural familiarity is deliberately reduced through controlled, reversible transformations.

Calculate exactly these three independent primary scores:

- **Standard Overall Score**
- **LLM Practical Effectiveness Score**
- **LLM Intrinsic Learnability Score**

Create a separate ranking for each score.

Do not average or merge the three scores into a single primary score or ranking. They answer different questions.

The two LLM scores are not called axes. Treat them the same way the Standard score is treated: as separate first-class benchmark results.

- **LLM Practical Effectiveness** asks: “Using this language today, with this model and its existing knowledge, how well does it work?”
- **LLM Intrinsic Learnability** asks: “When familiarity is controlled as far as practicable, how learnable and rule-consistent is this language for the model?”

Neither LLM score is a correction of the other.

---


## 6.1 Fixed LLM Execution Configuration

The following configuration applies to both **LLM Practical Effectiveness** and **LLM Intrinsic Learnability** unless a subtest explicitly overrides one field.

The primary LLM benchmark must use one exact model identity for all 10 languages. Record the provider, public model name, exact model/version identifier exposed by the provider, API or client version, and benchmark date. A model/version change requires a new benchmark run and must not be mixed into an existing run.

Primary LLM trials use these fixed defaults:

- initial independent trials per task/scenario/condition: **5**, allocated as defined below
- maximum repair turns after the initial generation: **3**
- temperature: **0**, when the interface exposes temperature
- top-p: **1.0**, when the interface exposes top-p
- deterministic seed: **0**, when the interface exposes a seed
- maximum output tokens per generation or repair turn: **16,384**, when the interface exposes an output-token limit
- maximum cumulative generated-output tokens per trial, including repairs: **65,536**, when enforceable
- context: a **fresh isolated conversation/session** for every initial trial
- cross-trial memory: **prohibited**
- human edits to generated source counted as LLM success: **prohibited**
- external web/search/document retrieval during primary generation: **prohibited**, except for benchmark-supplied specifications, references, source code, tests, and compiler/runtime diagnostics
- compiler, interpreter, test runner, and benchmark harness access: **allowed and identical in purpose across languages**

### Allocating the five trials

Five trials in every cell of both LLM tracks is several thousand generations.
Stating the number without stating where it is spent guarantees that every run
reports the same deviation, which makes the requirement decorative. The five are
therefore allocated as follows, and a run that follows this allocation has met
the requirement.

**Five independent trials are required** for each cell of:

- Practical Effectiveness Correct@1 on the primary implementation task, because
  this is the metric the largest weight rests on and the one whose variance
  matters most;
- any cell used to compute Prompt Robustness, since that metric is about
  stability and a single sample cannot express it.

**One trial per cell is sufficient**, and is what the specification asks for, in:

- every Intrinsic condition that is already replicated across seeds or
  transformation sets. There, replication is supplied by the seed count that
  §10.3 and §10.5 fix, and the subtest score is the seed mean. Five trials per
  seed would replicate replication.

Whatever allocation is used, the run must state the trial count actually
executed per cell, and must not present a single sample as though it carried the
precision of five. Where only one trial was run, differences of a few points
between languages are not resolved by the measurement and must not be described
as if they were.

If a provider or client does not expose one of temperature, top-p, seed, or token-limit controls, do not emulate it with a language-specific workaround. Record the field as **provider-controlled / unavailable**, preserve the provider defaults if known, and use the same interface/configuration for every language.

If deterministic decoding causes repeated trials to be byte-identical, preserve all five trials and report the duplication rate. Do not add ad-hoc prompt noise merely to force diversity. Prompt-robustness variations and Intrinsic transformation seeds remain separate controlled sources of variation.

Before the first scored LLM generation, write an immutable run configuration containing all fields above plus:

- exact system prompt
- tool permissions
- retry policy for provider/network failures
- timeout policy
- context-window limit
- specification/reference-pack token counts
- examples budget
- repair prompt template
- task ordering policy

This configuration must be identical across languages except for the benchmark-controlled language-specific material required by the task.

Provider failures, rate limits, transport failures, and infrastructure failures must be recorded separately from language/model failures and must not be silently converted into incorrect generations.

---

# 7. Standard Evaluation Fairness

For Standard Evaluation, use normal, idiomatic, production-reasonable best practices for each language.

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

**Standard Score = the capability of the language when used properly.**

---

# 8. Standard Score Metrics

Evaluate the following metrics.

All final normalized metric scores must range from 0 to 100, where 100 is best.

## Performance

- Native Execution Performance
- Interactive / Interpreter Performance
- Long-running Performance
- Compile / Build Performance
- Startup / REPL Latency

## Resource / Distribution

- Memory Efficiency
- Source Code Size
- Binary / Artifact Size
- Deployment Footprint
- Runtime Overhead

Source size, artifact size, and deployment footprint must remain separate metrics.

## Language / Development

- Code Efficiency / Conciseness
- Readability
- Functionality / Expressiveness
- Diagnostics
- Dependency Simplicity
- Portability
- FFI / Interoperability
- Concurrency

## Safety / Robustness

- Type Safety
- Memory Safety
- Runtime Safety
- Boundary Value Safety
- Adversarial Input Robustness
- Early Error Detection
- Debuggability
- Silent Bug Resistance
- Compiler / Interpreter Robustness

## Semantic Simplicity

- Semantic Regularity
- Rule Exception Density
- Context-sensitive Rule Count
- Implicit Behavior Count

For all normalized scores in this category, fewer irregularities or exceptions must produce a higher score.

For example:

**lower raw Rule Exception Density → higher Rule Exception Density Score**

## Maturity

- Ecosystem
- Tooling
- Library Availability

Use these metrics to calculate the:

**Standard Overall Score**

Do not fabricate human-study results for metrics such as Readability.

If an objective proxy is used, define it explicitly.

---



## 8.1 Fixed Standard Overall Weighting

The Standard Overall Score uses fixed category weights. Do not choose or tune these weights during a benchmark run.

| Standard category | Weight |
|---|---:|
| Performance | 20% |
| Resource / Distribution | 15% |
| Language / Development | 20% |
| Safety / Robustness | 25% |
| Semantic Simplicity | 15% |
| Maturity | 5% |

Within each category, every listed normalized metric has equal weight unless this specification explicitly defines a more specific sub-metric aggregation.

Calculate each category score as the arithmetic mean of its applicable normalized metrics, then calculate:

**Standard Overall Score = 0.20*Performance + 0.15*Resource + 0.20*LanguageDevelopment + 0.25*SafetyRobustness + 0.15*SemanticSimplicity + 0.05*Maturity**

Apply the N/A policy in Section 26 within the affected category first. If an entire category is genuinely N/A, renormalize the remaining category weights proportionally and document the reason. A missing capability intentionally tested by a category is not N/A.

Do not change category weights, metric membership, or within-category equal weighting after measurements begin.

---

# 9. LLM Practical Effectiveness Evaluation

During LLM evaluation, humans must not manually improve generated code and then count the result as an LLM success.

If generated code fails, provide the actual diagnostics, errors, or test results back to the LLM and allow the LLM itself to perform repairs.

Preserve the entire repair history.

Use the real language name, real syntax, real standard-library names, and real toolchain.

Existing pretraining exposure is deliberately included. Python, C++, Rust, and other widely represented languages are allowed to benefit from what the selected model already knows. Quidra is likewise evaluated with whatever prior knowledge the selected model actually has.

Measure:

- Generation Success Rate
- Compile / Parse Success Rate
- Correct@1
- Correct@N
- Test Pass Rate
- Repair Success Rate
- Repair Iterations
- Diagnosis Efficiency
- Silent Bug Resistance
- Syntax Hallucination Resistance
- Specification Compliance
- Prompt Robustness
- Unseen-case Generalization
- Source Token Efficiency
- Total Token Efficiency
- Generated Code Performance
- Generated Code Memory Efficiency
- Generated Code Compile Performance

Use these metrics to calculate the:

**LLM Practical Effectiveness Score**

At minimum, the final LLM Practical Effectiveness table must separately contain:

- LLM Generation Success
- LLM Compile Success
- LLM Correct@1
- LLM Repair Success
- LLM Repair Efficiency
- LLM Diagnosis Efficiency
- LLM Token Efficiency
- LLM Prompt Robustness
- LLM Unseen-case Generalization
- LLM Hallucination Resistance
- LLM Silent Bug Resistance
- LLM Generated Code Performance
- LLM Practical Effectiveness Score

A specification-assisted condition may be used as the normal practical prompt when that is how the benchmark is defined. If a no-specification condition is also run, report it as a practical diagnostic of prior knowledge; do not transform it into a familiarity-corrected primary score.

All normalized LLM scores must follow:

**100 = best, 0 = worst.**

---


## 9.1 Fixed LLM Practical Effectiveness Weighting

The primary LLM Practical Effectiveness Score uses the following fixed metric weights:

| Metric | Weight |
|---|---:|
| Generation Success Rate | 4% |
| Compile / Parse Success Rate | 4% |
| Correct@1 | 12% |
| Correct@N | 5% |
| Test Pass Rate | 10% |
| Repair Success Rate | 7% |
| Repair Efficiency | 5% |
| Diagnosis Efficiency | 5% |
| Silent Bug Resistance | 12% |
| Syntax Hallucination Resistance | 7% |
| Specification Compliance | 7% |
| Prompt Robustness | 5% |
| Unseen-case Generalization | 6% |
| Source Token Efficiency | 3% |
| Total Token Efficiency | 3% |
| Generated Code Performance | 3% |
| Generated Code Memory Efficiency | 1% |
| Generated Code Compile Performance | 1% |

The weights sum to 100%.

Where a final summary table uses a shorter presentation row set, preserve this full weighted breakdown in the detailed LLM results. The shorter table is presentation only and must not change the score calculation.

The twelve rows of the §28 presentation table map onto the eighteen weighted
metrics above as follows. This mapping is fixed so that two runs do not invent
different correspondences.

| §28 presentation row | §9.1 weighted metric it displays |
|---|---|
| LLM Generation Success | Generation Success Rate |
| LLM Compile Success | Compile / Parse Success Rate |
| LLM Correct@1 | Correct@1 |
| LLM Repair Success | Repair Success Rate |
| LLM Repair Efficiency | Repair Efficiency |
| LLM Diagnosis Efficiency | Diagnosis Efficiency |
| LLM Token Efficiency | Source Token Efficiency |
| LLM Prompt Robustness | Prompt Robustness |
| LLM Unseen-case Generalization | Unseen-case Generalization |
| LLM Hallucination Resistance | Syntax Hallucination Resistance |
| LLM Silent Bug Resistance | Silent Bug Resistance |
| LLM Generated Code Performance | Generated Code Performance |

Six weighted metrics have no presentation row and appear only in the full
breakdown: Correct@N, Test Pass Rate, Specification Compliance, Total Token
Efficiency, Generated Code Memory Efficiency, and Generated Code Compile
Performance. They still carry their weights.

Apply the N/A policy in Section 26 to genuinely inapplicable metrics. Do not relabel a failed or unsupported capability as N/A merely to remove its weight.

Do not alter these weights after any scored LLM output has been observed.

---

# 10. LLM Intrinsic Learnability Evaluation

## 10.1 Objective

The purpose of this evaluation is to reduce the advantage of having seen a language many times before and to test whether the model can learn and correctly apply the language's rules from a supplied specification.

It does not claim to mathematically remove pretraining. Structural similarities, general programming knowledge, tokenizer behavior, and learned abstractions cannot be erased completely.

The required claim is narrower:

**LLM Intrinsic Learnability is a familiarity-controlled, specification-grounded evaluation, not a proof of zero prior exposure.**

The model must not be told the real language name during transformed trials. Use a neutral identifier such as Language A, with the assignment randomized independently of presentation order.

This applies to the conditions that transform the surface: I1, I2, I3 and I6.
It cannot apply to I4 and I5, which deliberately keep the real surface because
they measure acquisition of a **new rule** rather than removal of familiarity;
anonymizing the surface underneath the overlay would mix I1's effect into I4's
and leave neither measurable. In I4 and I5 the model is still never *told* the
language name, but it can recognize the language, and the results for those two
subtests must be read with that stated rather than implied.

Every transformed program must still be validated by the real implementation after a published reversible mapping back to the actual language.

Run this evaluation for all 10 fixed languages.

## 10.2 Common controls

All transformed trials inherit the fixed LLM execution configuration in Section 6.1. If this section is stricter, this section takes precedence.

Keep these fixed or matched as closely as possible across all languages and transformed conditions:

- model and model version
- system prompt
- task semantics
- user prompt structure
- specification template
- specification token budget
- examples budget
- temperature and sampling settings
- repair budget
- total token budget
- test oracle
- source-to-source mapping validation
- number of random seeds

Use an **Intrinsic Reference Pack** for each language. It must describe exactly the subset of syntax and semantics needed by the tasks, including every transformed token or rule. Reference packs must use the same section template and a comparable token budget. Publish their token counts.

Do not silently give one language substantially more examples or explanation because its syntax is harder.

## 10.3 Controlled random lexicalization

Randomized transformed conditions must use deterministic recorded seeds.

Use at least **5 independent seeds** for each lexical-randomization condition.

Within a seed:

- the mapping is one-to-one;
- the same source token always maps to the same transformed token within that language and seed;
- transformed tokens must not collide with identifiers, literals, or each other;
- transformed tokens must not be prefixes of one another when that could change tokenization or parsing;
- prefer ASCII lowercase pseudo-words of comparable character length;
- reject obvious natural-language words and common programming terms;
- record actual model-tokenizer token counts when the tokenizer is available;
- match the transformed-token length/token-count distribution across languages as closely as practical;
- if exact tokenizer matching is impossible, publish the residual token-count differences.

Randomization must never change program semantics by accident.

## 10.4 Reversible transformation requirement

For every transformed condition, provide:

1. forward transformer,
2. inverse transformer,
3. transformation manifest,
4. deterministic seed,
5. round-trip tests,
6. positive compile/run fixtures,
7. negative fixtures where applicable.

The required invariant is:

**real source -> transformed source -> inverse mapping -> semantically identical real source**

The inverse-mapped source must be compiled, checked, and executed by the language's real toolchain.

A mapping defect is a benchmark infrastructure failure, not a language failure.

### Mandatory pre-flight validation

An infrastructure defect in this track does not announce itself. It arrives
looking exactly like a language failure: the cell compiles nothing, or fails a
check, and the language is scored zero for it. The following must therefore be
verified **before any Reference Pack is built and before any trial is scored**,
and the verification must be preserved as evidence:

1. **Every fixture compiles and runs on the real toolchain, and its output
   matches the expected output exactly.** A fixture is the source of the worked
   example shown to the model; a fixture that does not build teaches every trial
   in that language to reproduce something that cannot build.
2. **Every harness convention the task does not state is satisfied by the
   harness, not demanded of the model.** If the prompt withholds a fact — for
   instance because stating it would reveal the language — the model cannot be
   marked down for not knowing it. File naming and entry-point naming are the
   usual cases.
3. **Every validator can both pass and fail.** For each check, exercise a
   correct input that must pass and a corrupted input that must fail. A
   validator that cannot pass any input, or cannot reject any input, is measuring
   nothing. This applies with particular force to conditions whose mapping is a
   permutation of the language's own vocabulary, where a "did any transformed
   token survive?" test is vacuous by construction.

An all-zero or near-all-zero row for one language, one condition, or one
validator is to be treated as a suspected infrastructure defect and investigated
before it is reported as a result. If investigation confirms it is a genuine
language outcome, record the evidence that confirmed it.

Defects found during a run are fixed, the affected cells re-run or re-verified,
and both the defect and the fix recorded. They are not silently corrected.

## 10.5 Intrinsic subtests

The primary Intrinsic score consists of six independently reported subtests.

### I1. Keyword Anonymization — 20%

Replace language keywords and grammar-significant word tokens with controlled pseudo-words.

Examples include constructs equivalent to conditional branches, loops, returns, declarations, matching, and imports.

Do not rename ordinary user identifiers merely to make the task harder.

Purpose:

**measure rule learning when familiar keyword recall is removed.**

Run at least 5 seeds and score the seed mean.

### I2. Vocabulary Anonymization — 20%

Apply I1 and additionally anonymize the standard-library and builtin names required by the benchmark tasks.

Examples include operations equivalent to output, length, range construction, sorting, collection helpers, and relevant standard namespaces.

Purpose:

**measure whether the model can learn the language/API vocabulary from specification rather than recall familiar names.**

Run at least 5 seeds and score the seed mean.

### I3. Structural Surface Perturbation — 15%

Apply a reversible unfamiliar surface form to selected grammar structure while preserving the underlying language semantics.

Examples may include controlled replacement of grouping delimiters, declaration separators, block markers, call argument separators, and selected operator spellings.

The transformation budget must be matched across languages. Do not redesign one language more aggressively than another.

Purpose:

**measure whether the model can follow an explicitly described grammar instead of replaying a familiar source-code shape.**

Use at least 3 independently generated transformation sets; 5 are preferred.

### I4. Novel-rule Generalization — 20%

Add a small synthetic overlay rule that the model could not have learned as a property of the original language because the rule is created for this benchmark run.

The overlay must:

- be described only in the Intrinsic Reference Pack;
- be mechanically validated before inverse mapping;
- leave the underlying program semantics unchanged after validation/removal;
- impose equivalent reasoning difficulty across languages.

Examples include a deterministic marker requirement tied to mutability, return category, or another statically observable property.

Purpose:

**measure whether the model can acquire a genuinely new rule from specification and apply it correctly.**

Use multiple rule sets and seeds. Publish every rule set.

### I5. Held-out Rule Composition — 15%

Teach several rules individually in the specification/examples, but withhold examples containing their important combinations.

The test tasks must require combinations such as rule A + rule C, rule B + rule C, and rule A + rule B + rule C.

Purpose:

**measure compositional generalization rather than memorization of isolated examples.**

The held-out combinations must be fixed before observing results.

### I6. Prior-conflict Resistance — 10%

Construct a reversible counterfactual mapping in which familiar-looking tokens or surface cues intentionally conflict with their usual learned role.

For example, a permutation may assign familiar grammar words to different grammar roles in the transformed language, while the supplied specification states the new mapping unambiguously.

Do not use semantic traps that alter the actual program result; the inverse mapper must restore the original real source.

Purpose:

**measure whether the model follows the supplied specification when prior lexical expectations point elsewhere.**

Use multiple deterministic mappings and report failure modes separately.

## 10.6 Scoring

For each I1-I6 condition, evaluate the applicable LLM metrics using the same definitions used in Practical Effectiveness where possible:

- compile / parse success
- Correct@1
- Correct@N
- test pass rate
- repair success
- repair efficiency
- specification compliance
- hallucination resistance
- silent-bug resistance
- unseen-case generalization
- source and total token efficiency

Calculate a **Condition Task Effectiveness Score** on the same 0-100 direction using these fixed weights:

| Intrinsic condition metric | Weight |
|---|---:|
| Compile / Parse Success | 7% |
| Correct@1 | 18% |
| Correct@N | 8% |
| Test Pass Rate | 15% |
| Repair Success | 8% |
| Repair Efficiency | 6% |
| Specification Compliance | 10% |
| Hallucination Resistance | 8% |
| Silent Bug Resistance | 10% |
| Unseen-case Generalization | 7% |
| Source Token Efficiency | 1% |
| Total Token Efficiency | 2% |

The weights sum to 100%. Do not tune them after observing results.

For each randomized subtest:

1. calculate the score independently for each seed;
2. report every seed;
3. report mean;
4. report standard deviation;
5. report minimum and maximum;
6. use the mean as that subtest's primary score.

Then calculate:

**LLM Intrinsic Learnability Score = 0.20*I1 + 0.20*I2 + 0.15*I3 + 0.20*I4 + 0.15*I5 + 0.10*I6**

These weights are fixed before the benchmark results are observed.

Do not tune the weights after seeing which language benefits.

Report each I1-I6 score separately in addition to the aggregate.

## 10.7 Familiarity-drop diagnostics

For I1 and I2, also report:

**Practical baseline score - transformed score**

for each language.

This drop is diagnostic only. It is not itself the Intrinsic score and must not be used as a correction factor.

A small drop can mean the model learned the transformed specification well; it does not prove the model had no structural prior.

## 10.8 Interpretation

The two LLM primary results intentionally answer different questions:

- **LLM Practical Effectiveness Score** includes real-world model familiarity and measures present-day usefulness.
- **LLM Intrinsic Learnability Score** uses controlled unfamiliarization and novel-rule tests to measure specification-grounded learnability and rule consistency.

Do not merge them.

A language may legitimately rank high on one and lower on the other.

---

# 11. Quidra Native and Interpreter Evaluation

Quidra must be evaluated in both:

## Compiled / Native Mode

and

## Interpreter / REPL Mode

For Native mode, measure:

- Execution Time
- CPU Time
- Peak RSS
- Compile Time
- Compile + Execute Time
- Startup Time
- Binary Size

For Interpreter / REPL mode, measure:

- Interpreter Startup Time
- First Response Latency
- Repeated Expression Latency
- Algorithm Execution Time
- Memory Usage
- Long-running Workload Performance
- Error Recovery

In detailed raw results, keep:

- Quidra Native
- Quidra Interpreter

separate.

In the final language-level comparison table, Quidra must appear as one language.

Define the aggregation rule for combining Native and Interpreter evidence into Quidra's final metric scores **before examining benchmark results**.

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

Perform multiple runs.

Preserve:

- individual measurements
- representative statistic
- variability / dispersion

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

# 20. Semantic Regularity

Measure how often each language requires reasoning of the form:

**“The normal rule is A, except in this case where the rule becomes B.”**

Include at least:

- implicit conversions
- context-dependent meaning
- special-case syntax
- operator exceptions
- scope exceptions
- initialization exceptions
- argument-passing exceptions
- ownership / mutation exceptions
- naming exceptions
- standard-library convention exceptions

Define the semantic-rule counting methodology **before counting results**.

Where feasible, calculate:

`Rule Exception Density = Exception or Special-case Rules / Total Semantic Rules`

Also record:

- Total Semantic Rules
- Exception / Special-case Count
- Context-sensitive Rule Count
- Implicit Behavior Count

Do not score this category from intuition alone.

Because all final scores use higher-is-better direction:

**lower raw Rule Exception Density must produce a higher normalized score.**

---

# 21. Unseen-case Generalization

Provide the LLM with:

- part of the language specification
- a limited number of examples

Then test cases not directly demonstrated in those examples, including:

- new rule combinations
- boundary cases
- nested expressions
- type combinations
- error cases
- API combinations

Measure how reliably the LLM derives correct behavior from the known rules.

Score this as:

**LLM Unseen-case Generalization**

---

# 22. Prompt Robustness

For the same semantic task, create equivalent prompt variations.

Vary aspects such as:

- wording
- sentence order
- concise vs verbose phrasing
- formatting

Keep the requested behavior unchanged.

Use the same variation set for every language.

Measure whether output correctness remains stable.

Score this as:

**LLM Prompt Robustness**

---

# 23. Complete Preservation of LLM Trials

Do not preserve only successful final programs.

For every LLM trial, preserve:

- exact input prompt
- initial output
- initial source code
- compile / parse result
- runtime result
- test result
- diagnosis
- repair prompt
- `repair_01`
- `repair_02`
- `repair_03`
- additional repairs if allowed
- final success
- final failure
- token usage
- error logs

If a version contains a Silent Bug, preserve that version as part of the trial history.

A third party must be able to reconstruct:

**prompt → generated program → failure → diagnosis → repair → final result**

---

# 24. Prompt Preservation

Preserve all prompts actually used during LLM evaluation.

This includes:

- system prompts
- user prompts
- supplied language specifications
- examples
- Specification → Implementation prompts
- Reference → Porting prompts
- repair prompts
- adversarial-test prompts
- unseen-case prompts
- prompt-robustness variations

For each trial, preserve enough information to reconstruct the exact final input presented to the model.

This file,

`benchmark/master_prompt.md`

is the authoritative specification for future runs and may be revised between runs. For a completed run, that run's immutable `prompt.md` is the authoritative copy of the specification actually used.

---

# 25. Scoring Methodology

The scoring pipeline must be:

**Raw Data → Normalization → Metric Score → Overall Score**

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

Examples: compile success rate, test pass rate, Correct@1, repair success rate.

`Score = 100 * clamp(raw_fraction, 0, 1)`

**B. Bounded error/failure fractions where 0.0 is ideal**

Examples: silent bug rate, hallucination rate, failure rate, rule-exception density when represented as a fraction.

`Score = 100 * (1 - clamp(raw_fraction, 0, 1))`

**C. Positive lower-is-better physical/resource quantities**

Examples: execution time, compile time, startup latency, memory, source bytes, binary size, deployment footprint, source tokens, total tokens.

For the same metric and workload across the fixed comparison set:

`Score_i = 100 * best_positive_raw / raw_i`

where `best_positive_raw` is the smallest valid positive raw value among applicable languages.

If a legitimate exact zero is possible for that metric, use the predeclared shifted form:

`Score_i = 100 * (best_raw + epsilon) / (raw_i + epsilon)`

and define `epsilon` from measurement resolution before running the benchmark.

Do not introduce logarithmic scaling unless this specification explicitly requires it for that metric.

**Known property of family C, and the reporting it requires.**

Family C is a hyperbola, so it compresses hard. A language twice as slow as the
best scores 50; ten times slower scores 10; fifty times slower scores 2. On
metrics whose raw values span two or more orders of magnitude across the fixed
comparison set — native execution time and binary size are the usual ones — this
places most of the ten languages inside a few points of zero, and the resulting
metric score is close to a restatement of "how near the single best language is
this". Genuine differences between the slower languages survive in the raw values
but not in the normalized score.

This is a property of the specified formula, not a defect to be worked around at
scoring time. Do not switch families to recover the lost resolution.

Instead, whenever a family-C metric's applicable raw values span a factor of 100
or more, the run must publish, beside the normalized score:

- the raw value in its natural unit for every language;
- the ratio `raw_i / best_positive_raw` for every language;
- an explicit note that the normalized score for this metric is compressed and
  that the ratios, not the scores, carry the comparison between the
  non-leading languages.

**D. Positive higher-is-better quantities without a natural 0-1 bound**

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

Metrics such as Readability, Diagnostics, Portability, Interoperability, Concurrency, Ecosystem, Tooling, and Library Availability that cannot be reduced honestly to one direct physical quantity must use a published objective rubric or proxy with explicit observable criteria.

The rubric, its levels, and its conversion to 0-100 must be stored before any language is scored. The same rubric must be applied unchanged to all languages. Unsupported capabilities intentionally covered by the rubric receive the rubric-defined low score rather than N/A.

### 25.2 Cross-workload aggregation

Normalize at the lowest comparable workload level first.

When a metric has multiple benchmark workloads, aggregate language scores using the **unweighted arithmetic mean** across the fixed applicable workload set unless this specification explicitly defines a different aggregation.

Do not weight a workload more heavily because it favors or disfavors a language.

For repeated timing/resource measurements within one workload, use the **median** as the representative raw value and preserve all individual runs plus dispersion.

### 25.3 Fixed clipping and precision

All normalized scores are clipped to `[0, 100]`.

Retain raw calculations at full available precision. Report normalized metric and overall scores to **two decimal places**. Ranking comparisons use the unrounded values; displayed rounding must not decide ties.

### 25.4 No post-result formula selection

All final normalized scores must satisfy:

**100 = best**  
**0 = worst**

The formulas above are the defaults and are part of this specification. A metric-specific formula may override them only when that formula is already explicitly written in this file before benchmark execution.

Do not select min-max scaling, logarithmic scaling, percentile scaling, winsorization, clipping thresholds, alternative weights, or other transformations after observing results.

The normalization families of §25.1 are fixed. Any change to one of them must be registered before the run it first applies to, and that run must publish its results under both the old and the new formula, so that the effect of the change is visible rather than absorbed into the scores.

Do not modify formulas, weights, metric definitions, trial counts, or aggregation rules after observing results in order to benefit any language.

---

# 26. N/A Policy

Do not automatically convert `N/A` into a score of zero.

If a metric is genuinely not applicable, exclude that metric from the applicable-weight denominator and renormalize the remaining weights.

However, distinguish:

**not applicable**

from:

**the language lacks a capability that the metric is intentionally testing.**

A missing capability must not automatically escape scoring by being labeled `N/A`.

Document the reason for every `N/A`.

---

# 27. Standard Final Comparison Table

Produce a final table with the following fixed columns:

| Metric | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Native Performance | | | | | | | | | | |
| Interactive Performance | | | | | | | | | | |
| Long-running Performance | | | | | | | | | | |
| Compile / Build Performance | | | | | | | | | | |
| Startup / REPL Latency | | | | | | | | | | |
| Memory Efficiency | | | | | | | | | | |
| Source Code Size | | | | | | | | | | |
| Binary / Artifact Size | | | | | | | | | | |
| Deployment Footprint | | | | | | | | | | |
| Runtime Overhead | | | | | | | | | | |
| Code Efficiency / Conciseness | | | | | | | | | | |
| Readability | | | | | | | | | | |
| Functionality / Expressiveness | | | | | | | | | | |
| Diagnostics | | | | | | | | | | |
| Dependency Simplicity | | | | | | | | | | |
| Portability | | | | | | | | | | |
| Interoperability | | | | | | | | | | |
| Concurrency | | | | | | | | | | |
| Type Safety | | | | | | | | | | |
| Memory Safety | | | | | | | | | | |
| Runtime Safety | | | | | | | | | | |
| Boundary Value Safety | | | | | | | | | | |
| Adversarial Input Robustness | | | | | | | | | | |
| Early Error Detection | | | | | | | | | | |
| Debuggability | | | | | | | | | | |
| Silent Bug Resistance | | | | | | | | | | |
| Semantic Regularity | | | | | | | | | | |
| Rule Exception Density | | | | | | | | | | |
| Implementation Robustness | | | | | | | | | | |
| Ecosystem / Tooling | | | | | | | | | | |
| **Standard Overall Score** | | | | | | | | | | |

Every value in this table that is a normalized score must follow:

**higher = better.**

Create an independent:

**Standard Ranking**

based on Standard Overall Score.

---


# 28. LLM Final Comparison Tables

Produce a primary **LLM Practical Effectiveness** table using the fixed language columns:

| Metric | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| LLM Generation Success | | | | | | | | | | |
| LLM Compile Success | | | | | | | | | | |
| LLM Correct@1 | | | | | | | | | | |
| LLM Repair Success | | | | | | | | | | |
| LLM Repair Efficiency | | | | | | | | | | |
| LLM Diagnosis Efficiency | | | | | | | | | | |
| LLM Token Efficiency | | | | | | | | | | |
| LLM Prompt Robustness | | | | | | | | | | |
| LLM Unseen-case Generalization | | | | | | | | | | |
| LLM Hallucination Resistance | | | | | | | | | | |
| LLM Silent Bug Resistance | | | | | | | | | | |
| LLM Generated Code Performance | | | | | | | | | | |
| **LLM Practical Effectiveness Score** | | | | | | | | | | |

Create an independent **LLM Practical Effectiveness Ranking** based on LLM Practical Effectiveness Score.

Produce a second primary **LLM Intrinsic Learnability** table:

| Metric | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| I1 Keyword Anonymization | | | | | | | | | | |
| I2 Vocabulary Anonymization | | | | | | | | | | |
| I3 Structural Surface Perturbation | | | | | | | | | | |
| I4 Novel-rule Generalization | | | | | | | | | | |
| I5 Held-out Rule Composition | | | | | | | | | | |
| I6 Prior-conflict Resistance | | | | | | | | | | |
| I1 Familiarity Drop, raw diagnostic | | | | | | | | | | |
| I2 Familiarity Drop, raw diagnostic | | | | | | | | | | |
| **LLM Intrinsic Learnability Score** | | | | | | | | | | |

For I1-I6, preserve separate seed-level tables including seed identifiers, transformation manifests, means, standard deviations, minima, and maxima.

Every normalized score must follow **higher = better.**

Create an independent **LLM Intrinsic Learnability Ranking** based on LLM Intrinsic Learnability Score.

Do not merge Practical Effectiveness and Intrinsic Learnability. Keep both rankings separate.

---

# 29. Detailed Raw Benchmark Tables

In addition to normalized score tables, preserve raw benchmark tables.

For SVM, GMM, LightGrad, and other appropriate workloads, use structures such as:

| Language / Mode | Compile | Tests | Numerical Match | Execution Time | Memory | Source Tokens | Repair Count | Silent Bug |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Quidra Native | | | | | | | | |
| Quidra Interpreter | | | | | | | | |
| Python | | | | | | | | |
| C++ | | | | | | | | |
| Rust | | | | | | | | |
| Go | | | | | | | | |
| Java | | | | | | | | |
| TypeScript | | | | | | | | |
| Kotlin | | | | | | | | |
| Swift | | | | | | | | |
| Zig | | | | | | | | |

Raw tables must preserve actual units and must not be modified simply to follow the 100-is-best score convention.

---

# 30. Required `benchmark/` Contents

At completion, preserve at least the following inside the run directory.

## Specification

- `prompt.md`, copied from the exact `master_prompt.md` used for the run
- methodology
- scoring definitions
- benchmark conditions

## Results

- Summary
- Standard Score Table
- LLM Score Table
- Standard Ranking
- LLM Ranking
- Raw Results
- Native vs Interpreter Results
- Micro Benchmark Results
- SVM Results
- GMM Results
- LightGrad Results
- Adversarial / Safety Results
- Debuggability Results
- Semantic Regularity Results
- LLM Results
- LLM Repair Results
- Token Results
- Charts

## Source Code

- Micro Benchmark implementations for all 10 languages
- SVM implementations for all 10 languages
- GMM implementations for all 10 languages
- LightGrad common-subset implementations for all 10 languages
- Adversarial / Safety test programs

## LLM Evidence

- prompts
- supplied specifications
- initial generations
- failed generations
- repair generations
- final generations
- silent-bug generations
- token usage
- generation metadata

## Reproducibility

- test inputs
- expected outputs
- measurement scripts
- scoring / normalization scripts
- raw logs
- compiler logs
- runtime logs
- environment information
- compiler versions
- runtime versions
- immutable LLM run configuration
- exact LLM model/version identifier and provider/client metadata
- normalization formulas and fixed weights
- Quidra HEAD SHA
- SVM reference SHA
- GMM reference SHA
- LightGrad reference SHA
- README with rerun instructions

Use machine-readable formats such as CSV or JSON for raw data where practical.

Also provide human-readable summaries such as Markdown tables and charts where useful.

An Excel workbook may also be generated when useful, but raw machine-readable evidence must not exist only inside Excel.

---

# 31. Git Operations

Do not:

- commit
- push
- merge
- rebase

Benchmark results must remain only in the local working tree.

Do not modify the Quidra implementation.

As a general rule, only files under `benchmark/` may be created, deleted, or updated.

---

# 32. Prohibited Practices

Do not:

- alter conditions to make Quidra score higher
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

---


# 33. Completion Criteria

The benchmark is complete only when all applicable items below are satisfied:

1. All 10 fixed languages were evaluated.
2. Quidra Native was measured.
3. Quidra Interpreter / REPL was measured.
4. Micro Benchmarks were executed.
5. SVM Benchmark was executed.
6. GMM Benchmark was executed.
7. LightGrad Benchmark was executed.
8. Adversarial / Safety Tests were executed.
9. Early Error Detection was evaluated.
10. Debuggability was evaluated.
11. Semantic Regularity was evaluated.
12. Unseen-case Generalization was evaluated.
13. Prompt Robustness was evaluated.
14. Standard metric scores were calculated.
15. LLM Practical metric scores were calculated.
16. LLM Intrinsic subtest scores were calculated.
17. Standard Overall Score was calculated.
18. LLM Practical Effectiveness Score was calculated.
19. LLM Intrinsic Learnability Score was calculated.
20. Standard Ranking was created.
21. LLM Practical Effectiveness Ranking was created.
22. LLM Intrinsic Learnability Ranking was created.
23. Raw Data was preserved.
24. Scoring formulas and weights were preserved.
25. All actual LLM prompts were preserved.
26. Initial generations and complete repair histories were preserved.
27. Source code for all 10 languages was preserved.
28. Reference SHAs were preserved.
29. Environment information was preserved.
30. Previous completed benchmark run directories were preserved unchanged.
31. The active run directory was self-contained, held the current run's infrastructure and generated artifacts, and was assigned a distinct `YYYY-MM-DD-<Quidra-short-SHA>` identity directly under `benchmark/`.
32. The benchmark is reproducible.
33. The exact `benchmark/master_prompt.md` used for the run was copied to the run directory as immutable `prompt.md`, and `benchmark/master_prompt.md` remained intact during benchmark execution.
34. The Quidra implementation was not modified during benchmark execution.
35. No commit or push was performed during benchmark execution.
36. LLM Practical Effectiveness was measured for all 10 languages.
37. I1 Keyword Anonymization was measured for all 10 languages with at least 5 seeds.
38. I2 Vocabulary Anonymization was measured for all 10 languages with at least 5 seeds.
39. I3 Structural Surface Perturbation was measured for all 10 languages with at least 3 transformation sets.
40. I4 Novel-rule Generalization was measured for all 10 languages.
41. I5 Held-out Rule Composition was measured for all 10 languages.
42. I6 Prior-conflict Resistance was measured for all 10 languages.
43. Forward/inverse mappings, manifests, seeds, and round-trip validation were preserved, and the §10.4 pre-flight validation was performed and preserved: every fixture compiled, ran and matched its expected output; every harness-side convention the prompt withholds was satisfied by the harness; and every validator was shown able to both pass a correct input and reject a corrupted one.
44. Seed-level Intrinsic results, means, standard deviations, minima, and maxima were reported.
45. I1/I2 familiarity-drop diagnostics were reported separately from the Intrinsic score.
46. Intrinsic Reference Pack token counts and transformation-budget controls were published.
47. The immutable LLM run configuration was written before the first scored LLM generation.
48. The exact LLM model/version, decoding controls, repair budget, trial count, and token limits or provider-controlled status were preserved.
49. Standard, LLM Practical, and Intrinsic condition weights matched the fixed weights in this specification.
50. Every normalized score used the fixed normalization family or an explicitly pre-existing metric-specific override, with no post-result formula selection, and every family-C metric whose applicable raw values span a factor of 100 or more published its raw values and ratios alongside the compressed score.

If a technically impossible or unavailable item prevents completion, do not invent a result.

Record it explicitly as N/A or Not Executed with the exact reason.

---

# 34. Final Report

After completion, keep the chat response concise.

Report at least:

- evaluated Quidra HEAD SHA
- Standard Overall Score
- Standard Ranking
- LLM Practical Effectiveness Score
- LLM Practical Effectiveness Ranking
- LLM Intrinsic Learnability Score
- LLM Intrinsic Learnability Ranking
- major Quidra strengths
- major Quidra weaknesses
- any N/A or Not Executed items
- main files or directories created under benchmark/

Do not flood the final chat response with raw benchmark evidence.

Store detailed evidence under benchmark/.

---


# 35. Final Objective

Evaluate the Quidra implementation exactly as it exists on the local develop branch.

Compare it against the fixed set of Python, C++, Rust, Go, Java, TypeScript, Kotlin, Swift, and Zig under reproducible and fair conditions.

Measure not only raw runtime speed, but also the Standard metrics defined above, Practical LLM effectiveness with real-world familiarity included, and Intrinsic LLM learnability under controlled unfamiliarization.

The Intrinsic evaluation must include keyword anonymization, vocabulary anonymization, structural surface perturbation, novel-rule generalization, held-out rule composition, and prior-conflict resistance.

Calculate exactly three independent primary scores:

**Standard Overall Score**

**LLM Practical Effectiveness Score**

**LLM Intrinsic Learnability Score**

using reproducible evidence.

Create a separate ranking for each score. Do not average the three into a combined primary ranking.

For every normalized score:

**100 means best. 0 means worst. Higher is always better.**

Historical completed benchmark results must remain preserved as historical evidence. New completed results receive a distinct `YYYY-MM-DD-<Quidra-short-SHA>` run identity directly under `benchmark/`; do not overwrite an older completed run. Exactly one newest completed run carries the literal `(latest)` suffix.

At the `benchmark/` root, keep only `master_prompt.md` and the run directories. Store every other benchmark file and directory inside the applicable run directory.

Keep `benchmark/master_prompt.md` intact while executing a benchmark, and preserve the exact version used as `prompt.md` inside that run's directory. Revisions are allowed only between runs.

Do not modify Quidra itself during benchmark execution.

Do not commit or push anything while benchmark measurements are in progress.

---
