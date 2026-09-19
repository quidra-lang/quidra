# 03 — FROZEN Counting Rules: Semantic Determinacy (§6.1.4.B) and Semantic Locality (§6.1.4.C)

**Status: FROZEN.** This document is fixed before any language is scored. It must not be edited in
response to observed results. Any change requires a new benchmark run, or — per spec §24 — dual
publication of results under both the old and the new rule.

**Run:** `2026-09-17-7677581`
**Authority:** `prompt.md` §6.1.2, §6.1.3, §6.1.4.B, §6.1.4.C, §6.1.5, §6.1.7, §25.1, §26, §32.
**Applies to:** all 10 fixed languages, in the fixed column order
**Quidra, Python, C++, Rust, Go, Java, TypeScript, Kotlin, Swift, Zig.**

---

## 0. Scope, fairness posture, and reading order

### 0.1 What this document fixes

This document fixes, for every one of the 40 frozen probes × 10 languages:

* **Part 1** — how to obtain the integer `B_i` (local semantic branching count) for probe `i`;
* **Part 2** — how to obtain the integer `H_i` (required external semantic lookups / declaration-graph
  hops) for probe `i`;
* how each is aggregated, normalized, audited, and marked `N/A`.

It does **not** define the probe corpus, the semantic-fact inventory, the token-counting rule, the
hidden-behaviour checklist, or the support rubric. Those are frozen in the sibling methodology
documents. This document **binds to** the frozen probe corpus
`methodology/01_capability_universe_and_probes.json`: every probe id, every measured fragment, every
canonical task and every fact annotation used below is taken from that file and is never restated from
memory. Where this document uses a short role name for a probe, the role name is a label for a named
probe id, not a description that some other program might also satisfy:

| Role | Frozen probe | What the measured fragment contains |
|---|---|---|
| **ARG-PLAIN** | `F05.P1` | (1) the local binding of `x` to a growable integer sequence containing 1, 2, 3; (2) the plainest ordinary call of `f` on `x`, with no sigil, address-of, borrow marker, copy, clone, wrapper, annotation or modifier at the argument position; (3) the post-call read of element 0. `f`'s declaration is **outside** the fragment. |
| **ARG-EXPLICIT** | `F05.P2` | `put`'s **full declaration and body** (the body is exactly the one assignment that sets the caller's variable to 12), the caller's mutable signed-32-bit local `cell` initialized to 3, the call of `put` on `cell` in whatever explicit reference / borrow / address-of / inout / pointer / dereference form the language requires or conventionally uses, and the post-call read (which must be 12). `put` is **inside** the fragment for this probe. |
| **ARG-FUNCTION** | `F05.P3` | the third probe of family 5, in which the **argument is a function**. No specialised table of §1.5 applies to it; its in-scope axes come from the §1.4.1 mapping applied to its own frozen `semantic_facts_expected` (Rule 1.4.2). It is listed here so it is not overlooked because the two older roles are the ones §6.1.4.B names. |
| **ARITH-EXPR** | `F07.P1` | one binding statement `r = a * b + c` over three given, already-initialized signed-32-bit locals, using the language's ordinary operators and precedence. |
| **ARITH-DIV** | `F07.P2` | three binding statements over two given signed-32-bit locals (where `a` may be negative): integer quotient `q`, remainder `m`, and 64-bit floating-point division `d`. |
| **OVERFLOW-SITE** | `F08.P1` | `m` declared **inside the fragment** and initialized to the signed-32-bit maximum (using the language's standard named maximum where one exists); `o` bound to `m + 1` computed with **the operation whose overflow behaviour is determined by the operation itself**, selected in the corpus's own fixed rung order — (i) a standard checked operation yielding an optional/error value with the fallback integer 0 substituted for the exceptional case, (ii) a standard wrapping operation, (iii) a standard saturating operation, (iv) the plain operator — with the rung used and the reason each higher rung was unavailable recorded; and the production of `o`. |
| **DIVZERO-SITE** | `F08.P2` | the division-or-guard construct for `a / b` where `b` may be 0 at run time, selected in the corpus's fixed rung order — (i) a standard checked-division operation yielding an optional/error value, (ii) an explicit local guard on `b == 0`, (iii) the plain operator — with the fallback integer 0 substituted for the exceptional case and the rung used recorded, and the production of `q`. `a` and `b`'s declarations are given context, outside the fragment. |

**Rule 0.1.1 (no invented probes).** No rule, table, precedent or example in this document may be
stated over a program that is not the frozen fragment of a named probe id, and no example may state a
fragment, a fact set or an operand type that the corpus does not state. Where the two disagree, the
corpus governs and this document is defective (Rule 1.5.0). The pre-remediation text of this section
introduced a "focus span" concept the corpus does not contain and an `ARITH` probe (`a + b` on two
operands) that the corpus does not contain; both are withdrawn and replaced above. The table above is
an **index of labels**, not a restatement: the corpus's `canonical_task` and `measured_fragment` are
read from the file for every enumeration, and a discrepancy between the table and the file is resolved
in the file's favour without argument. **The table covers 6 of the 44 probes.** The other 38 are not
exempt from anything: they are enumerated under the general rules of §1.4 and §2, with their in-scope
axes taken from the §1.4.1 mapping applied to their own frozen `semantic_facts_expected`.

The frozen probe corpus contains **44 probes** (`probe_count` = 44) across the 20 capability families
of §6.1.2. **2 probes per family is the guaranteed minimum, not a uniform count**: families 4, 5, 14
and 15 carry 3 probes each after the corpus's own remediation, which added the counterpart probes those
families were missing. Formulas below are written for `N = 44`; `N` is the frozen corpus size and is
read from `probe_count`, never from a language's applicable-probe count (Rule 1.8.3). If the corpus
document ever states a different `probe_count`, every formula uses that `N` and the predeclared
constants of §1.8 and §2.9 are recomputed from it by the stated formula (`epsilon = 1/N`) before any
language is scored; no per-language quantity ever enters that constant.

**The pre-remediation text of this section stated `N = 40`, "2 probes for each of the 20 families", and
a capability denominator of 103.** The corpus now states 44 probes, a non-uniform per-family count, and
a fixed denominator of **88** capability points at a uniform **2 points per probe**. Every figure in
this document is corrected to the corpus's values; where a stale figure survives anywhere, the corpus
governs and this document is defective (Rule 0.1.1).

### 0.2 Fairness posture (binding)

Restated from §32 and §6.1.7 because these rules are where bias would enter most easily:

1. No axis, outcome class, table or ruling below is derived from Quidra's syntax, operators, types or
   feature set. Every one was written from the union of the ten languages' own authoritative
   specifications and from the capability families of §6.1.2. Two rules nevertheless bear on Quidra's
   expected counts in a way a reader is entitled to see stated, rather than left implicit:
   **Rule 1.2.2** (unobservability) disposes of the one documented source of openness at a bare Quidra
   call site, and **Rule 1.9.5** (absence flag) exists because a language can lower `B` by lacking a
   construct rather than by constraining one. Both are written language-blind, both apply to all ten
   languages, and both now carry evidence requirements that bite in both directions.
2. A language may score well on something Quidra does badly, and badly on something Quidra does well.
   Nothing here is conditioned on which language is being scored. The procedure is applied by the
   same script and the same two analysts, blind to the running totals (§3.2).
3. Capabilities Quidra lacks remain in the universe. A probe a language cannot express is recorded as
   `support_level: NONE` with a justification — never as `N/A` (doc 01 `na_policy` NA-6) — is excluded
   from these two metrics' means, and is charged in full against Capability Coverage `C` with its full
   `capability_points` remaining in `C`'s fixed denominator of **88** (§6.1.5, §26, NA-1/NA-2). It is
   never silently deleted. Doc 01's NA-6 states no list of probes on which `NONE` is "expected" for any
   language, and neither does this document: which probes yield `NONE` for which language is an output
   of the measurement, never an input to it.
4. **No glyph reasoning.** See §1.6. Determinacy is scored from the complete surface form of the
   measured fragment under the language's own specification, never from resemblance to another
   language's notation.
5. Where a count cannot be made honestly, it is recorded with a machine-readable reason code
   (§1.9, §2.10). Nothing is estimated, interpolated, or filled in by analogy.
6. **Absence is not determinacy.** Where a closed outcome class is unrealizable for a language because
   the language has no such construct *at all* — rather than because the construct would have to be
   spelled inside the fragment — that is flagged and published (Rule 1.9.5), so a determinacy result
   earned by design can be told apart from one earned by omission. This applies to every language, in
   both directions, and costs nothing to compute.
7. **Evidence is required in both directions.** A counted branch needs a witness; an *excluded* class
   needs a specification citation **and** a refutation witness (Rule 1.3.1). A language whose reference
   documentation is authored by the party running the benchmark therefore cannot close a branch by
   documentary assertion while the other nine languages' openness is demonstrated by construction.
8. **No results have been observed.** At the time these corrections were applied, no `B_i`, no `H_i`,
   no aggregate and no normalized score had been computed for any language, including Quidra. Every
   rule below is pre-registration, not post-result formula selection (§24, §32).
9. **Identical material.** Both metrics are reported primarily on the **common basis** — the probes for
   which all ten languages have a fragment — so no language's aggregate is computed over an easier
   probe set than another's (§1.8.6, §2.9.4, doc 01 `support_rubric.interaction_with_quality_metrics`).
   `PARTIAL` fragments are in the common basis, identically for all ten languages, with no exemption.
   Every published figure states its basis and its basis size.
10. **The build mode is a controlled variable, not a language property.** Witnesses are built and run
   under the frozen recipe, which pins every language that has a choice to its optimized **checked**
   mode (Rule 1.3.3). No language's always-on checking is compared against another language's
   checks-disabled release build, in either direction, and a language that has no checked mode records
   that as a language fact scored by `B` and by Hidden Semantic Cost rather than as a recipe artefact.

### 0.3 Relationship between the two metrics

Determinacy and Locality are **not** two views of one quantity. `B_i` counts what the measured fragment
can mean; `H_i` counts how far outside the fragment a reader must travel to learn which meaning holds.
A language can score well on one and badly on the other: a language that omits a fact from its call
sites but records it in a declaration pays in `H` and not in `B`, while a language that admits many
meanings at the site and settles none of them in a declaration pays in both. Reporting both is
therefore informative rather than redundant, and neither metric may be back-derived from the other.

**Whether that dissociation actually occurs, for which languages, and in which direction, is an outcome
of the measurement and is not asserted here.** No number in this document is a prediction. The
pre-remediation text of this section asserted specific per-language values (Python `B = 3` "lowest of
the nine", C++ `B = 11`) that had never been produced by a witnessed enumeration against a frozen
fragment; those values are withdrawn (§4) and no replacement is stated in advance.

---

## 1. PART 1 — SEMANTIC DETERMINACY (`B_i`)

> §6.1.4.B: "For each fixed probe, count the number `B_i` of materially different semantic
> interpretations that remain compatible with the local surface form before consulting external
> declarations or whole-program facts."

### 1.1 The counting unit: the probe's measured fragment

**Rule 1.1.1 (definition, bound to the corpus).** The **focus span** for probe `i` in language `L` is
**exactly that probe's `measured_fragment`**, for that language, as defined in
`01_capability_universe_and_probes.json` under authoring rule `R5_measured_fragment_boundary`:
*"Only the text inside that boundary is counted for Semantic Density (tokens and facts), Determinacy,
Locality, and Hidden Cost."* Where the probe record carries a `SCORING NOTE` naming a sub-expression as
the site whose interpretations are to be counted, that sub-expression is the **counting site**: `B_i`
enumerates the interpretations that remain compatible with the counting site, while every fact
established elsewhere **inside** the measured fragment is known at zero cost and closes branches. Where
no `SCORING NOTE` names a narrower site, the counting site is the whole measured fragment.

Two consequences, both mandatory, both of which the pre-remediation text got wrong:

* At **ARG-PLAIN (`F05.P1`)** the measured fragment is (1) the binding of `x` to a growable integer
  sequence containing 1, 2, 3, (2) the bare call, (3) the post-call read of element 0, and the
  `SCORING NOTE` makes (2) the counting site. The operand's type is therefore **local and known** in
  every language — `std::vector<int>`, `Vec<i32>`, `list`, `ArrayList<Integer>`, `[]int`,
  `MutableList<Int>`, `number[]`, `[Int]`, a growable integer sequence of the language's own — and
  **any outcome class that requires a different operand type is not realizable and must not be
  counted.** `f`'s declaration, signature, documentation and body remain outside the fragment.
* At **ARG-EXPLICIT (`F05.P2`)** the corpus places `put`'s **full declaration and body inside** the
  measured fragment ("for this probe `put`'s declaration and body ARE inside the boundary, because the
  explicit form may live there"). That enumeration is therefore performed with the callee visible, and
  any outcome class that depends on *not* knowing the callee is not realizable there. ARG-PLAIN and
  ARG-EXPLICIT ask materially different questions and may not share an enumeration or a precedent.

**Rule 1.1.2 (everything outside the measured fragment is external).** For `B_i`, everything outside
the probe's measured fragment is **external** and must not be consulted, even when it appears in the
same file or on the line immediately above: declarations the probe designates as "given context",
callee signatures and bodies not inside the fragment, type declarations, trait/interface/protocol
implementations, overload sets, imports, attributes on external declarations, build flags, and language
edition/version selectors. Conversely, everything the corpus places **inside** the fragment is local
and free. This is the literal reading of "before consulting external declarations or whole-program
facts" *as the frozen corpus draws that boundary*. The boundary is fixed per probe and is identical for
all ten languages, which is what makes the count comparable: a language that puts a fact inside the
fragment keeps its branch closed; a language that puts the same fact in an external declaration does
not.

**Rule 1.1.2.a (no unilateral redefinition of the unit).** If a narrower or wider counting unit than
the corpus's `measured_fragment` is ever wanted for `B`, it is obtained by amending
`01_capability_universe_and_probes.json` and dual-publishing under §25.4 — never by redefining the unit
inside this document. The pre-remediation §1.1 defined a "focus span" that the corpus does not contain
and declared the operand's own declaration external although the corpus places it inside the fragment;
that definition is withdrawn. Every enumeration in this document that depended on it is withdrawn with
it (§1.6.2, §1.7, §2.8, §4).

**Rule 1.1.3 (fragment equivalence across languages).** The measured fragment for a probe denotes the
*same semantic event* in all 10 languages — the same call, the same operator application, the same
index — rendered per the corpus's authoring rules R1–R10. Where more than one form satisfies R1–R3,
**R10 selects the measured form mechanically** (fewest tokens under the frozen tokenizer of methodology
02, ties broken by the form that appears first in the language's official documentation, with every
alternative considered recorded with its token count and citation) — except where a `canonical_task`
states its own preference order (`F08.P1`, `F08.P2`, `F10.P1`), which overrides R10. An analyst's
judgement of what is "the normal choice" never selects the fragment, because the fragment selects every
value this document computes. Analysts may not widen one
language's fragment to capture a declaration, or narrow another's to hide a modifier. Where a
language's idiomatic rendering of the event unavoidably carries a modifier inside the fragment (e.g. a
required `await`, a required `try`, a required `mut`), that modifier is part of the fragment and its
determinacy effect is counted. Where a language's idiomatic rendering unavoidably omits such a
modifier, its absence is likewise part of the fragment. Where the canonical task directs a language to
use an operation that names its own discipline (`OVERFLOW-SITE`, `DIVZERO-SITE`), that operation is the
fragment for that language, and the classes the plain operator would have admitted are not realizable.

**Rule 1.1.4 (language spec is free, but an exclusion still needs evidence).** Facts fixed by the
language's own specification for the tokens inside the fragment are *known*, at zero cost, and close
branches. Example: the Go specification states that all arguments are passed by value; that fact is
available to the analyst at ARG-PLAIN without any lookup, and it eliminates outcome classes for Go.
This is the one and only body of external knowledge admitted in Part 1, and it is admitted identically
for all ten languages, including for Quidra (from the Quidra language reference at the frozen HEAD
SHA). **A specification statement is a citation, not evidence**: a class excluded on the strength of one
must additionally carry the refutation witness of Rule 1.3.1(e), so that a language documented by the
party running the benchmark closes branches on the same terms as a language documented by an
independent standards body.

### 1.2 "Materially different" — the materiality test

**Rule 1.2.1 (test M).** Two candidate interpretations `u` and `v` of the same measured fragment are
**materially different** iff there exists a well-formed **observer program** — identical inside the
measured fragment, differing only outside it — under which `u` and `v` differ in at least one of the
nine dimensions named in §6.1.4.B (below, "the span" means the probe's measured fragment, or its
counting site where the probe names one — Rule 1.1.1):

| # | Dimension | Counts as material when it differs |
|---|-----------|------------------------------------|
| 1 | mutation | whether, or which, program state visible after the span can change |
| 2 | aliasing | whether two names/handles reach the same storage |
| 3 | failure | whether the span can trap, panic, throw, abort, or yield an error value |
| 4 | conversion | whether a value is converted, and whether the conversion can change the value |
| 5 | dispatch | which *kind* of target the span selects (see Rule 1.4.7) |
| 6 | resource behaviour | whether allocation, copy, clone, move, refcount change, destruction, or cleanup occurs at the span |
| 7 | control flow | whether the span falls through, returns early, propagates, unwinds, suspends, defers, or diverges |
| 8 | representation | the width, signedness, precision, boxing, layout, or ownership representation of the value produced or consumed |
| 9 | observable result | the value or output the program produces |

**Rule 1.2.2 (performance is not material).** A difference that is observable only as elapsed time,
instruction count, or memory high-water mark is **not** material. A difference in resource behaviour
(dimension 6) is material only where the language gives a program a defined way to observe the event.
The following are **examples, not a closed list**: a user-defined constructor/destructor/`deinit`/
`Drop`, an allocator or `new`/`delete` hook, a reference-count query (`isKnownUniquelyReferenced`,
`Rc::strong_count`, `sys.getrefcount`), a finalizer with defined timing, **or any other mechanism the
language's own authoritative specification defines for observing allocation, copy, move, refcount
change, destruction or cleanup**. The list is open in both directions and is drawn per language from
that language's specification: a language whose observation channel is not named above is not thereby
deprived of it, and a language named above holds no privilege from being named. Where the language
makes the event unobservable to conforming programs
(e.g. an elided copy the specification permits the implementation to remove, GC-internal copying),
the difference is not material and does not create a branch. This rule is what stops copy-elision
lawyering from inflating `B` for compiled languages.

**Rule 1.2.2.a (an unobservability exclusion must be refuted, not asserted).** An exclusion on the
ground of unobservability is an exclusion like any other and requires the refutation witness of
Rule 1.3.1(e): an attempted observer program, plus the recorded result showing that the event is not
detectable by any conforming program under the frozen toolchain at the recipe entries Rule 1.3.3 admits. **A vendor or
specification statement that an optimization is unobservable is a citation, not evidence.** This
matters symmetrically and is disclosed rather than left implicit: Rule 1.2.2 removes classes for C++
(elided copies), Go and Java (GC-internal copying), Swift (retain/release traffic the runtime may
elide) — and it also removes the one documented source of openness at a bare Quidra call site, since
the Quidra architecture reference states that the IR "may internally borrow an ordinary by-value
aggregate parameter when a conservative whole-body analysis proves that the callee neither mutates that
parameter nor exposes it through writable storage", describing it as "an unobservable optimization of
ordinary value passing". Under Rule 1.2.2 that class is immaterial; under Rule 1.2.2.a the exclusion is
only accepted once an observer has been attempted and has failed, exactly as for the other nine.

**Rule 1.2.2.b (unobservable is not free).** An event excluded here as unobservable is recorded with
the flag `UNOBSERVABLE-RESOURCE-EVENT`, naming the dimension-6 event and the language, and is reported
to methodology 04, where §6.1.4.D charges implicit allocation, copy, move, destruction or cleanup that
is not signalled at the use site. Determinacy does not charge it; Hidden Semantic Cost does. Neither
metric may charge it twice, and neither may drop it.

**Rule 1.2.3 (existence of the observer must be demonstrated).** Materiality is never asserted. It is
demonstrated by the witness of Rule 1.3.1.

### 1.3 The witness rule (the reproducibility anchor)

**Rule 1.3.1 (witness).** An interpretation is counted **only if** the analyst records a **witness**:
a concrete completion of the probe program — declarations, signatures, types, imports and build
invocation outside the measured fragment, with the measured fragment byte-identical to the language's
frozen fragment for that probe — that

* (a) builds and runs under the **frozen toolchain recipe** for that language
  (`environment/environment.json` → `semantic_compression_recipes`), on the frozen host, in one of that
  language's admissible recipe entries (Rule 1.3.3), with the entry recorded;
* (b) does not rely on undefined behaviour, except where the *interpretation itself* is
  "this span has undefined behaviour" (outcome class `R4` in §1.5.2), which is a legitimate,
  specification-documented outcome and must be evidenced by the language specification clause, not by
  a run;
* (c) uses only documented, non-deprecated, non-removed features of the frozen toolchain version, and
  no benchmark-specific external package;
* (d) is accompanied by an **observer**: the printed/instrumented output that distinguishes this
  interpretation from every interpretation already counted for that probe and language.

No witness ⇒ no branch. This single rule is what makes two independent analysts converge: they do not
argue about whether something is "possible in principle", they exhibit a program or they drop the
branch.

**Rule 1.3.1.e (exclusions carry evidence too — the negative control).** The pre-remediation rule
required a built, run, observed witness for every *counted* branch and nothing at all for an *excluded*
class, which made the procedure a positive control with no negative control: openness had to be
demonstrated by construction, closure could be claimed in prose. Every entry in `excluded_classes`
(§1.10) therefore now carries **both**:

* **(a)** a `spec_citation` to a numbered clause, section or rule of the language's authoritative
  specification or reference, published before the run and identified by version or SHA; **and**
* **(b)** a `refutation_witness_path`: a program that *attempts* to realize the excluded class under
  the frozen toolchain at the recipe entries Rule 1.3.3 admits, stored with its build command and its recorded
  outcome — the compiler's rejection, the run-time rejection, or the observer output showing the class
  does not occur.

An exclusion carrying neither, or only one, is **invalid** and the raw file fails validation. This
applies identically to all ten languages. It is the counterpart of §10.4's pre-flight principle that
every validator must be able to both pass and fail, and it is what turns Rule 1.1.4's zero-cost
specification knowledge into a checked claim rather than an assertion.

**Rule 1.3.2 (witnesses are preserved).** Every witness and every refutation witness is stored under
`semantic-compression/probes/witnesses/<probe_id>/<language>/<class_id>/` — with `<probe_id>` in the
corpus's own form (`F05.P1`, not `SC-F05-P1`) — together with its build command, the recipe entry it
was built or run under, and its observer output, and is referenced by path from the raw determinacy
record (§1.9, §1.10).

**Rule 1.3.3 (admissible build and execution modes — the frozen recipe, at its pinned safety mode).**
The admissible modes for a witness are **exactly the entries the frozen recipe lists for that
language** in `environment/environment.json` → `semantic_compression_recipes`, as mirrored in doc 01
`toolchain_binding.recipes` and checked for byte agreement by gate V5 — plus
`toolchain_binding.multi_unit_recipes` at the two probes that require a second compilation unit. The
list is read from the file, never constructed by the analyst, and each witness records which entry it
used. No other mode is admissible, **in either direction**: a class realizable only under a mode the
frozen recipe does not name is not realizable for that language, and a class the recipe's mode does
reach may not be excluded by appealing to a mode the recipe does not name.

The recipe's `safety_mode_clause` is the operative part of this rule and is restated here because it is
the single configuration choice that could decide `F08.P1`, `F08.P2` and `F11.P1` by artefact:

> Where a language exposes more than one arithmetic/bounds safety mode, the frozen recipe selects the
> **optimized checked mode**. Applied: Rust `-O -C debug-assertions=on` rather than `-O` alone, which
> silently turns overflow checks off; Zig `-OReleaseSafe` rather than `-OReleaseFast`, which makes
> signed overflow and out-of-range indexing undefined behaviour; Swift `-O`, which retains traps; and
> the equivalent optimized-and-checked selection for every other language that has a choice. Selecting
> the checked mode is the frozen recipe, not a deviation, for any language.

Two consequences, both binding and both symmetric:

* **No checks-disabled comparison.** Because every language with a choice is pinned to its checked
  mode, no language's always-on checking is ever compared against another language's checks-disabled
  release build. The pre-remediation admission of "every documented mode" reintroduced exactly that
  comparison from the other side: it would have charged the multi-mode incumbents extra branches for
  unchecked modes the benchmark does not actually build, which is equally an artefact of the mode list
  rather than a property of the language.
* **No checked mode is not a penalty and not an exemption.** A language that has no checked mode (the
  corpus records C++ as such) is enumerated on its unchecked mode's merits, because that is the
  language's only mode; that is a **language fact**, scored by `B` here and by Hidden Semantic Cost in
  doc 04, and it is never corrected for, softened, or re-described as an incumbency effect (§7).
  Equally, a language with more than one *execution* mode in its recipe — for example one whose recipe
  lists both a compiled binary and an interpreter run — has every listed mode enumerated, and a class
  realizable in any listed mode is realizable for that language.

A language whose behaviour is identical across every mode its recipe lists records that identity, with
refutation witnesses under Rule 1.3.1.e, rather than being credited for it in advance. The
*non-locality* of the mode selection is charged once, in Part 2, as a `G` hop (§2.6) — the source text
still does not say which mode built it — and is flagged, not added, in Part 1 (§1.5.2, `R5`).

### 1.4 The fixed enumeration axes

`B_i` is enumerated along a fixed, closed list of ten axes. Each axis has a **closed** list of outcome
classes. Analysts may not invent an axis or a class; unanticipated cases go to the adjudication
register (§3.3), which then applies retroactively to all ten languages.

| Axis | Name | Closed outcome classes |
|------|------|------------------------|
| **A1** | Argument/value binding at the span | copy · converting copy · move/consume · alias to the operand's own storage · deferred (operand or body not evaluated here) · non-call/token substitution |
| **A2** | Caller-observable mutation via the span | none · the operand variable's **own storage** may be written · state **referred to** by the operand may be mutated (but not its own storage) |
| **A3** | Initialization / definedness | operand guaranteed initialized · operand may be read uninitialized/undefined · operand may be a null/nil/absent value |
| **A4** | Type and representation of the value produced/consumed | fixed-width signed integer · fixed-width unsigned integer · arbitrary-precision integer · binary floating point · boxed/reference-typed numeric · text · user-defined type · pointer/handle |
| **A5** | Conversion applied at the span | none · lossless widening · narrowing/lossy · sign reinterpretation · boxing/unboxing/existential wrapping · user-defined conversion · textual coercion |
| **A6** | Failure mode of the span | cannot fail · may trap/panic/abort · may throw a declared/checked exception · may throw an undeclared exception · may yield an error value that the span does not force the caller to handle · undefined behaviour |
| **A7** | Dispatch kind | target fixed by compile-time information · target may differ between executions of this same span (virtual, dynamic attribute, rebindable name, function-valued variable) · the operator/call is user-redefinable for the operand types |
| **A8** | Resource event at the span | none observable · allocation may occur · copy/clone may run user code · move (source invalidated) · refcount change · destruction/cleanup may run here · borrow begins and ends here |
| **A9** | Control-flow effect of the span | falls through · may return/propagate early · may unwind · may suspend/resume later · may not return |
| **A10** | Numeric / result semantics | mathematically exact · wraps modulo 2^n · saturates · traps on overflow · undefined on overflow · build-mode dependent · rounding/NaN/Inf applies · identity vs structural comparison · total vs partial order |

**Rule 1.4.1 (in-scope axes, read from the corpus).** For each probe the frozen probe record carries
`semantic_facts_expected`: the subset of the 13 `semantic_fact_kinds` of §6.1.3 that the probe is
annotated with. **That list is read programmatically from
`01_capability_universe_and_probes.json[probes][i].semantic_facts_expected`; no rule, table or worked
example in this document may state a probe's fact set by hand.** The 13 snake_case ids are the only
fact vocabulary usable anywhere in Primary Evaluation 1 and are the join key between this document, the
corpus, and the raw records. The **in-scope axes** for a probe are exactly the axes the following fixed
mapping associates with those ids, subject to the precedence rule of §1.4.2. The in-scope set is
identical for all ten languages for a given probe and is frozen with the probe.

| `semantic_fact_kinds` id | Axes brought in scope |
|---|---|
| `value_vs_storage` | A1 |
| `mutability` | A2 |
| `aliasing_writable_aliasing` | A1, A2 |
| `initialization_state` | A3 |
| `type_and_representation` | A4 |
| `conversion_behavior` | A5 |
| `possible_failure` | A6 |
| `alternative_value_cases` | A6, A7 |
| `overflow_exceptional_numeric` | A10 |
| `allocation_copy_move_borrow_destroy` | A8 |
| `externally_visible_side_effects` | A2, A7 |
| `control_flow_effect` | A9 |
| `lifetime_resource_effect` | A8 |

**Rule 1.4.2 (specialised tables govern, and every excluded fact names its charging metric).**
§6.1.4.B fixes the ARG-PLAIN / ARG-EXPLICIT question: "how many materially distinct **aliasing /
mutation** outcomes remain possible from the call-site syntax alone", and `F05.P1`'s `SCORING NOTE`
fixes the minimum outcome set that answer must contain. **Where a specialised table of §1.5 applies to
a probe, that table's axis set governs and overrides the §1.4.1 mapping for that probe.** For
ARG-PLAIN and ARG-EXPLICIT the governing axis set is **A1 × A2 × E**, where `E` is the escape axis of
Table ARG (§1.5.1) carved out of A8's lifetime dimension; A8's remaining resource dimensions, and
axes A3, A4, A5, A6, A7, A9 and A10, are out of scope there.

Excluding an axis is only legitimate if the fact it would have carried is charged somewhere. Every
fact in `F05.P1`'s and `F05.P2`'s frozen `semantic_facts_expected` is therefore accounted for
explicitly:

| Fact in the probe's frozen set | In scope for `B` here? | Where it is charged instead |
|---|---|---|
| `aliasing_writable_aliasing` | yes — A1 × A2 | — |
| `mutability` | yes — A2 | — |
| `value_vs_storage` | yes — A1 | — |
| `allocation_copy_move_borrow_destroy` | the **escape/retention** dimension only, as axis E | the allocation, copy, move and destruction dimensions are charged at Hidden Semantic Cost (doc 04, §6.1.4.D) and at Semantic Locality (§2) |
| `control_flow_effect` | no | Semantic Locality (§2.8.1.b), and Hidden Semantic Cost for unsignalled control-flow effects |
| `externally_visible_side_effects` | no | Semantic Locality (§2.8.1.a), and Hidden Semantic Cost |
| `lifetime_resource_effect` (`F05.P2`) | the escape dimension only, as axis E | Semantic Locality and Hidden Semantic Cost |

**Any axis excluded by a specialised table whose fact appears in that probe's frozen
`semantic_facts_expected` must name the metric that does charge it, in a table like the one above, or
it must be brought back in scope.** An analyst who finds a fact with no charging metric records an
adjudication-register case (§3.3) rather than dropping the fact. This is also the mechanism that
prevents the same language property from being counted twice inside Semantic Determinacy: a property
is enumerated on exactly one axis, and axes removed here are charged by a different metric with a
different weight, never a second time by this one.

**Rule 1.4.3 (enumeration order).** Axes are always enumerated in index order A1 → A10. The
enumeration is a decision tree, not a product: the analyst enumerates a parent axis fully, then for
each realizable parent outcome enumerates the next in-scope axis.

**Rule 1.4.4 (entailment / no double counting).** If a parent outcome **entails** an outcome on a
later in-scope axis, the later axis contributes no branching under that parent. Binding rulings:

* A1 = *copy* entails A2 ≠ *own storage written* (the callee holds a different object).
* A1 = *move/consume* entails that the operand's own state changes as a documented consequence of
  passing; the whole A1 = move subtree therefore collapses to a **single** outcome, with no A2
  sub-branching, for every language.
* A1 = *deferred* entails A2 = *none at this span*; the subtree collapses to a single outcome.
* A1 = *non-call/token substitution* admits arbitrary meaning; the subtree collapses to a **single**
  outcome (it is one class of failure-of-locality, not a multiplier).
* A8 = *no observable resource event* entails nothing about A2, and vice versa.

**Rule 1.4.5 (observational-equivalence merge).** Two enumerated vectors that no observer program can
distinguish (Rule 1.2.1) are **one** outcome and are counted once. The canonical instance: in C++,
`void f(const S&)` whose body mutates a `mutable` member, and `void f(S&)`, produce the same vector
(alias · own storage written) and count once, even though the two signatures differ. Determinacy
counts **meanings**, not spellings.

**Rule 1.4.6 (unbounded families collapse to classes).** Where a language admits an unbounded family
of completions (an arbitrarily large overload set, arbitrary user-defined conversions, arbitrary
trait/protocol implementations, arbitrary macro expansions), the family contributes only the closed
outcome classes of §1.4 that it can realize. `B_i` is never "many" and never grows with the size of a
program the analyst imagines.

**Rule 1.4.7 (dispatch is a *kind*, not an identity — no cross-execution multiplication).** The
identity of the callee is external in **every** language; treating "which `f` runs" as branching
would fold Semantic Locality into Semantic Determinacy and would double-count. A7 therefore creates a
branch only when the span leaves open a *different kind* of dispatch that **enables an outcome class
not otherwise realizable** at that span. Mere variability of the target across executions (a
rebindable global, a function-valued variable, a virtual call) adds **no** branch, because on each
execution the behaviour is one of the classes already enumerated. A user-redefinable operator **does**
add branches, because it makes classes reachable (allocation, failure, mutation, arbitrary control
flow) that the built-in operator cannot reach.

**Rule 1.4.8 (token substitution).** A mechanism that can replace the measured fragment's tokens
before semantic analysis, **without any marker inside the fragment**, contributes exactly one
additional outcome (A1 = non-call/token substitution). It does not apply to mechanisms that require a
marker in the fragment (a macro invocation the language spells with its own sigil, a compile-time block
the language requires to be written out), and it does not apply to a language that has no such
mechanism at all — which is an exclusion carrying the `LOW-B-BY-ABSENCE` flag of Rule 1.9.5, like any
other class a language cannot reach because it lacks the construct. Which languages have such a
mechanism is determined per language from the specification and evidenced under Rule 1.3.1.e, not
assumed.

**Rule 1.4.9 (minimum).** `B_i ≥ 1`. `B_i = 1` means the measured fragment, read against the language
specification alone, admits exactly one materially different interpretation: **fully determined**.
`B_i = 0` is not defined and must never be recorded.

**Rule 1.4.10 (no cap).** There is no upper cap on `B_i`. Every counted branch carries a witness, so a
large `B_i` is auditable rather than rhetorical. Values above 16 additionally require both analysts to
sign the enumeration (§3.2).

### 1.5 The two specialised outcome tables

These tables are frozen instantiations of §1.4 for the probe families where §6.1.4.B or the corpus's
own `SCORING NOTE` mandates a specific question. They exist so that the enumeration is a *checklist*,
not an essay.

**Rule 1.5.-1 (the tables cover 6 probes of 44; the rest are not exempt).** Table ARG governs `F05.P1`
and `F05.P2`; Table NUM governs `F07.P1`, `F07.P2`, `F08.P1` and `F08.P2`. **Every other probe in the
corpus — including `F05.P3`, the third family-5 probe, whose argument is a function — is enumerated
under §1.4 directly**, with its in-scope axes taken from the §1.4.1 mapping applied to its own frozen
`semantic_facts_expected`, its classes taken from the closed class lists of §1.4, and every class
dispositioned as witnessed or refuted under Rule 1.7.1 step 4. A probe is never skipped, never given a
reduced enumeration, and never assigned `B = 1` by default because no specialised table names it.

**Rule 1.5.0 (the corpus's minimum outcome sets govern).** Where a frozen probe record states a
minimum outcome set — as `F05.P1` does, listing *{callee cannot observe caller's object at all; callee
may read but not write it; callee may write elements observable through `x`; callee may replace/resize
the sequence observable through `x`; callee may retain the alias beyond the call; a copy was made so no
caller-visible change is possible}* — **every member of that set must map onto a class in the
applicable table of this section.** A member with no mapping is a defect in *this* document, not in the
probe, and is repaired here. A language for which a mapped class is not realizable records it as an
exclusion under Rule 1.3.1.e, exactly as for any other class; the class is never deleted from the
table because some language cannot reach it.

#### 1.5.1 Table ARG — argument-passing probes (ARG-PLAIN `F05.P1`, ARG-EXPLICIT `F05.P2`)

Governing axes **P × M × E**. `P` classes are mutually exclusive; `M` classes are mutually exclusive;
`E` classes are mutually exclusive.

**Axis P (what happens to the argument at the boundary):**

| Id | Class | Question the analyst answers |
|----|-------|------------------------------|
| P1 | copy / derived handle | Can the callee receive an independent copy of the operand's value (or a reborrowed/derived handle), with the operand still usable afterwards? |
| P2 | converting copy | Can an implicit conversion at this site hand the callee an object of a *different type* than the operand's? |
| P3 | move / consume | Can passing leave the operand invalid, unusable, or in a documented changed state? |
| P4 | alias to the operand's own storage | Can the callee be given access to the operand variable's own storage, with no copy of its value? |
| P5 | deferred | Can the callee's body fail to execute at this site (generator, lazy coroutine, by-name)? |
| P6 | non-call | Can the fragment fail to denote a call at all (Rule 1.4.8)? |

**Axis M (what caller-observable mutation is possible through this argument):**

| Id | Class |
|----|-------|
| M0 | none — after the call, no caller-observable state can have changed through this argument |
| M1 | the callee may write the **caller's variable's own storage** (replace or rebind the variable itself) |
| M2 | the callee may mutate the **contents** reachable through the argument — write elements in place — without changing which storage the variable denotes or how much of it there is |
| M3 | the callee may change the **extent or identity** of the storage the argument denotes: replace, resize, reallocate, clear, append to or truncate the sequence observable through the argument, without writing the caller's variable itself |

**Axis E (escape / retention beyond the call):**

| Id | Class |
|----|-------|
| E0 | the callee cannot retain access to the operand's storage or contents after the call returns |
| E1 | the callee may retain access beyond the call — store the alias in reachable state, capture it in a closure or task, hand it to another thread, or return it |

`B = |{(P, M, E) vectors with a witness}|`, after the entailment collapses of Rule 1.4.4 and the
observational-equivalence merge of Rule 1.4.5.

**Rule 1.5.1.a (M1 vs M2 vs M3 boundary).** M1 is *the caller's variable's own storage*: the callee can
make the caller's *variable* hold something different, or write the bytes of that variable. M2 is a
write to contents the argument reaches, leaving the sequence's extent and backing storage as they were.
M3 is a change to the extent or identity of that storage. The three phrases are stated without any
language's private value/object vocabulary and are decidable in all ten languages from the observers
the corpus's fragment already provides (the post-call read of element 0 plus, where the class requires
it, a length or identity observation).

**Rule 1.5.1.b (reborrow).** A derived alias handed to the callee while the operand remains usable
afterwards is **P1**, not P3 and not P4. The operand is the *variable named in the fragment*; if that
variable is itself a reference, mutation of its referent is M2 or M3 by Rule 1.5.1.a.

**Rule 1.5.1.c (entailments on M and E).**

* P3, P5 and P6 each contribute exactly **1** outcome and take no M and no E sub-branch (Rule 1.4.4).
* Under P1 where the callee receives an *independent copy of the value*, E1 is unrealizable with
  respect to the caller's storage: the callee retaining its own copy is not an escape of the caller's
  object. Under P1 where the callee receives a *derived handle*, E is enumerated.
* Under P2, E is enumerated: a converted object may capture a reference to the operand.
* Under P4, E is enumerated.
* E is about reachability after return, not about duration inside the call; a borrow that provably ends
  at the return is E0.

**Rule 1.5.1.d (mapping of `F05.P1`'s mandated minimum set).** For audit, the corpus's six mandated
outcomes map onto this table as: *callee cannot observe caller's object at all* → P1·M0·E0 (independent
copy) — the same vector as *a copy was made so no caller-visible change is possible*, which merges with
it under Rule 1.4.5 if and only if no observer distinguishes them; *callee may read but not write it* →
P4·M0·E0; *callee may write elements observable through `x`* → M2; *callee may replace/resize the
sequence observable through `x`* → M3; *callee may retain the alias beyond the call* → E1. No mandated
outcome is unmapped.

#### 1.5.2 Table NUM — numeric-operator probes (ARITH-EXPR `F07.P1`, ARITH-DIV `F07.P2`, OVERFLOW-SITE `F08.P1`, DIVZERO-SITE `F08.P2`)

Governing axes **A4 × A5 × A6 × A10** (from the fact ids `type_and_representation`,
`conversion_behavior`, `possible_failure`, `overflow_exceptional_numeric`), plus **A7** at `F08.P2`,
whose frozen fact set contains `alternative_value_cases`. Each axis is in scope for a probe only where
that probe's frozen `semantic_facts_expected` contains the fact that brings it in, and every fact those
sets contain that no axis here carries is accounted for in Rule 1.5.2.c. Outcome classes, mutually
exclusive as *behaviour classes*;
`B = |{realizable classes with a witness}|`:

| Id | Class |
|----|-------|
| R1 | exact / arbitrary precision — overflow is not possible |
| R2 | wraps modulo 2^n, defined |
| R3 | may trap / panic / throw / raise a run-time error |
| R4 | undefined or illegal behaviour on the exceptional case |
| R5 | behaviour is selected by a build mode, execution mode or global configuration (**flag only**, see below) |
| R6 | binary floating point — rounding, NaN, Inf |
| R7 | an implicit promotion/conversion is applied to an operand before the operation |
| R8 | the operator's meaning is supplied by the operand types (user-redefinable): may allocate, fail, mutate, or be non-arithmetic |
| R9 | pointer/handle arithmetic rather than numeric arithmetic |
| R10 | the rounding direction of an integer division, or the sign of a remainder, is not determined by the operator alone (truncation toward zero vs floor vs Euclidean; remainder sign following dividend vs divisor) |
| R11 | the exceptional case is turned into a **value** rather than an event: an optional, an error value, a saturated result, or a flag-plus-result pair that the fragment itself consumes |
| R12 | the operation **names its own discipline at the site** — an explicit wrapping / checked / saturating / trapping operation, or an explicit local guard the fragment contains — so that exactly one numeric behaviour is compatible with the fragment |

R5 is not counted *in addition to* the behaviour it selects: if a mode makes the fragment trap, that is
R3; R5 exists only to record that the selection is non-local, and is **reported as a flag on the probe
record**, never as an extra `+1`. (The non-locality itself is charged in Part 2 as a `G` hop.)

**Rule 1.5.2.a (per-probe rulings).**

* **ARITH-EXPR (`F07.P1`)** — the fragment is one binding statement `r = a * b + c` over three given
  signed-32-bit locals. Two operations occur inside the fragment, and a class realizable at *either*
  operation is realizable for the probe; it is counted once, not twice (Rule 1.4.5 merges identical
  vectors). The intermediate product is part of the fragment, so any class that only the intermediate
  can reach (an intermediate overflow that the final result would hide, a promotion applied to the
  product) is realizable and must be witnessed at the intermediate.
* **ARITH-DIV (`F07.P2`)** — the fragment is three binding statements. R10 is in scope for statements
  (1) and (2) and is the probe's central class: the corpus names as its **OBSERVATION TARGET** —
  expressly "not a support requirement; scored by metrics B/D only" — "whether a reader can determine
  the rounding direction of (1) and the sign of (2) when `a` is negative". A language that leaves it
  undeterminable is enumerated accordingly here; it is **not** thereby scored `PARTIAL` or `NONE`, and
  an analyst who converts an observation target into a support judgement has made an error in the
  rubric's favour or against it and must be corrected. R3/R4/R11 cover
  the zero-divisor and `INT_MIN / -1` cases; R6 and R7 cover statement (3) and the conversion the
  language requires to reach a 64-bit float. A class realizable at any of the three statements is
  realizable for the probe and is counted once.
* **OVERFLOW-SITE (`F08.P1`)** — the canonical task directs each language to use "the operation whose
  overflow behavior is determined BY THE OPERATION ITSELF", selected in the corpus's own fixed rung
  order: (i) a standard checked operation yielding an optional/error value, with the fallback integer 0
  substituted for the exceptional case; (ii) a standard wrapping operation; (iii) a standard saturating
  operation; (iv) the plain operator. The rung used, and the reason each higher rung was unavailable,
  are recorded with the fragment; the rung is selected by the corpus's order, never by the analyst's
  preference, and never by which rung would produce a lower `B`. **That operation is the fragment.**
  `m` is declared **inside** the fragment, so its type and its maximum value are local and free.
  Because the frozen recipe pins every language with a choice to its optimized checked mode
  (Rule 1.3.3), rung (iv)'s behaviour is that language's **checked-mode** behaviour, and a class
  realizable only under an unchecked mode the recipe does not name is not realizable. Classes realizable only
  for the plain operator are therefore *not realizable* for a language whose fragment names an explicit
  operation, and R12 is realizable for it. A language with no discipline-naming operation enumerates
  everything its plain operator admits; that is measured openness, not a penalty. The pre-remediation
  sentence excluding Zig's R2 on the ground that "wrapping requires the distinct `+%` operator inside
  the span" is **deleted**: under `F08.P1` the explicit operation *is* the span, for Zig and for every
  other language that has one. Conversely, a language in which the exceptional case cannot be selected
  at all — because only one discipline is expressible — records the classes it cannot reach as
  `LOW-B-BY-ABSENCE` exclusions under Rule 1.9.5.
* **DIVZERO-SITE (`F08.P2`)** — the corpus fixes the order of preference (checked-division operation,
  then an explicit local guard on `b == 0`, then the plain operator) and the fragment is whichever of
  those the language reaches first. Where the fragment contains an explicit guard, the guard is inside
  the counting unit and closes the classes it decides (R12); where it does not, R3/R4/R11 are
  enumerated on the plain operator's merits, at the recipe's pinned checked mode (Rule 1.3.3). The rung
  used and the reason each higher rung was unavailable are recorded, and the fallback integer 0 is
  substituted for the exceptional case in every language. `alternative_value_cases` is in this probe's
  frozen fact set, so A6 × A7 are in scope alongside A10.

**Rule 1.5.2.b (no hand-stated fragments).** For each of these four probes the analyst reads the
language's actual frozen fragment before enumerating. An enumeration performed against a remembered or
imagined expression — `a + b` on two operands, say, where the corpus's fragment is `a * b + c` on
three — is invalid and is discarded, not adjusted.

**Rule 1.5.2.c (every fact Table NUM excludes names its charging metric).** Rule 1.4.2 requires that an
axis a specialised table removes must name the metric that charges the fact instead, or be brought back
in scope. Table NUM's governing axes are A4 × A5 × A6 × A10 (with A7 added at `F08.P2` for
`alternative_value_cases`), and two facts in these four probes' frozen `semantic_facts_expected` fall
outside them. They are accounted for here rather than left implicit, because an unaccounted fact is the
same defect Finding 9 identified at Table ARG:

| Fact in the probe's frozen set | Probes | In scope for `B` here? | Where it is charged instead |
|---|---|---|---|
| `type_and_representation` | all four | yes — A4 | — |
| `conversion_behavior` | `F07.P1`, `F07.P2`, `F08.P1` | yes — A5 (classes R6, R7) | — |
| `overflow_exceptional_numeric` | all four | yes — A10 | — |
| `possible_failure` | `F07.P2`, `F08.P1`, `F08.P2` | yes — A6 (classes R3, R4, R11) | — |
| `alternative_value_cases` | `F08.P2` only | yes — A6 × A7 (Rule 1.5.2.a) | — |
| `value_vs_storage` | `F07.P1`, `F07.P2` | **no** — A1 is not an axis of Table NUM; these fragments are binding statements over given locals, not argument-passing sites, and R7/R8/R9 already carry every representation-changing outcome reachable at the operator | Semantic Locality (§2, §2.8.2 — the fact is in `F_i`, so every lookup required to resolve it is a hop), and Hidden Semantic Cost (doc 04, §6.1.4.D) |
| `control_flow_effect` | `F08.P1`, `F08.P2` | **no** — A9 would double-count: R3 (may trap/panic/throw/raise) and R11 (the exceptional case becomes a value) already decide, at the site, whether the span diverges, and Rule 1.4.4 forbids re-branching a later axis that a parent outcome entails | Semantic Locality (§2, §2.8.2, under the three-way test of Rule 2.8.1.b), and Hidden Semantic Cost for an unsignalled control-flow effect |

An analyst who finds a fact in one of these four probes' frozen sets that this table does not account
for records an adjudication-register case (§3.3) rather than dropping the fact or inventing an axis.

### 1.6 The glyph rule (mandatory, from §6.1.4.B)

> "Do not award or remove points merely because a language uses the glyph `&`. Score the semantic
> determinacy conveyed by the complete surface form."

**Rule 1.6.1.** `&`, `*`, `ref`, `inout`, `borrow`, `ptr`, `addr`, `mut` and every other marker are
**tokens with language-specific meaning**. The analyst resolves each token against the language's own
specification and then enumerates §1.5.1. It is forbidden to:

* transfer an intuition about a marker from one language to another;
* award a branch reduction because a marker is *present*;
* add a branch because a marker is *absent*;
* treat two languages' identically-spelled spans as having the same `B`;
* treat a language's lack of any explicit alias form as a determinacy advantage (it is an
  ARG-EXPLICIT `support_level: NONE` with reason code `NO_EXPLICIT_ALIAS_FORM`, excluded from the
  determinacy mean and charged in full against Capability Coverage `C`, per §1.9 and §6.1.5);
