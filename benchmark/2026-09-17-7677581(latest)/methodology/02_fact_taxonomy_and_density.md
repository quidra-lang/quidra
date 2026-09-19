# FROZEN METHODOLOGY 02 — Semantic-Fact Taxonomy and Semantic Density Counting Rule

**Benchmark run:** `2026-09-17-7677581`
**Authoritative specification:** `../prompt.md` §6.1.2, §6.1.3, §6.1.4.A, §6.1.7, §25.1(D), §26, §32
**Status:** FROZEN. No clause in this document may be changed after the first probe is annotated or the first token is counted. The corrections listed in the **Remediation changelog** at the end of this document were applied **before any probe was annotated, any score-bearing token count was taken, and any result was observed**; they are pre-registration, not post-result revision (§25.4, §32).
**Scope:** This document defines (1) the semantic-fact taxonomy, (2) the "explicitly recoverable" test and its interaction with Semantic Locality, (3) the anti-double-counting rule, (4) the language-neutral token-counting rule and its tokenizer, and (5) the Semantic Density raw value, aggregation, and normalization.

**Fixed comparison set and fixed column order (all 10 languages, identical treatment):**

`Quidra, Python, C++, Rust, Go, Java, TypeScript, Kotlin, Swift, Zig`

---

## 0. Fairness invariants governing this document

These are binding constraints on every rule below, taken from §6.1.1, §6.1.7 and §32.

- **I1 — Non-derivation.** No rule in this document was derived from Quidra's syntax, operators, types, or feature set. Every fact kind is taken verbatim from the closed list in spec §6.1.3; every worked example in §1 is drawn from non-Quidra languages *on purpose*, so that the taxonomy can be audited for Quidra-independence by inspection. Quidra is annotated by these rules like any other language, last, and with no exceptions. One departure from the *examples* convention is deliberate and is recorded here rather than left implicit: the K13 example table carries a single **pre-adjudication row** that names reference-counted managed reclamation (Swift ARC, and any other language of the fixed set whose managed references are reference-counted — Quidra's managed retain/release included) and fixes its disposition and its `basis_class` **before** annotation begins. That row exists precisely because Quidra is annotated last: it removes annotator discretion over a disposition that would otherwise be settled after the other nine counts are known. It grants Quidra nothing that Swift does not receive from the same row.
- **I2 — No capability removal.** Nothing here removes a fact kind, site class, or capability because a language (including Quidra) lacks it. A language that cannot express a probe scores that way; it does not make the probe disappear.
- **I3 — Symmetry.** Every rule is stated as a language-independent predicate over (surface form, language specification). A language must be able to win a fact kind that Quidra loses and lose one that Quidra wins.
- **I4 — Anti-fabrication.** Every counted fact must carry a `basis` citation to a named clause/section of a published language specification or reference, or to a published standard-library contract. A row without a `basis` is not counted. Where a fact cannot be adjudicated honestly, it is recorded `INDET` or `NA` with a written reason (§2.6, §5.5); it is never guessed.
- **I5 — Chosen reading of "semantic fact" (declared in advance, and it is the reading *less* favourable to explicitly-annotated languages).** Two readings were available:
  - **(a) Information-content reading:** a fact counts when the reader can pin it down from the local form plus the language rules, *including* when the language pins it by a universal rule that offers no alternative (e.g. "all Go variables are mutable", "all Java class types are references", "Python integers are unbounded").
  - **(b) Distinguishing-choice reading:** a fact counts only where the language offered at least two values and the surface form selected one — i.e. only annotated choices count.

  **Reading (a) is frozen**, because it is the reading faithful to §6.1.1 ("how much important program meaning is communicated explicitly and determinately per unit of syntax"): a guarantee the reader can rely on is communicated program meaning whether a modifier spells it out or a universal rule supplies it.

  **No claim is made here about which language reading (a) favours, and none may be inferred from this document.** Reading (a) rewards languages that obtain guarantees from universal rules at zero token cost; reading (b) rewards languages whose surface form is dense with explicit modifiers. Languages of the fixed set sit on *both* sides of that split simultaneously — Go's zero-value and assignability rules, Java's reference-semantics and unchecked-exception rules, Python's unbounded-integer rule, Rust's borrow-requires-`&` rule and Quidra's always-checked-arithmetic rule are all universal-rule facts, while Rust, Swift, Zig, Java, Kotlin and Quidra all carry explicit local markers — so the direction of the tilt is not derivable by inspection and is therefore **not asserted**. It is **measured**: §5.5 requires `density.json` to publish, for every language, the complete reading-(b) recomputation beside the primary value, together with the rank-order delta between the two readings. Any auditor can check the direction of I5's effect on any language, including Quidra, instead of taking this paragraph's word for it. The worked example in §6 is illustrative only and is not evidence about the ten measured languages.
  Every `COUNT` row records `basis_class ∈ {local-marker, universal-rule, stdlib-contract}` so the split is visible in the published raw data and the effect of I5 is auditable rather than hidden.

---

## 1. The fact taxonomy

### 1.0 Structure of the taxonomy

The taxonomy is **closed**: exactly the thirteen kinds listed in spec §6.1.3, no more and no fewer. They are labelled **`K01`–`K13`** and used under these labels in every raw file. The `K` prefix is deliberate: capability **families** are identified `F01`–`F20` and probes `F<nn>.P<k>` in the frozen probe document (§2.6), so labelling the fact kinds `F01`–`F13` — as an earlier draft did — made `F09` mean both "overflow / exceptional numeric behavior" and "capability family 9", in documents that are read together. The kinds, their order and their spec §6.1.3 wording are unchanged; only the label is.

| ID | Fact kind (spec §6.1.3 wording) |
|---|---|
| K01 | value versus storage |
| K02 | mutability |
| K03 | aliasing / writable aliasing |
| K04 | initialization state |
| K05 | type and representation |
| K06 | conversion behavior |
| K07 | possible failure |
| K08 | alternative value cases |
| K09 | overflow / exceptional numeric behavior |
| K10 | allocation, copying, moving, borrowing, or destruction |
| K11 | externally visible side effects |
| K12 | control-flow effect |
| K13 | lifetime / resource effect |

Facts are not asserted about a probe as a whole. They are asserted at **fact-bearing sites** (§1.1), one fact per `(site, fact kind)` pair at most (§3). A probe's fact ceiling is therefore `13 × |sites|` (sub-sites included, §1.1/E7), which bounds annotation inflation mechanically.

### 1.1 Fact-bearing sites (mechanical enumeration)

A **site** is a syntactic construct physically present in the probe region (§4.2). Sites are enumerated by walking the probe region in source order and applying these rules in order; each construct yields at most one site, except that a call, a composite literal, or a multi-declarator statement additionally yields one **sub-site per argument, field initializer or declarator** under rule E7 below.

