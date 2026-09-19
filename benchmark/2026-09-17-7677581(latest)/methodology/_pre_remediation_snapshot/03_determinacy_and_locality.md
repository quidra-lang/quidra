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
documents. This document binds to whatever probe IDs and fact annotations the frozen probe-set
document defines. Where this document needs to refer to specific probes it does so **by function**,
not by ID:

* **ARG-PLAIN** — the mandatory argument-passing probe of §6.1.4.B: an ordinary call-site expression
  equivalent to `f(x)`, with no reference / borrow / address / alias marker anywhere in the span.
* **ARG-EXPLICIT** — the companion probe of §6.1.4.B: the same call expressed with whatever explicit
  reference, borrow, address, dereference, or writable-alias form the language provides
  (`f(&x)`, `f(&mut x)`, `f(x*)`, `f(ref x)`, …), or `N/A-UNSUPPORTED` if the language provides none.
* **ARITH** — the arithmetic-operator probe of capability families 7–8: a binary `a + b` on two
  previously declared numeric variables.

The frozen probe corpus contains **40 probes**: 2 probes for each of the 20 capability families of
§6.1.2. Formulas below are written for `N = 40`; if the frozen corpus document states a different
count, every formula uses that `N` and the predeclared constants of §1.8 and §2.9 are recomputed from
it by the stated formula (`epsilon = 1/N`) before any language is scored.

### 0.2 Fairness posture (binding)

Restated from §32 and §6.1.7 because these rules are where bias would enter most easily:

1. Nothing in this document is derived from Quidra's syntax, operators, types, or feature set. Every
   axis, outcome class, and ruling below was written from the union of the ten languages' own
   authoritative specifications and from the capability families of §6.1.2.
2. A language may score well on something Quidra does badly, and badly on something Quidra does well.
   Nothing here is conditioned on which language is being scored. The procedure is applied by the
   same script and the same two analysts, blind to the running totals (§3.2).
3. Capabilities Quidra lacks remain in the universe. A probe a language cannot express is
   `N/A-UNSUPPORTED` **for these two metrics only**, and is charged in full against Capability
   Coverage `C` (§6.1.5, §26). It is never silently deleted.
4. **No glyph reasoning.** See §1.6. Determinacy is scored from the complete surface form under the
   language's own specification, never from resemblance to another language's notation.
5. Where a count cannot be made honestly, it is marked `N/A` with a machine-readable reason code
   (§1.9, §2.10). Nothing is estimated, interpolated, or filled in by analogy.

### 0.3 Relationship between the two metrics

Determinacy and Locality are **not** two views of one quantity, and a language can score well on one
and badly on the other. The canonical demonstration, worked out in full below, is Python at
**ARG-PLAIN**: Python has the *lowest* branching count of the nine non-Quidra languages examined
(`B = 3`) because its call semantics admit very few outcome classes, and simultaneously a *high* hop
count (`H = 3`, including a mandatory callee-body hop) because no Python signature carries the fact
that resolves the remaining branch. Meanwhile C++ has both the highest `B` (11) and a high `H`.
Reporting both is therefore informative, not redundant. Neither metric is permitted to be
back-derived from the other.

---

## 1. PART 1 — SEMANTIC DETERMINACY (`B_i`)

> §6.1.4.B: "For each fixed probe, count the number `B_i` of materially different semantic
> interpretations that remain compatible with the local surface form before consulting external
> declarations or whole-program facts."

### 1.1 The local surface form: the *focus span*

Every probe record in the frozen corpus carries, per language, a complete compilable program and a
marked **focus span**: a contiguous token range of that program.

**Rule 1.1.1 (definition).** The *focus span* is the probe's marked token range and nothing else. It
includes every token inside the range — keywords, modifiers, annotations, punctuation, literals and
identifiers — and excludes every token outside it.

**Rule 1.1.2 (everything else is external).** For `B_i`, all of the following are **external** and
must not be consulted, even when they appear in the same file, the same function, or the line
immediately above: variable declarations, callee signatures, type declarations, trait/interface/
protocol implementations, overload sets, imports, attributes on declarations, build flags, language
edition/version selectors, and the probe's own preamble. This is the literal reading of "before
consulting external declarations or whole-program facts" and it is what makes the count comparable
across languages: a language that puts a fact inside the span keeps its branch closed; a language
that puts the same fact in a declaration does not.

**Rule 1.1.3 (span equivalence across languages).** The focus span for a probe denotes the *same
semantic event* in all 10 languages — the same call, the same operator application, the same
index — rendered idiomatically per the frozen corpus. Analysts may not widen one language's span to
capture a declaration, or narrow another's to hide a modifier. Where a language's idiomatic rendering
of the event unavoidably carries a modifier inside the span (e.g. a required `await`, a required
`try`, a required `mut`), that modifier is part of the span and its determinacy effect is counted.
Where a language's idiomatic rendering unavoidably omits such a modifier, its absence is likewise
part of the span.

**Rule 1.1.4 (language spec is free).** Facts fixed by the language's own specification for the
tokens inside the span are *known*, at zero cost, and close branches. Example: the Go specification
states that all arguments are passed by value; that fact is available to the analyst at ARG-PLAIN
without any lookup, and it eliminates outcome classes for Go. This is the one and only body of
external knowledge admitted in Part 1, and it is admitted identically for all ten languages,
including for Quidra (from the Quidra language reference at the frozen HEAD SHA).

### 1.2 "Materially different" — the materiality test

**Rule 1.2.1 (test M).** Two candidate interpretations `u` and `v` of the same focus span are
**materially different** iff there exists a well-formed **observer program** — identical inside the
focus span, differing only outside it — under which `u` and `v` differ in at least one of the nine
dimensions named in §6.1.4.B:

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
(dimension 6) is material only where the language gives a program a defined way to observe the event
— a user-defined constructor/destructor/`deinit`/`Drop`, an allocator or `new`/`delete` hook, a
reference-count query (`isKnownUniquelyReferenced`, `Rc::strong_count`, `sys.getrefcount`), or a
finalizer with defined timing. Where the language makes the event unobservable to conforming programs
(e.g. an elided copy the specification permits the implementation to remove, GC-internal copying),
the difference is not material and does not create a branch. This rule is what stops copy-elision
lawyering from inflating `B` for compiled languages.

**Rule 1.2.3 (existence of the observer must be demonstrated).** Materiality is never asserted. It is
demonstrated by the witness of Rule 1.3.1.

### 1.3 The witness rule (the reproducibility anchor)

**Rule 1.3.1 (witness).** An interpretation is counted **only if** the analyst records a **witness**:
a concrete completion of the probe program — declarations, signatures, types, imports and build
invocation outside the focus span, with the focus span byte-identical to the frozen probe — that

* (a) builds and runs under the **frozen toolchain recipe** for that language
  (`environment/environment.json` → `frozen_toolchain_recipes`), on the frozen host;
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

