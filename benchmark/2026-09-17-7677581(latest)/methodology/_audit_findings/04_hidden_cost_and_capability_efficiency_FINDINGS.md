# Audit findings for `04_hidden_cost_and_capability_efficiency.md`

**Verdict:** needs_fix  
**Quidra bias found:** True  

## [BLOCKER] Finding 1
*Spec section:* §6.1.4.D / §6.1.7 item 3; doc 04 §0.1, §1.2.1, H8a

**Issue**

QUIDRA BIAS — the only pre-declared execution-mode exemption in the document is Quidra's. §0.1 (lines 34-37) states: 'Where a recipe has both a native and an interpreted form (Quidra), semantic evidence is taken from the native compiled form ...; interpreter behavior is recorded as a note only and NEVER CHANGES A COUNT.' Meanwhile H8a counts as hidden any case where 'the outcome at the edge differs depending on build mode (debug/release), compiler or runtime flags, optimization level, target platform', and the mandatory illustration in §1.2.1 charges exactly that against a language 'whose arithmetic outcome at overflow differs between the language's debug and release build configurations' (i.e. Rust). The frozen recipes pin ONE mode for every other language (rustc -O, zig -OReleaseFast, clang++ -O2), so Rust and Zig are charged for a mode divergence that the evidence never even exercises, while Quidra — the only language in the frozen recipe table with two shipped execution engines (`quidra build`/`./BIN` and `quidra run`) — has any actual, measurable divergence between its two engines pre-declared out of scope. This is a rule whose effect is to improve Quidra's position on H8/H3/H9, which §32 and §25.4 prohibit.

**Required fix**

Replace the last sentence of §0.1 with a symmetric rule: 'Primary evidence is taken from the native compiled form. Where the frozen recipes provide more than one execution engine or build mode for a language, any materially different outcome between those modes is a configuration dependence and triggers the same checklist item it would trigger for any other language (H8a for numeric edges; H3 for dispatch; H9 for state/effects). The operator must run every probe under both Quidra recipes (`quidra build`/`./BIN` and `quidra run`), record both outputs as evidence records, and record any divergence as a triggered item.' Add a matching validation check V16: 'For every language with more than one frozen execution mode, both modes were executed for every supported probe and the per-probe divergence set is published.'

---

## [BLOCKER] Finding 2
*Spec section:* §6.1.7 items 1-2; doc 04 §0.3 lines 60-66, §1.6 line 537, §2.5 STEP 7, V10, V13

**Issue**

MECHANICAL REPRODUCIBILITY — every frozen input this document names is wrong, so the document cannot be executed and its own blocking validation check can never pass. Doc 04 declares probe identifiers `PR-01`…`PR-40`, inputs `semantic-compression/probes/capability_universe.json`, `semantic-compression/probes/probes.json`, `semantic-compression/raw/capability_matrix.json`, and implementations at `semantic-compression/probes/impl/<lang>/PR-NN.<ext>`. None of these exist. The actual frozen corpus is `methodology/01_capability_universe_and_probes.json` with probe ids `F01.P1`…`F20.P2`, and implementations at `semantic-compression/probes/<lang>/F01.P1.qui` etc. V13 makes this fatal: it blocks publication unless 'probe ids are exactly PR-01…PR-40' and a hash of `probes.json` matches. §1.6 compounds it by using a support vocabulary ('support level S = full or P = partial') that the frozen rubric does not use (its levels are FULL / PARTIAL / NONE with support_factor 1.0 / 0.5 / 0.0). Sibling doc 03 shows the correct pattern: it binds to probes by FUNCTION (ARG-PLAIN, ARG-EXPLICIT, ARITH) and states what happens if N differs.

**Required fix**

Rewrite §0.3 to name the real inputs: capability universe and probe set = `methodology/01_capability_universe_and_probes.json` (40 probes, ids `F01.P1`…`F20.P2`, `capability_points` per probe, `semantic_facts_expected` per probe, `support_rubric`); implementations = `semantic-compression/probes/<lang>/<probe_id>.<ext>`; support result = the published capability matrix file, named by its real path. Replace every occurrence of `PR-NN` / `PR-01` with `<probe_id>` / `F01.P1`. Rewrite §1.6 as `Supp(L) = { p : support_level(L,p) ∈ {FULL, PARTIAL} }` and make the §1.8 schema field `"support": "FULL" | "PARTIAL"`. Rewrite V13 as: 'probe count is exactly 40; ids are exactly those in 01_capability_universe_and_probes.json; the SHA-256 of that file matches the pre-measurement hash.' Rewrite V10 against the real coverage numerator (see the CapPoints finding below).