| Class | Name | Created by |
|---|---|---|
| **S1** | Binding site | Each place the source introduces a named entity that holds or denotes a value: variable, constant, parameter, field, pattern/destructuring binding, loop variable, explicit capture, named function or type *insofar as it introduces an entity*. |
| **S2** | Operation site | Each syntactically present application of an operator, index/slice, call, explicit conversion/cast, allocation expression, assignment or compound assignment, comparison, or propagation operator. |
| **S3** | Signature site | Each function/method/closure declaration, considered once for its *return* semantics. (Its parameters are S1 sites; its body's constructs are their own sites.) |
| **S4** | Type-definition site | Each user-declared type, alias, variant/sum/enum, interface/trait/protocol, or generic parameter list declared inside the probe region. |
| **S5** | Control-transfer site | Each `return`, `break`, `continue`, `throw`/`raise`, `goto`, early-exit, `defer`, loop header, conditional, and `match`/`switch`/`when` construct. |
| **S6** | Scope-exit site | Each lexical scope whose end is a point at which the language specifies (or conspicuously does not specify) cleanup for an entity declared inside the probe region: one site per such scope. |
| **S7** | Boundary site | Each import/`use`/module reference, FFI declaration or foreign call, and each concurrency primitive (spawn, `await`, channel operation, lock/synchronisation operation). |

**Binding rules for site enumeration (frozen):**

- **E1 — Only visible syntax creates sites.** A behaviour that the language performs *implicitly* (an implicit conversion, an implicit copy, an implicit destructor call, an implicit box, an implicit `await`) never creates a site. It is recorded as a fact *value* at the enclosing visible site, and it is the subject of metric D (Hidden Semantic Cost). This is load-bearing: without E1 a language would gain Semantic Density *by being implicit*, which inverts the metric.
- **E2 — One construct, one site.** A compound assignment (`x += 1`) is a single S2 site bearing both the arithmetic facts and the assignment facts. A chained call `a.b().c()` is two S2 sites. A `let`/`var` declaration with an initializer is **one S1 site (the binding) plus one S2 site (the initialization operation)**.
- **E3 — Literals are not sites.** A literal contributes to the facts of the site that consumes it (its type contributes to that site's K05). This is required by §3.
- **E4 — Facts about an entity are asserted at its binding site only.** An S2 site asserts facts about *the operation* (what it does, what its result is), never about the identity/mutability/type of an operand that already has an S1 site in the probe region. This eliminates the largest source of double counting.
- **E5 — Redundant grouping creates nothing.** Parentheses or blocks used purely for grouping create no site.
- **E6 — Boilerplate outside the probe region creates no site** (§4.2).
- **E7 — Argument sub-sites.** Each **argument position** of a call, each **field initializer** of a composite/struct/record literal, and each **declarator** of a multi-declarator statement is its own S2 sub-site, identified `s<n>.a<k>` (arguments and field initializers, in source order, `k` from 1) or `s<n>.d<k>` (declarators). A sub-site bears its own rows under the same applicability matrix and the same gates as any other S2 site, and is subject to the same one-row-per-`(site, fact kind)` cap (§3.1/A3.1). The enclosing call site `s<n>` retains S2 rows for its **result** and for its **control/effect** facts only (K01, K05, K07, K08, K11, K12 under their gates); the per-argument facts — notably K03 (does this position hand out a writable alias?), K06 (is this argument converted, and how?) and K10 (is it copied, moved or borrowed?) — are annotated at the sub-sites, one answer per position. Sub-sites are sites for every purpose in this document, including the `13 × |sites|` ceiling and the site counts published under §5.5. This rule exists because a call such as `f(a, b, c)` can pass one argument by value, one as a writable alias and one with an implicit conversion; without sub-sites the ledger has one row for three different answers and two annotators cannot converge.

### 1.2 Applicability matrix

A `(site, fact kind)` row exists only where the matrix permits. Rows outside the matrix are not annotated and are not counted anywhere.

| | S1 binding | S2 operation | S3 signature | S4 type def | S5 control | S6 scope exit | S7 boundary |
|---|---|---|---|---|---|---|---|
| K01 value/storage | ● | ◐ | ● | ● | | | |
| K02 mutability | ● | ◐ | ● | ● | | | |
| K03 aliasing | ● | ● | ● | ● | | | |
| K04 initialization | ● | ● | | ● | | | |
| K05 type/representation | ● | ◐ | ● | ● | | | |
| K06 conversion | | ● | ● | | | | ● |
| K07 possible failure | | ◐ | ● | | ● | ● | ● |
| K08 alternative cases | ● | ◐ | ● | ● | ● | | |
| K09 numeric edge | | ◐ | | | | | |
| K10 alloc/copy/move/borrow/destroy | ● | ● | ● | | | ● | ● |
| K11 external side effects | | ◐ | ● | | ● | ● | ● |
| K12 control-flow effect | | ◐ | ● | | ● | ● | ● |
| K13 lifetime/resource | ● | | ● | ● | | ● | ● |

● = applicable. ◐ = applicable **only under the gate** stated in the kind's definition below. Blank = never annotated.

The three gates used by ◐ (frozen, language-independent):

- **G-value** (K01, K05, K08 at S2): applicable only when the operation produces a value that the probe subsequently uses, returns, or observes. A discarded/void result makes the row `NA`.
- **G-mutate** (K02 at S2): applicable only when the operation modifies an entity that is already initialized. Pure initialization makes the row `NA` (K04 covers it).
- **G-effect** (K07, K09, K11, K12 at S2): K09 is applicable only at arithmetic, numeric-conversion, and numeric-comparison operations. K07 is applicable at calls, indexing/slicing, conversions, allocations, resource operations, propagation operators, and arithmetic. K11 and K12 are applicable only at operations that *could*, in some language in the fixed set, reach state or control outside the probe region: calls, propagation, throw-capable and trap-capable operations, writes through a reference/pointer, I/O, FFI, and concurrency operations. A local-only operation on a local-only entity makes K11/K12 `NA`.

Gates are stated in terms of "some language in the fixed set", never "this language". Consequently: **gates depend only on the syntactic shape and class of the site and never on the language under annotation, so two sites of the same class and shape are annotated with the same rows in every language, and the disposition of those rows is the only thing the language decides.** Site *counts* legitimately differ between languages, because the number of constructs a language requires in order to express the probe is part of what the metric measures — E2 splits a Rust `let … = c.wrapping_add(1)` into four sites where a Go `+=` yields three. No row-count parity across languages is claimed or expected; `density.json` publishes each language's site count per probe (§5.5) so the difference is visible and auditable rather than assumed away.

### 1.3 Row dispositions

Every annotated row receives exactly one disposition.

| Code | Meaning | Density numerator | Feeds |
|---|---|---|---|
| `COUNT` | The fact is **applicable, behaviourally determinate, and explicitly recoverable from the local surface form** (§2). | **+1** | Metric A |
| `LOOKUP` | Applicable and determinate, but resolving it requires ≥1 declaration-graph hop outside the probe region (§2.3). | 0 | Evidence for metric C; **methodology 03 computes C by its own algorithm and is authoritative for every hop count** (§2.3) |
| `INDET` | Applicable, but no single behaviourally determinate answer exists even with unlimited lookups: the specification says *undefined* or *erroneous*, or the answer is a runtime-data-dependent property (§2.5, and see Rule 2.5.1 for the *universally-specified-indeterminacy* case, which is `COUNT`). | 0 | Metrics B and D; an `INDET` fact also triggers methodology 03 Rule 2.7.1 (`SAT`) for metric C (§2.3) |
| `DUP` | Applicable and determinate, but the identical fact value has already been counted for this entity/operation at another site in the same probe (§3). | 0 | Audit only |
| `NA` | Not applicable under the matrix or a gate, with a written reason. | 0 | Audit only (per §26, `NA` here never becomes a silent zero for a capability the probe is testing — that is handled by Capability Coverage `C`) |

### 1.4 The thirteen fact kinds

Format for each kind: **Definition** → **Counts when** → **Does NOT count when** → **Worked examples** (drawn from the fixed set; `+` positive, `−` negative).

**Absence facts (frozen, applies to every kind below).** A row whose *value* asserts the **absence** of a behaviour — `cannot fail`, `no externally visible effect`, `returns normally / cannot panic or diverge`, `no allocation`, `no alias can exist`, `no cleanup runs` — is `COUNT` **only** where (i) the cited language-specification clause or published standard-library contract **states the absence**, or (ii) a **universal language rule entails it** (e.g. "a borrow requires a visible `&`, so no alias can exist"; "a type that implements no destructor runs no cleanup"; "integer addition is total, so it cannot fail"). Where the absence is merely **not mentioned** by the cited clause, the row is **`LOOKUP` with `hops_decl = 1`** — the callee's or entity's definition is what a reader must actually consult — and the `note` field records which absence was unsupported. A value may be narrowed to the part that is supported (recording the unsupported part in `note`) rather than dropped, provided the recorded value is fully supported by the citation. This rule is what stops an absence from being free: without it, any opaque name could be awarded "cannot fail, no effect, returns normally" at zero evidentiary cost, in any language, and the metric would reward calling more library functions rather than communicating more meaning. It is applied identically to all 10 languages and to every fact kind.

---

#### K01 — Value versus storage

**Definition.** Which of these the site denotes: (i) an independent storage location whose identity is observable (addressable, aliasable, or assignable through another name); (ii) a value without observable identity (a temporary/rvalue/pure result); (iii) a handle or reference denoting storage owned elsewhere.

**Counts when.** The local surface form plus the language rules select exactly one of (i)/(ii)/(iii) for that site — including when the language fixes it by a universal rule (I5a). At S2, only under gate **G-value**.

**Does NOT count when.** The category depends on a declaration outside the probe region (a callee's return type, an inferred type sourced from an import, a type alias defined elsewhere) → `LOOKUP`; or the category is unspecified by the language → `INDET`.

**Examples.**

| # | Language | Surface form | Verdict | Reason (basis) |
|---|---|---|---|---|
| + | C++ | `int x = 5;` at block scope | `COUNT` (i) | Declares an object with automatic storage duration; `&x` is well-formed. [basic.stc.auto] |
| + | Java | `String s = t;` | `COUNT` (iii) | All non-primitive types are reference types; `s` holds a reference, never a copy of the object. JLS §4.3.1 |
| + | Go | `var a [3]int` | `COUNT` (i) | Arrays are values with storage; assignment copies the whole array. Go spec, "Array types", "Assignability" |
| − | C++ | `auto x = f();` | `LOOKUP` | `auto` drops references; whether `x` is an independent object or a copy of a referent depends on `f`'s return type. 1 hop (callee signature). |
| − | TypeScript | `const x = obj.p;` | `LOOKUP` | Primitive value vs shared object reference depends on the declared type of `obj.p`. 1 hop (type declaration). |
| − | Kotlin | `val x = compute()` | `LOOKUP` | Value class vs reference class vs boxed primitive depends on the callee's return type. 1 hop. |

---

#### K02 — Mutability

**Definition.** At S1/S3/S4: whether the entity introduced at this site may be modified after this point, and through which name(s). At S2 (gate **G-mutate**): whether this operation modifies an entity that is already initialized.

**Counts when.** The local form plus the language rules determine the answer — by an explicit marker (`const`, `mut`, `let`/`var`, `val`/`var`, `final`, `readonly`) *or* by a universal rule (I5a), *or* because the operand's type is pinned by a literal in the probe region.

**Does NOT count when.** The answer depends on the declaration of a type or entity outside the probe region → `LOOKUP`; or on a compiler mode/configuration → `LOOKUP`.

**Note.** Binding mutability and referent mutability are the *same* fact kind, but they are different *values* at different sites, so a language that separates them (binding at S1, referent at the S1 of the reference) earns them at their own sites without violating §3.

**Examples.**

| # | Language | Surface form | Verdict | Reason (basis) |
|---|---|---|---|---|
| + | Swift | `let a = 1` / `var a = 1` | `COUNT` | Immutable vs mutable is selected by the declaration keyword. Swift Reference, "Declarations: Constant/Variable Declaration" |
| + | Kotlin | `val n = 0` | `COUNT` | `val` binding cannot be reassigned. Kotlin spec, "Property declaration" |
| + | Go | `x := 0` | `COUNT` (`basis_class = universal-rule`) | Go has no immutable variables; every declared variable is assignable. Go spec, "Variables", "Assignment statements" |
| + | Java | `final int x = 5;` | `COUNT` | `final` forbids reassignment; `int` is a primitive value. JLS §4.12.4 |
| − | C++ | `auto& r = get();` | `LOOKUP` | Whether the referent is `const` depends on `get`'s return type. 1 hop. |
| − | TypeScript | `const o: Config = load();` | `LOOKUP` | `const` pins only the binding; field mutability requires the `Config` declaration (`readonly` or not). 1 hop. |
| − | Python | `x = f()` | `LOOKUP` | The name is rebindable (determinate), but whether the *object* is mutable depends on the callee's return type. 1 hop. Note the contrast: `x = 5` is `COUNT`, because the literal pins `int`, which is immutable. Python Reference §3.1, §4.2.1 |

---

#### K03 — Aliasing / writable aliasing

**Definition.** Whether, at this site, another name or path may read the same storage, and whether another name may *write* it; and, at a call/argument site, whether the operation hands out a writable alias.

**Counts when.** The local form plus language rules determine both the read-alias and write-alias answers — including "no alias can exist" and "an alias is necessarily created".

**Does NOT count when.** The answer needs a callee signature, a parameter mode declared elsewhere, an overload set, a trait/interface implementation, or the declaration of the operand's type → `LOOKUP`. Spec §6.1.4.B names the `f(x)` call site as the canonical case; §6.1.4.B also forbids awarding or removing points merely because a language uses the glyph `&` — K03 is adjudicated on the *complete* local surface form plus the language rules, not on any glyph.

**Examples.**

| # | Language | Surface form | Verdict | Reason (basis) |
|---|---|---|---|---|
| + | Rust | `let r = &mut v;` (with `v` bound in the probe region) | `COUNT` | Exclusive mutable borrow; no other live alias may exist for the borrow's lifetime. Rust Reference, "References and Borrowing" |
| + | C++ | `int& r = x;` | `COUNT` | `r` is a writable alias of `x`. [dcl.ref] |
| + | Swift | `f(&x)` where `f` takes `inout` | `COUNT` | The `&` at the call site is mandatory for `inout`; copy-in/copy-out with write-back is guaranteed by the *call-site form together with the language rule that `&` may only appear for `inout`*. Swift Reference, "In-Out Parameters" |
| + | Java | `f(list)` where `list` is declared in the probe region with a class type | `COUNT` | The callee necessarily receives a writable alias to the same object (reference semantics are universal for class types). Whether it *does* write is K11/K02, a separate row that is `LOOKUP`. JLS §8.4.1 |
| − | C++ | `f(x)` | `LOOKUP` | By-value, by-reference, and by-const-reference are indistinguishable at the call site. 1 hop (callee signature); more if `f` is overloaded. |
| − | Go | `f(s)` where `s` comes from an import | `LOOKUP` | Slice/map/pointer types share backing storage while arrays and structs copy; the operand's type must be resolved. ≥1 hop. |
| − | TypeScript | `f(x)` | `LOOKUP` | Object arguments always share, primitives never do; which applies depends on `x`'s declared type. ≥1 hop. |

---

#### K04 — Initialization state

**Definition.** Whether the entity is initialized at this site, and after this operation: initialized with a specified value, initialized with a language-specified default, or left uninitialized.

**Counts when.** The local form plus the language rules determine the *state* (initialized / default-initialized / uninitialized). Note: this kind asks about the **state**, not the **value**. A form that determinately leaves a variable uninitialized therefore *counts* — the reader learns a real and important fact.

**Does NOT count when.** Initialization is performed by a callee, another translation unit, or a construct outside the probe region → `LOOKUP`; or the state at the site depends on data-dependent flow that cannot be resolved from the probe region → `INDET`.

**Examples.**

| # | Language | Surface form | Verdict | Reason (basis) |
|---|---|---|---|---|
| + | Zig | `var x: i32 = undefined;` | `COUNT` (uninitialized) | `undefined` explicitly marks the variable as holding an unspecified value; reading it before assignment is illegal behaviour. Zig Language Reference, "undefined" |
| + | C++ | `int x;` at block scope | `COUNT` (uninitialized) | Default-initialization of a non-class type performs no initialization; the value is indeterminate. [dcl.init] |
| + | Go | `var x int` | `COUNT` (default-initialized) | The zero value rule initializes `x` to `0`. Go spec, "The zero value" |
| + | Java | field `int f;` | `COUNT` (default-initialized) | Fields receive default values. JLS §4.12.5 |
| − | C++ | `S s; init(&s);` | `LOOKUP` | Whether `s` is initialized afterwards depends on `init`. 1 hop. |
| − | Python | a name that may be bound in an enclosing or global scope | `LOOKUP` | Binding state requires resolving the scope chain outside the probe region. ≥1 hop. Python Reference §4.2.2 |

---

#### K05 — Type and representation

**Definition.** The static type (or interface) of the entity or of the operation's result, together with its representation (width, signedness, encoding, layout) where the language pins it.

**Counts when.** The local form plus the language rules pin **the type identity**. At S2, only under gate **G-value**.
**Representation sub-rule (frozen):** if the type identity is pinned but its representation is implementation-defined (e.g. C++ `int` width, Go `int` width), the row still `COUNT`s — the type is the fact — and the ledger records `representation: implementation-defined`, which is consumed by metrics B and D. This prevents double-penalising, and it keeps the row cap at one fact per `(site, kind)`.

**Does NOT count when.** The type is inferred from an entity declared outside the probe region → `LOOKUP`; or the type is dynamic/`any`-typed such that no static type exists → `INDET`.

**Examples.**

| # | Language | Surface form | Verdict | Reason (basis) |
|---|---|---|---|---|
| + | Rust | `let n: i32 = 0;` | `COUNT` (type + representation) | 32-bit two's complement. Rust Reference, "Integer types" |
| + | Java | `int n = 0;` | `COUNT` (type + representation) | 32-bit two's complement, fixed by the JLS. JLS §4.2.1 |
| + | TypeScript | `let n: number = 0;` | `COUNT` (type + representation) | `number` is IEEE-754 binary64. ECMAScript §6.1.6.1 |
| + | Go | `var n int = 0` | `COUNT` (type pinned, `representation: implementation-defined`) | `int` is 32 or 64 bits, implementation-specific. Go spec, "Numeric types" |
| − | C++ | `auto x = f();` | `LOOKUP` | The type is `f`'s return type after `auto` deduction. 1 hop. |
| − | Python | `x = f()` | `LOOKUP` | The type is the callee's result type; no local annotation. 1 hop. |
| − | TypeScript | `const v = JSON.parse(s);` | `INDET` | The static type is `any`; no static type constrains the value. TypeScript Handbook, "any"; ES §25.5.1 |

---

#### K06 — Conversion behavior

**Definition.** Whether a value changes type or representation as it crosses into this operation (initialization, assignment, argument passing, return, explicit cast, arithmetic promotion), and under which rule (identity/no conversion, widening, narrowing/truncation, sign change, boxing/unboxing, reinterpretation, user-defined conversion).

**Counts when.** The local form plus the language rules determine both *whether* a conversion occurs and *which* conversion. Explicit conversion syntax counts; so does a universal rule that guarantees no implicit conversion happens.

**Does NOT count when.** The presence or kind of conversion depends on a callee signature, an overload set, or a user-defined conversion operator declared outside the probe region → `LOOKUP`; or the conversion's result is undefined/unspecified (e.g. out-of-range floating-to-integer in C++) → `INDET`.

**Examples.**

| # | Language | Surface form | Verdict | Reason (basis) |
|---|---|---|---|---|
| + | Go | `int64(x)` | `COUNT` | Explicit conversion with spec-defined truncation/extension. Go spec, "Conversions" |
| + | Java | `long y = x;` where `x` is `int` | `COUNT` | Widening primitive conversion, value-preserving, fixed by the JLS. JLS §5.1.2 |
| + | Rust | `let y = x as i64;` | `COUNT` | Rust performs no implicit numeric coercion; `as` is defined truncation/sign-extension. Rust Reference, "Type cast expressions", "Type coercions" |
| + | Zig | `@as(i64, x)` | `COUNT` | Explicit, compile-time-checked widening. Zig Language Reference, "Type Coercion", "@as" |
| − | C++ | `f(x)` with `x` of type `int` | `LOOKUP` | An implicit standard or user-defined conversion may occur depending on the parameter type. ≥1 hop; also a Hidden Semantic Cost item (metric D). |
| − | Java | `f(x)` with `f` overloaded | `LOOKUP` | The applicable conversion depends on overload resolution. ≥1 hop (overload set). |
| − | C++ | `int i = d;` with `d` a `double` holding an out-of-range value | `INDET` | The converted value is undefined when unrepresentable. [conv.fpint] |

---

#### K07 — Possible failure

**Definition.** Whether this operation can fail, and **how failure is signalled** (returned error value, optional/absent value, thrown/raised exception, trap/abort/panic, error union, sentinel).

**Frozen uniformity rules.**
- **U-07.1** Resource-exhaustion failures (out-of-memory, stack overflow) are **excluded** from K07 for all 10 languages. They exist everywhere and would add a constant to every language.
- **U-07.2** K07 asks *whether* and *how*, not *the exhaustive set of failure modes*. The exhaustive set is a Semantic Determinacy (B) and Semantic Locality (C) question. This is what allows exception-based languages to earn K07 by universal rule while still being measured on the imprecision by B and C.

**Counts when.** The local form plus language rules determine whether the operation can fail and by what mechanism — including "cannot fail" and including a universal rule ("any call may throw an unchecked exception").

**Does NOT count when.** Determining whether failure is possible requires the callee's signature or the result type's declaration → `LOOKUP`; or the failure behaviour is undefined/unspecified → `INDET`; or it depends on a compiler mode → `LOOKUP`.

**Examples.**

| # | Language | Surface form | Verdict | Reason (basis) |
|---|---|---|---|---|
| + | Swift | `try f()` | `COUNT` | `try` is mandatory at the call site of a throwing function; failure and its mechanism are marked locally. Swift Reference, "Error Handling" |
| + | Zig | `try f()` | `COUNT` | The error-union unwrap is spelled at the use site. Zig Language Reference, "Errors" |
| + | Java | `f()` (any method call) | `COUNT` (`basis_class = universal-rule`) | Any invocation may complete abruptly by throwing; the mechanism is exceptions. JLS §11, §15.12.4.5 |
| + | Python | `d[k]` | `COUNT` | Missing key raises `KeyError`. Python Reference §6.3.2; `dict.__getitem__` contract |
| − | Go | `a, b := f()` | `LOOKUP` | The two-value form does not by itself establish that the second result is an `error`; the identifier `err` is a convention, not a rule. 1 hop (callee signature). |
| − | C++ | `v[i]` on `std::vector` | `INDET` | Out-of-range indexing is undefined behaviour — not a *failure* with a signalling mechanism. [sequence.reqmts] |
| − | TypeScript | `arr[i]` | `LOOKUP` | Whether the result is `T` or `T \| undefined` depends on the `noUncheckedIndexedAccess` compiler mode. 1 hop (global configuration/compiler mode). |

---

#### K08 — Alternative value cases

**Definition.** Whether the value at this site is one of a set of alternatives (optional/nullable, sum/variant/enum, error union, tagged union, open interface/subtype set), what the alternative set is, and whether the handling at this site is exhaustive.

**Counts when.** The alternative set and (where the site handles alternatives) exhaustiveness are determined by the local form plus the language rules — including the case where the type is declared *inside* the probe region, which is local by §2.2.

**Does NOT count when.** The alternative set lives in a declaration outside the probe region → `LOOKUP`; or the set is open/unbounded so that no determinate set exists → `INDET`. Rule 2.5.1 does **not** rescue the open-set case: which dynamic type an open value holds is a runtime-data-dependent property (§2.5), not a universally specified rule the reader can state and rely on.

**Examples.**

| # | Language | Surface form | Verdict | Reason (basis) |
|---|---|---|---|---|
| + | Rust | `match o { Some(v) => …, None => … }` | `COUNT` | `Option` alternatives and match exhaustiveness are fixed by the language and the named standard type. Rust Reference, "Match expressions"; `core::option` |
| + | Swift | `switch e { case .a: …; case .b: … }` with `enum E { case a; case b }` declared in the probe region | `COUNT` | Enumeration cases and switch exhaustiveness are local. Swift Reference, "Enumerations", "Switch" |
| + | Kotlin | `when (x) { is A -> …; is B -> … }` with `sealed class` declared in the probe region | `COUNT` | Sealed hierarchy is closed and `when` exhaustiveness is checked. Kotlin spec, "Sealed classes", "When expression" |
| − | Java | `switch (o) { case A a -> …; }` over an **imported** sealed interface | `LOOKUP` | The permitted subtypes live in another compilation unit. ≥1 hop. |
| − | Go | type switch over an interface value | `INDET` | Go interfaces are open; no determinate alternative set exists and no exhaustiveness is defined. Go spec, "Interface types", "Type switches" |
| − | Python | `match v: case …` over duck-typed values | `INDET` | No closed alternative set exists for an unannotated value. Python Reference §8.6 |

---

#### K09 — Overflow / exceptional numeric behavior

**Definition.** What happens at this numeric operation in the exceptional cases the language defines for it: signed/unsigned integer overflow, division or remainder by zero, shift by an out-of-range amount, lossy or out-of-range numeric conversion, and IEEE-754 special cases where the language pins them.

**Counts when.** The local form plus the language rules pin a **single behaviourally determinate outcome** for the exceptional cases (wrap, trap/panic, raise a named exception, saturate, produce a defined special value, or "cannot occur").

**Does NOT count when.** The specification's answer is *undefined*, *unspecified*, or *implementation-defined behaviour* → `INDET` (an "undefined" answer is determinate about the standard's text but not about the program's meaning, and Semantic Density measures communicated *program* meaning per §6.1.1); or the answer depends on a build mode / compiler flag → `LOOKUP` (spec §6.1.4.C explicitly lists "global configuration or compiler mode" as a lookup).

**Examples.**

| # | Language | Surface form | Verdict | Reason (basis) |
|---|---|---|---|---|
| + | Java | `a + b` on `int` | `COUNT` (wraps, two's complement) | JLS §15.18.2, §4.2.2 |
| + | Swift | `a &+ b` / `a + b` | `COUNT` (wraps / traps) | The two spellings pin two different determinate behaviours. Swift Reference, "Overflow Operators"; Standard Library `+(_:_:)` |
| + | Go | `a / b` | `COUNT` (run-time panic when `b == 0`; integer overflow wraps) | Go spec, "Arithmetic operators", "Integer overflow" |
| + | Python | `a + b` on `int`; `a // 0` | `COUNT` (unbounded — overflow cannot occur; `ZeroDivisionError`) | Python Reference §3.2 "Numeric types", §6.7 |
| − | C++ | `a + b` on `int` | `INDET` | Signed integer overflow is undefined behaviour. [expr.pre] |
| − | Rust | `a + b` on `i32` | `LOOKUP` | Panic vs two's-complement wrap depends on the `debug-assertions`/`overflow-checks` compilation mode. 1 hop (compiler mode). `a.wrapping_add(b)` is `COUNT`. Rust Reference, "Overflow" |
| − | Zig | `a + b` on `i32` | `LOOKUP` | Detected in Debug/ReleaseSafe, illegal behaviour in ReleaseFast — build-mode dependent. 1 hop. `a +% b` is `COUNT`. Zig Language Reference, "Integer Overflow" |

---

#### K10 — Allocation, copying, moving, borrowing, or destruction

**Definition.** What this site does to the *ownership and storage* of values: does it allocate (and where — stack/heap/static), copy (shallow or deep), move/transfer ownership, borrow/reference without owning, or destroy/release.

**Counts when.** The local form plus the language rules determine the answer, including "no allocation" and "copies the reference, never the object".

**Does NOT count when.** The answer depends on the operand's type declared outside the probe region (value type vs reference type vs COW type) → `LOOKUP`. Where the language *universally* specifies that object creation/identity is left to the implementation, the row is `COUNT` under **Rule 2.5.1** with the value naming that rule, not `INDET`; `INDET` remains correct where the outcome is undefined/erroneous or runtime-data-dependent.

**Examples.**

| # | Language | Surface form | Verdict | Reason (basis) |
|---|---|---|---|---|
| + | C++ | `auto w = std::move(v);` / `std::vector<int> v(n);` | `COUNT` (move / heap allocation) | [vector.cons]; [class.copy.ctor], `std::move` contract |
| + | Rust | `let b = Box::new(x);` | `COUNT` (heap allocation, ownership moved into `b`) | Rust Reference, "Box"; `alloc::boxed` contract |
| + | Go | `s := make([]int, n)` | `COUNT` (allocates a backing array) | Go spec, "Making slices, maps and channels" |
| + | Java | `a = b;` with class types | `COUNT` (copies the reference; never copies the object) | JLS §15.26.1 |
| + | Python | `c = 0` | `COUNT` (`basis_class = universal-rule`) | A universal rule pins the fact: object identity for immutable built-ins is not committed — an implementation may create a new object or reuse a cached one. The reader learns a real and important fact (do not rely on identity here), exactly as a C++ reader learns one from `int x;` under K04. Rule 2.5.1. Python Reference §3.1 |
| − | Go | `y := x` where `x`'s type comes from an import | `LOOKUP` | Struct/array copy vs slice/map header sharing depends on the type. ≥1 hop. |
| − | Swift | `let a = arr` on `Array` | `COUNT` (value semantics; copy-on-write is an unobservable optimisation) | Swift Reference, "Structures and Enumerations Are Value Types"; `Array` documented value semantics |

---

#### K11 — Externally visible side effects

**Definition.** Whether this site produces an effect observable **outside the probe region**: I/O, mutation of shared/global/static state, mutation through a reference handed in from outside, synchronisation, foreign-function effects, or observable non-determinism.

**Frozen boundary rule.** Modifying a local entity that is declared *inside* the probe region is **not** an externally visible side effect; that is K02/K04. This keeps K11 from becoming a free per-statement fact for every language.

**Counts when.** The local form plus the language rules (or a named standard-library contract, §2.2) determine the presence and nature of the effect — including a determinate "no externally visible effect".

**Does NOT count when.** The effect is performed by a callee whose body/contract is outside the probe region → `LOOKUP`; or the effect depends on dynamic dispatch to an unknown implementation → `LOOKUP`; or ordering/visibility is unspecified because the outcome depends on which run-time interleaving occurs (e.g. unsynchronised concurrent access) → `INDET`. Rule 2.5.1 does **not** apply to that case: a data race's outcome is runtime-data-dependent, not a universally specified rule the reader can state.

**Examples.**

| # | Language | Surface form | Verdict | Reason (basis) |
|---|---|---|---|---|
| + | Go | `fmt.Println(x)` | `COUNT` (writes to standard output) | `fmt` package contract (`hops_decl = 0`, `hops_stdlib = 1`) |
| + | Python | `print(x)` | `COUNT` (writes to `sys.stdout`) | Python Library Reference, `print` |
| + | Rust | `println!("{}", x)` | `COUNT` (writes to standard output, locks stdout) | `std::println!` contract |
| − | C++ | `f(x)` (user-defined `f`) | `LOOKUP` | Any global/IO effect is in the callee. ≥1 hop. |
| − | Java | `obj.run()` | `LOOKUP` | Dynamic dispatch: the executed body depends on the runtime class. ≥1 hop (interface/implementation set). |
| − | Kotlin | `x.value = 1` where `value` is a property with a custom setter declared elsewhere | `LOOKUP` | Assignment syntax may invoke arbitrary code. ≥1 hop; also a Hidden Semantic Cost item. |

---

#### K12 — Control-flow effect

**Definition.** What this site does to the flow of control: falls through, branches, loops, exits early, propagates, throws, suspends/resumes, diverges (never returns), or transfers to another thread/task.

**Frozen distinction from K07.** K07 asks *can it fail and how is failure signalled*; a universal rule can answer that. K12 asks *where control goes*; a universal rule ("any call may throw") does **not** answer it, because the call may also diverge, loop forever, suspend, or transfer. Therefore an opaque call site is `LOOKUP` for K12 even where it is `COUNT` for K07.

**Counts when.** The local form plus the language rules determine the control-flow effect of the construct itself — early exit, propagation, loop, branch, deferred execution, suspension point.

**Does NOT count when.** The effect depends on the callee's body or contract → `LOOKUP`; or the construct's termination is data-dependent and unresolvable from the probe region → `INDET`.

**Examples.**

| # | Language | Surface form | Verdict | Reason (basis) |
|---|---|---|---|---|
| + | Rust | `expr?` | `COUNT` (may return early from the enclosing function) | Rust Reference, "The question mark operator" |
| + | Zig | `try f()` | `COUNT` (may return the error early) | Zig Language Reference, "try" |
| + | Swift | `guard … else { return }` | `COUNT` (mandatory early exit on the false path) | Swift Reference, "Early Exit" |
| + | Go | `defer cleanup()` | `COUNT` (schedules execution at function return) | Go spec, "Defer statements" |
| + | Kotlin | `x ?: return null` | `COUNT` | Kotlin spec, "Elvis operator", "Return expression" |
| − | Python | `f()` | `LOOKUP` | The callee may return, raise, or never return. ≥1 hop. |
| − | Java | `f()` | `LOOKUP` | Same; plus dynamic dispatch. ≥1 hop. |

---

#### K13 — Lifetime / resource effect

**Definition.** How long the entity lives, at which point it is released, and whether the site acquires or releases a non-memory resource (file, socket, lock, handle) with a determinate release point.

**Counts when.** The local form plus the language rules determine the release point (scope exit, block exit, explicit `defer`/`close`, end of borrow) or determinately establish that no cleanup occurs.

**Does NOT count when.** The lifetime depends on ownership established outside the probe region (an owner, arena, pool, or container declared elsewhere) → `LOOKUP`; or the release point depends on a runtime-data-dependent property that no universal rule states → `INDET`.

**Frozen reclamation rule (Rule 2.5.1 applied to K13, identically for all 10 languages).** Where the language *universally* fixes the reclamation discipline, the row is `COUNT` and the **value** names that discipline. What differs between languages is the *value*, never whether the row counts:

| Discipline | Value recorded | `basis_class` |
|---|---|---|
| Scope/ownership-bound destruction (C++ automatic storage, Rust `Drop`, Zig `defer`, a language's `using`/`with`/`try`-with-resources block) | "released at the point the form names" | `local-marker` where a keyword names the point, else `universal-rule` |
| Tracing garbage collection (Java, Kotlin/JVM, Go, Python's cycle collector, TypeScript/JS) | "no deterministic release point; reclamation occurs by the garbage collector at an unspecified time" | `universal-rule` |
| Reference counting / managed retain-release (Swift ARC, and **any** language of the fixed set whose managed references are reference-counted — **Quidra's managed retain/release is adjudicated by this same row**) | "released when the last strong reference is released; the point is fixed by the whole program's reference graph, not by this local form" | `universal-rule` |

A "no deterministic release point" value is a *worse* answer for the reader than "released at scope exit", and the benchmark measures that difference where the specification puts it — in Semantic Determinacy (B), Semantic Locality (C, via methodology 03) and Hidden Semantic Cost (D) — **not** by deleting the row from Semantic Density. Deleting it would hand every fact-per-token point at a scope-exit site to the scope-bound languages, which under §1.1/S6 is nearly every probe.

**Examples.**

| # | Language | Surface form | Verdict | Reason (basis) |
|---|---|---|---|---|
| + | C++ | `std::lock_guard<std::mutex> g(m);` | `COUNT` (acquires now, releases at scope exit) | [thread.lock.guard]; [class.dtor] |
| + | Python | `with open(p) as f:` | `COUNT` (closed at block exit, deterministically) | Python Reference §8.5; `io` contract |
| + | Go | `defer f.Close()` | `COUNT` (released at function return) | Go spec, "Defer statements" |
| + | Java | `try (var r = open()) { … }` | `COUNT` (closed at block exit) | JLS §14.20.3 |
| + | Java | `Object o = new Object();` | `COUNT` (`universal-rule`) | Universal rule: no deterministic release point; reclamation is by the garbage collector at an unspecified time, and no local form can change that. JLS §12.6 |
| + | Swift, **and any reference-counted managed language of the fixed set including Quidra** (pre-adjudication row, frozen before annotation — see invariant I1) | `let o = Obj()` (class instance / managed reference) | `COUNT` (`universal-rule`) | Universal rule: released when the last strong reference is released; the point is fixed by the whole program's reference graph, not by the local form. **The same disposition, the same `basis_class` and the same class of value apply to Quidra's managed retain/release; an annotator may not adjudicate Quidra's reclamation differently from Swift's.** An *explicit* release/`defer`/scope-bound form in any language, Quidra included, records the named point instead and takes `basis_class = local-marker`. Swift Reference, "Automatic Reference Counting" |
| − | Rust | `let f = File::open(p)?;` | `COUNT` (dropped, and the file closed, at end of scope) | Rust Reference, "Destructors"; `std::fs::File` `Drop` contract |

---

## 2. "Explicitly recoverable" — the local-surface-form test

### 2.1 The Local Knowledge Set (LKS)

A fact is **explicitly recoverable** at site *s* if and only if a competent reader can derive a single determinate value for it using **only** the Local Knowledge Set:

1. **The complete text of the probe region** of that probe in that language (§4.2) — every token between the frozen `BEGIN PROBE` / `END PROBE` markers, including any declaration the probe itself contains.
2. **The language's own specification / reference manual**, at the version frozen in `../environment/environment.json`.
3. **The published contract of a standard-library entity whose name appears literally at the site or in the probe region**, at the frozen version — and nothing reached *through* it.

Anything else is outside the LKS.

### 2.2 The two hop counters

Every annotated row carries two integers:

- **`hops_decl`** — the number of **declaration-graph hops** required outside the probe region: callee signature, variable declaration, type declaration, overload set, trait/interface/protocol implementation, imported user definition, global configuration or compiler mode. (This is exactly the list in spec §6.1.4.C.)
- **`hops_stdlib`** — the number of **nominal standard-library contract consultations**: reading the documented semantics of a standard-library entity *named literally in the probe region*.

**Frozen recoverability test:**

> A row is `COUNT` **iff** it is applicable, behaviourally determinate, **`hops_decl == 0`**, and **`hops_stdlib ≤ 1`**.

A user-declared entity that is declared **inside** the probe region contributes `hops_decl = 0` — its declaration *is* local surface form. A standard-library name is admitted at `hops_stdlib = 1` because the name itself is the local evidence (`wrapping_add` says what it does; `try` says it can fail; `println` says it writes). Reaching a *second* level (the contract of something the first contract returns) is `hops_stdlib = 2` and is **not** recoverable.

This rule is applied identically to all 10 languages. It is the only reading under which the metric retains resolution: excluding all library names would zero out nearly every I/O, collection, and concurrency probe in every language simultaneously. What a named contract may be used to establish is bounded by the **absence-facts rule** in §1.4: the contract must actually state the value recorded, and an absence it does not mention is `LOOKUP`, not `COUNT`.

**`hops_decl` and `hops_stdlib` are inputs to this `COUNT` test and to nothing else.** They are annotation aids recorded for audit; they are **never** summed into Semantic Locality's `H_i`, which methodology 03 computes by its own algorithm from its own records (§2.3).

### 2.3 Interaction with Semantic Locality (metric C)

**There is no shared ledger, and this document does not compute Semantic Locality.** Metric A (Semantic Density) is computed from the per-`(site, fact)` ledger defined here (§2.6). Metric C (Semantic Locality) is computed by **methodology 03's per-probe minimum-resolving-set algorithm**, from 03's own per-probe records and its own record schema, and **methodology 03 is authoritative for every hop count in the benchmark**. The `hops_decl` / `hops_stdlib` integers recorded in this ledger are annotation aids and inputs to the `COUNT` test of §2.2 only; **they are never summed into `H_i` and never enter metric C.**

*(A previous draft of this section asserted that "the fact ledger is shared" and that "the two metrics cannot disagree about a fact". That assertion was false against the frozen methodology 03 — which defines Locality independently, uses a different record schema, and states in Rule 2.2.2 that standard-library declarations are hops on the same terms as user declarations — and it is withdrawn. Nothing downstream may rely on it.)*

The two metrics ask different questions and are allowed to answer differently about the same construct. Because that is the sort of divergence that can hide a free ride, every row's treatment under **both** metrics is written out here:

| Row in this ledger | Semantic Density (A), computed here | Semantic Locality (C), computed by methodology 03 |
|---|---|---|
| `hops_decl == 0` and `hops_stdlib == 0` | `COUNT` → **+1** to the numerator | 0 hops |
| `hops_decl == 0` and `hops_stdlib == 1` (a standard-library contract named literally in the probe region) | `COUNT` → **+1** to the numerator | **1 hop.** 03 Rule 2.2.2 counts standard-library declarations as hops on the same terms as user declarations, and that rule stands unchanged. |
| `hops_decl ≥ 1`, or `hops_stdlib ≥ 2` | `LOOKUP` → 0 | its own hop count, computed by 03's algorithm (which may differ from the integers recorded here, and 03's value governs) |
| `INDET` | 0 | **03 Rule 2.7.1 applies: the fact is unresolvable, so the probe records `H_i = SAT = 6`**, the worst bounded value. An `INDET` fact is never treated as a small or zero locality cost. |
| `DUP`, `NA` | 0 | nothing |

**The one declared divergence, and why it is not a free ride.** A single named standard-library consultation is admitted by Density's `COUNT` test (§2.2) and is simultaneously charged as one hop by Locality. The row therefore **earns density and pays locality**; there is no path by which a row earns density while escaping its locality cost, and no path by which a row is invisible to both metrics. The divergence is deliberate, pre-registered, and applies identically to all 10 languages: Density asks whether the local surface form plus one named contract pins the fact, Locality asks how much declared material a reader must open. Both answers are published.

> **Counterpart obligation (cross-document).** Methodology 03 §2.2.2 must carry an explicit cross-reference naming this section: *"Standard-library declarations are hops on the same terms as user declarations. Methodology 02 §2.2 admits one such consultation into Semantic Density's `COUNT` test; that admission is confined to metric A and does not reduce any hop count here."* Methodology 03 §2.7 must likewise state: *"An `INDET` fact in the 02 ledger contributes 0 to Semantic Density and triggers Rule 2.7.1 here."* Until 03 carries both sentences, the two documents are read together with **this** section governing the boundary and **03** governing every hop count.

**Optional lookups are never counted** (spec §6.1.4.C): if the fact is already determinate from the LKS, a reader's optional deeper inspection adds nothing to `hops_decl`.

### 2.4 Minimality of hop counts

`hops_decl` is the **minimum** number of distinct external declarations that must be consulted, counted as distinct declaration entities (not as file opens, not as re-reads). Resolving an overload set counts as **1** hop regardless of the number of candidates; the candidate count is Determinacy (B) evidence, not Locality evidence.

### 2.5 Determinacy requirement

A row is `COUNT` only if the LKS yields a **single behaviourally determinate answer**. The following yield `INDET`:

- the specification says *undefined behaviour*, or *erroneous/illegal behaviour with no defined outcome*;
- the specification says *implementation-defined* **and** the fact kind's content is the behaviour itself (K06, K07, K09, K10, K11, K13) **and Rule 2.5.1 does not apply**. For **K05 only**, implementation-defined *representation* does not block the row; see the representation sub-rule in §1.4/K05;
- the answer is a runtime-data-dependent property that no static reading can pin (e.g. which dynamic type an open interface value holds, which interleaving a racing program takes, which branch a data-dependent loop follows).

**Rule 2.5.1 — universally specified indeterminacy is a counted fact (frozen, applied identically to every fact kind and every language).** Where a language rule **universally** specifies that an outcome is unspecified, non-deterministic, or left to the implementation, **and that rule is the same at every site so that no local form could have selected a different answer**, the row is **`COUNT`**, with `basis_class = universal-rule` and a `value` that names the rule itself — for example "no deterministic release point; reclamation occurs by the garbage collector at an unspecified time", "object identity is not committed; the implementation may create a new object or reuse a cached one", "the value is indeterminate until first assignment". The reader has learned a real and important fact — *do not rely on this* — and §6.1.1 measures communicated program meaning, not comfortable meaning.

`INDET` is reserved for (i) **undefined / erroneous behaviour**, where the program has no meaning at all and the reader learns nothing they may rely on, and (ii) **runtime-data-dependent** properties, where the answer exists but varies per execution and no rule states it.

**This reading is applied identically to K04, K10 and K13**, which previously treated the identical structure three different ways: K04 counted C++ `int x;` ("determinately uninitialized") while K10 marked Python `c = 0` `INDET` and K13 marked Java and Swift reclamation `INDET`. All four are now `COUNT` with the value naming the rule. The correction removes a systematic transfer of one-to-two facts per probe from the six GC/ARC languages to the scope-bound ones at S6 scope-exit sites, which occur in essentially every probe.

**Compiler-mode dependence is `LOOKUP`, not `INDET`** — the frozen build recipes in `../environment/environment.json` do resolve it, but consulting them is exactly a "global configuration or compiler mode" hop under §6.1.4.C. This is applied uniformly (it affects Rust overflow checks, Zig build modes, TypeScript `strict`/`noUncheckedIndexedAccess`, C++ `-O`-independent UB, and any Quidra mode flags identically).

**The magnitude of the compiler-mode rule is published, not left invisible.** The rule is language-neutral as written, but its *measured effect* is not symmetric: a language whose behaviour is fixed by the language itself pays nothing under it, while a language that exposes the same behaviour as a build mode loses the row at every site. Rust and Zig lose K09 at plain arithmetic sites, TypeScript loses K07 at indexing sites, and a language whose checking is always on — Quidra's checked arithmetic among them — loses nothing. §5.5 therefore requires `density.json` to publish `mode_driven_rows: {language: {fact_kind: count}}`, counting the rows whose disposition is `LOOKUP` **solely** because of a compiler-mode dependency, so that an auditor can see exactly how much of the density spread this single rule produced for each language.

> **Counterpart obligation (cross-document).** The frozen build recipes in `../environment/environment.json` and the constraints in `00_cross_language_constraints.md` must not pair a language's checks-enabled configuration against another language's checks-disabled configuration. Each language is built in the configuration a competent professional would ship, the choice is recorded per language with its reason, and where a language offers both, the recipe states which was chosen and why. A run that compares an always-checked language against another language's checks-disabled release mode is measuring the recipe, not the languages.

### 2.6 Required ledger record

Facts are recorded as JSON Lines at
`../semantic-compression/raw/facts/<language>/<probe_id>.jsonl`, one object per annotated row.

**Probe identifiers (frozen, single authoritative scheme).** `probe_id` is always the identifier used by the frozen capability/probe document `01_capability_universe_and_probes.json`: **`F<nn>.P<k>`**, `F01.P1 … F20.P2`, where `F<nn>` is the capability family of spec §6.1.2 and `P<k>` is the probe within it. No other probe-identifier form is used anywhere in this document, in the ledger, in the tokenizer output, or in `density.json`. (This is why the thirteen fact kinds are labelled **`K01`–`K13`** rather than `F01`–`F13`: the `F<nn>` prefix belongs to capability families, and the two labelings must not collide. `K01`–`K13` are the same thirteen kinds of spec §6.1.3 in the same order; only the label changed.)

```json
{
  "probe_id": "F08.P1",
  "language": "rust",
  "site_id": "s4",
  "site_class": "S2",
  "span": {"line": 2, "col_start": 5, "col_end": 26},
  "construct": "c.wrapping_add(1)",
  "fact_kind": "K09",
  "disposition": "COUNT",
  "value": "two's-complement wraparound on overflow",
  "basis": "core::num::i32::wrapping_add contract, Rust 1.95.0 std docs",
  "basis_class": "stdlib-contract",
  "hops_decl": 0,
  "hops_stdlib": 1,
  "note": ""
}
```

`disposition ∈ {COUNT, LOOKUP, INDET, DUP, NA}`. `basis_class ∈ {local-marker, universal-rule, stdlib-contract}` and is required for `COUNT`. `note` is mandatory and non-empty for `INDET`, `NA`, and for any `LOOKUP` produced by the absence-facts rule of §1.4 (this is the §26 "document the reason" requirement and the §32 anti-fabrication guard).

`site_id` is `s<n>` for a site and `s<n>.a<k>` / `s<n>.d<k>` for an argument, field-initializer or declarator sub-site (§1.1/E7).

**`basis_class` selection order (frozen).** `basis_class` is not left to annotator preference, because §5.5's `basis_class_histogram` is the audit hook for invariant I5 and the reading-(b) recomputation is computed from this field. Where more than one basis applies, `basis_class` takes **the first that applies in this fixed order**:

1. **`local-marker`** — a lexeme *of the language* (keyword, modifier, operator, sigil, annotation, punctuation) physically present at, or syntactically governing, the site selects the value. A standard-library *name* is not a local marker.
2. **`stdlib-contract`** — a standard-library entity named literally in the probe region states the value.
3. **`universal-rule`** — a language rule that admits no alternative at any site states the value.

Where a second or third basis independently pins the same value, it is named in `note` (e.g. `"also pinned by the zero-value rule"`). This ordering is deliberately conservative for I5: it attributes a fact to an explicit marker whenever one exists, which is the attribution *least* favourable to the claim that reading (a) is neutral, and therefore the one that makes the published histogram hardest to game.

### 2.7 Audit requirement

After annotation, a re-annotation audit is run over a sample of the `(language, probe)` pairs. The sample is **10% of all pairs, selected with the fixed seed `7677581`** by sorting all pairs lexicographically by `(language, probe_id)` and drawing without replacement using Python's `random.Random(7677581).sample`, **plus 100% of the Quidra pairs**. Quidra is audited in full for a stated reason: invariant I1 has Quidra annotated **last**, after the other nine languages' counts are already known, which is the one position in the schedule where an annotator could — even unintentionally — adjudicate toward a target. Full auditing removes that opportunity. Any other language annotated last would be audited at 100% on the same grounds; the rule is positional, not about Quidra.

**A1 — Independence.** The re-annotation is performed by a **second analyst who did not produce the first ledger for that pair**, working from this document alone, without reading the first ledger and without seeing any other language's counts for that probe. This matches the two-analyst standard methodology 03 §3.2 applies to Determinacy and Locality; the two metrics are held to the same process standard even though they are computed from separate records (§2.3).

**A2 — Which count is scored.** A disagreement in total `COUNT` rows of more than **±5%** for a sampled pair triggers re-adjudication of that pair against the rules in §1–§3 by both analysts jointly. **Where re-adjudication finds the first annotation wrong under §1–§3, the corrected count is the value that enters the score**, and the correction is applied to **every non-sampled pair exhibiting the same pattern**, in every language, before any score is computed. Where re-adjudication finds the first annotation correct, the first count stands. The adjudication, both counts, the rule invoked, and the list of non-sampled pairs corrected by propagation are published in `../semantic-compression/raw/facts/audit.json`. The scored value is never left undefined by a disagreement.

**A3 — Corpus-level failure criterion.** If **more than 20% of sampled pairs require re-adjudication**, the sample has demonstrated that the ledger is not reproducible from this document, and **the full ledger is re-annotated for all ten languages before any Semantic Density score is computed**. This is a corpus-level gate with a stated threshold, not a per-pair note.

**A4 — The rules are never changed to resolve a disagreement** (§32). A disagreement that cannot be resolved by the rules as written is published as unresolved, with both counts and the reason, and the defect procedure in §7 applies.

---

## 3. Anti-double-counting

### 3.1 The rule

- **A3.1 — One fact per `(site, fact kind)`.** A fact kind is annotated at most once per site. The same fact spelled twice in one probe counts once. A sub-site created by E7 (`s<n>.a<k>`, `s<n>.d<k>`) is a site for this rule: each argument position carries its own single row per kind, and the enclosing call carries rows for its result and its control/effect facts only. This is what allows `f(a, b, c)` — one argument by value, one a writable alias, one implicitly converted — to record three different K03/K06/K10 answers instead of forcing one row to stand for three, which no two annotators would fill in the same way.
- **A3.2 — Disjoint kinds are separate facts.** The thirteen kinds ask thirteen disjoint questions. Answering two of them at one site is two facts, even when one answer *entails* the other under the language rules. **Entailed facts count.** This is deliberate: the ability to communicate many facts from little syntax is precisely what Semantic Density measures (§6.1.1). The ceiling of 13 facts per site and the mandatory `basis` citation bound this.
- **A3.3 — Different sites, same value, same entity → `DUP`.** If a fact about the *same entity or the same operation* with the *identical value* has already been counted at another site of the same probe, the later row is `DUP` and counts 0. (Rule E4 in §1.1 prevents most of these from arising at all.)
- **A3.4 — Different entities → not duplicates.** Two variables each having their own mutability fact are two facts; they are different subjects.
- **A3.5 — Decorative syntax earns nothing and still costs tokens.** A construct whose removal would change no fact *value* at any site is decorative: every row it would create is `DUP` (if it restates a counted fact) or `NA` (if it corresponds to no taxonomy kind). Its tokens remain in the Semantic Density denominator. Decorative syntax therefore strictly *lowers* density — which is the intended direction.
- **A3.6 — Redundant grouping is not even a site** (rule E5).
- **A3.7 — No fact for being well-formed.** Compiling, type-checking, or satisfying a linter is not a taxonomy fact.

### 3.2 Worked examples

| Language | Surface form | Adjudication |
|---|---|---|
| Rust | `let mut count: i32 = 0i32;` | The type is spelled twice (annotation `i32` and literal suffix `0i32`). **One** K05 fact at the binding site; the literal is not a site (E3). Both spellings still cost tokens: 9 tokens vs 7 for `let mut count: i32 = 0;`. Net effect: lower density. |
| TypeScript | `const x: number = 5 as number;` | `as number` is an S2 conversion site, but its K05/K06 values are identical to those already established → both rows `DUP`, 0 facts, +3 tokens. Decorative. |
| Java | `@Override public void run() { … }` | `@Override` maps to no taxonomy kind (it asserts that an override exists; that is not one of K01–K13) → `NA`, 0 facts, +2 tokens. Contrast `final int x = 5;`, where `final` changes the K02 *value* and is therefore not decorative. |
| C++ | `const int x = 5;` and later `const int& r = x;` | Two S1 sites, two different entities → K02 counts at each. Not double counting (A3.4). |
| Go | `var x int = 0` vs `x := 0` | Identical facts (type `int` is pinned either way — by annotation, or by the untyped-constant default type rule). 5 tokens vs 3. The verbose form has strictly lower density. Go spec, "Constants" (default type of an untyped integer constant is `int`). |
| Swift | `let x: Int = 5` | The annotation duplicates the type already pinned by the literal → **one** K05 fact, +2 tokens. No penalty beyond the tokens. |
| Rust | `#[derive(Clone)] struct P { … }` | Not decorative: it changes the K10 value at the S4 type-definition site (the type acquires copy-by-clone semantics). Counts once, at S4. |
| Python | `x = int(5)` | The explicit `int(...)` call is an S2 conversion site whose K06 value ("identity conversion, no change") restates the type already pinned by the literal → `DUP`. +3 tokens, 0 facts. |

---

## 4. Language-neutral token-counting rule

This section discharges spec §6.1.4.A: *"Comments and whitespace do not count as source tokens. Use a documented language-neutral token-counting rule or a language lexer with an explicit reconciliation rule so punctuation-heavy and word-heavy syntaxes are treated consistently."*

We do **both**: a single documented tokenizer (§4.3–§4.6) that is authoritative for all 10 languages, plus a reference-lexer reconciliation procedure (§4.7) wherever a reference lexer exists on the measurement host.

### 4.1 Governing principle

The denominator measures **mandatory syntactic surface**, not authorial verbosity. Two distinct sources of token-count variation must be separated:

- **Grammar-imposed tokens** (C++ requires `;`, Java requires a type before every declarator, Zig requires `;`). These **stay**. Measuring them *is* the metric: a language that demands more mandatory punctuation per fact has lower semantic density. Erasing this difference would erase the metric.
- **Author-discretion tokens** (an optional semicolon, a redundant parenthesis, a comment, an unused import, a longer identifier). These are **canonicalised away** identically in every language (§4.2, R1–R7), so that nobody's score depends on who typed the probe — an optional terminator by the freeze-time check R7, a redundant grouping parenthesis by R4, comments and whitespace by R2.

Neutrality between punctuation-heavy and word-heavy syntaxes is achieved by the **uniform unit rule**: *every lexeme is exactly one token, whatever its length or class.* `func` is one token and `{` is one token; `wrapping_add` is one token and `+%` is one token. No class is weighted, discounted, or exempted. This is the only rule that treats "word-heavy" and "punctuation-heavy" symmetrically without a subjective weighting table. **This guarantee is unconditional and §4 contains no exception to it.** §4.4(a) previously carved one out for string-interpolation holes; it has been brought back under the uniform unit rule, so that a hole pays for the delimiters the author writes exactly as a call form pays for its parentheses. Any future clause that discounts or exempts a lexeme class contradicts this paragraph and is a defect under §7, not a refinement.

### 4.2 Canonicalisation rules (applied before counting, identically to all 10 languages)

- **R1 — Probe region.** Each probe file as presented to the tokenizer contains exactly one region delimited by two marker comment lines (spelled in that language's line-comment syntax): `BEGIN PROBE <probe_id>` and `END PROBE <probe_id>`, where `<probe_id>` is the `F<nn>.P<k>` identifier of §2.6. **Only tokens strictly between the marker lines are counted**, and only sites inside the region are annotated (§1.1/E6).
  **R1b — The markers are harness output, not fragment content.** The frozen probe document (`01_capability_universe_and_probes.json`) defines each probe's `measured_fragment` boundary in its own terms; it does not contain marker comments and is not required to. **The measurement harness writes each frozen fragment into its runnable wrapper and emits the two marker comment lines itself**, immediately outside the `measured_fragment` boundary that 01 declares, before invoking the tokenizer. The marker lines are therefore never part of fragment content, are removed by R2 before counting, and create no site. 01's authoring rule `R3_minimality` ("no comments") applies to fragment content only and is not violated by harness-emitted markers. This is the same procedure for all ten languages, and no fragment author writes a marker by hand. Scaffolding that the language requires in order to have a runnable file at all — `package main`, `public class Main { public static void main(String[] a) {`, `fn main() {`, `const std = @import("std");`, `int main() {`, closing braces — lives outside the markers.
  **Exception R1a:** for probes in capability families **18 (modules/imports/dependency boundaries), 19 (concurrency/asynchrony), and 20 (FFI/interoperability)**, the construct under test *is* the boundary construct, so the relevant import/spawn/extern declarations are placed **inside** the markers and are counted and annotated. This is decided per probe in the frozen probe document, before any measurement.
- **R2 — Comments and whitespace are not tokens.** Line comments, block comments, doc comments, and all whitespace including newlines are removed and contribute nothing. Marker comment lines are removed with them.
- **R3 — No synthesis of invisible tokens.** Virtual/implicit tokens are never added: no Python `NEWLINE`/`INDENT`/`DEDENT`/`ENDMARKER`, no Go or JavaScript automatically-inserted semicolons, no implicit block delimiters for layout-based syntax. Whatever the author did not write is not counted.
- **R4 — Redundant grouping parentheses are removed.** R4 removes **only** a parenthesis pair that is purely grouping: a pair whose removal leaves the same parse and changes no fact *value* at any site under §1 (§1.1/E5 already gives it no site). `optional_lexemes` in the profile schema (§4.6) is **restricted to grouping parentheses**; no profile may list a statement terminator there. R4 is a purely lexical post-pass and is the same in every language.
  *Why R4 no longer removes terminators:* the earlier form of R4 required a per-occurrence judgement ("the grammar permits omitting it **at that position**") that the profile implemented as a static per-language `optional_lexemes` list applied blindly, and the two do not agree — TypeScript ASI and Swift's newline rules make `;` removability position-dependent. Worse, the test was circular: it asked whether removal changes a fact value at a site, while the site enumeration is performed on the region R4 helps define. The obligation is therefore moved to fragment freezing, where it is checked once and mechanically (R7).
- **R7 — No omissible statement terminators at freeze time.** No frozen fragment may contain a statement terminator that its own grammar permits omitting at that position. This is **verified at freeze time**, before any measurement, by review against the language's grammar, and the verification is recorded per fragment. A fragment that contains one is corrected and re-frozen, not silently re-tokenized. Consequently a terminator that survives into a measured fragment is **mandatory**, and is counted: C++, Java, Zig and Rust pay their mandatory `;`, and Rust's `;` is counted in every case because it also carries K01/K12 fact values. R7 does not touch a Python trailing comma that makes a tuple, which is not a statement terminator and is not omissible.
- **R5 — Identifier length is free.** Every identifier is one token regardless of spelling, so naming style cannot move a score. Qualified names are *not* one token: `fmt.Println` is `fmt` `.` `Println` = 3 tokens; `std::cout` is `std` `::` `cout` = 3 tokens. This is the same rule in both directions.
- **R6 — No dead code.** The probe region contains no unused declarations, no unreachable statements, and no debugging output beyond what the probe's stated task requires. Verified by review before freezing the probes.

### 4.3 Token classes and the one-token unit

Every lexeme below is **exactly one token**. Class tags exist only for the audit trail in §4.7; they never change the count.

| Tag | Class | Rule |
|---|---|---|
| `IDENT` | Identifier | Any identifier, including contextual keywords, type names, field names, labels, macro names, and back-tick- or `@`-quoted identifiers (Kotlin `` `is` ``, Zig `@"x y"` — the quoting is part of the single identifier token). |
| `KW` | Reserved word | Any reserved word of the language. |
| `NUM` | Numeric literal | The **entire** literal is one token: base prefix, digits, digit separators, decimal point, exponent, and type suffix. `0xFF_FFu32`, `1'000'000ULL`, `1_000L`, `3.14e-2f`, `0b1010`, `100n` are one token each. |
| `STR` | String / char literal | A non-interpolated string, raw string, multi-line string, or character/rune literal is one token, escapes included. Interpolated strings: see §4.4. |
| `OP` | Operator | Maximal munch: the **longest operator lexeme** in the language's operator table is one token. `==`, `!=`, `<=>`, `<<=`, `&&`, `||`, `->`, `=>`, `::`, `?.`, `??=`, `...`, `..=`, `+%`, `&+`, `?:`, `!!`, `|>` are one token each. `?` (Rust propagation), `!` (Swift force-unwrap), `&` (address/borrow), `*` (dereference) are one token each. |
| `DELIM` | Delimiter / punctuation | Each of `( ) [ ] { } , ; : . @ # $ \| ->`-as-delimiter etc. is one token. `.` is one token whether it is read as an operator or a delimiter — the class does not affect the count. |
| `HOLE_OPEN` / `HOLE_CLOSE` / `HOLE` | Interpolation delimiter | One token per delimiter the author must actually write around an interpolation hole: `HOLE_OPEN` + `HOLE_CLOSE` for a two-delimiter form (`{`…`}`, `${`…`}`, `\(`…`)`), a single `HOLE` for a one-delimiter bare-sigil form (`$ident`). Identical accounting to a call form's `(` and `)`. See §4.4(a). |

Nothing else emits a token. In particular: whitespace, comments, line continuations, byte-order marks, and end-of-file markers emit nothing.

### 4.4 The four contested cases, resolved

**(a) String interpolation.** An interpolated string emits:

> `1` `STR` token for the literal shell, and for **each** embedded expression: **one token per delimiter the language actually requires the author to write** around the hole, plus the tokens of the embedded expression, lexed by the same rules.

Concretely: a hole written with an opening and a closing delimiter (`{`…`}`, `${`…`}`, `\(`…`)`) emits `HOLE_OPEN` + expression + `HOLE_CLOSE` = **2** delimiter tokens; a bare-sigil hole with a single written delimiter (`$c`) emits one `HOLE` token. This is the same accounting a call form receives, where `(` and `)` are each one token — and it is the accounting §4.1 and §4.3 already require of every other lexeme.

*Correction of a prior rule (recorded because it moved a score).* This section previously read "the hole costs exactly one token regardless of how the language spells it". That was the **only** class exemption in §4, and it contradicted §4.1's own neutrality guarantee ("no class is weighted, discounted, or exempted"). It charged one token for two written lexemes, a flat one-token-per-hole discount to exactly the five languages of the fixed set that have a braced or parenthesised interpolation form — **Python, TypeScript, Kotlin, Swift and Quidra** — while the five without interpolation — **C++, Go, Java, Rust and Zig** — paid every `(`, `,` and `)` of the call form separately under the same section's own rule. Tokens are the Semantic Density *denominator*, so the discount inflated the density of the favoured group, Quidra included. It is removed. (See also `CORRECTIONS.md` D-4, which records the same correction on the tokenizer side.)

| Language | Source | Tokens |
|---|---|---|
| Python | `f"x={c}"` | `STR` shell, `HOLE_OPEN`, `c`, `HOLE_CLOSE` → **4** |
| TypeScript | `` `x=${c}` `` | `STR` shell, `HOLE_OPEN`, `c`, `HOLE_CLOSE` → **4** |
| Kotlin | `"x=$c"` | `STR` shell, `HOLE`, `c` → **3** (one delimiter is written, so one is charged) |
| Kotlin | `"x=${c}"` | `STR` shell, `HOLE_OPEN`, `c`, `HOLE_CLOSE` → **4** |
| Swift | `"x=\(c)"` | `STR` shell, `HOLE_OPEN`, `c`, `HOLE_CLOSE` → **4** |
| Go | `fmt.Sprintf("x=%d", c)` | `fmt` `.` `Sprintf` `(` `STR` `,` `c` `)` → **8** |
| C++ | `std::format("x={}", c)` | `std` `::` `format` `(` `STR` `,` `c` `)` → **8** |

Languages without interpolation pay the call syntax, which is the honest measurement of their mandatory surface; languages with interpolation pay their two delimiters, which is the honest measurement of theirs. Concatenation (`"x=" + c`) is lexed normally with no special rule. A language whose interpolation genuinely requires fewer written delimiters pays less, and that is a real difference in mandatory surface, not an exemption.

*(The C++ row's stated total was 7 in an earlier draft while its own lexeme list showed 8; the lexeme list was right. Recorded as `CORRECTIONS.md` D-1.)*

**(b) Generic / type-argument brackets.** The tokenizer is **purely lexical**; it does not resolve whether `<` opens a generic argument list. Therefore:

| Language | Source | Tokens |
|---|---|---|
| Java | `List<String> xs` | `List` `<` `String` `>` `xs` → **5** |
| C++ | `std::vector<int> v` | `std` `::` `vector` `<` `int` `>` `v` → **7** |
| Rust | `Vec::<i32>::new()` | `Vec` `::` `<` `i32` `>` `::` `new` `(` `)` → **9** |
| Go | `Map[string, int]` (type args) | `Map` `[` `string` `,` `int` `]` → **6** |
| Zig | `fn f(comptime T: type)` | `fn` `f` `(` `comptime` `T` `:` `type` `)` → **8** |

Maximal munch applies to shift/compare operators, so C++ `vector<vector<int>>` closes with a single `>>` token (one token, per §4.3), and Rust's turbofish `::<` lexes as `::` + `<` (two tokens). These consequences are frozen and identical for every file.

**(c) Attribute / annotation syntax.** Attributes receive **no exemption and no penalty**: they are lexed like any other tokens.

| Language | Source | Tokens |
|---|---|---|
| Java | `@Override` | `@` `Override` → **2** |
| Rust | `#[derive(Clone)]` | `#` `[` `derive` `(` `Clone` `)` `]` → **7** |
| C++ | `[[nodiscard]]` | `[` `[` `nodiscard` `]` `]` → **5** |
| Kotlin | `@Suppress("x")` | `@` `Suppress` `(` `STR` `)` → **5** |
| Swift | `@inlinable` | `@` `inlinable` → **2** |
| Zig | `@intCast(x)` | `@` `intCast` `(` `x` `)` → **5** |

An attribute earns a fact only if the **language specification** (not a lint convention) attaches one of K01–K13 to it (§3.2).

**(d) Statement terminators.** Counted exactly as written. Under **R7** no frozen fragment contains a terminator its grammar permits omitting at that position, so every terminator that reaches the tokenizer is mandatory and is counted: C++, Java, Zig and Rust pay their mandatory `;`, and Rust's `;` is counted in every case because it additionally carries K01/K12 fact values. Go, TypeScript, Kotlin and Swift fragments contain no optional `;` at all, so they pay none — the same outcome the old blanket-removal rule aimed at, but decided once at freeze time by a grammar check instead of per-token by a circular semantic test. Python and layout-based syntax receive no synthesised terminator (R3).

### 4.5 The frozen tokenizer

- **Script:** `../scripts/tokenize_probe.py` (Python 3.14.5, standard library only, no network, deterministic).
- **Profiles:** `../scripts/token_profiles.json` — one lexical profile per language (§4.6).
- **CLI:** `python3 tokenize_probe.py --lang <L> --probe <path> [--json-out <path>] [--selftest]`
- **Output:** `../semantic-compression/raw/tokens/<language>/<probe_id>.json`:
  `{"probe_id":…, "language":…, "count": N, "tokens":[{"tag":"KW","text":"let","line":1,"col":1}, …], "class_histogram":{…}, "removed_by_R4":[…], "profile_sha256":…, "script_sha256":…}`
- **Authority:** this script is authoritative for all 10 languages. Reference lexers (§4.7) are audit instruments only. This matters for fairness: languages lacking a usable reference lexer are not counted by a different instrument from those that have one.
- **Provenance:** the SHA-256 of the script and of the profile file are recorded in every output and in `../semantic-compression/scores/density.json`.

**Algorithm (implementable as written):**

1. Read the file as UTF-8. The harness has already written the two marker comment lines around the frozen fragment (R1b). Locate them by exact text match on `BEGIN PROBE <id>` / `END PROBE <id>`, where `<id>` is the `F<nn>.P<k>` probe identifier; if either is missing or duplicated, **abort with an error** (never guess a region) — an abort here is a harness defect to be fixed before measurement, not a fragment defect. Take the text strictly between them.
2. Load the language's profile (§4.6). Sort the profile's operator table by descending lexeme length once, so that maximal munch is a simple ordered prefix match.
3. Scan left to right from position 0. At each position, try the recognisers in this fixed priority order and take the first that matches:
   1. **whitespace** (Unicode `White_Space`, including newlines) → consume, emit nothing;
   2. **line comment** (profile's line-comment starters) → consume to end of line, emit nothing;
   3. **block comment** (profile's block delimiters, honouring the profile's `nesting` flag: `true` for Rust, Kotlin, Swift; `false` for C++, Java, TypeScript, Go; absent for Python and Zig) → consume, emit nothing;
   4. **string or character literal** (profile's literal forms, honouring raw/multi-line/prefixed forms and the profile's escape character) → emit per §4.3/§4.4. For an interpolated form, emit `STR` for the shell, then for each hole emit one `HOLE_OPEN` token, recursively scan the hole's contents with the same tokenizer state, and emit one `HOLE_CLOSE` token — except for a single-delimiter (bare-sigil) hole form, which emits one `HOLE` token and no closer, per §4.4(a). The profile's `interpolation` entry states, per string form, whether the form has a closing delimiter;
   5. **numeric literal** (profile's numeric grammar: base prefixes, digit separators, exponent forms, type suffixes) → emit one `NUM`;
   6. **identifier or keyword** (profile's identifier start/continue character classes, plus the profile's quoted-identifier forms) → emit `KW` if the lexeme is in the profile's reserved-word list, else `IDENT`;
   7. **operator** (longest match from the sorted operator table) → emit one `OP`;
   8. **delimiter** (profile's delimiter set) → emit one `DELIM`;
   9. **no match** → **abort with an error naming the file, line, column and offending character.** There is no silent skip and no catch-all fallback; an unrecognised character is a profile defect to be fixed *before* any probe is measured, and the fix is a profile change that is re-applied to every probe and re-hashed.
4. Apply **R4** as a post-pass over the emitted token list, using the profile's `optional_lexemes` rule set, which may contain **grouping parentheses only** (§4.2/R4, §4.6). Each removed token is retained in `removed_by_R4` with the reason for its removal, for audit; removed tokens are not in `count`. The tokenizer performs **no** terminator removal: R7 has already guaranteed at freeze time that no omissible terminator is present, and if the profile lists any lexeme other than a grouping parenthesis in `optional_lexemes` the tokenizer **aborts** rather than removing it.
5. Emit the JSON. `count` is `len(tokens)` after step 4.
6. `--selftest` runs the frozen fixture set in `../scripts/token_fixtures.json`, which must include at minimum the §6 worked example (`rust = 17`, `go = 8`, `python = 6`) and **one fixture per row of every §4.4 table as those tables now stand** — including the interpolation rows at their corrected totals (Python `f"x={c}"` = 4, TypeScript `` `x=${c}` `` = 4, Kotlin `"x=$c"` = 3, Kotlin `"x=${c}"` = 4, Swift `"x=\(c)"` = 4, Go = 8, C++ `std::format` = 8), and the corrected Kotlin attribute row (`@Suppress("x")` = 5). Any mismatch is a hard failure and blocks measurement. Because some fixtures were run against an earlier, defective interpolation rule, **every probe file is re-tokenized in one authoritative pass with the current script before any density value is computed**, for all ten languages equally, and only those re-tokenized counts are used for scoring (`CORRECTIONS.md` D-4).

**Ambiguity resolutions frozen into the profiles** (these are the known lexical traps):

- **Rust lifetimes:** `'` followed by an identifier-start character and *not* terminated by a closing `'` is a lifetime/label token (one `IDENT`), not a character literal.
- **Rust raw strings / identifiers:** `r"…"`, `r#"…"#`, `b"…"`, `r#ident` handled by the profile's literal and identifier forms.
- **C++ digit separators:** `'` inside a numeric literal is a separator, not a character-literal delimiter, when it occurs between digits.
- **C++ raw strings:** `R"delim(…)delim"` consumed whole as one `STR`.
- **Zig:** no block comments; `\\` begins a multi-line string line (one `STR` per line); `@` is a `DELIM` and the following builtin name is an `IDENT` (so `@intCast` is 2 tokens); `@"x y"` is one quoted `IDENT`.
- **Python:** prefixed strings (`r`, `b`, `f`, `rb`, `br`, `u`) and triple-quoted strings are single `STR` tokens (or a `STR` shell plus holes for `f`); there is no character literal; `_` is a digit separator; `j` is an imaginary suffix.
- **Go:** back-tick raw strings are one `STR`; `'x'` is a rune literal (one `STR`); `_` is a digit separator.
- **Java:** text blocks (`"""`) are one `STR`; `$` and `_` are identifier characters; `L`, `f`, `d` are numeric suffixes.
- **TypeScript:** template literals with `${…}` use the interpolation rule; regular-expression literals (if any appear) are one `STR`; `n` is the BigInt suffix; `$` is an identifier character.
- **Kotlin:** `$ident` and `${…}` are both interpolation holes; `` `quoted` `` identifiers are one `IDENT`; nested block comments.
- **Swift:** `\(…)` is the interpolation hole; `"""` multi-line strings; nested block comments; `0x1p3` hexadecimal float exponents.
- **Quidra:** the profile is populated from Quidra 0.2.0's own published grammar/reference by exactly the procedure used for the other nine languages. Where a lexical detail is not documented, the profile falls back to the documented defaults of this section (Unicode identifier classes, `_` separator, `//` line comment) **and the fallback is logged in `token_profiles.json` under `fallbacks`** and published. No Quidra lexical rule is invented to change a count, and no rule elsewhere in this document was written after inspecting Quidra's lexis (invariant I1).

### 4.6 Profile schema

```json
{
  "<language>": {
    "line_comment": ["//"],
    "block_comment": {"open": "/*", "close": "*/", "nesting": true},
    "strings": [{"open": "\"", "close": "\"", "escape": "\\", "raw": false,
                 "interpolation": {"open": "\\(", "close": ")", "has_closer": true}}],
    "chars": [{"open": "'", "close": "'", "escape": "\\"}],
    "ident_start": "XID_Start|_", "ident_continue": "XID_Continue",
    "quoted_ident": [{"open": "`", "close": "`"}],
    "numbers": {"prefixes": ["0x","0o","0b"], "separator": "_",
                "exponent": ["e","E","p","P"], "suffixes": []},
    "keywords": ["…"],
    "operators": ["…"],
    "delimiters": ["(",")","[","]","{","}",",",";",":",".","@","#"],
    "optional_lexemes": [{"lexeme": "(", "kind": "grouping", "removable": true, "reason": "redundant grouping pair; §4.2/R4"}],
    "reference_lexer": {"available": true, "command": "…", "mapping": "…"},
    "fallbacks": []
  }
}
```

**Schema constraints (frozen).** `optional_lexemes` may contain **grouping parentheses only**; a statement terminator, a keyword, or any other lexeme appearing there is a profile defect and the tokenizer aborts on it (§4.5, step 4). `interpolation.has_closer` is `false` only for a genuine single-delimiter hole form (e.g. Kotlin `$ident`) and `true` for every form the author must close (§4.4(a)). Both constraints are checked by `--selftest` before any probe is measured, and they apply to all ten profiles identically, Quidra's included.

### 4.7 Reconciliation rule (required by §6.1.4.A)

For every language whose toolchain on this host exposes a reference lexer, the probe is **also** lexed by that reference lexer, its output is mapped to our token classes by a documented mapping, and the difference is published.

| Language | Reference lexer | Status | Mapping / reason |
|---|---|---|---|
| Python | `python3 -m tokenize` | **used** | Drop `NEWLINE`, `NL`, `INDENT`, `DEDENT`, `ENDMARKER`, `COMMENT`; f-strings re-mapped to shell + hole delimiters per §4.4(a). |
| Go | `go/scanner` driven by a 20-line Go program | **used** | Drop auto-inserted `;` (`Pos` of an inserted semicolon has literal `"\n"`), drop `EOF`. |
| C++ | `clang++ -std=c++20 -fsyntax-only -Xclang -dump-tokens` | **used** | Drop `eof`; join `greater greater` back into one `>>` where our maximal-munch rule differs; drop tokens outside the marker span by line/column. |
| TypeScript | `typescript` compiler API `ts.createScanner` via `node` | **used** | Drop `NewLineTrivia`, `WhitespaceTrivia`, all comment trivia; template spans re-mapped to shell + hole delimiters per §4.4(a). |
| Rust | none on stable `rustc 1.95.0` (`-Zunpretty` requires nightly) | **N/A** | Reason: no stable token-dump interface ships with the frozen toolchain. |
| Java | `com.sun.tools.javac.parser` is internal and unsupported | **N/A** | Reason: no supported lexer interface in the frozen OpenJDK 26.0.1 distribution. |
| Kotlin | `kotlinc 2.3.21` exposes no token dump | **N/A** | Reason: no CLI or supported API for lexing only. |
| Swift | `swift-syntax` is not installed as a CLI on this host | **N/A** | Reason: not present in the frozen toolchain. |
| Zig | `zig 0.16.0` exposes no token dump CLI | **N/A** | Reason: `zig ast-check` reports diagnostics, not tokens. |
| Quidra | used iff `quidra` exposes a documented token/lex dump; otherwise **N/A** with that reason recorded | conditional | Determined once, before measurement, and recorded in `token_profiles.json`. |

**Reconciliation procedure.**

1. For each `(language, probe)` with an available reference lexer, compute `delta = frozen_count − reference_count_after_mapping`.
2. `delta` must be `0`. A non-zero `delta` is investigated and must be fully explained by a rule already written in §4.2–§4.4; the explanation is recorded in `../semantic-compression/raw/tokens/reconciliation.json`.
3. A `delta` that **cannot** be explained by an existing rule marks that probe `DISPUTED`. The frozen tokenizer's count remains authoritative (so no language is measured by a different instrument from another), and the dispute, the two counts, and the unresolved cause are published beside the score.
4. **N/A languages are not disadvantaged or advantaged.** They are measured by the same authoritative tokenizer; the absence of a second opinion is recorded with its reason per §26, and no substitute penalty or bonus is applied.
5. Reconciliation is run **before** any density value is computed, and its report is published with the results (§6.1.7).

---

## 5. Semantic Density — raw value, aggregation, normalization

### 5.1 Raw value

For a language `L` over the fixed probe set `P` (the **40 frozen probes `F01.P1 … F20.P2`** defined in the frozen capability/probe document `01_capability_universe_and_probes.json`, two probes per each of the 20 capability families of spec §6.1.2; that identifier form is the only one used anywhere in this document, per §2.6):

```
facts(L, p)   = number of ledger rows for (L, p) with disposition == COUNT
tokens(L, p)  = frozen-tokenizer count for (L, p) after canonicalisation R1–R7

                       Σ_{p ∈ P_applicable(L)} facts(L, p)
raw_density(L)  =  ───────────────────────────────────────────
                       Σ_{p ∈ P_applicable(L)} tokens(L, p)
```

Unit: **explicitly recoverable semantic facts per lexical source token.** Higher is better (§6.1.4.A).

**Form selection is fixed by the probe document, not by the author (frozen).** Where a language offers both an operator form and a standard-library call for the same frozen probe semantics — `a + b` versus `a.wrapping_add(b)`, `x[i]` versus a checked accessor, `a == b` versus an equality method — **the frozen probe document names the form to be measured, per language, before annotation begins.** Without this, §2.2's admission of one named standard-library contract would let the same task be written for a higher score: a stdlib-heavy spelling can earn several `COUNT` rows for four or five tokens where the operator spelling earns fewer, so the choice of spelling, not the language, would move the number. The named form must be the one 01's `R2_idiomatic` rule describes — what a competent professional would write for production code — and the choice, once named, is the same for every annotator and is published.

> **Counterpart obligation (cross-document).** `01_capability_universe_and_probes.json` must record, for every probe and every language where both spellings exist, which spelling is the measured fragment, with the one-line reason, before annotation begins.

### 5.2 Aggregation (frozen)

The aggregation is **sum of facts over sum of tokens** across the 40 probes — a single macro-ratio. It is explicitly **not** the mean of the 40 per-probe ratios.

Reason, frozen in advance: a per-probe mean gives a one-line probe the same leverage as a twenty-line probe, which would let probe granularity move a score. The macro-ratio weights each probe by its actual syntactic mass, which is the quantity §6.1.4.A names.

Per-probe ratios `facts(L,p)/tokens(L,p)` and per-capability-family subtotals are nevertheless **published in full** in `../semantic-compression/scores/density.json` and in the §29 raw tables, so the aggregation choice is inspectable and the distribution is visible.

### 5.3 Applicability, unsupported probes, and partial support

- **Supported probe:** implemented per the frozen probe statement using documented language features and the normal standard runtime/library, with no benchmark-specific external packages and no code generation (§6.1.2). Included in both sums.
- **Partially supported probe** (expressible only via the workaround permitted by the frozen partial-support rubric): the frozen partial implementation is the measured source. Its facts and tokens are included normally. The partial-support flag affects Capability Coverage `C`, not this metric.
- **Unsupported probe:** no source exists, so the probe contributes **0 facts and 0 tokens** — it is excluded from *both* sums. This is not a free win *for the overall score*: spec §6.1.5 and §26 keep the unsupported capability in the Capability Coverage denominator, and the harmonic mean of §6.1.6 applies the penalty there. Every exclusion is recorded with its reason in `density.json` under `excluded_probes`.
- **But the published Semantic Density row is a comparison across different probe subsets, and must say so.** Excluding a probe from both sums changes the *macro-ratio itself*, because probes are not equally dense: concurrency, FFI and module-boundary probes are token-heavy, hop-heavy and fact-sparse in **every** language, so a language that cannot express them has its density computed over a smaller and systematically easier subset than a language that can. Any language of the fixed set may be affected — a language without concurrency primitives, without an FFI boundary, or without a module system will drop the corresponding family — and the effect flatters whoever drops the most. **Therefore:** the **primary** Semantic Density value remains the full-applicable-subset macro-ratio defined in §5.1, and beside it `density.json` publishes `probes_included` and the **common-subset** value `raw_density_common_subset` / `normalized_score_common_subset`, computed over exactly those probes for which **all ten languages** have a fragment (§5.5). The §27.1 Semantic Density row carries a footnote stating, per language, the number of probes its published value was computed over.

> **Counterpart obligation (cross-document).** The document that renders the §27.1 results table must carry that per-language `probes_included` footnote on the Semantic Density row, and must print the common-subset value beside the primary one. A Semantic Density row published without it is incomplete.
- **Fully N/A language:** if `Σ tokens(L, ·) == 0` (no probe expressible at all), `raw_density(L)` is **`N/A`, reason: "no probe in the fixed universe is expressible in this language; density is undefined (0/0)"**. Per §26 the metric is then excluded from that language's applicable-weight denominator and the remaining Semantic Compression quality weights are renormalized. This case is not expected for any of the 10 languages and must be reported loudly if it occurs.

### 5.4 Normalization — family D (spec §25.1.D)

Semantic Density is a positive higher-is-better quantity without a natural 0–1 bound. Family **D** applies:

```
best_raw   = max over the 10 fixed languages of raw_density(L), among languages with an applicable value
Score(L)   = 100 * raw_density(L) / best_raw
```

Scores are clipped to `[0, 100]` and reported to two decimal places; ranking uses unrounded values (§25.3). The best language scores exactly `100.00`. **No other transformation is applied**: no log scaling, no min-max rescaling, no percentile scaling, no winsorization (§25.4).

Weight in the Semantic Compression quality score: **20%** (`Q = 0.20*Density + 0.25*Determinacy + 0.20*Locality + 0.20*HiddenCost + 0.15*CapabilityEfficiency`, §6.1.6).

### 5.5 Required published outputs

`../semantic-compression/scores/density.json` must contain, for each of the 10 languages in the fixed column order:

`raw_density`, `total_facts`, `total_tokens`, `per_probe: [{probe_id, facts, tokens, sites, ratio, support_status}] × 40` (`probe_id` in the `F<nn>.P<k>` form of §2.6), `per_family: [{family_id, facts, tokens, ratio}] × 20`, `excluded_probes: [{probe_id, reason}]`, `disposition_histogram: {COUNT, LOOKUP, INDET, DUP, NA}`, `basis_class_histogram: {local-marker, universal-rule, stdlib-contract}`, `normalized_score`, `script_sha256`, `profile_sha256`, `reconciliation_status`.

**Additionally required, and not optional:**

- **`probes_included`** (integer) and **`raw_density_common_subset`**, **`normalized_score_common_subset`** — the macro-ratio and its normalized score computed over only those probes for which **all ten** languages have a fragment (§5.3). Published beside the primary value, never in place of it.
- **`raw_density_reading_b`** and **`normalized_score_reading_b`** — the full recomputation under invariant I5's **reading (b)**: the sum of `COUNT` rows whose `basis_class == "local-marker"`, over the **same** token denominator, for every language; plus **`reading_b_rank_delta`**, each language's change in rank order between reading (a) and reading (b). The ledger already records `basis_class` on every `COUNT` row (§2.6), so this costs no extra annotation. Reading (a) remains the primary and frozen metric; reading (b) is published as a **mandatory sensitivity analysis** so that the direction and size of I5's effect on each language is *measured rather than asserted* (§0/I5).
- **`mode_driven_rows: {language: {fact_kind: count}}`** — the number of rows whose disposition is `LOOKUP` **solely** because of a compiler-mode or global-configuration dependency (§2.5). This makes visible how much of the spread is produced by that one rule, which bites hard on languages that expose a behaviour as a build mode and not at all on languages that fix it in the language.
- **`sites_per_probe`** — each language's site count per probe, per §1.2, so that differing site counts across languages are visible rather than assumed away.

Publishing `disposition_histogram` and `basis_class_histogram` is mandatory: together they let any auditor see exactly how much of each language's density comes from explicit markers versus universal rules versus library contracts, which is the audit hook for invariant I5. **Publishing the reading-(b) recomputation is mandatory for the same reason and at a higher standard: it replaces an assertion about I5's direction with a number.**

---

## 6. Worked end-to-end example (illustrative only — NOT one of the 40 probes)

**Task:** bind a 32-bit signed integer to `0`, then add `1` to it with wrapping-on-overflow semantics.
This example exists solely to demonstrate that §1–§5 are mechanical. It is not part of the measured probe set, and its numbers do not enter any score.

### 6.1 Rust — 17 facts / 17 tokens = 1.000

```rust
let mut c: i32 = 0;
c = c.wrapping_add(1);
```

Tokens (17): `let` `mut` `c` `:` `i32` `=` `0` `;` `c` `=` `c` `.` `wrapping_add` `(` `1` `)` `;`

Sites: `s1` = S1 binding `c`; `s2` = S2 initialization `=`; `s3` = S2 assignment `=`; `s4` = S2 call `wrapping_add`.

| Site | Kind | Disp. | Value | basis_class |
|---|---|---|---|---|
| s1 | K01 | COUNT | a place (variable) with storage in the enclosing block | local-marker |
| s1 | K02 | COUNT | mutable (`mut`) | local-marker |
| s1 | K03 | COUNT | no alias exists; any alias requires a visible `&`/`&mut` | universal-rule |
| s1 | K04 | COUNT | initialized at declaration | local-marker |
| s1 | K05 | COUNT | `i32`, 32-bit two's complement | local-marker |
| s1 | K08 | NA | `i32` has no alternative cases | — |
| s1 | K10 | COUNT | no allocation; `i32: Copy`; no `Drop` | universal-rule |
| s1 | K13 | COUNT | lives to end of block; no resource | universal-rule |
| s2 | K04 | DUP | restates s1's initialization fact | — |
| s2 | K06 | COUNT | no implicit conversion; `0` inferred as `i32` | universal-rule |
| s2 | K10 | COUNT | copy of a `Copy` value into the place | universal-rule |
| s2 | K01/K05 | NA | gate G-value: result discarded | — |
| s2 | K07/K09/K11/K12 | NA | gate G-effect: local, non-arithmetic, non-call | — |
| s3 | K02 | COUNT | modifies the already-initialized `c` | local-marker |
| s3 | K06 | COUNT | no conversion; RHS is `i32` | universal-rule |
| s3 | K10 | COUNT | copy; no prior value to drop | universal-rule |
| s3 | K01/K04/K05/K07/K09/K11/K12 | NA | gates | — |
| s4 | K01 | COUNT | produces a value, not a place | universal-rule |
| s4 | K05 | COUNT | result type `i32` | stdlib-contract |
| s4 | K06 | COUNT | argument `1` is `i32`; no implicit conversion | universal-rule |
| s4 | K07 | **LOOKUP** (`hops_decl = 1`) | absence fact: the cited `wrapping_add` contract states the wrapping *result*; it does not state that the call cannot fail, and no universal rule entails it (§1.4, Absence facts). `note`: "no-failure not stated by the cited contract" | — |
| s4 | K08 | NA | result is not an alternative type | — |
| s4 | K09 | COUNT | two's-complement wraparound on overflow | stdlib-contract |
| s4 | K10 | COUNT | receiver copied (`i32: Copy`); no borrow is created, because a borrow requires a visible `&` | universal-rule |
| s4 | K11 | **LOOKUP** (`hops_decl = 1`) | absence fact: no cited clause states the absence of an externally visible effect (§1.4, Absence facts) | — |
| s4 | K12 | **LOOKUP** (`hops_decl = 1`) | absence fact; and §1.4/K12's own frozen distinction already makes an opaque call site `LOOKUP` for control flow, which the earlier version of this table contradicted | — |

`COUNT` rows: 7 (s1) + 2 (s2) + 3 (s3) + 5 (s4) = **17**. Tokens **17**. Ratio **1.000**.

Three of s4's rows became `LOOKUP` under the absence-facts rule of §1.4. Before that rule, a single standard-library call earned eight `COUNT` rows for five tokens, four of them by asserting absences (`cannot fail`, `no externally visible effect`, `returns normally`, `no allocation`) that the cited contract does not state — which violated invariant I4 and made "call more library functions" a scoring strategy in every language. `K10` survives because what it now records — the receiver is copied (`i32: Copy`) and no borrow is created (a borrow requires a visible `&`) — *is* entailed by universal rules; the unsupported "no allocation" clause was removed from its value and recorded in `note`.

### 6.2 Go — 14 facts / 8 tokens = 1.750

```go
var c int32 = 0
c += 1
```

Tokens (8): `var` `c` `int32` `=` `0` `c` `+=` `1`

- `s1` (binding): K01 COUNT (variable with storage), K02 COUNT (all Go variables are assignable — `universal-rule`), K03 COUNT (no alias can exist without a visible `&` — entailed by a universal rule, so it survives the absence-facts test), K04 COUNT (explicitly initialized by the `= 0`; `basis_class = local-marker` by the §2.6 selection order, with "also pinned by the zero-value rule" in `note`), K05 COUNT (`int32`, 32-bit two's complement), K10 COUNT (no *observable* allocation; whether the storage is stack or heap is unobservable and implementation-chosen — a universal rule, not an unstated absence), K13 COUNT (Go has no destructors, so determinately nothing runs at scope exit — entailed by a universal rule) → **7**
- `s2` (initialization `=`): K06 COUNT (untyped constant `0` converted to `int32` at compile time; representable), K10 COUNT (copy) → **2**
- `s3` (compound assign `+=`, one site by E2): K02 COUNT (modifies `c`), K06 COUNT (untyped constant `1` → `int32`), K07 COUNT (cannot fail), K09 COUNT (signed overflow wraps — Go spec "Integer overflow"), K10 COUNT (copy) → **5**

Total **14 / 8 = 1.750**.

### 6.3 Python — 14 facts / 6 tokens = 2.333

```python
c = 0
c += 1
```

Tokens (6): `c` `=` `0` `c` `+=` `1`

- `s1`: K01 COUNT (a name bound to an object; the binding is not itself storage), K02 COUNT (name rebindable; the bound `int` object is immutable — pinned by the literal), K03 COUNT (other names may refer to the same object, but `int` is immutable so no writable aliasing is possible), K04 COUNT (bound), K05 COUNT (`int`, unbounded precision), **K10 COUNT** (universal rule, Rule 2.5.1: object identity is not committed — the implementation may create a new object or reuse a cached one), **K13 COUNT** (universal rule, Rule 2.5.1: no deterministic release point; reclamation occurs at an unspecified time), K08 NA → **7**
- `s2` (`=`): K06 COUNT (no implicit conversion at binding), K10 COUNT (Rule 2.5.1, as above) → **2**
- `s3` (`+=`): K02 COUNT (rebinds `c`; `int` is immutable so `+=` does not mutate in place), K06 COUNT (int + int → int, no conversion), K07 COUNT (integer addition is total, so it cannot fail — entailed by a universal rule; U-07.1 excludes `MemoryError`), K09 COUNT (unbounded precision — overflow cannot occur), K10 COUNT (Rule 2.5.1) → **5**

Total **14 / 6 = 2.333**. The four rows that were `INDET` in an earlier version of this example are now `COUNT` under Rule 2.5.1, because "the implementation may reuse a cached object" and "there is no deterministic release point" are real facts a reader learns and must act on — exactly as "this variable is determinately uninitialized" is a real fact under K04. Those values are *worse answers* than a scope-bound release point, and the benchmark charges that where the specification puts it: in Determinacy (B), Locality (C) and Hidden Cost (D). (In the real probe set this task would additionally be flagged partial-support for Python, since fixed-width 32-bit wrapping is not expressible without a masking workaround; that flag affects Capability Coverage `C`, not this metric.)

### 6.4 What the example demonstrates

Under these frozen rules the two *least* explicitly annotated languages out-score the most explicitly annotated one on this task (Python 2.333, Go 1.750, Rust 1.000), because Go and Python obtain the same facts from universal rules at a fraction of the token cost while Rust pays 17 tokens for the explicit wrapping call, and three of that call's rows are absences its contract does not state.

Two cautions, both binding:

1. **This example is illustrative and proves nothing about the ten measured languages.** It is one three-language task chosen to show that §1–§5 are mechanical. It is not evidence about the direction of invariant I5's tilt, and it may not be cited as such. The direction of that tilt is established only by the reading-(b) recomputation that §5.5 requires `density.json` to publish for all ten languages (§0/I5).
2. **The example's numbers moved when the rules were corrected** — Rust 1.176 → 1.000, Python 1.667 → 2.333 — which is exactly why an illustrative example must never be used as evidence for a rule that produced it. The numbers here are recomputed from the corrected rules and enter no score (§6 preamble).

---

## 7. Freeze declaration

This document is frozen as of benchmark run `2026-09-17-7677581`, before any probe is annotated and before any token is counted. The adversarial-audit corrections recorded in the **Remediation changelog** below were applied at that same point, with **no result of any kind observed** — no density value, no ranking, no score — so they select rules, not outcomes. It may not be amended in response to observed counts, observed rankings, or any language's performance (§25.4, §32). Any defect discovered during measurement is recorded, published, and — if it must be fixed — forces a full re-run of Semantic Density for **all 10 languages** under the amended rule, with both the old and the new results published side by side (§25.1).

**Dependencies produced by this document, to be implemented exactly as specified:**

- `../scripts/tokenize_probe.py`, `../scripts/token_profiles.json`, `../scripts/token_fixtures.json`
- `../semantic-compression/raw/facts/<language>/<probe_id>.jsonl`, `../semantic-compression/raw/facts/audit.json`
- `../semantic-compression/raw/tokens/<language>/<probe_id>.json`, `../semantic-compression/raw/tokens/reconciliation.json`
- `../semantic-compression/scores/density.json`

---

## Remediation changelog

**Status of this document at the time of these corrections: no results have been observed.** No Semantic
Density value, no token count entering a score, and no ranking has been computed from this document or
from any sibling methodology document in run `2026-09-17-7677581`. These corrections are therefore
**pre-registration**, not post-result formula selection, and none of them is barred by spec §25.4 or §32.
The pre-freeze status is recorded here so that any later reader can verify the order of events.

Corrections were applied in the direction the adversarial audit specified. Where a correction is expected
to move Quidra's Semantic Density, the direction is stated explicitly. **A correction that lowers Quidra's
expected score is the normal and correct outcome of this exercise**, not a failure of it.

Every rule below was checked against the symmetry test: *would this rule look equally reasonable if
Quidra were replaced by Zig, or by Python?* Rules that only made sense because of something Quidra
specifically does or lacks were rewritten as predicates any of the ten languages can satisfy or fail.

| # | Severity | Defect (one line) | What changed | Effect on Quidra's expected score |
|---|---|---|---|---|
| 1 | **BLOCKER** | §2.3 claimed "the fact ledger is shared" and that Density and Locality "cannot disagree about a fact"; false against frozen methodology 03, which computes Locality independently, uses a different record schema, and contradicts 02 on stdlib hops and on unresolvable facts. | §2.3 rewritten: no shared ledger; **03 is authoritative for every hop count**; `hops_decl`/`hops_stdlib` are inputs to §2.2's `COUNT` test only and are never summed into `H_i`. A per-row table states each row's treatment under **both** metrics. The substantive disagreement is resolved in direction (b): a named stdlib consultation is admitted by Density **and charged as one hop by Locality** — the row earns density and pays locality, so it is not a free ride. `INDET` → 0 for Density **and** triggers 03 Rule 2.7.1 (`SAT = 6`) for Locality. §1.3's disposition table updated to match. Counterpart obligation recorded for 03 §2.2.2 and §2.7. | Unchanged as a scoring rule; removes a false claim that concealed a cross-document contradiction. |
| 2 | **BLOCKER** | Three incompatible probe-id schemes across one run (`P01…P40` here, `F<nn>.P<k>` in 01, `SC-F<nn>-P<k>` in 03); the `F01`–`F13` fact-kind labels collided with 01's `F01`–`F20` family prefixes; and §4.2 R1 required `BEGIN/END PROBE` markers that exist in no probe document, so the tokenizer would have aborted on all 400 fragments. | **01's `F<nn>.P<k>` adopted as the single authoritative probe id** in §2.6, §5.1 and §5.5. **Fact kinds renamed `F01`–`F13` → `K01`–`K13` throughout** §1–§6 and the ledger schema (same kinds, same order, same spec §6.1.3 wording). New **R1b**: the measurement harness writes each frozen fragment into its wrapper and **emits the marker lines itself**; markers are harness output, removed by R2, create no site, and 01's `R3_minimality` ("no comments") applies to fragment content only. | Unchanged; mechanical-reproducibility fix that affects all ten languages identically. |
| 3 | **MAJOR — QUIDRA BIAS** | §4.1/§4.3 guarantee "no class is weighted, discounted, or exempted"; §4.4(a) then gave every interpolation hole a flat one-token discount for two written delimiters — the only class exemption in §4 — favouring exactly Python, TypeScript, Kotlin, Swift and **Quidra** against C++, Go, Java, Rust and Zig, in the Density **denominator**. | §4.4(a) rewritten: a hole pays **one token per delimiter actually written** — `HOLE_OPEN` + `HOLE_CLOSE` for `{`…`}`, `${`…`}`, `\(`…`)`; a single `HOLE` for a bare-sigil `$ident`. §4.4(a) table updated (Python 4, TypeScript 4, Kotlin `$c` 3, Kotlin `${c}` 4, Swift 4); `HOLE_*` tags added to §4.3; §4.5 step 3.4 and the §4.5 fixture list updated; §4.1's guarantee restated as unconditional. Matches `CORRECTIONS.md` D-4 on the tokenizer side. Arithmetic slips from `CORRECTIONS.md` D-1 also fixed here (C++ `std::format` 7 → 8, Kotlin `@Suppress("x")` 4 → 5). | **LOWER.** Quidra's token denominator rises at every interpolation hole; its density falls. |
| 4 | **MAJOR — QUIDRA BIAS** | Invariant I5 asserted, without evidence, that reading (a) was self-handicapping because "Quidra is a language with explicit semantic markers". Only half of Quidra's profile: Quidra is also among the most universal-rule-pinned languages in the set (always-checked arithmetic with no opt-out, lossless-only implicit conversion, enforced definite initialization, visible-`&` writable aliasing), so reading (a) may tilt **toward** Quidra. | The unsupported claim is deleted. I5 now states that the direction **is not asserted and may not be inferred**, names languages on both sides of the split, and points to a published measurement. §5.5 now **requires** `raw_density_reading_b`, `normalized_score_reading_b` (the `local-marker`-only recomputation over the same denominator) and `reading_b_rank_delta` for every language. Costs no extra annotation: `basis_class` is already mandatory on every `COUNT` row. | **MIXED, and now measured rather than asserted.** If reading (a) was in fact tilting toward Quidra, the published sensitivity will show it. |
| 5 | **MAJOR — QUIDRA BIAS** | "The specification leaves it unspecified" was adjudicated three different ways for one structure: K04 counted C++ `int x;`, while K10 marked Python `c = 0` `INDET` and K13 marked Java GC and Swift ARC `INDET` — costing the six GC/ARC languages one-to-two facts per probe at S6 sites and handing them to C++, Rust, Zig **and Quidra**, whose managed retain/release was left unadjudicated while Quidra is annotated last. | New frozen **Rule 2.5.1** in §2.5: where a language rule **universally** specifies that an outcome is unspecified or non-deterministic, the row is `COUNT`, `basis_class = universal-rule`, with the value naming that rule. `INDET` is reserved for undefined/erroneous behaviour and runtime-data-dependent properties. Applied identically to K04, K10, K13; K10's and K13's "does not count" clauses and example rows rewritten (Java GC, Swift ARC and Python `c = 0` now `COUNT`). K13 gains a frozen three-row **reclamation table** (scope-bound / tracing GC / reference counting) and a **pre-adjudication row** binding Quidra's managed retain/release to exactly Swift ARC's disposition and `basis_class`, recorded in invariant I1. §2.7 now audits **100% of Quidra pairs** (positional rule: any language annotated last is audited in full). K11 explicitly excluded from 2.5.1 (a race is runtime-data-dependent). | **LOWER.** The GC/ARC languages regain the facts they were losing at scope-exit sites in essentially every probe, and Quidra's reclamation is bound in advance to the same treatment as Swift's rather than left to a post-hoc adjudication. |
| 6 | **MAJOR** | E2 made a call one S2 site and A3.1 capped it at one row per (site, kind), so `f(a, b, c)` with one by-value, one writable-alias and one implicitly-converted argument had a single K03 and a single K06 row and no rule saying whose answer went in them. Pervasive in the family 18/19/20 probes. | New **E7 — Argument sub-sites** in §1.1: each argument position, each composite-literal field initializer and each declarator is its own S2 sub-site (`s<n>.a<k>` / `s<n>.d<k>`) bearing its own rows under the same matrix and gates; the enclosing call keeps result and control/effect rows only. §1.1 preamble, §1.0 ceiling and §3.1/A3.1 aligned. | Unchanged in direction; removes an annotator-dependent count affecting all ten languages. |
| 7 | **MAJOR** | Absence facts (`cannot fail`, `no externally visible effect`, `returns normally`, `no allocation`) had no adjudication rule but supplied four of the eight `COUNT` rows the §6.1 example awarded to one stdlib call — none of them stated by the cited `wrapping_add` contract, violating invariant I4. Combined with §2.2's stdlib admission, "call more library functions" became a scoring strategy. | New **Absence facts** rule at the head of §1.4: such a row is `COUNT` only where the cited clause/contract **states** the absence or a universal rule **entails** it; otherwise `LOOKUP` with `hops_decl = 1` and a mandatory `note`. §2.2 and §2.6 cross-reference it. §6.1 re-adjudicated: K07, K11, K12 at `s4` → `LOOKUP`; K10's unsupported "no allocation" clause removed from its value; **Rust 20/17 = 1.176 → 17/17 = 1.000**. §5.1 adds: where a language offers both an operator form and a stdlib call for the same probe semantics, **the probe document names the measured form before annotation begins** (counterpart obligation on 01). | Unchanged in direction for Quidra specifically; removes a fact-inflation route open to every language with a rich standard library. |
| 8 | **MAJOR** | §5.3 excluded an unsupported probe from both sums, so the published Density row compared numbers computed over **different probe subsets** with no annotation. Concurrency, FFI and module probes are token-heavy, hop-heavy and fact-sparse everywhere, so dropping them mechanically raises the survivor's macro-ratio. | §5.3 now states the effect explicitly and §5.5 **requires** `probes_included`, `raw_density_common_subset` and `normalized_score_common_subset`, computed over only the probes all ten languages implement. The primary value stays the full-applicable-subset macro-ratio; the common-subset value is published beside it, and the §27.1 Semantic Density row must carry a per-language "computed over N probes" footnote (counterpart obligation). | **LOWER where it applies.** Any language that drops hard families — on the current record most plausibly Quidra (concurrency, WASM, self-hosting are stated development areas; family 20's exported-C-ABI probe is constrained) — loses the unannotated advantage of being scored on an easier subset. The rule is written positionally and applies to whichever languages drop probes. |
| 9 | MINOR | §2.7's re-annotation audit did not say which count is scored after a disagreement, had no corpus-level failure criterion, and imposed no analyst independence, while 03 §3.2 required two-analyst reconciliation for the metrics 02 called "the same rows". | §2.7 rewritten as A1–A4: **A1** second analyst, blind to the first ledger and to other languages' counts; **A2** the corrected count is the scored value and the correction propagates to every non-sampled pair with the same pattern; **A3** more than 20% of sampled pairs requiring re-adjudication forces full re-annotation of all ten languages before any score; **A4** rules are never changed to settle a disagreement. Quidra sampled at 100% (see finding 5). | Unchanged in direction; removes discretion at the point where Quidra is annotated last. |
| 10 | MINOR | R4 removed a lexeme by a **per-occurrence, semantic** test, but §4.6 implemented it as a static per-language list applied blindly, and the test was circular (it asked about fact values at sites that R4 helps define). TypeScript ASI and Swift newline rules make `;` removability position-dependent. | **R4 restricted to redundant grouping parentheses**; `optional_lexemes` restricted to grouping parentheses in the §4.6 schema, with a tokenizer abort if anything else appears there. New **R7**: no frozen fragment may contain a terminator its grammar permits omitting at that position, **verified at freeze time**. §4.4(d), §4.5 step 4 and §4.1 updated; `R1–R6` → `R1–R7` in §5.1. Counterpart obligation for 01's authoring rules. | Unchanged; same outcome, decided once and mechanically instead of by a circular per-token test. |
| 11 | MINOR | `basis_class` is mandatory and its histogram is the stated audit hook for I5, but no rule said how to choose when several bases apply — and the document's own examples showed the conflict. Finding 4's reading-(b) recomputation reads the same field. | §2.6 adds a **frozen selection order**: `local-marker` (a lexeme *of the language* at or governing the site; a stdlib **name** is not a local marker) → `stdlib-contract` → `universal-rule`, with any further basis named in `note`. §6.2's Go `s1` K04 row adjudicated accordingly. The order is deliberately the attribution least favourable to the claim that reading (a) is neutral. | Slightly **LOWER** for any language whose density leans on universal rules that a marker also pins, Quidra included; the effect is now deterministic instead of annotator-dependent. |
| 12 | MINOR | The "compiler-mode dependence is `LOOKUP`" rule is spec-faithful and language-neutral as written, but its measured effect is highly asymmetric (Rust/Zig lose K09 at every plain arithmetic site, TypeScript loses K07 at every index) and invisible in the published output — while a language whose checking is always on, Quidra included, pays nothing. | §2.5 keeps the rule and adds a published-magnitude requirement; §5.5 adds required key **`mode_driven_rows: {language: {fact_kind: count}}`**. A **counterpart obligation** is recorded against `environment.json` / `00_cross_language_constraints.md`: the frozen recipes must not pair one language's checks-enabled build against another's checks-disabled build, and each choice is recorded with its reason. | Unchanged as a rule; the size of the advantage an always-checked language draws from it becomes visible and auditable. |
| 13 | MINOR | §1.2 claimed "the set of annotated rows is identical across the 10 languages for a given probe's site structure", which an auditor reads as row-count parity — and site structure is exactly what differs between languages. | Sentence replaced: gates depend only on the site's class and shape and never on the language; **site counts legitimately differ** and are part of what the metric measures; `density.json` publishes `sites_per_probe` per language (added to §5.5) so the difference is visible. | Unchanged; removes a near-vacuous claim. |

**Findings rejected: none.** All thirteen were applied. Finding 1 offered two permitted resolutions of the
stdlib-hop disagreement and direction (b) was taken — Density admits one named stdlib consultation,
Locality charges it as a hop — because it keeps the metric's resolution (excluding every library name
would zero out nearly every I/O, collection and concurrency probe in all ten languages at once) while
leaving 03's Rule 2.2.2 intact; the choice is recorded in §2.3 with its counterpart obligation, and it is
not a free ride, because the same row that earns density pays locality.

**Counterpart obligations created by these corrections** (each must be discharged in the named document;
until then §2.3 governs the metric boundary and 03 governs every hop count):

1. `03_determinacy_and_locality.md` §2.2.2 — add the cross-reference naming 02 §2.2 (stdlib consultation
   admitted by Density only, hop count here unaffected); §2.7 — add "an `INDET` fact in the 02 ledger
   contributes 0 to Density and triggers Rule 2.7.1 here"; and adopt `F<nn>.P<k>` in place of
   `SC-F<nn>-P<k>` for probe identifiers.
2. `01_capability_universe_and_probes.json` — record, per probe and per language, which spelling
   (operator form or standard-library call) is the measured fragment where both exist; confirm that
   marker comment lines are harness output and not fragment content (02 §4.2/R1b); and record the
   freeze-time verification that no fragment contains an omissible statement terminator (02 §4.2/R7).
3. `04_hidden_cost_and_capability_efficiency.md` — replace the `PR-01 … PR-40` probe identifiers with
   `F<nn>.P<k>`, so that one scheme is used across the run.
4. The document rendering the §27.1 results table — carry the per-language `probes_included` footnote on
   the Semantic Density row and print the common-subset value beside the primary one.
5. `../environment/environment.json` and `00_cross_language_constraints.md` — record, per language, the
   build configuration used and its reason, and confirm that no language's checks-enabled build is
   compared against another language's checks-disabled build.

**Net expected direction for Quidra: LOWER.** Findings 3, 5, 8 and 11 each move Quidra's expected
Semantic Density down; findings 1, 2, 6, 7, 9, 10, 12 and 13 are neutral in direction; finding 4 replaces
an assertion about the direction of I5's tilt with a published measurement that may move it either way.
