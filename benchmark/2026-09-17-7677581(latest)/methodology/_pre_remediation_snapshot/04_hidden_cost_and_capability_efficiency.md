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
`environment/environment.json → frozen_toolchain_recipes`. No other flags, packages, or code generation may be
used to produce evidence under this methodology. Where a recipe has both a native and an interpreted form
(Quidra), semantic evidence is taken from the **native compiled** form (`quidra build` / `./BIN`); interpreter
behavior is recorded as a note only and never changes a count.

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

This document does **not** define the probe set or the support rubric; it consumes them. The frozen inputs are:

| Input | Canonical path (relative to the run directory) | Used for |
|---|---|---|
| Fixed capability universe, 20 families `F01`–`F20` | `semantic-compression/probes/capability_universe.json` | Part 2 denominator, §20 scoping |
| Fixed probe set, 40 probes `PR-01`–`PR-40` | `semantic-compression/probes/probes.json` | Parts 1–3 unit of work |
| Per-probe semantic-fact annotation (spec §6.1.3) | `semantic-compression/probes/semantic_facts.json` | Materiality bounds |
| Support / partial-support rubric and its result | `semantic-compression/raw/capability_matrix.json` | Applicability, Part 2 denominator |
| Frozen per-language probe implementations | `semantic-compression/probes/impl/<lang>/PR-NN.<ext>` | Evidence for Parts 1–3 |

Probe identifiers are `PR-01` … `PR-40`, two probes per capability family, family `F01`–`F20` in the order
given in spec §6.1.2. If the frozen probe file assigns a different probe→family mapping, **the frozen probe
file wins**; this methodology never re-partitions probes.

