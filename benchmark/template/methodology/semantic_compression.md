# Semantic Compression Specification

This is the authoritative detailed specification for **Primary Evaluation 1 — Semantic Compression** in new benchmark runs.

## Required runner coverage IDs

The deterministic runner assigns these IDs to frozen work units before measurement. Across non-aggregation work units, every ID below must be covered by the current run plan. These IDs are orchestration metadata; they do not change the scoring definition.

- `gate.capability_universe`
- `gate.semantic_site_matrix`
- `gate.matrix_validator`
- `gate.comparability_audit`
- `coverage.all_frozen_probes`
- `metric.semantic_density`
- `metric.semantic_determinacy`
- `metric.semantic_locality`
- `metric.hidden_semantic_cost`
- `metric.capability_efficiency`
- `metric.capability_coverage`

The deterministic plan is validated mechanically before `manifest-merge`; missing or unknown requirement IDs are fatal pre-measurement errors.


Ordinary workers receive only the compact worker rules, frozen Primary configuration, assigned requirement IDs, and selected sections of this specification needed for those requirements. They do not need the root prompt, root conversation, historical runs, sibling outputs, or other evaluation specifications.

The frozen Primary configuration overrides only replication/execution counts. Evaluation meaning, language-neutral fairness rules, metric definitions, formulas and capability universes below remain binding unless explicitly changed before the run.

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

The template stores this frozen matrix in `methodology-assets/semantic_compression/semantic_site_matrix.json`. Before any language-specific annotation, the runner mechanically validates that definition, the identical ordered `site_id` template for all 10 languages, all multiplicity/metric/denominator/unsupported rules, and the required negative self-tests. Language-specific annotation must then fill that **identical ordered set of `site_id` rows**; it may change only annotation state/evidence, never matrix structure.

Rules:

1. A language may not add a semantic-fact row that the other languages were not offered.
2. A language may not omit a row because the semantic fact is inconvenient, implicit, unsupported, or absent. Record the prescribed zero / unsupported / hidden-behavior state instead.
3. One matrix row contributes at most one unit unless its multiplicity rule was explicitly frozen before annotation.
4. A compound syntax form does not receive more facts merely because an annotator chose to describe it at finer granularity.
5. A concise syntax form does not lose facts merely because several frozen semantic sites are expressed by one token.
6. Hidden behavior is counted only through frozen rows/checklist items, never through free-form commentary.
7. If a semantic phenomenon cannot be represented consistently by the frozen matrix, stop and revise the matrix **before any score is observed**, then restart the affected annotation from the frozen version.

Before scoring, run the mechanical matrix validator. Its pre-measurement pass must confirm the frozen structural contract for every probe, and every language-specific annotation must preserve that contract exactly:

- identical `site_id` set and ordering across all 10 languages;
- no unknown or language-only rows;
- no missing required rows;
- valid values for every row;
- identical multiplicity rules;
- identical metric mapping; and
- identical capability denominator; and
- consistency of every frozen `unsupported_rule`: if a language records `UNSUPPORTED` for one row governed by a shared unsupported rule, every row in that probe governed by the same rule must use the corresponding unsupported state rather than selectively collecting ordinary positive states.

The validator must carry negative self-tests for each of these invariants, including inconsistent site order, language-only and missing rows, multiplicity and metric-map changes, denominator drift, and an inconsistent shared unsupported rule. Those tests execute mechanically in every run; a failed self-test blocks Semantic Compression before any metric worker runs.

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

Regardless of how those families are subdivided, freeze and include these mandatory counter-probes for every language before observing results:

- **capture-bearing lexical closure** — a callable that reads captured local state, distinguishing a true closure environment from capture-free function values;
- **first-class callable value** — store, pass, return, and invoke a callable through a value rather than only calling a declaration by name;
- **shared object/storage identity** — create two handles/references that intentionally denote the same mutable or identity-bearing object and test whether identity is observable;
- **open polymorphism / runtime dispatch** — invoke behavior through an abstraction whose concrete implementation is selected at runtime, distinguishing dynamic dispatch from static inheritance, unions, or monomorphization.

A language that does not support one of these capabilities keeps the probe as `UNSUPPORTED`; the probe must never be removed, weakened, or replaced with an easier language-specific analogue. These counter-probes exist to prevent accidental capability cherry-picking, not to privilege any language.

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

Freeze, before any annotation, an observable counting rule for each complexity category, and require the annotator to itemise what was counted so the figure is auditable. A sub-count derived from documentation must be recorded **`unknown`, never `0`**, when no specification text exists to count: `0` asserts that no such rule exists, which is a different claim from "no document was found". A complexity vector whose categories are not governed by a written counting rule must not be scored, because it measures documentation volume for a sparsely documented language and language complexity for the others.

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
