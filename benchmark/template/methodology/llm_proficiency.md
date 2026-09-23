# LLM Proficiency Specification

This is the authoritative detailed specification for **Primary Evaluation 5 — LLM Proficiency** in new benchmark runs.

## Required runner coverage IDs

The deterministic runner assigns these IDs to frozen work units before measurement. Across non-aggregation work units, every ID below must be covered by the current run plan. These IDs are orchestration metadata; they do not change the scoring definition.

- `gate.fixed_model_configuration`
- `gate.prompt_preservation`
- `coverage.all_10_languages`
- `metric.generation_success_rate`
- `metric.compile_parse_success_rate`
- `metric.correct_at_1`
- `metric.correct_at_n`
- `metric.test_pass_rate`
- `metric.repair_success_rate`
- `metric.repair_efficiency`
- `metric.diagnosis_efficiency`
- `metric.silent_bug_resistance`
- `metric.syntax_hallucination_resistance`
- `metric.specification_compliance`
- `metric.prompt_robustness`
- `metric.unseen_case_generalization`
- `metric.source_token_efficiency`
- `metric.total_token_efficiency`
- `metric.generated_code_performance`
- `metric.generated_code_memory_efficiency`
- `metric.generated_code_compile_performance`

The deterministic plan is validated mechanically before `manifest-merge`; missing or unknown requirement IDs are fatal pre-measurement errors.


Ordinary workers receive only the compact worker rules, frozen Primary configuration, assigned requirement IDs, and selected sections of this specification needed for those requirements. They do not need the root prompt, root conversation, historical runs, sibling outputs, or other evaluation specifications.

The frozen Primary configuration overrides only replication/execution counts. Evaluation meaning, language-neutral fairness rules, metric definitions, formulas and capability universes below remain binding unless explicitly changed before the run.

## 6.2 Fixed LLM Execution Configuration

All LLM Proficiency replication counts are read **only** from `/quidra-benchmark/template/config/primary.json`. Do not hardcode an alternate trial count in prompts, scripts or reports.

Binding controls:

- one exact model/version and provider/client interface for the whole Primary evaluation;
- exactly `llm_proficiency.independent_trials_per_replicated_cell` fresh independent trials for every replicated Proficiency cell;
- at most `llm_proficiency.max_repair_turns` repair turns per trial;
- identical decoding controls or the same recorded provider-controlled/unavailable state for all languages;
- identical prompt structure/budget, oracle policy, success stopping rule and token-accounting rule across languages;
- no prior scored generation, repair history, sibling-agent output or hidden parent conversation in a fresh trial;
- provider/network/rate-limit failures are infrastructure events, never incorrect language/model generations;
- actual scored prompts are content-addressed with `prompt-save` before dispatch.

If deterministic decoding produces duplicate outputs, preserve all configured independent trials and report duplication; do not add ad-hoc prompt noise.

For every Primary Proficiency initial completion and repair completion, the trusted runtime writes the returned source into an isolated trial directory, performs the frozen target-language compile/parse step, and executes the frozen run recipe. The worker does not choose these commands. **Generation Success Rate** and **Compile / Parse Success Rate** are bound to this runtime-owned evidence and a contradictory worker-reported value invalidates the unit. Repair prompts are also runtime-owned and contain only the trusted verifier's compiler/run diagnostics plus the frozen instruction to return replacement source; the orchestration worker may not tutor the scored model.

The verifier's fixed-workload run result is preserved as audit evidence but does not, by itself, mechanically define Correct@1 or Correct@N. The current Primary workloads use fixed toy cases and do not yet expose a hidden-input external correctness oracle, so treating an observed `PASS` line alone as semantic correctness would allow a hard-coded answer to masquerade as a correct implementation. Correctness, specification compliance, silent-bug resistance, and related semantic metrics therefore remain separately adjudicated from the preserved source and evidence. A future hidden-input workload revision may make those correctness metrics runner-owned as well, but such a revision changes the frozen benchmark contract and cache generation.

The Primary replicated-cell universe is exactly the Cartesian product of `llm_proficiency.primary_workloads` and `llm_proficiency.primary_scenarios` from `primary.json`. Do not add or drop a workload/scenario cell during a run.

The concrete Primary tasks are frozen offline in `methodology-assets/llm_proficiency/workloads.json`. That asset records the exact upstream commit provenance and the predeclared common subset for SVM, GMM and LightGrad, plus the complete specification, validation contract and one frozen C++ reference implementation per workload. A scored run never fetches or reinterprets the live upstream repositories.

The trusted sandbox runtime, not the orchestration worker, constructs every initial Proficiency trial prompt from that frozen asset. Replications of one workload/scenario cell receive byte-identical prompts in fresh sessions. The `specification_to_implementation` prompt contains no reference source; the `reference_to_porting` prompt contains the same frozen C++ reference for every target language. A worker-supplied replacement initial prompt is rejected before inference. Prompt hashes are rechecked by the integrity gate and certified into cache records.

Extended replication is diagnostic only and begins after all five Primary evaluations are complete or legitimately blocked.

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

The configured trial allocation in Section 6.2 is part of validity, not merely a reporting preference. Every replicated cell that contributes to Proficiency Correct@1 or Prompt Robustness must contain exactly the Primary independent-trial count from `primary.json` before the LLM Proficiency score may be published.

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

# 16. LLM Implementation Scenarios

For the Primary score, evaluate exactly the workloads listed in `llm_proficiency.primary_workloads` under exactly the scenarios listed in `llm_proficiency.primary_scenarios`. The workload text and reference source are not authored during a run: they come only from the frozen `methodology-assets/llm_proficiency/workloads.json` contract and are inserted by the trusted runtime. Additional substantial tasks may be run only as extended diagnostics after Primary completion and do not change the Primary cell universe.

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