If a required input file is absent at measurement time, measurement does not begin. This methodology may not be
executed against an improvised probe set.

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
  "evidence_id": "EV-<LANG>-PR-07-003",
  "language": "rust",
  "probe": "PR-07",
  "artifact": "semantic-compression/probes/impl/rust/PR-07.rs",
  "line_span": [12, 12],
  "kind": "static" | "dynamic",
  "command": "rustc -O semantic-compression/probes/impl/rust/PR-07.rs -o /tmp/pr07 && /tmp/pr07",
  "observed_output": "<verbatim stdout/stderr, truncated to 2000 chars with an explicit truncation marker>",
  "note": "<one sentence stating what the output demonstrates>"
}
```

`kind: "static"` is used where the behavior is not dynamically observable (for example, "the implementation is
permitted to elide this copy"). A `static` record must carry a `citation_id` instead of a `command`, and the
written rule alone carries the claim.

### 0.7 The local site (shared definition for Parts 1–3)

Several rules below turn on what counts as "local". This definition is fixed once and used identically by
Hidden Semantic Cost, Capability Efficiency, and §20, and is deliberately consistent with spec §6.1.4.B
("before consulting external declarations or whole-program facts"):

> **Local site of a probe annotation** = the single statement or expression that the frozen probe annotation
> points at, together with every token lexically inside that same statement — including any type annotation,
> binding keyword, cast, operator, attribute, modifier, or marker written inside it.
>
> **Not local:** declarations of names used by the statement but written elsewhere (including elsewhere in the
> same file), imported definitions, type definitions, trait/interface/protocol implementations, overload sets,
> generic instantiations chosen elsewhere, build flags, compiler modes, target configuration, and any
> whole-program fact.

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
| The language supports **zero** probes under the frozen support rubric | Hidden Semantic Cost, Capability Efficiency | Both raw values are undefined. Mark `N/A`, reason `"no supported probes"`. Exclude from the applicable-weight denominator of `Q` and renormalize the remaining quality weights per spec §26. Capability Coverage `C` is 0, so the Semantic Compression Overall Score is 0 regardless. |
| A probe is **unsupported** by a language under the frozen support rubric | per-probe measurement | The probe is excluded from that language's Hidden Semantic Cost denominator and from its Capability Efficiency scope. It is **not** scored 0 and **not** scored well. It remains in the Capability Coverage denominator and lowers `C`. |

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

**A zero on any checklist item is a legitimate, reportable result.** If a language triggers zero items across
all 40 probes, its raw Hidden Semantic Cost is 0 and it is the best language on this metric. The normalization
in §1.7 is predeclared to handle an exact zero.

## 1.3 General trigger test (applies to every item H1–H9)

A candidate behavior B in probe `PR-NN` for language `L` triggers its checklist item **iff all five conditions
hold**:

1. **Occurrence.** Under the pinned reference, B occurs, or may occur, when the probe's frozen implementation
   is executed over the probe's declared input domain. ("May occur" includes implementation latitude that the
   reference grants, e.g. "the implementation is permitted to …".)
2. **Materiality.** B is semantically material per §0.8, and is not on the universal exclusion list.
3. **In annotated scope.** B affects at least one semantic fact that the frozen semantic-fact annotation
   (`semantic_facts.json`) lists for this probe. Behaviors outside the annotated fact set are out of scope for
   this metric and are not counted.
4. **No local signal.** There is no local signal for B in the local site (§0.7).
5. **LED fails.** The LED test of §1.2 does not apply — i.e. the local site plus the reference do not fix a
   single outcome.

If any one of the five fails, the behavior is not counted, and the reason is recorded.

### 1.3.1 Single-assignment rule (prevents double counting inside this metric)

Spec §20 forbids counting the same underlying rule twice inside one metric merely because it fits several
descriptive categories. Therefore:

> **Every distinct hidden event is classified into exactly ONE checklist item**, using the fixed precedence
> order **H1 → H2 → H3 → H4 → H5 → H6 → H7 → H8 → H9**, first match wins. **H9 is the residual category** and
> is used only for events that no earlier item claims.

Example of the rule working: a hidden implicit conversion that also changes the in-memory representation is a
single event; it is assigned to H1 and is **not** additionally counted under H7.

### 1.3.2 Distinct events

Two occurrences of a behavior are the **same event** iff they arise from the same rule at the same local site.
Two different local sites inside one probe are different events. This distinction only affects the secondary
occurrence-count evidence (§1.5); the scored unit is item-level, so multiplicity never inflates the score.

## 1.4 The nine checklist items (frozen)

Each item is defined by: **Definition**, **Counts when** (the operational test), **Exclusion** (what must not be
counted), and an **LED note** naming at least one concrete way a language legitimately scores 0.

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
reference, has cardinality ≥ 2, **and** the candidates are not semantically interchangeable for the probe's
declared inputs (they can differ in any material respect per §0.8).

**Exclusion — do not count:**
- forms for which the reference fixes exactly one meaning language-wide (built-in operators on built-in types
  in a language that forbids operator overloading, when the operand type set is fixed by the local site);
- calls whose target is fully determined by a locally written fully-qualified path, a locally written type
  ascription, or a local token the reference defines as forbidding override/overload;
- multiple candidates that the reference proves observationally equivalent for the probe's inputs;
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
- allocation named by a local token whose specified meaning is allocation (a `new`-style operator, a container
  literal or macro documented as allocating, an explicitly passed allocator, an explicit allocation call);
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

**Counts when.** All of §1.3 hold **and** the reference states that the form can raise, throw, panic, trap,
abort, or propagate an error out of the enclosing function, **and** the failure mode is one the probe's frozen
semantic-fact annotation lists under "possible failure", **and** the failure is reachable within the probe's
declared input domain.

**Exclusion — do not count:**
- failure marked by a local token (`try`, `?`, `!`, an explicit `throw`, an explicit `catch`/`match` on the
  failure at the same site, a locally written `throws`/`rethrows` effect on this expression);
- forms the reference proves total for the declared input domain (they cannot fail);
- host resource exhaustion (universal exclusion, §0.8);
- failures outside the probe's declared input domain.

**LED note.** A language whose every fallible call must carry a propagation marker scores 0 on H5. A language
with unchecked exceptions that may unwind from any call triggers H5 wherever the annotation lists a reachable
failure. Both are legitimate.

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
  reference does **not** give one and the same documented outcome for the edge classes the probe's
  semantic-fact annotation lists, and no local token selects the edge behavior.

**Exclusion — do not count:**
- forms where the reference fixes exactly one documented outcome across every type the form admits in this
  probe, independent of mode and target (for example: one uniform wraparound rule for all integer types, or one
  uniform trapping rule for all integer types, or arbitrary-precision integers with no overflow edge at all);
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
H(L, p) = | { i in {H1..H9} : at least one event in probe p is assigned to item i } |
```

