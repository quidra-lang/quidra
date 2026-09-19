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
- `version.txt`, recording the evaluated Quidra version (see below)
- results and score tables
- raw measurements and logs
- generated implementations
- LLM trials and complete repair histories
- charts and reports
- environment information
- evaluated Quidra HEAD SHA
- reference SHAs
- any run-specific methodology or configuration required to audit that run

### Required: `version.txt`

Every run directory must contain `version.txt`, written at benchmark start, before any measurement.

It records **what the results describe**, which is not necessarily what the repository contains when the
run is later read. A completed run may be filed, read, or compared months after it was measured, and by
then `develop` may be hundreds of commits ahead. A reader must never have to infer the evaluated version
from a directory name.

`version.txt` must record at minimum:

- the evaluated Quidra version, taken from `compiler_version` in `quidra.manifest.json` **at the
  evaluated commit** — not from a tag. A tag reachable from a commit is not necessarily the release that
  commit belongs to, because tags are often created later on the same line of development. Record a tag
  only as corroboration, and say that the manifest value is authoritative.
- the evaluated commit SHA (full and short), branch, commit date and subject
- the working-tree status at benchmark start
- the build commands used to produce the evaluated binary
- the version string the built binary itself reports
- the host and the frozen comparison toolchain versions

When the completed run is filed under `benchmark/`, append a second block recording the repository state
**at filing time**: the then-current commit, the then-current version, the number of commits since the
evaluated commit, and the implementation diff between them. If that count is non-zero, `version.txt` and
the run's final report must both state plainly that the results describe the evaluated version and not
the current one.

Every claim of the form "Quidra scores X" in a run's report means "Quidra `<evaluated version>` at
`<evaluated commit>` scores X", and the report must say so where a reader will see it.

A new benchmark must be executed in a disposable scratch copy of the entire repository at the exact local `develop` HEAD recorded at benchmark start. The scratch copy may exclude `.git`, but it must preserve repository-relative paths and benchmark infrastructure.

Only inside that disposable scratch copy may generated artifacts from a previous scratch run be removed so that the active scratch workspace contains only the new run's generated artifacts.

Do not mix measurements from different runs.

Each run directory must be self-contained. Put the methodology, task definitions, prompt templates, tests, inputs, expected outputs, measurement and scoring scripts, utilities, reproducibility documentation, and generated evidence used for that run inside its directory. Material may be copied forward from an older run only after it is checked against this specification; the copied version then belongs to the new run and does not create shared infrastructure at the `benchmark/` root.

Source implementations, task solutions, harness code, fixtures, and other generated artifacts from an older completed run may also be copied forward when they are still valid for the current task and specification. Reuse is allowed and is preferred over meaningless regeneration when it does not change what a metric measures. Every reused artifact must be identified in the new run and revalidated before use.

Reuse of an artifact never permits reuse of its old measurements or scores. In every new run, rebuild or recompile as applicable, execute correctness checks again, repeat every timing/resource/token measurement required by the current specification, preserve the new raw evidence, and calculate all scores from the new run's measurements.

LLM generation evaluations are the exception to source-solution reuse. Each scored initial LLM trial must still use the required fresh isolated session and must not be seeded with a prior run's generated solution or repair history unless a benchmark condition explicitly supplies the same prior artifact to every language being compared. This restriction protects what the LLM-generation metrics actually measure; it does not prohibit reuse of non-LLM benchmark infrastructure.

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

## 4.2 Pre-flight execution manifest and resumable run ledger

The benchmark is large enough that a run must be resumable without mixing evidence or silently skipping work.

Before the first measurement or scored model request, create a machine-readable execution manifest and run ledger inside the active run directory. The ledger must enumerate every required unit of work, including:

- every language × workload measurement;
- every Language Quality rubric / adversarial case;
- every Semantic Compression probe and semantic-site matrix validation;
- every Learnability language × condition × seed / transformation set;
- every Proficiency language × task × required independent trial;
- every validator self-test;
- every comparability / leakage audit; and
- every final aggregation and report-consistency check.

For each unit, record at least:

- stable work-unit ID;
- input / fixture hashes;
- relevant toolchain or model identity;
- benchmark prompt/configuration hash;
- status: `PENDING`, `RUNNING`, `COMPLETE`, `INVALID`, or `BLOCKED`;
- evidence paths; and
- validation result.

A work unit may become `COMPLETE` only after its raw evidence is written and its required validator passes. Write results atomically where practical. A process interruption must leave the unit resumable as `PENDING` or `RUNNING`, never falsely complete.

On resume:

1. re-read the immutable run `prompt.md`, execution manifest, and ledger;
2. verify the evaluated Quidra SHA, toolchain identities, model identity, prompt/configuration hashes, fixtures, and scoring rules still match;
3. reuse only `COMPLETE` work units whose hashes and validation evidence still match;
4. rerun incomplete, invalid, or mismatched work units; and
5. never import measurements or scored LLM generations from a different run identity.

If the exact primary LLM model/version changes, do not mix old and new LLM trials in one primary score. Start a new run or restart the affected LLM evaluation under one fixed model identity.

Before expensive scoring begins, complete **all infrastructure pre-flight work first**: build/reference validation, golden-output agreement, validator pass/fail self-tests, Semantic Compression matrix/comparability gates, Learnability transformation round trips, and Reference Pack leakage audits. Do not spend scored LLM trials on infrastructure that has not passed pre-flight.

Provider/network/rate-limit failures are infrastructure events, not model failures. Preserve them separately, leave the affected work unit incomplete, and resume it under the same fixed configuration rather than scoring the transport failure as an incorrect generation.

The ledger is execution state, not a score source. Final scores must still be recomputed from validated raw evidence after all required work units for that evaluation are complete.

## 4.3 Hard environment-readiness barrier

A benchmark must not begin scored execution merely because the repository builds. Before the first scored measurement or scored LLM request, complete one **all-or-nothing environment-readiness pre-flight** for the entire run and preserve its machine-readable result as `preflight/readiness.json`.

The readiness pre-flight must cover **all 10 fixed languages and every shared measurement dependency** that the run will need. At minimum, verify:

- the exact compiler/interpreter/runtime command for every language exists and reports its version;
- a minimal source file for every language can be compiled or checked as applicable, executed, and validated against an exact expected output;
- the Quidra native compiler and interpreter / REPL both build, start, execute a minimal valid program, and produce the expected output;
- every required reference repository / frozen source snapshot is locally available at the recorded SHA;
- every golden-output generator and validator needed by SVM, GMM, LightGrad, microbenchmarks, Semantic Compression, Learnability, Proficiency, and Language Quality evaluations can run;
- the wall-clock timer, CPU-time measurement mechanism, peak-RSS measurement mechanism, source-token counter, file-size measurement, and any other required measurement utility are available and pass a known-answer smoke test;
- required filesystem operations work in the active scratch copy: create, atomic replace/rename, hash, and cleanup;
- enough writable disk space exists for the planned run plus temporary build products; record free space before scored execution;
- the selected LLM provider/client is reachable through the exact interface that will be used for scoring, the exact model/version can be selected, and one **unscored** minimal request succeeds;
- before scored LLM work, derive and preserve `preflight/llm_capacity_estimate.json` from the frozen execution manifest and immutable LLM configuration. Record, per LLM evaluation and in total, the mandatory initial-trial count, the maximum allowed repair-turn count, the resulting minimum and configured-maximum generation-call counts, estimated model-visible input-token demand, configured output-token ceilings, fresh-session/context count, and every assumption used by the estimate;
- when the provider/client exposes remaining request, token, spend, usage, or other directly comparable hard quotas, record the observed values and timestamp in the capacity estimate and fail readiness if an exposed hard limit proves that the mandatory frozen plan cannot complete. If remaining quota is not exposed, record it explicitly as `UNAVAILABLE`; unavailability alone does not fail readiness and must not be replaced by a guessed quota. Rate limits are recorded separately from total-capacity limits;
- provider decoding controls, context limits, token accounting, retry behavior, and timeout behavior are recorded before scored LLM work;
- every required benchmark script imports/parses successfully and its CLI help or dry-run path can execute without starting scored work; and
- the execution manifest contains no unresolved placeholder paths, missing fixture hashes, unknown toolchain identities, or duplicate work-unit IDs.