**Rule 1.3.2 (witnesses are preserved).** Every witness is stored under
`semantic-compression/probes/witnesses/<probe_id>/<language>/<class_id>/` together with its build
command and observer output, and is referenced by path from the raw determinacy record (§1.9).

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

**Rule 1.4.1 (in-scope axes).** For each probe the frozen probe record lists the semantic facts of
§6.1.3 that the probe is annotated with. The **in-scope axes** for that probe are exactly the axes
that the following fixed mapping associates with those facts. The in-scope set is identical for all
ten languages for a given probe, and is frozen with the probe.

| §6.1.3 fact | Axes brought in scope |
|---|---|
| value versus storage | A1 |
| mutability | A2 |
| aliasing / writable aliasing | A1, A2 |
| initialization state | A3 |
| type and representation | A4 |
| conversion behavior | A5 |
| possible failure | A6 |
| alternative value cases | A6, A7 |
| overflow / exceptional numeric behavior | A10 |
| allocation, copying, moving, borrowing, or destruction | A8 |
| externally visible side effects | A2, A7 |
| control-flow effect | A9 |
| lifetime / resource effect | A8 |

**Rule 1.4.2 (ARG-PLAIN / ARG-EXPLICIT in-scope axes).** §6.1.4.B fixes these probes' question:
"how many materially distinct **aliasing / mutation** outcomes remain possible from the call-site
syntax alone". Their in-scope axes are therefore exactly **A1 × A2**, with the specialised outcome
tables of §1.5.1. A6, A9 and A10 are *not* in scope for these probes; they are in scope for the
error-handling, control-flow and arithmetic probes respectively. This is the mechanism that prevents
the same language property from being counted twice inside Semantic Determinacy.

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

**Rule 1.4.8 (token substitution).** A mechanism that can replace the focus span's tokens before
semantic analysis, **without any marker inside the span**, contributes exactly one additional outcome
(A1 = non-call/token substitution). This applies to the C/C++ preprocessor. It does not apply to
mechanisms that require a marker in the span (Rust `name!(…)`, Nim-style templates with explicit
invocation, Zig `comptime` blocks) and it does not apply to languages without such a mechanism.

**Rule 1.4.9 (minimum).** `B_i ≥ 1`. `B_i = 1` means the focus span, read against the language
specification alone, admits exactly one materially different interpretation: **fully determined**.
`B_i = 0` is not defined and must never be recorded.

**Rule 1.4.10 (no cap).** There is no upper cap on `B_i`. Every counted branch carries a witness, so a
large `B_i` is auditable rather than rhetorical. Values above 16 additionally require both analysts to
sign the enumeration (§3.2).

### 1.5 The two specialised outcome tables

These two tables are frozen instantiations of §1.4 for the probe families where §6.1.4.B mandates a
specific question. They exist so that the enumeration is a *checklist*, not an essay.

#### 1.5.1 Table ARG — argument-passing probes (ARG-PLAIN, ARG-EXPLICIT)

In-scope axes A1 × A2. `P` classes are mutually exclusive; `M` classes are mutually exclusive.

**Axis P (what happens to the argument at the boundary):**

| Id | Class | Question the analyst answers |
|----|-------|------------------------------|
| P1 | copy / derived handle | Can the callee receive an independent copy of the operand's value (or a reborrowed/derived handle), with the operand still usable afterwards? |
| P2 | converting copy | Can an implicit conversion at this span hand the callee an object of a *different type* than the operand's? |
| P3 | move / consume | Can passing leave the operand invalid, unusable, or in a documented changed state? |
| P4 | alias to the operand's own storage | Can the callee be given access to the operand variable's own storage, with no copy of its value? |
| P5 | deferred | Can the callee's body fail to execute at this span (generator, lazy coroutine, by-name)? |
| P6 | non-call | Can the span fail to denote a call at all (Rule 1.4.8)? |

**Axis M (what caller-observable mutation is possible through this argument):**

| Id | Class |
|----|-------|
| M0 | none — after the call, no caller-observable state can have changed through this argument |
| M1 | the callee may write the **caller's variable's own storage** (replace or modify the variable itself) |
| M2 | the callee may mutate state **referred to** by the argument, but not the variable's own storage |

`B = |{(P,M) pairs with a witness}|`, after the entailment collapses of Rule 1.4.4 (P3, P5 and P6 each
contribute exactly 1 and take no M sub-branch).

**Rule 1.5.1.a (M1 vs M2 boundary).** M1 is *the caller's variable's own storage*. This phrase is
unambiguous in every one of the ten languages and deliberately avoids each language's private
value/object vocabulary. It is M1 only if the callee can make the caller's *variable* hold something
different, or write bytes of that variable. Everything else reachable through the argument is M2.

**Rule 1.5.1.b (reborrow).** A derived alias handed to the callee while the operand remains usable
afterwards is **P1**, not P3 and not P4 (Rust implicit reborrow of `&mut`, Swift `borrowing` of a
Copyable value). The operand here is the *variable named in the span*; if that variable is itself a
reference, mutation of its referent is M2 by Rule 1.5.1.a.

#### 1.5.2 Table ARITH — arithmetic-operator probes

In-scope axes A5 × A6 × A10 (from the fact annotation "conversion behavior", "possible failure",
"overflow / exceptional numeric behavior"). Outcome classes, mutually exclusive as *behaviour
classes*; `B = |{realizable classes with a witness}|`:

| Id | Class |
|----|-------|
| R1 | exact / arbitrary precision — overflow is not possible |
| R2 | wraps modulo 2^n, defined |
| R3 | may trap / panic / throw at run time |
| R4 | undefined behaviour on overflow |
| R5 | behaviour is selected by a build mode or global configuration (counted once per *distinct additional* behaviour a documented supported mode produces; see Rule 1.7) |
| R6 | binary floating point — rounding, NaN, Inf |
| R7 | an implicit promotion/conversion is applied to an operand before the operation |
| R8 | the operator's meaning is supplied by the operand types (user-redefinable): may allocate, fail, mutate, or be non-arithmetic |
| R9 | pointer/handle arithmetic rather than numeric arithmetic |

R5 is not counted *in addition to* the behaviour it selects: if a build mode makes the span trap, that
is R3; R5 exists only to record that the selection is non-local, and is **reported as a flag on the
probe record**, never as an extra `+1`. (The non-locality itself is charged in Part 2 as a G-hop.)

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
  ARG-EXPLICIT `N/A-UNSUPPORTED` and a Capability Coverage deduction, per §1.9 and §6.1.5).

**Rule 1.6.2 (worked consequence of the glyph rule).** The same glyph yields very different counts,
and that difference must come out of the enumeration rather than out of sympathy. Summary of the
ARG-EXPLICIT enumerations of §1.7.4:

| Span | Language | `B` | Why the enumeration lands there |
|---|---|---|---|
| `f(&mut x)` | Rust | 3 | unique writable borrow of `x`'s storage (M1); deref-coercion to a `&mut` of *reachable* storage, e.g. `&mut String → &mut str` (M2); `async fn` deferral (P5). No copy, no move, no conversion, no macro. |
| `f(&x)` | Zig | 2 | `*T` parameter (M1) or coercion to `*const T` (M0). No overloads, no user conversions, no preprocessor. |
| `f(&x)` | Go | 2 | `*T` parameter (M1); implicit interface boxing of the pointer, a representation change with dynamic dispatch (P2·M1). |
| `f(&x)` | Swift | 4 | `inout` copy-in/copy-out (M1); implicit conversion to `UnsafeMutablePointer<T>` (M1, different representation and validity window); to `UnsafePointer<T>`/`UnsafeRawPointer` (M0); for an array operand, to a pointer to the *buffer* (M2). |
| `f(&x)` | C++ | 6 | `T*` (M1); `const T*` (M0); implicit pointer→`bool` or →`void*` conversion (P2·M0); converting constructor capturing the pointer (P2·M1); coroutine deferral (P5); function-like macro (P6). An overloaded `operator&` is additionally realizable and merges into P2 by Rule 1.4.5. |

The same glyph produces 2, 2, 3, 4 and 6. Nothing here was awarded for using `&`; every number came
from the closed table plus witnesses.

### 1.7 Worked examples — binding precedents

These enumerations are **binding precedents**. A measuring agent applying §1.1–§1.6 to these probes in
these languages must reproduce these numbers; if it does not, it has made an error or found an
adjudication-register case (§3.3).

Quidra is deliberately absent from the precedents. Precedents exist to calibrate the procedure on
languages whose specifications are long-settled and publicly checkable; Quidra's `B_i` values are
produced at measurement time by the identical procedure against the frozen corpus, with the same
witness requirement, and no value is pre-assigned to it here.

#### 1.7.1 ARG-PLAIN — focus span `f(x);`

The span is a call statement in which `f` and `x` are plain identifiers, declared outside the span.
In-scope axes: A1 × A2, Table ARG (§1.5.1).

##### C++ (`clang++ -std=c++20 -O2`) — `B = 11`

| # | (P, M) | Witness sketch (declarations outside the span) | Observer |
|---|--------|-----------------------------------------------|----------|
| 1 | P1·M0 | `struct S{int v;}; void f(S);` | callee's writes invisible; copy ctor counter = 1 |
| 2 | P1·M2 | `struct S{int*p;}; void f(S s){*s.p=9;}` | `*p` changes; `x` itself unchanged |
| 3 | P2·M0 | `struct U{U(const S&);}; void f(U);` | `U`'s ctor counter fires; `x` unchanged |
| 4 | P2·M1 | `struct W{S*p; W(S&s):p(&s){}}; void f(W w){w.p->v=9;}` — implicit converting ctor taking `S&` hands the callee a writable pointer to `x` itself | `x.v` changes |
| 5 | P2·M2 | `struct V{int*p; V(const S&s):p(s.p){}}; void f(V);` | `*x.p` changes |
| 6 | P3 | `struct S{S(S&o){o.p=nullptr;}};` — a conforming copy constructor taking a non-const reference may empty its source | `x.p == nullptr` after the call |
| 7 | P4·M0 | `void f(const S&);` | no copy ctor fires; `x` unchanged |
| 8 | P4·M1 | `void f(S& s){s.v=9;}` (merged with the `mutable`-member and `const_cast` variants by Rule 1.4.5) | `x.v` changes, copy ctor counter = 0 |
| 9 | P4·M2 | `void f(const S& s){*s.p=9;}` — `const S&` makes the *pointer member* const, not the pointee | `*x.p` changes, `x` unchanged |
| 10 | P5 | `Task f(S);` with `initial_suspend()` returning `suspend_always` — the body does not run at this span | instrumentation in the body does not fire |
| 11 | P6 | `#define f(a) ((a).v = 0)` — the span is not a call | no function is entered; `x.v == 0` |

`log2(11) = 3.4594`.

##### Python 3.14 — `B = 3`

| # | (P, M) | Witness | Observer |
|---|--------|---------|----------|
| 1 | P4·M0 | `x = 7` (or any immutable object); `def f(a): a = a + 1` | `x` unchanged; rebinding the parameter is invisible — merges with the immutable case, same vector |
| 2 | P4·M2 | `x = [1]; def f(a): a.append(2)` | `x == [1, 2]` |
| 3 | P5 | `def f(a): yield a.pop()` (or `async def`) — the body does not run at this span | the `pop` never happens until the generator is driven |

Not realizable, with reasons: **P1** — the language reference states that argument passing binds the
same object; Python performs no implicit copy at a call boundary. **P2** — no implicit conversion is
applied to an argument. **P3** — no move/consume semantics; `x` remains bound. **P6** — no textual
substitution mechanism. **M1** — a callee cannot rebind or write the caller's variable itself.

`log2(3) = 1.5850`. This is the **lowest** ARG-PLAIN branching count among the nine languages worked
here. It is recorded as measured. Python's cost appears in Part 2, not here.

##### Rust 1.95 (`rustc -O`) — `B = 4`

| # | (P, M) | Witness | Observer |
|---|--------|---------|----------|
| 1 | P1·M0 | `#[derive(Clone,Copy)] struct S{v:i32}` with `fn f(s:S)`; or `let x:&S` with `fn f(s:&S)` | `x` still usable, unchanged |
| 2 | P1·M2 | `let x:&Cell<i32>` with `fn f(c:&Cell<i32>){c.set(9)}`; or `let x:&mut i32` reborrowed by `fn f(r:&mut i32)` with `x` still usable after | the cell/referent changes; `x` itself unchanged and still usable |
| 3 | P3 | `struct S{v:Vec<i32>}` (non-`Copy`) with `fn f(s:S)` | `x` is moved; use after the call is a compile error — the observer is the rejected program |
| 4 | P5 | `async fn f(s:S)` — calling it constructs a future and does not run the body | body instrumentation does not fire |

Not realizable: **P2** — Rust applies no value-producing implicit conversion in argument position;
deref/unsize coercions retarget a *reference type* without constructing a new owned object and are
therefore P1, not P2 (binding ruling, adjudication register AR-004). **P4/M1** — a Rust call site
cannot hand the callee access to the storage of `x` itself without `&`/`&mut` inside the span. **P6**
— macro invocation requires `!`.

`log2(4) = 2.0000`.

##### Go 1.26 — `B = 4`

| # | (P, M) | Witness | Observer |
|---|--------|---------|----------|
| 1 | P1·M0 | `type S struct{v int}; func f(s S)` | `x` unchanged |
| 2 | P1·M2 | `x []int` or `type S struct{p *int}` with `func f(s S){*s.p=9}` | backing array / pointee changes |
| 3 | P2·M0 | `func f(w fmt.Stringer)` with a value-receiver concrete type — implicit interface conversion: representation change plus possible boxing allocation | allocation/boxing observable; mutations land in the boxed copy |
| 4 | P2·M2 | `func f(a any)` with `x` a pointer or slice — boxed, then mutated through a type assertion | pointee changes |