* treat the *absence* of a construct as a determinacy result without flagging it (Rule 1.9.5).

**Rule 1.6.2 (the glyph rule is a procedure, not a table of results).** The same glyph is expected to
yield different counts in different languages, and any such difference must come out of the enumeration
rather than out of sympathy. The pre-remediation text stated a five-row table of ARG-EXPLICIT `B`
values (Rust 3, Zig 2, Go 2, Swift 4, C++ 6) as the proof of glyph-neutrality. **That table is
withdrawn** (§4): it was enumerated over a bare call to an *unknown* callee, whereas `F05.P2`'s frozen
measured fragment contains `put`'s full declaration and body, so most of those classes are not realizable
inside the actual counting unit and the numbers cannot be reproduced from the corpus. A withdrawn
number is not replaced by a guess.

What replaces it is the procedure, which is what the glyph rule actually requires:

1. Read the language's frozen `F05.P2` fragment, including `put`'s declaration and body.
2. Enumerate Table ARG (§1.5.1) over the whole fragment, with `put` visible.
3. Record every counted vector with a witness (Rule 1.3.1) and every excluded class with a
   specification citation and a refutation witness (Rule 1.3.1.e).
4. Publish the resulting `B` per language beside the glyph each language used, so a reader can check
   for themselves that identical glyphs did not produce identical counts and that different glyphs did
   not produce a systematic advantage.

Neutrality is demonstrated by step 4 on measured data, not asserted in advance by this document.

### 1.7 Enumeration procedure and the status of the worked examples

**Rule 1.7.0 (status: the pre-remediation precedents are withdrawn).** The pre-remediation §1.7 stated
"binding precedents" — ARG-PLAIN `B` values of C++ 11, Python 3, Rust 4, Go 4, Java 4, Kotlin 4, Zig 4,
TypeScript 5, Swift 6, and ARITH values of C++ 7, Python 4, Rust 4, Go 3, Zig 4 — that a measuring agent
was required to reproduce. Every one of them is withdrawn, for three independent reasons, each
sufficient on its own:

