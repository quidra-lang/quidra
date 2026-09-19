# FROZEN METHODOLOGY 02 — Semantic-Fact Taxonomy and Semantic Density Counting Rule

**Benchmark run:** `2026-09-17-7677581`
**Authoritative specification:** `../prompt.md` §6.1.2, §6.1.3, §6.1.4.A, §6.1.7, §25.1(D), §26, §32
**Status:** FROZEN. No clause in this document may be changed after the first probe is annotated or the first token is counted.
**Scope:** This document defines (1) the semantic-fact taxonomy, (2) the "explicitly recoverable" test and its interaction with Semantic Locality, (3) the anti-double-counting rule, (4) the language-neutral token-counting rule and its tokenizer, and (5) the Semantic Density raw value, aggregation, and normalization.

**Fixed comparison set and fixed column order (all 10 languages, identical treatment):**

`Quidra, Python, C++, Rust, Go, Java, TypeScript, Kotlin, Swift, Zig`

---

## 0. Fairness invariants governing this document

These are binding constraints on every rule below, taken from §6.1.1, §6.1.7 and §32.

- **I1 — Non-derivation.** No rule in this document was derived from Quidra's syntax, operators, types, or feature set. Every fact kind is taken verbatim from the closed list in spec §6.1.3; every worked example in §1 is drawn from non-Quidra languages *on purpose*, so that the taxonomy can be audited for Quidra-independence by inspection. Quidra is annotated by these rules like any other language, last, and with no exceptions.
- **I2 — No capability removal.** Nothing here removes a fact kind, site class, or capability because a language (including Quidra) lacks it. A language that cannot express a probe scores that way; it does not make the probe disappear.
- **I3 — Symmetry.** Every rule is stated as a language-independent predicate over (surface form, language specification). A language must be able to win a fact kind that Quidra loses and lose one that Quidra wins.
- **I4 — Anti-fabrication.** Every counted fact must carry a `basis` citation to a named clause/section of a published language specification or reference, or to a published standard-library contract. A row without a `basis` is not counted. Where a fact cannot be adjudicated honestly, it is recorded `INDET` or `NA` with a written reason (§2.6, §5.5); it is never guessed.
- **I5 — Chosen reading of "semantic fact" (declared in advance, and it is the reading *less* favourable to explicitly-annotated languages).** Two readings were available:
  - **(a) Information-content reading:** a fact counts when the reader can pin it down from the local form plus the language rules, *including* when the language pins it by a universal rule that offers no alternative (e.g. "all Go variables are mutable", "all Java class types are references", "Python integers are unbounded").
  - **(b) Distinguishing-choice reading:** a fact counts only where the language offered at least two values and the surface form selected one — i.e. only annotated choices count.

  **Reading (a) is frozen.** Reading (b) would systematically reward languages whose surface form is dense with explicit modifiers and punish languages that obtain the same guarantee from a universal rule at zero token cost. Since Quidra is a language with explicit semantic markers, reading (b) would tilt the metric toward Quidra; choosing (a) removes that tilt and is also the reading faithful to §6.1.1 ("how much important program meaning is communicated explicitly and determinately per unit of syntax"). The worked example in §6 confirms the direction: under reading (a), Go and Python out-score Rust on this metric.
  Every `COUNT` row records `basis_class ∈ {local-marker, universal-rule, stdlib-contract}` so the split is visible in the published raw data and the effect of I5 is auditable rather than hidden.

---

## 1. The fact taxonomy

### 1.0 Structure of the taxonomy

The taxonomy is **closed**: exactly the thirteen kinds listed in spec §6.1.3, no more and no fewer. They are labelled `F01`–`F13` and used under these labels in every raw file.

| ID | Fact kind (spec §6.1.3 wording) |
|---|---|
| F01 | value versus storage |
| F02 | mutability |
| F03 | aliasing / writable aliasing |
| F04 | initialization state |
| F05 | type and representation |
| F06 | conversion behavior |
| F07 | possible failure |
| F08 | alternative value cases |
| F09 | overflow / exceptional numeric behavior |
| F10 | allocation, copying, moving, borrowing, or destruction |
| F11 | externally visible side effects |
| F12 | control-flow effect |
| F13 | lifetime / resource effect |

Facts are not asserted about a probe as a whole. They are asserted at **fact-bearing sites** (§1.1), one fact per `(site, fact kind)` pair at most (§3). A probe's fact ceiling is therefore `13 × |sites|`, which bounds annotation inflation mechanically.

### 1.1 Fact-bearing sites (mechanical enumeration)