Not realizable: **P3** (no move semantics), **P4/M1** (no implicit address-taking; `&` is required in
the span), **P5** (`go`/`defer` are markers inside the span when present), **P6**.

`log2(4) = 2.0000`.

##### Java 26 — `B = 4`

| # | (P, M) | Witness | Observer |
|---|--------|---------|----------|
| 1 | P1·M0 | `void f(int a)` with `int x`; or a `String`/record operand | `x` unchanged |
| 2 | P1·M2 | `void f(int[] a){a[0]=9;}` with `int[] x` | array contents change |
| 3 | P2·M0 | `void f(Object a)` with `int x` — autoboxing; or `void f(long a)` with `int x` — widening primitive conversion | boxed identity / value width observable |
| 4 | P2·M2 | `void f(Object... a)` with a reference operand — varargs array construction is an implicit conversion at the span, and the referent remains mutable | new array allocated; referent mutation visible |

Not realizable: **P3**, **P4**, **M1** (Java cannot modify a caller's variable), **P5** (no unmarked
deferred call), **P6**.

`log2(4) = 2.0000`.

##### Swift 6.2 (`swiftc -O`) — `B = 6`

| # | (P, M) | Witness | Observer |
|---|--------|---------|----------|
| 1 | P1·M0 | `struct S{var v:Int}; func f(_ s:S)` | value semantics; `x` unchanged |
| 2 | P1·M2 | `final class C{var v=0}; func f(_ c:C){c.v=9}` — the operand is a class reference; or a struct holding one | instance mutates; `x` (the reference) unchanged |
| 3 | P2·M0 | `func f(_ p:P)` with `S:P` — implicit existential boxing; or `func f(_ o:Int?)` with `Int x` — implicit Optional promotion | representation change observable via `type(of:)`/boxing |
| 4 | P2·M2 | existential/Optional wrapping of a class reference, mutated through the wrapper | instance mutates |
| 5 | P3 | `struct S: ~Copyable {}` with `func f(_ s: consuming S)` — the operand is consumed at an unmarked call site | use after the call is rejected — the observer is the rejected program |
| 6 | P4·M0 | `func f(_ s: borrowing S)` — the callee reads `x`'s storage with no copy | `isKnownUniquelyReferenced` on a class-backed payload shows no copy was made |

Not realizable: **M1** (`inout` requires `&` inside the span), **P5** (`async` requires `await` inside
the span; `@autoclosure` deferral is not material when the argument is a plain variable reference —
binding ruling AR-002), **P6**.

`log2(6) = 2.5850`.

##### TypeScript 7 / Node 24, Kotlin 2.3, Zig 0.16 — compact enumerations