---

## [BLOCKER] Finding 3
*Spec section:* §6.1.4.D; §4 General Evaluation Principle; doc 04 §1.3, §1.8, §3.6 line 969

**Issue**

MECHANICAL REPRODUCIBILITY — Part 1 never defines the candidate set. §1.3 gives a five-condition test for 'a candidate behavior B', but nothing says where candidates come from, so no two analysts enumerate the same candidates and H(L,p) is not reproducible. §3.6 (line 969) says rules tagged `is_implicit`/`is_context_sensitive` 'are the candidate source for H1–H9 triggers, and each trigger must cite the rule's citation_id', but that sentence sits in a reporting table in Part 3, is never made binding in Part 1, and the §1.8 event schema requires only `citation_id` + `evidence_id` — there is no `rule_id` field, so the link to the registry is not actually enforced. Compounding this, doc 04 has a single unnamed 'operator' with no blinding, no second analyst, and no adjudication procedure for the central judgement ('does the reference fix exactly one outcome?'), whereas sibling doc 03 §0.2/§3.2 specifies two analysts blind to running totals.

**Required fix**

Add §1.3.0 (Closed candidate enumeration, binding): 'For language L and supported probe p, the candidate set is EXACTLY the set of rules r in registry(L) with p ∈ r.probes and r.tags.is_implicit OR r.tags.is_context_sensitive. No behavior outside this set may be counted, and every member of the set must be adjudicated against §1.3 and recorded as either a triggered event or an led[] rejection. The registry is therefore built (Part 2) before Part 1 is scored.' Add `"rule_id"` as a REQUIRED field on every event object and every `led[]` object in the §1.8 schema, and add V16: 'every event.rule_id and led.rule_id exists in registry(L) and lists p in its probes[]; the union of events and led entries for (L,p) equals the candidate set.' Import doc 03's dual-analyst procedure verbatim: two analysts adjudicate independently from a language-anonymized packet, disagreements are resolved by a written tie-break recorded in the per-probe record, and the raw agreement rate is published.

---

## [BLOCKER] Finding 4
*Spec section:* §6.1.4.D; doc 01 R5 / measured_fragment; doc 04 §0.7 line 155, §1.3.2

**Issue**

MECHANICAL REPRODUCIBILITY — the 'local site' is defined against an artifact that does not exist, and is self-contradictory for the actual corpus. §0.7 defines it as 'the single statement or expression that the frozen probe annotation points at'. The frozen probe file has no such pointer: it has `measured_fragment`, which is usually SEVERAL statements (F16.P2 = four statements including a `for` loop and a `match`; F17.P1 = an entire function declaration and body; F09.P2 = two statements). §1.3.2 then presumes multiple local sites per probe ('Two different local sites inside one probe are different events'), contradicting the singular definition. An analyst cannot determine, for F16.P2, whether the local site is the whole four-statement fragment (so a lookup-miss behavior is 'local' because the `match` two statements later resolves it) or each statement in isolation (so it is not). H(L,p) differs by several items depending on which reading is taken, for every multi-statement probe — i.e. most of the corpus.

**Required fix**

Replace the §0.7 definition with: 'The local sites of probe p are exactly the top-level statements and, for an expression-only fragment, the single expression, enumerated in order inside that probe's `measured_fragment` as authored under doc 01 R5. A construct nested inside a statement (a loop body, a match arm, a lambda body) belongs to the local site of its enclosing top-level statement. Each local site is adjudicated separately; H(L,p) is the cardinality of the union of items triggered over all local sites of p. Tokens in a different local site of the same fragment are NOT local for the site under adjudication.' Then align terminology with doc 03's 'focus span' and state explicitly which document wins where they differ.

---