`H(L, p)` is an integer in `[0, 9]`. **The unit is the count of distinct checklist items triggered, not the
count of occurrences.** A probe that triggers implicit conversion five times and nothing else scores 1.

**Secondary evidence, recorded but not scored.** The operator additionally records `occurrences(L, p, i)`, the
number of distinct events (§1.3.2) assigned to item `i`. These occurrence counts are published as raw evidence
and used in the audit narrative. They never enter the score, the normalization, or any ranking.

## 1.6 Aggregation over the 40 probes (frozen)

Let `Supp(L) ⊆ {PR-01..PR-40}` be the probes language `L` supports (support level `S` = full or `P` = partial)
under the frozen support rubric in `capability_matrix.json`.

```
HSC_raw(L) = ( Σ_{p ∈ Supp(L)} H(L, p) ) / |Supp(L)|
```

Rules:

1. **Unsupported probes are excluded from both numerator and denominator.** They are not zeros and they are not
   penalties here; they lower Capability Coverage `C` instead (spec §26, §6.1.5). Measuring hidden cost for a
   construct a language cannot express would be fabrication.
2. **Partially supported probes are included**, measured over the portion of the probe the language can
   express, with `"support": "partial"` recorded on every affected evidence record.
3. Raw values are retained at full precision. Reported to 2 decimal places (spec §25.3).
4. The per-probe matrix `H(L, p)` for all 10 languages × 40 probes is published in full, before normalization
   (spec §25: "Never assign a 0–100 score before preserving the underlying evidence").

## 1.7 Normalization (frozen, family C, spec §25.1)

Hidden Semantic Cost is a positive lower-is-better quantity → **family C**. An exact raw zero is legitimate
here (a language may conceal nothing on any probe), so the **predeclared shifted form is used**:

```
Score_i = 100 * (best_raw + epsilon) / (raw_i + epsilon)
```

- `best_raw` = the smallest valid raw value among languages with a defined raw value.
- **`epsilon = 0.025`, fixed before measurement**, derived from measurement resolution: the per-probe unit is an
  integer and the maximum denominator is 40 probes, so the smallest non-zero difference this metric can resolve
  is `1/40 = 0.025`.
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
| `semantic-compression/raw/hidden_cost_<lang>.json` | Per-probe records: probe id, support level, per-item triggered flags, assigned events with `citation_id` + `evidence_id`, LED records for every rejected candidate, `H(L,p)`, occurrence counts |
| `semantic-compression/raw/hidden_cost_matrix.json` | The 10 × 40 matrix of `H(L,p)`, plus `HSC_raw` per language |
| `semantic-compression/scores/hidden_cost_scores.json` | `HSC_raw`, `epsilon`, `best_raw`, ratio, normalized score per language |

Per-probe record schema:

```json
{
  "probe": "PR-12",
  "language": "go",
  "support": "full",
  "items": {
    "H1": {"triggered": true, "events": [
      {"event_id": "E-1", "local_site_line": 9, "behavior": "<one sentence>",
       "citation_id": "CIT-GO-0007", "evidence_id": "EV-GO-PR-12-001"}
    ]},
    "H2": {"triggered": false, "led": [
      {"behavior": "<one sentence>", "reason": "local signal present: <token>",
       "citation_id": "CIT-GO-0011"}
    ]}
  },
  "H_probe": 1,
  "occurrences": {"H1": 2},
  "disputed": []
}
```

