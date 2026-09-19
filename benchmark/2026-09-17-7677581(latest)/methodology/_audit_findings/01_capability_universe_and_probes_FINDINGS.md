# Audit findings for `01_capability_universe_and_probes.json`

**Verdict:** needs_fix  
**Quidra bias found:** True  

## [BLOCKER] Finding 1
*Spec section:* 6.1.2 (family 5 "function calls and argument-passing semantics", family 15), 32 ("remove a capability ... because Quidra lacks it")

**Issue**

No probe in the 40-probe universe requires the fragment to construct or pass a function value / closure / callback. Quidra is the only one of the ten languages that cannot express one: docs/spec/language.md:684 states "Function and method declarations are callable names, not first-class values in the current language. A bare function or method name in value position is rejected with FUNCTION_NOT_VALUE." Three separate probes route around exactly that construct: F12.P2 supplies `h` as "an already-declared unary function" given context rather than asking the fragment to build one; F09.P2 says "with no user-supplied comparator if the language supplies a default order"; F16.P1 permits a plain loop instead of a fold/reduce. Family 5 has two probes and both spend their points on argument aliasing. The one capability the subject language lacks and the other nine have is therefore worth 0 of 103 points.

**Required fix**

Add a family-5 probe (3 capability points, matching F05.P2) whose canonical_task is: "(1) declare a local unary function/closure `neg` that returns the negation of its signed-32-bit-integer argument and that captures an enclosing local `k`; (2) pass `neg` to the language's standard higher-order sequence operation to transform `xs`; (3) produce element 0." Simultaneously delete the escape clause in F09.P2 ("with no user-supplied comparator if the language supplies a default order") or add a second F09 variant that mandates a descending comparator, and change F12.P2 so `h` is declared inside the measured fragment. Quidra scoring NONE on the new probe is the measurement, not a defect — this is the same logic hard_family_honesty_declaration already applies to concurrency and FFI.

---

## [BLOCKER] Finding 2
*Spec section:* 6.1.4.B, 6.1.4.D, 6.1.7 ("identical for all 10 languages"), 7

**Issue**

toolchain_binding.recipes puts Rust and Zig in their unchecked configuration and Quidra and Swift in their checked one, and probes F08.P1, F08.P2 and F11.P1 score precisely that difference. `rustc -O` turns debug-assertions off, so plain `+` wraps rather than panicking; `zig build-exe -OReleaseFast` makes signed overflow and out-of-bounds indexing undefined behavior. Quidra's only mode is optimized-and-checked (docs/spec/language.md:692: "optimized with Clang -O3 ... checked arithmetic, bounds checks ... are not disabled for speed"). F08.P1 then demands that "A reader must be able to state, from the fragment alone, exactly what happens at the overflow, without consulting the build recipe" — a question that is unanswerable for Rust and Zig only because of the recipe this document froze, and that Quidra answers for free.

**Required fix**

For Primary Evaluation 1, pin the safety posture rather than the optimization level, and match it to Quidra's: change the Rust recipe to `rustc -O -C debug-assertions=on FILE.rs -o BIN` and the Zig recipe to `zig build-exe -OReleaseSafe FILE.zig -femit-bin=BIN`, both of which are optimized-and-checked. Add a toolchain_binding clause: "Where a language exposes more than one arithmetic/bounds safety mode, the frozen Semantic Compression recipe selects the optimized checked mode, because that is the only mode the evaluated Quidra build provides; the selection is a controlled variable and is recorded per language. C++ has no checked mode and is recorded as such — that is a language fact and is scored by metrics B and D, not by the recipe."

---

## [BLOCKER] Finding 3
*Spec section:* 8 ("intentionally excluded from the Standard score to avoid double counting"), 6.1.4.B/C/D, 6.1.5

**Issue**