## [MAJOR] Finding 5
*Spec section:* §6.1.4.D ('count semantically material behaviors that may occur without being signaled at the use site'); doc 04 §1.3 condition 3, line 264

**Issue**

SPEC COMPLIANCE + REPRODUCIBILITY — §1.3 condition 3 ('In annotated scope') adds a gate the spec does not contain, and its key verb is undefined. Spec §6.1.4.D counts material behaviors that are unsignalled, full stop; doc 04 counts only those that 'affect at least one semantic fact that the frozen semantic-fact annotation lists for this probe'. The annotation was written for the density metric and is narrow: only 9 of 40 probes list `conversion_behavior`, only 9 list `aliasing_writable_aliasing`, only 9 list `overflow_exceptional_numeric`. Concretely, F09.P2 (sort + `a < b`) lists neither `conversion_behavior` nor `overflow_exceptional_numeric`, so on the one comparison probe in the corpus it is undecidable whether TypeScript's/C++'s implicit conversions at the comparison site count (H1) — under a narrow reading of 'affects' they are silenced; under a wide reading ('the conversion changes type_and_representation, which IS listed') the gate is vacuous. F03.P1 (mutation) omits `aliasing_writable_aliasing`, so H2 is silenced on the mutation probe. Two honest analysts will produce materially different H matrices.

**Required fix**

Either delete condition 3 (spec-faithful), or keep it and define it operationally in the same paragraph: 'B affects fact F iff, for the probe's declared input domain, the correct answer a reader must give for F changes depending on whether B occurs. The operator records, for every rejected candidate, which listed facts were tested and why none is affected.' If it is kept, add a declaration naming the items it can suppress per probe, and add to §1.9 the audit line: 'the count of candidates rejected solely by condition 3, per language per item, is published' — so an item silenced corpus-wide is visible rather than invisible.

---

## [MAJOR] Finding 6
*Spec section:* §6.1.4.D; §20 (which forbids double counting one RULE, not merging distinct EVENTS); doc 04 §1.5

**Issue**

SPEC COMPLIANCE — the scored unit is not what the spec asks for. Spec §6.1.4.D says to count BEHAVIORS; §1.5 scores 'the count of distinct checklist items triggered, not the count of occurrences', explicitly noting 'A probe that triggers implicit conversion five times and nothing else scores 1' and capping H(L,p) at 9. The document itself defines distinct events in §1.3.2 and records `occurrences` — then discards them from the score. §20's anti-double-counting rule is already fully discharged by the single-assignment rule of §1.3.1 (one event → one item); collapsing five distinct conversion events to 1 is an additional choice with no spec warrant, and it flattens exactly the differences the metric exists to expose (a language concealing 5 conversions per probe is indistinguishable from one concealing 1).

**Required fix**

Score the event count and keep the item count as reported evidence: 'H(L,p) = number of distinct events (§1.3.2) assigned to any item H1–H9 at any local site of p, each event assigned to exactly one item by §1.3.1. The vector of items triggered and the per-item occurrence counts are published beside it as raw evidence.' Update V1 to 'H(L,p) is a non-negative integer' and §1.7's epsilon justification accordingly. If the item-level unit is deliberately retained, §1.5 must state that it is a deviation from the literal wording of §6.1.4.D, give the reason, and publish BOTH raw aggregates (item-mean and event-mean) with both normalized scores, per §25.4's dual-publication requirement for formula departures.

---

## [MAJOR] Finding 7
*Spec section:* §25.4 ('Do not select ... transformations after observing results'), §32; doc 04 §2.3 G4, §4.1 step 3

**Issue**

POST-RESULT LEVER — G4 permits a rule change after per-language results are in view. The operator counts SCU for one probe across all ten languages, looks at max/min, and is then authorized to make 'a clarification of granularity applied identically and retroactively to all ten languages'. The identity of each language is known at that moment, the trigger threshold (ratio > 5) has no defined consequence other than 'write a justification', and the content of a permissible 'clarification' is unbounded. Granularity is the single largest determinant of SCU, so this is a legitimized opportunity to retune the metric after seeing where Quidra lands on probe 1. It is also arbitrary that the calibration probe is F01.P1, the most trivial probe in the corpus (a constant binding), which will not exercise the granularity questions that matter (generics, error propagation, resource cleanup).