| Language | `B` | Realizable classes | Principal exclusions |
|---|---|---|---|
| TypeScript | 5 | P1·M0 (primitive/frozen); P1·M2 (object property mutated); P2·M0 and P2·M2 (rest-parameter packing constructs a new array at the span — the same ruling applied to Java/Kotlin varargs); P5 (`function* f(x)` — a generator's body does not run at the span) | P3, P4, M1, P6; no implicit coercion is applied to *argument binding* in JS (coercion lives in operators, scored at ARITH) |
| Kotlin | 4 | P1·M0; P1·M2; P2·M0 and P2·M2 (autoboxing to `Any`/platform types; `vararg` array construction) | P3, P4, M1, P6. `suspend fun f(x)` is *not* P5: the body does begin executing at the span. Kotlin's unmarked suspend call is charged at the concurrency probe and in Part 2, not here |
| Zig | 4 | P1·M0; P1·M2 (operand is or contains a pointer/slice); P2·M0 and P2·M2 (Zig's documented implicit coercions at call sites: integer widening, `T → ?T`, `T → E!T`, `*T → *const T`, `*[N]T → []T`) | P3 (no move semantics in 0.16); P4/M1 — parameters are immutable and `&` is required in the span to expose `x`'s storage; P5 (no unmarked deferred call in 0.16); P6 (no preprocessor) |

##### ARG-PLAIN summary (nine languages; Quidra measured at run time)

| Language | `B` | `log2(B)` |
|---|---:|---:|
| Python | 3 | 1.5850 |
| Rust | 4 | 2.0000 |
| Go | 4 | 2.0000 |
| Java | 4 | 2.0000 |
| Kotlin | 4 | 2.0000 |
| Zig | 4 | 2.0000 |
| TypeScript | 5 | 2.3219 |
| Swift | 6 | 2.5850 |
| C++ | 11 | 3.4594 |

#### 1.7.2 ARITH — focus span `a + b`

`a` and `b` are previously declared variables outside the span. In-scope axes A5 × A6 × A10, Table
ARITH (§1.5.2). Five languages worked; the remainder follow the identical checklist.

| Language | Realizable classes | `B` | `log2(B)` | Notes |
|---|---|---:|---:|---|
| C++ | R2 (unsigned wrap), R3 (a documented supported mode — `-ftrapv` / `-fsanitize=signed-integer-overflow` — makes it trap), R4 (signed overflow is UB by default), R6 (floating point), R7 (integral promotion / usual arithmetic conversions), R8 (`operator+` overload), R9 (pointer arithmetic on `char* + int`) | 7 | 2.8074 | R1 excluded: no built-in arbitrary-precision integer |
| Python | R1 (`int` is arbitrary precision), R3 (`TypeError` on incompatible operands; `MemoryError`), R6 (`float`), R8 (`__add__`, which also covers `str`/`list`/`tuple` concatenation and third-party wrapping types) | 4 | 2.0000 | R2/R4/R5/R7/R9 not realizable |
| Rust | R2 (release profile wraps, defined), R3 (debug profile panics — the same source, a different documented profile), R6, R8 (`impl Add`) | 4 | 2.0000 | R7 excluded: Rust applies no implicit numeric promotion, operand types must already agree. R4 excluded: never UB |
| Go | R2 (defined wrap), R6, R8-equivalent (`+` on `string` is a built-in allocating concatenation — a non-arithmetic behaviour class) | 3 | 1.5850 | R7 excluded (no implicit conversion between numeric types); R3, R4 not realizable for `+` |
| Zig | R1 (`comptime_int` operands are arbitrary precision), R3 (safe build modes panic on overflow), R4 (`ReleaseFast` makes overflow illegal behaviour), R6 | 4 | 2.0000 | R2 excluded: wrapping requires the distinct `+%` operator inside the span. R7, R8, R9 not realizable |

R5 is flagged (not added) for C++, Rust, Zig and Swift at this probe; the corresponding G-hop is
charged in Part 2 (§2.6).

#### 1.7.3 What the worked examples demonstrate

1. The procedure separates languages by *what their syntax cannot mean*, not by taste.
2. It does not reward terseness: Python wins ARG-PLAIN and loses ARITH's companion locality count.
3. It does not punish explicitness: Rust's ARG-PLAIN 4 and Zig's ARITH 4 both come from genuine,
   witnessed openness, and Zig's exclusion of R2 is a real determinacy gain that the checklist records.
4. It does not reward a language for lacking a capability: an unexpressible probe leaves the mean
   (`N/A-UNSUPPORTED`) and is charged in full against `C`.

#### 1.7.4 ARG-EXPLICIT enumerations

The five enumerations summarised in Rule 1.6.2 are the binding precedents for ARG-EXPLICIT; their
witness sets are constructed by the same method as §1.7.1 and stored under
`semantic-compression/probes/witnesses/`. Python, Java, Kotlin and TypeScript record
`N/A-UNSUPPORTED` at ARG-EXPLICIT with reason code `NA-NO-EXPLICIT-ALIAS-FORM`: the language provides
no explicit reference / borrow / address / writable-alias form at a call site. That probe is then
excluded from their determinacy mean **and** scores 0 for the corresponding capability point of
family 4 in Capability Coverage `C`, per §6.1.5 and §26.

### 1.8 Aggregation and normalization

**Raw aggregate (per language L):**

```
D_L = mean over applicable probes i of log2(B_i)
```

Lower is better. Applicable probes are those not marked `N/A-UNSUPPORTED` / `N/A-NOT-EXPRESSIBLE`
(§1.9).

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

where `D_best = min over applicable languages of D_L`, and

```
epsilon = 1/N = 1/40 = 0.025
```

**Rule 1.8.3 (epsilon is derived from measurement resolution, before the run).** `B_i` are integers
`≥ 1`, so the smallest nonzero per-probe contribution to `D_L` is `log2(2) = 1`, and the smallest
nonzero value `D_L` can take over `N = 40` probes is `1/40 = 0.025`. `epsilon` is set to exactly that
quantum. It is fixed now, before any language is measured, and is used for both metrics in this
document.

**Rule 1.8.4 (mandatory family-C reporting).** Publish beside the normalized score, for every
language: every raw `B_i`, `D_L`, and the ratio `D_L / D_best`. If the applicable raw `D_L` values
span a factor of 100 or more, additionally publish the §25.1 compression note stating that the ratios,
not the scores, carry the comparison between non-leading languages. Do not switch normalization
families to recover resolution.

**Rule 1.8.5 (weight).** Semantic Determinacy carries 0.25 of the Semantic Compression quality score
`Q` (§6.1.6). This document does not change that weight.

### 1.9 `N/A` policy for `B_i`

| Code | Condition | Effect on `D_L` | Effect on `C` |
|---|---|---|---|
| `NA-UNSUPPORTED` | The frozen support rubric records that the language cannot express the probe's capability at all | probe excluded from the mean | the capability point scores 0 and stays in `C`'s denominator |
| `NA-NO-EXPLICIT-ALIAS-FORM` | ARG-EXPLICIT only: the language has no explicit reference/borrow/address/alias form | probe excluded from the mean | as above |
| `NA-PARTIAL-WORKAROUND` | Expressible only through a workaround the rubric scores as partial | **not** excluded — the workaround's own focus span is enumerated normally | partial credit per rubric |
| `NA-TOOLCHAIN` | The frozen toolchain cannot build or run the probe's witnesses at all | probe excluded; reason and failing command recorded | recorded, rubric decides |

`N/A` is never converted to zero, never converted to `B = 1`, and never used to remove an inconvenient
probe (§26, §32). Every `N/A` carries the reason code and a free-text justification in the raw record.
A language with many `N/A-UNSUPPORTED` probes cannot buy a good determinacy score with them, because
`C` falls and the Semantic Compression Overall Score is the harmonic mean `2QC/(Q+C)` (§6.1.6).

### 1.10 Raw record schema (determinacy)

One JSON file per language at
`semantic-compression/raw/determinacy_<language>.json`:

```json
{
  "language": "rust",
  "toolchain": "rustc 1.95.0",
  "methodology_doc": "methodology/03_determinacy_and_locality.md",
  "probes": [
    {
      "probe_id": "SC-F05-P1",
      "probe_role": "ARG-PLAIN",
      "focus_span": "f(x)",
      "in_scope_axes": ["A1", "A2"],
      "outcome_table": "ARG",
      "branches": [
        {
          "class_id": "P1.M0",
          "description": "Copy type passed by value; callee cannot affect caller state",
          "witness_path": "semantic-compression/probes/witnesses/SC-F05-P1/rust/P1.M0/",
          "observer": "prints x.v unchanged; compiles with rustc -O",
          "spec_citation": "Rust Reference, 'Copy' trait; call expressions"
        }
      ],
      "excluded_classes": [
        {"class_id": "P2", "reason": "no value-producing implicit conversion in argument position; coercions are P1 per AR-004"}
      ],
      "B": 4,
      "log2_B": 2.0,
      "analyst_a": "...", "analyst_b": "...", "reconciled": true,
      "na": null
    }
  ],
  "D_raw_mean_log2": 2.0,
  "applicable_probe_count": 40,
  "na_probes": []
}
```

Every field is mandatory. A branch without `witness_path` is invalid and the file fails validation.

---

## 2. PART 2 — SEMANTIC LOCALITY (`H_i`)

> §6.1.4.C: "Measure how much non-local context must be inspected to determine the semantic facts of
> each probe … A lookup that is optional for extra detail must not be counted; count only context
> required to resolve a fact in the fixed inventory."

### 2.1 What is being resolved

For probe `i`, the frozen probe record carries a **fact set** `F_i`: the subset of the §6.1.3 fact
taxonomy the probe is annotated with. `F_i` is identical for all ten languages for that probe. Part 2
asks: **starting from the focus span, how many distinct external declaration sites or configuration
artifacts must be read to resolve every fact in `F_i`?**

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
* An overload set = **1** hop per distinct declaration site that must be examined. Candidates with the
  same name and a compatible arity, visible at the span, all count; candidates excluded by arity
  without being read do not.
* A type declaration and one of its members are the same declared entity if the member is written
  inside that declaration's own text = **1** hop; a member defined out-of-line elsewhere is a second
  hop.
* A trait / interface / protocol **implementation** is a distinct entity from the trait / interface /
  protocol **declaration**: reading both = **2** hops.
* An import/`use`/`using`/`#include` statement is a declared entity outside the focus span. If the
  analyst must read it to know which module a name comes from, that is **1** hop; the imported
  definition is another. A wildcard import that forces scanning `k` modules to find the name costs
  `k` hops.
* Standard-library declarations are hops on the same terms as user declarations.

**Rule 2.2.3 (zero-cost knowledge).** The following are **not** hops:

* facts fixed by the language specification for the tokens in the span (Go's pass-by-value rule,
  Java's defined `int` wraparound, Python's arbitrary-precision `int`, Rust's "no implicit numeric
  promotion", Zig's immutable parameters);
* facts written inside the focus span itself;
* the probe's own source file considered as a file (only its *declarations*, individually, are hops).

**Rule 2.2.4 (transitive chains).** If resolving a fact requires a signature, and then the type named
in that signature, and then that type's protocol conformance, that is 3 hops. Chains count their
length. `H_i` is the size of the **smallest** set of declared entities that resolves all of `F_i`
(Rule 2.3.2).

**Rule 2.2.5 (`H_i` may be 0).** A probe whose fact set is fully resolved by the focus span plus the
language specification has `H_i = 0`. This is a legitimate value and is why aggregation uses the
shifted family-C form (§2.9).

### 2.3 Counting algorithm (mechanical)

```
INPUT : probe i, language L, the probe's frozen program, its marked focus span,
        its fact set F_i, the language's authoritative specification,
        the frozen build recipe for L.
OUTPUT: H_i (integer >= 0), the hop list, the unresolved list, flags.

1.  U := F_i                       # unresolved facts
    S := {}                        # set of distinct entities read
    G := {}                        # set of distinct configuration artifacts read
2.  Resolve from the focus span alone, plus the language specification (Rule 2.2.3).
    Remove every fact so resolved from U. Record, per fact, "resolved locally".
3.  while U is non-empty:
4.      Let CAND be the set of entities/artifacts not yet in S∪G, each of which,
        if read, would resolve at least one fact in U.
5.      if CAND is empty:
6.          mark every fact remaining in U as UNRESOLVABLE (Rule 2.7) and break.
7.      Choose the entity e in CAND that resolves the largest number of facts in U.
        Tie-break, in order: (a) the entity reachable in the fewest traversals from
        the focus span; (b) the entity declared in the same file as the span;
        (c) the entity whose fully-qualified name sorts first (byte order).
8.      Add e to S (or to G if it is a configuration artifact).
9.      Remove from U every fact that e resolves at the class level (§2.1, §2.4).
10. H_i := |S| + |G|
11. if UNRESOLVABLE facts remain: H_i := SAT  (Rule 2.7), flag "whole-program".
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

**Counted (positive examples):**

| # | Situation | Why required |
|---|---|---|
| P-1 | C++ `f(x)`, fact "value versus storage": read the declaration of `f` | Without the signature, the span is compatible with copy, converting copy, alias and move; the fact has no class |
| P-2 | C++ `f(x)` with signature `void f(const S&)`, fact "writable aliasing": read the declaration of `S` | `const S&` does **not** settle the fact: a `mutable` member or a pointer member leaves mutation reachable. The type declaration is required to choose between M0 and M2 |
| P-3 | Python `f(x)`, fact "mutability": read the body of `f` | Python signatures carry no mutability information, and annotations are neither required nor enforced. There is no signature-level fact, so the body is the *only* resolving entity — required, not optional |
| P-4 | Rust `f(x)`, fact "allocation/copying/moving": read the `impl`/`derive` that decides whether the operand type is `Copy` | Move versus copy is not decidable from the span or from the signature alone |
| P-5 | Go `a + b`, fact "type and representation" where the operands are `int`: read the target architecture from the build configuration | `int` is 32 or 64 bits depending on the platform; the source does not say. This is a **G-hop** (§2.6) |
| P-6 | Java `x.m()`, fact "alternative value cases / dispatch": read the declared type of `x` and the interface declaring `m` | Which method set is in play is not on the span |
| P-7 | Kotlin `f(x)` where `f` may be `suspend`, fact "control-flow effect": read the declaration of `f` | A suspend call is spelled identically to an ordinary call; the control-flow fact has no class without the signature |

**Not counted (negative examples):**

| # | Situation | Why optional |
|---|---|---|
| N-1 | Reading the **body** of a C++ callee already known from its signature to take `const S&`, where `S` is a type with no `mutable` and no pointer members, to see whether it "really" mutates | The class (M0) is already settled by the signature plus the type declaration; the body adds detail only |
| N-2 | Reading a Rust callee's body after its signature says `&mut T`, to learn *what* it writes | The fact "writable aliasing" is already M1 |
| N-3 | Reading a type's documentation for complexity, thread-safety, or performance characteristics | Not a §6.1.3 fact |
| N-4 | Reading the definitions of *other* overload candidates after the winning overload is determined by the argument's declared type | Only entities that must be examined to determine the winner count |
| N-5 | Reading a Go interface's implementations to learn which concrete type will be passed | The fact "type and representation" for the *probe's* operand is resolved by the operand's declaration; the callee's dynamic type is a property of the caller's data, not of the span |
| N-6 | Reading a Java superclass chain after the declared type already resolves the probe's annotated facts | Confirmation only |
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
| `OVL` | one declaration site contributing an overload candidate |
| `IMPL` | trait / interface / protocol implementation or conformance |
| `IFACE` | trait / interface / protocol *declaration* |
| `IMPORT` | import / `use` / `using` / `#include` statement |
| `BODY` | a callee body, counted only where no signature-level fact exists (P-3) |
| `G` | global configuration, build flag, language edition/version, or whole-program mode (§2.6) |

### 2.6 Whole-program and compiler-mode dependencies (G-hops)

**Rule 2.6.1.** A fact that is decided by a build flag, compiler mode, language edition/version,
target architecture, or other whole-program configuration is resolved by reading a **configuration
artifact**. That is one hop of kind `G`, counted once per distinct artifact per probe.

**Rule 2.6.2 (the artifact counts even though the recipe is frozen).** The benchmark's frozen build
recipes fix these flags, but the *source does not say so*. The fact is genuinely absent from the
program text, and a reader must leave the source to obtain it. It is therefore counted, identically
for every language. Known instances, none of which is specific to any language's advantage:

| Language | Example fact | Artifact | Kind |
|---|---|---|---|
| Rust | integer overflow panics or wraps | the `overflow-checks` / profile setting implied by `rustc -O` | `G` |
| C++ | signed overflow is UB, wraps, or traps | the flag set (`-fwrapv`, `-ftrapv`, `-fsanitize=…`) in the build command | `G` |
| Zig | overflow panics or is illegal behaviour | the build mode (`-OReleaseFast` vs a safe mode) | `G` |
| Swift | overflow traps or is unchecked | `-O` versus `-Ounchecked` | `G` |
| Go | width of `int`/`uint` | `GOARCH` | `G` |
| Java | — | (defined by the JLS; zero-cost, Rule 2.2.3) | — |
| Python | — | (defined by the language reference; zero-cost) | — |

**Rule 2.6.3 (separate disclosure).** `|G|` is published per language per probe and as a per-language
total alongside `H`. It is **included** in `H_i` — it is required context — and is **also** reported
separately so that a reader can see how much of a language's locality cost is configuration rather
than declaration.

**Rule 2.6.4 (no double counting with Part 1).** A build-mode dependence contributes a `G` hop here
and an `R5` *flag* (not a `+1`) in Part 1 (§1.5.2). Where the modes produce genuinely different
behaviour classes, those classes are counted in Part 1 on their own merits, as behaviours, and the
`G` hop is counted here, as context. These are different metrics with different weights and the
specification requires both; this is not double counting, and the rule fixing which side gets the
`+1` is stated so that no analyst may take it twice on the same side.

### 2.7 Unresolvable facts and the saturation constant

**Rule 2.7.1 (`SAT = 6`).** If, at step 5 of the algorithm, no finite set of declared entities can
resolve a remaining fact — the required context is the whole program, or is only known at run time —
then the probe records `H_i = SAT = 6`, with the flag `whole-program` and the list of unresolvable
facts.

**Rule 2.7.2 (rationale, and why not 0).** Without this rule, a language whose facts are *unknowable*
would score **better** on locality than a language whose facts are merely *distant*, because the
analyst would stop looking and record a small number. That is a perverse incentive and would corrupt
the metric. `SAT` states the honest answer: the required context is not bounded by the probe's
declaration graph.

**Rule 2.7.3 (`SAT` is predeclared and justified before measurement).** Each frozen probe is a
self-contained program; the deepest resolving chain its declaration graph can produce is
probe → declaration → its type → that type's member/conformance → standard-library declaration, i.e.
5. `SAT = 6` is set to exactly one more than that, so "unbounded" always ranks strictly worse than any
bounded chain, and no bounded chain is ever clipped. `SAT` applies identically to all ten languages
and is fixed now.

**Rule 2.7.4 (chain cap).** No bounded chain is capped. If a measurement produces a bounded chain
longer than 5, the value is recorded as measured, the register (§3.3) is updated to note that the
probe corpus admits a deeper graph than anticipated, and `SAT` is **not** changed mid-run (changing it
would be a post-hoc transformation, forbidden by §24).

### 2.8 Worked examples

#### 2.8.1 ARG-PLAIN — `f(x)` with fact set `F = {value versus storage, mutability, aliasing / writable aliasing, allocation·copying·moving·destruction, type and representation}`

| Language | Hops (ordered, with kind) | `H` | Facts resolved locally at step 2 (zero cost) |
|---|---|---:|---|
| **C++** | 1 `SIG` decl of `f` → 2 `VAR` decl of `x` → 3 `TYPE` decl of `S` | **3** | none — the span settles nothing; `const S&` still needs `S` to decide M0 vs M2 (P-2) |
| **Rust** | 1 `VAR` decl of `x` → 2 `SIG` decl of `f` → 3 `IMPL` the `Copy`/`derive` site for the operand type | **3** | "aliasing / writable aliasing": the language guarantees `f(x)` cannot expose `x`'s own storage without `&`/`&mut` in the span — M1 is excluded at zero cost |
| **Python** | 1 `VAR` the binding of `x` → 2 `TYPE` the class of the bound object → 3 `BODY` the body of `f` | **3** | "value versus storage" and "allocation/copying": the language reference fixes that no copy occurs at argument binding — resolved at zero cost. The `BODY` hop is *required* (P-3): no Python signature carries mutability |
| **Go** | 1 `VAR` decl of `x` → 2 `SIG` decl of `f` (is the parameter an interface? boxing/representation change) | **2** | "value versus storage" and "writable aliasing of `x` itself": pass-by-value and the absence of implicit address-taking are specification facts |
| **Java** | 1 `VAR` decl of `x` → 2 `SIG` decl of `f` | **2** | "value versus storage" (references are copied, always) and M1 (impossible) are specification facts |
| **Swift** | 1 `SIG` decl of `f` (`borrowing` / `consuming` / default) → 2 `TYPE` decl of the operand type (value type vs class; CoW) | **2** | M1 excluded at zero cost (`inout` requires `&` in the span) |
| **Zig** | 1 `VAR` decl of `x` → 2 `SIG` decl of `f` (which documented coercion applies) | **2** | "mutability of the parameter": parameters are immutable — a specification fact; M1 excluded |

Note the intended asymmetry between the two metrics, which validates measuring both: Python's `B = 3`
(best of the nine) sits beside `H = 3` with a mandatory `BODY` hop, while Go's `B = 4` sits beside
`H = 2`. Neither metric predicts the other.

#### 2.8.2 ARITH — `a + b` with fact set `F = {type and representation, conversion behavior, overflow / exceptional numeric behavior, possible failure}`

| Language | Hops | `H` | of which `G` | Comment |
|---|---|---:|---:|---|
| **C++** | `VAR` a, `VAR` b, `G` build flags | **3** | 1 | promotions follow from the two declared types (specification); overflow behaviour needs the flag set |
| **Rust** | `VAR` a, `VAR` b, `G` overflow-checks profile | **3** | 1 | operand types must already agree, so no conversion lookup |
| **Go** | `VAR` a, `VAR` b, `G` `GOARCH` | **3** | 1 | wraparound is a specification fact (0 hops); `int` width is not |
| **Zig** | `VAR` a, `VAR` b, `G` build mode | **3** | 1 | coercion rules are specification facts once both types are known |
| **Java** | `VAR` a, `VAR` b | **2** | 0 | promotion, wraparound and widths are all fixed by the JLS |
| **Python** | `VAR` binding of a, `VAR` binding of b, (+ `TYPE` the class's `__add__` when either operand is not a built-in numeric type) | **2–3** | 0 | no overflow and no build-mode dependence to resolve |

#### 2.8.3 A negative-example walkthrough (why the count stops)

Rust, span `f(x)`, after hop 2 the signature reads `fn f(s: &mut Vec<i32>)`. The fact
"aliasing / writable aliasing" now has a class (the callee may mutate the referent: M2). An analyst
who then opens `f`'s body to see *which* elements it writes has made an **optional** lookup (N-2) and
must not count it. An analyst who opens the `Copy` question for `Vec<i32>` when the signature is a
reference has also made an optional lookup: with a reference parameter, no move can occur, so the
`IMPL` hop that was required in §2.8.1 is *not* required here. The hop list is probe-specific and is
recomputed per probe, never copied between probes.

### 2.9 Aggregation and normalization

**Raw aggregate (per language L):**

```
H_L = mean over applicable probes i of H_i        # mean required lookups per probe
```

Lower is better. **No logarithm** is applied — §1.8.1's exception is exclusive to Semantic Determinacy.

**Rule 2.9.1 (normalization — family C, shifted form).** `H_L = 0` is legitimate in principle, so
§25.1.C's predeclared shifted form applies:

```
Score_L = 100 * (H_best + epsilon) / (H_L + epsilon),    epsilon = 1/N = 0.025
```

with `H_best` the smallest applicable raw mean among the ten languages. `epsilon` is the same
predeclared constant as §1.8.3, derived from the same measurement resolution (`H_i` are integers;
`1/40` is the smallest nonzero increment of the mean over 40 probes).

**Rule 2.9.2 (mandatory reporting).** Publish per language: every `H_i` with its ordered hop list and
kinds, `H_L`, `|G|` totals, the count of `whole-program`-flagged probes, and the ratio `H_L / H_best`.
If the applicable `H_L` span a factor of 100 or more, publish the §25.1 compression note as well.

**Rule 2.9.3 (weight).** Semantic Locality carries 0.20 of `Q` (§6.1.6). Unchanged by this document.

### 2.10 `N/A` policy for `H_i`

Identical in structure to §1.9, with the same reason codes. In addition:

* `NA-UNSUPPORTED` probes are excluded from `H_L` and charged against `C`.
* A probe whose facts are unresolvable is **not** `N/A`; it is `H_i = SAT` with the `whole-program`
  flag (Rule 2.7.1). Marking it `N/A` instead is forbidden, because that would let non-locality
  escape scoring (§26).
* `N/A` is never converted to 0 and never converted to `SAT` automatically.

### 2.11 Raw record schema (locality)

One JSON file per language at `semantic-compression/raw/locality_<language>.json`:

```json
{
  "language": "go",
  "toolchain": "go 1.26.3",
  "methodology_doc": "methodology/03_determinacy_and_locality.md",
  "probes": [
    {
      "probe_id": "SC-F05-P1",
      "probe_role": "ARG-PLAIN",
      "fact_set": ["value versus storage", "mutability", "aliasing / writable aliasing",
                   "allocation, copying, moving, borrowing, or destruction",
                   "type and representation"],
      "resolved_locally": [
        {"fact": "value versus storage", "basis": "Go spec: arguments are passed by value"},
        {"fact": "aliasing / writable aliasing", "basis": "Go spec: no implicit address-taking; & required in span"}
      ],
      "hops": [
        {"n": 1, "kind": "VAR", "entity": "main.x", "site": "probe.go:7", "facts_resolved": ["type and representation"]},
        {"n": 2, "kind": "SIG", "entity": "main.f", "site": "probe.go:3", "facts_resolved": ["mutability", "allocation, copying, moving, borrowing, or destruction"]}
      ],
      "rejected_lookups": [
        {"entity": "main.f body", "reason": "optional per N-1: class already settled by the signature"}
      ],
      "H": 2, "G_hops": 0, "flags": [],
      "analyst_a": "...", "analyst_b": "...", "reconciled": true,
      "na": null
    }
  ],
  "H_raw_mean": 2.0,
  "applicable_probe_count": 40,
  "G_hop_total": 0,
  "whole_program_flagged_probes": []
}
```

---

## 3. Process controls

### 3.1 Order of operations (binding)

1. This document is frozen (now), before any `B_i` or `H_i` is produced.
2. The probe corpus, fact annotations and support rubric are frozen.
3. Witnesses are built and run under the frozen recipes.
4. Counts are produced language by language, in the fixed column order, with the running aggregate
   hidden from the analysts until all ten are complete.
5. Aggregation and normalization are executed by script from the raw JSON. No hand-entered score.

### 3.2 Two-analyst reconciliation

Each probe × language is counted independently by two analysts. Both records are preserved. On
disagreement:

1. Compare witness sets. A branch without a witness is dropped (Rule 1.3.1). A hop that step 9 cannot
   justify is dropped (Rule 2.4.1).
2. If both analysts hold witnesses for classes the other omitted, the union is taken — provided every
   member survives the observational-equivalence merge (Rule 1.4.5).
3. If the disagreement is about a *rule*, not a fact, it goes to the adjudication register (§3.3).
4. The reconciled value, both original values, and the reason are all recorded.
5. Any `B_i > 16` or `H_i = SAT` additionally requires both analysts' explicit sign-off.

### 3.3 Adjudication register

New edge cases are resolved once, recorded in
`semantic-compression/adjudication_register.json` with a timestamp and a rationale, and applied
**retroactively to all ten languages** — including re-counting already-counted languages. A ruling may
never be written in a way that names a language in its condition. Seed entries, fixed now:

| Id | Ruling |
|---|---|
| `AR-001` | A conforming copy constructor / copy hook that mutates its source makes P3 realizable. Exotic but specification-conforming completions count; UB-dependent ones do not (Rule 1.3.1b). |
| `AR-002` | Deferred *argument* evaluation (`@autoclosure`, by-name parameters) is not material when the probe's argument is a plain variable reference, because the deferral is unobservable. It is material where the probe's argument is an expression with effects. |
| `AR-003` | A call that begins executing the callee and may suspend (Kotlin `suspend`, an `async` function whose body runs to the first suspension point) is **not** P5. P5 requires that the body does not begin executing at the span. |
| `AR-004` | Coercions that retarget a reference/view type without constructing a new owned object (Rust deref/unsize coercion) are P1, not P2. Conversions that construct a value of a different type (boxing, existential wrapping, varargs/rest packing, optional promotion, widening) are P2. |
| `AR-005` | Varargs / rest-parameter packing at a call site is a P2 converting copy in every language that has it, because a new aggregate is constructed at the span. |
| `AR-006` | Subtype widening with no representation change (passing a subclass where a superclass is expected, in a language where that is a reference conversion only) is **not** P2. |
| `AR-007` | A `G` hop is charged once per distinct configuration artifact per probe, even when several facts depend on the same artifact. |

### 3.4 Prohibited operations on these two metrics

Restating §24, §25.1 and §32 in metric-specific form. Do not:

* change any axis, outcome class, ruling, `epsilon`, or `SAT` after any language has been counted;
* apply a logarithm to Semantic Locality, or to Semantic Determinacy anywhere other than the
  pre-registered `mean(log2(B_i))` aggregate;
* switch normalization families to decompress family C's hyperbola;
* winsorize, clip, min-max, or percentile-scale either aggregate;
* drop a probe, a branch, or a hop because of the ranking it produces;
* record a branch without a witness, or a hop without a resolved fact;
* convert `N/A` to 0, to `B = 1`, or to `H = 0`;
* count the same language property twice inside Semantic Determinacy (Rule 1.4.2) or twice inside
  Semantic Locality (Rule 2.2.2);
* reuse a hop list or a branch enumeration between probes without recomputing it (§2.8.3).

### 3.5 Deliverable checklist for the measuring agent

* [ ] `semantic-compression/raw/determinacy_<lang>.json` for all 10 languages, schema §1.10
* [ ] `semantic-compression/raw/locality_<lang>.json` for all 10 languages, schema §2.11
* [ ] `semantic-compression/probes/witnesses/<probe>/<lang>/<class>/` for every counted branch
* [ ] `semantic-compression/adjudication_register.json`, seeded with `AR-001`…`AR-007`
* [ ] `semantic-compression/scores/determinacy.json` and `locality.json`, produced by script from the
      raw files, containing raw aggregates, ratios to best, normalized family-C scores, `epsilon`, and
      the §25.1 compression note where the span requires it
* [ ] every `N/A` carrying a reason code and a justification
* [ ] both analysts' pre-reconciliation records preserved

---

**End of frozen document 03.**