Determinacy is being counted inside Capability Coverage as well as inside metrics B and D. Two mechanisms do it. (a) support_rubric.FULL criterion F-5 requires every fact kind in semantic_facts_expected to be "DETERMINABLE"; for a dynamically typed language F05.P1's `aliasing_writable_aliasing` and `type_and_representation` are not determinable at all without whole-program analysis, and no bound on the permitted analysis is stated, so Python and TypeScript can be pushed to PARTIAL on many probes purely for being dynamic. (b) 17 of the 40 canonical_tasks end with an unnumbered clause of the form "A reader must be able to determine X" (F01.P2, F06.P1, F07.P2, F08.P1, F09.P1, F09.P2, F10.P1, F10.P2, F11.P1, F11.P2, F13.P1, F13.P2, F14.P2, F16.P2, F17.P1, F18.P1, F20.P1, plus F04.P2's variant wording), and nothing says whether those clauses are support requirements. If they are, hidden semantics costs a language twice — once in B/D and again in C, which then enters the harmonic mean — and the languages that pay twice are exactly the ones whose design differs most from the subject language's local-determinacy thesis.

**Required fix**

Delete criterion F-5 outright, and add to support_rubric a binding sentence: "Support level records only whether the canonical_task's stated behavior is delivered. Whether a fact is recoverable from the local surface form is measured exclusively by metrics B and D and NEVER affects FULL/PARTIAL/NONE; a fragment that delivers the behavior while leaving facts locally indeterminate is FULL and pays for the indeterminacy in B and D." Then mark every "A reader must be able to determine ..." clause in the 18 affected probes as non-normative by prefixing each with "OBSERVATION TARGET (not a support requirement; scored by metrics B/D only):".

---

## [BLOCKER] Finding 4
*Spec section:* 32 ("derive Semantic Compression probes from Quidra's current syntax or feature set"), 6.1.1, 6.1.7

**Issue**

provenance_and_fairness_statement.derivation asserts "No Quidra documentation, source, examples, README, or grammar was consulted. No probe was shaped by Quidra's operators, types, syntax, keywords, or feature set." That is contradicted by the probes themselves. F15.P1 asks for a function named `first`, generic over T, taking a sequence of T, returning the element at index 0 with declared result type T — Quidra's README generics example is `T first<T>(T[] values) / return values[0]`, and the produced fragment semantic-compression/probes/quidra/F15.P1.qui is that example verbatim. F05.P2 asks for a one-parameter function that takes writable access to a caller's 32-bit integer and whose body is exactly "sets that variable to 7", with the caller's local "initialized to 1" — Quidra's README call-authority example is `void initialize(int &value) / value = 7` with `int value = 1`. F04.P1 asks for `x` initialized to 1 plus a second name denoting the same storage, and F04.P2 for a non-copying read-only view — Quidra's README law-1 example is `int x = 1 / int &writer = &x / const int &view = &x`. F20.P1 picks C `abs`; the README FFI example is `extern int c_abs(int value) = "llabs"`. Four of the twenty families were subdivided into precisely the sub-question that Quidra's own design laws 1, 2 and 3 exist to answer, carrying 12 of 103 points.

**Required fix**

Replace the derivation paragraph with a truthful one and neutralize the affected probes. Concretely: rewrite F04.P1/F04.P2/F05.P2/F15.P1 identifiers and constants away from the README's (`x`/1/`writer`/`view`/7/`first`/`values[0]`) — this alone does not fix the tilt, so also pair each with a counterpart probe in the same family that a value-semantics-with-explicit-storage language cannot win: e.g. in family 4, a probe requiring two independently-created handles to the same heap object observed as identical (reference identity), which Quidra explicitly cannot express (README: "Quidra does not expose ... a general object-identity operator"). Then state the provenance honestly: "Probe subdivision within each spec-mandated family was authored with knowledge of the subject language; the following probes were therefore paired with counterpart probes chosen to be inexpressible in the subject language, and the pairing is listed here."

---

## [MAJOR] Finding 5
*Spec section:* 6.1.5, 6.1.4.E, 25 ("preserve ... aggregation method, weight"), 6.1.7

**Issue**

The 103-point weight vector is the single most load-bearing number in the document — it is the Capability Coverage denominator and metric E's denominator — and no derivation rule for it is given anywhere. Per-probe values range 1 to 3 and per-family totals 3 to 6 with no stated basis. It is not the count of numbered sub-requirements (F01.P1 has 2 sub-requirements and 2 points; F01.P2 has 2 and 3 points), and it is not the count of semantic_facts_expected (F02.P1 has 5 facts and 3 points; F02.P2 has 5 facts and 2 points). An independent analyst cannot reproduce the weights, and the low-weight families (3 mutation and 7 arithmetic, 3 points each) are the two where every one of the ten languages is equally capable.

**Required fix**