**Required fix**

Blind and close the loop: (a) run G4 on a fixed, pre-declared SET of three probes spanning difficulty — one simple (F01.P1), one mid (F09.P2), one hard (F17.P1) — not one trivial probe; (b) require the calibration packets to be language-anonymized as L1…L10, with identities not revealed to the adjudicator until the granularity text is frozen and hashed; (c) replace 'a clarification' with a closed list of permitted actions, e.g. 'the only permitted change is to merge candidate rules that fail G2 or to split rules that fail G3, stated as a general predicate over reference statements and containing no language name'; (d) require publication of every language's SCU for the calibration probes BEFORE and AFTER the clarification, so its effect is visible; (e) state that once any non-calibration probe has been counted, no granularity change is permitted for any reason.

---

## [MAJOR] Finding 8
*Spec section:* §6.1.4.E ('based on written language rules and reproducible probe evidence'); doc 04 §2.3 G2/G3, §2.7 schema

**Issue**

REPRODUCIBILITY — the granularity tests are stated but never made falsifiable. G2 is an existential over an infinite program space ('there exists a program ... that violates S1 while correctly applying S2') and G3 a universal over the corpus, yet the only artifact required is `granularity_notes`: 'G2 satisfied against KOTLIN.R.0030: <one sentence>'. An independent analyst cannot check a one-sentence assertion, cannot reconstruct why a language ended with 180 rules rather than 60, and SCU — the entire numerator of Capability Efficiency and the entire §20 Total Semantic Rules count — therefore is not reproducible. This is the metric the spec most explicitly demands be evidence-backed.

**Required fix**

Require a stored witness per adjacent rule pair: add to the §2.7 schema `"g2_witness": {"violates_this_satisfies_base": "<path to fragment>", "violates_base_satisfies_this": "<path to fragment>"}` and `"g3_check": "<the probes in which both rules are jointly satisfied/violated, or the probe that separates them>"`, with fragments stored under `semantic-compression/raw/granularity/<lang>/<rule_id>/`. Add V17: 'every rule whose granularity_notes names a neighbouring rule has both G2 witness fragments on disk, and each builds (or is explicitly marked as a static/non-buildable witness with a citation).' Additionally require publication of the per-language rule-count histogram by category before normalization, so a 3× SCU gap is visibly attributable to named rules.

---

## [MAJOR] Finding 9
*Spec section:* §6.1.4.E; §7 (advantages of established languages must be scored normally); doc 04 §2.4 bullet 2

**Issue**

FAIRNESS ACROSS THE TEN — the standard-library counting rule is both undefined and directionally uneven. §2.4 says 'a library function's documented contract counts as ONE rule unless the probe depends on more of its specified behavior'. 'Depends on more of its specified behavior' has no test. The consequence is systematic: where a language's stdlib supplies the whole probe in one call, the language pays 1 SCU; where the probe must be assembled from language constructs, it pays one SCU per construct. On F16.P2 (map lookup with default), Python's `mp.get("b", 0)` is ~1 rule while an optional-returning `get` matched with a two-arm `match` (as in the frozen Quidra fragment, and as in Rust/Swift/Kotlin) is 3-4 rules. The same asymmetry runs the other way on F17.P1 (Python's `with` is 1 rule, C++ RAII is several). The metric therefore varies by an undefined factor with an undefined rule, for every language.

**Required fix**

Replace the bullet with an operational test: 'A standard-library API contributes one rule per DOCUMENTED BEHAVIORAL CLAUSE of its contract that the frozen fragment's stated observable behavior depends on — where a clause is an independently stated normative sentence in the pinned library reference (return value on success, behavior on the absent/failure case, mutation-in-place vs new value, ordering/complexity guarantee, error signalling). The operator lists the clauses relied on, by citation anchor, in the rule's granularity_notes. Clauses the fragment does not rely on are not counted.' Add a worked cross-language example for one probe (F16.P2 is the natural choice) showing the clause list for all ten languages, so the granularity is pinned by demonstration rather than prose.

---