If a required toolchain or measurement dependency is missing but can be installed or prepared without modifying the Quidra implementation, perform that setup **during readiness**, then rerun the readiness checks from the beginning and freeze the resulting toolchain identities. Do not start scored execution while setup is still changing.

The readiness barrier passes only when every required readiness check is `PASS`. A warning may be recorded only for information that cannot affect correctness, comparability, execution, or evidence preservation.

If any required readiness check remains failed:

1. do not start any scored work;
2. repair the environment or benchmark infrastructure;
3. rerun the complete readiness pre-flight; and
4. begin scored execution only after a clean all-`PASS` readiness result is preserved.

This barrier exists specifically to prevent discovering a missing compiler, broken validator, unavailable measurement utility, stale fixture, or unreachable LLM provider halfway through a run.

## 4.4 Deterministic process / timeout / recovery policy

Before scored execution, write one run-wide process policy and reference it from every executable work unit. Unless a workload explicitly freezes a stricter predeclared value, use these defaults:

- **compile/build/check timeout:** 900 seconds per invocation;
- **single workload execution timeout:** 300 seconds per invocation;
- **validator/scoring-script timeout:** 300 seconds per invocation;
- **LLM transport timeout:** use the provider/client timeout frozen in the immutable LLM configuration;
- **infrastructure retry count:** at most 2 retries after the original attempt, with the same inputs and configuration;
- **language/program failures:** never retry merely to improve the score; retry only when evidence identifies an infrastructure/transport failure rather than a deterministic program result.

A timeout limit may be increased for a legitimately longer benchmark only **before any language is measured for that workload**. The new limit must be recorded in the manifest and applied identically to every language for that workload.

For an infrastructure failure, timeout caused by the harness/provider, interrupted process, runner reset, or temporary resource error:

1. preserve the failed-attempt log as infrastructure evidence;
2. terminate the entire process tree for that work unit;
3. remove only that work unit's disposable temporary/build directory;
4. verify that its final evidence paths were not marked `COMPLETE`;
5. retry from the start of that work unit under identical frozen inputs/configuration; and
6. if retries are exhausted, leave the unit `BLOCKED` or `INVALID`; never manufacture a measurement.

Every executable work unit must use its own uniquely named temporary directory. Final evidence must be written atomically where practical. A work unit is never `COMPLETE` merely because its main process exited zero; its expected outputs, raw measurement cardinality, hashes, and validator results must all pass.

After any resume, crash, or retry, check for orphaned benchmark processes before continuing. Do not allow surviving processes from an earlier attempt to contaminate timing, memory, files, ports, caches, or outputs of later work.

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

# 6. Five Independent Primary Evaluations

The benchmark must report exactly five independent primary evaluations.

Their numbering reflects **Quidra's design priorities**, not scoring weights and not permission to favor Quidra. The scoring rules inside every evaluation must remain language-neutral, predeclared, reproducible, and equally applicable to all 10 languages.

The priority order is:

1. **Semantic Compression**
2. **LLM Learnability**
3. **Language Quality**
4. **Ecosystem**
5. **LLM Proficiency**

## Primary Evaluation 1 — Semantic Compression

Measures how much reliable, statically or locally recoverable meaning a language communicates per unit of surface syntax while minimizing ambiguity, hidden behavior, semantic lookup distance, and special-case rules.

Semantic Compression is not a short-code contest. A shorter program is not better when it hides more behavior or supports fewer capabilities.

## Primary Evaluation 2 — LLM Learnability

This is implemented by the **LLM Learnability Evaluation** in Section 10.

It measures how effectively the selected LLM can learn, apply, compose, and resist misremembering the language's rules when direct lexical and structural familiarity is deliberately reduced through controlled, reversible transformations.

## Primary Evaluation 3 — Language Quality

Measures how strong each language and its first-party / normally expected toolchain are as a practical general-purpose programming platform when used appropriately by a competent developer using normal best practices.

This evaluation includes performance, resource/distribution characteristics, language/development quality, safety/robustness, interoperability, and present-day toolchain/developer experience. It intentionally excludes external popularity, third-party ecosystem breadth, community scale, and production adoption, which belong to Primary Evaluation 4.

## Primary Evaluation 4 — Ecosystem

This is implemented by the **Ecosystem Evaluation** in Section 8.2.

It measures how much external ecosystem, community knowledge, third-party integration, and real-world adoption exist for the language today. This evaluation intentionally captures accumulated network effects and incumbency advantages rather than attributing them to language design or compiler quality.

## Primary Evaluation 5 — LLM Proficiency

This is implemented by the **LLM Proficiency Evaluation** in Section 9.

It measures how effectively the selected LLM can use the language as it actually exists today, including any advantage or disadvantage created by pretraining exposure, ecosystem prevalence, familiar syntax, and existing examples.

The benchmark defines exactly five independent primary scores:

- **Semantic Compression Overall Score**
- **LLM Learnability Score**
- **Language Quality Score**
- **Ecosystem Score**
- **LLM Proficiency Score**

Attempt all five evaluations, but publish a primary score and its separate ranking only when that evaluation is `COMPLETE` under Section 4.1. For any other status, publish the status and valid diagnostic evidence without a primary score or ranking.

**Do not calculate, publish, imply, or use any cross-evaluation weighted overall score or overall ranking.**

The five evaluations answer different questions. Combining them would require subjective cross-evaluation weights and would encode Quidra's priorities into the result.

The ordering of benchmark sections may be organizational, but the five numbered primary evaluations above are the authoritative priority order.

---

## 6.1 Semantic Compression Methodology

### 6.1.1 Core principle

Semantic Compression measures:

**how much important program meaning is communicated explicitly and determinately per unit of syntax, relative to the breadth of capabilities the language can actually express.**

The benchmark must not derive this evaluation from Quidra's existing operators, types, syntax, or feature set. Define the capability universe and probe set before scoring any language, and keep them identical for all 10 languages.

A capability that Quidra lacks must remain in the probe universe. Unsupported capabilities are not silently removed and are not automatically `N/A`.

### 6.1.1A Mandatory semantic-site matrix

The semantic unit must be fixed **before any language-specific annotation is performed**.

For every Semantic Compression probe, create a language-neutral **semantic-site matrix**. The matrix defines the only semantic facts that may contribute to cross-language counts for that probe.

Each row must contain at least:

- stable `site_id`;
- capability family;
- semantic question being tested;
- inclusion criterion;
- exclusion criterion;
- multiplicity rule;
- whether the row contributes to Semantic Density, Determinacy, Locality, Hidden Semantic Cost, Capability Efficiency, or more than one of them; and
- the rule for unsupported capabilities.

Typical semantic questions include binding, type constraints, storage / mutability, conversion, dispatch / lookup, control flow, failure behavior, ownership / reference behavior, allocation / lifetime, bounds or runtime checks, evaluation order, synchronization, effects / I/O, and implicit/default behavior. These are examples of semantic roles, not a permission to add language-specific rows after seeing syntax.

The matrix is defined from the capability and probe semantics, **not from any of the 10 implementations**. It must not contain language spellings, Quidra-specific constructs, or rows invented because one language exposes an implementation detail conveniently.