1. They were enumerated over a counting unit this document invented (a "focus span" excluding the
   operand's own declaration) rather than over the corpus's `measured_fragment`, which includes it
   (Rule 1.1.1, Rule 1.1.2.a).
2. They were enumerated over operands the frozen probe forbids. `F05.P1` pins the operand to a local
   `x` bound to a growable integer sequence containing 1, 2, 3. The withdrawn witnesses used
   `struct S{int v;}` and `struct S{int*p;}` (C++), an immutable `x = 7` (Python), a
   `#[derive(Clone,Copy)] struct S` (Rust), `int x` / `int[] x` (Java). With the operand pinned as the
   corpus pins it, several of those rows are impossible: a growable integer sequence is not `Copy` in
   Rust; `std::vector<int>`'s copy constructor is specified by the C++ standard and cannot empty its
   source; Java's autoboxing/widening row has no operand to apply to.
3. The ARITH rows were enumerated over `a + b` on two operands. The corpus contains no such probe:
   `F07.P1` is `a * b + c` on three operands, `F07.P2` is three division statements, `F08.P1` is
   `m + 1` at the signed-32-bit maximum under a discipline-naming operation, and `F08.P2` is a guarded
   division.

No replacement numbers are stated here. **Until each enumeration has been produced against the real
frozen fragment and its witnesses and refutation witnesses have been stored, §1.7 is illustrative of
*procedure* only and is explicitly non-binding on any value.** Spec §4 forbids a score supported only
by general language knowledge, and a precedent that no stored artefact backs is exactly that.

**Rule 1.7.1 (the procedure that replaces the precedents).** For each probe `i` and language `L`:

1. Load the probe record and read `measured_fragment`, `canonical_task`, any `SCORING NOTE`, and
   `semantic_facts_expected` (programmatically — Rule 1.4.1).
2. Load `L`'s frozen fragment for that probe. If `L` has no fragment, the probe is
   `support_level: NONE` (§1.9); stop.
3. Determine the counting site (Rule 1.1.1) and the governing axis set (Rule 1.4.2).
4. Walk the applicable table of §1.5 class by class, in table order. For each class, either build a
   witness (Rule 1.3.1) or record an exclusion with a specification citation and a refutation witness
   (Rule 1.3.1.e). Every class in the table is dispositioned; none is skipped.
5. Apply the entailment collapses (Rule 1.4.4, Rule 1.5.1.c) and the observational-equivalence merge
   (Rule 1.4.5).
6. Flag `LOW-B-BY-ABSENCE` on every exclusion whose ground is that the language has no such construct
   at all (Rule 1.9.5), and `R5` on every mode-selected behaviour (§1.5.2).
7. Record `B_i` and `log2(B_i)`; two analysts do this independently (§3.2).

**Rule 1.7.2 (calibration is sealed, not published in the frozen text).** Calibration enumerations —
worked examples whose purpose is to let an analyst check their reading of the procedure — are useful,
but publishing them inside the frozen document lets every analyst see the number to beat before
counting, and the language counted first in the fixed column order benefits most. Calibration
enumerations are therefore produced against the real fragments, stored in the sealed annex of §4, and
released to an analyst **only after that analyst's own enumeration for that probe and language has been
recorded and signed**. Until the annex is populated with witness-backed enumerations it is empty, and
its emptiness is not a licence to enumerate from memory.

**Rule 1.7.3 (what the procedure is and is not claimed to do).** Four claims, corrected:

1. The procedure separates languages by *what their fragments cannot mean*, not by taste. It does this
   only to the extent that every counted class carries a witness and every excluded class carries a
   refutation witness.
2. It does not reward terseness as such: a short fragment that admits many meanings scores badly on
   `B`, and a long fragment that admits one scores well.
3. It does not punish explicitness: a marker inside the fragment closes the classes it decides, and
   that closure is recorded as a determinacy gain.
4. **It does not reward a language for lacking a capability — but only where the absence makes a probe
   unexpressible.** An unexpressible probe is `support_level: NONE`, leaves the metric's mean, and is
   charged in full against `C`. An absence that merely *removes an outcome class* from a probe the
   language *can* express is a different channel, and `C` does not reach it: a language without
   wrapping arithmetic cannot realize R2, a language without user-redefinable operators cannot realize
   R8, a language without implicit numeric promotion cannot realize R7, a language without textual
   substitution cannot realize P6, and each such absence lowers `B` and raises the determinacy score
   with no offsetting charge anywhere. §6.1.5's guard that "a tiny language cannot obtain a high primary
   score merely by having very few rules" is implemented solely through `C` and does not reach this
   channel. Rule 1.9.5 makes the channel visible; it is the only protection this metric has against it,
   and it applies to all ten languages identically.

**Rule 1.7.4 (ARG-EXPLICIT where the language has no explicit form).** A language that provides no
documented way for a callee to write a caller's scalar local variable writes no `F05.P2` fragment. Per
doc 01 `na_policy` NA-6 that is recorded as **`support_level: NONE`** with
`none_reason_code: NO_EXPLICIT_ALIAS_FORM` and a written justification — **not** as `N/A`. The probe is
excluded from that language's determinacy and locality means, and scores **0 for both capability
points of `F05.P2` in family 5 (function calls and argument-passing semantics)**, with those 2 points
remaining in `C`'s fixed denominator of 88 (NA-1, NA-2). Every probe in the corpus is worth exactly
2 points, so no probe's coverage cost depends on which language fails it. Wrapping the integer in a mutable
container/object is not an acceptable substitute for FULL; the rubric scores that under criterion P-a
as PARTIAL, and a PARTIAL fragment **is** enumerated normally for `B` and `H` (§1.9). Which languages
land in which of these states is a measurement, and is not pre-assigned here.

### 1.8 Aggregation and normalization

**Raw aggregate (per language L), on a stated basis:**

```
D_L = mean over the probes of the reporting basis, of log2(B_i)
```

Lower is better. Doc 01's `support_rubric.interaction_with_quality_metrics` predeclares **two** bases,
and Rule 1.8.6 fixes which is primary:

* **COMMON BASIS (primary)** — the probes for which **all ten** languages have a fragment, `FULL` or
  `PARTIAL`. Every language's `D_L` is then a mean over identical material.
* **ALL-FRAGMENTS BASIS (secondary, published)** — each language's own applicable probes, i.e. those for
  which it has a fragment. Probes recorded `support_level: NONE` leave this mean and are charged in
  full against `C` (§1.9).

**No figure produced by this document may be published without stating its basis and its basis size**,
and a `D_best` from one basis is never combined with a `D_L` from the other.

**Rule 1.8.1 (the logarithm is pre-registered).** §25.1 states: "Do not introduce logarithmic scaling
unless this specification explicitly requires it for that metric. Semantic Determinacy is the explicit
exception: its raw aggregate is `mean(log2(B_i))` before lower-is-better normalization." This log
aggregation is therefore the specification's single, explicit, pre-registered exception to the
no-logarithms rule of §25.1 / §24. It is applied here and **nowhere else** in the benchmark. No other
metric in this document, including Semantic Locality, may use a logarithm.

**Rule 1.8.2 (normalization — family C, shifted form).** `D_L` is a positive lower-is-better quantity
for which a legitimate exact zero exists (a language all of whose applicable probes are fully
determined has `D_L = 0`). §25.1.C therefore requires the predeclared shifted form:

```
Score_L = 100 * (D_best + epsilon) / (D_L + epsilon)
```

where `D_best = min over the ten languages of D_L on the same basis as the `D_L` being normalized`,
and

```
epsilon = 1/N = 1/44   (exact rational; ≈ 0.0227272727, carried at full precision by the script)
```

**Rule 1.8.3 (epsilon is a predeclared constant of the corpus, not of a language).** `epsilon` is
fixed at `1/N` with `N = 44`, the frozen corpus size (`probe_count`). It is a predeclared constant,
fixed now, before any language is measured, used for both metrics in this document, and **it is not
recomputed per language even where a language's applicable probe count is smaller than 44.** It is
carried as the exact rational `1/44` and never rounded before the division; the decimal above is for
reading only. Two pre-remediation defects are corrected here: the constant was stated for a corpus of
40 probes, which the corpus no longer is; and it was *derived* as "the smallest nonzero value `D_L` can
take over `N = 40` probes", which is wrong for exactly the languages whose mean is taken over fewer
probes than the corpus holds. The fix is to stop deriving the constant from a per-language quantity
rather than to let it vary: a constant that moved with a language's exclusion count would make the
normalization itself depend on how much a language could not express.

**Rule 1.8.4 (mandatory family-C reporting).** Publish beside the normalized score, for every
language: every raw `B_i`, `D_L`, and the ratio `D_L / D_best`. If the applicable raw `D_L` values
span a factor of 100 or more, additionally publish the §25.1 compression note stating that the ratios,
not the scores, carry the comparison between non-leading languages. Do not switch normalization
families to recover resolution.

**Rule 1.8.5 (weight).** Semantic Determinacy carries 0.25 of the Semantic Compression quality score
`Q` (§6.1.6). This document does not change that weight.

**Rule 1.8.6 (the common basis is PRIMARY; the all-fragments basis is a published secondary).**
`D_L` taken over each language's own applicable probes is a mean over a probe set that **differs per
language**, because a language records `support_level: NONE` on the probes it cannot express and those
probes leave its denominator. A language that cannot express the hardest probes therefore has them
removed from its determinacy denominator, which generally lowers its mean and raises its normalized
score, while the offsetting charge lands only in `C`. `D_best`, which sets the scale for all ten
languages under family-C normalization, may likewise be produced by whichever language has the smallest
and easiest applicable set.

The pre-remediation text kept the variable-basis aggregate as primary, added the common core as a
disclosure, and defended the arrangement with the assertion that the harmonic mean `2QC/(Q+C)` makes
the trade "unprofitable". **That assertion is withdrawn**: it is an argument rather than a measurement,
it was never derived, and doc 01 records that it is false under a variable basis. It is replaced by the
corpus's own predeclared rule, which removes the trade mechanically instead of arguing about it:

```
COMMON  = { probes i : all ten languages have a fragment for i, FULL or PARTIAL }
D_L            = mean over i in COMMON of log2(B_i)          # PRIMARY — reported as the result
D_L^allfrag    = mean over i applicable to L of log2(B_i)     # SECONDARY — published beside it
```

Binding consequences, all taken from doc 01
`support_rubric.interaction_with_quality_metrics.common_basis_rule` and
`.partial_fragments_enter_the_common_basis`:

1. **The common basis is the primary basis** for Semantic Determinacy and Semantic Locality, so every
   language's `Q` contribution is measured on identical material. A language cannot change the material
   its aggregate is computed on, because that material is the same intersection for everyone.
2. **`PARTIAL` fragments are in the common basis**, applied identically to all ten languages, with no
   exemption. A `PARTIAL` fragment is a real fragment with real branches and real lookups; excluding
   `PARTIAL`s while also excluding `NONE`s would make a clumsy approximation cost more than a flat
   inability, which would invert the incentive the metric exists to create.
3. **Both bases are published**, together with `|COMMON|`, each language's fragment count `n_L`
   (corpus authoring rule R8), and the per-metric delta between the two bases.
4. **Every published figure states its basis and its basis size.** Where the two bases disagree about a
   ranking, **both rankings are published** and the common-basis ranking is the one reported as the
   result. Neither basis may be selected after seeing which flatters a language (§3.4).
5. `COMMON` is computed once, from the same fragment-existence table, for both metrics in this
   document, and is not recomputed per metric or per language.
6. Capability Coverage `C`, on the fixed 88-point denominator, remains the **sole** channel through
   which an unsupported capability affects the score. The common basis removes the exclusion advantage
   from `B` and `H`; it does not move the coverage charge, soften it, or duplicate it.

This reverses the primary/secondary orientation the pre-remediation text used. It is adopted because
the frozen corpus predeclares it and names this document in its `counterpart_obligations`, and because
it is the stronger form of the same correction: the audit asked for the common core to be *disclosed*,
and the corpus requires it to be *reported as the result*. In this document's field names and schemas
the common basis keeps the key `core` (`D_core_mean_log2`, `core_probe_ids`, `|CORE|`) so downstream
scripts and sibling documents do not have to be renamed; the semantics are the corpus's.

### 1.9 Missing-capability policy for `B_i` (`support_level: NONE`, not `N/A`)

Doc 01 `na_policy` NA-6 is binding: *"'The language has no concept of X' is a NONE with a
justification, never an N/A."* NA-5 requires every `N/A` actually recorded anywhere in Primary
Evaluation 1 to name the rule that permits it. This document therefore records capability absences as
`support_level: NONE` with a machine-readable `none_reason_code`, and reserves `N/A` for the one case
NA-4 allows — a measurement that cannot be taken for a reason unrelated to the language's capability —
where it carries `na_policy_rule: "NA-4"` and a written justification.

| `support_level` | `none_reason_code` | Condition | Effect on `D_L` | Effect on `C` |
|---|---|---|---|---|
| `NONE` | `UNSUPPORTED` | The frozen support rubric records that the language cannot express the probe's capability at all | probe excluded from the mean | the probe's full `capability_points` (2, uniformly) score 0 and stay in `C`'s fixed denominator of 88 (NA-1, NA-2) |
| `NONE` | `NO_EXPLICIT_ALIAS_FORM` | `F05.P2` only: the language provides no documented way for a callee to write a caller's scalar local | probe excluded from the mean | as above — both points of `F05.P2` |
| `PARTIAL` | — | Expressible only through a workaround the rubric scores PARTIAL (criterion P-a, or a substitution the probe's own `canonical_task` names under corpus authoring rule R9) | **not** excluded — the workaround's own measured fragment is enumerated normally, and the probe stays in the common basis (§1.8.6) | partial credit per rubric (`support_factor` 0.5) |
| `FULL` | — | normal case | included | full credit |
| `N/A` | — | `na_policy` NA-4 only: the measurement cannot be taken for a reason unrelated to capability (e.g. the frozen toolchain cannot build or run the probe's witnesses at all, with the failing command recorded) | probe excluded; reason, rule id and failing command recorded | unaffected by this document; the rubric decides |

A missing capability is never converted to zero-determinacy, never converted to `B = 1`, never
converted to `H = 0`, and never used to remove an inconvenient probe (§26, §32). A language with many
`NONE` probes cannot buy a good determinacy score with them, because the primary aggregate is taken
over the **common basis** (Rule 1.8.6), which is the same probe set for all ten languages, so a `NONE`
removes nothing from the denominator that the metric reports; and because `C` falls on the fixed
88-point denominator. This document makes no claim that any particular aggregation makes the trade
"unprofitable": the common basis removes the trade mechanically instead of arguing about it.

**Rule 1.9.5 (absence flag — `LOW-B-BY-ABSENCE`).** `C` charges an absence that makes a probe
unexpressible. It does not charge an absence that merely deletes an outcome class from a probe the
language *can* express, and that second channel lowers `B` and raises the determinacy score with no
offsetting charge anywhere in the benchmark. Therefore:

> Where a class in a closed outcome table (§1.5) is excluded for a language **because the language
> provides no such construct at all** — rather than because the construct would have to be spelled
> inside the measured fragment, or because the probe's fragment pins an operand or an operation that
> does not reach the class — the probe record carries the flag `LOW-B-BY-ABSENCE` with the excluded
> `class_id` and the missing construct named in plain words.

The measuring agent publishes, per language: the count of `LOW-B-BY-ABSENCE` exclusions, the list of
class ids, and a **recomputed `D_L` with those classes counted as realizable**, beside the primary
`D_L`. The recomputed value is a disclosure only; it never replaces the primary score and no ranking is
computed from it. Examples of the channel, in the direction each cuts: a language whose integer
arithmetic is overflow-checked only cannot realize R2 (wrapping); a language without user-redefinable
operators cannot realize R8; a language without implicit numeric promotion cannot realize R7; a language
without textual substitution cannot realize P6; a language without any deferred-call form cannot realize
P5. Each of those absences lowers that language's `B`. The flag is language-blind, costs nothing to
compute, and is the only way a reader can tell a determinacy result earned by design from one earned by
omission.

The distinction the flag turns on is stated once, to keep it mechanical: an exclusion is
`LOW-B-BY-ABSENCE` iff the refutation witness of Rule 1.3.1.e fails because **no program in the
language** can realize the class, and is an ordinary exclusion iff it fails because **no program whose
measured fragment is this probe's fragment** can realize it while some other program in the language
can.

### 1.10 Raw record schema (determinacy)

One JSON file per language at
`semantic-compression/raw/determinacy_<language>.json`:

```json
{
  "language": "rust",
  "toolchain": "rustc 1.95.0",
  "recipe_entries_admitted": ["rustc -O -C debug-assertions=on FILE.rs -o BIN", "./BIN"],
  "recipe_agreement_check": "<path to the V5 record showing this matches environment.json byte for byte>",
  "methodology_doc": "methodology/03_determinacy_and_locality.md",
  "corpus_doc": "methodology/01_capability_universe_and_probes.json",
  "probes": [
    {
      "probe_id": "F05.P1",
      "probe_role": "ARG-PLAIN",
      "measured_fragment": "<the language's frozen fragment, verbatim>",
      "counting_site": "<the sub-expression named by the probe's SCORING NOTE, or the whole fragment>",
      "fact_set": ["aliasing_writable_aliasing", "mutability",
                   "allocation_copy_move_borrow_destroy", "value_vs_storage",
                   "control_flow_effect", "externally_visible_side_effects"],
      "in_scope_axes": ["A1", "A2", "E"],
      "outcome_table": "ARG",
      "branches": [
        {
          "class_id": "P1.M0.E0",
          "description": "<what this vector means for this fragment>",
          "witness_path": "semantic-compression/probes/witnesses/F05.P1/rust/P1.M0.E0/",
          "recipe_entry": "rustc -O -C debug-assertions=on FILE.rs -o BIN",
          "observer": "<the printed/instrumented output distinguishing this vector>",
          "spec_citation": "<numbered clause of the authoritative specification>"
        }
      ],
      "excluded_classes": [
        {
          "class_id": "P6",
          "reason": "<why this class is not realizable for this fragment>",
          "spec_citation": "<numbered clause, with specification version or SHA>",
          "refutation_witness_path": "semantic-compression/probes/witnesses/F05.P1/rust/EX-P6/",
          "refutation_outcome": "compiler rejection | runtime rejection | observer shows class does not occur",
          "low_b_by_absence": false,
          "missing_construct": null
        }
      ],
      "flags": [],
      "B": 0,
      "log2_B": 0.0,
      "analyst_a": "...", "analyst_b": "...", "reconciled": true,
      "support_level": "FULL",
      "none_reason_code": null,
      "na_policy_rule": null
    }
  ],
  "D_core_mean_log2": 0.0,
  "D_core_basis": "common",
  "core_probe_ids": [],
  "core_basis_size": 0,
  "D_allfrag_mean_log2": 0.0,
  "D_allfrag_basis": "all-fragments",
  "applicable_probe_count": 0,
  "n_L": 0,
  "corpus_probe_count": 44,
  "none_probes": [],
  "low_b_by_absence_count": 0,
  "D_core_mean_log2_absence_recomputed": 0.0
}
```

Every field is mandatory, and `D_core_mean_log2` — the **common-basis** figure — is the one reported as
the result (Rule 1.8.6). Validation fails if: a branch has no `witness_path`, `recipe_entry` or
`observer`; a `recipe_entry` is not an entry the frozen recipe lists for that language (Rule 1.3.3); an
entry in `excluded_classes` lacks either `spec_citation` or `refutation_witness_path`; a `probe_id` is
not a probe id of the frozen corpus; `corpus_probe_count` disagrees with the corpus's `probe_count`; a
`fact_set` member is not one of the 13 `semantic_fact_kinds` ids, or differs from that probe's frozen
`semantic_facts_expected`; a `low_b_by_absence: true` entry has no `missing_construct`; a published
figure carries no basis label; or a record uses `N/A` without `na_policy_rule`. The `B` and `log2_B` values shown above are placeholders: this
document states no expected value for any language.

---

## 2. PART 2 — SEMANTIC LOCALITY (`H_i`)

> §6.1.4.C: "Measure how much non-local context must be inspected to determine the semantic facts of
> each probe … A lookup that is optional for extra detail must not be counted; count only context
> required to resolve a fact in the fixed inventory."

### 2.1 What is being resolved

**Rule 2.1.1 (`F_i` is read from the corpus, never written by hand).** For probe `i`, the fact set
`F_i` is **read programmatically from
`01_capability_universe_and_probes.json[probes][i].semantic_facts_expected`**, using the 13 snake_case
`semantic_fact_kinds` ids, which the corpus states "are the ONLY fact kinds usable anywhere in Primary
Evaluation 1". `F_i` is identical for all ten languages for that probe. **No rule, table or worked
example in this document may state a fact set by hand**, and a worked example whose fact set differs
from the corpus's is discarded rather than reconciled. The pre-remediation §2.8.1 and §2.8.2 stated
fact sets from prose that added a fact the corpus does not annotate (`type_and_representation` at
`F05.P1`) and dropped two the corpus does (`control_flow_effect`, `externally_visible_side_effects`),
which changed every `H` value in those tables; both are withdrawn (§4).

Part 2 asks: **starting from the probe's measured fragment (Rule 1.1.1), how many distinct external
declaration sites or configuration artifacts must be read to resolve every fact in `F_i`?** Because the
counting unit is the corpus's `measured_fragment`, anything the corpus places inside the fragment is
already read and is never a hop — at `F05.P1` the operand's own binding, at `F05.P2` the whole of `put`.

Resolution is at the **class level**, matching Part 1's outcome classes: a fact is resolved when the
analyst can name which outcome class holds, not when they know every detail of the callee's
implementation. This is the hinge on which "required" versus "optional" turns (§2.4).

### 2.2 Definition of one hop

**Rule 2.2.1 (hop).** One **hop** is one traversal from the material already read to **one distinct
declared entity or one distinct configuration artifact** whose text must be read to resolve at least
one unresolved fact in `F_i`.

**Rule 2.2.2 (unit of counting).** A hop is counted **per distinct declared entity**, not per lookup
event, not per fact, and not per line:

* Reading one function signature that resolves five facts = **1** hop.
* Reading the same declaration twice = **1** hop.
* Two variables declared in a single statement are two declared entities = **2** hops if both must be
  read.
* An overload set = **1** hop per distinct declaration site in the candidate set captured under
  Rule 2.2.2.a. The analyst does not construct the set by reading.
* A type declaration and one of its members are the same declared entity if the member is written
  inside that declaration's own text = **1** hop; a member defined out-of-line elsewhere is a second
  hop.
* A trait / interface / protocol **implementation** is a distinct entity from the trait / interface /
  protocol **declaration**: reading both = **2** hops.
* An import/`use`/`using`/`#include` statement is a declared entity outside the measured fragment. If
  the analyst must read it to know which module a name comes from, that is **1** hop; the imported
  definition is another. A wildcard import costs `k` hops, with `k` fixed by Rule 2.2.2.b.
* Standard-library declarations are hops on the same terms as user declarations — except where the
  language's own specification fixes the fact for that declaration, which is zero-cost knowledge under
  Rule 2.2.3 and is not converted into a hop by the declaration's existence.

**Rule 2.2.2.a (the overload candidate set is a tool output, not a reading).** "Candidates with a
compatible arity, visible at the span, that must be examined" is not mechanical: default arguments,
parameter packs, optional parameters and platform-overload generators make arity a range; C++
argument-dependent lookup makes the candidate set depend on argument types that are themselves resolved
by a hop, which is circular; and several languages' resolution examines every candidate before ranking
them. Under-definition here falls almost entirely on five of the ten languages, so it is removed rather
than interpreted:

> The overload candidate set for a fragment is **the set the frozen toolchain itself reports**,
> captured mechanically and stored with the probe record — for example `clang -Xclang -ast-dump` or a
> `clangd` query; `javac -Xdiags:verbose` on a deliberately ambiguous variant of the call; `swiftc`
> diagnostics; `tsc` plus the TypeScript language service; `kotlinc` verbose resolution; the
> equivalent documented facility of any other language's frozen toolchain, including Quidra's.
> `H` counts the number of **distinct declaration sites in that captured set**. No analyst constructs
> the set by reading, and a language whose toolchain exposes no such facility records that fact, and
> the analyst's manual set, in the probe record for audit.

**Rule 2.2.2.b (wildcard imports).** `k` = the number of wildcard-imported modules that the language's
own name-resolution rules require to be examined **to establish that the name is unambiguous** — for
every language whose wildcard import is ambiguity-checked, that is all of them — regardless of the
order in which a human reader would happen to look. Scan order is not a measurable quantity and no
count may depend on it.

**Rule 2.2.3 (zero-cost knowledge).** The following are **not** hops:

* facts fixed by the language specification for the tokens in the fragment (Go's pass-by-value rule,
  Java's defined `int` wraparound, Python's arbitrary-precision `int`, Rust's "no implicit numeric
  promotion", Zig's immutable parameters, and the equivalent specification guarantees of the other
  five languages, each cited by clause);
* facts written inside the probe's measured fragment itself, including any declaration the corpus
  places inside it;
* the probe's own source file considered as a file (only *declarations outside the fragment*,
  individually, are hops).

**Rule 2.2.4 (transitive chains).** If resolving a fact requires a signature, and then the type named
in that signature, and then that type's protocol conformance, that is 3 hops. Chains count their
length. `H_i` is the size of the **smallest** set of declared entities that resolves all of `F_i`
(Rule 2.3.2).

**Rule 2.2.5 (`H_i` may be 0).** A probe whose fact set is fully resolved by the measured fragment plus
the language specification has `H_i = 0`. This is a legitimate value and is why aggregation uses the
shifted family-C form (§2.9).

### 2.3 Counting algorithm (mechanical)

```
INPUT : probe i, language L, the probe's frozen program, its measured_fragment
        (Rule 1.1.1), its fact set F_i read from the corpus (Rule 2.1.1),
        the language's authoritative specification, and the frozen build recipe
        and admissible recipe entries for L (Rule 1.3.3).
OUTPUT: H_i (integer >= 0), the hop list, the unresolved list, flags.

1.  U := F_i                       # unresolved facts
    S := {}                        # set of distinct entities read
    G := {}                        # set of distinct configuration artifacts read
2.  Resolve from the measured fragment alone, plus the language specification
    (Rule 2.2.3).    Remove every fact so resolved from U. Record, per fact, "resolved locally".
3.  while U is non-empty:
4.      Let CAND be the set of entities/artifacts not yet in S∪G, each of which,
        if read, would resolve at least one fact in U.
5.      if CAND is empty:
6.          mark every fact remaining in U as UNRESOLVABLE (Rule 2.7.1) and break.
7.      Choose the entity e in CAND that resolves the largest number of facts in U.
        Tie-break, in order: (a) the entity reachable in the fewest traversals from
        the measured fragment; (b) the entity declared in the same file as it;
        (c) the entity whose fully-qualified name sorts first (byte order).
8.      Add e to S (or to G if it is a configuration artifact).
9.      Remove from U every fact that e resolves at the class level (§2.1, §2.4).
10. H_i := |S| + |G|
11. if UNRESOLVABLE facts remain: H_i := SAT  (Rule 2.7.1), flag "whole-program".
12. Record H_i, the ordered hop list, |G| separately, and every flag.
```

**Rule 2.3.1 (determinism).** Step 7's greedy rule with its three tie-breaks is fully deterministic,
so two analysts running the algorithm on the same probe obtain the same `H_i` and the same hop list.
Disagreement is therefore always traceable to a disagreement about step 9 (did entity `e` actually
resolve fact `φ`?), which is adjudicated by §2.4 and, if still open, by the register (§3.3).

**Rule 2.3.2 (minimality).** `H_i` is the size of the smallest resolving set. The greedy rule of step 7
is the *fixed procedure* for finding it; where an analyst can exhibit a strictly smaller resolving
set, they record it and the smaller value is used, with the exhibit attached. This keeps the metric
from being inflated by a clumsy traversal order.

### 2.4 Required versus optional (the critical distinction)

**Rule 2.4.1.** A lookup is **required** iff, without it, at least one fact in `F_i` cannot be assigned
to an outcome class. A lookup is **optional** iff it only sharpens, confirms, quantifies, or explains
a fact already assigned to a class. **Optional lookups are never counted.**

**Counted (positive examples).** Each row names the frozen probe it applies to. A row that applies to
a construct no frozen probe contains is a rule illustration and is marked as such; it is never a
precedent for a count.

| # | Probe | Situation | Why required |
|---|---|---|---|
| P-1 | `F05.P1` | fact `value_vs_storage`: read the declaration of `f`, which the corpus places outside the fragment | Without the callee's declaration the call is compatible with several P classes and the fact has no class |
| P-2 | rule illustration | a signature whose parameter type is user-defined and whose own contents decide the class (e.g. a `const`-qualified reference to a type with a `mutable` or pointer member): read the type declaration | The signature alone does not choose between M0, M2 and M3. Where the parameter type is one whose semantics the language's own specification fixes, Rule 2.2.3 applies instead and there is no hop |
| P-3 | `F05.P1` | fact `mutability` in a language whose signatures carry no mutability information and whose annotations are neither required nor enforced: read the body of `f` | There is no signature-level fact, so the body is the *only* resolving entity — required, not optional. Which languages this applies to is determined per language from the specification, not assumed |
| P-4 | `F05.P1` | fact `allocation_copy_move_borrow_destroy` where copy-versus-move for the operand type is decided by a separate `impl`/`derive`/conformance site | Not decidable from the fragment or from the signature alone |
| P-5 | `F07.P1` | fact `type_and_representation` where the language's default integer width is platform-dependent: read the target architecture from the build configuration | The source does not say. This is a **G-hop** (§2.6) |
| P-6 | rule illustration | fact `alternative_value_cases` at a dispatching call: read the declared type of the receiver and the interface declaring the member | Which member set is in play is not in the fragment |
| P-7 | `F05.P1` | fact `control_flow_effect` in a language where a suspending call is spelled identically to an ordinary call: read the declaration of `f` | The control-flow fact has no class without the declaration (see Rule 2.8.1.b for the full three-way test) |

**Not counted (negative examples).** These are rule illustrations: each shows a lookup that must not be
counted once a fact already has a class. None is a precedent for a count.

| # | Situation | Why optional |
|---|---|---|
| N-1 | Reading the **body** of a C++ callee already known from its signature to take `const S&`, where `S` is a type with no `mutable` and no pointer members, to see whether it "really" mutates | The class (M0) is already settled by the signature plus the type declaration; the body adds detail only |
| N-2 | Reading a Rust callee's body after its signature says `&mut T`, to learn *what* it writes | The fact "writable aliasing" is already M1 |
| N-3 | Reading a type's documentation for complexity, thread-safety, or performance characteristics | Not one of the 13 `semantic_fact_kinds`, and not in this probe's `semantic_facts_expected` |
| N-4 | Reading the definitions of declarations outside the captured overload candidate set | Only the declaration sites in the toolchain's captured candidate set count (Rule 2.2.2.a) |
| N-5 | Reading an interface's implementations to learn which concrete type some *other* caller might pass | `type_and_representation` for the probe's own operand is fixed by the fragment or by the operand's declaration; a hypothetical caller's dynamic type is a property of other programs, not of this fragment — and at a probe whose fact set omits `type_and_representation` the lookup is not counted at all (Rule 2.4.2) |
| N-6 | Reading a superclass or supertrait chain after the declared type already resolves the probe's annotated facts | Confirmation only |
| N-7 | Reading the standard library's implementation of an operator whose behaviour the language specification already fixes | Rule 2.2.3: specification knowledge is free |
| N-8 | Following a chain further after every fact in `F_i` has a class | Step 3's loop has terminated |

**Rule 2.4.2 (the fact set is the boundary).** Curiosity is not a hop. If a fact is not in `F_i`, no
lookup for it is counted — even if it is interesting, and even if another probe annotates it.

### 2.5 Hop kinds (recorded, and all counted equally in `H_i`)

Every hop is labelled with exactly one kind. All kinds count 1 toward `H_i`; the labels exist for
reporting, diagnosis, and the separate G-hop disclosure of §2.6.

| Kind | Meaning |
|---|---|
| `SIG` | callee signature / function declaration |
| `VAR` | variable or parameter declaration |
| `TYPE` | type, struct, class, enum, or alias declaration |
| `OVL` | one declaration site in the captured overload candidate set (Rule 2.2.2.a) |
| `IMPL` | trait / interface / protocol implementation or conformance |
| `IFACE` | trait / interface / protocol *declaration* |
| `IMPORT` | import / `use` / `using` / `#include` statement |
| `BODY` | a callee body, counted only where no declaration-level fact exists for the fact being resolved (P-3; Rules 2.8.1.a and 2.8.1.b) |
| `G` | global configuration, build flag, language edition/version, or whole-program mode (§2.6) |

### 2.6 Whole-program and compiler-mode dependencies (G-hops)

**Rule 2.6.1.** A fact that is decided by a build flag, compiler mode, language edition/version,
target architecture, or other whole-program configuration is resolved by reading a **configuration
artifact**. That is one hop of kind `G`, counted once per distinct artifact per probe.

**Rule 2.6.2 (the artifact counts even though the recipe is frozen).** The benchmark's frozen build
recipes fix these flags, but the *source does not say so*. The fact is genuinely absent from the
program text, and a reader must leave the source to obtain it. It is therefore counted, identically
for every language. Known instances, none of which is specific to any language's advantage:

| Language | Example fact whose value is absent from the source | Artifact a reader must leave the source for | Kind |
|---|---|---|---|
| Quidra | whether the fragment was compiled to a binary or run under the interpreter, where any annotated fact differs between them | the recipe entry used (`quidra build …` vs `quidra run …`) | `G` |
| Python | which interpreter and version executes the fragment | the `run` recipe entry | `G` |
| C++ | signed overflow is UB, wraps, or traps; which standard mode applies | the flag set in the build command (`-std=c++20 -O2`, and any `-fwrapv` / `-ftrapv` / `-fsanitize=…` the recipe does or does not carry) | `G` |
| Rust | integer overflow panics or wraps | the overflow-check setting carried by the recipe (`-O -C debug-assertions=on`) | `G` |
| Go | width of `int`/`uint` | `GOARCH` | `G` |
| Java | which facts, if any, the JLS does not already fix | measurement to be made | `G` |
| TypeScript | whether optionality and implicit `any` are checked; which target the emitted JavaScript assumes | `--strict --target es2022 --module nodenext` in the build command | `G` |
| Kotlin | which backend/target and runtime the fragment is compiled for | the recipe's compile and run entries | `G` |
| Swift | overflow traps or is unchecked | the optimization/checking flag carried by the recipe (`-O`) | `G` |
| Zig | overflow panics, saturates, or is illegal behaviour; bounds checking | the build mode carried by the recipe (`-OReleaseSafe`) | `G` |

**No row is an exemption and no blank is an exoneration.** Every cell above is a *measurement to be
made per probe*, not a fixed allocation: a `G` hop is charged only where **that probe's** annotated
fact is actually decided by the artifact, and it is charged for every language where it is, including
languages not anticipated above. Where a language's own specification fixes the fact regardless of the
artifact (Rule 2.2.3), there is no `G` hop and the specification clause is cited. The frozen recipe
pins one safety mode per language (Rule 1.3.3), which decides *what the fact is*; it does not make the
fact local, because the source text still does not say which mode built it. That is why the `G` hop
survives a frozen recipe — for all ten languages, symmetrically.

**Rule 2.6.2.a (every language's recipe entries, on the same terms).** Any language in the fixed set
whose frozen recipe lists more than one build or execution entry — including a language whose two
entries are a compiled binary and an interpreter run — is charged a `G` hop wherever a probe's
annotated fact differs between those entries, on exactly the terms of Rule 2.6.1. A language whose
recipe lists one entry is still charged a `G` hop wherever the *single* entry's flags decide a fact the
source does not state: **one mode is not zero configuration**, and reading "which mode was it built
under" off a one-entry recipe is the same traversal out of the source as reading it off a two-entry
one. A language whose fact is identical across every entry its recipe lists, or is fixed by its own
specification, records `|G| = 0` for that probe, and the identity is evidenced under Rule 1.3.1.e
rather than assumed. The table above lists anticipated instances, not an exhaustive or fixed
allocation; a row is a hypothesis to be tested per probe, not an exemption granted or withheld.

**Rule 2.6.3 (separate disclosure).** `G` hops count 1 toward `H_i` exactly like every other hop, and
are **additionally** reported separately: per language, publish `|G|` per probe and the `G_hop_total`,
beside `H_L`. The separate figure exists because a `G` hop and a `SIG` hop are equally one lookup but
are not the same kind of non-locality — one leaves the source entirely — and a reader is entitled to see
how much of a language's `H` is declaration distance and how much is configuration dependence. The
disclosure changes no score: it may not be weighted, subtracted, added, or used to adjust `H_L`, and no
ranking is computed from it.

**Rule 2.6.4 (no double counting with Part 1).** A build-mode dependence contributes a `G` hop here
and an `R5` *flag* (not a `+1`) in Part 1 (§1.5.2). Where the modes produce genuinely different
behaviour classes, those classes are counted in Part 1 on their own merits, as behaviours, and the
`G` hop is counted here, as context. These are different metrics with different weights and the
specification requires both; this is not double counting, and the rule fixing which side gets the
`+1` is stated so that no analyst may take it twice on the same side.

### 2.7 Unresolvable facts and the saturation constant

**Rule 2.7.1 (`SAT` is a predeclared rule, not a predeclared literal).** If, at step 5 of the
algorithm, no finite set of declared entities can resolve a remaining fact — the required context is
the whole program, or is only known at run time — then the probe records `H_i = SAT`, with the flag
`whole-program` and the list of unresolvable facts, where

```
SAT = 1 + max over all probes and all ten languages of the largest bounded H_i measured in this run
```

`SAT` is computed **by script after all counting is complete**, applied uniformly to every
`SAT`-flagged cell, and published together with the maximum bounded chain that determined it and the
(probe, language) pair that produced that chain. Its provisional value for planning and for schema
examples is **6**. Nothing about the rule depends on which language produces the maximum, and the rule
is registered here, before any counting, which is what makes computing the constant afterwards
legitimate rather than post-hoc: the *rule* is frozen, only the arithmetic is deferred.

**Rule 2.7.2 (rationale, and why not 0).** Without this rule, a language whose facts are *unknowable*
would score **better** on locality than a language whose facts are merely *distant*, because the
analyst would stop looking and record a small number. That is a perverse incentive and would corrupt
the metric. `SAT` states the honest answer: the required context is not bounded by the probe's
declaration graph.

**Rule 2.7.3 (why the literal was replaced).** The pre-remediation text fixed `SAT = 6` as one more
than the deepest *anticipated* bounded chain (5), while Rule 2.7.4 simultaneously allowed a measured
bounded chain to exceed 5 and be recorded as measured. Those two rules together defeat Rule 2.7.2's
rationale: a probe whose context is genuinely unbounded would score 6 while a probe that is merely
distant would score 7 or 8 — the exact inversion the rule exists to prevent — and the inversion would
fall on the languages most likely to hit true unresolvability (rebindable names, dynamically installed
attributes, run-time-supplied targets) while sparing the languages most likely to produce deep bounded
chains. Defining `SAT` as one more than the largest bounded chain *actually measured* preserves
"unbounded always ranks strictly worse than bounded" under every measurement outcome, with no post-hoc
choice and no clipping.

**Rule 2.7.4 (chain cap).** No bounded chain is capped, clipped or winsorized. If a measurement
produces a bounded chain deeper than the corpus's declaration graph was expected to allow, the value is
recorded as measured and the register (§3.3) notes it. `SAT` is not a cap on bounded chains and is
never applied to one; under Rule 2.7.1 it is computed from them.

### 2.8 Fact-set rulings and the status of the worked examples

**Rule 2.8.0 (status: the pre-remediation `H` tables are withdrawn).** The pre-remediation §2.8.1 and
§2.8.2 stated per-language `H` values (C++ 3, Rust 3, Python 3, Go 2, Java 2, Swift 2, Zig 2 at
ARG-PLAIN; C++ 3, Rust 3, Go 3, Zig 3, Java 2, Python "2–3" at ARITH) as binding precedents. They are
withdrawn (§4) because they were computed:

* from fact sets stated in prose that do not match the corpus (`type_and_representation` added at
  `F05.P1`, `control_flow_effect` and `externally_visible_side_effects` dropped — Rule 2.1.1);
* over a counting unit that treats the operand's own binding as an external `VAR` hop, although the
  corpus places it **inside** `F05.P1`'s measured fragment, so that hop does not exist;
* over an `a + b` probe the corpus does not contain, with two operand declarations where `F07.P1` has
  three (Rule 1.5.2.b);
* and, in one cell, as a **range** ("2–3") inside a table the document called a binding precedent,
  which two analysts cannot reproduce identically.

**Rule 2.8.0.a (no ranges).** No cell of any worked table, and no raw record, may state a range for
`H_i` or `B_i`. The frozen fragment fixes the operand declarations and the conditional that produced
the pre-remediation range; where a genuine conditional remains, it is resolved by the probe record or
it goes to the register (§3.3), never left open in a published table.

Restoring the corpus's real fact sets changes every ARG-PLAIN `H`, because two of the restored facts
are not resolvable at class level from a bare call in any of the ten languages without either a body
hop or a whole-program finding. The two rulings below are therefore binding, are stated before any
counting, and pre-assign no answer to any language.

#### 2.8.1 ARG-PLAIN (`F05.P1`) — the frozen fact set and two binding rulings

`F_i` for `F05.P1`, read from the corpus, is exactly:

```
aliasing_writable_aliasing, mutability, allocation_copy_move_borrow_destroy,
value_vs_storage, control_flow_effect, externally_visible_side_effects
```

`type_and_representation` is **not** in this probe's set and no lookup for it is counted (Rule 2.4.2).
The operand's binding is **inside** the measured fragment and is never a `VAR` hop for this probe.

**Rule 2.8.1.a (`externally_visible_side_effects` at a call whose callee is outside the fragment).**
A language-blind three-way test, applied per language from that language's authoritative
specification, recorded with the clause, and signed by both analysts before any aggregate is computed:

1. **RESOLVED-BY-DECLARATION** — the language requires the callee's declaration to state, in a form the
   language checks, whether the call may perform effects observable outside the fragment. Reading that
   declaration (1 `SIG` hop) resolves the fact.
2. **REQUIRED-BODY** — the fact has no class until the callee's body is read. The body is then the
   smallest resolving entity and the `BODY` hop is **required**, on the P-3 logic; further callees are
   followed only as far as assigning a class requires (Rule 2.2.4, Rule 2.3.2).
3. **UNRESOLVABLE** — the set of bodies that can execute at the call is not bounded by any finite set of
   declarations reachable from the fragment (a rebindable name, a dynamically installed attribute, a
   reflective or run-time-supplied target). The probe then records `H_i = SAT` with the `whole-program`
   flag (Rule 2.7.1).

No language's answer is pre-assigned here, and a language does not escape the test because its
declarations are conventionally informative; the test is whether the language *requires and checks* the
statement.

**Rule 2.8.1.b (`control_flow_effect` at the same site).** The same three-way test, over control-flow
escape rather than effects. RESOLVED-BY-DECLARATION holds **only** where the language requires every
escape the fact covers — suspension, asynchrony, exception propagation, divergence, non-local exit — to
be announced in the callee's declaration and checks the announcement. A language that admits an
*undeclared* escape at a call — an unchecked exception, a panic, a raised run-time error, a non-local
jump — is not resolved by the declaration for that class of escape; the body is required, and outcome
(3) applies where the reachable bodies are unbounded. This is applied to all ten languages identically,
including to any language whose runtime raises errors that its declarations do not mention, and
including Quidra.

**Rule 2.8.1.c (re-derivation obligation).** With the restored fact set and these rulings, every
ARG-PLAIN `H` value is re-derived from scratch, per language, by the algorithm of §2.3 against the
language's real frozen fragment. The results are stored in the raw records (§2.11) and, for
calibration, in the sealed annex (§4). None of the withdrawn values may be reused, adjusted, or used as
a starting point.

#### 2.8.2 The numeric probes (`F07.P1`, `F07.P2`, `F08.P1`, `F08.P2`) — frozen fact sets

Read from the corpus, and binding:

| Probe | `semantic_facts_expected` |
|---|---|
| `F07.P1` (ARITH-EXPR) | `type_and_representation`, `overflow_exceptional_numeric`, `value_vs_storage`, `conversion_behavior` |
| `F07.P2` (ARITH-DIV) | `type_and_representation`, `conversion_behavior`, `overflow_exceptional_numeric`, `possible_failure`, `value_vs_storage` |
| `F08.P1` (OVERFLOW-SITE) | `overflow_exceptional_numeric`, `type_and_representation`, `possible_failure`, `control_flow_effect`, `conversion_behavior` |
| `F08.P2` (DIVZERO-SITE) | `overflow_exceptional_numeric`, `possible_failure`, `control_flow_effect`, `type_and_representation`, `alternative_value_cases` |

`possible_failure` is **not** in `F07.P1`'s set; `alternative_value_cases` is in `F08.P2`'s and in no
other of the four. `F07.P1` has **three** declared operands as given context, `F07.P2` and `F08.P2`
have two, and `F08.P1` declares its own `m` **inside** the fragment — so `m` is never a `VAR` hop for
`F08.P1`. Hop counts follow from these fragments and from the algorithm of §2.3; no per-language value
is stated here in advance.

**Rule 2.8.2.a (`G` hops at the numeric probes).** Where a probe's overflow, division or width fact
differs between a language's documented build or execution modes, the mode-selecting artifact is one
`G` hop (Rule 2.6.1, Rule 2.6.2.a) and an `R5` flag in Part 1 (§1.5.2), never a `+1` in both. Where the
canonical task directs the language to an operation that names its own discipline (`F08.P1`) or the
fragment contains an explicit guard (`F08.P2`), the fact is resolved **inside** the fragment and there
is no `G` hop for it — for every language that has such an operation, and only for the facts that
operation actually decides.

#### 2.8.3 A negative-example walkthrough (why the count stops)

Take any language at `F05.P1` where, after a `SIG` hop, the signature announces that the callee
receives a writable alias to the operand's contents. The fact `aliasing_writable_aliasing` now has a
class. An analyst who then opens the callee's body to see *which* elements it writes has made an
**optional** lookup (N-2) and must not count it. An analyst who then opens the operand type's
copy-versus-move conformance site, when the signature already establishes that a reference parameter
makes a move impossible, has also made an optional lookup: a hop that some *other* signature would have
required is not required under this one. The hop list is probe-specific and signature-specific, is
recomputed per probe and per language, and is never copied between probes (§3.4). Facts still without a
class after the loop terminates — typically `externally_visible_side_effects` and `control_flow_effect`
under Rule 2.8.1.a/b — do not stop the count: they are resolved by a required `BODY` hop or they make
the probe `SAT`, and they are never silently dropped.

### 2.9 Aggregation and normalization

**Raw aggregate (per language L):**

```
H_L = mean over the probes of the reporting basis, of H_i    # mean required lookups per probe
```

Lower is better. The reporting basis is fixed by Rule 2.9.4: the common basis for the primary figure,
the all-fragments basis for the published secondary. **No logarithm** is applied — §1.8.1's exception is exclusive to Semantic Determinacy.

**Rule 2.9.1 (normalization — family C, shifted form).** `H_L = 0` is legitimate in principle, so
§25.1.C's predeclared shifted form applies:

```
Score_L = 100 * (H_best + epsilon) / (H_L + epsilon),    epsilon = 1/N = 1/44
```

with `H_best` the smallest raw mean among the ten languages **on the basis being reported** (Rule 2.9.4:
the common basis for the primary figure, the all-fragments basis for the secondary figure — never a
`H_best` taken from one basis and an `H_L` from the other). `epsilon` is the same predeclared constant
as Rule 1.8.3: `1/N` with `N = 44`, the frozen corpus size, fixed before any measurement and **not**
recomputed per language where a language's applicable probe count is smaller.

**Rule 2.9.2 (mandatory reporting).** Publish per language: every `H_i` with its ordered hop list and
kinds, `H_L`, `|G|` totals, the count of `whole-program`-flagged probes, and the ratio `H_L / H_best`.
If the applicable `H_L` span a factor of 100 or more, publish the §25.1 compression note as well.

**Rule 2.9.3 (weight).** Semantic Locality carries 0.20 of `Q` (§6.1.6). Unchanged by this document.

**Rule 2.9.4 (the common basis is PRIMARY here too).** The exact counterpart of Rule 1.8.6, for the
same reason and under the same predeclared corpus rule: `H_L` taken over each language's own applicable
probes is a mean over a probe set that differs per language, so `H_best` — which sets the scale for all
ten languages under family-C normalization — could otherwise be produced by whichever language has the
smallest and easiest applicable set. With `COMMON` defined exactly as in Rule 1.8.6 (the probes for
which all ten languages have a fragment, `FULL` or `PARTIAL`):

```
H_L         = mean over i in COMMON of H_i              # PRIMARY — reported as the result
H_L^allfrag = mean over i applicable to L of H_i        # SECONDARY — published beside it
```

Publish both, with `|COMMON|`, each language's `n_L`, the per-metric delta, and the family-C score
computed from each, for every language. Every figure states its basis and its basis size, and
`H_best` is always taken from the same basis as the `H_L` it normalizes. Where the two bases disagree
about a ranking, both rankings are published and the common-basis ranking is reported as the result.
`COMMON` is computed once, from the same fragment-existence table, for both metrics. Neither basis may
be selected after seeing which flatters a language (§3.4).

### 2.10 Missing-capability policy for `H_i`

Identical in structure to §1.9, with the same vocabulary: capability absences are
`support_level: NONE` with a `none_reason_code`, never `N/A` (doc 01 `na_policy` NA-6), and the one
`N/A` that NA-4 permits carries `na_policy_rule: "NA-4"` and a written justification. In addition:

* `NONE` probes are excluded from `H_L` and charged in full against `C`, with their full
  `capability_points` remaining in the fixed denominator of 88.
* A probe whose facts are unresolvable is **not** `NONE` and **not** `N/A`; it is `H_i = SAT` with the
  `whole-program` flag (Rule 2.7.1). Recording it as either instead is forbidden, because that would
  let non-locality escape scoring (§26).
* A probe scored `PARTIAL` is enumerated normally: the workaround's own measured fragment is the
  counting unit.
* An absence is never converted to `H = 0`, never converted to `SAT` automatically, and never used to
  remove a probe.

### 2.11 Raw record schema (locality)

One JSON file per language at `semantic-compression/raw/locality_<language>.json`:

```json
{
  "language": "go",
  "toolchain": "go 1.26.3",
  "recipe_entries_admitted": ["go build -o BIN FILE.go", "./BIN"],
  "recipe_agreement_check": "<path to the V5 record showing this matches environment.json byte for byte>",
  "methodology_doc": "methodology/03_determinacy_and_locality.md",
  "corpus_doc": "methodology/01_capability_universe_and_probes.json",
  "probes": [
    {
      "probe_id": "F05.P1",
      "probe_role": "ARG-PLAIN",
      "fact_set": ["aliasing_writable_aliasing", "mutability",
                   "allocation_copy_move_borrow_destroy", "value_vs_storage",
                   "control_flow_effect", "externally_visible_side_effects"],
      "resolved_locally": [
        {"fact": "value_vs_storage", "basis": "<specification clause, cited>"}
      ],
      "resolved_inside_fragment": [
        {"fact": "<a fact in this probe's fact_set that text the corpus places INSIDE the measured fragment resolves>",
         "basis": "<the fragment text that resolves it, quoted>"}
      ],
      "facts_out_of_scope_not_counted": ["<any fact an analyst looked for that is NOT in this probe's fact_set; recorded for audit, never counted (Rule 2.4.2)>"],
      "hops": [
        {"n": 1, "kind": "SIG", "entity": "main.f", "site": "probe.go:3",
         "facts_resolved": ["mutability", "allocation_copy_move_borrow_destroy"]}
      ],
      "effect_ruling": "RESOLVED-BY-DECLARATION | REQUIRED-BODY | UNRESOLVABLE",
      "effect_ruling_citation": "<specification clause supporting the ruling, Rule 2.8.1.a>",
      "control_flow_ruling": "RESOLVED-BY-DECLARATION | REQUIRED-BODY | UNRESOLVABLE",
      "control_flow_ruling_citation": "<specification clause, Rule 2.8.1.b>",
      "overload_candidate_set_capture": "<path to the tool output, Rule 2.2.2.a>",
      "rejected_lookups": [
        {"entity": "main.f body", "reason": "optional per N-1: class already settled by the signature"}
      ],
      "H": 0, "G_hops": 0, "flags": [],
      "analyst_a": "...", "analyst_b": "...", "reconciled": true,
      "support_level": "FULL",
      "none_reason_code": null,
      "na_policy_rule": null
    }
  ],
  "H_core_mean": 0.0,
  "H_core_basis": "common",
  "core_probe_ids": [],
  "core_basis_size": 0,
  "H_allfrag_mean": 0.0,
  "H_allfrag_basis": "all-fragments",
  "applicable_probe_count": 0,
  "n_L": 0,
  "corpus_probe_count": 44,
  "G_hop_total": 0,
  "whole_program_flagged_probes": [],
  "sat_value_applied": null
}
```

Every field is mandatory, and `H_core_mean` — the **common-basis** figure — is the one reported as the
result (Rule 2.9.4). Validation fails if a `probe_id` is not a corpus probe id; a `fact_set` member is
not one of the 13 `semantic_fact_kinds` ids or differs from that probe's frozen
`semantic_facts_expected`; a member of `resolved_locally` or `resolved_inside_fragment` is not a member
of `fact_set`; a hop resolves no fact; a `SIG`/`BODY` ruling is recorded without its citation; an `H` or
`G` value is a range rather than an integer; a published figure carries no basis label; or
`sat_value_applied` is set on a record with no `whole-program` flag. The numeric values above are placeholders; this
document states no expected value for any language.

---

## 3. Process controls

### 3.1 Order of operations (binding)

1. This document is frozen (now), before any `B_i` or `H_i` is produced. No `B_i`, `H_i`, aggregate or
   score existed for any language, including Quidra, when the corrections of the remediation changelog
   were applied.
2. The probe corpus, fact annotations and support rubric are frozen; this document reads probe ids,
   measured fragments and fact sets from the corpus and never restates them.
2a. Doc 01's pre-measurement gates are satisfied and their evidence preserved **before** any count is
   recorded — in particular **V5**, the byte agreement between `toolchain_binding.recipes` and
   `environment/environment.json`, which Rule 1.3.3 depends on and which the corpus records as failing
   by construction until the counterpart change to the environment document is applied. No witness may
   be built while V5 fails.
2b. The common basis is computed from the fragment-existence table, once, for both metrics, and
   `|COMMON|` and every `n_L` are fixed before any aggregate is taken (Rules 1.8.6, 2.9.4).
3. Witnesses and refutation witnesses are built and run under the frozen recipes, at the recipe entries
   Rule 1.3.3 admits, and stored before any count is recorded.
4. Counts are produced language by language, in the fixed column order, with the running aggregate and
   every other language's records hidden from the analysts until all ten are complete, and with the
   sealed calibration annex (§4) withheld from an analyst until that analyst's own enumeration for the
   probe and language is recorded and signed.
5. `SAT` is computed by script from the completed bounded measurements (Rule 2.7.1) and applied
   uniformly.
6. Aggregation and normalization — the primary common-basis figures and the secondary all-fragments
   figures (Rules 1.8.6, 2.9.4) — are executed by script from the raw JSON, each labelled with its
   basis and basis size. No hand-entered score.

### 3.2 Two-analyst reconciliation

**Rule 3.2.0 (what an analyst is).** An **analyst** is one measuring agent instance given only: the
frozen methodology documents, the frozen probe corpus, the language's authoritative specification, and
the frozen toolchain with its documented modes. An analyst has **no** access to the other analyst's
records, to any previously counted language's records, to the running aggregate, or to the sealed
calibration annex of §4 before their own enumeration for that probe and language is recorded and
signed. Analyst A and analyst B run in **separate sessions**. Where the analysts are agent instances
rather than people, this separation is what independence means operationally; two instances sharing a
context are one analyst, not two, and a reconciliation between them is not a control.

Each probe × language is counted independently by two analysts. Both records are preserved. On
disagreement:

1. Compare witness sets. A branch without a witness is dropped (Rule 1.3.1). An exclusion without a
   specification citation and a refutation witness is invalid and the class is re-enumerated
   (Rule 1.3.1.e). A hop that step 9 cannot justify is dropped (Rule 2.4.1).
2. If both analysts hold witnesses for classes the other omitted, the union is taken — provided every
   member survives the observational-equivalence merge (Rule 1.4.5).
3. If the disagreement is about a *rule*, not a fact, it goes to the adjudication register (§3.3).
4. The reconciled value, both original values, and the reason are all recorded.
5. Any `B_i > 16`, any `H_i = SAT`, and every `LOW-B-BY-ABSENCE` flag additionally require both
   analysts' explicit sign-off.

**Rule 3.2.1 (the union rule is monotone in effort, and is bounded by the tables).** Because step 2
takes the union, `B` grows with how hard an analyst looks. That is bounded on three sides and the
bounds are what make it reproducible: the outcome tables of §1.5 are **closed**, so there is a finite
checklist to exhaust; Rule 1.7.1 step 4 requires **every** class in the applicable table to be
dispositioned as either witnessed or refuted, so an unexamined class cannot be silently omitted; and an
exclusion now costs an artefact (Rule 1.3.1.e), so the cheap direction is no longer "do not look". An
analyst who dispositions the whole table has looked exactly as hard as the procedure requires.

### 3.3 Adjudication register

**Rule 3.3.0 (scope, and its relationship to §3.4).** §3.3 governs **genuinely new cases**: a situation
no existing axis, class or ruling covers. §3.4 bullet 1 governs **revision of existing rulings and
constants**, which remains prohibited once any language has been counted. The two sections do not
conflict once that line is drawn, and the line is drawn here because the same lever — a ruling written
after counting has begun, by an analyst who has already seen at least one language's enumeration, and
in the fixed column order the first language counted is Quidra — is the one thing that could move
results after the fact.

**Rule 3.3.1 (discipline for rulings written after counting has begun).** Every adjudication ruling
created after counting has begun must be:

* **(a)** written and signed **before its numeric effect is computed** — the ruling's text is committed
  before any `B_i` or `H_i` is recomputed under it;
* **(b)** published with a **before/after table of `B_i` and `H_i` for all ten languages at every probe
  it touches**, per §25.4's dual-publication discipline;
* **(c)** listed in the final report, with its timestamp; and
* **(d)** recorded with **which languages had already been counted when it was written**, and by which
  analyst.

A ruling may never be written in a way that names a language in its condition — and, because that
constrains wording rather than tailoring, (a)–(d) are what actually constrain tailoring. Rulings are
applied **retroactively to all ten languages**, including re-counting already-counted languages; a
ruling applied to fewer than ten is invalid.

**Rule 3.3.2 (seeds).** `AR-001`…`AR-009` below are **pre-run seeds**: every one of them was written
before any language was counted — `AR-008` and `AR-009` during remediation, at a point when no `B_i` or
`H_i` existed for any language — and they are therefore exempt from Rule 3.3.1(b) and (d). They remain subject to §3.4 bullet 1 — once
counting begins they may not be revised, only supplemented by new rulings under Rule 3.3.1.

Seed entries, fixed now:

| Id | Ruling |
|---|---|
| `AR-001` | A conforming copy constructor / copy hook that mutates its source makes P3 realizable. Exotic but specification-conforming completions count; UB-dependent ones do not (Rule 1.3.1(b)). |
| `AR-002` | Deferred *argument* evaluation (`@autoclosure`, by-name parameters) is not material when the probe's argument is a plain variable reference, because the deferral is unobservable. It is material where the probe's argument is an expression with effects. |
| `AR-003` | A call that begins executing the callee and may suspend (Kotlin `suspend`, an `async` function whose body runs to the first suspension point) is **not** P5. P5 requires that the body does not begin executing at the span. |
| `AR-004` | Coercions that retarget a reference/view type without constructing a new owned object (Rust deref/unsize coercion) are P1, not P2. Conversions that construct a value of a different type (boxing, existential wrapping, varargs/rest packing, optional promotion, widening) are P2. |
| `AR-005` | Varargs / rest-parameter packing at a call site is a P2 converting copy in every language that has it, because a new aggregate is constructed at the span. |
| `AR-006` | Subtype widening with no representation change (passing a subclass where a superclass is expected, in a language where that is a reference conversion only) is **not** P2. |
| `AR-007` | A `G` hop is charged once per distinct configuration artifact per probe, even when several facts depend on the same artifact. |
| `AR-008` | Where the corpus's `canonical_task` directs a language to an operation that names its own discipline (`F08.P1`) or to a fragment that contains an explicit guard (`F08.P2`), that operation or guard **is** the fragment, and classes reachable only by the plain operator are not realizable for that language at that probe. Conversely, a language with no such operation enumerates its plain operator's classes in full. |
| `AR-009` | A class excluded because **no program in the language** can realize it is `LOW-B-BY-ABSENCE` (Rule 1.9.5); a class excluded because no program **whose measured fragment is this probe's fragment** can realize it, while some other program in the language can, is an ordinary exclusion. Both require a refutation witness. |

### 3.4 Prohibited operations on these two metrics

Restating §24, §25.1 and §32 in metric-specific form. Do not:

* change any axis, outcome class, existing ruling, `epsilon`, or the `SAT` **rule** after any language
  has been counted (the `SAT` *value* is computed from the completed measurements by the pre-registered
  rule of Rule 2.7.1, which is not a change to the rule);
* write a new adjudication ruling without Rule 3.3.1(a)–(d);
* apply a logarithm to Semantic Locality, or to Semantic Determinacy anywhere other than the
  pre-registered `mean(log2(B_i))` aggregate;
* switch normalization families to decompress family C's hyperbola;
* winsorize, clip, min-max, or percentile-scale either aggregate;
* select between the common-basis and the all-fragments aggregate after seeing either, or report a
  figure without its basis and basis size;
* combine a `D_best` or `H_best` from one basis with a `D_L` or `H_L` from the other;
* exclude `PARTIAL` fragments from the common basis, for any language, for any reason;
* build or run a witness under a mode the frozen recipe does not list, or exclude a class by appealing
  to one;
* recompute `epsilon` from a language's applicable probe count;
* drop a probe, a branch, or a hop because of the ranking it produces;
* record a branch without a witness, an exclusion without a specification citation **and** a refutation
  witness, or a hop without a resolved fact;
* record a capability absence as `N/A` where doc 01 `na_policy` NA-6 requires `NONE`, or convert either
  to 0, to `B = 1`, or to `H = 0`;
* state a `B_i` or `H_i` as a range;
* enumerate against a remembered, idealized or invented fragment instead of the corpus's
  `measured_fragment` (Rule 0.1.1, Rule 1.5.2.b);
* count the same language property twice inside Semantic Determinacy (Rule 1.4.2) or twice inside
  Semantic Locality (Rule 2.2.2), or charge the same property in both a `+1` and a flag on the same
  side (Rule 2.6.4);
* reuse a hop list or a branch enumeration between probes without recomputing it (§2.8.3).

### 3.5 Deliverable checklist for the measuring agent

* [ ] `semantic-compression/raw/determinacy_<lang>.json` for all 10 languages, schema §1.10
* [ ] `semantic-compression/raw/locality_<lang>.json` for all 10 languages, schema §2.11
* [ ] `semantic-compression/probes/witnesses/<probe_id>/<lang>/<class>/` for every counted branch, with
      `<probe_id>` in the corpus's form (`F05.P1`), each recording the recipe entry it used
* [ ] a refutation witness and a specification citation for **every** excluded class, stored alongside
* [ ] the captured overload candidate set (Rule 2.2.2.a) for every probe where an overload set is a hop
* [ ] the per-language `effect_ruling` and `control_flow_ruling` (Rules 2.8.1.a, 2.8.1.b) with citations,
      signed by both analysts before any aggregate is computed
* [ ] `semantic-compression/adjudication_register.json`, seeded with `AR-001`…`AR-009`, each later
      ruling carrying Rule 3.3.1(a)–(d)
* [ ] the recorded V5 recipe-agreement check, passing, dated before the first witness was built
* [ ] `semantic-compression/scores/determinacy.json` and `locality.json`, produced by script from the
      raw files, containing the **primary common-basis aggregates with `|COMMON|`**, the secondary
      all-fragments aggregates with each `n_L` and the per-metric delta, **every figure labelled with
      its basis and basis size**, ratios to best computed within a basis, normalized family-C scores,
      `epsilon = 1/44`, the computed `SAT` with the bounded chain that determined it, the
      `LOW-B-BY-ABSENCE` counts and the absence-recomputed `D_L`, and the §25.1 compression note where
      the span requires it
* [ ] where the common-basis and all-fragments rankings disagree, **both rankings published**, with the
      common-basis ranking reported as the result
* [ ] every `NONE` carrying a reason code and a justification; every `N/A` additionally carrying the
      `na_policy` rule that permits it
* [ ] both analysts' pre-reconciliation records preserved, from separate sessions (Rule 3.2.0)
* [ ] the sealed calibration annex (§4), populated only with witness-backed enumerations

---

## 4. Sealed calibration annex, and the withdrawn enumerations

### 4.1 What the annex is

Calibration enumerations let an analyst check their reading of the procedure against a worked case.
Published inside the frozen text, they also tell every analyst the number to beat before counting
begins, and in the fixed column order the language counted first benefits most from that. The annex
therefore exists, but it is **sealed**:

* it contains only enumerations produced against the corpus's real `measured_fragment` for a named
  probe id, with stored witnesses and refutation witnesses;
* it is released to an analyst **only after** that analyst's own enumeration for that probe and
  language has been recorded and signed (Rule 1.7.2, Rule 3.2.0);
* it contains no summary table of per-language values, and no statement of which language is best or
  worst on either metric;
* while it is empty, its emptiness is not a licence to enumerate from memory.

The annex lives at `semantic-compression/calibration_annex/` and its release events are logged.

### 4.2 The withdrawn enumerations

The following values appeared in the pre-remediation text of this document as **binding precedents** a
measuring agent was required to reproduce. They were never produced by a witnessed enumeration against
a frozen fragment, and every one of them was computed against a counting unit, an operand, or an
expression the frozen corpus does not contain. They are withdrawn in full, are not replaced by
estimates, and may not be used as starting points:

* ARG-PLAIN `B`: C++ 11, Python 3, Rust 4, Go 4, Java 4, Kotlin 4, Zig 4, TypeScript 5, Swift 6, and
  the summary table ranking them.
* ARG-EXPLICIT `B` (the glyph-neutrality table): Rust 3, Zig 2, Go 2, Swift 4, C++ 6.
* ARITH `B`: C++ 7, Python 4, Rust 4, Go 3, Zig 4.
* ARG-PLAIN `H`: C++ 3, Rust 3, Python 3, Go 2, Java 2, Swift 2, Zig 2.
* ARITH `H`: C++ 3, Rust 3, Go 3, Zig 3, Java 2, Python "2–3".
* The derived claims that depended on them: that Python has the lowest ARG-PLAIN branching count of the
  nine non-Quidra languages, that C++ has the highest, and the §0.3 and §2.8 illustrations of metric
  dissociation built on those two facts.

The pre-remediation text of this document is retained at
`methodology/_pre_remediation_snapshot/03_determinacy_and_locality.md` for audit.

### 4.3 Counterpart obligations on sibling frozen documents

These are recorded here so that the owners of the sibling documents make the matching change. Nothing
in this section changes a sibling document; each is the responsibility of its own owner.

| Document | Change required |
|---|---|
| `01_capability_universe_and_probes.json` | Confirm — and, where a probe needs one, add — a machine-readable `counting_site` (or an equivalent explicit statement) for every probe whose `SCORING NOTE` narrows the site at which `B` is counted, so Rule 1.1.1 can be applied by script rather than by reading prose. `F05.P1` already states it in prose; the other 43 probes must either state one or be explicitly whole-fragment. No probe, fragment, operand or fact set may be changed to accommodate this document. |
| `01_capability_universe_and_probes.json` (discharged here) | Doc 01's `counterpart_obligations` requires this document to confirm that metrics B and C are computed on the **common basis as primary** with the all-fragments basis as a published secondary, that `PARTIAL` fragments are included, and that every published figure states its basis and basis size. **Confirmed and implemented** in Rules 1.8.6 and 2.9.4, §0.2 item 9, both schemas and §3.4. Doc 01 also requires confirmation that **`B` is the sole home of local determinacy**, rubric criterion F-5 having been deleted there: **confirmed** — local determinacy is counted once, in Part 1 of this document, and no other metric in this benchmark may re-charge it (§1.4.2, Rule 2.6.4, §3.4). |
| `04_hidden_cost_and_capability_efficiency.md` | Accept and charge the two cross-charges this document emits: `UNOBSERVABLE-RESOURCE-EVENT` (Rule 1.2.2.b — a dimension-6 event excluded here as unobservable, which §6.1.4.D charges as unsignalled implicit allocation/copy/move/destruction/cleanup), and the A8 resource dimensions and A9/A7 effects that Rule 1.4.2 removes from `B`'s scope at `F05.P1`/`F05.P2`. If doc 04 does not charge them, they must be brought back in scope here rather than left uncharged. |
| `02_fact_taxonomy_and_density.md` | Use the corpus's 13 snake_case `semantic_fact_kinds` ids as the sole fact vocabulary and join key, so that fact sets join across documents 01–04 without translation. |
| `environment/environment.json` | **Blocking.** Apply the recipe changes doc 01 `counterpart_obligations` already names — `rustc -O -C debug-assertions=on`, `zig build-exe -OReleaseSafe`, `tsc --strict --target es2022 --module nodenext`, plus the `multi_unit_recipes` — so that `semantic_compression_recipes` matches `toolchain_binding.recipes` byte for byte and gate **V5 passes**. Until it does, Rule 1.3.3 has no admissible mode list, no witness may be built, and no `B_i` or `H_i` may be recorded. Carry the `safety_mode_clause` rationale (or reference it) so the phrase "every language uses its normal optimized/release build" cannot be read as authorising an unchecked mode for some languages and a checked mode for others. |
| `05_standard_rubrics.json` | Confirm that rubric criterion **F-5** (local determinacy) is deleted, since this document is the sole home of local determinacy. If any rubric criterion still scores determinacy, one of the two must be removed; it may not be charged in both. |
| scoring scripts | Emit the **primary common-basis** aggregates with `|COMMON|`, the **secondary all-fragments** aggregates with each `n_L` and the per-metric delta, every figure labelled with its basis and basis size, both rankings where they disagree, `epsilon = 1/44`, the computed `SAT` and the bounded chain that determined it (Rule 2.7.1), and the `LOW-B-BY-ABSENCE` counts with the absence-recomputed `D_L` (Rule 1.9.5). |

---

## Remediation changelog

**Status of the run when these corrections were applied: no results had been observed.** No `B_i`, no
`H_i`, no aggregate, no normalized score and no ranking had been computed for any language, including
Quidra, from this document or from any sibling document. This is confirmed independently by the frozen
corpus's own `no_results_observed_at_remediation` field: only Quidra and Python probe *fragments*
existed, and no metric had been run over them. Correcting the methodology at this point is
pre-registration, not post-result formula selection (§24, §32). Every correction below is applied in
the direction the audit specified; none is resolved by weakening the finding, by declaring it out of
scope, by adding a caveat while keeping the rule, or by adding a compensating bias in the other
direction. No finding was rejected.

Every change was checked against the symmetry test: would it look equally reasonable if Quidra were
replaced by Zig, or by Python? Each rule below is stated so that any of the ten languages could satisfy
or fail it, and no rule's condition names a language.

| # | Severity | Defect | Change | Direction for Quidra's expected score |
|---|---|---|---|---|
| 1 | BLOCKER | The document was built on an invented "focus span" the frozen corpus does not contain, and declared the operand's own declaration external although the corpus's `measured_fragment` (authoring rule R5) places it **inside** the unit. Two frozen documents specified different scoring units for the same metric. | Rules 1.1.1, 1.1.2 and 1.1.2.a rebind the counting unit to the corpus's `measured_fragment`, with the `SCORING NOTE`'s sub-expression as the counting site; §0.1 binds every role name to a probe id; Rule 0.1.1 forbids any rule or example stated over a program the corpus does not contain; any narrower unit now requires amending doc 01 and dual publication under §25.4. | Not language-specific in itself. It closes outcome classes for every language whose fragment pins the operand, and it invalidates the precedents, so no language keeps an advantage the old unit gave it. |
| 2 | BLOCKER | The binding locality examples used hand-written fact sets contradicting the corpus: `type_and_representation` invented at `F05.P1`, `control_flow_effect` and `externally_visible_side_effects` dropped, `F07.P1`'s set mis-stated. | Rule 2.1.1 makes `F_i` a programmatic read of `semantic_facts_expected` and forbids hand-stated fact sets; §2.8.1 and §2.8.2 restate the corpus's real sets; Rules 2.8.1.a and 2.8.1.b give the language-blind three-way ruling (RESOLVED-BY-DECLARATION / REQUIRED-BODY / UNRESOLVABLE→`SAT`) for the two restored facts, with no language's answer pre-assigned; Rule 2.8.1.c requires every ARG-PLAIN `H` to be re-derived. | Restoring two facts raises `H` for every language that cannot resolve them from a declaration. Quidra is subject to the identical test, and Rule 2.8.1.b explicitly covers a language whose runtime raises errors its declarations do not mention — so this is expected to **lower** Quidra's locality score rather than spare it. |
| 3 | BLOCKER | The ARITH probe the document scored (`a + b`, two operands) does not exist in the corpus; its five "binding precedents" were unreproducible, and its Zig ruling contradicted `F08.P1`'s canonical task. | §1.5.2 is re-anchored as **Table NUM** over the four probes that exist (`F07.P1`, `F07.P2`, `F08.P1`, `F08.P2`), with classes R10–R12 added for rounding/remainder sign, value-returning exceptional cases, and discipline-naming operations, and per-probe rulings in Rule 1.5.2.a. The sentence excluding Zig's R2 because "wrapping requires the distinct `+%` operator inside the span" is **deleted**; `AR-008` states that the explicit operation *is* the fragment for every language that has one. All five `B` values and all the `H` values are withdrawn (§4.2). | **Lowers** Quidra's relative position. Languages with explicit discipline-naming arithmetic (`+%`, `wrapping_add`, `checked_div`, `&+`, …) now close classes at the site instead of being charged for the plain operator's openness, which removes an advantage the old ruling handed to a language whose arithmetic has only one discipline. |
| 4 | BLOCKER | Table ARG structurally could not count an outcome class the corpus declares mandatory for `F05.P1` — "callee may retain the alias beyond the call" — and Rule 1.4.2 removed the axis where it would live. | Table ARG gains **Axis E** (E0/E1, escape/retention) and splits M2 into M2 (element writes) and M3 (extent/identity change, i.e. replace/resize), with entailments in Rule 1.5.1.c and an audit mapping of all six mandated outcomes in Rule 1.5.1.d. Rule 1.5.0 states the precedence: a mandated outcome with no mapping is a defect in this document, not in the probe. | **Raises** Quidra's expected score relative to the uncorrected text, and the audit says so: languages that permit an escaping alias now realize a class that ownership-checked languages can exclude. It is applied because it is a correctness defect, and the direction is recorded rather than traded against. |
| 5 | BLOCKER | Every ARG-PLAIN witness used an operand type `F05.P1` forbids, and the ARG-EXPLICIT precedents were enumerated over a bare call although `F05.P2` puts `put`'s declaration and body inside the fragment. | Rule 1.1.1 states the pinned operand and the `F05.P2` boundary explicitly; Rule 1.7.0 withdraws all the precedent values with the three independent reasons; Rule 1.7.1 replaces them with the enumeration procedure; Rule 1.7.2 seals calibration; §4.2 lists every withdrawn number. §1.7 is now explicitly non-binding on any value until witness-backed enumerations exist (spec §4 forbids a score supported only by general language knowledge). | Neutral by construction — no number survives for any language. It removes a set of incumbent numbers that had never been executed, which means Quidra will be compared against measured values rather than against unverified ones. |
| 6 | MAJOR (Quidra bias) | §1.7.3(4)'s central anti-bias claim was false: `C` charges an absence that makes a probe unexpressible, but nothing charges an absence that merely deletes an outcome class from a probe the language can express. R2, R7, R8, P6 and P5 exclusions lower `B` for free, and the cross-language constraints document records Quidra on the favourable side of exactly this (overflow-checked arithmetic; wraparound not expressible). | §1.7.3(4) rewritten to state the gap accurately; **Rule 1.9.5** adds the `LOW-B-BY-ABSENCE` flag, requiring the excluded class id and the missing construct to be named, the per-language count to be published, and a **recomputed `D_L` with those classes counted as realizable** to be published beside the primary `D_L`. `AR-009` makes the absence-versus-fragment distinction mechanical. §0.2 item 6 restates it as a fairness commitment. | **Lowers** Quidra's expected standing as reported. The primary score is unchanged by construction, but the disclosure exposes how much of any determinacy advantage is bought by absence, and Quidra is documented to sit on the favourable side of this channel. |
| 7 | MAJOR | Strong positive control, no negative control: counted branches needed a built, run, observed witness; excluded classes needed only free text. Combined with Rule 1.1.4's zero-cost specification knowledge, a language whose reference is authored by the party running the benchmark could close branches by assertion while the nine incumbents' openness was demonstrated by construction. | **Rule 1.3.1.e** requires every `excluded_classes` entry to carry both a `spec_citation` to a numbered clause published before the run and a `refutation_witness_path` with the recorded rejection or observer output; a record with neither, or only one, fails validation. Rule 1.1.4 is amended to say a specification statement is a citation, not evidence. The §1.10 schema enforces it. Applied identically to all ten languages. | **Lowers** Quidra's expected score. Quidra has the most branch closures resting on a first-party reference document; every one must now survive an executed refutation attempt. |
| 8 | MAJOR | §3.4 forbade changing rulings after counting began; §3.3 anticipated exactly that and applied new rulings retroactively. In the fixed column order Quidra is counted first, so any post-start ruling is written by an analyst who has already seen Quidra's enumeration. | Rules 3.3.0–3.3.2 reconcile the sections: §3.3 governs genuinely new cases, §3.4 bullet 1 governs revision and stays prohibited. Every post-start ruling must be (a) written and signed before its numeric effect is computed, (b) published with a before/after table of `B_i` and `H_i` for all ten languages at every probe it touches (§25.4), (c) listed in the final report, and (d) recorded with which languages had already been counted and by which analyst. `AR-001`…`AR-007` are declared pre-run seeds and remain unrevisable. | **Lowers** the opportunity for Quidra's score to move after the fact; removes a lever that favoured whichever language is counted first, which is Quidra. |
| 9 | MAJOR | Rules 1.4.1 and 1.4.2 assigned different in-scope axis sets to the same mandatory probe with no precedence, and 1.4.2 was silent on A7 and A8, which the mapping also brings in. | Rule 1.4.2 now states precedence explicitly (a §1.5 table's axis set governs and overrides the §1.4.1 mapping for that probe), lists the excluded axes exhaustively, and adds a table naming, for every fact in `F05.P1`'s and `F05.P2`'s frozen sets, **the metric that charges it instead**. Any excluded axis whose fact is in the probe's frozen set must name its charging metric or be brought back in scope. | Neutral. It removes an ambiguity that could be resolved either way by an analyst, in either language's favour. |
| 10 | MAJOR | `D_L` and `H_L` are means over per-language probe sets, so family-C normalization compares non-comparable raws and `D_best` may come from the language with the easiest applicable set; `epsilon`'s derivation was wrong for exactly those languages. | Rules **1.8.6** and **2.9.4** add the mandatory common-core aggregates `D_L^core`, `H_L^core` over `CORE` (probes at which no language records an exclusion), published with `|CORE|` and their family-C scores beside the primary ones, with an explicit report statement wherever primary and CORE ranks differ by more than one position. Rule 1.8.3 is corrected: `epsilon = 1/N`, `N = 40` (the frozen corpus size), never recomputed per language. §3.4 forbids selecting between the aggregates after seeing either. | Direction depends on Quidra's own exclusion count, which is unknown. If Quidra expresses more probes than average its CORE score is unaffected; if it expresses fewer, the CORE aggregate will **lower** its reported standing. Either way the exclusion channel becomes visible. |
| 11 | MAJOR | `SAT = 6` defeated its own rationale: Rule 2.7.4 allows a measured bounded chain longer than 5, so an unbounded probe could score 6 while a merely distant one scored 7 or 8 — inverting exactly the ordering the rule exists to enforce, and doing so against the languages most likely to produce deep bounded chains. | Rule 2.7.1 predeclares `SAT` as a **rule** — `1 + max bounded H_i measured across all probes and all ten languages` — computed by script after counting, applied uniformly, published with the chain that determined it, with 6 as a planning value only. Rule 2.7.3 records why the literal was replaced; Rule 2.7.4's no-clipping rule is kept and strengthened. | Neutral across languages; it fixes an ordering inversion that fell on the incumbents. Legal because the rule is registered before any counting and only its arithmetic is deferred. |
| 12 | MAJOR | The overload-set and wildcard-import hop rules were not mechanical, and the ambiguity fell almost entirely on five of the ten languages ("compatible arity", "visible at the span", "must be examined", and an undefined scan order for `k`). | **Rule 2.2.2.a** makes the overload candidate set a captured **tool output** of the frozen toolchain (AST dump / verbose diagnostics / language service), stored with the probe record, with `H` counting distinct declaration sites in that capture and no analyst constructing the set by reading. **Rule 2.2.2.b** fixes `k` as the number of wildcard-imported modules the language's name-resolution rules require to be examined to establish unambiguity, independent of scan order. The §2.11 schema stores the capture path. | Neutral in expectation; it removes discretion that could have been exercised in any direction, and it applies to Quidra's toolchain on the same terms. |
| 13 | MINOR (Quidra bias, disclosure) | Rule 1.2.2's unobservability carve-out is the single rule that disposes of the one documented source of openness at a bare Quidra call site (the architecture reference's "unobservable optimization of ordinary value passing"), and the document did not disclose it or require any evidence before accepting an "unobservable" exclusion. | **Rule 1.2.2.a** requires a refutation witness for every unobservability exclusion and states in terms that a vendor or specification statement is a citation, not evidence; the Quidra instance is disclosed by name alongside the C++, Go, Java and Swift instances. **Rule 1.2.2.b** adds the `UNOBSERVABLE-RESOURCE-EVENT` cross-charge to methodology 04 under §6.1.4.D, so the behaviour is not free. §0.2 item 1 discloses the rule rather than claiming unqualified Quidra-independence. | **Lowers** Quidra's expected score: the exclusion must now survive an attempted observer, and the event is charged at Hidden Semantic Cost instead of vanishing. |
| 14 | MINOR | Machine-level mismatches that would make a conformant record fail validation or mis-join: probe ids `SC-F05-P1` vs the corpus's `F05.P1`; prose fact names vs the 13 snake_case ids; `NA-*` codes where doc 01 `na_policy` NA-6 requires `NONE` with a justification and NA-5 requires every `N/A` to name its permitting rule; and `F05.P2` described as one capability point of family 4 when it is 3 points of family 5. | All probe ids and witness paths use the corpus form `F05.P1`; Rule 1.4.1's mapping table and both schemas use the 13 snake_case ids; §1.9 and §2.10 are rewritten around `support_level: NONE` + `none_reason_code` (`UNSUPPORTED` / `NO_EXPLICIT_ALIAS_FORM`) with `na_policy_rule: "NA-4"` reserved for the one `N/A` NA-4 allows; Rule 1.7.4 corrected to "0 for all 3 capability points of `F05.P2` in family 5", with the points remaining in `C`'s fixed denominator of 103. | Neutral; audit-trail and tooling correctness. The substantive treatment (excluded from the mean, charged in full against `C`, never converted to 0 or `B = 1`) was already right and is unchanged. |
| 15 | MINOR | Three reproducibility gaps: a range ("2–3") inside a binding table; "two analysts" never defined, so two instances sharing a context could pass as independent while the union rule makes `B` monotone in effort; and §3.1 item 4's hidden-aggregate requirement contradicted by §1.7 publishing nine languages' values inside the frozen text, which the analyst counting Quidra first would read. | Rule 2.8.0.a forbids ranges anywhere; **Rule 3.2.0** defines an analyst (one measuring agent instance, separate sessions, no access to the other analyst's records, to previously counted languages, to the running aggregate, or to the sealed annex before its own enumeration is signed) and **Rule 3.2.1** bounds the union rule on three sides (closed tables, every class dispositioned, exclusions now cost an artefact); §4 seals the calibration annex and §3.1 item 4 requires it to be withheld until the analyst's own enumeration is signed. | **Lowers** the informational advantage held by the language counted first in the fixed column order, which is Quidra. |
| S-1 | Self-review (cross-cutting check: checks-disabled release mode) | Rule 1.3.1(a) required every witness to build under the frozen recipe, while §1.7.2 counted classes realizable only under *other* modes (`-ftrapv`, a debug profile). The frozen recipes name `rustc -O`, `swiftc -O`, `zig -OReleaseFast` — checks-disabled release builds — with no rule saying whether a language's checked mode is admissible, which is precisely the configuration that would compare one language's always-on checking against another's checks-disabled build. | **Rule 1.3.3** admits every documented, supported, non-deprecated build or execution mode of the frozen toolchain version for witnesses, identically for every language that has more than one — explicitly including a language whose two modes are a compiled binary and an interpreter, and a language whose checks are on in one mode and off in another — with the mode list taken from `environment/environment.json` and recorded per witness. **Rule 2.6.2.a** applies the same symmetry to `G` hops. A counterpart obligation is recorded on the environment document (§4.3). | **Lowers** Quidra's expected score: Quidra ships two documented execution modes (native compile and interpreter), and any counted class that differs between them is now enumerated for Quidra exactly as a debug/release difference is enumerated for Rust or Zig, instead of Quidra being treated as single-mode by default. |
| S-2 | Self-review (cross-cutting check: neutrality claim contradicted by the document's own content) | §0.2 item 1 claimed that nothing in the document is derived from or conditioned on Quidra, while §1.2.2 disposed of Quidra's one documented call-site openness and §1.7.3(4) asserted an anti-bias protection the document did not provide. A neutrality claim that the document's own content contradicts is worse than no claim. | §0.2 item 1 rewritten to state the claim accurately and to name the two rules that bear on Quidra's expected counts, with pointers to where each is now audited in both directions; items 6–8 added (absence is not determinacy; evidence required in both directions; no results observed). §0.3's specific per-language predictions withdrawn. | **Lowers** Quidra's expected score to the extent that the disclosed rules are now evidence-gated; neutral as a matter of wording. |

**Net direction.** Mixed, and the mix is asymmetric: findings 3, 6, 7, 8, 13, 15, S-1 and S-2 are each
expected to **lower** Quidra's reported score or its reported standing, finding 2 is expected to lower
its locality score specifically, finding 4 is expected to **raise** it, and findings 1, 5, 9, 10, 11,
12 and 14 are neutral or direction-unknown. Finding 4 is applied in full despite cutting in Quidra's
favour, because it is a correctness defect that the frozen corpus requires to be fixed; findings that
cut against Quidra are applied on exactly the same terms. No finding was rejected, and no correction
was traded against another.

---

### Second remediation pass — re-audit against the remediated frozen corpus

The corrections above were applied against an earlier state of
`01_capability_universe_and_probes.json`. That corpus has since been remediated in its own right, and
this document's Rule 0.1.1 makes the corpus governing: where the two disagree, **this document is
defective**. A full re-read of both files found that a majority of the numeric constants and two of the
probe descriptions in the text above no longer matched the corpus, that one rule had lost its body in
editing, and that three defects of the kinds the audit flagged across the methodology survived the
first pass. All are corrected below. **No results had been observed when this pass was applied
either** — the corpus's own `no_results_observed_at_remediation` records that only Quidra and Python
probe fragments existed and that no metric had been run over them — so this pass is pre-registration on
the same footing as the first.

| # | Severity | Defect | Change | Direction for Quidra's expected score |
|---|---|---|---|---|
| R-1 | BLOCKER | **Every corpus constant in the document was stale.** The text stated `N = 40` probes, "2 probes for each of the 20 families", `epsilon = 1/40 = 0.025`, a Capability Coverage denominator of **103**, and `F05.P2` worth **3** capability points. The frozen corpus states 44 probes (families 4, 5, 14 and 15 carry 3 each), a fixed denominator of **88**, and a uniform **2** points per probe. Every normalized score and every coverage charge this document specifies was therefore computed from the wrong constants. | §0.1, Rules 1.7.4, 1.8.2, 1.8.3, 2.9.1, the §1.9 and §2.10 tables and both schemas now carry `N = 44`, `epsilon = 1/44` (exact rational, never rounded before the division), denominator 88 and 2 points per probe. §0.1 records the superseded figures so the change is auditable rather than silent. | Neutral. The constants apply identically to all ten languages; a uniform 2 points per probe in fact removes any possibility that a probe's coverage cost depends on which language fails it. |
| R-2 | BLOCKER | **`F05.P2` was described from memory and described wrongly.** §0.1, Rule 1.1.1, §1.6.2 and §2.1 named the callee `g`, the caller's local `x = 1`, and the required post-call value 7. The corpus's `canonical_task` names `put`, a local `cell` initialized to 3, and a required post-call value of 12. This is exactly the failure Rule 0.1.1 was written to forbid, committed by the rule's own document. | All four sites corrected to the corpus's identifiers and values. `F08.P1` and `F08.P2` likewise now state the corpus's **fixed rung preference orders** (checked → wrapping → saturating → plain; checked-division → explicit guard → plain), the fallback integer 0 substituted for the exceptional case, and the requirement that the rung used and the reason each higher rung was unavailable be recorded — none of which the summary carried. `F07.P2`'s clause is corrected from "the corpus requires" to the corpus's actual wording, **OBSERVATION TARGET**, "not a support requirement; scored by metrics B/D only", with an explicit instruction not to convert it into a support judgement in either direction. | Neutral. It replaces remembered text with the frozen text for all ten languages. Stating the rung order removes analyst discretion over which arithmetic operation is measured, which is discretion that could have been exercised in any direction. |
| R-3 | BLOCKER (corpus contradiction, named in doc 01's `counterpart_obligations`) | **The aggregation basis was inverted relative to the frozen corpus.** Doc 01 predeclares that metrics A–D are computed **primarily on the COMMON BASIS** — the probes for which all ten languages have a fragment — with the all-fragments basis as a published secondary, and names this document in its counterpart obligations. Rules 1.8.6 and 2.9.4 did the reverse: variable-basis primary, common core as a disclosure only. They also defended the variable basis with the claim that the harmonic mean `2QC/(Q+C)` makes the trade "unprofitable" — an assertion doc 01 records as unproved and false under a variable basis. | Rules 1.8.6 and 2.9.4 rewritten: **common basis primary, reported as the result**; all-fragments secondary, published with each `n_L` and the per-metric delta; `PARTIAL` fragments in the common basis for all ten languages with no exemption; every published figure labelled with its basis and basis size; `best` never taken from one basis and normalized against the other; both rankings published where they disagree. The "unprofitable" assertion is withdrawn by name. §0.2 item 9, §3.1 items 2b and 6, §3.4, §3.5 and both schemas follow. The field key `core` is retained so downstream scripts need no rename. | **Direction depends on Quidra's own fragment count, which is unknown and unmeasured.** The correction is strictly stronger than the audit's Finding 10, which asked only that the common core be *disclosed*: on the common basis a language can no longer improve the reported aggregate by failing to express a probe at all, because the reported material is the same intersection for everyone. If Quidra expresses fewer probes than the intersection requires, this **lowers** its reported standing; if more, it is unaffected. |
| R-4 | BLOCKER (checks-disabled comparison — the cross-cutting item the audit flagged) | **Rule 1.3.3 contradicted the corpus's `safety_mode_clause` and reintroduced the defect it was written to cure.** The first pass fixed S-1 by admitting "every documented, supported, non-deprecated mode" for witnesses. The corpus meanwhile pinned every language with a choice to its optimized **checked** mode (Rust `-O -C debug-assertions=on`, Zig `-OReleaseSafe`, Swift `-O`), for exactly the reason the audit gave. Admitting every mode would have charged the multi-mode incumbents extra branches for unchecked builds the benchmark never produces — the same artefact as the original defect, entered from the opposite side — and would have let a class be excluded by appealing to a mode the recipe does not name. | Rule 1.3.3 rebound to the frozen recipe's own entries, read from the file and checked by gate V5, with the `safety_mode_clause` quoted. No mode outside the recipe is admissible **in either direction**. A language whose recipe lists more than one *execution* entry has every listed entry enumerated. A language with no checked mode is enumerated on its only mode's merits — a language fact scored by `B` and by Hidden Semantic Cost, never corrected for, softened, or re-described as an incumbency effect (§7). §0.2 item 10 states the posture; §3.1 item 2a makes V5 blocking; §4.3 makes the environment change blocking; witness records now carry `recipe_entry` rather than a free-text mode. | **Lowers** Quidra's expected standing relative to the first pass. Pinning the incumbents to checked mode *removes* branches from Rust, Zig and Swift at `F08.P1`/`F08.P2`/`F11.P1`, improving their determinacy; Quidra's own recipe lists two execution entries (`quidra build` and `quidra run`) and both are enumerated for it. The net of both effects runs against Quidra. |
| R-5 | MAJOR (unaccounted fact — Finding 9's defect, surviving at the other table) | Finding 9 was fixed at Table ARG and left unfixed at Table NUM. Table NUM's governing axes carry no A1 and no A9, while `value_vs_storage` is in `F07.P1`'s and `F07.P2`'s frozen fact sets and `control_flow_effect` is in `F08.P1`'s and `F08.P2`'s. Rule 1.4.2's own requirement — an excluded axis whose fact is in the probe's frozen set must name the metric that charges it or be brought back in scope — was therefore violated by this document's own second table. | New **Rule 1.5.2.c** accounts for all seven facts across the four numeric probes, naming for each whether it is in scope and, where it is not, the metric that charges it instead (`value_vs_storage` → Semantic Locality plus Hidden Semantic Cost; `control_flow_effect` → Semantic Locality under Rule 2.8.1.b plus Hidden Semantic Cost, with the reason A9 is not added here: R3 and R11 already decide divergence at the site, so re-branching would double-count under Rule 1.4.4). The §1.5.2 header is corrected to A4 × A5 × A6 × A10 (+A7 at `F08.P2`). | Neutral, and deliberately so. `control_flow_effect` is charged at Locality rather than added to `B`, which avoids charging a trapping operation twice — the double-counting pattern the audit flagged. Quidra's checked arithmetic is charged there once, exactly as Rust's and Zig's are. |
| R-6 | MAJOR (silent omission) | **Table coverage was mistaken for probe coverage.** The specialised tables name 6 probes; the corpus holds 44. Nothing said what governs the other 38, and the corpus has since added `F05.P3` — a third family-5 probe whose argument is a **function** — which no rule in this document mentioned. A probe no rule reaches is a probe with no enumeration. | New **Rule 1.5.-1**: Table ARG governs `F05.P1`/`F05.P2`, Table NUM the four numeric probes, and **every other probe is enumerated under §1.4 directly**, with axes from the §1.4.1 mapping applied to its own frozen fact set and every class dispositioned under Rule 1.7.1 step 4. A probe is never skipped, never given a reduced enumeration, and never assigned `B = 1` by default for want of a table. `F05.P3` is added to the §0.1 role index, and §0.1 states that the index covers 6 of 44 probes and that the other 38 are not exempt from anything. | Neutral. It closes a gap through which any language could have received an unenumerated probe. |
| R-7 | MAJOR (stale prediction embedded in a frozen instrument) | Rule 1.8.6 quoted a **superseded** NA-6 that named ten probes on which `NONE` was "expected". The corpus deleted that list precisely because naming it embeds a predicted result in a frozen instrument, and its NA-6 now states that which probes yield `NONE` for which language "is an output of the measurement, not an input to it". | The quotation and the ten probe ids are deleted from Rule 1.8.6, and §0.2 item 3 now states the corpus's actual position. No replacement list is given. | Neutral in arithmetic; it removes a pre-registered expectation about which languages would fail which probes, which is an input no analyst should have seen. |
| R-8 | MAJOR (mechanical defect — a rule with no content) | **Rule 2.6.3 had lost its body.** The heading "**Rule 2.6.3 (separate disclosure).**" was concatenated directly onto Rule 2.6.4's heading with no text between them, while §2.5 and §2.6.1 both cross-referenced the separate `G`-hop disclosure. A rule referenced twice and stating nothing is not enforceable. | Rule 2.6.3 restored: `G` hops count 1 toward `H_i` like every other hop and are **additionally** reported per probe and as a `G_hop_total`, so a reader can see how much of a language's `H` is declaration distance and how much is configuration dependence. The disclosure explicitly changes no score and no ranking is computed from it. | Neutral; it is a disclosure with no arithmetic effect, applied to all ten languages. |
| R-9 | MINOR (closed list drawn from nine languages) | Rule 1.2.2 made a dimension-6 difference material only where the language gives "a defined way to observe the event — a user-defined constructor/destructor/`deinit`/`Drop`, an allocator hook, a reference-count query, or a finalizer". Read as closed, that list is an enumeration of nine languages' mechanisms, and a language whose observation channel is not named would lose branches by omission from a list rather than by measurement. | The list is made explicitly open and drawn per language from that language's own specification: "or any other mechanism the language's own authoritative specification defines", with the statement that a language not named is not deprived and a language named holds no privilege. | Slightly **raises** the chance that a branch survives for any language, Quidra included; the effect is symmetric and is not expected to favour any language. |
| R-10 | MINOR (mechanical) | Three defects that would have broken tooling or an audit trail: the adjudication-register table had a duplicated, malformed header row (`\| Id \| Ruling \|\| Id \| Ruling \|` over a two-column separator); Rule 3.3.2 declared `AR-001`…`AR-007` the pre-run seeds while the table and §3.5 both carried nine, leaving `AR-008` and `AR-009` in an undefined state between "seed" and "post-start ruling"; and the §2.11 schema's `resolved_inside_fragment` example named `type_and_representation`, a fact that is **not** in `F05.P1`'s set at all. | Table header repaired; Rule 3.3.2 corrected to `AR-001`…`AR-009`, recording that both were written during remediation when no `B_i` or `H_i` existed for any language; the schema example replaced with a placeholder bound to the probe's own fact set, and both schemas given a validation rule that every member of `resolved_locally` and `resolved_inside_fragment` must be a member of `fact_set`, and that a `fact_set` must equal the probe's frozen `semantic_facts_expected`. | Neutral; audit-trail and tooling correctness. |
| R-11 | MINOR (fragment selection left to judgement) | Rule 1.1.3 said fragments are "rendered idiomatically per the corpus's authoring rules R1–R4". The corpus has since added **R10**, which makes fragment selection mechanical (fewest tokens under the frozen tokenizer, ties broken by first appearance in official documentation, every alternative recorded) because "the normal choice" was the dominant undefined judgement in the corpus — and every value this document computes is computed from the text R10 selects. | Rule 1.1.3 now cites R1–R10 and states R10's mechanism and its override (a `canonical_task`'s own preference order, at `F08.P1`, `F08.P2`, `F10.P1`). An analyst's judgement of idiom never selects the fragment. | Neutral; it removes discretion that could have been exercised for or against any language, at the one point where discretion moves all four metrics at once. |

**Findings rejected in this pass: none.** No finding from the audit brief was rejected, weakened,
declared out of scope, or answered with a caveat attached to a surviving rule, in either pass.

**Superseded rows in the table above.** Row 10 and row S-1 of the first-pass table describe the state
of the document as it then stood; their *changes* are superseded by R-3 and R-4 respectively, and their
stated directions no longer apply. They are left in place as the record of the first pass rather than
rewritten, per the corpus's discipline that a defect is fixed and recorded, not silently corrected.

**Net direction of this pass.** Mixed, weighted against Quidra. R-4 is expected to **lower** Quidra's
standing (the incumbents' arithmetic probes are measured at their checked modes, which closes branches
for them, while Quidra's two execution entries are both enumerated for it). R-3 lowers it if Quidra
expresses fewer probes than the ten-language intersection and leaves it unchanged otherwise. R-9
slightly raises the branch count for every language symmetrically. R-1, R-2, R-5, R-6, R-7, R-8, R-10
and R-11 are neutral: they replace stale or missing text with the frozen corpus's text, close a gap
through which any language could have escaped an enumeration, and remove analyst discretion in both
directions. No correction in this pass was traded against another, and none was adopted because of the
direction it moves any language's score.

---

**End of frozen document 03.**
