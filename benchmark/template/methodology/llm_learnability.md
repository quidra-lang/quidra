# LLM Learnability Specification

This is the authoritative detailed specification for **Primary Evaluation 2 — LLM Learnability** in new benchmark runs.

## Required runner coverage IDs

The deterministic runner assigns these IDs to frozen work units before measurement. Across non-aggregation work units, every ID below must be covered by the current run plan. These IDs are orchestration metadata; they do not change the scoring definition.

- `gate.infrastructure_preflight`
- `gate.reference_pack_leakage`
- `coverage.all_10_languages`
- `condition.i1_keyword_anonymization`
- `condition.i2_vocabulary_anonymization`
- `condition.i3_structural_surface_perturbation`
- `condition.i4_novel_rule_generalization`
- `condition.i5_held_out_rule_composition`
- `condition.i6_prior_conflict_resistance`

The deterministic plan is validated mechanically before `manifest-merge`; missing or unknown requirement IDs are fatal pre-measurement errors.


Ordinary workers receive only the compact worker rules, frozen Primary configuration, assigned requirement IDs, and selected sections of this specification needed for those requirements. They do not need the root prompt, root conversation, historical runs, sibling outputs, or other evaluation specifications.

The frozen Primary configuration overrides only replication/execution counts. Evaluation meaning, language-neutral fairness rules, metric definitions, formulas and capability universes below remain binding unless explicitly changed before the run.

## 6.2 Fixed LLM Execution Configuration

All LLM Learnability replication counts are read **only** from `./.quidra-benchmark/template/config/primary.json`. Do not hardcode or infer alternate counts from prose.

Binding controls:

- one exact model/version and provider/client interface for the whole Primary evaluation;
- fresh isolated context for every independent scored trial;
- identical decoding controls or the same recorded provider-controlled/unavailable state for all languages;
- at most `llm_learnability.max_repair_turns` repair turns per trial, identically across languages;
- the same success stopping rule, oracle policy and token-accounting rule across languages;
- no prior scored generation, repair history, sibling-agent output or hidden parent conversation in a fresh trial;
- provider/network/rate-limit failures are infrastructure events, never incorrect language/model generations;
- actual scored prompts are content-addressed with `prompt-save` before dispatch.

I1/I2/I3 replication is already supplied by the configured seeds/transformation sets. Do not multiply those cells by an additional generic trial count. I4/I5/I6 use their own per-language trial counts from `primary.json`.

If deterministic decoding produces duplicate outputs, preserve the configured independent trials/seeds and report duplication; do not inject ad-hoc prompt noise.

The Primary counts apply identically to all 10 languages. Higher extended replication in `primary.json` is diagnostic only and starts after all five Primary evaluations are complete or legitimately blocked.

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

Use exactly the configured I1/I2 seed counts from `primary.json`; the mapping for every seed must be deterministic and recorded.

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

**This rule must be tested before the example set and task set are frozen.** For every worked-example slot × scored-task pair, generate representative reference solutions in at least one neutral implementation and run the same normalized-token overlap detector used by the leakage audit. Any pair at or above the frozen leakage threshold is a design defect and must be changed while the design is still editable.

Every leakage predicate must include a planted-leak positive control that it is required to detect. A leakage audit that cannot detect its own planted control fails pre-flight regardless of the observed packs.

Every structural/compliance predicate used to score a cell must have both a positive reference fixture and a deliberately mutated negative fixture stored beside the predicate. The negative fixture must fail for the intended reason before the predicate may be frozen.

Preserve an audit manifest for every scored pack. The manifest must record the pack hash, fixture hash, leakage checks performed, and pass/fail status.

**No Learnability scored trial may begin until every pack that can enter that trial has passed the leakage audit.**

If leakage is discovered after a scored request was sent, all affected scores are invalid. Mark those cells `WITHDRAWN`, fix the infrastructure without inspecting replacement outputs, rebuild and re-audit the packs, and run fresh independent trials. Do not retain the contaminated score in any aggregate or ranking.

## 10.5 Learnability subtests

The primary Learnability score consists of six independently reported subtests.

The Primary replication counts come from `./.quidra-benchmark/template/config/primary.json`, which is the **single source of truth for the run shape**. Generate the cell plan mechanically from that configuration. Do not maintain an independent hand-written scope document with a second copy of seed, transformation-set, trial or generation counts. Any derived plan must be hash-linked back to the frozen config and mechanically checked for exact agreement before scoring.

### I1. Keyword Anonymization — 20%

Replace language keywords and grammar-significant word tokens with controlled pseudo-words.

Examples include constructs equivalent to conditional branches, loops, returns, declarations, matching, and imports.

Do not rename ordinary user identifiers merely to make the task harder.

Purpose:

**measure rule learning when familiar keyword recall is removed.**

Run exactly `llm_learnability.keyword_anonymization_seeds` seeds and score the seed mean.

### I2. Vocabulary Anonymization — 20%

Apply I1 and additionally anonymize the standard-library and builtin names required by the benchmark tasks.

Examples include operations equivalent to output, length, range construction, sorting, collection helpers, and relevant standard namespaces.

Purpose:

**measure whether the model can learn the language/API vocabulary from specification rather than recall familiar names.**

Run exactly `llm_learnability.vocabulary_anonymization_seeds` seeds and score the seed mean.

### I3. Structural Surface Perturbation — 15%

Apply a reversible unfamiliar surface form to selected grammar structure while preserving the underlying language semantics.

Examples may include controlled replacement of grouping delimiters, declaration separators, block markers, call argument separators, and selected operator spellings.

The transformation budget must be matched across languages. Do not redesign one language more aggressively than another.

Purpose:

**measure whether the model can follow an explicitly described grammar instead of replaying a familiar source-code shape.**

Use exactly `llm_learnability.structural_surface_transformation_sets` independently generated transformation sets for the Primary score. Extended sets are diagnostic only.

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