All 10 languages must then fill the **identical ordered set of `site_id` rows**.

Rules:

1. A language may not add a semantic-fact row that the other languages were not offered.
2. A language may not omit a row because the semantic fact is inconvenient, implicit, unsupported, or absent. Record the prescribed zero / unsupported / hidden-behavior state instead.
3. One matrix row contributes at most one unit unless its multiplicity rule was explicitly frozen before annotation.
4. A compound syntax form does not receive more facts merely because an annotator chose to describe it at finer granularity.
5. A concise syntax form does not lose facts merely because several frozen semantic sites are expressed by one token.
6. Hidden behavior is counted only through frozen rows/checklist items, never through free-form commentary.
7. If a semantic phenomenon cannot be represented consistently by the frozen matrix, stop and revise the matrix **before any score is observed**, then restart the affected annotation from the frozen version.

Before scoring, run a mechanical matrix validator that must confirm for every probe:

- identical `site_id` set and ordering across all 10 languages;
- no unknown or language-only rows;
- no missing required rows;
- valid values for every row;
- identical multiplicity rules;
- identical metric mapping; and
- identical capability denominator.

In addition, before normalization or ranking, perform a blinded cross-language comparability audit on a predeclared sample covering at least 20% of probes and every capability family. Any disagreement caused by annotation depth, row interpretation, or asymmetric treatment must be adjudicated against the frozen matrix and the affected annotations revalidated.

**If the matrix validator or comparability audit fails, Semantic Compression scoring must not start.** Raw annotations may be preserved for debugging, but Semantic Density, `Q`, Semantic Compression Overall Score, and Semantic Compression Ranking must remain unpublished until the gate passes.

The matrix, validator output, audit sample, disagreements, and resolutions must be preserved in the run directory.

### 6.1.2 Fixed language-neutral capability universe

The Semantic Compression probe set must cover, at minimum, these capability families:

1. value and storage declaration
2. initialization and potentially uninitialized state
3. mutation
4. aliasing, references, borrowing, addressing, and dereferencing where applicable
5. function calls and argument-passing semantics
6. return-value semantics
7. arithmetic operators
8. integer overflow, division-by-zero, and other numeric edge behavior
9. equality, ordering, and comparison
10. explicit and implicit type conversion
11. indexing and slicing
12. nullable / optional values
13. errors, failure values, exceptions, and propagation
14. alternatives / sum / variant types and dispatch over alternatives
15. generics / parametric polymorphism
16. collections and iteration
17. resource management, ownership, lifetime, destruction, or equivalent cleanup semantics
18. modules, imports, and dependency boundaries
19. concurrency / asynchronous execution where the language supports it
20. FFI / interoperability boundaries

The run may subdivide these families into fixed probes, but it must not add or remove probes after observing results.

Support is credited when the fixed probe can be expressed using documented language features or the language's normal standard runtime/library without benchmark-specific external packages or code generation. Partial support must be scored by a predeclared rubric rather than guessed after results are known.

### 6.1.3 Semantic facts inventory

For each fixed probe, the semantic-site matrix defines the semantic propositions that a competent reader or static tool would need to resolve. The shared taxonomy includes, as applicable:

- value versus storage
- mutability
- aliasing / writable aliasing
- initialization state
- type and representation
- conversion behavior
- possible failure
- alternative value cases
- overflow / exceptional numeric behavior
- allocation, copying, moving, borrowing, or destruction
- externally visible side effects
- control-flow effect
- lifetime / resource effect

The taxonomy is a classification vocabulary, **not a source of free-form fact counting**. Only frozen `site_id` rows may contribute facts.

For each language and each `site_id`, record exactly one semantic value plus one evidence state:

- `EXPLICIT_LOCAL` — the site's semantic value is determined by the probe's local source form under the language specification;
- `NONLOCAL` — the semantic value can be determined, but requires one or more external semantic lookups;
- `IMPLICIT` — the language imposes the behavior/default without a local source signal;
- `ABSENT` — the frozen semantic proposition does not occur for that supported probe under the matrix rule; or
- `UNSUPPORTED` — the language cannot express the capability represented by the row.

The semantic value may differ by language; the **row definition, evidence-state meanings, and counting rule may not**.

Free-form explanations, multiple prose observations about one row, AST-node count, specification paragraph count, or annotator verbosity never increase the fact count.

### 6.1.4 Required Semantic Compression metrics

Calculate and report all of the following normalized 0-100 metrics.

#### A. Semantic Density — 20% of quality score

Raw value:

**explicit local semantic sites / lexical source tokens**

where:

**explicit local semantic sites = number of frozen `site_id` rows whose evidence state is `EXPLICIT_LOCAL`**

Each `site_id` contributes at most one fact, except when the matrix froze an explicit multiplicity rule before any language was annotated. The numerator therefore cannot change because one language received a more detailed prose annotation than another.

Comments and whitespace do not count as source tokens. Use a documented language-neutral token-counting rule or a language lexer with an explicit reconciliation rule so punctuation-heavy and word-heavy syntaxes are treated consistently.

A single syntax token may legitimately make several frozen semantic sites explicit; that is semantic compression and those distinct predeclared sites each count once. Conversely, repeating or elaborating syntax for the same site does not create additional facts.

`NONLOCAL`, `IMPLICIT`, `ABSENT`, and `UNSUPPORTED` rows do not enter the Semantic Density numerator. Their effects are captured by Semantic Locality, Hidden Semantic Cost, Capability Coverage, or other applicable fixed metrics.

Higher raw density is better.

#### B. Semantic Determinacy — 25% of quality score

For each fixed probe, count the number `B_i` of materially different semantic interpretations that remain compatible with the local surface form before consulting external declarations or whole-program facts.

Record both the raw branching counts and:

**mean(log2(B_i))**

Lower semantic branching is better.

A materially different interpretation is one that can change mutation, aliasing, failure, conversion, dispatch, resource behavior, control flow, representation, or observable result.

A required argument-passing probe must include an ordinary call-site expression equivalent to `f(x)`. Measure how many materially distinct aliasing / mutation outcomes remain possible from the call-site syntax alone, then separately test any explicit reference, borrow, address, dereference, or writable-alias forms the language provides.

Do not award or remove points merely because a language uses the glyph `&`. Score the semantic determinacy conveyed by the complete surface form.

Apply the same method to other language-neutral families, including arithmetic operators, equality, conversions, indexing, optional/error handling, generic calls, assignment/update, and resource-affecting operations.

#### C. Semantic Locality — 20% of quality score

Measure how much non-local context must be inspected to determine the semantic facts of each probe.

Record the number of required external semantic lookups or declaration-graph hops, such as:

- callee signature
- variable declaration
- type declaration
- overload set
- trait / interface / protocol implementation
- imported definition
- global configuration or compiler mode

Lower lookup cost is better.

A lookup that is optional for extra detail must not be counted; count only context required to resolve a fact in the fixed inventory.

#### D. Hidden Semantic Cost — 20% of quality score

Using a fixed checklist, count semantically material behaviors that may occur without being signaled at the use site.

The checklist must include, where applicable:

- implicit conversions
- hidden writable aliasing or mutation
- overload / dynamic dispatch not locally evident
- implicit allocation, copy, move, destruction, or cleanup
- implicit exception / failure propagation
- hidden nullability or optionality
- implicit representation change
- context-dependent overflow / numeric behavior
- hidden side effects on existing state

Lower hidden semantic cost is better.

Do not count a behavior as hidden if the fixed local syntax makes that behavior unambiguous under the language specification.

#### E. Capability Efficiency — 15% of quality score

Measure semantic complexity required per supported capability.

