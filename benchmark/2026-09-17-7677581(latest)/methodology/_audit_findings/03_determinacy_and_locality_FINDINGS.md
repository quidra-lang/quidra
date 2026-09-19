# Audit findings for `03_determinacy_and_locality.md`

**Verdict:** needs_fix  
**Quidra bias found:** True  

## [BLOCKER] Finding 1
*Spec section:* doc 03 §1.1.1–§1.1.2 vs 01_capability_universe_and_probes.json authoring_rules R5

**Issue**

The entire document is built on a 'focus span' that the frozen probe corpus does not contain. §1.1.1 asserts 'Every probe record in the frozen corpus carries, per language, a complete compilable program and a marked focus span.' Grepping 01_capability_universe_and_probes.json for 'focus_span'/'focus span' returns 0 hits; each probe carries only {probe_id, family, title, intent, canonical_task, measured_fragment, capability_points, semantic_facts_expected}. The corpus's own R5_measured_fragment_boundary states 'Only the text inside that boundary is counted for Semantic Density (tokens and facts), Determinacy, Locality, and Hidden Cost.' For F05.P1 that boundary is '(1)+(2)+(3)' — it INCLUDES the local binding of x with its type. Doc 03 Rule 1.1.2 declares variable declarations external and forbids consulting them. The two frozen documents therefore specify different scoring units for the same metric, and the difference is not cosmetic: if the operand's declaration is inside the unit, most P/M classes close and every B_i in §1.7 collapses. Doc 03 never once mentions measured_fragment and never cites a real probe id.

**Required fix**

Delete the invented 'focus span' concept or bind it explicitly. Replace Rule 1.1.1 with: 'The focus span for probe i is exactly the probe's measured_fragment as defined in 01_capability_universe_and_probes.json, with the sub-expression named in the probe's SCORING NOTE (where one exists) marked as the counting site.' Then restate Rule 1.1.2 as 'everything outside the measured_fragment is external', and re-derive every §1.7 enumeration against the actual fragments. If a narrower unit than measured_fragment is genuinely wanted for B, that requires amending doc 01 and a dual publication under spec §25.4, not a silent redefinition in doc 03.

---

## [BLOCKER] Finding 2
*Spec section:* doc 03 §2.8.1, §2.8.2 vs corpus semantic_facts_expected

**Issue**

The binding locality worked examples use fact sets that contradict the frozen corpus. §2.8.1 states F for ARG-PLAIN is {value versus storage, mutability, aliasing/writable aliasing, allocation·copying·moving·destruction, type and representation}. The corpus's F05.P1 semantic_facts_expected is ['aliasing_writable_aliasing','mutability','allocation_copy_move_borrow_destroy','value_vs_storage','control_flow_effect','externally_visible_side_effects'] — doc 03 invents type_and_representation and drops control_flow_effect and externally_visible_side_effects. Both dropped facts are load-bearing: doc 03's own positive example P-7 charges a SIG hop for control_flow_effect at a bare Kotlin call, and externally_visible_side_effects is not resolvable at class level for a bare f(x) in ANY of the ten languages without a BODY hop or a whole-program flag. Restoring them changes every H in the table (Go 2→3+, Java 2→3+, Swift 2→3+). §2.8.2 has the same defect: it lists F = {type and representation, conversion behavior, overflow/exceptional numeric, possible failure} while F07.P1 is ['type_and_representation','overflow_exceptional_numeric','value_vs_storage','conversion_behavior'].

**Required fix**

Regenerate §2.8.1 and §2.8.2 from the corpus JSON rather than from prose. Add a binding rule: 'F_i is read programmatically from 01_capability_universe_and_probes.json[probes][i].semantic_facts_expected. No worked example may state a fact set by hand.' Then re-run the hop algorithm for all ten languages with control_flow_effect and externally_visible_side_effects included, and state explicitly whether externally_visible_side_effects at a bare call is a required BODY hop (P-3 logic) or a whole-program SAT for each language — that single ruling moves every ARG-PLAIN H value.

---

## [BLOCKER] Finding 3
*Spec section:* doc 03 §0.1, §1.5.2, §1.7.2, §2.8.2 vs corpus F07/F08

**Issue**

