# FROZEN METHODOLOGY 04 — Hidden Semantic Cost, Capability Efficiency, and Semantic Regularity Evidence

Benchmark run: `2026-09-17-7677581`
Covers: spec §6.1.4.D (Hidden Semantic Cost), §6.1.4.E (Capability Efficiency), §20 (Semantic Regularity Evidence).
Status: **FROZEN.** Written before any language was measured. No part of this document may be altered after
measurement of any language has begun (spec §6.1.7, §25.4, §32).

This document is written to be executed mechanically by an operator who has never seen the conversation that
produced it. Where a judgement call is unavoidable, the decision procedure is written out, the evidence that
must be recorded is specified, and the output is auditable.

---

## 0. Scope, inputs, identifiers, and shared definitions

### 0.1 Fixed comparison set and column order

Exactly these ten languages, in this fixed column order, in every table produced under this methodology:

| # | Column label | Language ID | Toolchain (frozen) |
|---|---|---|---|
| 1 | Quidra | `quidra` | Quidra 0.2.0 |
| 2 | Python | `python` | Python 3.14.5 |
| 3 | C++ | `cpp` | Apple clang 17, `-std=c++20` |
| 4 | Rust | `rust` | rustc 1.95.0 |
| 5 | Go | `go` | go 1.26.3 |
| 6 | Java | `java` | OpenJDK 26.0.1 (arm64) |
| 7 | TypeScript | `typescript` | tsc 7.0.2 + node 24.2.0 |
| 8 | Kotlin | `kotlin` | kotlinc 2.3.21 |
| 9 | Swift | `swift` | swiftc 6.2.3 |
| 10 | Zig | `zig` | zig 0.16.0 |

All build/run commands are the frozen recipes in
`environment/environment.json → semantic_compression_recipes` (identical to `methodology/01_capability_universe_and_probes.json → toolchain_binding.recipes`; see `00_cross_language_constraints.md` item C-8 for why Primary Evaluation 1 uses the CHECKED recipe set and the Standard evaluation uses the release set). No other flags, packages, or
code generation may be used to produce evidence under this methodology.

**Multi-mode execution rule (language-neutral, binding).** Primary evidence is taken from the native compiled
form where the frozen recipe provides one. Where the frozen recipes provide **more than one execution engine or
build mode** for a language, any materially different outcome (§0.8) between those modes is a **configuration
dependence** and triggers the same checklist item it would trigger for any other language — H8a for numeric
edges, H3 for dispatch, H4 for allocation/copy/cleanup, H5 for failure, H6 for optionality, H7 for
representation, H9 for state and effects. The operator must:

1. run every supported probe under **every** frozen mode of that language;
2. record each run as its own probe-evidence record (§0.6), naming the mode;
3. record any divergence between modes as a **triggered item**, with both outputs attached as evidence;
4. publish the per-probe divergence set for that language.

As of this freeze, the only language whose frozen recipe table lists more than one execution engine is Quidra
(`quidra build FILE.qui -o BIN` + `./BIN`, and `quidra run FILE.qui`); both Quidra engines are therefore
executed for every supported probe and any divergence between them is counted, exactly as a divergence between
two modes of any other language would be. If the frozen environment lists a second mode for any other language,
the identical rule applies to it with no amendment to this document. **No language's second execution mode is
recorded as "a note only".**

**Mode-dependence is assessed from the pinned reference, not from the pinned recipe.** Where a language's
pinned reference (§0.4) documents that a behavior differs across build modes, flags, optimization levels, or
targets that the language itself provides, H8a triggers even though the frozen recipe pins one of those modes:
the reader at the local site cannot tell which mode the code will be built under. This is applied identically to
all ten languages and is neither softened for a language whose frozen recipe happens to pin the checked mode nor
sharpened for one whose recipe pins the unchecked mode. The operator publishes, per language, which safety or
checking modes the frozen recipe selects, so that this assessment is auditable.

**A recipe's checking level is neither a credit nor a debit (binding).** Where a frozen recipe selects a mode
that disables a language's own overflow, bounds, or assertion checking, while another language's only shipped
mode has that checking always on, the difference is a property of the **frozen environment**, and this document
uses it for exactly one thing: the §0.8 materiality assessment of what the pinned reference says can happen.
It does not excuse the language built unchecked from H8a, and it does not earn the always-checked language an
exclusion — an exclusion asserting that checking is uniform across the modes a language provides is universal
in scope and must be discharged under §1.4.0. A language with exactly one execution mode is neither credited
nor penalised for having one. This document does not set the recipes; correcting a recipe that pins an unchecked
mode for one language and a checked mode for another is the environment owner's obligation, recorded in §4.3.

### 0.2 Fairness preconditions (spec §32, §6.1.7)

These bind every rule below and override any convenience:

1. Quidra is the object of evaluation, not the beneficiary. Nothing in this methodology may be derived from
   Quidra's syntax, operators, types, or feature set.
2. No capability is removed from the probe universe because Quidra lacks it. Unsupported capabilities reduce
   Capability Coverage `C`; they never silently disappear.
3. Every rule here is language-neutral, predeclared, and applied identically to all ten languages.
4. A language must be free to score 0 hidden-cost items on a checklist item that Quidra triggers, and to
   trigger items that Quidra does not. Both outcomes are legitimate results, not defects in the checklist.
5. Nothing is fabricated. Every count traces to (a) a written rule in a pinned reference document and
   (b) reproducible probe evidence. Where the written rule is absent, the `observed` policy of §0.4 applies;
   where the capability itself is absent, the `N/A` policy of §0.9 applies.

### 0.3 Frozen inputs consumed by this methodology

This document does **not** define the probe set, the fact taxonomy, or the support rubric; it consumes them. The
frozen inputs are:

| Input | Canonical path (relative to the run directory) | Used for |
|---|---|---|
| Fixed capability universe (20 families `F01`–`F20`) and fixed probe set (**44 probes**, ids `F01.P1` … `F20.P2` as published in that file), each probe carrying `capability_points` and `semantic_facts_expected` | `methodology/01_capability_universe_and_probes.json` → `probes` | Parts 1–3 unit of work; Part 2 denominator; §20 scoping |
| Measured-fragment boundary rule (what text of a probe is measured) | same file → `authoring_rules_for_probe_fragments.R5_measured_fragment_boundary` | §0.7 local sites |
| Frozen support rubric — levels `FULL` / `PARTIAL` / `NONE` with `support_factor` `1.0` / `0.5` / `0.0`, and the award formula `awarded_points = capability_points × support_factor` | same file → `support_rubric` | Applicability |
| **Common-basis rule** — metrics A–D primary on the set of probes for which all ten languages have a fragment; all-fragments basis published as secondary | same file → `support_rubric.interaction_with_quality_metrics` | §1.6, §2.5 |
| Capability-coverage formula and its frozen denominator (**88 capability points**; every probe is worth exactly 2, `capability_point_assignment_rule`) | same file → `capability_coverage`, `total_fixed_capability_points` | Part 2 denominator |
| Applied support result (the capability matrix): one record per (language, probe) giving `support_level`, `support_factor`, `capability_points`, `awarded_points` | `semantic-compression/raw/capability_matrix.json` | Part 1 probe scope, Part 2 denominator |
| Fact taxonomy (the 13 fact kinds of spec §6.1.3) | `methodology/02_fact_taxonomy_and_density.md`; ids in `01_capability_universe_and_probes.json → semantic_fact_kinds` | Fact vocabulary |
| Frozen per-language probe implementations | `semantic-compression/probes/<lang>/<probe_id>.<ext>` (e.g. `semantic-compression/probes/rust/F07.P1.rs`) | Evidence for Parts 1–3 |