For the fixed probe universe, record the number of distinct syntax forms, context-dependent rules, implicit rules, and documented semantic exceptions required to express and correctly interpret the supported probes, then divide by the number of supported capability points.

Lower semantic complexity units per supported capability are better.

This metric must be based on written language rules and reproducible probe evidence, not subjective impressions of elegance.

### 6.1.5 Capability Coverage

Calculate a separate **Capability Coverage Score `C`**:

**C = 100 * supported capability points / total fixed capability points**

The denominator is fixed before results are observed and includes capabilities a language does not support.

A tiny language therefore cannot obtain a high primary Semantic Compression score merely by having very few rules.

Report the coverage result by capability family in addition to the aggregate `C`.

### 6.1.6 Coverage-adjusted Semantic Compression score

First calculate the quality score `Q` from the five normalized quality metrics:

**Q = 0.20*SemanticDensity + 0.25*SemanticDeterminacy + 0.20*SemanticLocality + 0.20*HiddenSemanticCost + 0.15*CapabilityEfficiency**

Then calculate the primary score as the harmonic mean of quality and coverage:

**Semantic Compression Overall Score = 2*Q*C / (Q + C)**

If `Q + C = 0`, the score is 0.

Report all three values separately:

- Raw Semantic Compression Quality `Q`
- Capability Coverage `C`
- Semantic Compression Overall Score

The harmonic mean is fixed in advance to prevent either high semantic quality with trivial capability coverage or broad capability coverage with poor semantic quality from dominating the result.

Do not replace this formula with a weighted arithmetic mean after results are known.

### 6.1.7 Fairness and audit requirements

Before measuring Semantic Compression:

1. freeze the capability universe;
2. freeze every probe;
3. freeze the semantic-site matrix and all row-level inclusion, exclusion, and multiplicity rules;
4. freeze the hidden-behavior checklist;
5. freeze support / partial-support rubrics;
6. freeze token counting and normalization rules;
7. validate each probe against the real language implementation or authoritative specification where possible;
8. run the mechanical semantic-site matrix validator across all 10 languages; and
9. pass the predeclared cross-language comparability audit before calculating any normalized Semantic Compression metric.

Publish the probe definitions, raw counts, scoring scripts, capability matrix, and normalization calculations.

A language must be allowed to score well for semantics Quidra does not have. A language must also be allowed to score poorly on a capability Quidra implements well. Do not redesign the probe set in response to observed rankings.

---

## 6.2 Fixed LLM Execution Configuration

The following configuration applies to both **LLM Proficiency** and **LLM Learnability** unless a subtest explicitly overrides one field.

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

- Proficiency Effectiveness Correct@1 on the primary implementation task, because
  this is the metric the largest weight rests on and the one whose variance
  matters most;
- any cell used to compute Prompt Robustness, since that metric is about
  stability and a single sample cannot express it.

**One trial per cell is sufficient**, and is what the specification asks for, in:

- every Learnability condition that is already replicated across seeds or
  transformation sets. There, replication is supplied by the seed count that
  §10.3 and §10.5 fix, and the subtest score is the seed mean. Five trials per
  seed would replicate replication.

Whatever allocation is used, the run must state the trial count actually
executed per cell, and must not present a single sample as though it carried the
precision of five. Where only one trial was run, differences of a few points
between languages are not resolved by the measurement and must not be described
as if they were.

If a provider or client does not expose one of temperature, top-p, seed, or token-limit controls, do not emulate it with a language-specific workaround. Record the field as **provider-controlled / unavailable**, preserve the provider defaults if known, and use the same interface/configuration for every language.

If deterministic decoding causes repeated trials to be byte-identical, preserve all five trials and report the duplication rate. Do not add ad-hoc prompt noise merely to force diversity. Prompt-robustness variations and Learnability transformation seeds remain separate controlled sources of variation.

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

**Language Quality Score = the practical capability of the language when used properly.**

Language Quality must not be tuned to Quidra's design philosophy. If an established language objectively has better first-party or normally expected tooling quality, package/dependency workflow, IDE/editor integration, debugger/profiler support, build/test integration, documentation quality, installation/distribution experience, or toolchain stability, that advantage must be scored normally.

External library breadth, third-party integration count, community scale, public knowledge volume, and production adoption are not Language Quality metrics. They belong exclusively to Primary Evaluation 4 — Ecosystem, so incumbency and network effects are visible without being conflated with the language/toolchain itself.

---

# 8. Language Quality Score Metrics

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

## Toolchain / Developer Experience

- Tooling
- Package / Dependency Management Quality
- IDE / Editor Support Quality
- Debugger / Profiler Support
- Build / Test Integration
- Documentation Quality
- Installation / Distribution Experience
- Toolchain Stability / Release Maturity

Semantic Compression-specific concepts such as Semantic Regularity, Rule Exception Density, context-sensitive semantic branching, and hidden behavior are intentionally excluded from the Language Quality score to avoid double counting. Their direct evaluation belongs to Primary Evaluation 1.

Language Quality intentionally excludes external adoption and network-effect metrics. Do not raise or lower Language Quality merely because a language has more users, more third-party packages, more community content, or more production deployments. Do score the present-day quality and availability of the language's own normally expected development toolchain, even when a young language legitimately lacks those capabilities.

Use these metrics to calculate the:

**Language Quality Score**

Do not fabricate human-study results for metrics such as Readability.

If an objective proxy is used, define it explicitly.

---

## 8.1 Fixed Language Quality Weighting

The Language Quality Score uses fixed category weights. Do not choose or tune these weights during a benchmark run.

| Language Quality category | Weight |
|---|---:|
| Performance | 20% |
| Resource / Distribution | 15% |
| Language / Development | 20% |
| Safety / Robustness | 25% |
| Toolchain / Developer Experience | 20% |

Within each category, every listed normalized metric has equal weight unless this specification explicitly defines a more specific sub-metric aggregation.

Calculate each category score as the arithmetic mean of its applicable normalized metrics, then calculate:

**Language Quality Score = 0.20*Performance + 0.15*Resource + 0.20*LanguageDevelopment + 0.25*SafetyRobustness + 0.20*ToolchainDeveloperExperience**

Apply the N/A policy in Section 26 within the affected category first. If an entire category is genuinely N/A, renormalize the remaining category weights proportionally and document the reason. A missing capability intentionally tested by a category is not N/A.

Do not change category weights, metric membership, or within-category equal weighting after measurements begin.

A genuinely inapplicable metric may follow Section 26. An applicable metric that was not executed is **not** `N/A` and its weight may not be silently redistributed.

If any applicable Language Quality metric required by the fixed score is `Not Executed`, or if an entire applicable category lacks the evidence required by this specification, mark Language Quality `PARTIAL` and do **not** calculate or publish Language Quality Score or Language Quality Ranking. Partial category and metric measurements may still be reported as diagnostics.

---

## 8.2 Ecosystem Evaluation

This section implements **Primary Evaluation 4 — Ecosystem**.

Ecosystem measures external assets and real-world use that accumulate around a language over time. It is deliberately separate from Language Quality so that language/toolchain quality is not conflated with popularity, age, installed base, or network effects.

Evaluate the following five metrics:

- Third-party Library Availability / Domain Coverage
- Package Ecosystem Activity / Maintenance
- Third-party Tool / Integration Availability
- Production Adoption / Deployment Evidence
- Community / Public Knowledge Availability

Do **not** score first-party installation convenience, official package/dependency-management quality, official LSP/editor support, debugger/profiler quality, build/test integration, documentation quality, or release/toolchain stability here. Those belong to Language Quality — Toolchain / Developer Experience.

Conversely, do **not** move third-party package counts, external integrations, community size/activity, public Q&A/tutorial availability, or real production adoption back into Language Quality.