The ARITH probe the document scores does not exist. §0.1 defines ARITH as 'a binary a + b on two previously declared numeric variables' and §1.7.2/§2.8.2 enumerate 'a + b'. The frozen corpus has no such probe: F07.P1 is a*b+c (three operands, fused multiply-add), F07.P2 is quotient/remainder/float division (three statements), F08.P1 is m+1 at INT32_MAX, F08.P2 is guarded division. Every Table-ARITH 'binding precedent' (C++ 7, Python 4, Rust 4, Go 3, Zig 4) is therefore unreproducible, and the VAR hop counts in §2.8.2 are wrong by construction (F07.P1 has three declared operands, not two). Worse, F08.P1's canonical_task directs each language to use 'the operation whose overflow behavior is determined BY THE OPERATION ITSELF' where one exists — which flatly contradicts doc 03's ruling 'Zig R2 excluded: wrapping requires the distinct +% operator inside the span'. Under the corpus, +% IS the fragment for Zig at F08.P1.

**Required fix**

Re-anchor Part 1's second specialised table to the probes that exist. Rename Table ARITH to cover F07.P1 (expression-level arithmetic, three operands), and add separate rulings for F07.P2 (division/remainder — R3 and rounding/sign classes dominate) and F08.P1/F08.P2 (where the fragment may legitimately name an explicit discipline, so R2/R3/R5 are decided by the chosen operation, not by the plain operator). Re-derive all five worked B values and all §2.8.2 H values against the real fragments, and delete the Zig +% exclusion sentence, which the corpus overrides.

---

## [BLOCKER] Finding 4
*Spec section:* doc 03 §1.5.1 Table ARG vs corpus F05.P1 'SCORING NOTE, binding for metric B'

**Issue**

Table ARG structurally cannot count an outcome class the frozen corpus declares mandatory. F05.P1 says: 'the determinacy count for this probe is the number of materially distinct outcomes ... including at minimum {callee cannot observe caller's object at all; callee may read but not write it; callee may write elements observable through x; callee may replace/resize the sequence observable through x; callee may retain the alias beyond the call; a copy was made so no caller-visible change is possible}'. Doc 03's P×M grid has no class for 'callee may retain the alias beyond the call' (an escaping alias / lifetime outcome), and Rule 1.4.2 explicitly removes A8 (resource/lifetime) and A9 (control flow) from the in-scope axes for exactly this probe, which is where such a class would live. Two frozen documents thus declare two different, incompatible enumerations 'binding for metric B'. Note the direction: the missing class is one Rust and Quidra could exclude at zero cost (lifetimes; Quidra references are documented nocapture and non-storable), while Python, Java, TypeScript, Go, Swift and C++ would all realize it — so the omission costs the ownership-checked languages, Quidra included. It is a correctness defect regardless of direction.

**Required fix**

Add a third axis to Table ARG — Axis E (escape): E0 the callee cannot retain access beyond the call; E1 the callee may retain access beyond the call — and state its entailments (E1 unrealizable under P1 with a copied value; collapses under P3/P5/P6 like M). Alternatively amend Rule 1.4.2 to A1 × A2 × A8-escape. Either way, add a sentence resolving precedence: 'Where the frozen probe record states a minimum outcome set, every member of that set must map onto a class in the applicable table; a member with no mapping is a defect in this document, not in the probe.'

---

## [BLOCKER] Finding 5
*Spec section:* doc 03 §1.7.1, §1.6.2 vs corpus F05.P1/F05.P2 canonical_task

**Issue**

Every ARG-PLAIN witness in §1.7.1 uses an operand type the frozen probe forbids, and the ARG-EXPLICIT precedents are built on a program shape the corpus does not use. F05.P1 fixes the operand as 'a local x bound to a growable integer sequence containing the elements 1, 2, 3' (Vec<i32>/list/ArrayList/std::vector<int>/[]int). Doc 03's witnesses instead use struct S{int v;} and struct S{int*p;} (C++), x = 7 immutable (Python), #[derive(Clone,Copy)] struct S (Rust), int x / int[] x (Java). With the operand pinned to a growable integer sequence: Rust's P1·M0 copy witness is impossible (Vec is not Copy); C++ row 6 (P3 via a copy constructor that empties its source) is impossible for std::vector<int>, whose copy constructor is specified by the standard; Java's P2·M0 autoboxing/widening witness is impossible. So C++ 11, Rust 4, Java 4, Python 3 are all unreproducible. Separately, F05.P2's measured_fragment explicitly includes g's full declaration and body ('for this probe g's declaration and body ARE inside the boundary'), so §1.6.2's ARG-EXPLICIT numbers (Rust 3, Swift 4, C++ 6), which are enumerated over a bare call with an unknown callee, collapse toward 1 once the callee is inside the unit — and §1.6.2 is precisely the table the document uses to prove glyph-neutrality.