Every item must appear in `items`, either with `triggered: true` and ≥1 event, or with `triggered: false`. When
`triggered: false` because a candidate behavior was rejected by the LED gate or by a §1.4 exclusion, the
rejection must be recorded in `led[]` with its reason. **Silent omission of a rejected candidate is an audit
failure.**

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

**G3 — Co-extension Test (merging bound).** If two candidate rules are jointly satisfied or jointly violated in
**every** supported probe of that language, they merge into one ID. Conversely, a rule may not be merged with
another merely because both appear in the same probe or the same reference section.

**G4 — Calibration pass (mandatory, before the full count).** The operator counts probe `PR-01` for all ten
languages first and publishes `semantic-compression/raw/granularity_calibration.json` containing, per language,
the rule list for that one probe. If `max(SCU_PR-01) / min(SCU_PR-01)` across the ten languages exceeds 5, the
operator must write an explicit justification stating that the gap reflects the languages and not the
granularity, citing the specific rules that produce the gap. The calibration output may lead to **only one**
kind of change: a clarification of granularity applied **identically and retroactively to all ten languages**,
recorded with a timestamp, before any further counting. It may never be used to adjust a single language.

## 2.4 Scope: which rules are in and which are out

**In scope** — a rule is counted iff it is required to do at least one of these for at least one **supported**
probe, using that probe's frozen implementation:

- write the construct correctly (expression);
- predict the construct's meaning over the probe's declared input domain (correct interpretation), including
  its effects on any semantic fact in the probe's frozen fact annotation.

**Out of scope — never counted:**

- rules exercised only by probes the language does not support (those already cost the language via `C`);
- rules of a standard-library API beyond the documented contract actually used by the probe (a library
  function's documented contract counts as **one** rule unless the probe depends on more of its specified
  behavior);
- tooling, build-system, packaging, formatting, or style rules that do not change program meaning;
- lexical trivia (§0.8);
- rules about diagnostics or error-message wording;
- alternative ways to express the same probe that the frozen implementation does not use. **Only the frozen
  implementation is counted**, so a language is never charged for having many alternatives it did not need.

The last bullet is a deliberate fairness guarantee: this metric charges for the complexity **used**, not for the
size of the language. Language size is measured elsewhere (Capability Coverage `C` rewards breadth).

## 2.5 Counting algorithm (frozen, executed in this order)

```
INPUT:  probes.json, semantic_facts.json, capability_matrix.json,
        impl/<lang>/PR-NN.<ext> (frozen implementations), pinned references (§0.4)
OUTPUT: semantic_rules_<lang>.json, capability_efficiency_<lang>.json

STEP 0.  Verify every frozen implementation of a supported probe builds and runs under the frozen
         recipe in environment.json. A probe whose implementation does not build is a support-rubric
         failure, not a counting problem: stop and report it. Do not silently reclassify.

STEP 1.  Run the G4 calibration pass on PR-01 for all ten languages. Publish it. Resolve granularity
         once, identically for all, before continuing.

STEP 2.  For each supported probe p, walk the frozen implementation construct by construct, in source
         order. For each construct, and for each semantic fact the frozen annotation lists for p, write
         down every normative statement a reader must know to predict that fact.

STEP 3.  For each written-down statement, apply G1, G2, G3. Then look it up in the registry using the
         §2.2 identity test.
           - already present  -> append p to its "probes" list; do not create a new ID
           - not present      -> create a new ID with a citation_id (§0.5) and >=1 evidence_id (§0.6)

STEP 4.  Assign each new rule its single ledger by the precedence SE > CR > IR > SF.
         Assign its §20 category (§3.4) and its overlapping boolean tags
         (is_exception, is_context_sensitive, is_implicit).

STEP 5.  Apply the scope exclusions of §2.4. Remove any rule whose "probes" list is empty after the
         supported-probe filter.

STEP 6.  SCU(L) = number of distinct rule IDs in the registry.
         Verify: SCU(L) == |SF| + |CR| + |IR| + |SE|  (partition check, must hold exactly)
         Verify: every rule ID unique; every rule has >=1 citation_id or is tagged source "observed";
                 every rule has >=1 probe in "probes".

STEP 7.  CapPoints(L) = the number of SUPPORTED capability points for L, taken verbatim from
         capability_matrix.json -- the same numerator used for C = 100 * supported / total.
         This methodology does not redefine support; it consumes it.

STEP 8.  CapEff_raw(L) = SCU(L) / CapPoints(L)
```