For each Ecosystem metric, define and freeze an objective rubric or proxy before scoring any language. The same evidence sources, snapshot date or observation window, query rules, thresholds, and 0–100 conversion must be applied unchanged to all 10 languages.

Raw popularity indicators such as GitHub stars, search-result counts, download counts, or package counts must not be used as a single standalone proxy for the entire evaluation. They may be used as declared evidence within an individual metric when collected consistently for all languages and accompanied by the limitations of that proxy.

Each of the five metrics has equal weight:

**Ecosystem Score = 0.20*ThirdPartyLibraryAvailability + 0.20*PackageEcosystemActivity + 0.20*ThirdPartyToolIntegrationAvailability + 0.20*ProductionAdoptionEvidence + 0.20*CommunityPublicKnowledgeAvailability**

Unsupported or absent ecosystem evidence intentionally covered by a metric receives the rubric-defined low score rather than `N/A`. Apply Section 26 only to genuinely inapplicable cases.

If any applicable Ecosystem metric is `Not Executed`, mark Primary Evaluation 4 `PARTIAL` and do **not** calculate or publish Ecosystem Score or Ranking.

---

# 9. LLM Proficiency Evaluation

This section implements **Primary Evaluation 5 — LLM Proficiency**.

During LLM evaluation, humans must not manually improve generated code and then count the result as an LLM success.

If generated code fails, provide the actual diagnostics, errors, or test results back to the LLM and allow the LLM itself to perform repairs.

Preserve the entire repair history.

### Task Completion Tokens

For every scored LLM trial, record **Task Completion Tokens (TCT)** as the total model-visible token cost from the start of the task until the first verified success or until the fixed trial budget is exhausted. Count, using the selected model/provider's recorded tokenizer accounting wherever available:

- original task, specification, examples, and other model-visible input context;
- the initial generated answer/code;
- compiler, parser, test, and verifier diagnostics that are fed back to the model;
- every repair prompt and repeated model-visible context;
- every repair output; and
- model-visible tool results required to complete the task.

Do not count hidden provider/system implementation tokens that cannot be measured consistently across all languages. Document the exact accounting source and tokenizer. Apply the identical accounting rule, prompt budget, repair budget, and success stopping rule to all 10 languages.

**Total Token Efficiency** is the normalized inverse of raw Task Completion Tokens: lower TCT is better. Preserve raw TCT for every trial, including budget-exhausted failures. **Source Token Efficiency** remains a separate metric based on the resulting source and must never be substituted for Task Completion Tokens. This distinction prevents a short final program from receiving credit when reaching it required a long prompt, many diagnostics, or repeated repairs.

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

**LLM Proficiency Score**

At minimum, the final LLM Proficiency table must separately contain:

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
- LLM Proficiency Score

A specification-assisted condition may be used as the normal practical prompt when that is how the benchmark is defined. If a no-specification condition is also run, report it as a practical diagnostic of prior knowledge; do not transform it into a familiarity-corrected primary score.

All normalized LLM scores must follow:

**100 = best, 0 = worst.**

The required trial allocation in Section 6.2 is part of validity, not merely a reporting preference. In particular, every cell that contributes to Proficiency Correct@1 or Prompt Robustness must have all five required independent trials before the LLM Proficiency primary score may be published.

If fewer required trials are available, preserve the observations as a clearly labelled **pilot / partial result**, mark the primary evaluation `PARTIAL`, and do not publish LLM Proficiency Score or Ranking.

---

## 9.1 Fixed LLM Proficiency Weighting

The primary LLM Proficiency Score uses the following fixed metric weights:

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

The twelve rows of the §28 presentation table map onto the eighteen weighted metrics above as follows. This mapping is fixed so that two runs do not invent different correspondences.

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

Six weighted metrics have no presentation row and appear only in the full breakdown: Correct@N, Test Pass Rate, Specification Compliance, Total Token Efficiency, Generated Code Memory Efficiency, and Generated Code Compile Performance. They still carry their weights.

Apply the N/A policy in Section 26 to genuinely inapplicable metrics. Do not relabel a failed or unsupported capability as N/A merely to remove its weight.

Do not alter these weights after any scored LLM output has been observed.

---

# 10. LLM Learnability Evaluation

This section implements **Primary Evaluation 2 — LLM Learnability**.

## 10.1 Objective

The purpose of this evaluation is to reduce the advantage of having seen a language many times before and to test whether the model can learn and correctly apply the language's rules from a supplied specification.

It does not claim to mathematically remove pretraining. Structural similarities, general programming knowledge, tokenizer behavior, and learned abstractions cannot be erased completely.

The required claim is narrower:

**LLM Learnability is a familiarity-controlled, specification-grounded evaluation, not a proof of zero prior exposure.**

The model must not be told the real language name during transformed trials. Use a neutral identifier such as Language A, with the assignment randomized independently of presentation order.

This applies to the conditions that transform the surface: I1, I2, I3 and I6.
It cannot apply to I4 and I5, which deliberately keep the real surface because they measure acquisition of a **new rule** rather than removal of familiarity; anonymizing the surface underneath the overlay would mix I1's effect into I4's and leave neither measurable. In I4 and I5 the model is still never *told* the language name, but it can recognize the language, and the results for those two subtests must be read with that stated rather than implied.

Every transformed program must still be validated by the real implementation after a published reversible mapping back to the actual language.

Run this evaluation for all 10 fixed languages.

## 10.2 Common controls

All transformed trials inherit the fixed LLM execution configuration in Section 6.2. If this section is stricter, this section takes precedence.

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

Use an **Learnability Reference Pack** for each language. It must describe exactly the subset of syntax and semantics needed by the tasks, including every transformed token or rule. Reference packs must use the same section template and a comparable token budget. Publish their token counts.

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

An infrastructure defect in this track does not announce itself. It arrives looking exactly like a language failure: the cell compiles nothing, or fails a check, and the language is scored zero for it. The following must therefore be verified **before any Reference Pack is built and before any trial is scored**, and the verification must be preserved as evidence:

1. **Every fixture compiles and runs on the real toolchain, and its output matches the expected output exactly.** A fixture is the source of the worked example shown to the model; a fixture that does not build teaches every trial in that language to reproduce something that cannot build.
2. **Every harness convention the task does not state is satisfied by the harness, not demanded of the model.** If the prompt withholds a fact — for instance because stating it would reveal the language — the model cannot be marked down for not knowing it. File naming and entry-point naming are the usual cases.
3. **Every validator can both pass and fail.** For each check, exercise a correct input that must pass and a corrupted input that must fail. A validator that cannot pass any input, or cannot reject any input, is measuring nothing. This applies with particular force to conditions whose mapping is a permutation of the language's own vocabulary, where a "did any transformed token survive?" test is vacuous by construction.

An all-zero or near-all-zero row for one language, one condition, or one validator is to be treated as a suspected infrastructure defect and investigated before it is reported as a result. If investigation confirms it is a genuine language outcome, record the evidence that confirmed it.

Defects found during a run are fixed, the affected cells re-run or re-verified, and both the defect and the fix recorded. They are not silently corrected.


### Mandatory Reference Pack leakage audit

After the §10.4 infrastructure pre-flight passes but **before any scored model request**, audit every Learnability Reference Pack for answer leakage.

For every language, condition, seed, and transformation set used in scoring, verify mechanically where possible and manually by a blinded reviewer where necessary that the model-visible pack does not reveal:

- the exact scored solution;
- the exact expected output when predicting that output is part of the task;
- an isomorphic worked example that differs only by identifiers or literals;
- withheld transformed-to-real token mappings;
- validator acceptance conditions that directly encode the answer;
- comments, filenames, fixture names, diagnostics, or metadata that disclose the target; or
- any other information that makes the scored task solvable by copying rather than learning the supplied rules.