**Required fix**

Rebuild §1.7.1 and §1.6.2 from the actual frozen fragments: fix the operand to the growable integer sequence for ARG-PLAIN, and enumerate ARG-EXPLICIT over F05.P2's real boundary (caller local + call site + g's declaration and body). Publish the new numbers as the binding precedents, and store the witnesses before freezing, so that §1.7's 'must reproduce these numbers' is a claim about executed artifacts rather than about reasoning. Until the witnesses exist, mark §1.7 'illustrative, non-binding' — spec §4 forbids scores supported only by 'general language knowledge'.

---

## [MAJOR] Finding 6
*Spec section:* prompt.md §6.1.5, §32 / doc 03 §1.7.3 item 4

**Issue**

QUIDRA BIAS — the document's central anti-bias claim is false as written, and the gap benefits Quidra. §1.7.3(4) asserts 'It does not reward a language for lacking a capability: an unexpressible probe leaves the mean (N/A-UNSUPPORTED) and is charged in full against C.' That protection only covers absences that make a probe unexpressible. It does not cover absences that merely delete an outcome class from a probe the language CAN express. Concretely: Table ARITH's R2 (wrapping) is unrealizable for a language whose integer arithmetic is overflow-checked only; R8 is unrealizable for a language without user-redefinable operators; R7 for a language without implicit numeric promotion; P6 for a language without textual substitution. Each absence lowers B, raises the determinacy score, and is charged nowhere in C, because the capability universe has no coverage point for 'wrapping arithmetic', 'operator overloading' or 'preprocessor'. Cross-language constraint C-1 in 00_cross_language_constraints.md documents that Quidra sits on the favourable side of exactly this: 'in Quidra, state * 6364136223846793005 on a uint64 raises runtime error[INTEGER_OVERFLOW] rather than wrapping ... wraparound is not expressible.' §6.1.5's guard ('a tiny language cannot obtain a high primary score merely by having very few rules') is implemented solely through C and does not reach this channel.

**Required fix**

Replace §1.7.3(4) with an accurate statement and add an enforcement rule: 'Rule 1.9.x (absence flag). Where a class in a closed outcome table is excluded for a language because the language provides no such construct at all — rather than because the construct is spelled inside the span — the probe record carries flag LOW-B-BY-ABSENCE with the excluded class id and the missing construct named. The measuring agent publishes, per language, the count of LOW-B-BY-ABSENCE exclusions and a recomputed D_L with those classes counted as realizable, beside the primary D_L, so a reader can see how much of a determinacy advantage is bought by absence.' This is language-blind, costs nothing to compute, and it is the only way the reader can tell a determinacy result earned by design from one earned by omission.

---

## [MAJOR] Finding 7
*Spec section:* prompt.md §10.4 pre-flight item 3 (analogue), §6.1.7 item 6 / doc 03 §1.3.1, §1.1.4, §1.10

**Issue**

The document has a strong positive control and no negative control, and the asymmetry is exploitable by exactly one language. Counted branches need a witness, a build, an observer and a spec_citation (schema §1.10). EXCLUDED classes need only free-text 'reason' — the schema's excluded_classes entries carry no spec_citation and no executed evidence. Combined with Rule 1.1.4 ('facts fixed by the language's own specification ... are known, at zero cost, and close branches ... including for Quidra (from the Quidra language reference at the frozen HEAD SHA)'), a language whose reference document is authored by the same party running the benchmark can close branches by documentary assertion, while the nine incumbents' openness is demonstrated by construction. Spec §6.1.7 item 6 requires validating probes 'against the real language implementation or authoritative specification'; doc 03 requires implementation evidence only in the direction that raises B. The spec's own §10.4 pre-flight principle — 'Every validator can both pass and fail ... exercise a correct input that must pass and a corrupted input that must fail' — has no counterpart here.

**Required fix**

Amend Rule 1.3.1 and the §1.10 schema: 'Every entry in excluded_classes carries (a) a spec_citation to a numbered clause of the language's authoritative specification published before the run, AND (b) a refutation_witness_path: a program that attempts to realize the excluded class, together with the recorded compiler rejection or the observer output showing the class does not occur. An exclusion with neither is invalid and the file fails validation.' Apply it identically to all ten languages. This turns Rule 1.1.4's zero-cost specification knowledge into a checked claim rather than an assertion, in both directions.

---

## [MAJOR] Finding 8
*Spec section:* prompt.md §25.4, §24 / doc 03 §3.3 vs §3.4