## [MAJOR] Finding 10
*Spec section:* §6.1.3 / §20; doc 04 §3.1 clause 1, §2.1

**Issue**

FAIRNESS ACROSS THE TEN — no ruling on compile-time-only rules, which is decisive for TypeScript and significant for four others. §3.1 requires a counted rule to 'constrain or determine an observable semantic fact (§0.8 materiality)'. TypeScript's entire type system is erased and changes no runtime observable, so a literal reading gives TypeScript a near-empty type ledger — making it look artificially cheap on Capability Efficiency (SCU/CapPoints) while its erasure simultaneously drives its Hidden Semantic Cost up. The same question is unresolved for Rust's borrow checker, Java's checked-exception rule, Kotlin's null-safety checks, Swift's exclusivity rule and Zig's comptime: all are rules a reader must know to predict whether the frozen fragment is even a program, but none changes an observable of a fragment that compiles. The document is silent, so each analyst decides per language.

**Required fix**

Add §2.4.1 (binding, identical for all ten): 'A rule that governs only static acceptance — whether the frozen fragment is a legal program — is counted iff a reader must know it to predict any fact in that probe's `semantic_facts_expected` list, INCLUDING predicting that an alternative spelling of the fragment would be rejected. Static-acceptance rules are tagged `static_only: true` and their count is published separately per language beside SCU.' Then state the consequence explicitly for TypeScript (erased type rules are counted under this clause, since a reader must know them to predict `type_and_representation`) so the treatment is visible rather than emergent.

---

## [MAJOR] Finding 11
*Spec section:* §0.4 asymmetry declaration vs §1.4 H4/H1/H7 exclusions; §6.1.7

**Issue**

FAIRNESS ACROSS THE TEN — the 'observed' escape hatch covers counted rules but not exclusions, so documentation quality silently moves the score. §0.4 correctly rules that an undocumented-but-demonstrable rule still COUNTS (tagged `observed`). But the H4 exclusion is worded as 'a container literal or macro DOCUMENTED AS ALLOCATING', and H1/H7 exclusions similarly hinge on what 'the reference defines'. A language whose reference is thorough (ISO C++, JLS) escapes the charge; a language whose reference is thin (Zig 0.16, TypeScript's non-normative Handbook, and Quidra, whose pinned reference is the evaluated repo's own `docs/` tree plus `--help`) fails the exclusion and is charged for the identical construct. Note this cuts AGAINST Quidra as written, but it is still a non-neutral rule that makes hidden-cost partly a measure of documentation prose rather than of concealment.

**Required fix**

Make the exclusions symmetric with §0.4: add to §1.2 a sentence binding all items — 'An exclusion may be established by a `documented` citation OR by an `observed` record under §0.4, on the same terms as a trigger. Where the exclusion rests on an `observed` record, the record is published in the per-probe led[] entry.' Then reword the H4 exclusion to a construct test rather than a prose test: 'allocation named by a local token whose specified or demonstrated meaning is the creation of a new object/value of that type — a composite/container literal, a constructor or factory call, a `new`-style operator, an explicitly passed allocator, an explicit allocation call.'

---

## [MINOR] Finding 12
*Spec section:* §6.1.5; doc 01 capability_coverage; doc 04 §2.5 STEP 7, §2.6, V10

**Issue**

CapPoints is misdescribed as a count. STEP 7 says 'CapPoints(L) = the number of SUPPORTED capability points ... the same numerator used for C = 100 * supported / total', and V10 says it 'equals the supported-point count'. The frozen numerator in doc 01 is `Σ over 40 probes of capability_points × support_factor`, a weighted sum that is fractional whenever any probe is PARTIAL (support_factor 0.5) and whose maximum is 103, not 40. An operator reading 'the number of supported capability points' may produce a probe count or an integer point count instead. Relatedly, the document never states that a PARTIAL probe contributes its rules in full to the SCU numerator while contributing only half its points to the denominator — a real, defensible, but undeclared double charge for partial support.

**Required fix**