Worked examples must be structurally distinct from the scored fixtures. A scored fixture itself, or a mechanically trivial mutation of it, must never be used as a worked example.

Preserve an audit manifest for every scored pack. The manifest must record the pack hash, fixture hash, leakage checks performed, and pass/fail status.

**No Learnability scored trial may begin until every pack that can enter that trial has passed the leakage audit.**

If leakage is discovered after a scored request was sent, all affected scores are invalid. Mark those cells `WITHDRAWN`, fix the infrastructure without inspecting replacement outputs, rebuild and re-audit the packs, and run fresh independent trials. Do not retain the contaminated score in any aggregate or ranking.

## 10.5 Learnability subtests

The primary Learnability score consists of six independently reported subtests.

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

- be described only in the Learnability Reference Pack;
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

For each I1-I6 condition, evaluate the applicable LLM metrics using the same definitions used in Proficiency Effectiveness where possible:

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

| Learnability condition metric | Weight |
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

**LLM Learnability Score = 0.20*I1 + 0.20*I2 + 0.15*I3 + 0.20*I4 + 0.15*I5 + 0.10*I6**

These weights are fixed before the benchmark results are observed.

Do not tune the weights after seeing which language benefits.

Report each I1-I6 score separately in addition to the aggregate.

## 10.7 Familiarity-drop diagnostics

For I1 and I2, also report:

**Proficiency baseline score - transformed score**

for each language.

This drop is diagnostic only. It is not itself the Learnability score and must not be used as a correction factor.

A small drop can mean the model learned the transformed specification well; it does not prove the model had no structural prior.

## 10.8 Interpretation

The two LLM primary results intentionally answer different questions:

- **LLM Proficiency Score** includes real-world model familiarity and measures present-day knowledge-dependent usefulness.
- **LLM Learnability Score** uses controlled unfamiliarization and novel-rule tests to measure specification-grounded learnability and rule consistency.

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

### Mandatory repeated-measurement protocol

For every primary timing or resource-measurement cell, unless a workload section explicitly freezes a larger count **before any language is measured for that workload**:

1. execute **3 unscored warm-up runs**;
2. execute **10 scored measurement runs**;
3. validate expected output on every warm-up and measured run;
4. preserve all 10 raw scored measurements in execution order;
5. use the **median** of the 10 scored runs as the representative raw value; and
6. preserve dispersion at minimum as min, max, median, and MAD or IQR.

### Deterministic cross-language measurement interleaving

For every workload where multiple languages or execution modes are compared by primary timing or resource measurements, do not measure all scored repetitions of one language and then move to the next.

Before the first timing/resource sample for that workload:

1. define the complete comparison-cell set for the workload;
2. derive and record one deterministic `measurement_order_seed` from the immutable run identity and workload ID, or freeze an explicit seed in the manifest before measurement;
3. materialize and preserve the complete warm-up and scored execution schedule before observing any timing result; and
4. use that schedule unchanged unless the recovery policy requires a documented restart.

Execute warm-ups in rounds: each round gives at most one warm-up run to every cell that still requires warm-up, with the within-round cell order deterministically shuffled from the frozen seed and round index. Then execute scored measurements in **10 scored rounds**, each containing exactly one scored run from every applicable comparison cell, again using a deterministic per-round shuffle derived from the same frozen seed. Thus each cell still receives exactly the required number of samples, but machine drift, thermal state, scheduler load, and other time-correlated effects are distributed across languages instead of being coupled to presentation order.

The interleaving schedule is part of the raw evidence. Preserve the seed, generated order, actual start order, and any deviation/recovery record. Never choose or modify the order after observing timing results. Do not execute all scored samples for one language consecutively unless the comparison group contains only one applicable cell or a genuine platform constraint makes interleaving impossible; in that case document the constraint before scoring and treat any resulting comparability limitation explicitly.

For JIT / VM runtimes, the first 3 warm-ups are mandatory but need not be assumed sufficient. If runtime-specific normal practice requires additional warm-up to reach steady state, predeclare one deterministic warm-up rule for that runtime before measuring any workload, cap it at 10 total warm-up runs, preserve the warm-up measurements separately, and apply that rule consistently to all applicable workloads for that runtime. Additional runtime-specific warm-ups participate in the interleaved warm-up rounds until that cell's frozen warm-up count is satisfied. Startup/cold-start measurements remain separate and must not be replaced by steady-state values.

A measured run affected by a verified harness/runner/transport failure is **invalid evidence**, not an outlier to silently discard. Preserve the failed attempt, apply the run-wide recovery policy in Section 4.4, and restart the entire affected measurement cell from its warm-ups so the final cell still contains exactly 10 valid scored runs under one uninterrupted frozen configuration.

Do not trim, winsorize, cherry-pick, or discard a slow but valid scored run. Statistical outlier status alone is never a reason to remove valid evidence.

Before the first timing cell, perform one unscored timing-harness self-test and one peak-memory self-test with known finite programs. Confirm that the harness returns the expected number of samples, preserves units, distinguishes nonzero runtime from startup/measurement overhead where applicable, and reports failed commands as failures rather than as numeric zeros.

Preserve:

- all warm-up measurements;
- all 10 individual scored measurements;
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

# 20. Semantic Regularity Evidence for Semantic Compression

Semantic regularity is raw evidence for **Primary Evaluation 1 — Semantic Compression**. It is not a separate Language Quality category and must not be double-counted in Language Quality.

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

Do not score these values from intuition alone.

Use the raw counts in Semantic Compression probes where they are relevant to **Hidden Semantic Cost** and **Capability Efficiency**. Do not count the same underlying rule twice inside one metric merely because it appears in multiple descriptive categories.

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

Metrics such as Readability, Diagnostics, Portability, Interoperability, Concurrency, Tooling, Package / Dependency Management Quality, IDE / Editor Support Quality, Debugger / Profiler Support, Build / Test Integration, Documentation Quality, Installation / Distribution Experience, Toolchain Stability / Release Maturity, Third-party Library Availability / Domain Coverage, Package Ecosystem Activity / Maintenance, Third-party Tool / Integration Availability, Production Adoption / Deployment Evidence, and Community / Public Knowledge Availability that cannot be reduced honestly to one direct physical quantity must use a published objective rubric or proxy with explicit observable criteria.

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

# 27. Primary Evaluation Final Comparison Tables — 1–2

## 27.1 Semantic Compression Final Comparison Table

Produce this table first because Semantic Compression is Primary Evaluation 1.

| Metric | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Semantic Density | | | | | | | | | | |
| Semantic Determinacy | | | | | | | | | | |
| Semantic Locality | | | | | | | | | | |
| Hidden Semantic Cost | | | | | | | | | | |
| Capability Efficiency | | | | | | | | | | |
| Raw Semantic Compression Quality `Q` | | | | | | | | | | |
| Capability Coverage `C` | | | | | | | | | | |
| **Semantic Compression Overall Score** | | | | | | | | | | |

Every normalized score in this table must follow **higher = better**.

Preserve the raw semantic branching counts, lookup counts, hidden-behavior counts, token/fact counts, complexity units, and capability matrix separately.

Create an independent:

**Semantic Compression Ranking**

based only on Semantic Compression Overall Score, **and only when the Semantic Compression status is `COMPLETE` and the semantic-site comparability gate passed**. Otherwise print the status and withhold both Overall Score and Ranking.

## 27.2 Primary Evaluation 2 — LLM Learnability

Produce the **LLM Learnability** table:

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
| **LLM Learnability Score** | | | | | | | | | | |

For I1-I6, preserve separate seed-level tables including seed identifiers, transformation manifests, means, standard deviations, minima, and maxima.