**Issue**

The document contradicts itself on mid-run rule changes, and the contradiction sits on the one lever that could move results after Quidra is counted. §3.4 bullet 1: 'Do not change any axis, outcome class, ruling, epsilon, or SAT after any language has been counted.' §3.3 anticipates precisely that: 'New edge cases are resolved once, recorded ... and applied retroactively to all ten languages — including re-counting already-counted languages.' Per §3.1.4 counting runs 'in the fixed column order', and the fixed column order in §3 of the spec puts Quidra first — so every adjudication ruling written after counting begins is written by an analyst who has already seen Quidra's enumeration. The only stated safeguard, 'a ruling may never be written in a way that names a language in its condition', constrains wording, not tailoring.

**Required fix**

Reconcile the two sections and add the spec §25.4 dual-publication discipline to rulings: '§3.3 governs genuinely new cases; §3.4 bullet 1 governs revision of existing rulings and constants, which remains prohibited. Every AR ruling created after counting has begun must be (a) written and signed before its numeric effect is computed, (b) published with a before/after table of B_i and H_i for all ten languages at every probe it touches, and (c) listed in the final report. Rulings AR-001..AR-007 are exempt as pre-run seeds.' Also record, per ruling, which languages had already been counted when it was written.

---

## [MAJOR] Finding 9
*Spec section:* doc 03 §1.4.1 vs §1.4.2

**Issue**

Two rules assign different in-scope axis sets to the same probe and no precedence is stated. Rule 1.4.1: 'The in-scope axes for that probe are exactly the axes that the following fixed mapping associates with those facts.' F05.P1's frozen facts map, via that table, to A1, A2, A8 (allocation/copying/moving) and A9 (control-flow effect) plus A2/A7 (externally visible side effects). Rule 1.4.2 says the in-scope axes 'are therefore exactly A1 × A2' and that A6, A9 and A10 are not in scope — while saying nothing about A7 or A8, which the mapping also brings in. An analyst applying 1.4.1 and an analyst applying 1.4.2 produce different enumerations for the mandatory §6.1.4.B probe. This is not hypothetical: it is the same gap that drops the corpus's mandatory 'retain the alias beyond the call' class.

**Required fix**

Add an explicit precedence sentence to Rule 1.4.2: 'Where §1.5's specialised tables apply, their axis set governs and overrides the §1.4.1 mapping for that probe; the axes so excluded are listed exhaustively (here: A3, A4, A5 as separate axes — A1 subsumes conversion and move as outcome classes — A6, A7, A9, A10) together with the metric that charges each excluded fact instead (A9 and A7 at Semantic Locality, §2; A8 at Hidden Semantic Cost, doc 04).' Any excluded axis whose fact is in the probe's frozen fact set must name the metric that does charge it, or it must be brought back in scope.

---

## [MAJOR] Finding 10
*Spec section:* prompt.md §25.1.C, §26 / doc 03 §1.8, §1.9, §2.9

**Issue**

D_L and H_L are means over probe sets that differ per language, so the family-C comparison is across non-comparable raws, and the document never discloses it. Per doc 01 NA-6, NONE is 'the expected outcome for several languages on F02.P2, F04.P1, F04.P2, F05.P2, F14.P1, F17.P2, F19.P1, F19.P2, F20.P1 and F20.P2' — up to ten of forty probes. A language that cannot express the hardest probes has them removed from its determinacy and locality denominators, which generally lowers its mean and raises its normalized score; the offsetting charge lands only in C. The document asserts the harmonic mean makes this 'unprofitable' but never quantifies it, and D_best/H_best — which set the scale for all ten languages — may be produced by whichever language has the smallest and easiest applicable set. Relatedly, Rule 1.8.3's epsilon derivation ('the smallest nonzero value D_L can take over N = 40 probes is 1/40') is wrong for exactly these languages, whose mean is over fewer than 40 probes.

**Required fix**

Keep the primary aggregate as specified, and add a mandatory secondary: 'Rule 1.8.6 / 2.9.4 (common-core aggregate). Define CORE as the set of probes for which no language records an exclusion. Publish D_L^core and H_L^core over CORE, with |CORE| stated, beside the primary aggregates and their family-C scores, for every language. Where a language's primary rank and its CORE rank differ by more than one position, the final report states so explicitly.' Also correct Rule 1.8.3 to read 'epsilon is fixed at 1/N with N = 40, the frozen corpus size; it is a predeclared constant and is not recomputed per language even where a language's applicable probe count is smaller.'