Add a `capability_point_assignment_rule` object stating the mechanical rule and showing it reproduces all 40 values, e.g. "points(probe) = number of distinct semantic commitments the canonical_task requires the fragment to deliver, counted as one per numbered sub-requirement that introduces a new commitment, capped at 3." If no rule reproduces the current numbers, replace the vector with a uniform one — every probe 2 points, denominator 80, two probes per family so every family weighs 4 — and record the change as an erratum. A uniform vector is defensible without a derivation; an irregular one is not.

---

## [MAJOR] Finding 6
*Spec section:* 6.1.4.A-D, 6.1.6, 6.1.7

**Issue**

support_rubric.interaction_with_quality_metrics computes metrics A-D over each language's FULL-or-PARTIAL probes only, so every language's Q is measured on a different probe basis, with no requirement to publish the basis size. The excluded NONE probes (concurrency, FFI, deterministic destruction) are exactly the token-heavy, lookup-heavy, hidden-cost-heavy ones, so dropping them raises Q — and the language with the most NONEs is the young one. Worse, PARTIAL fragments are kept in A-D: a language that can only approximate a probe writes a clumsy substitute that drags its density, branching, lookup and hidden-cost counts, while a language that cannot do it at all pays nothing in A-D. PARTIAL is therefore sometimes worse for Q than NONE. The anti_gaming_note asserts the harmonic mean makes this unprofitable but shows no derivation, and spec 6.1.4 nowhere authorizes a variable basis.

**Required fix**

Predeclare, before any language beyond Quidra and Python is measured: "Metrics A-D are computed primarily on the COMMON BASIS — the set of probes for which all ten languages have a fragment — so every language's Q is measured on identical material. The all-fragments basis is computed and published as a secondary figure, together with each language's fragment count n_L and the delta between the two bases. Capability Coverage C, computed on the full fixed 103-point denominator, remains the sole channel through which unsupported capabilities affect the score." Also state explicitly whether PARTIAL fragments enter the common basis, and apply the same answer to all ten languages.

---

## [MAJOR] Finding 7
*Spec section:* 6.1.7 ("freeze every probe"), 4 ("explicitly defined objective criteria"), 25

**Issue**

Authoring rule R2 ("the way a competent, current, professional user of that language would write it") and its restatements inside the canonical_tasks ("the normal choice", "the way a normal author would write it", "whichever a normal author would use" — in F03.P1, F07.P2, F09.P1, F10.P1, F10.P2, F11.P1, F16.P1, F16.P2 and others) are the dominant undefined judgement in the document, and every one of metrics A, B, C and D is computed from the text they select. F16.P1 openly permits either an explicit loop or a standard sum/fold; in Python that is `total = sum(xs)` versus a four-line loop, a token-count difference of roughly 5x on a probe that feeds Semantic Density directly. The `deterministic_tie_break` object arbitrates only the support LEVEL, not the fragment TEXT, so there is no tie-break at all for the choice that actually moves the numbers.

**Required fix**

Add authoring rule R10: "Where more than one form satisfies R1-R3, the fragment is the form with the fewest tokens under the frozen tokenizer of methodology 02. Ties are broken by the form that appears first in the language's official reference or standard-library documentation for that operation. Every alternative form considered must be recorded in the probe's evidence record together with its token count and the documentation citation, so a reviewer can verify the selection mechanically." Then remove the explicit either/or from F16.P1 and F16.P2 by naming the outcome R10 produces.

---

## [MAJOR] Finding 8
*Spec section:* 6.1.2 ("without benchmark-specific external packages"), 10.4 (infrastructure defect must not be scored as a language failure), 7

**Issue**

F18.P2 states that "a second file is permitted here and is NOT a third-party dependency", but toolchain_binding.rule makes the frozen recipe the sole arbiter of expressibility and two frozen recipes cannot build a second file. `clang++ -std=c++20 -O2 FILE.cpp -o BIN` compiles exactly one translation unit, and `go build -o BIN FILE.go` compiles only the named file — while Go's private-identifier rule (lowercase = package-scoped) requires a separate package, i.e. a separate directory and a module. C++ and Go would therefore be scored PARTIAL or NONE on a 3-point probe for a harness limitation, not a language limitation, which is the exact failure mode spec 10.4 forbids. The subject language's frozen recipe resolves `import util = "./util.qui"` from a single command, so it takes FULL (see semantic-compression/probes/quidra/F18.P2.qui and util.qui).