Probe identifiers are exactly those published in `01_capability_universe_and_probes.json`: `F01.P1`, `F01.P2`,
`F02.P1`, … `F20.P2` — **at least** two probes per capability family (families `F04`, `F05`, `F14` and `F15`
carry three, per that file's `probes_per_family_by_family`), families `F01`–`F20` in the order given in spec
§6.1.2. `N = 44` is the fixed corpus size and `<probe_id>` denotes one of those 44 ids. If the frozen probe file
assigns a different probe→family mapping, a different id form, or a different probe count, **the frozen probe
file wins**; this methodology never re-partitions, renames, or re-counts probes, and every formula written here
for `N = 44` uses whatever `N` that file states (see §1.7 for the one predeclared constant derived from `N`).

**Corpus-size and denominator binding (binding, and the reason this document states numbers at all).** The two
constants this document repeats — `N = 44` probes and the **88**-point Capability Coverage denominator (2 points
per probe) — are **read from** `01_capability_universe_and_probes.json`, never asserted against it. They are
written out here only so that checks V10 and V13 are numerically checkable by an operator who has not opened
that file. If the two ever disagree, the frozen probe file is correct, this document's literals are the defect,
and the correction is made here under spec §10.4 before measurement begins — never by adjusting the probe file
to match this one, and never by a language's results.

**Capability matrix record format (frozen, so that §2.5 STEP 7 and check V10 are mechanically verifiable).**
The matrix is produced by applying the frozen `support_rubric` to the frozen fragments, before Parts 1–3 begin:

```json
{
  "language": "go",
  "probes": {
    "F01.P1": {"support_level": "FULL", "support_factor": 1.0, "capability_points": 2,
               "awarded_points": 2.0, "partial_criteria": [], "justification": "<one sentence>"}
  },
  "cap_points_total": 0.0,
  "denominator": 88
}
```

`cap_points_total` is `Σ_p awarded_points(L, p)` over all 44 probes and is the identical numerator of
`C = 100 × cap_points_total / 88`.

If a required input file is absent at measurement time, measurement does not begin. This methodology may not be
executed against an improvised probe set, an improvised id scheme, or an improvised support rubric.

### 0.4 Frozen authoritative reference documents (normative sources for citations)

Every counted rule must cite one of these, at the version matching the frozen toolchain. The operator records
the retrieval date and stores a local snapshot under `semantic-compression/raw/refs/<lang>/`.

| Language | Normative reference | Citation anchor form |
|---|---|---|
| Quidra | Quidra 0.2.0 language documentation as shipped in the evaluated repository (`docs/` tree), plus `quidra --help` output | repo-relative file path + heading + line range |
| Python | The Python Language Reference 3.14 and The Python Standard Library 3.14 | section number, e.g. `ref/6.2` |
| C++ | ISO/IEC 14882:2020 (C++20); working-draft N4868 is the citable proxy | stable clause name, e.g. `[expr.arith]/4` |
| Rust | The Rust Reference (rustc 1.95.0 channel), plus the accepted RFC where the Reference defers to it | Reference chapter path, e.g. `expressions/operator-expr.html#overflow` |
| Go | The Go Programming Language Specification (Go 1.26) | spec heading, e.g. `Arithmetic operators` |
| Java | The Java Language Specification, Java SE 26 Edition (JLS), plus the JVMS where semantics are defined there | JLS section, e.g. `JLS §15.18.1` |
| TypeScript | TypeScript Handbook (7.0) for type-level and erasure semantics; ECMA-262 (ECMAScript, edition implemented by node 24.2.0) for runtime semantics | Handbook page + heading, or ECMA-262 clause number |
| Kotlin | Kotlin language specification (2.x, matching kotlinc 2.3.21), plus the Kotlin documentation where the specification defers to it | specification section number |
| Swift | The Swift Programming Language (Swift 6.2), plus the accepted Swift Evolution proposal where TSPL defers to it | TSPL chapter + heading, or `SE-NNNN` |
| Zig | Zig Language Reference, version 0.16.0 | anchor heading, e.g. `Integer Overflow` |

**Asymmetry declaration.** TypeScript has no single normative language specification; Quidra and Zig have
references that are less formally complete than ISO C++ or the JLS. This is a real, pre-existing property of
the languages and must not be smoothed over or used to advantage or disadvantage anyone. The handling is fixed
and identical for all ten languages:

- A rule stated in the pinned reference is tagged `source: "documented"`.
- A rule that is **not** stated in any pinned reference but is reproducibly demonstrated by a probe artifact
  built with the frozen recipe is tagged `source: "observed"`. It **still counts** — a reader must still learn
  it — and the per-language count of `observed`-only rules is published beside every result that depends on it.
- A claimed rule that is neither documented nor reproducibly demonstrated is **not counted at all**, and the
  attempted claim is recorded in `disputed[]` with the reason.

### 0.5 Citation record format (frozen)

Every citation is one object in `semantic-compression/raw/citations.json`:

```json
{
  "citation_id": "CIT-<LANG>-0001",
  "language": "rust",
  "document": "The Rust Reference",
  "document_version": "rustc 1.95.0 channel",
  "anchor": "expressions/operator-expr.html#overflow",
  "retrieved": "2026-09-17",
  "snapshot": "semantic-compression/raw/refs/rust/operator-expr.html",
  "quoted_rule": "<verbatim sentence or table row establishing the rule, <=40 words>",
  "source": "documented"
}
```

`quoted_rule` is a short verbatim extract used solely as provenance for a technical fact; it is capped at 40
words and must be the shortest extract that establishes the rule. Where a rule is `observed`, `document` is
`"observed behavior"`, `anchor` is the artifact path, and `quoted_rule` is replaced by `reproduction`, a
command line plus its exact output.

### 0.6 Probe evidence record format (frozen)

Every claim of reproducible probe evidence is one object in
`semantic-compression/raw/probe_evidence.json`:

```json
{
  "evidence_id": "EV-<LANG>-<PROBE_ID>-<NNN>",
  "language": "rust",
  "probe": "F07.P1",
  "artifact": "semantic-compression/probes/rust/F07.P1.rs",
  "line_span": [12, 12],
  "local_site_index": 1,
  "mode": "<the frozen execution mode this run used, per §0.1>",
  "kind": "static" | "dynamic",
  "command": "<the frozen recipe for this language and mode, verbatim from environment.json, applied to this artifact>",
  "observed_output": "<verbatim stdout/stderr, truncated to 2000 chars with an explicit truncation marker>",
  "note": "<one sentence stating what the output demonstrates>"
}
```

Example evidence id: `EV-RUST-F07.P1-003`. `mode` is required on every `dynamic` record; where a language has
more than one frozen mode (§0.1), one record per mode is required.

`kind: "static"` is used where the behavior is not dynamically observable (for example, "the implementation is
permitted to elide this copy"). A `static` record must carry a `citation_id` instead of a `command`, and the
written rule alone carries the claim.

### 0.7 The local site (shared definition for Parts 1–3)

Several rules below turn on what counts as "local". This definition is fixed once and used identically by
Hidden Semantic Cost, Capability Efficiency, and §20, and is deliberately consistent with spec §6.1.4.B
("before consulting external declarations or whole-program facts").

The frozen corpus does not mark a single statement per probe: each probe declares a `measured_fragment`
(`01_capability_universe_and_probes.json → probes[].measured_fragment`, authored under rule R5), which for many
probes spans several statements — `F16.P2` is four statements including a `for` loop and a `match`, `F17.P1` is
an entire function declaration and body, `F09.P2` is two statements. The enumeration below is therefore fixed
mechanically so that two analysts obtain the same set:

> **Local sites of probe `p` (enumeration).** The local sites of `p` are exactly the **top-level statements**
> enumerated in source order inside that probe's `measured_fragment`, and, for a fragment that is a single
> expression rather than a statement list, the single expression. They are indexed `1, 2, 3, …` in source
> order; that index is `local_site_index` in every record.
>
> **Nesting.** A construct nested inside a top-level statement — a loop body, a match/switch arm, a branch
> body, a lambda or closure body, a nested block, an argument expression — belongs to the local site of its
> **enclosing top-level statement**, and every token of it is local to that site. Where the fragment is an
> entire function declaration and body (e.g. `F17.P1`), the top-level statements are the statements of that
> body, and the function header is local to the first of them.
>
> **Local to a site** = every token lexically inside that site, including any type annotation, binding
> keyword, cast, operator, attribute, modifier, effect marker, or comment-free trivia written inside it.
>
> **Not local to a site:** tokens in a **different local site of the same fragment** (a `match` two statements
> later does not make an earlier lookup-miss locally evident); declarations of names used by the site but
> written elsewhere, including elsewhere in the same file and including the probe's "given" context;
> imported definitions; type definitions; trait/interface/protocol implementations; overload sets; generic
> instantiations chosen elsewhere; build flags; compiler modes; target configuration; and any whole-program
> fact.

**Aggregation over local sites.** Each local site of `p` is adjudicated separately against §1.3. `H(L, p)` is
computed over the union of all local sites of `p` (§1.5). Facts fixed by the language's own pinned reference
for the tokens inside the site are known at zero cost and may close a candidate (this is the LED gate, §1.2);
that is the only body of external knowledge admitted, and it is admitted identically for all ten languages.

**Relationship to methodology 03's *focus span* (binding).** Methodology 03 §1.1 defines a *focus span* — one
contiguous token range per probe — for Semantic Determinacy (`B_i`) and Semantic Locality (`H_i`). The two
boundaries are related but not identical: a focus span is normally one statement or expression, while this
document enumerates every top-level statement of the `measured_fragment`. **Each document governs its own
metrics only**: methodology 03 §1.1 governs `B_i` and `H_i`; this §0.7 governs Hidden Semantic Cost and the
locality judgements of Parts 2–3. Neither boundary may be borrowed into the other metric to raise or lower a
count. Where a probe's focus span coincides with one of this document's local sites, the two must denote the
same tokens; a mismatch is a corpus defect, recorded in `CORRECTIONS.md` under spec §10.4 and fixed there, and
is never resolved by an analyst choosing the more convenient boundary.

A **local signal** for a behavior B is a token or construct inside the local site whose meaning, under the
pinned reference, entails B or explicitly flags that B may occur. A signal may be a keyword (`unsafe`, `try`,
`inout`, `mut`, `await`, `throws`), an operator (`?`, `!`, `&`, `*`, `&+`), an annotation, or a named
construct whose specified meaning is B (for example an identifier whose specification defines it as
allocating).

### 0.8 Materiality (shared definition)

A behavior is **semantically material** if it can change any of: observable result, mutation, aliasing,
failure, conversion, dispatch, resource behavior, control flow, or representation. This mirrors spec §6.1.4.B
and is used unchanged throughout this document. Behaviors that cannot change any of these are not counted
anywhere in this methodology.

**Universal materiality exclusions** (applied identically to all ten languages, declared before measurement):

- Host resource exhaustion (out-of-memory, stack exhaustion, thread-creation failure, file-descriptor
  exhaustion) is excluded everywhere. Every language can fail this way; counting it would add a constant.
- Non-deterministic scheduling of a garbage collector or allocator that has no effect observable to a
  conforming program, and no user-visible finalizer/deinitializer reachable in the probe.
- Diagnostics, warnings, logging, and timing that do not change program semantics.
- Lexical trivia: whitespace, comments, formatting, and naming style.

### 0.9 `N/A` policy inside this methodology (spec §26)

`N/A` is permitted only in the two cases enumerated below, and every `N/A` records a reason string.

| Case | Metric affected | Handling |
|---|---|---|
| The language's support level is `NONE` for **every** one of the 44 probes | Hidden Semantic Cost, Capability Efficiency | Both raw values are undefined. Mark `N/A`, reason `"no supported probes"`. Exclude from the applicable-weight denominator of `Q` and renormalize the remaining quality weights per spec §26. Capability Coverage `C` is 0, so the Semantic Compression Overall Score is 0 regardless. |
| A probe's support level is `NONE` for a language | per-probe measurement | The probe leaves the **common basis for every one of the ten languages** (§1.6), and is excluded from that language's all-fragments Hidden Semantic Cost denominator and from its all-fragments Capability Efficiency scope. It is **not** scored 0 and **not** scored well. Its full `capability_points` remain in the fixed 88-point Capability Coverage denominator with `support_factor = 0.0`, and therefore lower `C` (doc 01 `na_policy` NA-2). |

Lacking a capability **never** escapes scoring by being called `N/A` (spec §26). The penalty lands on `C`,
exactly where the spec places it.

---

# PART 1 — HIDDEN SEMANTIC COST CHECKLIST (spec §6.1.4.D)

Weight: **20% of the Semantic Compression quality score `Q`.**
Raw unit: **mean number of distinct checklist items triggered per supported probe.**
Direction: **lower is better.** Normalization family: **C** (spec §25.1).

## 1.1 What this metric measures

Hidden Semantic Cost counts semantically material behaviors that **may occur without being signaled at the use
site**. It measures *concealment*, not *complexity*, *danger*, or *performance*. A language that performs many
operations and announces all of them locally has low hidden cost. A language that performs few operations but
announces none of them has high hidden cost.

## 1.2 THE LOCAL EXPLICITNESS DEFENSE (LED) — the controlling rule

Spec §6.1.4.D states, without qualification:

> **"Do not count a behavior as hidden if the fixed local syntax makes that behavior unambiguous under the
> language specification."**

This rule is the controlling constraint of Part 1 and overrides every "counts when" test below. It is applied
as a mandatory, explicit gate on every candidate trigger:

> **LED TEST.** Given only the local site (§0.7) and the pinned language reference (§0.4), does the language
> specification determine that behavior B occurs (or does not occur) with exactly one outcome for this probe?
> **If yes, B is not hidden. Record the LED with its citation and do not count it.**

Two clarifications, both binding:

- **LED is satisfied by unambiguity, not by verbosity.** A single character is a sufficient signal if the
  reference makes it unambiguous. Requiring three lines of ceremony earns nothing extra.
- **LED is not satisfied by "an expert would guess correctly."** The signal must be in the local site and the
  meaning must be in the written reference. Convention, idiom, and community practice are not signals.

**Documentation symmetry (binding on every item H1–H9, and on every exclusion in §1.4).** §0.4 rules that a
rule which is not stated in any pinned reference but is reproducibly demonstrated still **counts**, tagged
`source: "observed"`. The same standing is granted to the *other* side of the ledger: **an LED, or any
exclusion listed in §1.4, may be established by a `documented` citation OR by an `observed` record under §0.4,
on identical terms.** Where an exclusion rests on an `observed` record, that record is published in the
per-probe `led[]` entry with its reproduction command and output.

Without this rule, the thoroughness of a language's reference prose — not the concealment in its code — would
decide the score: a language with an exhaustive normative document (ISO C++, the JLS) would escape charges that
an identical construct incurs in a language with a thinner reference (Zig, TypeScript's non-normative Handbook,
Quidra's shipped `docs/` tree). Hidden Semantic Cost measures concealment at the use site, not documentation
quality, and §0.4's asymmetry declaration must therefore cut both ways. Wherever §1.4 says "documented as", "the
reference defines", or "the reference states", read "documented **or** reproducibly demonstrated under §0.4".

**One limit, fixed in §1.4.0.** An `observed` record demonstrates what happened in the runs it reports. Where an
exclusion is **universal in scope** — asserting that no mode, target, candidate, type, or input produces a
different outcome — §1.4.0 states what must be shown to discharge it. That limit applies identically to all ten
languages and does not narrow the `observed` route for any exclusion that is not universal in scope.

### 1.2.1 Mandatory LED illustrations (application of the rule, not pre-assigned scores)

These worked examples fix the intended strictness. They are illustrations of *how the gate is applied*. They
are **not** results: at measurement time each one must be re-derived against the actual frozen probe text and
must carry its own citation and evidence record.

| Situation | LED outcome | Why |
|---|---|---|
| A language in which every heap allocation must be written with an explicit allocator argument at the call site, so that no expression allocates without a locally written allocator token | **LED applies. H4 scores 0 for implicit allocation.** | The local syntax names the allocation. Scoring 0 here is the correct, legitimate result — it is the metric working, not a bug or a calibration error. |
| A language in which error propagation out of a call must be written with an explicit propagation operator or keyword at the call site | **LED applies. H5 scores 0 for implicit propagation.** | Abnormal exit is spelled locally. A language that makes every failure edge visible is supposed to win this item. |
| A language whose integers are arbitrary-precision, so that integer addition has exactly one documented outcome and no overflow edge exists | **LED applies. H8 scores 0.** | There is one outcome under the reference; there is nothing concealed. |
| A language whose wrapping and trapping arithmetic are *different operators*, so the operator written locally fixes the edge behavior | **LED applies. H8 scores 0 for that probe.** | The local token disambiguates the edge behavior completely. |
| A call `f(x)` in a language where `f` may be declared to take its parameter by writable reference, so the callee may mutate the caller's storage, with nothing at the call site distinguishing this from a by-value call | **LED does NOT apply. H2 triggers.** | The local site admits two materially different outcomes; the reference does not fix one. Resolving it requires the callee's declaration, which §0.7 excludes from local. |
| A value expression whose arithmetic outcome at overflow differs between the language's debug and release build configurations | **LED does NOT apply. H8 triggers (sub-test H8a).** | The outcome depends on a build mode, which §0.7 excludes from local. |
| A scope exit that runs a user-visible destructor/deinitializer with no locally written token naming it | **LED does NOT apply. H4 triggers.** | The cleanup is material and is not spelled at the site. |
| A scope exit that runs cleanup named by a locally written `defer`/`using`/`with`-style token | **LED applies. H4 does not trigger for that cleanup.** | The cleanup is spelled locally. |

**A zero on any checklist item is a legitimate, reportable result.** If a language records zero events across
every probe of the basis, its raw Hidden Semantic Cost is 0 and it is the best language on this metric. The normalization
in §1.7 is predeclared to handle an exact zero.

## 1.3 General trigger test (applies to every item H1–H9)

### 1.3.0 Closed candidate enumeration (binding)

Part 1 is only reproducible if two analysts start from the same list of candidates. The candidate set is
therefore **closed and mechanically derived**, not improvised per analyst:

> **Candidate set.** For language `L` and supported probe `p`, the candidate set is **exactly** the set of
> rules `r` in `registry(L)` (the Part 2 rule registry) such that `p ∈ r.probes` **and**
> (`r.tags.is_implicit` **or** `r.tags.is_context_sensitive`).
>
> - **No behavior outside this set may be counted.** A behavior an analyst believes is hidden but which
>   corresponds to no registry rule is not counted; instead the analyst opens the missing rule in the registry
>   under Part 2's rules (with citation and evidence), and it then enters the candidate set for every language
>   and probe it applies to, or it is recorded in `disputed[]`.
> - **Every member of the set must be adjudicated** against the conditions below, at every local site of `p`
>   (§0.7), and recorded as either a triggered event or an `led[]` rejection with a reason. Silence is an
>   audit failure (§1.8).
> - The registry is therefore built (Part 2, STEP 2–6) **before** Part 1 is scored; §4.1 fixes that order.

Each adjudication is anchored to the rule: every event object and every `led[]` object carries the
`rule_id` it was derived from (§1.8 schema; check V17).

**The conditions.** A candidate behavior B, arising from candidate rule `r`, at local site `s` of probe
`<probe_id>` for language `L`, triggers its checklist item **iff all four conditions hold**:

1. **Occurrence.** Under the pinned reference, B occurs, or may occur, when the probe's frozen implementation
   is executed over the probe's declared input domain. ("May occur" includes implementation latitude that the
   reference grants, e.g. "the implementation is permitted to …".)
2. **Materiality.** B is semantically material per §0.8, and is not on the universal exclusion list.
3. **No local signal.** There is no local signal for B in the local site `s` (§0.7).
4. **LED fails.** The LED test of §1.2 does not apply — i.e. the local site plus the reference do not fix a
   single outcome.

If any one of the four fails, the behavior is not counted at that site, the failing condition is named, and the
reason is recorded in `led[]`.

**Deleted gate, recorded for audit.** An earlier draft of this document carried a fifth condition ("in
annotated scope": B must affect a fact listed in the probe's `semantic_facts_expected`). It is **removed**.
Spec §6.1.4.D counts material behaviors that are unsignalled at the use site, without any such filter, and the
per-probe fact lists were authored for the density metric: they are narrow and uneven across probes, so the
gate silenced whole checklist items on exactly the probes built to expose them (for example `F03.P1`, the
mutation probe, does not list `aliasing_writable_aliasing`, and `F09.P2`, the comparison probe, lists neither
`conversion_behavior` nor `overflow_exceptional_numeric`). The reproducibility work that gate was doing badly
is now done properly by the closed candidate set above. The probe's `semantic_facts_expected` list remains a
**floor** for what a reader must be able to predict (§2.4), never a ceiling on what may be counted.

### 1.3.1 Single-assignment rule (prevents double counting inside this metric)

Spec §20 forbids counting the same underlying rule twice inside one metric merely because it fits several
descriptive categories. Therefore:

> **Every distinct hidden event is classified into exactly ONE checklist item**, using the fixed precedence
> order **H1 → H2 → H3 → H4 → H5 → H6 → H7 → H8 → H9**, first match wins. **H9 is the residual category** and
> is used only for events that no earlier item claims.

Example of the rule working: a hidden implicit conversion that also changes the in-memory representation is a
single event; it is assigned to H1 and is **not** additionally counted under H7.

### 1.3.2 Distinct events

Two occurrences of a behavior are the **same event** iff they arise from the **same registry rule** (same
`rule_id`) at the **same local site** (§0.7). Two different local sites inside one probe are different events.
Two different rules at one site are different events. Repeated dynamic executions of one site (a loop body
running three times) are **one** event, because they arise from one rule at one site.

The distinct event is the **scored unit** (§1.5). Distinctness is therefore load-bearing and must be recorded:
every event carries `rule_id` and `local_site_index`, and check V3 verifies that no `event_id` is assigned to
two checklist items.

### 1.3.3 Two-analyst adjudication (binding)

The central judgement of Part 1 — "does the local site plus the reference fix exactly one outcome?" — is a
judgement, so it is made twice, exactly as methodology 03 §3.2 requires for `B_i` and `H_i`:

1. Every (language, probe) pair is adjudicated **independently by two analysts** over the full candidate set of
   §1.3.0. Both pre-reconciliation records are preserved and published.
2. Analysts work **blind to all running aggregates**: no per-language `HSC_raw`, no partial matrix, and no
   other language's counts are visible to either analyst until all ten languages are complete (methodology 03
   §3.1 item 4). The identity of the language cannot be hidden — the frozen source file *is* the evidence — so
   blinding is to the **totals**, which is the quantity an analyst could otherwise steer.
3. Disagreements are resolved in this fixed order, and the resolution is written into the per-probe record:
   (a) an event without a `citation_id` or `evidence_id` is dropped; (b) where both analysts hold evidence for
   events the other omitted, the union is taken, subject to the single-assignment rule §1.3.1; (c) if the
   disagreement is about a **rule** rather than a fact, it goes to the shared adjudication register
   `semantic-compression/adjudication_register.json`, is resolved once, is written in terms that **name no
   language**, and is applied **retroactively to all ten languages**, including re-counting languages already
   counted.
4. The **raw agreement rate** — the fraction of (language, probe, item) cells on which the two analysts agreed
   before reconciliation — is published per language in `hidden_cost_audit.json`. It is evidence about the
   method, and a low rate is reported, not suppressed.

## 1.4 The nine checklist items (frozen)

Each item is defined by: **Definition**, **Counts when** (the operational test), **Exclusion** (what must not be
counted), and an **LED note** naming at least one concrete way a language legitimately scores 0.

### 1.4.0 Universal-scope exclusions: what an `observed` record can and cannot establish (binding on H1–H9)

§1.2's documentation-symmetry clause lets an exclusion be established by a `documented` citation **or** by an
`observed` record, so that Hidden Semantic Cost measures concealment rather than the thoroughness of a
language's reference prose. That clause is correct and stays. It has one limit, which is fixed here so it is not
resolved differently per language:

> **An `observed` record establishes that a behavior *did* occur, or *did not* occur, in the runs it reports.
> It does not, on its own, establish a universal negative.**

Several §1.4 exclusions are **universal in scope** — they assert that *no* configuration, *no* candidate, or
*no* input produces a different outcome. They are, exhaustively: H1's "value-preserving and cannot affect
dispatch, overflow, equality, or representation"; H3's per-candidate equivalence; H4's "copy elision or other
optimizations that the reference makes unobservable"; H5's "forms the reference proves total for the declared
input domain"; H7's "representation choices the reference makes fully unobservable"; H8's "one documented
outcome across every type the form admits in this probe, **independent of mode and target**"; and H9's "effects
unobservable to a conforming program".

For these, and only these, the exclusion is established **only** by one of:

1. a `documented` citation in which the pinned reference **states the universal itself** (it says the outcome is
   the same across the modes, targets, candidates, types, or inputs at issue — not merely that one outcome
   occurs in one of them); **or**
2. an `observed` record that **exercises the whole space the exclusion quantifies over** and enumerates what was
   exercised: every frozen execution mode and build configuration the environment provides for that language
   (§0.1), every candidate body, and every edge class the fragment can reach in the probe's declared input
   domain. The record lists each cell it ran and its output.

**A single run, under a single mode, never discharges a universal-scope exclusion for any language.** Where
neither route is available, the exclusion does not apply and the item triggers on the ordinary §1.3 test.

This is symmetric by construction and is applied identically to all ten. It does **not** reinstate a
documentation-quality penalty: a thin-reference language keeps the `observed` route in full, and a
thorough-reference language gains nothing from prose that states one outcome in one mode without stating the
universal. What it forecloses, for every language equally, is discharging "this cannot differ anywhere" by
observing "it did not differ here". Note that the frozen recipes pin exactly one build configuration for most
languages (§0.1), so route 2 is bounded by what the frozen environment actually provides, and a language whose
environment provides a single mode cannot use that single mode's uniformity as evidence that its other modes,
were they provided, would agree — nor is it penalised for having one mode.

---

### H1 — Implicit conversions

**Definition.** A value is converted from one type, precision, signedness, or value-category to another as part
of the local form, without any conversion operator, cast, constructor call, or conversion annotation written in
the local site.

**Counts when.** All of §1.3 hold **and**: the pinned reference permits or requires the conversion to be
inserted without local syntax, **and** the conversion can change value, precision, sign interpretation, the set
of representable values, overload/dispatch selection, or comparison/equality outcome for the probe's declared
input domain.

**Exclusion — do not count:**
- a conversion written at the local site (cast, constructor, conversion function, coercion operator);
- a conversion between types the reference defines as the *same* type (type aliases, transparent newtypes that
  the reference defines as identical);
- fixing the type of an untyped/polymorphic literal where the reference determines the single resulting type
  from the local site alone;
- conversions that are value-preserving **and** cannot affect dispatch, overflow, equality, or representation
  observably for this probe's inputs.

**LED note.** A language that requires every cross-type conversion to be written explicitly, and whose
reference states that no implicit conversion exists, scores 0 on H1 for every probe. That is the intended
result for such a language.

---

### H2 — Hidden writable aliasing or mutation

**Definition.** The local form causes, or may cause, a write to storage reachable through a name or handle
other than one being assigned at the local site, or creates a handle through which such a write later becomes
possible, without any local token marking reference, borrow, address, pointer, `inout`, `mut`, or mutation.

**Counts when.** All of §1.3 hold **and** at least one of:
- there exists an instantiation consistent with the local site in which executing the form writes storage
  reachable from outside the local site (for example a by-reference parameter, a captured variable, a shared
  interior-mutable cell);
- the form yields a value that aliases existing storage with a writable path, so a later write through it is
  visible through the original name (for example a view, slice, or reference returned without a local marker).

**Exclusion — do not count:**
- mutation of a name that appears as the assignment target at the local site (`x = e`, `x += e`, `x[i] = e`,
  `x.f = e`) — that mutation is locally evident;
- read-only aliasing with no writable path under the reference;
- copy/value semantics that the reference guarantees for the form (no aliasing is created);
- aliasing that is marked by a local token (`&mut`, `inout`, `ref`, `*`, `out`, an explicit borrow).

**LED note.** A language whose call-site syntax distinguishes by-value from by-writable-reference at the call
site scores 0 on H2 for the argument-passing probes. A language that does not make that distinction locally
triggers H2 there. Both outcomes are legitimate.

---

### H3 — Overload or dynamic dispatch not locally evident

**Definition.** The concrete implementation that runs for the local form is not uniquely determined by the
local site under the reference: two or more distinct bodies could execute depending on static types, dynamic
types, interface/trait/protocol conformance, extension or module resolution, generic instantiation, or
operator overloading.

**Counts when.** All of §1.3 hold **and** the set of candidate bodies compatible with the local site, under the
reference, has cardinality ≥ 2, **and** the equivalence exclusion below has not been established on the record.

**Exclusion — do not count:**
- forms for which the reference fixes exactly one meaning language-wide (built-in operators on built-in types
  in a language that forbids operator overloading, when the operand type set is fixed by the local site);
- calls whose target is fully determined by a locally written fully-qualified path, a locally written type
  ascription, or a local token the reference defines as forbidding override/overload;
- **candidate equivalence, only on the record.** Multiple candidate bodies are excluded **only if** the
  operator records, **per candidate**, that it cannot differ from the others in any respect listed in §0.8 for
  the probe's declared input domain, citing for each candidate the reference clause or an `observed` record
  (§0.4) that establishes it. This exclusion is universal in scope, so **§1.4.0 governs what discharges it**.
  Absent that per-candidate record, the exclusion does not apply and H3 triggers.
  (The earlier wording, "candidates the reference proves observationally equivalent", was unusable: no
  language reference proves observational equivalence of an overload or override set, so read strictly the
  exclusion was dead and read loosely it let any overload set be waved away.)
- *statically resolved* generic instantiation where the local site names the instantiation explicitly.

**LED note.** A language without overloading, without subtype polymorphism reachable at this form, and without
user-definable operators scores 0 on H3. A dynamically typed language will typically trigger H3 on operator and
method probes. Both are correct outputs of the same test.

---

### H4 — Implicit allocation, copy, move, destruction, or cleanup

**Definition.** The local form acquires memory or another resource, duplicates a value, transfers ownership,
releases a resource, or runs user-visible cleanup, without a local token that the reference ties to that effect.

**Counts when.** All of §1.3 hold **and** the reference requires or permits the implementation to, as part of
this form: allocate; copy a value whose copy is observable (identity, cost class, or independent mutation);
move/transfer ownership such that the source becomes invalid or changed; destroy/finalize/deinitialize; or run
a cleanup hook.

**Exclusion — do not count:**
- allocation named by a local token whose **specified or demonstrated** meaning (§0.4, §1.2 documentation
  symmetry) is the creation of a new object or value of that type: a composite or container literal, a
  constructor or factory call, a `new`-style operator, an explicitly passed allocator, an explicit allocation
  call, or an allocating macro. The test is the **construct**, not whether the reference happens to contain
  the word "allocates": a language whose reference is thin does not thereby forfeit this exclusion, and a
  language whose reference is exhaustive does not thereby earn it;
- cleanup named by a locally written scope construct (`defer`, `using`, `with`, explicit `close`/`free`);
- moves/copies marked by a local token (an explicit move operation, an explicit clone/copy call);
- garbage-collector reclamation with no observable effect and no user-visible finalizer reachable in the probe
  (universal exclusion, §0.8) — note that the *allocation* itself is still assessed;
- copy elision or other optimizations that the reference makes unobservable.

**LED note.** A language in which every allocation is spelled out locally scores 0 for implicit allocation, and
**that is a legitimate result rather than a bug in the checklist.** Conversely, a language whose ordinary
assignment may deep-copy, or whose scope exit may run a user-visible destructor with nothing written locally,
triggers H4. Both are correct.

---

### H5 — Implicit exception / failure propagation

**Definition.** Control may leave the local site abnormally, or a failure may be propagated to the caller,
without a local token marking that possibility.

**Counts when.** All of §1.3 hold **and** the reference states (or an `observed` record under §0.4 demonstrates)
that the form can raise, throw, panic, trap, abort, or propagate an error out of the enclosing function,
**and** the failure is reachable within the probe's declared input domain.

The probe's `semantic_facts_expected` list is **not** a gate here: a failure reachable in the declared input
domain counts whether or not that probe's fact list happens to name `possible_failure`. Several probes whose
fragments can trap or throw do not list it, and using the list as a filter would silence H5 on exactly those
probes.

**Exclusion — do not count:**
- failure marked by a local token (`try`, `?`, `!`, an explicit `throw`, an explicit `catch`/`match` on the
  failure at the same site, a locally written `throws`/`rethrows` effect on this expression);
- forms the reference proves total for the declared input domain (they cannot fail) — universal in scope,
  **§1.4.0 governs what discharges it**;
- host resource exhaustion (universal exclusion, §0.8);
- failures outside the probe's declared input domain.

**LED note.** A language whose every fallible call must carry a propagation marker scores 0 on H5. A language
with unchecked exceptions that may unwind from any call triggers H5 wherever a failure is reachable in the
probe's declared input domain. Both are legitimate.

---

### H6 — Hidden nullability or optionality

**Definition.** A value that may be absent — null, nil, undefined, uninitialized, or otherwise not present —
flows through the local site without a local token marking optionality.

**Counts when.** All of §1.3 hold **and** the reference permits the expression's value at the local site to be
absent/null/undefined for some instantiation consistent with the local site, **and** the local site contains no
optionality marker, unwrap operator, default-supplying operator, presence test, or pattern match on presence.

**Exclusion — do not count:**
- types the reference guarantees non-optional at this form;
- a local optionality marker of any kind, including a forced-unwrap token (forced unwrap is *locally evident*
  for H6 purposes; its failure mode is assessed under H5 and its branching under Semantic Determinacy);
- a presence test written in the same statement;
- "absent" values that the reference gives total, defined semantics for at this form with no distinct absent
  case (for example a total default that cannot be distinguished from a present value).

**LED note.** A language whose type system makes every reference non-optional unless marked scores 0 on H6. A
language in which any reference may be null scores > 0. That contrast is exactly what the item is for.

---

### H7 — Implicit representation change

**Definition.** The representation of a value changes as part of the local form, without a local token: boxing
or unboxing, interning or caching, reference/value category change, wrapping into an interface or trait object
or existential, array/slice/pointer decay, string or character encoding change, widening or narrowing of the
storage format, or a layout change relevant to FFI or serialization.

**Counts when.** All of §1.3 hold **and** the change is *observable* by at least one of: reference/identity
equality, aliasing behavior, visibility of mutation, precision or overflow behavior, serialization or FFI
layout, or a documented change of algorithmic complexity class.

**Exclusion — do not count:**
- representation choices the reference makes fully unobservable to a conforming program;
- a representation change written at the local site (an explicit box/unbox, explicit conversion, explicit
  encode/decode call);
- a change already assigned to H1 under the single-assignment rule (§1.3.1) because it is fundamentally an
  implicit conversion — assign once, to H1.

**LED note.** A language with a single uniform value representation and no boxing scores 0 on H7. A language
that silently boxes primitives when they cross a generic or interface boundary triggers it.

---

### H8 — Context-dependent overflow / numeric behavior

**Definition.** The numeric edge behavior of the local form — overflow, wraparound, trap, saturation, undefined
behavior, division or modulo by zero, shift beyond width, signed/unsigned mixing, rounding, precision, NaN and
infinity handling — is not fixed by the local form.

**Counts when.** All of §1.3 hold **and** at least one of the two sub-tests fires. The item counts **once** even
if both fire.

- **H8a — configuration dependence.** For the *same* operand types, the outcome at the edge differs depending
  on build mode (debug/release), compiler or runtime flags, optimization level, target platform, pointer or
  integer width, or the reference labels the outcome undefined, unspecified, or implementation-defined.
- **H8b — non-uniform edge rule.** For the set of numeric types the local form admits *in this probe*, the
  reference does **not** give one and the same documented outcome across the edge classes the form can reach
  in the probe's declared input domain — overflow and underflow, division or modulo by zero, shift beyond
  width, signed/unsigned mixing, rounding and precision, NaN and infinity — and no local token selects the
  edge behavior. The probe's `semantic_facts_expected` list does not gate this sub-test; the edge classes the
  fragment's own operations can reach do.

**Exclusion — do not count:**
- forms where the reference fixes exactly one documented outcome across every type the form admits in this
  probe, independent of mode and target (for example: one uniform wraparound rule for all integer types, or one
  uniform trapping rule for all integer types, or arbitrary-precision integers with no overflow edge at all).
  **This exclusion is universal in scope: §1.4.0 governs what discharges it**, and observing one outcome under
  the one mode the frozen recipe pins does not;
- forms where the edge behavior is selected by a locally written operator or call (a distinct wrapping
  operator, a distinct saturating or checked operation);
- variation that arises **only** from which type the operands were declared to be elsewhere, with a single
  uniform rule per edge class — that cost is measured by Semantic Locality (§6.1.4.C) and Semantic Determinacy
  (§6.1.4.B), and is deliberately not re-charged here.

**LED note.** A language with exactly one documented integer-overflow behavior, the same in every build mode
and on every target, scores 0 on H8. A language whose overflow behavior differs between debug and release, or
is undefined, triggers H8a. These are the honest, intended outcomes.

---

### H9 — Hidden side effects on existing state (residual category)

**Definition.** The local form reads or writes program-visible state that is not named in the local site:
globals, statics, thread-locals, module-level state, environment, I/O, an implicit context or receiver, locks,
random-number-generator state, memoization caches, interning tables, observers, or registered hooks.

**Counts when.** All of §1.3 hold, the effect is not already assigned to H1–H8 under §1.3.1, **and** executing
the form can modify such state, or its result can depend on such state, in a way observable per §0.8.

**Exclusion — do not count:**
- effects on state named in the local site;
- effects already assigned to an earlier item (single-assignment rule);
- effects unobservable to a conforming program (universal exclusion, §0.8);
- effects that the local site marks explicitly (an explicitly passed context/receiver/handle, an explicit
  effect annotation).

**LED note.** A language in which every effect on non-local state must be routed through an explicitly written
parameter, capability, or effect annotation scores 0 on H9.

## 1.5 Per-probe counting unit (frozen)

For language `L` and supported probe `p`:

```
H(L, p) = | { distinct events (§1.3.2) at any local site of p that are assigned to some item H1..H9 } |
```

Each event is assigned to **exactly one** item by the single-assignment rule (§1.3.1), so `H(L, p)` is the
count of distinct hidden **behaviors** the probe conceals. `H(L, p)` is a non-negative integer; it is not
capped at 9.

**Why events and not items.** Spec §6.1.4.D directs the checklist to "count semantically material behaviors
that may occur without being signaled at the use site" — behaviors, not categories. Spec §20's prohibition on
double counting forbids counting **one rule** twice inside one metric because it fits several descriptive
categories; that prohibition is fully discharged by §1.3.1 (one event → exactly one item) and §1.3.2 (one rule
at one site → exactly one event). Collapsing five distinct concealed conversions at five distinct sites to a
single point would be an additional choice with no warrant in the spec, and it would erase precisely the
difference this metric exists to expose: a language concealing five behaviors per probe would be
indistinguishable from one concealing one.

**Reported beside the score, as raw evidence.** The operator additionally publishes, per (language, probe):

- `items_triggered`, the vector of which of H1–H9 fired, and `H_items(L, p) = |items_triggered|`, an integer in
  `[0, 9]` — the item-level view of the same data;
- `occurrences(L, p, i)`, the number of distinct events assigned to item `i` (so
  `H(L, p) = Σ_i occurrences(L, p, i)`).

`H_items` is published in full as a 10 × 44 matrix beside the scored matrix, and the item-mean aggregate
`Σ_p H_items(L, p) / |basis|` is published beside `HSC_raw` **on both bases of §1.6.1**, so a reader can see
both views. Only `H(L, p)` is scored.

## 1.6 Aggregation over the corpus (frozen) — the common basis

Let `Supp(L)` be the probes language `L` supports, using the frozen rubric's own vocabulary:

```
Supp(L) = { p ∈ {the 44 frozen probe ids} : support_level(L, p) ∈ {FULL, PARTIAL} }
```

read verbatim from `semantic-compression/raw/capability_matrix.json` (§0.3). Probes with
`support_level = NONE` are excluded from `Supp(L)`. This document never assigns a support level itself.

### 1.6.1 The two bases (binding; consumes doc 01 `support_rubric.interaction_with_quality_metrics`)

Hidden Semantic Cost is **metric D**, and doc 01's frozen common-basis rule governs it. Two bases are computed;
the primary one is the same material for every language.

```
CommonBasis = { p : support_level(L, p) ∈ {FULL, PARTIAL} for ALL TEN languages L }
            = ∩_L Supp(L)

HSC_raw_common(L) = ( Σ_{p ∈ CommonBasis} H(L, p) ) / |CommonBasis|      [PRIMARY — the reported result]
HSC_raw_all(L)    = ( Σ_{p ∈ Supp(L)}    H(L, p) ) / |Supp(L)|           [SECONDARY — published beside it]
```

`HSC_raw(L)` unqualified means `HSC_raw_common(L)`. It is the value normalized in §1.7 and the value that is
reported as the result.

**Why the primary basis is fixed rather than per-language.** Averaging each language over its own supported
subset is an aggregation shape that changes a score through **averaging rather than measurement**: the probes a
language scores `NONE` are, as a class, the hidden-cost-heavy ones — concurrency, FFI, deterministic
destruction, closures, heap identity — so dropping them raises the metric most for whichever language has the
most `NONE`s. Spec §6.1.4 nowhere authorises a variable basis. Under the common basis a language **cannot
change the material its Hidden Semantic Cost is measured on**, because that material is the same intersection
for all ten. Unsupported capabilities still cost the language, through the single channel spec §26 assigns them:
Capability Coverage `C` on the fixed 88-point denominator (§0.9).

Rules:

1. **`PARTIAL` probes enter the common basis**, identically for all ten languages, with no exemption. A probe is
   in `CommonBasis` if all ten languages have a fragment for it, `FULL` or `PARTIAL`. Excluding `PARTIAL`s while
   also excluding `NONE`s would make a language that can only approximate a probe pay for its approximation
   while a language that cannot express the probe at all pays nothing here — which would make `PARTIAL`
   sometimes better than `NONE` for `Q`. A `PARTIAL` probe is measured over the portion of the probe the
   language can express, carrying `"support": "PARTIAL"` and the rubric's recorded `partial_criteria` letters on
   every affected evidence record.
2. **`NONE` probes are excluded from both numerator and denominator on both bases**, and a `NONE` for any one
   language removes that probe from the common basis **for all ten**. They are not zeros and not penalties here;
   they lower `C` instead (spec §26, §6.1.5; doc 01 `na_policy` NA-2). Measuring hidden cost for a construct a
   language cannot express would be fabrication.
3. `|CommonBasis|` is an **output of the support rubric, not an input to it**. It may not be adjusted, and no
   probe may be added to or removed from the common basis to change any language's figure. Which probes fall out
   of the common basis, and because of which language, is published in full.
4. **Basis reporting requirement (binding).** Every published table, matrix, or figure of Hidden Semantic Cost
   must state **in the table**: the basis used (`"common"` or `"all-fragments"`), the size of that basis, and,
   for the all-fragments basis, each language's `n_L = |Supp(L)|`. **A figure published without its basis and
   basis size is not a result.** The per-language delta `HSC_raw_all(L) − HSC_raw_common(L)` is published for
   all ten languages.
5. Where the two bases disagree about a **ranking**, both rankings are published, and the common-basis ranking
   is the one reported as the result.
6. Raw values are retained at full precision. Reported to 2 decimal places (spec §25.3).
7. The per-probe matrices `H(L, p)` and `H_items(L, p)` for all 10 languages × 44 probes are published in full,
   before normalization (spec §25: "Never assign a 0–100 score before preserving the underlying evidence"), with
   each cell's support level, so that a reader can recompute either basis from the published evidence.

## 1.7 Normalization (frozen, family C, spec §25.1)

Hidden Semantic Cost is a positive lower-is-better quantity → **family C**. An exact raw zero is legitimate
here (a language may conceal nothing on any probe), so the **predeclared shifted form is used**:

```
Score_i = 100 * (best_raw + epsilon) / (raw_i + epsilon)
```

- `best_raw` = the smallest valid raw value among languages with a defined raw value, **on the basis being
  normalized**.
- **`epsilon = 1/N = 1/44 = 0.0227272727…`, fixed before measurement**, where `N = 44` is the **fixed corpus
  size** — the finest resolution this metric can attain for any language, since `H(L, p)` is an integer and a
  language measured over all 44 probes moves its mean in steps of `1/44`. It is a **single fixed constant
  applied to all ten languages and to both bases**. It is deliberately **not** recomputed from any language's
  `|Supp(L)|` (a language supporting 25 probes resolves only to `1/25 = 0.04`, and letting the constant follow
  the denominator would give each language its own shift), and it is deliberately **not** recomputed from
  `|CommonBasis|`, which is an output of the measurement and must not be allowed to move a predeclared
  constant. It may not be changed after any language has been measured (spec §25.4). This is the same
  `epsilon = 1/N` convention as methodology 03 §1.8.3 and §2.9 use for `B_i` and `H_i`; if the frozen corpus
  file states a different `N`, `epsilon = 1/N` is recomputed from that `N` once, before any language is scored,
  and in no other circumstance.
- **Both bases are normalized and published**: the common-basis score is the result; the all-fragments score is
  published beside it with its basis size and each `n_L`, together with the per-language delta. The same
  `epsilon` and the same family are used for both, so the two are comparable.
- All scores clipped to `[0, 100]`; reported to 2 decimals; rankings use unrounded values.
- No logarithmic scaling, no family substitution, no winsorization (spec §25.1, §25.4).

**Mandatory compressed-range reporting (spec §25.1, family C note).** If the applicable raw values span a
factor of 100 or more, publish beside the normalized score: (a) every language's raw value in its natural unit
(items per probe), (b) the ratio `(raw_i + epsilon) / (best_raw + epsilon)` for every language, and (c) the
fixed note that the normalized score is compressed and the ratios, not the scores, carry the comparison among
non-leading languages.

## 1.8 Output files (frozen)

| File | Contents |
|---|---|
| `semantic-compression/raw/hidden_cost_<lang>.json` | Per-probe records: probe id, support level, per-item triggered flags, assigned events with `rule_id` + `citation_id` + `evidence_id`, LED records for every rejected candidate, `H(L,p)`, `H_items(L,p)`, occurrence counts, both analysts' pre-reconciliation records |
| `semantic-compression/raw/hidden_cost_matrix.json` | The 10 × 44 matrix of `H(L,p)` and the 10 × 44 matrix of `H_items(L,p)`, each cell carrying its support level, plus `HSC_raw` and the item-mean aggregate per language **on both bases** |
| `semantic-compression/raw/hidden_cost_basis.json` | The membership of `CommonBasis`, its size, which probes fall out of it and because of which language, each language's `n_L = \|Supp(L)\|`, and the per-language delta `HSC_raw_all − HSC_raw_common` |
| `semantic-compression/raw/hidden_cost_mode_divergence.json` | Per language with more than one frozen execution mode (§0.1): per probe, both modes' outputs and the divergence set |
| `semantic-compression/scores/hidden_cost_scores.json` | Per language and **per basis**, labelled with the basis and its size: `HSC_raw`, `epsilon`, `best_raw`, ratio, normalized score; plus both rankings where they differ |

Per-probe record schema:

```json
{
  "probe": "F03.P1",
  "language": "go",
  "support": "FULL",
  "partial_criteria": [],
  "local_sites": [{"index": 1, "line_span": [9, 9], "text": "<verbatim site text>"}],
  "candidate_set": ["GO.R.0007", "GO.R.0011"],
  "items": {
    "H1": {"triggered": true, "events": [
      {"event_id": "E-1", "rule_id": "GO.R.0007", "local_site_index": 1, "local_site_line": 9,
       "behavior": "<one sentence>", "citation_id": "CIT-GO-0007",
       "evidence_id": "EV-GO-F03.P1-001"}
    ]},
    "H2": {"triggered": false, "led": [
      {"rule_id": "GO.R.0011", "local_site_index": 1, "behavior": "<one sentence>",
       "failed_condition": "3 (no local signal)", "reason": "local signal present: <token>",
       "citation_id": "CIT-GO-0011"}
    ]}
  },
  "H_probe": 1,
  "H_items": 1,
  "items_triggered": ["H1"],
  "occurrences": {"H1": 1},
  "analyst_a": "<pre-reconciliation record>",
  "analyst_b": "<pre-reconciliation record>",
  "reconciled": true,
  "tie_break": "<written resolution, or null>",
  "disputed": []
}
```

Every item must appear in `items`, either with `triggered: true` and ≥1 event, or with `triggered: false`. When
`triggered: false` because a candidate behavior was rejected by the LED gate or by a §1.4 exclusion, the
rejection must be recorded in `led[]` with its `rule_id`, the numbered condition of §1.3.0 that failed, and its
reason. Every member of `candidate_set` must appear exactly once across the union of all `events[]` and `led[]`
entries for that probe (check V17). **Silent omission of a rejected candidate is an audit failure.**

## 1.9 Mandatory self-audit before the metric is published

The operator must complete and store `semantic-compression/raw/hidden_cost_audit.json` confirming:

1. Every triggered item carries at least one `citation_id` or `evidence_id`.
2. No event is assigned to more than one item (§1.3.1 verified mechanically over all event ids).
3. Each of H1–H9 has at least one recorded LED rejection somewhere in the run, demonstrating the gate was
   actually applied rather than skipped. If an item has zero rejections run-wide, that fact is stated
   explicitly with a reason.
4. For every language, the count of items scored 0 is listed, with the LED citation that justifies each zero.
   **A zero is a claim and requires evidence exactly as a trigger does.**
5. No probe was added, removed, reworded, or re-annotated during measurement.
6. For every language and every item H1–H9, the count of candidates rejected at each numbered condition of
   §1.3.0 is published. An item that fires nowhere in the corpus for some language is thereby visibly a
   measured result with a stated reason, not an invisible omission.
7. For every language with more than one frozen execution mode (§0.1), both modes were executed for every
   supported probe and the per-probe divergence set is published.
8. The two-analyst raw agreement rate (§1.3.3) is published per language, together with every tie-break and
   every adjudication-register entry applied during the run.
9. Both the scored aggregate (`HSC_raw`, event-mean) and the item-mean aggregate are published for all ten
   languages, before normalization, **on both bases of §1.6.1**, each labelled with its basis and basis size.
10. `CommonBasis` and its size are published, together with each probe excluded from it and the language whose
    `NONE` excluded it, and each language's `n_L`. No figure is published without its basis and basis size.
11. Every exclusion that is universal in scope (§1.4.0) carries either a citation in which the reference states
    the universal, or an `observed` record enumerating every cell it exercised. The count of universal-scope
    exclusions taken by each route is published per language.

---

# PART 2 — CAPABILITY EFFICIENCY (spec §6.1.4.E)

Weight: **15% of the Semantic Compression quality score `Q`.**
Raw unit: **semantic complexity units per supported capability point.**
Direction: **lower is better.** Normalization family: **C** (spec §25.1).

## 2.1 Definition of a semantic complexity unit (SCU)

A **semantic complexity unit** is one distinct **semantic obligation** that a competent reader or writer must
know in order to **express** at least one supported probe **and** to **correctly interpret** what the frozen
probe implementation means, under the pinned reference.

Semantic obligations are partitioned into exactly four ledgers:

| Ledger | Code | Definition |
|---|---|---|
| Distinct syntax forms | `SF` | A surface construct the author must write and the reader must recognize, whose specified meaning is distinct from every other counted form: a declaration form, an operator, a call form, a literal form, a control form, a pattern form, a modifier/annotation, an effect marker. |
| Context-dependent rules | `CR` | A rule whose outcome depends on something other than the construct itself: surrounding type context, declaration site, scope or module, build mode/flags, target, evaluation order, position in an expression, or the presence of another construct. |
| Implicit rules | `IR` | A rule that produces a semantic effect with no construct written for it: an inserted conversion, an inserted copy/move/allocation/cleanup, a default, an automatic propagation, an implicit receiver or capture, an automatic dispatch choice. |
| Documented semantic exceptions | `SE` | A rule of the shape "the normal rule is A, except here it is B" (see §3.2): a carve-out, special case, or exception to a rule already counted. |

**SCU is the cardinality of the union of the four ledgers**, counted over the rule registry defined in §2.2.
The four-way split exists for reporting; the total is a set cardinality and cannot be inflated by the split.

## 2.2 Canonical rule registry and the non-double-counting rule

Every semantic obligation is assigned a **canonical Rule ID** and appears **exactly once** in the language's
rule registry `semantic-compression/raw/semantic_rules_<lang>.json`:

```
Rule ID = "<LANG>.R.<NNNN>"      e.g. "SWIFT.R.0042"
```

**Rule identity test.** Two candidate obligations are the **same rule** — and therefore share one ID — iff both:

1. they are established by the **same written normative statement** in the pinned reference (same clause, same
   table row, same sentence), **and**
2. they impose the **same obligation on the reader**: knowing one is sufficient to predict the other in every
   supported probe where either appears.

**THE NON-DOUBLE-COUNTING RULE (spec §20, binding).**

> A rule contributes **exactly 1** to SCU, no matter how many probes exercise it, how many of the four ledgers
> it could plausibly fit, how many of the ten §20 descriptive categories it belongs to, or how many times it is
> cited. Membership in several descriptive categories never multiplies the count.

Mechanically enforced as follows:

- **Ledger assignment is a partition.** Each rule is placed in exactly one ledger by the fixed precedence
  **`SE` > `CR` > `IR` > `SF`**, first match wins. The precedence affects only the published breakdown; the
  total `SCU = |registry|` is invariant under it.
- **Descriptive tags are separate from the count.** The §20 categories, and the boolean tags
  `is_exception` / `is_context_sensitive` / `is_implicit`, are **tags on a rule**, may overlap freely, and are
  used only for §20 reporting. They are never summed into SCU.
- **Cross-probe dedup.** A rule exercised by 17 probes appears once in the registry.
- Automated check: `SCU == len(set(rule_ids))` must hold, and each `rule_id` must appear exactly once as a
  registry key. Any duplicate key is a hard failure.

## 2.3 Rule granularity (frozen) — the anti-inflation and anti-deflation rules

Granularity is the main fairness risk of this metric: splitting one rule into ten inflates a language's SCU;
merging ten into one deflates it. Three tests fix granularity identically for all ten languages.

**G1 — Reader-obligation level.** A rule is counted at the granularity of the smallest **independently stated
normative statement** in the pinned reference that a reader could get wrong on its own. Grammar productions,
lexical rules, and tokenization rules are **not** counted unless they carry distinct semantic meaning.

**G2 — Independent Violation Test (splitting bound).** Candidate statements `S1` and `S2` are two rules only if
there exists a program, consistent with the frozen probe set's domain, that violates or misapplies `S1` while
correctly applying `S2`, and vice versa. If no such program exists, they are one rule.

**G2-W — Stored witnesses (mandatory; G2 is not satisfied by assertion).** G2 is an existential claim, so it is
discharged by exhibiting the programs, not by asserting them. For every rule whose `granularity_notes` names a
neighbouring rule, the operator stores **both** witness fragments under
`semantic-compression/raw/granularity/<lang>/<rule_id>/`:

- `violates_this_satisfies_base.<ext>` — a fragment that violates or misapplies this rule while correctly
  applying the neighbour;
- `violates_base_satisfies_this.<ext>` — the converse.

Each witness is built and run under the frozen recipe where the language has one, and its build/run output is
stored beside it. Where the distinction is not dynamically observable, the witness is marked
`"kind": "static"` and carries a `citation_id` establishing the distinction instead of a command (same terms as
§0.6). A rule whose witnesses are absent fails check V18 and is **merged** with its neighbour rather than
counted separately — the anti-inflation direction, so that a missing witness can never raise a language's SCU.

**G3 — Co-extension Test (merging bound).** If two candidate rules are jointly satisfied or jointly violated in
**every** supported probe of that language, they merge into one ID. Conversely, a rule may not be merged with
another merely because both appear in the same probe or the same reference section. The check is recorded in
`g3_check`: either the probes in which both rules are jointly satisfied or jointly violated, or the single
probe that separates them.

**G4 — Calibration pass (mandatory, before the full count; blinded and closed).**

The calibration pass exists to fix granularity once, identically for all ten languages. Because it is the only
point in this methodology at which a rule may still change, it is bounded on all sides:

- **(a) Fixed calibration set, spanning difficulty.** The calibration set is the three probes
  **`F01.P1`** (simple: one immutable binding and its result), **`F09.P2`** (mid: a standard sort plus a
  scalar comparison), **`F17.P1`** (hard: scoped resource acquisition and release on every exit path,
  including the failure path). This set is fixed here, before measurement. One trivial probe is not enough: the
  granularity questions that dominate SCU — generic instantiation, error propagation, resource cleanup — do not
  arise in a constant binding.
- **(b) Blinding.** Calibration packets are **language-anonymized** as `L1`…`L10` by a fixed, seeded shuffle
  recorded in a sealed mapping file: each packet contains the candidate rule statements and their reference
  extracts with language names, dialect names, file extensions, keywords-as-branding, and citation document
  titles replaced by neutral placeholders. The adjudicator writes the granularity text without the mapping.
  The mapping is revealed only **after** the granularity text is frozen and its SHA-256 recorded.
- **(c) Closed list of permitted actions.** The **only** permitted outcomes of calibration are:
  (i) **merge** candidate rules that fail G2, and (ii) **split** a candidate rule that fails G3. Each such
  action is stated as a **general predicate over reference statements** and **may contain no language name,
  no language-specific construct, and no citation to a single language's reference**. No other change of any
  kind is permitted — not to the ledgers, the identity test, the scope rules, the normalization, or the
  weights.
- **(d) Before/after publication.** Every language's SCU on all three calibration probes is published **before**
  the clarification and **again after** it, in `granularity_calibration.json`, so the clarification's effect on
  each language is visible and auditable rather than absorbed.
- **(e) Hard close.** Once any **non-calibration** probe has been counted for any language, **no granularity
  change is permitted for any reason**. A granularity problem discovered after that point is recorded in
  `disputed[]` and in `CORRECTIONS.md`, and is fixed only by a new benchmark run.

If `max(SCU_calib) / min(SCU_calib)` across the ten languages exceeds 5 on the calibration set, the operator
must publish an explicit justification stating that the gap reflects the languages and not the granularity,
citing the specific rules that produce the gap. That justification is a **reporting** obligation; it is not a
licence to take any action outside the closed list in (c), and it may never be used to adjust a single
language.

## 2.4 Scope: which rules are in and which are out

**In scope** — a rule is counted iff it is required to do at least one of these for at least one **supported**
probe, using that probe's frozen implementation:

- write the construct correctly (expression);
- predict the construct's meaning over the probe's declared input domain (correct interpretation), including
  but **not limited to** its effects on the semantic facts listed in the probe's `semantic_facts_expected`.
  That list is a **floor** — the facts a reader must certainly be able to predict — never a ceiling on what a
  rule may be counted for.

**Out of scope — never counted:**

- rules exercised only by probes the language does not support (those already cost the language via `C`);
- tooling, build-system, packaging, formatting, or style rules that do not change program meaning;
- lexical trivia (§0.8);
- rules about diagnostics or error-message wording;
- alternative ways to express the same probe that the frozen implementation does not use. **Only the frozen
  implementation is counted**, so a language is never charged for having many alternatives it did not need.

The last bullet is a deliberate fairness guarantee: this metric charges for the complexity **used**, not for the
size of the language. Language size is measured elsewhere (Capability Coverage `C` rewards breadth).

**Scope basis (binding).** Doc 01's common-basis rule requires metric E's numerator to be counted on the same
basis as metrics A–D and the basis to be stated with the figure. Therefore `SCU` is computed twice, from one
registry, by intersecting each rule's `probes[]` with the basis:

```
SCU_common(L) = | { r ∈ registry(L) : r.probes ∩ CommonBasis ≠ ∅ } |    [PRIMARY — the reported result]
SCU_all(L)    = | { r ∈ registry(L) : r.probes ∩ Supp(L)     ≠ ∅ } |    [SECONDARY — published beside it]
```

`SCU(L)` unqualified means `SCU_common(L)`. Both are published with their basis and basis size, per §1.6.1
rule 4. The registry itself is built once over `Supp(L)`; the basis selects which of its rules are counted, so
no rule is authored, retained, or discarded on the strength of which basis it lands in.

**Reconciliation with §2.4.1 (binding, so the two rules are not read as contradicting each other).** The bullet
above excludes the rules of alternative spellings; §2.4.1 counts a static-acceptance rule that a reader must
know to predict a listed fact, *including* predicting that an alternative spelling would be rejected. These are
reconciled as follows, identically for all ten languages: what §2.4.1 counts is **the acceptance rule the frozen
fragment itself must satisfy** — one rule, the one a reader must apply to the fragment in front of them. It
never licenses counting the **semantics of the alternative construct**, nor one rule per alternative that could
have been written. The alternative is evidence that the acceptance rule has content; it is not itself a counted
obligation. Where an analyst cannot state the rule as a property of the frozen fragment, it is out of scope
under the bullet above.

### 2.4.1 Static-acceptance rules (binding, identical for all ten languages)

Some rules a reader must know govern only whether the fragment is a **legal program**, and change no observable
of a fragment that compiles: an erased type system, a borrow or exclusivity checker, a checked-exception
requirement, a null-safety check, a definite-assignment check, a compile-time-evaluation rule. The document
must rule on these once, for everyone, or each analyst rules per language:

> A rule that governs **only static acceptance** is **counted** iff a reader must know it to predict any fact
> in that probe's `semantic_facts_expected` list — **including** predicting that an alternative spelling of the
> fragment would be rejected, or that the fragment's stated fact holds *because* the rule excluded the
> alternatives. Such rules are tagged **`static_only: true`**, and the per-language count of `static_only`
> rules is published beside `SCU`.

Stated consequences, so the treatment is visible rather than emergent, and applied by the same sentence in
every case:

- **TypeScript.** Its type rules are erased and change no runtime observable, but a reader must know them to
  predict `type_and_representation` at the probe's sites; they are therefore **counted** under this clause.
  They are not waived merely because erasure makes them invisible at run time, and erasure's separate
  consequence — that the runtime value may not match the written type — is a distinct rule assessed on its own.
- **Rust** (borrow/ownership checking), **Java** (checked-exception requirements), **Kotlin** (null-safety
  checks), **Swift** (exclusive-access-to-memory), **Zig** (comptime evaluation rules), **C++** (constant
  expression and access rules), **Go**, **Python**, **Quidra**: the identical sentence applies. Where such a
  rule is needed to predict a listed fact for a supported probe, it counts and is tagged `static_only: true`;
  where it is not, it does not.
- A language is **never** credited for having no static-acceptance rules to count when the reason is that its
  reference does not state them: §0.4's `observed` route and §1.2's documentation symmetry apply here too.

### 2.4.2 Standard-library contracts: how many rules one API call costs

The earlier wording ("a library function's documented contract counts as one rule unless the probe depends on
more of its specified behavior") had no test, and its effect was directionally uneven in both directions: where
a language's standard library supplies a whole probe in one call, that language paid 1 SCU, while a language
assembling the same observable behavior from language constructs paid one SCU per construct. The operational
replacement:

> **A standard-library API contributes one rule per DOCUMENTED BEHAVIORAL CLAUSE of its contract that the
> frozen fragment's stated observable behavior depends on**, where a *clause* is an independently stated
> normative sentence in the pinned library reference (§0.4) — typically: the return value on success; the
> behavior in the absent, empty or failure case; mutation-in-place versus returning a new value; ordering,
> indexing or iteration-order guarantees; complexity guarantees where the probe's stated behavior depends on
> them; and how errors are signalled. The operator lists the clauses relied on, **by citation anchor**, in the
> rule's `granularity_notes`. Clauses the fragment does not rely on are **not** counted.

Clauses are subject to G1–G3 like any other rule, and to the §2.2 identity test: two probes relying on the same
clause share one rule ID.

**Worked illustration (application of the rule, not a pre-assigned score).** For the map-lookup-with-default
probe `F16.P2`, whose canonical task requires (1) construction, (2) iteration over entries summing values, and
(3) a lookup of an absent key substituting `0`, the clauses an analyst would list for step (3) are of this
shape, in whichever spelling the language's frozen fragment actually uses:

| Fragment shape used for step (3) | Clauses the stated behavior depends on |
|---|---|
| A single lookup-with-default call (`get(key, default)`-shaped) | value returned when the key is present; value returned when the key is absent; whether the map is mutated by the lookup |
| A lookup returning an optional/`Option`/nullable, plus a locally written default (`??`, `orElse`, `unwrap_or`-shaped) | value returned when present; the absent representation returned when absent; the default-supplying construct's own rule |
| A lookup returning an optional, plus an exhaustive match/switch over present and absent | value returned when present; the absent representation; the match construct's exhaustiveness rule; the binding rule for the present arm |
| A bare indexing form with a language-defined miss behavior (default value, insertion, failure) | value returned when present; the defined miss behavior; whether the miss mutates the map |

The point of the table is that **each spelling is charged for the clauses its own observable behavior actually
relies on** — a one-call spelling is not charged for clauses it does not use, and a multi-construct spelling is
not discounted for being longer. The same procedure is run on `F17.P1`, where the asymmetry runs the other way
(a scoped-cleanup statement is one construct with a small clause list; a destructor-based idiom relies on
several independently stated clauses about when destruction happens). At measurement time this table is
re-derived against the actual frozen fragments and every clause carries its own citation anchor.

## 2.5 Counting algorithm (frozen, executed in this order)

```
INPUT:  methodology/01_capability_universe_and_probes.json (probes, semantic_facts_expected,
        support_rubric, capability_points), semantic-compression/raw/capability_matrix.json,
        semantic-compression/probes/<lang>/<probe_id>.<ext> (frozen implementations),
        pinned references (§0.4)
OUTPUT: semantic_rules_<lang>.json, capability_efficiency_<lang>.json

STEP 0.  Verify every frozen implementation of a supported probe builds and runs under EVERY frozen
         mode for that language in environment.json (§0.1). A probe whose implementation does not
         build is a support-rubric failure, not a counting problem: stop and report it. Do not
         silently reclassify.

STEP 1.  Run the G4 calibration pass on the fixed calibration set F01.P1, F09.P2, F17.P1 for all ten
         languages, under the blinding and closed-action rules of §2.3. Publish before-and-after SCU
         for all ten. Resolve granularity once, identically for all, before continuing.

STEP 2.  For each supported probe p, walk the frozen implementation construct by construct, in source
         order, local site by local site (§0.7). For each construct, for every semantic fact in p's
         semantic_facts_expected (the floor, §2.4) and for every further fact of §0.8 that the
         fragment's stated observable behavior depends on, write down every normative statement a
         reader must know to predict that fact.

STEP 3.  For each written-down statement, apply G1, G2, G3. Then look it up in the registry using the
         §2.2 identity test.
           - already present  -> append p to its "probes" list; do not create a new ID
           - not present      -> create a new ID with a citation_id (§0.5) and >=1 evidence_id (§0.6)

STEP 4.  Assign each new rule its single ledger by the precedence SE > CR > IR > SF.
         Assign its §20 category (§3.4) and its overlapping boolean tags
         (is_exception, is_context_sensitive, is_implicit, static_only).

STEP 5.  Apply the scope exclusions of §2.4, the static-acceptance clause §2.4.1, and the
         standard-library clause rule §2.4.2. Remove any rule whose "probes" list is empty after the
         supported-probe filter.

STEP 6.  SCU_common(L) and SCU_all(L) per the scope-basis rule of §2.4, from the one registry.
         SCU(L) unqualified = SCU_common(L).
         Verify: |registry(L)| == |SF| + |CR| + |IR| + |SE|  (partition check, must hold exactly)
         Verify: every rule ID unique; every rule has >=1 citation_id or is tagged source "observed";
                 every rule has >=1 probe in "probes"; every G2 neighbour claim has stored witnesses.
         Publish the per-language rule-count histogram by §3.4 category, the static_only count, and
         both basis figures with their basis sizes, BEFORE any normalization.

STEP 7.  CapPoints(L) = SUM over ALL 44 probes of
             capability_points(p) * support_factor(support_level(L, p)),
         taken verbatim from semantic-compression/raw/capability_matrix.json -- the identical
         numerator of C = 100 * CapPoints(L) / 88. Every probe is worth exactly 2 capability
         points (doc 01 capability_point_assignment_rule), so the denominator is 2 * 44 = 88.
         It is a WEIGHTED POINT TOTAL in [0, 88], not a probe count and not an integer count:
         it is fractional whenever any probe is PARTIAL (support_factor 0.5). It is taken over
         all 44 probes on BOTH bases -- it is the capability channel of spec 26 and 6.1.5, and
         narrowing it to the common basis would delete the only channel through which an
         unsupported capability reaches this metric. This methodology does not redefine support;
         it consumes it.

STEP 8.  CapEff_raw(L) = SCU(L) / CapPoints(L), reported on both bases with the basis stated:
             CapEff_raw_common(L) = SCU_common(L) / CapPoints(L)   [PRIMARY]
             CapEff_raw_all(L)    = SCU_all(L)    / CapPoints(L)   [SECONDARY]
```

**Denominator note.** `CapPoints(L)` is read from the frozen capability matrix, including whatever partial
credit the frozen support rubric assigns. Using the identical numerator that feeds `C` guarantees the two
metrics cannot disagree about what "supported" means.

**Declared asymmetry for `PARTIAL` probes.** A `PARTIAL` probe contributes to the SCU numerator **all** the
rules its frozen fragment actually uses, while contributing only **half** its `capability_points` to the
`CapPoints` denominator. Partial support therefore costs a language twice on this metric. That is a real
consequence of the frozen rubric rather than a hidden thumb on the scale, it is declared here **before
measurement**, and it applies identically to all ten languages. It is stated so that a reader can see it in
the arithmetic instead of discovering it in the result; it is not adjusted, compensated, or offset.

## 2.6 Raw value, normalization, and `N/A`

```
CapEff_raw(L) = SCU(L) / CapPoints(L)          [semantic complexity units per supported capability point]
```

Lower is better → **family C**, unshifted form (spec §25.1):

```
Score_i = 100 * best_positive_raw / raw_i
```

where `CapPoints(L) ∈ [0, 88]` is the weighted point total of STEP 7 and may be fractional. The score of record
is computed from `CapEff_raw_common`; `CapEff_raw_all` is normalized and published beside it with both basis
sizes and the per-language delta, and where the two bases disagree about a ranking both rankings are published
(§1.6.1 rules 4–5).

**Zero is not attainable and the shifted form is therefore not used.** Justification, declared before
measurement: whenever `CapPoints(L) > 0`, at least one probe is supported, and expressing any probe requires at
least one distinct syntax form, so `SCU(L) ≥ 1` and `CapEff_raw(L) > 0`. When `CapPoints(L) = 0` the raw value
is undefined and §0.9 applies (`N/A`, reason `"no supported capability points"`, excluded from the applicable
weight denominator with renormalization; `C = 0` forces the overall score to 0 regardless).

All scores clipped to `[0, 100]`, reported to 2 decimals. The compressed-range reporting requirement of §1.7
applies identically here whenever the raw spread reaches a factor of 100.

## 2.7 Traceability requirement (spec §6.1.4.E, binding)

> "This metric must be based on written language rules and reproducible probe evidence, not subjective
> impressions of elegance."

Enforced as a hard schema requirement — a rule missing either half is not counted:

```json
{
  "rule_id": "KOTLIN.R.0031",
  "statement": "<one sentence, in language-neutral terms, stating the obligation>",
  "ledger": "IR",
  "category": "implicit_conversions",
  "tags": {"is_exception": false, "is_context_sensitive": true, "is_implicit": true,
           "static_only": false},
  "citation_id": "CIT-KOTLIN-0031",
  "source": "documented",
  "evidence_ids": ["EV-KOTLIN-F09.P2-002"],
  "probes": ["F09.P2", "F10.P1", "F16.P2"],
  "granularity_notes": "G2 satisfied against KOTLIN.R.0030: <one sentence>",
  "g2_neighbour": "KOTLIN.R.0030",
  "g2_witness": {
    "violates_this_satisfies_base":
      "semantic-compression/raw/granularity/kotlin/KOTLIN.R.0031/violates_this_satisfies_base.kt",
    "violates_base_satisfies_this":
      "semantic-compression/raw/granularity/kotlin/KOTLIN.R.0031/violates_base_satisfies_this.kt",
    "kind": "dynamic"
  },
  "g3_check": "<the probes in which both rules are jointly satisfied or jointly violated, or the probe that separates them>",
  "library_clauses": ["<citation anchor per clause relied on, where §2.4.2 applies>"]
}
```

`g2_neighbour`, `g2_witness` and `g3_check` are required on every rule whose granularity was decided against a
neighbouring rule; `g2_witness.kind` is `"static"` only where the distinction is not dynamically observable, in
which case a `citation_id` stands in for the command (§0.6). `library_clauses` is required on every rule
created under §2.4.2. `static_only` is required on every rule and is `true` exactly for the rules of §2.4.1.

Prohibited as grounds for any count, in any form: elegance, readability impressions, personal preference,
community reputation, popularity, verbosity, aesthetic judgement, "feels simpler", "feels cleaner". Any rule
whose `statement` cannot be restated as a checkable normative claim is moved to `disputed[]` and excluded.

## 2.8 Output files (frozen)

| File | Contents |
|---|---|
| `semantic-compression/raw/semantic_rules_<lang>.json` | The full rule registry (also the §20 raw evidence — see Part 3) |
| `semantic-compression/raw/granularity_calibration.json` | The G4 calibration pass on `F01.P1`, `F09.P2`, `F17.P1`: the anonymized packets, the sealed `L1`…`L10` mapping and its reveal time, every language's SCU on the three probes before and after the clarification, the clarification text and its SHA-256 |
| `semantic-compression/raw/granularity/<lang>/<rule_id>/` | The stored G2 witness fragments and their build/run outputs (§2.3 G2-W) |
| `semantic-compression/raw/capability_efficiency_<lang>.json` | `SCU_common` and `SCU_all` with their basis sizes, ledger breakdown, category histogram, `static_only` count, `CapPoints`, `CapEff_raw` on both bases, per-probe rule attribution |
| `semantic-compression/scores/capability_efficiency_scores.json` | Per language and **per basis**, labelled with the basis and its size: raw, ratio to best, normalized score; plus both rankings where they differ |

---

# PART 3 — SEMANTIC REGULARITY EVIDENCE (spec §20)

**Status (spec §20, binding):** Semantic regularity is **raw evidence for Primary Evaluation 1 — Semantic
Compression**. It is **not a separate Standard category** and **must not be double-counted in Standard**. It
carries **zero weight** in the Standard evaluation, zero weight of its own in `Q`, and appears in the report
only as raw evidence and as the input to Hidden Semantic Cost and Capability Efficiency.

**This methodology is defined before any counting**, as spec §20 requires.

## 3.1 What is one "semantic rule"

A **semantic rule** is one normative statement about what a program **means** — as opposed to what is
syntactically well-formed — that satisfies all of:

1. it constrains or determines an observable semantic fact (§0.8 materiality), **or** it is a
   static-acceptance rule counted under §2.4.1 (tagged `static_only: true`), which a reader must know to
   predict a listed fact of the probe — including predicting that an alternative spelling would be rejected;
2. it is stated as a single independently-stated normative statement in the pinned reference (§0.4), or is
   tagged `source: "observed"` per §0.4;
3. it passes the granularity tests **G1, G2, G3** of §2.3;
4. it is exercised by at least one supported probe (§2.4 scope).

**The registry is shared.** The set of semantic rules for language `L` is *exactly* the rule registry built in
Part 2 (`semantic_rules_<lang>.json`). Therefore, **per basis** (§2.4 scope basis):

```
Total Semantic Rules (L) on a basis  =  SCU(L) on that same basis
T(L)      = |registry(L)| restricted to CommonBasis   =  SCU_common(L)   [primary]
T_all(L)  = |registry(L)| restricted to Supp(L)       =  SCU_all(L)      [secondary, published beside it]
```

This identity is deliberate. It is what makes §20's raw counts feed Capability Efficiency **without any
possibility of double counting**: the same set is counted once and read two ways. The identity holds
**within** a basis; a §20 count and a Capability Efficiency figure may never be compared across bases, and
every published §20 count states its basis and basis size like every other figure (§1.6.1 rule 4).

Not semantic rules: grammar productions without distinct meaning, lexical/tokenization rules, formatting and
naming conventions, diagnostic wording, tooling behavior, performance guidance without semantic content.

## 3.2 What makes a rule an exception / special case

A rule is an **exception** — tagged `is_exception: true` — iff it has the **"normal rule is A, except here it
is B"** shape, operationalized as:

> **Exception test.** There exists another counted rule `A` in the same registry such that, in the situation `B`
> describes, applying `A` yields a **materially different** result (§0.8) from the actual specified behavior,
> and the reference states `B` as a carve-out, special case, restriction, or "except" clause relative to `A`.

Both halves are required:

- **the base rule `A` must itself be a counted rule in the registry** — an exception with no counted base rule
  is not an exception, it is just a rule (`is_exception: false`);
- **the deviation must be material** — a clarification, restatement, or non-semantic note is not an exception.

Each exception records `base_rule_id`, the ID of the rule it deviates from. Automated check: every rule with
`is_exception: true` has a `base_rule_id` that exists in the registry and is not itself.

**Explicitly not exceptions:** a rule that is uniformly general but hard; a rule with many cases enumerated up
front where no case is privileged as "normal" (that is one rule with N branches, or N rules by G2 — never an
exception); a rule that a *different* language treats as an exception; deprecation notes; and any "surprise" a
reader felt that the reference states plainly as the general rule.

## 3.3 Context-sensitive rules and implicit behaviors

These are **tags**, not separate counted entities. A single rule may carry any combination of them, and
carrying several never adds to any total.

**`is_context_sensitive: true`** iff the rule's outcome depends on something outside the construct itself:
surrounding expected type, declaration site vs use site, scope or module or visibility, build mode, compiler
flags, target platform, evaluation order or sequencing, position within an expression, or the presence of
another construct elsewhere. Record `context_dimension` naming which of these it is. A rule that depends on
**more than one** dimension is still **one** rule; the additional dimensions are listed in
`context_dimension[]`, not counted separately.

**`is_implicit: true`** iff the rule produces a semantic effect with **no construct written for it** at the site
where the effect occurs: inserted conversions, inserted copies/moves/allocations/cleanups, defaults, automatic
propagation, implicit receivers or captures, automatic dispatch selection, automatic memory management with a
user-visible effect. The test is "is there a token the author wrote for this effect?" — not "is the effect
surprising?".

Counts reported for §20:

```
Total Semantic Rules      T(L) = |registry(L)|
Exception / Special-case  E(L) = |{r : r.tags.is_exception}|
Context-sensitive Rules   X(L) = |{r : r.tags.is_context_sensitive}|
Implicit Behaviors        I(L) = |{r : r.tags.is_implicit}|
```

`E`, `X`, and `I` are subsets of the same set `T` and **may overlap each other**. They are never summed.
`E + X + I > T` is an expected and acceptable outcome; `E > T`, `X > T`, or `I > T` is a hard failure.

## 3.4 The ten §20 categories (frozen, with counting instructions)

Every rule in the registry carries **exactly one** `category` from this list, assigned by the first matching
row read top to bottom. The category is a reporting label only; it never multiplies a count.

| # | Category key | What belongs here | Counting instruction |
|---|---|---|---|
| 1 | `implicit_conversions` | Conversions inserted without a written conversion construct; promotion, coercion, widening/narrowing, boxing-as-conversion, subtyping coercions | One rule per independently stated conversion rule (G2). A conversion table with materially different rows per source/target class is one rule per class, not per cell. |
| 2 | `context_dependent_meaning` | Constructs whose meaning changes with expected type, position, scope, mode, or target | One rule per independently stated dependence. List extra dimensions in `context_dimension[]`, do not split. |
| 3 | `special_case_syntax` | Forms whose meaning departs from the general form they resemble; syntactic sugar with non-obvious desugaring; forms valid only in one position | One rule per special form. Its `base_rule_id` is the general form it departs from. |
| 4 | `operator_exceptions` | Operators that deviate from the general operator rule for some operand class: short-circuit vs eager, non-associativity, equality vs identity, mixed-signedness, string vs numeric `+`, division/modulo sign, shift semantics | One rule per deviation, keyed to the general operator rule as `base_rule_id`. |
| 5 | `scope_exceptions` | Deviations from the language's normal scoping/shadowing/capture/hoisting/visibility rule | One rule per deviation. The normal scoping rule itself is a rule with `is_exception: false`. |
| 6 | `initialization_exceptions` | Definite-assignment carve-outs, default/zero initialization, partial initialization, initialization order, uninitialized-read semantics, static/global initialization timing | One rule per independently stated carve-out. |
| 7 | `argument_passing_exceptions` | Deviations from the language's normal argument-passing rule: by-value vs by-reference by type, implicit copy, implicit borrow, variadics, defaults, named arguments, autoclosure/lazy arguments, `inout` writeback timing | One rule per deviation. |
| 8 | `ownership_mutation_exceptions` | Deviations from the normal ownership/mutation/lifetime/aliasing rule: interior mutability, escape hatches, special lifetimes, move-out carve-outs, copy vs move by type, destructor ordering | One rule per deviation. |
| 9 | `naming_exceptions` | Identifiers, keywords, or names whose meaning is special-cased: reserved/contextual keywords, magic method or entry-point names, name-driven behavior, resolution-order carve-outs | One rule per special-cased name class, not per name. |
| 10 | `stdlib_convention_exceptions` | Standard-library APIs whose documented contract deviates from the convention the library itself establishes: inconsistent error signalling, inconsistent mutation-in-place vs return, inconsistent ordering/indexing conventions, inconsistent null/optional handling | One rule per documented deviation actually relied on by a supported probe. |
| — | `other` | A counted rule that is genuinely none of the above | Permitted, but each `other` rule must carry a one-sentence justification. If `other` exceeds 20% of `T(L)` for any language, the operator must publish that fact and the reason. |

## 3.5 Rule Exception Density

```
Rule Exception Density (L) = E(L) / T(L)
```

Reported as a fraction in `[0, 1]` and as a percentage, per language, with `E(L)` and `T(L)` published beside
it. If `T(L) = 0` (no supported probes), RED is `N/A` with reason `"no supported probes"` — it is not 0.

**Scoring status.** RED is **raw evidence**, and carries **no weight** in `Q` and **no weight** in Standard
(spec §20). If the final report displays a normalized form of RED for readability, the fixed family is **B**
(bounded error fraction where 0.0 is ideal, spec §25.1):

```
Score = 100 * (1 - clamp(RED, 0, 1))
```

and it must be labelled explicitly as **unweighted evidence, contributing to no evaluation score**. No other
family may be substituted.

## 3.6 How these counts feed the scored metrics — and the anti-double-counting guarantee

| Consumer | What it takes from §20 | Guarantee against double counting |
|---|---|---|
| **Capability Efficiency (§6.1.4.E, Part 2)** | `SCU(L) = T(L) = card(registry(L))`, and the ledger breakdown for reporting | The registry is a **set keyed by canonical rule ID**. A rule counts once regardless of how many probes, ledgers, categories, or tags it touches (§2.2). `E`, `X`, `I` are tags on that same set and are never added to it. |
| **Hidden Semantic Cost (§6.1.4.D, Part 1)** | Rules tagged `is_implicit` or `is_context_sensitive` and listing the probe in `probes[]` are **the closed candidate set** for H1–H9 (§1.3.0, binding, not merely descriptive); every triggered event and every `led[]` rejection carries that rule's `rule_id` plus its `citation_id` or `evidence_id` | Within Hidden Semantic Cost, each hidden **event** is assigned to exactly one checklist item (§1.3.1), and one rule at one local site yields exactly one event (§1.3.2). §20's prohibition is on counting one **rule** twice inside one metric; it is discharged by those two rules. Distinct events — one rule at several sites, or several rules at one site — are distinct concealed behaviors and are counted as such, per spec §6.1.4.D. |
| **Standard evaluation (§8)** | **Nothing.** | Spec §20 forbids it. Semantic regularity contributes zero to every Standard category and zero to the Standard overall weighting of §8.1. |

The two consumers are **different metrics**, and spec §20 prohibits double counting *inside one metric*, not
across metrics. The same underlying rule may legitimately affect Hidden Semantic Cost (it conceals a behavior
at a use site) and Capability Efficiency (it is one unit of complexity the reader must carry). These measure two
different properties of the same fact and are weighted separately in `Q`. This overlap is declared here, before
measurement, and must be restated in the report.

## 3.7 Output files (frozen)

| File | Contents |
|---|---|
| `semantic-compression/raw/semantic_rules_<lang>.json` | The shared registry: every rule with ID, statement, ledger, category, tags, `base_rule_id` where applicable, citation, evidence, probes |
| `semantic-compression/raw/semantic_regularity_summary.json` | Per language: `T`, `E`, `X`, `I`, `RED`, category histogram, `observed`-only rule count, `disputed[]` count |

Summary schema:

```json
{
  "language": "swift",
  "total_semantic_rules": 0,
  "exception_count": 0,
  "context_sensitive_count": 0,
  "implicit_behavior_count": 0,
  "rule_exception_density": 0.0,
  "category_histogram": {"implicit_conversions": 0, "context_dependent_meaning": 0,
    "special_case_syntax": 0, "operator_exceptions": 0, "scope_exceptions": 0,
    "initialization_exceptions": 0, "argument_passing_exceptions": 0,
    "ownership_mutation_exceptions": 0, "naming_exceptions": 0,
    "stdlib_convention_exceptions": 0, "other": 0},
  "documented_rule_count": 0,
  "observed_only_rule_count": 0,
  "static_only_rule_count": 0,
  "library_clause_rule_count": 0,
  "disputed_count": 0,
  "na": null
}
```

---

# 4. Execution order, validation, and freeze declaration

## 4.1 Mandatory execution order

1. Verify the frozen inputs of §0.3 exist and are themselves frozen; record the SHA-256 of
   `01_capability_universe_and_probes.json` as the pre-measurement hash; and verify that this document's stated
   literals (`N = 44` probes, 88 capability points, 2 points per probe) equal that file's `probe_count`,
   `total_fixed_capability_points` and `capability_point_assignment_rule`. A mismatch is a defect in **this**
   document, corrected under spec §10.4 before measurement begins (§0.3).
2. Pin and snapshot the reference documents of §0.4; record retrieval dates.
3. Build and run every supported probe under **every** frozen execution mode of its language (§0.1), and
   record the per-probe mode-divergence set.
4. Run the **G4 granularity calibration** on the fixed set `F01.P1`, `F09.P2`, `F17.P1` across all ten
   languages, blinded as `L1`…`L10` (§2.3). Publish before-and-after SCU for all ten. Resolve granularity once,
   identically for all ten, before any further counting. After step 5 begins, granularity is closed.
5. Build the rule registry per language (Part 2, STEP 2–6). This simultaneously produces the §20 evidence and
   fixes the closed candidate set that Part 1 consumes (§1.3.0).
6. Score Hidden Semantic Cost per probe per language (Part 1), two analysts independently (§1.3.3), each event
   and each rejection citing its registry `rule_id`.
7. Compute `SCU`, `CapPoints`, and `CapEff_raw` (Part 2, STEP 7–8).
8. Compute `T`, `E`, `X`, `I`, `RED` (Part 3).
9. Compute `CommonBasis = ∩_L Supp(L)` from the completed capability matrix and publish it with its size, its
   excluded probes and the excluding language, and each language's `n_L` (§1.6.1). The basis is computed from
   the frozen support matrix only, after all ten languages' support levels are fixed, and is never adjusted.
10. Publish all raw values, matrices, histograms, deltas and agreement rates **before** normalizing (spec §25),
    each labelled with its basis and basis size.
11. Normalize both bases: Hidden Semantic Cost by family C shifted with `epsilon = 1/N = 1/44`; Capability
    Efficiency by family C unshifted. Clip to `[0, 100]`; report 2 decimals; rank on unrounded values. The
    common-basis figure is the result; the all-fragments figure is published beside it.
12. Run the audits of §1.9 and §4.2. Publish the audit files.

Languages are measured in the fixed column order of §0.1. Measuring Quidra first or last is immaterial; the
order is fixed so it cannot be chosen to suit an outcome.

## 4.2 Automated validation checks (all must pass before publication)

| Check | Condition |
|---|---|
| V1 | Every `H(L,p)` is a non-negative integer, equal to `Σ_i occurrences(L,p,i)`; every `H_items(L,p)` is an integer in `[0, 9]`. |
| V2 | Every triggered checklist item has ≥1 event, and every event has a `rule_id` and a `citation_id` or `evidence_id`. |
| V3 | No event id appears under two checklist items (single-assignment, §1.3.1). |
| V4 | Every `items.Hx.triggered == false` that rejected a candidate has a recorded `led[]` entry with a reason. |
| V5 | `SCU(L) == len(set(rule_ids))` and `SCU(L) == card(SF)+card(CR)+card(IR)+card(SE)`. |
| V6 | Every rule has ≥1 entry in `probes[]`, and every listed probe is supported by that language. |
| V7 | Every rule has a `citation_id`, or `source == "observed"` with a reproduction command and output. |
| V8 | Every `is_exception: true` rule has a `base_rule_id` present in the registry and different from itself. |
| V9 | `E(L) ≤ T(L)`, `X(L) ≤ T(L)`, `I(L) ≤ T(L)`. |
| V10 | `CapPoints(L)` equals, numerically to full precision, `Σ_p capability_points(p) × support_factor(support_level(L,p))` summed over **all 44** probes from `capability_matrix.json`, and equals that file's `cap_points_total`; it lies in `[0, 88]` and may be fractional; and that file's `denominator` equals `total_fixed_capability_points` in `01_capability_universe_and_probes.json`. |
| V11 | `NONE` probes appear in no Hidden Semantic Cost denominator on either basis, in no rule's counted `probes[]` for that language, and in no language's `CommonBasis`. |
| V12 | Every `N/A` has a reason string drawn from the two permitted cases in §0.9. |
| V13 | Probe count, probe ids, and the capability-point denominator are **exactly** those in `methodology/01_capability_universe_and_probes.json` (`probe_count`, `probes[].probe_id`, `total_fixed_capability_points`), and the literals this document states (44 probes, 88 points) equal them; the SHA-256 of that file matches the pre-measurement hash recorded at §4.1 step 1 (no probe added, removed, reworded, or re-annotated during measurement). |
| V14 | Both normalized metrics lie in `[0, 100]` with 100 = best (spec §25.4). |
| V15 | For each family-C metric whose applicable raw spread ≥ 100×, the raw values, the ratios, and the fixed compression note are present in the published output. |
| V16 | For every language with more than one frozen execution mode (§0.1), every supported probe was executed under **every** mode, each run has its own evidence record naming the mode, and the per-probe divergence set is published. |
| V17 | For every (L, p): every `event.rule_id` and every `led[].rule_id` exists in `registry(L)` and lists `p` in its `probes[]`; and the union of `events[]` and `led[]` rule ids equals the closed candidate set of §1.3.0 exactly — no member missing, no member outside it. |
| V18 | Every rule naming a `g2_neighbour` has both G2 witness fragments on disk under `semantic-compression/raw/granularity/<lang>/<rule_id>/`, and each either builds and runs under the frozen recipe or is marked `"kind": "static"` with a `citation_id`. Every rule created under §2.4.2 has a non-empty `library_clauses`. |
| V19 | The G4 calibration published the sealed `L1`…`L10` mapping, its reveal timestamp, the clarification text with its SHA-256, and every language's calibration SCU before and after; and every clarification statement contains no language name. |
| V20 | `CommonBasis` equals `∩_L Supp(L)` computed from `capability_matrix.json`, is identical for all ten languages, and is published with its size, its excluded probes and the excluding language, and each language's `n_L`. Every published figure of Hidden Semantic Cost, Capability Efficiency and §20 carries its basis label and basis size; a figure without them fails this check. `epsilon` equals `1/N` from the frozen `probe_count` and was **not** recomputed from `\|CommonBasis\|` or from any `\|Supp(L)\|`. |
| V21 | Every exclusion that §1.4.0 lists as universal in scope carries either (a) a `citation_id` whose `quoted_rule` states the universal, or (b) an `observed` record enumerating every frozen mode, candidate, and reachable edge class it exercised, with each cell's output. No universal-scope exclusion rests on a single run under a single mode. |
| V22 | For both metrics, the primary reported score is the **common-basis** figure; the all-fragments figure is present, normalized with the same family and the same `epsilon`, and the per-language delta and both rankings are published wherever the rankings differ. |

Any failed check blocks publication of the metric. A failed check is fixed by correcting the *measurement*, never
by relaxing this document.

## 4.3 Freeze declaration

This methodology is frozen as of benchmark run `2026-09-17-7677581`. It was authored without reference to any
measured result, and **no rule in it is derived from Quidra's syntax, operators, types, or feature set**.

**Precise statement of that independence, so the claim is checkable rather than rhetorical.** The document does
name Quidra in four places, and each is a factual statement about the frozen environment that is made in the
same terms for all ten languages, not a rule shaped around what Quidra does: its row in the comparison table
(§0.1); its row in the pinned-reference table, alongside the asymmetry declaration that covers TypeScript and
Zig on identical terms (§0.4, §1.2); the observation that it is, at this freeze, the one language whose frozen
recipe lists two execution engines — an observation that *adds* an obligation rather than an exemption, and that
applies verbatim to any other language given a second mode (§0.1); and its inclusion by name in the list of all
ten languages to which §2.4.1's single sentence applies. No checklist item, exclusion, granularity test,
threshold, basis, constant, or normalization family in this document is conditioned on a construct that only one
language has. Every rule was tested against the question *would this look equally reasonable if Quidra were
replaced by Zig, or by Python?*

**Pre-measurement remediation.** This document was revised in two passes, both after an adversarial audit of the
frozen methodology and both **before any language was measured under it**: at the time of each revision no probe
had been counted, no rule registry existed, `semantic-compression/raw/` and `semantic-compression/scores/` were
empty, and no Hidden Semantic Cost or Capability Efficiency value had been computed for any of the ten
languages. The second pass applied the audit's cross-cutting checks and reconciled this document with sibling
document 01, which had itself been re-frozen at 44 probes and an 88-point denominator in the interval.
The revision is therefore pre-registration, not post-result formula selection (spec §25.4). Every change is
itemised in the remediation changelog at the end of this document, including the expected direction of each
correction's effect on Quidra's score. From this freeze onward the prohibitions below apply without exception.

Permanently prohibited under this document (spec §25.4, §32):

- changing the checklist, the LED gate, the counting units, the granularity tests, the exception test, the
  weights, `epsilon`, or the normalization families after any language has been measured;
- adding, removing, rewording, or re-annotating a probe after measurement begins;
- relabelling an unsupported capability as `N/A` to avoid a coverage penalty;
- reporting a count that has no citation and no reproducible evidence;
- omitting a rejected candidate, a disputed rule, a failed trial, or an inconvenient raw value;
- changing granularity after any non-calibration probe has been counted, or taking any calibration action
  outside the closed list of §2.3 G4(c);
- exempting any language's execution mode, engine, or build configuration from measurement, or recording a
  divergence between two modes of one language as "a note only" (§0.1);
- changing the basis on which any metric is computed, adjusting `CommonBasis`, or publishing any figure without
  its basis and basis size, after any language has been measured (§1.6.1);
- discharging a universal-scope exclusion by a single run under a single mode (§1.4.0);
- introducing any rule, threshold, or transformation whose effect would be to improve Quidra's position.

A zero on a checklist item, a low rule count, a high rule count, or a last-place normalized score is a
**result**, for any of the ten languages including Quidra, and is published as measured.

---

## Remediation changelog

Applied in one pre-measurement pass against the adversarial audit brief for this document. **No score had been
computed from this or any sibling methodology document when these corrections were made**: the probe
implementations, the rule registries, `semantic-compression/raw/` and `semantic-compression/scores/` were all
empty, and no language had been measured under any part of this methodology. These corrections are therefore
pre-registration under spec §25.4, not selection of a formula after seeing results.

Every change below was tested for symmetry: *would this rule look equally reasonable if Quidra were replaced by
Zig, or by Python?* No rule in this document is conditioned on a construct that only one language has.

### Finding 1 — [BLOCKER] Quidra bias: the only execution-mode exemption in the document was Quidra's

**Defect.** §0.1 pre-declared that where a frozen recipe has both a native and an interpreted form (Quidra),
interpreter behavior "is recorded as a note only and never changes a count", while H8a charges any language
whose outcome differs across build modes, and §1.2.1 illustrates that charge against a language with
debug/release overflow divergence. Quidra is the only language in the frozen recipe table with two shipped
execution engines, so the one language with a real, measurable mode divergence had it pre-declared out of scope
while others are charged for mode divergence the pinned recipe never exercises.

**Change.** The exemption is deleted and replaced with a language-neutral multi-mode rule: primary evidence
from the native compiled form; **every** frozen execution mode of **every** language is run for **every**
supported probe; any material divergence between modes is a configuration dependence and triggers the same
item it would for any other language (H8a numeric, H3 dispatch, H4 resources, H5 failure, H6 optionality,
H7 representation, H9 state). Both Quidra engines (`quidra build`/`./BIN` and `quidra run`) are executed and
any divergence is counted. A second clause fixes that mode-dependence is assessed from the pinned reference
rather than from whichever mode the recipe pins, identically for all ten, so a language is neither excused for
having its checked mode pinned nor punished for having its unchecked mode pinned. New check **V16** and §1.9
audit item 7 enforce it; `hidden_cost_mode_divergence.json` publishes the divergence sets.

**Direction: LOWERS Quidra's expected score** on Hidden Semantic Cost — it exposes Quidra's two engines to a
charge they were exempt from, and removes an exemption no other language had.

### Finding 2 — [BLOCKER] Mechanical reproducibility: every named frozen input was wrong

**Defect.** The document named `probes.json`, `capability_universe.json`, `semantic_facts.json`,
`impl/<lang>/PR-NN.<ext>` and probe ids `PR-01`…`PR-40`. None exist. The real corpus is
`methodology/01_capability_universe_and_probes.json` with ids `F01.P1`…`F20.P2` and implementations at
`semantic-compression/probes/<lang>/<probe_id>.<ext>`. V13 made this fatal: it blocked publication unless the
ids were `PR-01`…`PR-40`. §1.6 also used a support vocabulary (`S`/`P`) the frozen rubric does not use.

**Change.** §0.3 rewritten to name the real inputs, including the rubric's real levels (`FULL`/`PARTIAL`/`NONE`
with `support_factor` 1.0/0.5/0.0), the real coverage denominator (103 points), and the real measured-fragment
rule (doc 01 R5). Every `PR-NN` replaced by `<probe_id>` / a real id throughout (§0.6, §1.3.0, §1.6, §1.8,
§2.3, §2.5, §2.7, §4.1, V13). §1.6 restated as `Supp(L) = { p : support_level(L,p) ∈ {FULL, PARTIAL} }`; §1.8
schema field is `"support": "FULL" | "PARTIAL"` with `partial_criteria`. V13 restated against the real file and
its SHA-256. A capability-matrix record format is declared so V10 is numerically checkable.

**Direction: neutral for Quidra** (a reproducibility fix, applied to all ten identically).

### Finding 3 — [BLOCKER] Part 1 never defined the candidate set

**Defect.** §1.3 tested "a candidate behavior B" without saying where candidates come from, so `H(L,p)` was not
reproducible; §3.6's sentence about `is_implicit`/`is_context_sensitive` rules sat in a Part 3 reporting table
and was never binding; the event schema had no `rule_id`, so nothing tied a trigger to the registry; and a
single unnamed operator made the central judgement with no second analyst.

**Change.** New **§1.3.0** makes the candidate set closed and mechanical: exactly the registry rules listing
that probe and tagged `is_implicit` or `is_context_sensitive`; nothing outside it may be counted; every member
must be adjudicated and recorded as a triggered event or an `led[]` rejection. `rule_id` is now required on
every event and every rejection (§1.8), and new check **V17** verifies that the union of events and rejections
equals the candidate set exactly. §3.6's row is restated as binding rather than descriptive. New **§1.3.3**
imports methodology 03 §3.2: two independent analysts, both records preserved, a fixed disagreement ladder, a
shared adjudication register whose rulings name no language and apply retroactively to all ten, and a published
raw agreement rate. *Partial implementation, recorded honestly:* the brief asks for "language-anonymized"
packets; for Part 1 the evidence **is** the language's own source file, so packet anonymization is impossible.
The strongest achievable form is adopted instead — blinding to all running aggregates and to other languages'
counts, which is methodology 03's own mechanism and the quantity an analyst could otherwise steer — and the
impossibility is stated in the text rather than papered over.

**Direction: neutral for Quidra** (reproducibility and dual adjudication apply identically to all ten).

### Finding 4 — [BLOCKER] "Local site" was defined against an artifact that does not exist

**Defect.** §0.7 defined the local site as "the single statement the frozen probe annotation points at". No such
pointer exists; the corpus has `measured_fragment`, which is several statements for many probes (`F16.P2` four,
`F17.P1` a whole function, `F09.P2` two), and §1.3.2 already presumed multiple sites per probe. `H(L,p)` moved
by several items depending on which reading an analyst took.

**Change.** §0.7 rewritten: the local sites of `p` are exactly the top-level statements of its
`measured_fragment` in source order (or the single expression, for an expression-only fragment), indexed as
`local_site_index`; nested constructs belong to their enclosing top-level statement; **tokens in a different
local site of the same fragment are not local** to the site under adjudication (a `match` two statements later
does not make an earlier lookup-miss locally evident); each site is adjudicated separately and `H(L,p)`
aggregates over all sites. Terminology is aligned with methodology 03's *focus span*, with an explicit ruling
that each document governs its own metrics and a mismatch is a corpus defect for `CORRECTIONS.md`, never an
analyst's choice.

**Direction: LOWERS Quidra's expected score, and every language's, on Hidden Semantic Cost** — the narrower,
site-by-site reading forecloses the lenient reading in which a later statement in the same fragment could be
claimed as a local signal.

### Finding 5 — [MAJOR] The "in annotated scope" gate had no spec warrant and an undefined verb

**Defect.** §1.3 condition 3 required B to affect a fact listed in the probe's annotation. Spec §6.1.4.D has no
such filter, "affects" was undefined, and the lists are narrow and uneven — silencing H2 on the mutation probe
`F03.P1`, and leaving H1 undecidable on the one comparison probe `F09.P2`.

**Change.** Condition 3 is **deleted** (the spec-faithful option the brief lists first); §1.3 now has four
conditions. The reproducibility bound it was attempting is supplied properly by the closed candidate set of
§1.3.0. The deletion is recorded in the text itself so the change is visible. The same annotation gate embedded
in **H5** ("the failure mode is one the annotation lists") and **H8b** ("the edge classes the annotation
lists") is removed for the same reason, with an explicit statement that the fact list does not gate them. §2.4
now states that `semantic_facts_expected` is a **floor, never a ceiling**, and §1.9 audit item 6 requires
publication of how many candidates each condition rejected, per language per item, so a corpus-wide silence is
visible.

**Direction: MIXED across languages, and on balance LOWERS Quidra's expected score** on Hidden Semantic Cost —
it un-silences H2, H5 and H8 on probes whose fact lists omitted the relevant fact, and Quidra's fragments are
adjudicated under the same un-gated test as everyone's.

### Finding 6 — [MAJOR] The scored unit was items, not behaviors

**Defect.** §1.5 scored "distinct checklist items triggered", capped at 9, explicitly collapsing five concealed
conversions to 1. Spec §6.1.4.D says to count **behaviors**; §20's anti-double-counting rule is already fully
discharged by §1.3.1, and the collapse flattened exactly the differences the metric exists to expose.

**Change.** `H(L, p)` is now the count of **distinct events** (§1.3.2) assigned to any item, each event assigned
to exactly one item. The item-level view is retained as published raw evidence: `items_triggered`,
`H_items(L,p)` as a full 10 × 40 matrix, per-item occurrence counts, and the item-mean aggregate published
beside `HSC_raw`. V1 updated to "non-negative integer"; §1.7's epsilon derivation still holds because the
per-probe unit remains an integer over a fixed 40-probe corpus.

**Direction: MIXED, and expected to LOWER Quidra's relative position** where any language conceals few *kinds*
of behavior but many *instances*; more generally it stops every language from having repeat concealment
rounded down to one point.

### Finding 7 — [MAJOR] G4 was a legitimized post-result lever on the metric's largest determinant

**Defect.** The operator counted SCU for one trivial probe across all ten languages **with identities visible**,
looked at max/min, and was then authorized to make an unbounded "clarification of granularity". Granularity is
the single largest determinant of SCU.

**Change.** G4 is blinded and closed: (a) a fixed calibration **set** spanning difficulty — `F01.P1`, `F09.P2`,
`F17.P1` — instead of one constant binding; (b) packets anonymized as `L1`…`L10` with a sealed mapping revealed
only after the granularity text is frozen and hashed; (c) a **closed list** of permitted actions — merge rules
failing G2, split rules failing G3, each stated as a general predicate over reference statements containing no
language name — and nothing else; (d) every language's calibration SCU published **before and after** the
clarification; (e) a hard close: once any non-calibration probe is counted, no granularity change for any
reason. The >5 ratio trigger is now a reporting obligation only, not a licence to act. New check **V19**;
§4.3's prohibition list extended.

**Direction: removes a lever that could have been used in Quidra's favour; expected direction for Quidra is
therefore LOWER or unchanged, never higher.**

### Finding 8 — [MAJOR] G2/G3 were stated but unfalsifiable

**Defect.** G2 is an existential over program space and G3 a universal over the corpus, yet the only required
artifact was a one-sentence `granularity_notes` assertion. SCU — the whole numerator of Capability Efficiency
and of §20's Total Semantic Rules — was therefore not reproducible.

**Change.** New **G2-W**: both witness fragments must be stored on disk under
`semantic-compression/raw/granularity/<lang>/<rule_id>/`, built and run under the frozen recipe, or marked
`"kind": "static"` with a citation. A rule whose witnesses are absent is **merged** with its neighbour rather
than counted — the anti-inflation direction, so a missing witness can never raise a language's SCU. Schema
gains `g2_neighbour`, `g2_witness`, `g3_check`. New check **V18**. STEP 6 now requires the per-language
rule-count histogram by category and the `static_only` count to be published **before** normalization.

**Direction: neutral for Quidra in expectation** (it binds all ten equally), but it removes the possibility of
an unauditable SCU for any language.

### Finding 9 — [MAJOR] The standard-library counting rule was undefined and directionally uneven

**Defect.** "One rule unless the probe depends on more of its specified behavior" had no test, so a language
whose stdlib supplies a probe in one call paid 1 SCU while a language assembling the same behavior from
constructs paid one per construct — in both directions, depending on the probe.

**Change.** New **§2.4.2**: one rule per **documented behavioral clause** of the contract that the frozen
fragment's stated observable behavior depends on, a clause being an independently stated normative sentence in
the pinned library reference; clauses listed by citation anchor in `granularity_notes` and `library_clauses`;
clauses not relied on are not counted. A worked cross-language illustration for `F16.P2` pins the granularity by
demonstration, written by **fragment shape** rather than by language name so that any of the ten can fall into
any row, with `F17.P1` named as the case where the asymmetry runs the other way. V18 requires
`library_clauses` to be non-empty on such rules.

**Direction: MIXED across languages; for Quidra, expected to RAISE SCU and therefore LOWER its Capability
Efficiency score** wherever its fragments reach an observable behavior through several constructs and clauses
that the old rule would have charged as one.

### Finding 10 — [MAJOR] No ruling on compile-time-only rules

**Defect.** §3.1 required a counted rule to determine an observable fact. Read literally, TypeScript's erased
type system counts for almost nothing — making it look artificially cheap on SCU/CapPoints while its erasure
simultaneously raises its Hidden Semantic Cost — and the same question was unresolved for Rust's borrow
checker, Java's checked exceptions, Kotlin's null-safety checks, Swift's exclusivity rule and Zig's comptime.

**Change.** New **§2.4.1**, one sentence applied to all ten: a rule governing only static acceptance is counted
iff a reader must know it to predict a fact in the probe's `semantic_facts_expected`, **including** predicting
that an alternative spelling would be rejected; such rules are tagged `static_only: true` and their count is
published per language beside SCU. The consequence is stated explicitly for TypeScript (erased type rules are
counted) and named for Rust, Java, Kotlin, Swift, Zig, C++, Go, Python and Quidra by the identical sentence.
§3.1 clause 1 amended to match; summary schema gains `static_only_rule_count`.

**Direction: LOWERS Quidra's expected Capability Efficiency score in relative terms** — Quidra's own
static-acceptance rules now count against its SCU exactly as every other language's do, and the languages whose
static rules were at risk of being under-counted are no longer made to look artificially cheap.

### Finding 11 — [MAJOR] The `observed` escape hatch covered triggers but not exclusions

**Defect.** §0.4 rules that an undocumented-but-demonstrable rule still counts, but H4's exclusion was worded
"documented as allocating" and H1/H7's hinge on "the reference defines". A language with an exhaustive
reference escaped charges that an identical construct incurred in a language with a thin one, making part of
Hidden Semantic Cost a measure of documentation prose. (As written this cut **against** Quidra; it is corrected
anyway, because a non-neutral rule is a defect whichever way it points.)

**Change.** §1.2 gains a binding documentation-symmetry clause: an LED or any §1.4 exclusion may be established
by a `documented` citation **or** by an `observed` record under §0.4, on identical terms, with the record
published in the `led[]` entry; wherever §1.4 says "documented as" or "the reference defines", read "documented
**or** reproducibly demonstrated". H4's first exclusion is reworded as a **construct** test (composite or
container literal, constructor or factory call, `new`-style operator, explicit allocator, explicit allocation
call, allocating macro) rather than a prose test.

**Direction: RAISES Quidra's expected score slightly** on Hidden Semantic Cost, together with Zig's and
TypeScript's, by removing a penalty that tracked reference thoroughness rather than concealment. It is applied
because the rule was non-neutral, not because of who it benefits; it is the one correction in this pass whose
direction favours Quidra, and it is reported as plainly as the others.

### Finding 12 — [MINOR] `CapPoints` was misdescribed as a count

**Defect.** STEP 7 and V10 described `CapPoints` as "the number of supported capability points". The frozen
numerator is `Σ capability_points × support_factor` — a weighted, possibly fractional sum with maximum 103, not
40. The document also never declared that a `PARTIAL` probe contributes its rules in full to SCU but only half
its points to `CapPoints`.

**Change.** STEP 7 rewritten as the weighted sum, stated as lying in `[0, 103]` and possibly fractional, and
identified as the identical numerator of `C = 100 × CapPoints / 103`. V10 now checks that equality numerically
against the matrix. §2.5 gains a declared-asymmetry paragraph stating the `PARTIAL` double charge before
measurement, applying identically to all ten, neither adjusted nor offset. §2.6 restates the range.

**Direction: neutral for Quidra** (arithmetic clarification), though it makes an existing double charge for
partial support explicit for every language.

### Finding 13 — [MINOR] The epsilon derivation was wrong as stated

**Defect.** `epsilon = 0.025` was derived from "the maximum denominator is 40 probes", but the denominator is
`|Supp(L)|`, which is below 40 for any language with a `NONE` probe — and several are expected. A
wrong-but-cited derivation invites a later "correction" of a predeclared constant, which §25.4 forbids.

**Change.** Restated as `epsilon = 1/N` where `N` is the fixed corpus size — the finest resolution the metric
can attain for any language — a single constant for all ten, explicitly **not** recomputed from any language's
`|Supp(L)|`, and unchangeable after measurement begins. Cross-referenced to methodology 03 §1.8.3's identical
`epsilon = 1/N` convention.

**Direction: unchanged** (only the justification was corrected in this pass).

*Superseded in the second pass by **S1**: the frozen corpus size is 44, not 40, so the predeclared
recomputation rule fired and `epsilon = 1/44`. The rule that fixed it is the one written here.*

### Finding 14 — [MINOR] H3's third exclusion was unusable in both readings

**Defect.** "Multiple candidates that the reference proves observationally equivalent" sets a bar no reference
meets; read strictly the exclusion was dead, read loosely it let any overload set be dismissed.

**Change.** Replaced with an on-the-record test: candidates are excluded only if the operator records, **per
candidate**, that it cannot differ from the others in any §0.8 respect for the probe's declared input domain,
citing a reference clause or an `observed` record for each; absent that record the exclusion does not apply and
H3 triggers. The "not semantically interchangeable" clause in "Counts when" is restated to point at the same
test.

**Direction: MIXED; expected to LOWER scores for languages with rich overload or dispatch sets**, and neutral
to slightly lower for Quidra, since an unrecorded dismissal is no longer available to any language.

### Finding 15 — [MINOR] No bias found in the LED illustrations

No change made, as the brief directs. The eight mandatory §1.2.1 illustrations describe an explicit-allocator
language, an explicit-propagation language, arbitrary-precision integers, distinct wrapping/trapping operators,
bare calls with by-reference callees, debug/release overflow divergence, and implicit destructors — none of
which is Quidra's spelling of anything, and the fifth of which charges Quidra too. The set was **not**
"balanced" by adding a row that describes Quidra, which would itself have been a bias. Retained verbatim.

### Findings rejected

None. Every finding in the brief was applied in the direction the auditor specified. One finding (3) is
recorded above as partially implemented, with the reason stated in the text rather than used to weaken the
requirement: source-code packets cannot be anonymized by language, so the blinding requirement is discharged in
the strongest achievable form (blinding to all running aggregates), which is methodology 03's own mechanism.

---

## Remediation changelog — second pass (cross-cutting self-review and sibling reconciliation)

Findings 1–15 above were verified as still correctly applied in the text. This pass adds the audit's
**cross-cutting checks**, which the brief directs this document to run against itself even where they were not
itemised, and reconciles the document with sibling doc 01 as re-frozen. **No results had been observed at the
time of this pass either**: `semantic-compression/raw/`, `semantic-compression/probes/` and
`semantic-compression/scores/` were empty, no capability matrix existed, and no score, sub-score or rank had
been computed for any of the ten languages from this or any sibling document. These corrections are therefore
pre-registration under spec §25.4, not post-result formula selection.

### S1 — [BLOCKER] The corpus size and the coverage denominator contradicted the frozen probe file

**Defect.** This document stated `N = 40` probes, "two probes per capability family", and a 103-point Capability
Coverage denominator. The frozen input `01_capability_universe_and_probes.json` states **44** probes (families
`F04`, `F05`, `F14`, `F15` carry three), **2 capability points per probe**, and a denominator of **88**. The
document's own blocking checks were therefore unpassable: V13 required "probe count is exactly 40" and V10
required `CapPoints ∈ [0, 103]`. Doc 01 records this as an explicit counterpart obligation on this document
("update metric E's denominator reference from 103 to 88 … with the 2-points-per-probe rule"). This is the same
class of defect as first-pass Finding 2, resurfaced because the sibling document was re-frozen afterwards.

**Change.** `40 → 44` and `103 → 88` throughout (§0.3, §0.9, §1.5, §1.6, §1.7, §1.8, §2.5 STEP 7, §2.6, V10,
V13, §4.1). `epsilon = 1/N` recomputed **once, before any language is scored**, from the frozen `probe_count`:
`epsilon = 1/44 = 0.0227272727…`, replacing `1/40 = 0.025`. This is the recomputation §1.7 and first-pass
Finding 13 predeclared for exactly this case, not a new choice of constant. §0.3 gains a binding statement that
the literals are **read from** the probe file and never asserted against it, and that a disagreement is a defect
in *this* document. V13 now checks the literals against the file's `probe_count` and
`total_fixed_capability_points`; §4.1 step 1 verifies them before measurement.

**Direction: MIXED and determined by measurement, not by this change.** The four added probes are counterpart
probes in the aliasing, argument-passing, alternatives and generics families, which no language is exempt from;
a smaller `epsilon` slightly widens the normalized spread among the leading languages for all ten equally.
Whether it helps or hurts Quidra is an output of the measurement. It is recorded here as a correctness fix.

### S2 — [BLOCKER] Variable-basis aggregation: a score changed by averaging rather than by measurement

**Defect.** §1.6 computed `HSC_raw(L)` as the mean of `H(L, p)` over `Supp(L)` — **each language averaged over
its own subset of the corpus**. This is the aggregation shape the audit's cross-cutting list names. The probes a
language scores `NONE` are, as a class, the hidden-cost-heavy ones (concurrency, FFI, deterministic destruction,
closures, heap identity), so dropping them from the denominator raises the metric, and raises it most for
whichever language has the most `NONE`s. Spec §6.1.4 nowhere authorises a variable basis, and doc 01's
re-frozen `support_rubric.interaction_with_quality_metrics` requires metric D to be computed primarily on the
**common basis**. The two documents contradicted each other, and this document's side was the permissive one.

**Change.** New **§1.6.1**: `CommonBasis = ∩_L Supp(L)`, the set of probes for which **all ten** languages have
a fragment, `FULL` or `PARTIAL`. `HSC_raw_common` is the primary, reported figure; `HSC_raw_all` is normalized
and published beside it with each language's `n_L`, both basis sizes, and the per-language delta; where the two
rankings differ both are published and the common-basis ranking is the result. `PARTIAL` fragments enter the
common basis identically for all ten, so that partial support is never better for `Q` than no support at all.
`CommonBasis` is an **output** of the frozen support matrix and may not be adjusted; `epsilon` is explicitly
**not** recomputed from `|CommonBasis|`, which would let a measured quantity move a predeclared constant. The
same basis rule is extended to metric E's numerator (§2.4: `SCU_common` / `SCU_all` from one registry) and to
the §20 counts (§3.1), as doc 01's `metric_E_denominator` requires. `CapPoints` stays over all 44 probes,
because it is the single channel through which an unsupported capability reaches the score (spec §26). New
checks **V20** and **V22**, new output file `hidden_cost_basis.json`, §1.9 audit item 10, §4.1 step 9, and a new
prohibition in §4.3.

**Direction: LOWERS the expected score of whichever language has the most `NONE` probes, on both Hidden
Semantic Cost and Capability Efficiency.** Doc 01's `na_policy` NA-6 forbids predicting which language that is,
and this document does not predict it. Stated plainly: Quidra is the youngest language in the set, and to
whatever extent it cannot express probes the mature languages can, this correction **lowers Quidra's expected
score** by removing its ability to be averaged over its own easier subset. That is the normal and correct
outcome. No compensating adjustment was made anywhere.

### S3 — [MAJOR] An `observed` record was able to discharge a universal negative

**Defect.** First-pass Finding 11 correctly made exclusions establishable by an `observed` record, so that
Hidden Semantic Cost would not become a measure of reference prose. But several §1.4 exclusions are **universal
in scope** — H8's "one documented outcome … independent of mode and target", H3's candidate equivalence, H5's
totality, H1's "cannot affect", H4's and H7's unobservability, H9's "unobservable to a conforming program". As
written, a single run under the single mode the frozen recipe pins could discharge them. That is not a
demonstration of a universal, and it favours precisely the languages with the thinnest references and the
fewest shipped modes. The frozen recipes make this concrete: they pin an unchecked optimized mode for some
languages and a checked mode for others, so "it did not differ here" is especially weak evidence.

**Change.** New **§1.4.0**, binding on H1–H9: an `observed` record establishes what happened in the runs it
reports and **does not on its own establish a universal negative**. The universal-scope exclusions are
enumerated exhaustively, and each is discharged only by (a) a citation in which the reference states the
universal itself, or (b) an `observed` record that exercises and enumerates the whole space quantified over —
every frozen mode, every candidate body, every reachable edge class. **A single run under a single mode never
discharges a universal-scope exclusion for any language.** Cross-referenced from §1.2 and from the H3, H5 and
H8 exclusion bullets. §0.1 gains a matching clause: a recipe's checking level is neither a credit nor a debit,
and a language with one execution mode is neither credited nor penalised for having one. New check **V21**,
§1.9 audit item 11, new §4.3 prohibition.

**Direction: LOWERS Quidra's expected score** on Hidden Semantic Cost, together with Zig's and TypeScript's. It
narrows the one first-pass correction whose direction favoured Quidra (Finding 11), keeping that correction's
valid core — thin references are not penalised — while removing the over-broad reading under which one run of
one engine could establish that nothing anywhere behaves differently.

### S4 — [MAJOR] §2.4 and §2.4.1 stated opposite rules about alternative spellings

**Defect.** §2.4 excludes "alternative ways to express the same probe that the frozen implementation does not
use", stated as a fairness guarantee. §2.4.1, added in the first pass at the auditor's direction, counts a
static-acceptance rule that a reader must know "including predicting that an alternative spelling of the
fragment would be rejected". A rule stated as neutral in one section and contradicted by a specific provision in
another is the audit's cross-cutting check; here it left each analyst free to choose the reading, and the
choice is worth many SCU for the languages with substantial static-acceptance rules.

**Change.** §2.4 gains an explicit reconciliation, identical for all ten: §2.4.1 counts **the acceptance rule
the frozen fragment itself must satisfy** — one rule, applied to the fragment in front of the reader. It never
licenses counting the semantics of the alternative construct, nor one rule per alternative that could have been
written; the alternative is evidence that the acceptance rule has content, not itself a counted obligation.
Where the rule cannot be stated as a property of the frozen fragment, it is out of scope.

**Direction: neutral for Quidra.** It bounds an inflation route that was open to every language with a rich
static-acceptance ledger and closes an ambiguity that could have been resolved either way per language.

### S5 — [MINOR] The claim of Quidra-independence was broader than the document's own content

**Defect.** §4.3 declared the document "authored … without reference to Quidra's syntax, operators, types, or
feature set", while §0.1 names Quidra's two execution engines and §2.4.1 names Quidra in a list. The claim as
worded was contradicted by the document's own text — the audit's cross-cutting check for exactly this.

**Change.** §4.3 now states the independence precisely and checkably: no rule is *derived from* Quidra's syntax
or feature set; the four places Quidra is named are enumerated, each shown to be a factual statement about the
frozen environment made in the same terms for all ten, and the §0.1 reference is noted to *add* an obligation
rather than an exemption.

**Direction: unchanged.** An accuracy fix to a declaration, with no effect on any count.

### S6 — [MINOR] Evidence commands were illustrated with a superseded recipe

**Defect.** The §0.6 example `command` hard-coded `rustc -O …`. Doc 01's counterpart obligations require that
recipe to change (to `rustc -O -C debug-assertions=on`, with `zig -OReleaseSafe` and `tsc --strict`), so the
illustration would have drifted from the frozen environment and invited an operator to run the stale command.

**Change.** The example is replaced by a pointer to the frozen recipe for the language and mode, verbatim from
`environment.json`, so the document cannot drift from the environment file again.

**Direction: unchanged** (a reproducibility fix binding all ten).

### Second-pass findings rejected

None. Every cross-cutting check in the brief was run against this document; the six defects it surfaced are
recorded above and fixed in the direction the audit specifies. Four of the audit's cross-cutting checks found no
residual defect here and are recorded as checked rather than silently omitted: (i) **no capability, probe, case
or metric omits something Quidra cannot do** — the nine checklist items map one-to-one onto the nine bullets of
spec §6.1.4.D and the probe set is consumed whole from doc 01, which added four probes in the interval, none of
them removed here; (ii) **no `N/A` escape** — §0.9 permits `N/A` in two enumerated cases only, and a missing
capability lands on `C` at full weight on the fixed 88-point denominator; (iii) **no double charge for one
property across two metrics beyond the one declared** — §1.3.1 and §1.3.2 keep one rule at one site to one
event and one item, §2.2 keeps one rule to one SCU, §3.6 declares the cross-metric overlap, and §2.5 declares
the `PARTIAL` asymmetry before measurement rather than hiding it; (iv) **the toolchain asymmetry** in which some
recipes pin an unchecked optimized mode while another language's only mode is checked is real, is not this
document's to fix, and is handled here by §0.1 and §1.4.0 and recorded below as a counterpart obligation that
doc 01 has already written on the environment owner.

### Counterpart obligations for sibling documents (both passes)

These are changes this document cannot make on its own side. Each is recorded so the sibling owner makes the
matching change; until then, the obligation is a known open item, not an assumption.

| Document | Change required |
|---|---|
| `environment/environment.json` (and doc 01 `toolchain_binding.recipes`) | Publish, per language, **which safety/checking modes the frozen recipe selects** (e.g. which recipes disable overflow or bounds checking in their optimized mode), so §0.1's mode-dependence assessment and the checked-vs-unchecked comparison across languages are auditable. Recipes themselves are not changed by this document. **Doc 01 has since written the specific version of this obligation** — `rustc -O -C debug-assertions=on`, `zig … -OReleaseSafe`, `tsc --strict …` — on the same owner; this document's §0.1 and §1.4.0 are written to be correct either way, and §0.6 now points at the frozen recipe rather than quoting one. |
| Owner of `01_capability_universe_and_probes.json` | Doc 01's `counterpart_obligations` entry addressed to this document ("confirm the same common-basis rule for D, and update metric E's denominator reference from 103 to 88 supported-capability points with the 2-points-per-probe rule") is **discharged** by second-pass corrections S1 and S2 and may be marked satisfied. If `probe_count` or `total_fixed_capability_points` changes again, this document's literals and `epsilon = 1/N` must be re-derived before measurement, per §0.3 and §4.1 step 1. |
| `environment/environment.json` | Any language given a second execution engine or build mode must have it listed in `frozen_toolchain_recipes`, since §0.1 binds "every frozen mode". |
| Owner of the capability matrix | Publish `semantic-compression/raw/capability_matrix.json` in the record format declared in §0.3, including `support_level`, `support_factor`, `capability_points`, `awarded_points`, `partial_criteria` and `cap_points_total`, before Parts 1–3 begin. Checks V10 and V11 read it directly. |
| `03_determinacy_and_locality.md` | Its §1.1 *focus span* and this document's §0.7 *local sites* must denote the same tokens wherever they coincide. §0.7 here rules that each document governs its own metrics and that a mismatch is a `CORRECTIONS.md` defect; doc 03's owner should record the reciprocal statement. **Checked in the second pass and consistent:** doc 03 has adopted the common basis as primary for `B` and `H` (its Rule 1.8.6), `N = 44`, `epsilon = 1/44` (its Rule 1.8.3), and the corpus probe-id form `F05.P1`, all matching §1.6.1 and §1.7 here. The earlier `SC-F05-P1` mismatch is resolved on doc 03's side and needs no further action. |
| `01_capability_universe_and_probes.json` | `downstream_obligations` should name this document's real consumers: the closed candidate set (§1.3.0) requires the rule registry to exist before Hidden Semantic Cost is scored, and §0.7 depends on `measured_fragment` being authored under R5 as a statement list with a determinate top-level statement order. |
| `CORRECTIONS.md` | This remediation pass should be summarised there as a pre-measurement defect-and-fix entry under spec §10.4, consistent with entries D-1…D-5. |