---

## [MAJOR] Finding 11
*Spec section:* doc 03 §2.7.1–§2.7.4

**Issue**

SAT = 6 defeats the rationale Rule 2.7.2 gives for it. Rule 2.7.2: without SAT 'a language whose facts are unknowable would score better on locality than a language whose facts are merely distant'. Rule 2.7.3 sets SAT = 6 as one more than the deepest anticipated chain of 5. Rule 2.7.4 then concedes that a bounded chain longer than 5 may be measured, is recorded as measured, and SAT is not changed. In that case a probe whose context is genuinely unbounded scores 6 while a probe that is merely distant scores 7 or 8 — the exact perversion the rule claims to prevent, now written into the document. The languages most likely to hit true unresolvability (Python, TypeScript: rebindable names, monkey-patching, dynamic attributes) are the ones capped at 6; the languages most likely to produce deep bounded chains (C++ template/trait chains, Kotlin, Swift) pay the full length.

**Required fix**

Predeclare SAT as a rule rather than a literal, which is legal because it is registered before any counting and is language-blind: 'Rule 2.7.1. A probe with unresolvable facts records H_i = SAT, where SAT = 1 + max over all probes, all ten languages, of the largest bounded H_i measured in this run. SAT is computed by script after all counting is complete, applied uniformly to every SAT-flagged cell, and published with the maximum bounded chain that determined it. Its provisional value for planning is 6.' Keep Rule 2.7.4's no-clipping rule. This preserves 'unbounded always ranks strictly worse than bounded' under all measurement outcomes without any post-hoc choice.

---

## [MAJOR] Finding 12
*Spec section:* prompt.md §6.1.4.C / doc 03 §2.2.2

**Issue**

The overload-set and wildcard-import hop rules are not mechanical, and they are under-defined for exactly five of the ten languages. 'An overload set = 1 hop per distinct declaration site that must be examined. Candidates with the same name and a compatible arity, visible at the span, all count' leaves three undefined terms: 'compatible arity' (C++ default arguments and parameter packs, Swift default arguments, Kotlin @JvmOverloads, TypeScript optional parameters all make arity a range); 'visible at the span' (C++ argument-dependent lookup makes the candidate set depend on the argument types, which are themselves resolved by a hop — circular); and 'must be examined' (C++ and Swift overload resolution examines all candidates before ranking them). 'A wildcard import that forces scanning k modules to find the name costs k hops' does not say whether k is the number of wildcard-imported modules or the number scanned before the name is found — and the latter depends on an undefined scan order. Python, Go, Zig and Rust are barely affected; C++, Java, Kotlin, Swift and TypeScript carry the whole ambiguity.

**Required fix**

Make the candidate set a tool output: 'Rule 2.2.2.a. The overload candidate set for a span is the set the frozen toolchain itself reports, captured mechanically (clang -Xclang -ast-dump / clangd, javac -Xdiags:verbose on a deliberately ambiguous call, swiftc diagnostics, tsc --explainFiles plus the language service, kotlinc verbose resolution) and stored with the probe record. Hops = the number of distinct declaration sites in that captured set. No analyst constructs the set by reading.' And: 'Rule 2.2.2.b. k = the number of wildcard-imported modules that the language's name-resolution rules require to be examined to establish that the name is unambiguous — for every language with ambiguity-checked wildcard import, that is all of them, regardless of the order in which a human would look.'

---

## [MINOR] Finding 13
*Spec section:* doc 03 §1.2.2 vs Quidra architecture.md

**Issue**

QUIDRA BIAS (disclosure, not necessarily tilt) — Rule 1.2.2's unobservability carve-out is the single rule that decides Quidra's ARG-PLAIN count, and the document does not say so. Quidra's own architecture reference states: 'For direct calls, the IR may internally borrow an ordinary by-value aggregate parameter when a conservative whole-body analysis proves that the callee neither mutates that parameter nor exposes it through writable storage. This is an unobservable optimization of ordinary value passing.' Rule 1.2.2 renders exactly that class immaterial. The rule is language-neutral in wording and its stated purpose (stopping copy-elision lawyering for C++) is legitimate, and it also removes classes for Go, Java and Swift — but it is not disclosed that it disposes of the one documented source of openness at Quidra's bare call site, and no witness attempt is required before an 'unobservable' exclusion is accepted. To be explicit: I found no evidence this rule was written for Quidra, and Rule 1.4.2's exclusion of the escape class cuts the other way, against Quidra.

**Required fix**