**Required fix**

Add a `multi_unit_recipes` block to toolchain_binding naming the exact augmented command permitted for F18.P2 per language, and declare it is not a P-b deviation: C++ `clang++ -std=c++20 -O2 util.cpp FILE.cpp -o BIN`; Go `go build -o BIN ./...` inside a directory containing `go.mod` and a `util/` package; Swift `swiftc -O util.swift FILE.swift -o BIN`; Java `javac -d OUT util/Util.java FILE.java`; Rust/TypeScript/Zig/Kotlin/Python/Quidra unchanged because their frozen recipes already follow module references. Add: "Any language forced to PARTIAL or NONE on F18.P2 solely by the single-file form of its recipe is an infrastructure defect under spec 10.4 and must be re-run under the multi-unit recipe before the result is reported."

---

## [MAJOR] Finding 9
*Spec section:* 6.1.2 ("Partial support must be scored by a predeclared rubric rather than guessed"), 6.1.7

**Issue**

support_rubric.deterministic_tie_break declares its question to be "the sole arbiter", and the question is "Does the fragment observably deliver EVERY explicitly numbered sub-requirement in the probe's canonical_task?". Eleven of the forty probes contain no numbered sub-requirements at all — F03.P1, F03.P2, F07.P1, F08.P2, F09.P1, F10.P1, F10.P2, F11.P1, F11.P2, F12.P2, F14.P2 — so for those the question is vacuously satisfied by anything that builds, and the sole arbiter arbitrates nothing. F11.P1 is the clearest case: a Python fragment `e = xs[i]` that raises IndexError delivers zero numbered sub-requirements because there are none, and the probe's actual demands live in unnumbered prose. Two reviewers will land on different levels for a 3-point probe.

**Required fix**

Renumber all eleven canonical_tasks so every behavioral demand carries its own (1)/(2)/(3), leaving the observation-target clauses out of the numbering per the F-5 fix above. For example F11.P1 becomes "(1) read the element of `xs` at index `i`; (2) bind it to a local `e`; (3) produce `e` as the fragment's result, written the way R10 selects." Record the renumbering as an erratum in CORRECTIONS.md; it changes no probe's intent.

---

## [MAJOR] Finding 10
*Spec section:* 7 ("normal, idiomatic, production-reasonable best practices"), R2 of this artifact, 6.1.7

**Issue**

The frozen TypeScript recipe is `tsc FILE.ts` with no tsconfig and no flags, so `strict` and `strictNullChecks` are off. In that mode `let o: number | undefined = undefined; let n: number = o;` compiles clean, which erases the entire point of F12.P1 and F12.P2 for TypeScript and understates its type-level optionality everywhere else. The document's own R2 requires the fragment to be written as a competent current professional would, and no production TypeScript project runs non-strict. The rubric then traps the language: enabling `--strict` is "a compiler flag beyond the frozen recipe" under P-b, which forces PARTIAL and a 0.5 factor. TypeScript is penalized either way, for a harness choice.

**Required fix**

Change the frozen TypeScript recipe in both this file and environment/environment.json to `tsc --strict --target es2022 --module nodenext FILE.ts`, and add: "`--strict` is part of the frozen TypeScript recipe, not a P-b deviation, because non-strict mode is not a production-reasonable configuration under R2 and spec 7." Record the change as an erratum; only Python and Quidra fragments exist so far, so nothing needs re-measuring.

---

## [MAJOR] Finding 11
*Spec section:* 6.1.4.B, 6.1.4.D, 25.4

**Issue**

F08.P1 instructs the author to use "the one that makes the behavior explicit at the site" when a language provides explicit overflow operations, but gives no preference order when several exist. Rust offers `checked_add`, `wrapping_add`, `saturating_add` and `overflowing_add`; Zig offers `+%`, `+|` and `@addWithOverflow`; Swift offers `&+` and the reporting `addingReportingOverflow`. The four choices differ in token count (metric A), in the number of surviving interpretations (metric B) and in whether a failure path appears (metric D), so the unstated choice moves three metrics. F08.P2 in the same family does supply an explicit (i)/(ii)/(iii) order, which shows the omission is an oversight rather than a policy.

**Required fix**