**Denominator note.** `CapPoints(L)` is read from the frozen capability matrix, including whatever partial
credit the frozen support rubric assigns. Using the identical numerator that feeds `C` guarantees the two
metrics cannot disagree about what "supported" means.

## 2.6 Raw value, normalization, and `N/A`

```
CapEff_raw(L) = SCU(L) / CapPoints(L)          [semantic complexity units per supported capability point]
```

Lower is better → **family C**, unshifted form (spec §25.1):

```
Score_i = 100 * best_positive_raw / raw_i
```

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
  "tags": {"is_exception": false, "is_context_sensitive": true, "is_implicit": true},
  "citation_id": "CIT-KOTLIN-0031",
  "source": "documented",
  "evidence_ids": ["EV-KOTLIN-PR-09-002"],
  "probes": ["PR-09", "PR-10", "PR-23"],
  "granularity_notes": "G2 satisfied against KOTLIN.R.0030: <one sentence>"
}
```

Prohibited as grounds for any count, in any form: elegance, readability impressions, personal preference,
community reputation, popularity, verbosity, aesthetic judgement, "feels simpler", "feels cleaner". Any rule
whose `statement` cannot be restated as a checkable normative claim is moved to `disputed[]` and excluded.

## 2.8 Output files (frozen)

| File | Contents |
|---|---|
| `semantic-compression/raw/semantic_rules_<lang>.json` | The full rule registry (also the §20 raw evidence — see Part 3) |
| `semantic-compression/raw/granularity_calibration.json` | The G4 calibration pass and any identical-for-all clarification |
| `semantic-compression/raw/capability_efficiency_<lang>.json` | `SCU`, ledger breakdown, `CapPoints`, `CapEff_raw`, per-probe rule attribution |
| `semantic-compression/scores/capability_efficiency_scores.json` | raw, ratio to best, normalized score per language |

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

1. it constrains or determines an observable semantic fact (§0.8 materiality);
2. it is stated as a single independently-stated normative statement in the pinned reference (§0.4), or is
   tagged `source: "observed"` per §0.4;
3. it passes the granularity tests **G1, G2, G3** of §2.3;
4. it is exercised by at least one supported probe (§2.4 scope).

**The registry is shared.** The set of semantic rules for language `L` is *exactly* the rule registry built in
Part 2 (`semantic_rules_<lang>.json`). Therefore:

```
Total Semantic Rules (L)  =  |registry(L)|  =  SCU(L)
```

This identity is deliberate. It is what makes §20's raw counts feed Capability Efficiency **without any
possibility of double counting**: the same set is counted once and read two ways.

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
| **Hidden Semantic Cost (§6.1.4.D, Part 1)** | Rules tagged `is_implicit` and `is_context_sensitive` are the candidate source for H1–H9 triggers, and each trigger must cite the rule's `citation_id` | Within Hidden Semantic Cost, each hidden **event** is assigned to exactly one checklist item (§1.3.1), and the scored unit is *items triggered per probe*, so multiple events under one rule cannot inflate the score (§1.5). |
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
  "disputed_count": 0,
  "na": null
}
```

---

# 4. Execution order, validation, and freeze declaration

## 4.1 Mandatory execution order