Every normalized score must follow **higher = better.**

Create an independent **LLM Learnability Ranking** based on LLM Learnability Score only when the Learnability evaluation status is `COMPLETE`. Any contaminated, incomplete, or withdrawn cell prevents publication of the primary score and ranking.

---

# 28. Primary Evaluation Final Comparison Tables — 3–5

## 28.1 Primary Evaluation 3 — Language Quality Final Comparison Table

Produce a Language Quality table with the following fixed columns:

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
| Implementation Robustness | | | | | | | | | | |
| Tooling | | | | | | | | | | |
| Package / Dependency Management Quality | | | | | | | | | | |
| IDE / Editor Support Quality | | | | | | | | | | |
| Debugger / Profiler Support | | | | | | | | | | |
| Build / Test Integration | | | | | | | | | | |
| Documentation Quality | | | | | | | | | | |
| Installation / Distribution Experience | | | | | | | | | | |
| Toolchain Stability / Release Maturity | | | | | | | | | | |
| **Language Quality Score** | | | | | | | | | | |

Every value in this table that is a normalized score must follow:

**higher = better.**

Create an independent:

**Language Quality Ranking**

based on Language Quality Score only when Language Quality status is `COMPLETE`. `Not Executed` applicable metrics prevent publication of the Language Quality Score and Ranking.

## 28.2 Primary Evaluation 4 — Ecosystem

Produce an Ecosystem table with the following fixed columns:

| Metric | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Third-party Library Availability / Domain Coverage | | | | | | | | | | |
| Package Ecosystem Activity / Maintenance | | | | | | | | | | |
| Third-party Tool / Integration Availability | | | | | | | | | | |
| Production Adoption / Deployment Evidence | | | | | | | | | | |
| Community / Public Knowledge Availability | | | | | | | | | | |
| **Ecosystem Score** | | | | | | | | | | |

Create an independent **Ecosystem Ranking** based on Ecosystem Score only when the Ecosystem evaluation status is `COMPLETE`.

## 28.3 Primary Evaluation 5 — LLM Proficiency

Produce the **LLM Proficiency** table using the fixed language columns:

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
| **LLM Proficiency Score** | | | | | | | | | | |

Create an independent **LLM Proficiency Ranking** based on LLM Proficiency Score only when the Proficiency evaluation status is `COMPLETE`, including all required trial replication.

Do not merge Proficiency Effectiveness and Learnability Learnability. Do not merge either LLM result with Semantic Compression, Language Quality, or Ecosystem.

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
- `version.txt`, recording the evaluated Quidra version
- methodology
- scoring definitions
- benchmark conditions

## Results

- Summary
- Semantic Compression Score Table
- Semantic Compression Ranking
- Semantic Compression Raw Probe Results
- Capability Coverage Matrix
- LLM Learnability Score Table
- LLM Learnability Ranking
- Language Quality Score Table
- Language Quality Ranking
- Ecosystem Score Table
- Ecosystem Ranking
- LLM Proficiency Score Table
- LLM Proficiency Ranking
- Raw Results
- Native vs Interpreter Results
- Micro Benchmark Results
- SVM Results
- GMM Results
- LightGrad Results
- Adversarial / Safety Results
- Debuggability Results
- Semantic Compression Results
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
- Semantic Compression probe programs / snippets for all 10 languages

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
- semantic-fact inventory
- Semantic Compression capability universe and probe definitions
- Semantic Compression support / partial-support rubric
- Semantic Compression raw counts and capability matrix
- frozen Semantic Compression semantic-site matrix
- semantic-site matrix validator output
- predeclared Semantic Compression comparability-audit sample and adjudication record
- Learnability Reference Pack leakage-audit manifests
- raw logs
- compiler logs
- runtime logs
- environment information
- compiler versions
- runtime versions
- immutable LLM run configuration
- exact LLM model/version identifier and provider/client metadata
- execution manifest and resumable run ledger
- final report-consistency audit output
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
11. The Semantic Compression capability universe, fact taxonomy, probe set, hidden-behavior checklist, support rubric, token-counting rule, and normalization rules were frozen before scoring.
11a. A language-neutral semantic-site matrix was frozen before language-specific annotation, with stable site IDs, inclusion/exclusion rules, multiplicity rules, metric mapping, and unsupported-capability handling.
11b. The semantic-site matrix validator confirmed identical required rows and rules across all 10 languages.
11c. The predeclared cross-language Semantic Compression comparability audit passed before any Semantic Compression normalized score or ranking was calculated.
12. The same Semantic Compression capability universe and probes were evaluated for all 10 languages.
13. Semantic Density was calculated from preserved fact and token counts.
14. Semantic Determinacy was calculated from preserved local semantic branching counts, including the required call-site argument-passing probe.
15. Semantic Locality was calculated from preserved external semantic lookup counts.
16. Hidden Semantic Cost was calculated from the frozen hidden-behavior checklist.
17. Capability Efficiency was calculated from preserved semantic-complexity counts per supported capability.
18. Capability Coverage was calculated from the same frozen capability universe for all 10 languages.
19. Raw Semantic Compression Quality `Q`, Capability Coverage `C`, and Semantic Compression Overall Score were calculated with the fixed formula.
20. Semantic Compression Ranking was created.
21. Unseen-case Generalization was evaluated.
22. Prompt Robustness was evaluated.
23. LLM Learnability subtest scores were calculated.
24. LLM Learnability Score was calculated.
25. LLM Learnability Ranking was created.
26. Language Quality metric scores were calculated without Semantic Compression-specific double counting and without Ecosystem leakage.
27. Language Quality Score was calculated.
28. Language Quality Ranking was created.
28a. Ecosystem metric scores were calculated from frozen, language-neutral rubrics or proxies.
28b. Ecosystem Score was calculated.
28c. Ecosystem Ranking was created.
29. LLM Proficiency metric scores were calculated.
30. LLM Proficiency Score was calculated.
31. LLM Proficiency Ranking was created.
32. No cross-evaluation weighted overall score or ranking was created.
33. Raw Data was preserved.
34. Scoring formulas and weights were preserved.
35. All actual LLM prompts were preserved.
36. Initial generations and complete repair histories were preserved.
37. Source code for all 10 languages was preserved.
38. Reference SHAs were preserved.
39. Environment information was preserved.
40. Previous completed benchmark run directories were preserved unchanged.
41. The active run directory was self-contained, held the current run's infrastructure and generated artifacts, and was assigned a distinct `YYYY-MM-DD-<Quidra-short-SHA>` identity directly under `benchmark/`.
42. The benchmark is reproducible.
43. The exact `benchmark/master_prompt.md` used for the run was copied to the run directory as immutable `prompt.md`, and `benchmark/master_prompt.md` remained intact during benchmark execution.
44. The Quidra implementation was not modified during benchmark execution.
45. No commit or push was performed during benchmark execution.
46. LLM Proficiency was measured for all 10 languages.
46a. Ecosystem was measured for all 10 languages using the same frozen evidence rules and observation window.
47. I1 Keyword Anonymization was measured for all 10 languages with at least 5 seeds.
48. I2 Vocabulary Anonymization was measured for all 10 languages with at least 5 seeds.
49. I3 Structural Surface Perturbation was measured for all 10 languages with at least 3 transformation sets.
50. I4 Novel-rule Generalization was measured for all 10 languages.
51. I5 Held-out Rule Composition was measured for all 10 languages.
52. I6 Prior-conflict Resistance was measured for all 10 languages.
52a. Every Learnability Reference Pack used for scoring passed the mandatory leakage audit before the first scored request, and the audit manifests were preserved.
53. Forward/inverse mappings, manifests, seeds, and round-trip validation were preserved, and the §10.4 pre-flight validation was performed and preserved: every fixture compiled, ran and matched its expected output; every harness-side convention the prompt withholds was satisfied by the harness; and every validator was shown able to both pass a correct input and reject a corrupted one.
54. Seed-level Learnability results, means, standard deviations, minima, and maxima were reported.
55. I1/I2 familiarity-drop diagnostics were reported separately from the Learnability score.
56. Learnability Reference Pack token counts and transformation-budget controls were published.
57. The immutable LLM run configuration was written before the first scored LLM generation.
58. The exact LLM model/version, decoding controls, repair budget, trial count, and token limits or provider-controlled status were preserved.
59. Semantic Compression, Language Quality, Ecosystem, LLM Proficiency, and Learnability formulas and weights matched the fixed rules in this specification.
60. Every normalized score used the fixed normalization family or an explicitly pre-existing metric-specific override, with no post-result formula selection, and every family-C metric whose applicable raw values span a factor of 100 or more published its raw values and ratios alongside the compressed score.
61. Each primary evaluation was assigned exactly one status from `COMPLETE`, `PARTIAL`, `WITHDRAWN`, or `NOT EXECUTED`.
62. No primary Overall Score or Ranking was published unless that primary evaluation was `COMPLETE`.
63. `Not Executed` applicable evidence was never converted to `N/A` or removed by weight renormalization.
64. The execution manifest enumerated all required work before scored execution, and every published result traces only to validated `COMPLETE` work units.
65. Any resumed work verified the evaluated SHA, prompt/configuration hashes, toolchain/model identities, and fixture hashes before reusing completed work units.
66. The final report-consistency audit passed and the machine-readable and human-readable primary statuses, scores, rankings, and evaluated version agree.
67. The hard environment-readiness barrier in Section 4.3 passed for all 10 languages, Quidra native/interpreter modes, reference inputs, validators, measurement utilities, filesystem operations, and the selected LLM interface before the first scored work unit began.
68. The frozen process/timeout/recovery policy in Section 4.4 was preserved, and every retried executable work unit retained its failed-attempt infrastructure evidence without mixing it into language scores.
69. Every required primary timing/resource cell contained exactly 10 valid scored runs under the frozen configuration, plus the required warm-ups, unless a workload explicitly froze a larger count before measurement began.
70. Timing/resource aggregation used the preserved raw samples and median rule; no valid slow sample was silently discarded as an outlier, and every invalid infrastructure sample caused a full-cell restart rather than selective replacement.