Give F08.P1 the same shape as F08.P2: "preferring, in this fixed order, (i) a standard checked operation yielding an optional/error value, with 0 substituted for the exceptional case; (ii) a standard wrapping operation; (iii) a standard saturating operation; (iv) the plain operator. Record which rung was used and why the higher rungs were unavailable."

---

## [MAJOR] Finding 12
*Spec section:* 6.1.2 (family 14), 6.1.7 ("A language must be allowed to score well for semantics Quidra does not have")

**Issue**

Both probes in family 14 reward closedness and nothing rewards extensibility. F14.P1 demands "the language's own rules prevent code elsewhere from adding a third alternative" and routes open class hierarchies to PARTIAL via P-a; F14.P2 forbids a default arm and scores exhaustiveness checking. Quidra's structural unions plus its exhaustive `match` (README law 5/6) take FULL on both. The dual capability — adding a new alternative from a separate compilation unit without editing the original, which is the whole point of open polymorphism in Java, Kotlin, Swift, C++, Python and TypeScript — is worth zero points anywhere in the universe, and Quidra explicitly cannot do it ("Inheritance is code and member reuse, not an implicit runtime subtype relation ... There is no implicit upcast ... or hidden dynamic dispatch"). The family is scored in one direction only.

**Required fix**

Add F14.P3 (3 points, raising the family to 9 and the denominator accordingly, or rebalance under the uniform-weight fix): "(1) declare an abstract operation `area` over shapes in unit `shapes`; (2) from a SEPARATE unit that does not modify `shapes`, introduce a third shape `Tri` implementing `area`; (3) call `area` through a value of the abstract type holding a `Tri` and produce the result. A language whose rules prevent extension from outside the declaring unit records NONE with the citation." This is the exact mirror of F14.P1 and restores the family's symmetry.

---

## [MAJOR] Finding 13
*Spec section:* 6.1.7 item 6 ("validate each probe against the real language implementation or authoritative specification"), 10.4 mandatory pre-flight validation

**Issue**

The document declares itself FROZEN while deferring probe validation to downstream_obligations item (6), and it carries no analogue of spec 10.4's validation triad. Specifically: the one mechanical validator the probe set defines — F20.P2's `nm` symbol check — is never required to be exercised on a known-positive and a known-negative case, so a validator that can never fail (or never pass) would silently produce NONE rows; there is no rule that an all-NONE column for one language or an all-NONE row for one probe be investigated as a suspected instrument defect before publication; and there is no requirement that each probe be shown expressible in at least one language before any language is scored NONE against it. Every NONE produced by this instrument is indistinguishable from a probe that is simply unwritable as specified.

**Required fix**

Add a `pre_measurement_validation` object requiring, before any language beyond the pilot is scored and preserved as evidence: "(V1) every FULL/PARTIAL fragment builds and runs under its frozen recipe and matches the canonical_task's stated result (already implied by R6 — make it an explicit gate); (V2) the F20.P2 `nm` validator is exercised once on a binary known to export `add2` and once on a binary known not to, and both outcomes recorded; every harness convention the canonical_task withholds — Java's `Main` class name, file naming, entry wrappers — is satisfied by the harness and never charged to the fragment; (V3) every probe is demonstrated expressible at FULL in at least one of the ten languages before any language is scored NONE on it; (V4) any probe with ten NONEs, or any language with NONE on more than a third of the probes, is investigated as a suspected instrument defect and the investigation recorded before the row is reported as a result."

---

## [MAJOR] Finding 14
*Spec section:* 6.1.2, 4 ("If a value cannot be measured honestly, do not fabricate it")

**Issue**

F19.P2's clause "If the language has concurrency but its semantics make this program race-free WITHOUT any explicit synchronization construct (e.g. a global interpreter lock, single-threaded event loop, or compiler-enforced exclusion), write the fragment that way" invites a factually wrong FULL for Python. A GIL does not make `counter += 1` atomic — it is a read-modify-write across bytecodes — and Python 3.14, the frozen version, additionally ships a free-threaded build. An analyst following the clause literally writes the unsynchronized version, observes 2000 on a lucky run, and credits Python 3 capability points for a program that has a data race.

**Required fix**