1. Verify the frozen inputs of §0.3 exist and are themselves frozen.
2. Pin and snapshot the reference documents of §0.4; record retrieval dates.
3. Run the **G4 granularity calibration** on `PR-01` across all ten languages (§2.3). Publish it. Resolve
   granularity once, identically for all ten, before any further counting.
4. Build the rule registry per language (Part 2, STEP 2–6). This simultaneously produces the §20 evidence.
5. Score Hidden Semantic Cost per probe per language (Part 1), citing registry rule IDs.
6. Compute `SCU`, `CapPoints`, and `CapEff_raw` (Part 2, STEP 7–8).
7. Compute `T`, `E`, `X`, `I`, `RED` (Part 3).
8. Publish all raw values and matrices **before** normalizing (spec §25).
9. Normalize: Hidden Semantic Cost by family C shifted with `epsilon = 0.025`; Capability Efficiency by
   family C unshifted. Clip to `[0, 100]`; report 2 decimals; rank on unrounded values.
10. Run the audits of §1.9 and §4.2. Publish the audit files.

Languages are measured in the fixed column order of §0.1. Measuring Quidra first or last is immaterial; the
order is fixed so it cannot be chosen to suit an outcome.

## 4.2 Automated validation checks (all must pass before publication)

| Check | Condition |
|---|---|
| V1 | Every `H(L,p)` is an integer in `[0, 9]`. |
| V2 | Every triggered checklist item has ≥1 event with a `citation_id` or `evidence_id`. |
| V3 | No event id appears under two checklist items (single-assignment, §1.3.1). |
| V4 | Every `items.Hx.triggered == false` that rejected a candidate has a recorded `led[]` entry with a reason. |
| V5 | `SCU(L) == len(set(rule_ids))` and `SCU(L) == card(SF)+card(CR)+card(IR)+card(SE)`. |
| V6 | Every rule has ≥1 entry in `probes[]`, and every listed probe is supported by that language. |
| V7 | Every rule has a `citation_id`, or `source == "observed"` with a reproduction command and output. |
| V8 | Every `is_exception: true` rule has a `base_rule_id` present in the registry and different from itself. |
| V9 | `E(L) ≤ T(L)`, `X(L) ≤ T(L)`, `I(L) ≤ T(L)`. |
| V10 | `CapPoints(L)` equals the supported-point count in `capability_matrix.json` exactly. |
| V11 | Unsupported probes appear in no Hidden Semantic Cost denominator and in no rule's `probes[]`. |
| V12 | Every `N/A` has a reason string drawn from the two permitted cases in §0.9. |
| V13 | Probe count is exactly 40 and probe ids are exactly `PR-01`…`PR-40`; no probe was added, removed, or reworded during measurement (hash of `probes.json` matches the pre-measurement hash). |
| V14 | Both normalized metrics lie in `[0, 100]` with 100 = best (spec §25.4). |
| V15 | For each family-C metric whose applicable raw spread ≥ 100×, the raw values, the ratios, and the fixed compression note are present in the published output. |

Any failed check blocks publication of the metric. A failed check is fixed by correcting the *measurement*, never
by relaxing this document.

## 4.3 Freeze declaration

This methodology is frozen as of benchmark run `2026-09-17-7677581`. It was authored without reference to any
measured result and without reference to Quidra's syntax, operators, types, or feature set.

Permanently prohibited under this document (spec §25.4, §32):

- changing the checklist, the LED gate, the counting units, the granularity tests, the exception test, the
  weights, `epsilon`, or the normalization families after any language has been measured;
- adding, removing, rewording, or re-annotating a probe after measurement begins;
- relabelling an unsupported capability as `N/A` to avoid a coverage penalty;
- reporting a count that has no citation and no reproducible evidence;
- omitting a rejected candidate, a disputed rule, a failed trial, or an inconvenient raw value;
- introducing any rule, threshold, or transformation whose effect would be to improve Quidra's position.

A zero on a checklist item, a low rule count, a high rule count, or a last-place normalized score is a
**result**, for any of the ten languages including Quidra, and is published as measured.