Rewrite STEP 7 as: 'CapPoints(L) = Σ_{p ∈ probes} capability_points(p) × support_factor(support_level(L,p)), taken verbatim from the published capability matrix — the identical numerator of C = 100 × CapPoints(L) / 103. It is a weighted point total in [0, 103] and may be fractional.' Rewrite V10 to check that equality numerically against the matrix, and add one sentence to §2.6: 'A PARTIAL probe contributes the rules its frozen fragment actually uses to SCU and half its capability points to CapPoints; the resulting asymmetry is declared here before measurement and applies identically to all ten languages.'

---

## [MINOR] Finding 13
*Spec section:* §25.1 family C shifted form; doc 04 §1.7

**Issue**

The epsilon justification is incorrect as stated. §1.7 derives `epsilon = 0.025` from '1/40' on the grounds that 'the maximum denominator is 40 probes, so the smallest non-zero difference this metric can resolve is 1/40'. The denominator is |Supp(L)|, which is below 40 for any language with an unsupported probe (several are expected — doc 01 NA-6 names F02.P2, F04.P1/P2, F05.P2, F14.P1, F17.P2, F19.P1/P2, F20.P1/P2 as expected NONEs for multiple languages). For a language supporting 25 probes the resolution is 1/25 = 0.04, coarser than the declared epsilon. The value itself is fine and predeclared; only the reasoning is wrong, and a wrong-but-cited derivation invites a later 'correction' of epsilon, which §25.4 forbids.

**Required fix**

Restate as: 'epsilon = 1/N = 0.025 where N = 40 is the fixed corpus size — the finest resolution this metric can attain for any language. It is a single fixed constant for all ten languages, is not recomputed from any language's |Supp(L)|, and may not be changed after any language has been measured.' (This also matches doc 03 §1.8's `epsilon = 1/N` convention, which should be cross-referenced.)

---

## [MINOR] Finding 14
*Spec section:* doc 04 §1.4 H3 exclusion 3

**Issue**

H3's third exclusion turns on an undefined and impossibly high bar: 'multiple candidates that the reference proves observationally equivalent for the probe's inputs.' No language reference 'proves' observational equivalence of overload or override candidates; read literally the exclusion is dead, read loosely it lets an analyst dismiss any overload set as 'obviously equivalent'. The same word appears in the §1.4 H3 'Counts when' clause as 'not semantically interchangeable'.

**Required fix**

Replace with an operational test: 'the candidates are excluded only if the operator records, per candidate, that it cannot differ from the others in any respect listed in §0.8 for the probe's declared input domain, citing the reference clause or an `observed` record for each. Absent that per-candidate record, the exclusion does not apply.'

---

## [MINOR] Finding 15
*Spec section:* §6.1.4.D; doc 04 §1.2.1

**Issue**

NO BIAS FOUND — reported for completeness, since the instruction asks for honest reporting in both directions. The eight mandatory LED illustrations do NOT describe Quidra's feature set: the four 'scores 0' rows describe an explicit-allocator language (Zig), an explicit-propagation language (Rust/Zig/Swift), arbitrary-precision integers (Python), and distinct wrapping/trapping operators (Swift `&+`, Zig `+%`) — Quidra's own frozen fragments show no allocator token (`map.Map<string,int32>()`, `[1,2,3]`) and fixed-width `int32`. The four 'triggers' rows describe C++/Go/Java (bare `f(x)` with by-reference callees), Rust (debug/release overflow) and C++ (implicit destructors) — and the first of those charges Quidra too, since its own F05.P1 fragment is `int[] x = [1,2,3]; f(x)` with a callee that writes `values[0]`. The nine checklist items map one-to-one onto the nine bullets of §6.1.4.D with nothing dropped, unsupported capabilities correctly stay in C's denominator (§0.9) rather than becoming N/A, N/A is never converted to zero, the normalization families and epsilon are predeclared with no post-result selection, §20 is correctly kept at zero weight in Standard, and the document contains no token-counting rule at all (that is doc 02), so no word-heavy/punctuation-heavy tilt can enter here. The mandatory §6.1.4.B `f(x)` probe is not this document's responsibility (it belongs to doc 03) and is present in the frozen corpus as F05.P1 with a companion F05.P2.

**Required fix**

No change required for this item. Retain §1.2.1's illustration set as written when addressing the other findings — in particular do not 'balance' it by adding a row that happens to describe Quidra's spelling of anything.

---