If a technically impossible or unavailable item prevents completion, do not invent a result.

Record it explicitly as N/A or Not Executed with the exact reason.

---

# 34. Final Report

Before generating the final report, run a machine-readable **report consistency audit** against the run ledger and primary-evaluation statuses.

The audit must fail report publication if any of the following is true:

- a non-`COMPLETE` primary evaluation has a numeric Overall Score;
- a non-`COMPLETE` primary evaluation has a Ranking;
- a `COMPLETE` evaluation is missing any required work unit or validation evidence;
- a `Not Executed` applicable metric was treated as `N/A`;
- a Semantic Compression score exists without a passing matrix/comparability gate;
- a Learnability score includes a Reference Pack without a passing leakage audit;
- a Proficiency score uses fewer than the required independent trials;
- report tables, JSON, Markdown summaries, and commit-summary values disagree; or
- the report labels a different Quidra version/SHA than `version.txt`.

Generate human-readable tables from the same validated machine-readable result objects used by the consistency audit. Do not independently hand-copy primary scores or rankings into separate report files.

After the consistency audit passes, keep the chat response concise.

Report at least, in this order:

- evaluated Quidra HEAD SHA
- status of each of the five primary evaluations
- Semantic Compression Overall Score, or `WITHHELD` if its status is not `COMPLETE`
- Semantic Compression Ranking, or `WITHHELD` if its status is not `COMPLETE`
- Raw Semantic Compression Quality `Q` and Capability Coverage `C`
- LLM Learnability Score, identified as Primary Evaluation 2, or `WITHHELD` unless `COMPLETE`
- LLM Learnability Ranking, or `WITHHELD` unless `COMPLETE`
- Language Quality Score, identified as Primary Evaluation 3, or `WITHHELD` unless `COMPLETE`
- Language Quality Ranking, or `WITHHELD` unless `COMPLETE`
- Ecosystem Score, identified as Primary Evaluation 4, or `WITHHELD` unless `COMPLETE`
- Ecosystem Ranking, or `WITHHELD` unless `COMPLETE`
- LLM Proficiency Score, identified as Primary Evaluation 5, or `WITHHELD` unless `COMPLETE`
- LLM Proficiency Ranking, or `WITHHELD` unless `COMPLETE`
- major Quidra strengths
- major Quidra weaknesses
- any N/A or Not Executed items
- main files or directories created under benchmark/

Do not report a combined score, combined ranking, weighted cross-evaluation total, or implied overall winner across the five primary evaluations.

Do not flood the final chat response with raw benchmark evidence.

Store detailed evidence under benchmark/.

---

# 35. Final Objective

Evaluate the Quidra implementation exactly as it exists in the current benchmarked checkout, and record the evaluated HEAD SHA.

Compare it against the fixed set of Python, C++, Rust, Go, Java, TypeScript, Kotlin, Swift, and Zig under reproducible and fair conditions.

Measure five independent primary evaluations in this priority order:

1. **Semantic Compression** — how much reliable meaning is communicated per unit of syntax, adjusted for capability coverage.
2. **LLM Learnability** — specification-grounded learnability under controlled unfamiliarization.
3. **Language Quality** — present-day general-purpose language and first-party / normally expected toolchain strength, excluding external adoption and network effects.
4. **Ecosystem** — present-day third-party ecosystem breadth, community/public knowledge availability, and real-world adoption.
5. **LLM Proficiency** — present-day LLM effectiveness with real pretraining familiarity included.

The Learnability evaluation must include keyword anonymization, vocabulary anonymization, structural surface perturbation, novel-rule generalization, held-out rule composition, and prior-conflict resistance.

Attempt to calculate these five independent primary scores:

**Semantic Compression Overall Score**

**LLM Learnability Score**

**Language Quality Score**

**Ecosystem Score**

**LLM Proficiency Score**

using reproducible evidence.

Publish a primary score and its separate ranking **only when that evaluation is `COMPLETE` under Section 4.1**. Otherwise preserve partial evidence, publish the evaluation status, and withhold its primary score and ranking.

**Do not average, weight, merge, or otherwise collapse the five primary evaluations into one final score or one overall ranking.** Assigning cross-evaluation weights would encode subjective design priorities and would undermine the benchmark's fairness.

For every normalized score:

**100 means best. 0 means worst. Higher is always better.**

Historical completed benchmark results must remain preserved as historical evidence. New completed results receive a distinct `YYYY-MM-DD-<Quidra-short-SHA>` run identity directly under `benchmark/`; do not overwrite an older completed run. Exactly one newest completed run carries the literal `(latest)` suffix.

At the `benchmark/` root, keep only `master_prompt.md` and the run directories. Store every other benchmark file and directory inside the applicable run directory.

Permanent compiler/runtime tests, CI smoke tests, performance-regression guards, release tooling, and other product-development infrastructure must live outside `benchmark/` (for example under `tests/` or `scripts/`). A benchmark run may copy such tooling into its immutable run directory for reproducibility, but production CI must not depend on transient benchmark-root files.

Keep `benchmark/master_prompt.md` intact while executing a benchmark, and preserve the exact version used as `prompt.md` inside that run's directory. Revisions are allowed only between runs.

Do not modify Quidra itself during benchmark execution.

Do not commit or push anything while benchmark measurements are in progress.

---