A **site** is a syntactic construct physically present in the probe region (§4.2). Sites are enumerated by walking the probe region in source order and applying these rules in order; each construct yields at most one site.

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
- **E3 — Literals are not sites.** A literal contributes to the facts of the site that consumes it (its type contributes to that site's F05). This is required by §3.
- **E4 — Facts about an entity are asserted at its binding site only.** An S2 site asserts facts about *the operation* (what it does, what its result is), never about the identity/mutability/type of an operand that already has an S1 site in the probe region. This eliminates the largest source of double counting.
- **E5 — Redundant grouping creates nothing.** Parentheses or blocks used purely for grouping create no site.
- **E6 — Boilerplate outside the probe region creates no site** (§4.2).

### 1.2 Applicability matrix

A `(site, fact kind)` row exists only where the matrix permits. Rows outside the matrix are not annotated and are not counted anywhere.

| | S1 binding | S2 operation | S3 signature | S4 type def | S5 control | S6 scope exit | S7 boundary |
|---|---|---|---|---|---|---|---|
| F01 value/storage | ● | ◐ | ● | ● | | | |
| F02 mutability | ● | ◐ | ● | ● | | | |
| F03 aliasing | ● | ● | ● | ● | | | |
| F04 initialization | ● | ● | | ● | | | |
| F05 type/representation | ● | ◐ | ● | ● | | | |
| F06 conversion | | ● | ● | | | | ● |
| F07 possible failure | | ◐ | ● | | ● | ● | ● |
| F08 alternative cases | ● | ◐ | ● | ● | ● | | |
| F09 numeric edge | | ◐ | | | | | |
| F10 alloc/copy/move/borrow/destroy | ● | ● | ● | | | ● | ● |
| F11 external side effects | | ◐ | ● | | ● | ● | ● |
| F12 control-flow effect | | ◐ | ● | | ● | ● | ● |
| F13 lifetime/resource | ● | | ● | ● | | ● | ● |

● = applicable. ◐ = applicable **only under the gate** stated in the kind's definition below. Blank = never annotated.

The three gates used by ◐ (frozen, language-independent):

- **G-value** (F01, F05, F08 at S2): applicable only when the operation produces a value that the probe subsequently uses, returns, or observes. A discarded/void result makes the row `NA`.
- **G-mutate** (F02 at S2): applicable only when the operation modifies an entity that is already initialized. Pure initialization makes the row `NA` (F04 covers it).
- **G-effect** (F07, F09, F11, F12 at S2): F09 is applicable only at arithmetic, numeric-conversion, and numeric-comparison operations. F07 is applicable at calls, indexing/slicing, conversions, allocations, resource operations, propagation operators, and arithmetic. F11 and F12 are applicable only at operations that *could*, in some language in the fixed set, reach state or control outside the probe region: calls, propagation, throw-capable and trap-capable operations, writes through a reference/pointer, I/O, FFI, and concurrency operations. A local-only operation on a local-only entity makes F11/F12 `NA`.

Gates are stated in terms of "some language in the fixed set", never "this language", so the *set of annotated rows is identical across the 10 languages for a given probe's site structure*. Languages then differ only in the **disposition** of those rows.

### 1.3 Row dispositions

Every annotated row receives exactly one disposition.

| Code | Meaning | Density numerator | Feeds |
|---|---|---|---|
| `COUNT` | The fact is **applicable, behaviourally determinate, and explicitly recoverable from the local surface form** (§2). | **+1** | Metric A |
| `LOOKUP` | Applicable and determinate, but resolving it requires ≥1 declaration-graph hop outside the probe region (§2.3). | 0 | Metric C (hops) |
| `INDET` | Applicable, but no single behaviourally determinate answer exists even with unlimited lookups: the specification says *undefined*, *unspecified*, *implementation-defined behaviour*, or the answer is a runtime-only property (§2.5). | 0 | Metrics B and D |
| `DUP` | Applicable and determinate, but the identical fact value has already been counted for this entity/operation at another site in the same probe (§3). | 0 | Audit only |
| `NA` | Not applicable under the matrix or a gate, with a written reason. | 0 | Audit only (per §26, `NA` here never becomes a silent zero for a capability the probe is testing — that is handled by Capability Coverage `C`) |

### 1.4 The thirteen fact kinds

Format for each kind: **Definition** → **Counts when** → **Does NOT count when** → **Worked examples** (each drawn from at least three different non-Quidra languages of the fixed set; `+` positive, `−` negative).

---

#### F01 — Value versus storage

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

#### F02 — Mutability

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

#### F03 — Aliasing / writable aliasing

**Definition.** Whether, at this site, another name or path may read the same storage, and whether another name may *write* it; and, at a call/argument site, whether the operation hands out a writable alias.

**Counts when.** The local form plus language rules determine both the read-alias and write-alias answers — including "no alias can exist" and "an alias is necessarily created".

**Does NOT count when.** The answer needs a callee signature, a parameter mode declared elsewhere, an overload set, a trait/interface implementation, or the declaration of the operand's type → `LOOKUP`. Spec §6.1.4.B names the `f(x)` call site as the canonical case; §6.1.4.B also forbids awarding or removing points merely because a language uses the glyph `&` — F03 is adjudicated on the *complete* local surface form plus the language rules, not on any glyph.

**Examples.**

| # | Language | Surface form | Verdict | Reason (basis) |
|---|---|---|---|---|
| + | Rust | `let r = &mut v;` (with `v` bound in the probe region) | `COUNT` | Exclusive mutable borrow; no other live alias may exist for the borrow's lifetime. Rust Reference, "References and Borrowing" |
| + | C++ | `int& r = x;` | `COUNT` | `r` is a writable alias of `x`. [dcl.ref] |
| + | Swift | `f(&x)` where `f` takes `inout` | `COUNT` | The `&` at the call site is mandatory for `inout`; copy-in/copy-out with write-back is guaranteed by the *call-site form together with the language rule that `&` may only appear for `inout`*. Swift Reference, "In-Out Parameters" |
| + | Java | `f(list)` where `list` is declared in the probe region with a class type | `COUNT` | The callee necessarily receives a writable alias to the same object (reference semantics are universal for class types). Whether it *does* write is F11/F02, a separate row that is `LOOKUP`. JLS §8.4.1 |
| − | C++ | `f(x)` | `LOOKUP` | By-value, by-reference, and by-const-reference are indistinguishable at the call site. 1 hop (callee signature); more if `f` is overloaded. |
| − | Go | `f(s)` where `s` comes from an import | `LOOKUP` | Slice/map/pointer types share backing storage while arrays and structs copy; the operand's type must be resolved. ≥1 hop. |
| − | TypeScript | `f(x)` | `LOOKUP` | Object arguments always share, primitives never do; which applies depends on `x`'s declared type. ≥1 hop. |

---

#### F04 — Initialization state

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

#### F05 — Type and representation

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

#### F06 — Conversion behavior

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

#### F07 — Possible failure

**Definition.** Whether this operation can fail, and **how failure is signalled** (returned error value, optional/absent value, thrown/raised exception, trap/abort/panic, error union, sentinel).

**Frozen uniformity rules.**
- **U-07.1** Resource-exhaustion failures (out-of-memory, stack overflow) are **excluded** from F07 for all 10 languages. They exist everywhere and would add a constant to every language.
- **U-07.2** F07 asks *whether* and *how*, not *the exhaustive set of failure modes*. The exhaustive set is a Semantic Determinacy (B) and Semantic Locality (C) question. This is what allows exception-based languages to earn F07 by universal rule while still being measured on the imprecision by B and C.

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

#### F08 — Alternative value cases

**Definition.** Whether the value at this site is one of a set of alternatives (optional/nullable, sum/variant/enum, error union, tagged union, open interface/subtype set), what the alternative set is, and whether the handling at this site is exhaustive.

**Counts when.** The alternative set and (where the site handles alternatives) exhaustiveness are determined by the local form plus the language rules — including the case where the type is declared *inside* the probe region, which is local by §2.2.

**Does NOT count when.** The alternative set lives in a declaration outside the probe region → `LOOKUP`; or the set is open/unbounded so that no determinate set exists → `INDET`.

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

#### F09 — Overflow / exceptional numeric behavior

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

#### F10 — Allocation, copying, moving, borrowing, or destruction

**Definition.** What this site does to the *ownership and storage* of values: does it allocate (and where — stack/heap/static), copy (shallow or deep), move/transfer ownership, borrow/reference without owning, or destroy/release.

**Counts when.** The local form plus the language rules determine the answer, including "no allocation" and "copies the reference, never the object".

**Does NOT count when.** The answer depends on the operand's type declared outside the probe region (value type vs reference type vs COW type) → `LOOKUP`; or the language leaves object allocation unspecified → `INDET`.

**Examples.**

| # | Language | Surface form | Verdict | Reason (basis) |
|---|---|---|---|---|
| + | C++ | `auto w = std::move(v);` / `std::vector<int> v(n);` | `COUNT` (move / heap allocation) | [vector.cons]; [class.copy.ctor], `std::move` contract |
| + | Rust | `let b = Box::new(x);` | `COUNT` (heap allocation, ownership moved into `b`) | Rust Reference, "Box"; `alloc::boxed` contract |
| + | Go | `s := make([]int, n)` | `COUNT` (allocates a backing array) | Go spec, "Making slices, maps and channels" |
| + | Java | `a = b;` with class types | `COUNT` (copies the reference; never copies the object) | JLS §15.26.1 |
| − | Python | `c = 0` | `INDET` | Whether a new object is created is explicitly unspecified (implementations may cache immutable objects). Python Reference §3.1 |
| − | Go | `y := x` where `x`'s type comes from an import | `LOOKUP` | Struct/array copy vs slice/map header sharing depends on the type. ≥1 hop. |
| − | Swift | `let a = arr` on `Array` | `COUNT` (value semantics; copy-on-write is an unobservable optimisation) | Swift Reference, "Structures and Enumerations Are Value Types"; `Array` documented value semantics |

---

#### F11 — Externally visible side effects

**Definition.** Whether this site produces an effect observable **outside the probe region**: I/O, mutation of shared/global/static state, mutation through a reference handed in from outside, synchronisation, foreign-function effects, or observable non-determinism.

**Frozen boundary rule.** Modifying a local entity that is declared *inside* the probe region is **not** an externally visible side effect; that is F02/F04. This keeps F11 from becoming a free per-statement fact for every language.

**Counts when.** The local form plus the language rules (or a named standard-library contract, §2.2) determine the presence and nature of the effect — including a determinate "no externally visible effect".

**Does NOT count when.** The effect is performed by a callee whose body/contract is outside the probe region → `LOOKUP`; or the effect depends on dynamic dispatch to an unknown implementation → `LOOKUP`; or ordering/visibility is unspecified (e.g. unsynchronised concurrent access) → `INDET`.

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

#### F12 — Control-flow effect

**Definition.** What this site does to the flow of control: falls through, branches, loops, exits early, propagates, throws, suspends/resumes, diverges (never returns), or transfers to another thread/task.

**Frozen distinction from F07.** F07 asks *can it fail and how is failure signalled*; a universal rule can answer that. F12 asks *where control goes*; a universal rule ("any call may throw") does **not** answer it, because the call may also diverge, loop forever, suspend, or transfer. Therefore an opaque call site is `LOOKUP` for F12 even where it is `COUNT` for F07.

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

#### F13 — Lifetime / resource effect

**Definition.** How long the entity lives, at which point it is released, and whether the site acquires or releases a non-memory resource (file, socket, lock, handle) with a determinate release point.

**Counts when.** The local form plus the language rules determine the release point (scope exit, block exit, explicit `defer`/`close`, end of borrow) or determinately establish that no cleanup occurs.

**Does NOT count when.** Release timing is unspecified because it is delegated to a garbage collector or to an unspecified reclamation mechanism → `INDET`; or the lifetime depends on ownership established outside the probe region → `LOOKUP`.

**Examples.**

| # | Language | Surface form | Verdict | Reason (basis) |
|---|---|---|---|---|
| + | C++ | `std::lock_guard<std::mutex> g(m);` | `COUNT` (acquires now, releases at scope exit) | [thread.lock.guard]; [class.dtor] |
| + | Python | `with open(p) as f:` | `COUNT` (closed at block exit, deterministically) | Python Reference §8.5; `io` contract |
| + | Go | `defer f.Close()` | `COUNT` (released at function return) | Go spec, "Defer statements" |
| + | Java | `try (var r = open()) { … }` | `COUNT` (closed at block exit) | JLS §14.20.3 |
| − | Java | `Object o = new Object();` | `INDET` | Finalization/reclamation timing is unspecified. JLS §12.6 |
| − | Swift | `let o = Obj()` (class instance) | `INDET` | ARC releases when the last strong reference goes away; the point is not determined by the local form. Swift Reference, "Automatic Reference Counting" |
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

This rule is applied identically to all 10 languages. It is the only reading under which the metric retains resolution: excluding all library names would zero out nearly every I/O, collection, and concurrency probe in every language simultaneously.

### 2.3 Interaction with Semantic Locality (metric C)

**The fact ledger is shared.** Metrics A (Density) and C (Locality) are computed from the same annotated rows, so the two metrics cannot disagree about a fact.

- A row with `hops_decl == 0` and `hops_stdlib ≤ 1` → `COUNT` → **contributes +1 to the Semantic Density numerator and 0 required declaration-graph hops to Semantic Locality.**
- A row with `hops_decl ≥ 1` (or `hops_stdlib ≥ 2`) → `LOOKUP` → **contributes 0 to Semantic Density and its `hops_decl` value to Semantic Locality's lookup count.**
- A row that is `INDET` → contributes 0 to Density and **0 to Locality** (a lookup would not resolve it, and spec §6.1.4.C says to count only context *required to resolve* a fact). It is instead evidence for Semantic Determinacy (B) and, where the behaviour is unsignalled at the use site, for Hidden Semantic Cost (D).
- `DUP` and `NA` rows contribute to neither.

**Therefore: a fact that needs a lookup is, by construction, not counted for Semantic Density.** There is no path by which the same row can both earn density and be free of locality cost, and no path by which a row is invisible to both metrics.

**Optional lookups are never counted** (spec §6.1.4.C): if the fact is already determinate from the LKS, a reader's optional deeper inspection adds nothing to `hops_decl`.

### 2.4 Minimality of hop counts

`hops_decl` is the **minimum** number of distinct external declarations that must be consulted, counted as distinct declaration entities (not as file opens, not as re-reads). Resolving an overload set counts as **1** hop regardless of the number of candidates; the candidate count is Determinacy (B) evidence, not Locality evidence.

### 2.5 Determinacy requirement

A row is `COUNT` only if the LKS yields a **single behaviourally determinate answer**. The following yield `INDET`:

- the specification says *undefined behaviour*, *unspecified behaviour*, or *erroneous/illegal behaviour with no defined outcome*;
- the specification says *implementation-defined* **and** the fact kind's content is the behaviour itself (F06, F07, F09, F10, F11, F13). For **F05 only**, implementation-defined *representation* does not block the row; see the representation sub-rule in §1.4/F05;
- the answer is a runtime-data-dependent property that no static reading can pin (e.g. which dynamic type an open interface value holds).

**Compiler-mode dependence is `LOOKUP`, not `INDET`** — the frozen build recipes in `../environment/environment.json` do resolve it, but consulting them is exactly a "global configuration or compiler mode" hop under §6.1.4.C. This is applied uniformly (it affects Rust overflow checks, Zig build modes, TypeScript `strict`/`noUncheckedIndexedAccess`, C++ `-O`-independent UB, and any Quidra mode flags identically).

### 2.6 Required ledger record

Facts are recorded as JSON Lines at
`../semantic-compression/raw/facts/<language>/<probe_id>.jsonl`, one object per annotated row:

```json
{
  "probe_id": "P07",
  "language": "rust",
  "site_id": "s4",
  "site_class": "S2",
  "span": {"line": 2, "col_start": 5, "col_end": 26},
  "construct": "c.wrapping_add(1)",
  "fact_kind": "F09",
  "disposition": "COUNT",
  "value": "two's-complement wraparound on overflow",
  "basis": "core::num::i32::wrapping_add contract, Rust 1.95.0 std docs",
  "basis_class": "stdlib-contract",
  "hops_decl": 0,
  "hops_stdlib": 1,
  "note": ""
}
```

`disposition ∈ {COUNT, LOOKUP, INDET, DUP, NA}`. `basis_class ∈ {local-marker, universal-rule, stdlib-contract}` and is required for `COUNT`. `note` is mandatory and non-empty for `INDET` and `NA` (this is the §26 "document the reason" requirement and the §32 anti-fabrication guard).

### 2.7 Audit requirement

After annotation, a re-annotation audit is run over a sample of **10% of the (language, probe) pairs, selected with a fixed seed `7677581`** by sorting all pairs lexicographically and drawing without replacement using Python's `random.Random(7677581).sample`. The re-annotation is performed from this document alone, without reading the first ledger. Disagreement in total `COUNT` rows of more than **±5%** for a sampled pair triggers re-adjudication of that pair against the rules in §1–§3; the adjudication and both counts are published in `../semantic-compression/raw/facts/audit.json`. The rules are never changed to resolve a disagreement.

---

## 3. Anti-double-counting

### 3.1 The rule

- **A3.1 — One fact per `(site, fact kind)`.** A fact kind is annotated at most once per site. The same fact spelled twice in one probe counts once.
- **A3.2 — Disjoint kinds are separate facts.** The thirteen kinds ask thirteen disjoint questions. Answering two of them at one site is two facts, even when one answer *entails* the other under the language rules. **Entailed facts count.** This is deliberate: the ability to communicate many facts from little syntax is precisely what Semantic Density measures (§6.1.1). The ceiling of 13 facts per site and the mandatory `basis` citation bound this.
- **A3.3 — Different sites, same value, same entity → `DUP`.** If a fact about the *same entity or the same operation* with the *identical value* has already been counted at another site of the same probe, the later row is `DUP` and counts 0. (Rule E4 in §1.1 prevents most of these from arising at all.)
- **A3.4 — Different entities → not duplicates.** Two variables each having their own mutability fact are two facts; they are different subjects.
- **A3.5 — Decorative syntax earns nothing and still costs tokens.** A construct whose removal would change no fact *value* at any site is decorative: every row it would create is `DUP` (if it restates a counted fact) or `NA` (if it corresponds to no taxonomy kind). Its tokens remain in the Semantic Density denominator. Decorative syntax therefore strictly *lowers* density — which is the intended direction.
- **A3.6 — Redundant grouping is not even a site** (rule E5).
- **A3.7 — No fact for being well-formed.** Compiling, type-checking, or satisfying a linter is not a taxonomy fact.

### 3.2 Worked examples

| Language | Surface form | Adjudication |
|---|---|---|
| Rust | `let mut count: i32 = 0i32;` | The type is spelled twice (annotation `i32` and literal suffix `0i32`). **One** F05 fact at the binding site; the literal is not a site (E3). Both spellings still cost tokens: 9 tokens vs 7 for `let mut count: i32 = 0;`. Net effect: lower density. |
| TypeScript | `const x: number = 5 as number;` | `as number` is an S2 conversion site, but its F05/F06 values are identical to those already established → both rows `DUP`, 0 facts, +3 tokens. Decorative. |
| Java | `@Override public void run() { … }` | `@Override` maps to no taxonomy kind (it asserts that an override exists; that is not one of F01–F13) → `NA`, 0 facts, +2 tokens. Contrast `final int x = 5;`, where `final` changes the F02 *value* and is therefore not decorative. |
| C++ | `const int x = 5;` and later `const int& r = x;` | Two S1 sites, two different entities → F02 counts at each. Not double counting (A3.4). |
| Go | `var x int = 0` vs `x := 0` | Identical facts (type `int` is pinned either way — by annotation, or by the untyped-constant default type rule). 5 tokens vs 3. The verbose form has strictly lower density. Go spec, "Constants" (default type of an untyped integer constant is `int`). |
| Swift | `let x: Int = 5` | The annotation duplicates the type already pinned by the literal → **one** F05 fact, +2 tokens. No penalty beyond the tokens. |
| Rust | `#[derive(Clone)] struct P { … }` | Not decorative: it changes the F10 value at the S4 type-definition site (the type acquires copy-by-clone semantics). Counts once, at S4. |
| Python | `x = int(5)` | The explicit `int(...)` call is an S2 conversion site whose F06 value ("identity conversion, no change") restates the type already pinned by the literal → `DUP`. +3 tokens, 0 facts. |

---

## 4. Language-neutral token-counting rule

This section discharges spec §6.1.4.A: *"Comments and whitespace do not count as source tokens. Use a documented language-neutral token-counting rule or a language lexer with an explicit reconciliation rule so punctuation-heavy and word-heavy syntaxes are treated consistently."*

We do **both**: a single documented tokenizer (§4.3–§4.6) that is authoritative for all 10 languages, plus a reference-lexer reconciliation procedure (§4.7) wherever a reference lexer exists on the measurement host.

### 4.1 Governing principle

The denominator measures **mandatory syntactic surface**, not authorial verbosity. Two distinct sources of token-count variation must be separated:

- **Grammar-imposed tokens** (C++ requires `;`, Java requires a type before every declarator, Zig requires `;`). These **stay**. Measuring them *is* the metric: a language that demands more mandatory punctuation per fact has lower semantic density. Erasing this difference would erase the metric.
- **Author-discretion tokens** (an optional semicolon, a redundant parenthesis, a comment, an unused import, a longer identifier). These are **canonicalised away** identically in every language (§4.2, R1–R6), so that nobody's score depends on who typed the probe.

Neutrality between punctuation-heavy and word-heavy syntaxes is achieved by the **uniform unit rule**: *every lexeme is exactly one token, whatever its length or class.* `func` is one token and `{` is one token; `wrapping_add` is one token and `+%` is one token. No class is weighted, discounted, or exempted. This is the only rule that treats "word-heavy" and "punctuation-heavy" symmetrically without a subjective weighting table.

### 4.2 Canonicalisation rules (applied before counting, identically to all 10 languages)

- **R1 — Probe region.** Each probe file contains exactly one region delimited by two frozen marker comment lines (spelled in that language's line-comment syntax): `BEGIN PROBE <probe_id>` and `END PROBE <probe_id>`. **Only tokens strictly between the marker lines are counted**, and only sites inside the region are annotated (§1.1/E6). Scaffolding that the language requires in order to have a runnable file at all — `package main`, `public class Main { public static void main(String[] a) {`, `fn main() {`, `const std = @import("std");`, `int main() {`, closing braces — lives outside the markers.
  **Exception R1a:** for probes in capability families **18 (modules/imports/dependency boundaries), 19 (concurrency/asynchrony), and 20 (FFI/interoperability)**, the construct under test *is* the boundary construct, so the relevant import/spawn/extern declarations are placed **inside** the markers and are counted and annotated. This is decided per probe in the frozen probe document, before any measurement.
- **R2 — Comments and whitespace are not tokens.** Line comments, block comments, doc comments, and all whitespace including newlines are removed and contribute nothing. Marker comment lines are removed with them.
- **R3 — No synthesis of invisible tokens.** Virtual/implicit tokens are never added: no Python `NEWLINE`/`INDENT`/`DEDENT`/`ENDMARKER`, no Go or JavaScript automatically-inserted semicolons, no implicit block delimiters for layout-based syntax. Whatever the author did not write is not counted.
- **R4 — Optional, fact-free lexemes are removed.** A lexeme is removed iff **(a)** the language's own grammar permits omitting it at that position **and (b)** omitting it changes no fact *value* at any site under §1. This removes written-but-optional statement terminators in Go, TypeScript, Kotlin and Swift, and removes redundant grouping parentheses in every language. It does **not** remove Rust's `;` (which distinguishes a statement from a trailing expression and therefore carries F01/F12 values), nor any mandatory terminator in C++, Java or Zig, nor a Python trailing comma that makes a tuple.
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

Nothing else emits a token. In particular: whitespace, comments, line continuations, byte-order marks, and end-of-file markers emit nothing.

### 4.4 The four contested cases, resolved

**(a) String interpolation.** An interpolated string emits:

> `1` token for the literal shell + for **each** embedded expression: `1` "interpolation hole" token + the tokens of the embedded expression, lexed by the same rules.

The hole costs exactly one token regardless of how the language spells it. This makes the four spellings identical in cost:

| Language | Source | Tokens |
|---|---|---|
| Python | `f"x={c}"` | `STR` shell, hole, `c` → **3** |
| TypeScript | `` `x=${c}` `` | `STR` shell, hole, `c` → **3** |
| Kotlin | `"x=$c"` | `STR` shell, hole, `c` → **3** |
| Swift | `"x=\(c)"` | `STR` shell, hole, `c` → **3** |
| Go | `fmt.Sprintf("x=%d", c)` | `fmt` `.` `Sprintf` `(` `STR` `,` `c` `)` → **8** |
| C++ | `std::format("x={}", c)` | `std` `::` `format` `(` `STR` `,` `c` `)` → **7** |

Languages without interpolation pay the call syntax, which is the honest measurement of their mandatory surface. Concatenation (`"x=" + c`) is lexed normally with no special rule.

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
| Kotlin | `@Suppress("x")` | `@` `Suppress` `(` `STR` `)` → **4** |
| Swift | `@inlinable` | `@` `inlinable` → **2** |
| Zig | `@intCast(x)` | `@` `intCast` `(` `x` `)` → **5** |

An attribute earns a fact only if the **language specification** (not a lint convention) attaches one of F01–F13 to it (§3.2).

**(d) Statement terminators.** Counted exactly as written, then filtered by R4. Consequently: C++, Java, Zig and Rust pay their mandatory `;`; Go, TypeScript, Kotlin and Swift do not pay for an optional `;` even if the author typed one; Python and layout-based syntax receive no synthesised terminator (R3). Rust's `;` is **never** removed by R4 because it carries fact values.

### 4.5 The frozen tokenizer

- **Script:** `../scripts/tokenize_probe.py` (Python 3.14.5, standard library only, no network, deterministic).
- **Profiles:** `../scripts/token_profiles.json` — one lexical profile per language (§4.6).
- **CLI:** `python3 tokenize_probe.py --lang <L> --probe <path> [--json-out <path>] [--selftest]`
- **Output:** `../semantic-compression/raw/tokens/<language>/<probe_id>.json`:
  `{"probe_id":…, "language":…, "count": N, "tokens":[{"tag":"KW","text":"let","line":1,"col":1}, …], "class_histogram":{…}, "removed_by_R4":[…], "profile_sha256":…, "script_sha256":…}`
- **Authority:** this script is authoritative for all 10 languages. Reference lexers (§4.7) are audit instruments only. This matters for fairness: languages lacking a usable reference lexer are not counted by a different instrument from those that have one.
- **Provenance:** the SHA-256 of the script and of the profile file are recorded in every output and in `../semantic-compression/scores/density.json`.

**Algorithm (implementable as written):**

1. Read the file as UTF-8. Locate the two marker comment lines by exact text match on `BEGIN PROBE <id>` / `END PROBE <id>`; if either is missing or duplicated, **abort with an error** (never guess a region). Take the text strictly between them.
2. Load the language's profile (§4.6). Sort the profile's operator table by descending lexeme length once, so that maximal munch is a simple ordered prefix match.
3. Scan left to right from position 0. At each position, try the recognisers in this fixed priority order and take the first that matches:
   1. **whitespace** (Unicode `White_Space`, including newlines) → consume, emit nothing;
   2. **line comment** (profile's line-comment starters) → consume to end of line, emit nothing;
   3. **block comment** (profile's block delimiters, honouring the profile's `nesting` flag: `true` for Rust, Kotlin, Swift; `false` for C++, Java, TypeScript, Go; absent for Python and Zig) → consume, emit nothing;
   4. **string or character literal** (profile's literal forms, honouring raw/multi-line/prefixed forms and the profile's escape character) → emit per §4.3/§4.4. For an interpolated form, emit `STR` for the shell, then for each hole emit one `HOLE` token and recursively scan the hole's contents with the same tokenizer state;
   5. **numeric literal** (profile's numeric grammar: base prefixes, digit separators, exponent forms, type suffixes) → emit one `NUM`;
   6. **identifier or keyword** (profile's identifier start/continue character classes, plus the profile's quoted-identifier forms) → emit `KW` if the lexeme is in the profile's reserved-word list, else `IDENT`;
   7. **operator** (longest match from the sorted operator table) → emit one `OP`;
   8. **delimiter** (profile's delimiter set) → emit one `DELIM`;
   9. **no match** → **abort with an error naming the file, line, column and offending character.** There is no silent skip and no catch-all fallback; an unrecognised character is a profile defect to be fixed *before* any probe is measured, and the fix is a profile change that is re-applied to every probe and re-hashed.
4. Apply **R4** as a post-pass over the emitted token list, using the profile's `optional_lexemes` rule set; removed tokens are retained in `removed_by_R4` for audit, not in `count`.
5. Emit the JSON. `count` is `len(tokens)` after step 4.
6. `--selftest` runs the frozen fixture set in `../scripts/token_fixtures.json`, which must include at minimum the §6 worked example (`rust = 17`, `go = 8`, `python = 6`) and one fixture per row of the §4.4 tables. Any mismatch is a hard failure and blocks measurement.

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
                 "interpolation": {"open": "\\(", "close": ")"}}],
    "chars": [{"open": "'", "close": "'", "escape": "\\"}],
    "ident_start": "XID_Start|_", "ident_continue": "XID_Continue",
    "quoted_ident": [{"open": "`", "close": "`"}],
    "numbers": {"prefixes": ["0x","0o","0b"], "separator": "_",
                "exponent": ["e","E","p","P"], "suffixes": []},
    "keywords": ["…"],
    "operators": ["…"],
    "delimiters": ["(",")","[","]","{","}",",",";",":",".","@","#"],
    "optional_lexemes": [{"lexeme": ";", "removable": true, "reason": "grammar permits omission; carries no fact value"}],
    "reference_lexer": {"available": true, "command": "…", "mapping": "…"},
    "fallbacks": []
  }
}
```

### 4.7 Reconciliation rule (required by §6.1.4.A)

For every language whose toolchain on this host exposes a reference lexer, the probe is **also** lexed by that reference lexer, its output is mapped to our token classes by a documented mapping, and the difference is published.

| Language | Reference lexer | Status | Mapping / reason |
|---|---|---|---|
| Python | `python3 -m tokenize` | **used** | Drop `NEWLINE`, `NL`, `INDENT`, `DEDENT`, `ENDMARKER`, `COMMENT`; f-strings re-mapped to shell+hole per §4.4. |
| Go | `go/scanner` driven by a 20-line Go program | **used** | Drop auto-inserted `;` (`Pos` of an inserted semicolon has literal `"\n"`), drop `EOF`. |
| C++ | `clang++ -std=c++20 -fsyntax-only -Xclang -dump-tokens` | **used** | Drop `eof`; join `greater greater` back into one `>>` where our maximal-munch rule differs; drop tokens outside the marker span by line/column. |
| TypeScript | `typescript` compiler API `ts.createScanner` via `node` | **used** | Drop `NewLineTrivia`, `WhitespaceTrivia`, all comment trivia; template spans re-mapped to shell+hole. |
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

For a language `L` over the fixed probe set `P` (the **40 frozen probes** `P01…P40` defined in the frozen capability/probe document, two probes per each of the 20 capability families of spec §6.1.2):

```
facts(L, p)   = number of ledger rows for (L, p) with disposition == COUNT
tokens(L, p)  = frozen-tokenizer count for (L, p) after canonicalisation R1–R6

                       Σ_{p ∈ P_applicable(L)} facts(L, p)
raw_density(L)  =  ───────────────────────────────────────────
                       Σ_{p ∈ P_applicable(L)} tokens(L, p)
```

Unit: **explicitly recoverable semantic facts per lexical source token.** Higher is better (§6.1.4.A).

### 5.2 Aggregation (frozen)

The aggregation is **sum of facts over sum of tokens** across the 40 probes — a single macro-ratio. It is explicitly **not** the mean of the 40 per-probe ratios.

Reason, frozen in advance: a per-probe mean gives a one-line probe the same leverage as a twenty-line probe, which would let probe granularity move a score. The macro-ratio weights each probe by its actual syntactic mass, which is the quantity §6.1.4.A names.

Per-probe ratios `facts(L,p)/tokens(L,p)` and per-capability-family subtotals are nevertheless **published in full** in `../semantic-compression/scores/density.json` and in the §29 raw tables, so the aggregation choice is inspectable and the distribution is visible.

### 5.3 Applicability, unsupported probes, and partial support

- **Supported probe:** implemented per the frozen probe statement using documented language features and the normal standard runtime/library, with no benchmark-specific external packages and no code generation (§6.1.2). Included in both sums.
- **Partially supported probe** (expressible only via the workaround permitted by the frozen partial-support rubric): the frozen partial implementation is the measured source. Its facts and tokens are included normally. The partial-support flag affects Capability Coverage `C`, not this metric.
- **Unsupported probe:** no source exists, so the probe contributes **0 facts and 0 tokens** — it is excluded from *both* sums. This is not a free win: spec §6.1.5 and §26 keep the unsupported capability in the Capability Coverage denominator, and the harmonic mean of §6.1.6 applies the penalty there. Every exclusion is recorded with its reason in `density.json` under `excluded_probes`.
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

`raw_density`, `total_facts`, `total_tokens`, `per_probe: [{probe_id, facts, tokens, ratio, support_status}] × 40`, `per_family: [{family_id, facts, tokens, ratio}] × 20`, `excluded_probes: [{probe_id, reason}]`, `disposition_histogram: {COUNT, LOOKUP, INDET, DUP, NA}`, `basis_class_histogram: {local-marker, universal-rule, stdlib-contract}`, `normalized_score`, `script_sha256`, `profile_sha256`, `reconciliation_status`.

Publishing `disposition_histogram` and `basis_class_histogram` is mandatory: together they let any auditor see exactly how much of each language's density comes from explicit markers versus universal rules versus library contracts, which is the audit hook for invariant I5.

---

## 6. Worked end-to-end example (illustrative only — NOT one of the 40 probes)

**Task:** bind a 32-bit signed integer to `0`, then add `1` to it with wrapping-on-overflow semantics.
This example exists solely to demonstrate that §1–§5 are mechanical. It is not part of the measured probe set, and its numbers do not enter any score.

### 6.1 Rust — 20 facts / 17 tokens = 1.176

```rust
let mut c: i32 = 0;
c = c.wrapping_add(1);
```

Tokens (17): `let` `mut` `c` `:` `i32` `=` `0` `;` `c` `=` `c` `.` `wrapping_add` `(` `1` `)` `;`

Sites: `s1` = S1 binding `c`; `s2` = S2 initialization `=`; `s3` = S2 assignment `=`; `s4` = S2 call `wrapping_add`.

| Site | Kind | Disp. | Value | basis_class |
|---|---|---|---|---|
| s1 | F01 | COUNT | a place (variable) with storage in the enclosing block | local-marker |
| s1 | F02 | COUNT | mutable (`mut`) | local-marker |
| s1 | F03 | COUNT | no alias exists; any alias requires a visible `&`/`&mut` | universal-rule |
| s1 | F04 | COUNT | initialized at declaration | local-marker |
| s1 | F05 | COUNT | `i32`, 32-bit two's complement | local-marker |
| s1 | F08 | NA | `i32` has no alternative cases | — |
| s1 | F10 | COUNT | no allocation; `i32: Copy`; no `Drop` | universal-rule |
| s1 | F13 | COUNT | lives to end of block; no resource | universal-rule |
| s2 | F04 | DUP | restates s1's initialization fact | — |
| s2 | F06 | COUNT | no implicit conversion; `0` inferred as `i32` | universal-rule |
| s2 | F10 | COUNT | copy of a `Copy` value into the place | universal-rule |
| s2 | F01/F05 | NA | gate G-value: result discarded | — |
| s2 | F07/F09/F11/F12 | NA | gate G-effect: local, non-arithmetic, non-call | — |
| s3 | F02 | COUNT | modifies the already-initialized `c` | local-marker |
| s3 | F06 | COUNT | no conversion; RHS is `i32` | universal-rule |
| s3 | F10 | COUNT | copy; no prior value to drop | universal-rule |
| s3 | F01/F04/F05/F07/F09/F11/F12 | NA | gates | — |
| s4 | F01 | COUNT | produces a value, not a place | universal-rule |
| s4 | F05 | COUNT | result type `i32` | stdlib-contract |
| s4 | F06 | COUNT | argument `1` is `i32`; no implicit conversion | universal-rule |
| s4 | F07 | COUNT | cannot fail | stdlib-contract |
| s4 | F08 | NA | result is not an alternative type | — |
| s4 | F09 | COUNT | two's-complement wraparound on overflow | stdlib-contract |
| s4 | F10 | COUNT | receiver copied; no allocation, no borrow | universal-rule |
| s4 | F11 | COUNT | no externally visible effect | stdlib-contract |
| s4 | F12 | COUNT | returns normally; cannot panic or diverge | stdlib-contract |

`COUNT` rows: 7 (s1) + 2 (s2) + 3 (s3) + 8 (s4) = **20**. Tokens **17**. Ratio **1.176**.

### 6.2 Go — 14 facts / 8 tokens = 1.750

```go
var c int32 = 0
c += 1
```

Tokens (8): `var` `c` `int32` `=` `0` `c` `+=` `1`

- `s1` (binding): F01 COUNT (variable with storage), F02 COUNT (all Go variables are assignable — universal-rule), F03 COUNT (no alias without a visible `&`), F04 COUNT (explicitly initialized; also the zero-value rule), F05 COUNT (`int32`, 32-bit two's complement), F10 COUNT (no observable allocation), F13 COUNT (no destructors; nothing runs at scope exit) → **7**
- `s2` (initialization `=`): F06 COUNT (untyped constant `0` converted to `int32` at compile time; representable), F10 COUNT (copy) → **2**
- `s3` (compound assign `+=`, one site by E2): F02 COUNT (modifies `c`), F06 COUNT (untyped constant `1` → `int32`), F07 COUNT (cannot fail), F09 COUNT (signed overflow wraps — Go spec "Integer overflow"), F10 COUNT (copy) → **5**

Total **14 / 8 = 1.750**.

### 6.3 Python — 10 facts / 6 tokens = 1.667

```python
c = 0
c += 1
```

Tokens (6): `c` `=` `0` `c` `+=` `1`

- `s1`: F01 COUNT (a name bound to an object; the binding is not itself storage), F02 COUNT (name rebindable; the bound `int` object is immutable — pinned by the literal), F03 COUNT (other names may refer to the same object, but `int` is immutable so no writable aliasing is possible), F04 COUNT (bound), F05 COUNT (`int`, unbounded precision), **F10 INDET** (whether `0` creates a new object is unspecified), **F13 INDET** (destruction timing is unspecified), F08 NA → **5**
- `s2` (`=`): F06 COUNT (no implicit conversion at binding), F10 INDET → **1**
- `s3` (`+=`): F02 COUNT (rebinds `c`; `int` is immutable so `+=` does not mutate in place), F06 COUNT (int + int → int, no conversion), F07 COUNT (cannot fail; U-07.1 excludes `MemoryError`), F09 COUNT (unbounded precision — overflow cannot occur), F10 INDET → **4**

Total **10 / 6 = 1.667**. (In the real probe set this task would additionally be flagged partial-support for Python, since fixed-width 32-bit wrapping is not expressible without a masking workaround; that flag affects Capability Coverage `C`, not this metric.)

### 6.4 What the example demonstrates

Under these frozen rules the two *least* explicitly annotated languages out-score the most explicitly annotated one on this task, because Go and Python obtain the same facts from universal rules at a fraction of the token cost while Rust pays 17 tokens for the explicit wrapping call. That is the intended, spec-faithful behaviour of the metric under invariant I5, and it is direct evidence that the rule is not constructed to favour a language with explicit semantic markers.

---

## 7. Freeze declaration

This document is frozen as of benchmark run `2026-09-17-7677581`, before any probe is annotated and before any token is counted. It may not be amended in response to observed counts, observed rankings, or any language's performance (§25.4, §32). Any defect discovered during measurement is recorded, published, and — if it must be fixed — forces a full re-run of Semantic Density for **all 10 languages** under the amended rule, with both the old and the new results published side by side (§25.1).

**Dependencies produced by this document, to be implemented exactly as specified:**

- `../scripts/tokenize_probe.py`, `../scripts/token_profiles.json`, `../scripts/token_fixtures.json`
- `../semantic-compression/raw/facts/<language>/<probe_id>.jsonl`, `../semantic-compression/raw/facts/audit.json`
- `../semantic-compression/raw/tokens/<language>/<probe_id>.json`, `../semantic-compression/raw/tokens/reconciliation.json`
- `../semantic-compression/scores/density.json`