Add to Rule 1.2.2: 'An exclusion on the ground of unobservability requires a refutation witness (see the amended Rule 1.3.1): an attempted observer program plus the recorded result showing the event is not detectable by any conforming program under the frozen toolchain. A vendor or specification statement that an optimization is unobservable is a citation, not evidence.' And add a cross-charge so the behaviour is not free: 'An event excluded here as unobservable is recorded with flag UNOBSERVABLE-RESOURCE-EVENT and is reported to methodology 04, where spec §6.1.4.D charges implicit allocation, copy, move, destruction or cleanup that is not signalled at the use site.'

---

## [MINOR] Finding 14
*Spec section:* doc 03 §1.10, §2.11, §1.4.1 vs corpus schema and doc 01 na_policy NA-5/NA-6

**Issue**

Three machine-level mismatches will make the validation script reject conformant records, or silently mis-join them. (1) Probe ids: doc 03's schemas use 'SC-F05-P1' and witness paths 'witnesses/SC-F05-P1/...'; the corpus uses 'F05.P1'. (2) Fact vocabulary: doc 03 writes facts in prose ('value versus storage', 'aliasing / writable aliasing', 'allocation, copying, moving, borrowing, or destruction') in Rule 1.4.1's mapping table and in the §2.11 fact_set/facts_resolved fields; the corpus's semantic_fact_kinds are the 13 snake_case ids (value_vs_storage, aliasing_writable_aliasing, allocation_copy_move_borrow_destroy, ...) and states they 'are the ONLY fact kinds usable anywhere in Primary Evaluation 1'. No join key exists. (3) N/A vs NONE: doc 03 §1.9 records NA-UNSUPPORTED and NA-NO-EXPLICIT-ALIAS-FORM, while doc 01 na_policy NA-6 says "'The language has no concept of X' is a NONE with a justification, never an N/A" and NA-5 requires every recorded N/A to name which rule permits it — doc 03's codes name none. Substantively doc 03 does the right thing (excluded from the mean, charged in full against C, never converted to 0 or to B=1), so this is an audit-trail and tooling defect, not a scoring defect. Separately, §1.7.4 says an ARG-EXPLICIT absence 'scores 0 for the corresponding capability point of family 4'; F05.P2 is in family 5 and is worth 3 capability points, not one.

**Required fix**

In §1.10 and §2.11, change probe_id and every witness path to the corpus form F05.P1; change every fact name to the 13 snake_case ids and rewrite Rule 1.4.1's left column with those ids; replace the NA-* codes with 'support_level: NONE' plus 'none_reason_code: NO_EXPLICIT_ALIAS_FORM | UNSUPPORTED | TOOLCHAIN' and add 'na_policy_rule: NA-2' to each record; and correct §1.7.4 to 'scores 0 for all 3 capability points of probe F05.P2 in family 5 (function calls and argument-passing semantics)'.

---

## [MINOR] Finding 15
*Spec section:* doc 03 §2.8.2, §3.1.4, §3.2

**Issue**

Three residual reproducibility gaps. (a) §2.8.2 gives Python's ARITH locality as 'H = 2–3' — a range inside a table the document calls a binding precedent; two analysts reading it get two different numbers, and the frozen probe fixes the operand declarations, so no range is warranted. (b) 'Two analysts' is never defined: the document does not say whether they are people or agent instances, what each is given, or how independence is enforced. Two instances of the same model given the same context are not independent, and §3.2's union rule makes B monotone in analyst effort, so the count depends on who looks harder. (c) §3.1.4 requires the running aggregate be 'hidden from the analysts', but §1.7 publishes nine languages' per-probe B values in the frozen document itself, including 'Python 3 ... the lowest ARG-PLAIN branching count among the nine languages'. The analyst who counts Quidra (first in the fixed column order) knows the number to beat.

**Required fix**

(a) State Python's ARITH H as a single value computed against F07.P1's actual operand declarations, with the conditional case resolved by the probe rather than left open. (b) Add to §3.2: 'An analyst is one measuring agent instance given only the frozen methodology documents, the frozen probe corpus, the language's authoritative specification and the frozen toolchain, with no access to the other analyst's records, to any previously counted language, or to §1.7's summary tables. Analyst A and analyst B run in separate sessions.' (c) Move §1.6.2 and the §1.7.1/§1.7.2 summary tables into a sealed calibration annex, and require that each language's enumeration be produced and signed before its analyst is shown that annex; or, at minimum, state that the annex is released to an analyst only after that analyst's own enumeration for the probe is recorded.

---