Tighten the clause to: "...write the fragment that way ONLY if the language's own documentation states a normative guarantee that the specific read-modify-write in (2) is atomic; quote the sentence and cite it in the evidence record. An interpreter-wide lock that serializes bytecodes without making read-modify-write atomic does NOT satisfy this, and the fragment must use an explicit synchronization construct. Verification requires the value 2000 on 20 consecutive runs, recorded."

---

## [MINOR] Finding 15
*Spec section:* 6.1.7, 10.4 ("Defects found during a run are fixed ... not silently corrected")

**Issue**

Two internal contradictions and one anchoring problem. (a) R9 says "If a language cannot express a probe, record NONE ... Do not substitute an easier task", but F02.P2, F12.P1, F12.P2, F14.P1, F15.P1, F15.P2 and F17.P2 each explicitly instruct substitution of a nearest standard construct. (b) `status` forbids any probe ever being "added, removed, reworded, or re-pointed", and no_post_hoc_redesign says a discovered defect is "recorded as an erratum in the results, not silently repaired here" — which makes it impossible to repair a defect that pre-measurement validation finds, contradicting spec 10.4's fix-and-record rule and contradicting this run's own CORRECTIONS.md, which does exactly that for methodology 02. (c) NA-6 pre-announces that NONE is "the expected outcome for several languages" on ten named probes, which is a predicted result embedded in the frozen instrument.

**Required fix**

(a) Amend R9 to "...except where a probe's canonical_task explicitly names a substitution; write the named substitute and score PARTIAL under P-a." (b) Replace the freeze wording with an amendment procedure: "Defects found by pre-measurement validation are corrected, the correction is given a number and a date in CORRECTIONS.md with the superseded text preserved verbatim, and any affected measurement is re-taken. Only changes made after results are observed, or made to improve any language's position, are prohibited." (c) Delete the ten-probe expectation list from NA-6; keep only the rule that "the language has no concept of X" is a NONE and never an N/A.

---

## [MINOR] Finding 16
*Spec section:* 6.1.4, 25, 26

**Issue**

Three loose ends that do not change a number today but will be argued about later. (a) F11.P1's `.checked` variant is "measured and reported separately" but no metric consumes it and nothing forbids it being cited selectively after results are seen. (b) F20.P2 makes `nm` evidence mandatory including for NONE, but Python and TypeScript produce no compiled artifact, so the required evidence cannot exist. (c) toolchain_binding.recipes duplicates environment/environment.json's frozen_toolchain_recipes verbatim with no checksum, so the two frozen copies can drift silently — and they already differ in shape (one Quidra entry here versus quidra_native/quidra_interpreter there). (d) downstream_obligations.freeze_order names only spec items (3), (5) and (6) as sibling documents and omits methodology 03 (metrics B and C) and 04 (metrics D and E), which exist and are load-bearing.

**Required fix**

(a) Add to F11.P1: "The `.checked` variant enters no metric and is published as evidence only; it may not be substituted for the primary fragment in any calculation." (b) Add to F20.P2: "For a language with no compiled artifact under its frozen recipe, the N-reason is N-1 and the nm field records 'no compiled artifact under the frozen recipe'; this is a NONE, not an N/A under NA-5." (c) Replace the inlined recipes with the environment.json pointer plus a SHA-256 of that file, or delete the inline copy. (d) Extend freeze_order to name 03 and 04 alongside 02.

---

## [MINOR] Finding 17
*Spec section:* 6.1.2, 6.1.7

**Issue**

Support rubric criterion F-6 disqualifies a mechanism the language's documentation marks as "unsafe, deprecated, discouraged, or reflective". "Discouraged" has no observable test, and the carve-out that follows only rescues facilities gated behind an `unsafe`/`unchecked` keyword. Python's `ctypes` (needed for F20.P1) carries documented crash warnings, Go's `unsafe` package, Java's reflective access paths and C++'s raw-`new` guidance all sit in the gap. Two reviewers will disagree about a 3-point probe on the basis of tone in a manual.

**Required fix**

Replace "unsafe, deprecated, discouraged, or reflective" with an observable test: "the language's official reference or standard-library documentation contains an explicit normative prohibition or deprecation for ordinary use — a sentence containing 'deprecated', 'should not be used', 'do not use', 'unstable', 'internal', 'no compatibility guarantee', or an equivalent normative form. The exact sentence and its URL must be quoted in the evidence record. Documented warnings about consequences of misuse ('can crash the interpreter') are not prohibitions and do not disqualify."

---
