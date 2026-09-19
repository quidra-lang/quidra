# Audit findings for `08_adversarial_cases.json`

**Verdict:** needs_fix  
**Quidra bias found:** True  

## [BLOCKER] Finding 1
*Spec section:* 25.4, 32 (no post-result selection); 17

**Issue**

QUIDRA BIAS + REPRODUCIBILITY. `type_binding_table.bindings` freezes a concrete construct for all nine comparison languages (Python `ctypes.c_int64`, C++ `std::int64_t`, TypeScript `Int32Array` element, Java `long`, Zig `i64`, ...), but every Quidra row is an unfilled placeholder: 'The type the Quidra documentation designates ... The operator records the citation.' Five of the ten bindings (`fixed_width_i64`, `fixed_width_i32`, `fixed_width_u32`, `most_general_reference`, `null_or_absent_value`) additionally carry 'If Quidra has none, rule TM3 applies', and TM3a awards 100. Quidra is therefore the only language whose case implementations are selected at run time, by an operator who can already see how each candidate construct behaves, and the selection can route straight to a perfect score. This defeats the document's own `status: FROZEN` claim for exactly one language — the one under evaluation.

**Required fix**

Publish an amendment that fills in all ten Quidra rows with named constructs and normative citations, and freeze it (with a sha256 recorded in the results) BEFORE the first Quidra adversarial program is written. For each Quidra row also pre-declare the TM3 branch (TM3a / TM3b / not applicable) with its citation, exactly as ADV-04's notes already do for Java ('TM3a applies to both sub-programs') and ADV-01's notes do for TypeScript and Python. If the amendment cannot be produced before measurement, record Quidra's affected rows as N/A-authoring-defect rather than letting the operator choose after the fact.

---

## [BLOCKER] Finding 2
*Spec section:* 7 (same workload in each language); 4 (measured, reproducible)

**Issue**

FAIRNESS — PYTHON. The Python binding for fixed-width integers is `ctypes.c_int32/c_int64/c_uint32`, and ctypes instances support no arithmetic, no ordering and no cross-type conversion. I verified this: `c_uint32(0) - 1` raises TypeError, `c_int32(-1) < c_uint32(1)` raises TypeError, `c_int32(c_int64(x))` raises TypeError. So ADV-01 step 2 ('convert it using the language's DEFAULT, UNQUALIFIED conversion form'), ADV-04a step 2 ('subtract ... using the ordinary subtraction operator') and ADV-04b step 2 ('evaluate the ordinary less-than comparison directly, with no cast') are literally unimplementable in Python as written, and the same applies to ADV-02, ADV-03, ADV-05 and ADV-08. The operator must insert `.value` somewhere, and the document gives no rule for where. The stage swings 0 / 75 / 100 on that undocumented choice: `c_int32(c_int64(MAX).value).value` yields -1 (Silent Bug, 0); leaving the operands as ctypes yields a TypeError (Runtime Safe Detection, 75 via L_TYPE at D5a'); `c_int32(-1).value < c_uint32(1).value` yields True on ADV-04b, which matches `reference_observation: CMP:true` and scores 100 at D6b. Separately this collides with authoring rule A2 ('the way a competent, non-defensive author of that language would write it') — no ordinary Python author reaches for ctypes.

**Required fix**

Add a `python_fixed_width_operand_rule` to `type_binding_table`: 'A Python fixed-width value is held as a ctypes instance. Every operation a construction names is performed by (a) reading `.value` from each operand, (b) applying the named Python operator to the resulting Python ints, and (c) storing the result back through the fixed-width constructor, whose documented modular conversion is the measured behaviour. The `.value` reads and the constructor store are binding plumbing and are not the conversion the case measures.' That makes ADV-01 deterministic (c_int32(...).value == -1, Silent Bug), ADV-04a deterministic (4294967295, Silent Bug) and ADV-04b deterministic (true, D6b). Then re-check A2 and state explicitly that the ctypes binding is a deliberate departure from idiomatic Python required to give Python a fixed-width type at all.

---

## [MAJOR] Finding 3
*Spec section:* 11 (define the Native/Interpreter combination rule before results); 7

**Issue**

QUIDRA BIAS. `toolchain_binding.quidra_mode_rule` scores Quidra from native compiled mode only and demotes interpreter mode to a non-scoring row. Spec 11 asks for a rule 'for combining Native and Interpreter evidence', not for discarding one mode, and no reason is given for preferring native over interpreter. The effect is not neutral: `toolchain_binding.rule` states that a language with no build step 'can never reach the Compile-time Detection rung' (Python is capped at 90 by D3b), so native mode is the Quidra configuration with the higher ceiling on the family-F ladder. Quidra is the only language permitted to be scored from one of two of its own implementations. This is also internally inconsistent: ADV-04 aggregates two sub-observations of the SAME language by 'the LOWEST-scoring stage ... it is only as safe as its weakest mandated sub-observation', yet Quidra's two modes are aggregated by keeping the stronger one.

**Required fix**

Replace `quidra_mode_rule` with an explicit combination rule that is score-blind and consistent with the document's own ADV-04 precedent. Either: (a) 'Quidra's scored stage for a row is the arithmetic mean of the native and interpreter stage scores', or (b) 'Quidra's scored stage for a row is the lower-scoring of its native and interpreter stages, by the same weakest-sub-observation rule declared for ADV-04.' Whichever is chosen, publish Quidra's Early Error Detection under native-only, interpreter-only and the combined rule side by side in the primary table, so the size of the mode choice is visible rather than absorbed.

---

## [MAJOR] Finding 4
*Spec section:* 7; 25.4

**Issue**

QUIDRA BIAS. Under `secondary_non_scoring_configurations`, Quidra's secondary configuration is the only one that contains a second, independent implementation of the language ('Quidra interpreter mode `quidra run FILE.qui` is recorded here as well'). Decision rule D7b promotes a record from Silent Bug (0) to Undefined Behavior (5) when the secondary configuration 'produces a DIFFERENT OBS payload from the primary configuration'. Two implementations of the same language disagree far more readily than one implementation with assertions enabled, so Quidra alone gets a systematic escape from 0 on every row where its compiler and interpreter differ. The document's own note claims D7b 'is applied identically to all ten languages'; it is not. The rationale for D7b is 'a checked build reports the hazard', which a second implementation does not satisfy.

**Required fix**

Amend D7b to read: 'The secondary configuration referred to here is the checked/sanitizer/assertion build of the SAME implementation. Observations from a second implementation of a language (in this run, Quidra interpreter mode) never satisfy D7b; they are recorded only in the Quidra native-versus-interpreter divergence table.' Add the corresponding line to `secondary_non_scoring_configurations.configurations.Quidra`.

---

## [MAJOR] Finding 5
*Spec section:* 17 (classify each case); 25.1.F

**Issue**

REPRODUCIBILITY. D1b awards the top rung (Compile-time Detection, 100) for ANY non-zero build exit that is not a toolchain crash, with no requirement that the diagnostic concern the case's hazard. D5a, by contrast, awards only 75 and only when stdout+stderr matches the case's `hazard_diagnostic_lexicon`. So a build that fails for a reason unrelated to the hazard scores 100 while a runtime that names the hazard exactly scores 75. `A6_compilability_precondition` only requires that the source match the construction; it does not require the rejection to be about the hazard. This systematically favours languages with strict front ends and makes an authoring accident indistinguishable from a real detection.

**Required fix**

Split D1b: 'D1b-i. The first build diagnostic matches this case's `hazard_diagnostic_lexicon`, or any fragment of `global_hazard_diagnostic_lexicon` → Compile-time Detection, 100, with the diagnostic quoted verbatim. D1b-ii. Otherwise the build failed for a reason unrelated to the hazard; this is an authoring defect under A6. The program is re-authored and re-run before any result is recorded; it is never scored 100.' Add the quoted diagnostic to `A10`-style audit requirements for every Compile-time Detection row, mirroring A10 for Runtime Safe Detection.

---

## [MAJOR] Finding 6
*Spec section:* 4 (explicitly defined objective criteria); 17

**Issue**

REPRODUCIBILITY. D5a_prime fires when 'a runtime-emitted diagnostic exists and names some SPECIFIC hazard, but not one in this case's lexicon', and D5b fires on 'only a generic one with no hazard named'. 'Specific' is nowhere defined, and the gap between the two rules is 55 points (75 vs 20). The document then asserts, in `crash_vs_runtime_safe_detection_summary`, that 'Both tests are performed on captured bytes, so neither depends on the auditor's judgement' — which is false for D5a_prime. Two independent analysts will classify a Python `TypeError`, a Kotlin `IllegalStateException` with no message, or a Swift `Fatal error:` with an unusual wording differently.

**Required fix**

Make D5a_prime mechanical by binding it to the already-frozen global lexicon: 'D5a_prime. The concatenation of stdout and stderr matches at least one fragment of `global_hazard_diagnostic_lexicon.fragments`, but not one listed in this case's `hazard_diagnostic_lexicon` → Runtime Safe Detection, 75, flag `diagnostic_mismatch`. D5b. No fragment of the global lexicon matches → Crash, 20.' Then delete the word 'SPECIFIC' from D5a_prime and keep the summary sentence, which then becomes true.

---

## [MAJOR] Finding 7
*Spec section:* 19; 25.1.C; 25.1.E

**Issue**

DEBUGGABILITY — FAILING IS REWARDED. `diagnosis_turns` is defined as turns 'up to and including the first turn in which the agent states the correct root cause' and `tokens_to_diagnosis` likewise, but neither is defined for a session in which the correct root cause is NEVER stated. `composite_score.failed_sessions` says such a session 'contributes its measured turns and tokens up to that point' and is not excluded. Both metrics are family C, lower-is-better. Consequently an agent that gives up after one turn without ever diagnosing scores near 100 on `diagnosis_turns`, `tokens_to_diagnosis`, `repair_turns`, `tokens_to_repair` and `misdiagnosis_count` — five of the six sub-metrics — while an agent that diagnoses correctly on turn 3 and fixes it scores far lower. `repair_success` is explicitly 'also_recorded_not_scored', so nothing inside Debuggability penalises the failure.

**Required fix**

Add to `recorded_metrics`: 'A session in which the correct root cause was never stated is right-censored. `diagnosis_turns` and `tokens_to_diagnosis` take the full session turn and token totals, and the session is additionally recorded as `diagnosis_failure: true`.' Then add a seventh sub-metric to `composite_score`, `diagnosis_success` = 100 * (sessions reaching a correct root cause / corpus entries), normalization family A, and change the weighting from 1/6 each to 1/7 each. State the change and the reason in the document before the run.

---

## [MAJOR] Finding 8
*Spec section:* 7 (same rule for every language); 4

**Issue**

DEBUGGABILITY — LANGUAGE-BIASED ROOT-CAUSE MATCHER. `root_cause_key.keys` are unanchored case-insensitive alternations that embed language-specific vocabulary. ADV-01/R requires a match on `(64|sixty-four)[- ]?bit|int64|i64|long` and on `(32|thirty-two)[- ]?bit|int32|i32`: a Java, C++, Kotlin or Rust transcript satisfies both for free merely by naming the types in the source (`long`, `int32_t`, `i64`, `i32`), while a Python or TypeScript transcript expressing the identical insight in prose may not. ADV-25/R requires `parse|convert|atoi|parseInt|strconv|Integer\.parse`, which again rewards transcripts that quote a specific language's API name. `long` is also unanchored and matches inside 'along' and 'belong'; ADV-17's `(length|len|size|count|member|field|propert)` matches inside dozens of unrelated words. The same conceptual diagnosis therefore counts as correct in some languages and not others.

**Required fix**

Rewrite every key with `\b` word boundaries, and replace language-specific type and API names with language-neutral concept alternations (e.g. ADV-01/R key 1 → `\b(64[- ]?bit|sixty-four[- ]?bit|wider|source (type|width))\b`, key 2 → `\b(32[- ]?bit|thirty-two[- ]?bit|narrower|target (type|width))\b`; ADV-25/R key 1 → `\b(parse|parsing|convert|conversion|numeric conversion)\b`). Alternatively pre-register a per-(case, language) key table that names each language's own type and API spellings symmetrically. Also tighten ADV-17 key 2 to `\b(length|len|size|count|member|field|property|properties)\b`.

---

## [MAJOR] Finding 9
*Spec section:* 25.2 (unweighted arithmetic mean unless THIS specification defines otherwise)

**Issue**

INVENTED AGGREGATION. ADV-04 is the only row in the set that aggregates sub-observations, and it does so by `sub_program_aggregation`: 'the LOWEST-scoring stage among its sub-programs'. Spec 25.2 fixes the unweighted arithmetic mean as the default and permits a different aggregation only when 'this specification' — prompt.md — explicitly defines one; it does not. Min-aggregation is also consequential and asymmetric: Kotlin is expected to reach Compile-time Detection (100) on ADV-04b and wrap silently (0) on ADV-04a, so min gives it 0; Java, which reaches TM3a on both sub-programs for lacking unsigned types, keeps 100. Two rows (ADV-04/C and ADV-04/R) move by up to 100 points each on a rule this document invented.

**Required fix**

Either (a) replace `sub_program_aggregation` with 'the arithmetic mean of the two sub-programs' stage scores (spec 25.2)', or (b) promote ADV-04a and ADV-04b to four separate rows of `fixed_scored_case_variant_list` (ADV-04a/C, ADV-04a/R, ADV-04b/C, ADV-04b/R), making the set 36 rows and removing the aggregation entirely — which is what the document already does for the C/R variants and is the more informative option. Update `Early_Error_Detection.denominator`, `A1`, `Boundary_Value_Safety.subset_rows` and `program_count_note` accordingly.

---

## [MAJOR] Finding 10
*Spec section:* 25.1.F ('Average over the fixed adversarial case set'); 17

**Issue**

WEIGHTING. Early Error Detection is the mean over 34 case-variant rows, but the 26 spec-17 hazards are not equally represented: eight hazards (ADV-01, 02, 03, 04, 05, 08, 09, 10 — all integer arithmetic, narrowing and indexing) contribute two rows each and therefore carry double weight, so 16 of 34 rows (47%) come from one narrow family, while 'malformed UTF-8', 'null misuse', 'missing return', 'invalid cast' and 'invalid mutation' carry single weight. Spec 17 enumerates hazards, not variants, and the document's own `one_case_per_hazard: true` asserts one case per hazard. The C/R split is a good idea; letting it silently reweight the hazard families is not.

**Required fix**

Keep the 34 rows as the raw evidence table, but define the scored quantity per hazard: 'Early Error Detection = mean over the 26 cases of each case's stage score, where a two-variant case's score is the unweighted mean of its C and R variant stage scores (spec 25.2).' Publish the 34-row per-variant mean beside it as a secondary figure, and state which of the two is the scored number before the run.

---

## [MAJOR] Finding 11
*Spec section:* 26; 7 ('Do not compensate a young language for lacking ... maturity'); 32

**Issue**

FEATURE ABSENCE SCORES 100. `TM3a` and the `Prevented By Construction` rung award the maximum stage score (100, identical to Compile-time Detection) when a language cannot express a hazard. The document's own examples make the consequence plain: Java scores 100 on the signed/unsigned hazard solely because JLS 4.2 gives it no unsigned type; Python scores 100 on integer overflow, underflow and the huge numeric literal solely because its int is arbitrary precision; a language with a bigger stack scores 100 on ADV-20 where Python's RecursionError scores 75. That is a defensible safety claim, but as currently specified a reader cannot tell how much of any language's Early Error Detection came from detecting hazards versus from not having the feature. In this benchmark that matters most for Quidra, whose bindings are unfrozen (see the first finding) and which therefore has the most TM3a routes still open. It is also the exact shape the spec warns about — a metric on which having fewer features wins.

**Required fix**

Keep the rung and the 100, which are pre-registered, but make the composition mandatory and visible. Add to `metrics_computed_from_this_evidence.Early_Error_Detection`: 'Every published Early Error Detection score carries three companion figures: `n_prevented_by_construction`, `n_detected` and `EED_detected_only` = the mean stage score over rows whose stage is not Prevented By Construction, with its own row count. Rankings cite both EED and EED_detected_only.' Add the same companion columns to Boundary_Value_Safety and Adversarial_Input_Robustness, and add an audit item A17 requiring them.

---

## [MAJOR] Finding 12
*Spec section:* 4; 25.2

**Issue**

RUNTIME SAFETY IS NOT COMPARABLE ACROSS LANGUAGES. `Runtime_Safety.raw_fraction` divides by 'rows that were not detected before execution', so every language is scored over a different, language-determined denominator. Rust, Kotlin and Swift, which reject many C-variant rows at compile time, are scored over a small handful of rows where one outcome moves the score tens of points; Python, which can never reach the compile rungs at all, is scored over nearly all 34. A language can also raise its Runtime Safety by detecting more at compile time, which is already rewarded by Early Error Detection. The denominator is not required to be published, so a reader cannot see that a 100.00 came from two rows.

**Required fix**

Add to `Runtime_Safety`: 'The denominator count is printed next to every Runtime Safety score, and a score computed over fewer than 8 rows is additionally marked `low_denominator`.' Publish, beside it, a fixed-denominator companion `Runtime_Safety_all_rows` = count(Runtime Safe Detection or Prevented By Construction) / count(all non-N/A rows), so the two readings are both visible and neither is chosen after results.

---

## [MINOR] Finding 13
*Spec section:* 7; 25.4

**Issue**

ASYMMETRIC SECONDARY CONFIGURATIONS DRIVE D7b. D7b is the one place a secondary configuration changes a score (0 → 5). But the configurations differ enormously in power: C++ gets ASan+UBSan, Rust gets debug-assertions, Zig gets ReleaseSafe, Swift gets -Onone, TypeScript gets --strict, while Python gets only `-X dev`, Go only `-race` (which the document itself notes 'for most cases changes nothing'), and Java and Kotlin only `-ea`. So the 0 → 5 promotion is realistically available to five languages and not to four, even though D7b's note claims it 'is applied identically to all ten languages'.

**Required fix**

Reword the D7b note to state the asymmetry honestly rather than claim identity: 'This exception is declared before measurement and evaluated by the same rule for all ten languages; the languages' secondary configurations differ in detection power, and which languages can in practice satisfy D7b is itself published in the secondary-configuration table.' Add a column to that table recording, per language, whether its secondary configuration is capable of reporting the hazard class at all.

---

## [MINOR] Finding 14
*Spec section:* 4; 17

**Issue**

D7b IS UNDEFINED WHEN THE SECONDARY CONFIGURATION PRODUCES NO OBSERVATION. D7b fires when the secondary configuration 'emits a report that names undefined, illegal or erroneous behaviour ... or produces a DIFFERENT OBS payload from the primary configuration'. It says nothing about a secondary configuration that fails to build (TypeScript under `tsc --strict`, Zig under `-OReleaseSafe` where a check becomes a compile error) or that aborts before printing OBS. 'No OBS payload' is trivially 'different' on one reading and 'not a payload at all' on another, and the difference is 5 points on every such row.

**Required fix**

Add a clause to D7b: 'A secondary configuration that fails to build, or that terminates before writing its OBS line, does not by itself satisfy the different-payload condition. It satisfies D7b only through the first condition, i.e. if its build or run output names undefined, illegal or erroneous behaviour for this operation. The outcome is recorded either way in the secondary table.'

---

## [MINOR] Finding 15
*Spec section:* 17; 4

**Issue**

OVER-BROAD LEXICON FRAGMENTS CAN PROMOTE A CRASH TO RUNTIME SAFE DETECTION. Several `global_hazard_diagnostic_lexicon.fragments` are unanchored substrings that match ordinary English: `L_NULL` contains bare `null|nil|none` (matches inside 'nonexistent'), `L_NARROW` contains bare `conversion|convert`, `L_STATE` is `invariant|precondition|contract|assertion|requires` (matches any message containing 'requires'), and `L_BOUNDS` contains bare `subscript`. Because the match is against the concatenation of stdout and stderr, an incidental word in a runtime's generic abort message can carry a row from Crash (20) to Runtime Safe Detection (75).

**Required fix**

Anchor the loose alternatives with word boundaries and drop the ones that carry no hazard information: `L_NULL` → `\bnull\b|\bnil\b|\bnone\b|NullPointerException|...`; `L_NARROW` → remove bare `convert`, keep `\bnarrow|truncat|does not fit|cannot be represented|overflow|out of range`; `L_STATE` → `\binvariant\b|\bprecondition\b|\bcontract\b|\bassertion (failed|failure)\b` (drop bare `requires` and bare `assertion`); `L_BOUNDS` → `\bsubscript\b`. Re-run the frozen self-test over the fragment set after the change.

---

## [MINOR] Finding 16
*Spec section:* 25.1.C (family-C reporting requirement)

**Issue**

MISSING MANDATORY FAMILY-C DISCLOSURE. Four Debuggability sub-metrics (`diagnosis_turns`, `repair_turns`, `tokens_to_diagnosis`, `tokens_to_repair`) are declared family C, but the document nowhere carries spec 25.1's conditional obligation: when a family-C metric's applicable raw values span a factor of 100 or more, the raw value, the ratio `raw_i / best_positive_raw` and an explicit compression note must be published beside the normalized score. `tokens_to_diagnosis` and `tokens_to_repair` can easily span that range across ten languages (a one-turn compile-error session versus a four-turn session with large compiler output).

**Required fix**

Add to `debuggability_protocol.recorded_metrics`: 'For each family-C sub-metric, publish per language the raw value in its natural unit and the ratio raw_i / best_positive_raw. Where the applicable raw values span a factor of 100 or more, additionally publish the compression note required by spec 25.1.C, stating that the ratios rather than the normalized scores carry the comparison between the non-leading languages.' Add a matching audit item.

---

## [MINOR] Finding 17
*Spec section:* 25.1 (a changed normalization family must publish results under both old and new formula); 25.4

**Issue**

THE NEW STAGE LABEL IS NOT DUAL-PUBLISHED. `fixed_stage_score_table.registered_addition` adds a ninth stage, 'Prevented By Construction' = 100, to a table spec 25.1.F states verbatim and this document itself says 'must not be changed, reweighted, rescaled, or interpolated'. Spec 25.1 allows a registered change to a normalization family only if 'that run must publish its results under both the old and the new formula, so that the effect of the change is visible'. The document requires separate labelling in tables, which is good, but does not require the dual-formula publication.

**Required fix**

Add to `fixed_stage_score_table.registered_addition`: 'Because this is a registered addition to a fixed normalization family, this run publishes Early Error Detection, Boundary Value Safety and Adversarial Input Robustness twice: once with Prevented By Construction scored 100, and once with Prevented By Construction rows excluded from numerator and denominator (the pre-addition reading). Both tables appear in the Adversarial / Safety results (spec 25.1).' This also satisfies the sensitivity column asked for in the TM3a finding.

---

## [MINOR] Finding 18
*Spec section:* 25.4; 7

**Issue**

ADV-22 SELECTS ITS STIMULUS PER LANGUAGE AFTER OBSERVING A RESULT. `frozen_generators.malformed_source.selection_rule` reads 'Use the primary mutation. If the primary mutation's output builds and runs successfully, use the fallback.' So a language whose truncated file happens to compile receives a different, harsher mutated file than the others, chosen after its build outcome was observed. This breaks authoring rule A1 (the ten implementations must be the same program) and is the shape of post-result stimulus selection that spec 25.4 forbids, even though the score impact here is probably small.

**Required fix**

Run BOTH mutations for all ten languages and add them as two rows, ADV-22a (60% truncation) and ADV-22b ('@#$' injected at the midpoint), each scored in the normal way; the case's score is the mean of the two per spec 25.2. Update the row count and audit item A1. If two rows are unacceptable, instead pre-register per language which mutation is used by running both mutations against the VALID files only, before any hazard result is recorded, and freeze the table.

---

## [MINOR] Finding 19
*Spec section:* 4; 25.4

**Issue**

FIVE CATEGORIES OF AUTHORING CHOICE ARE RECORDED BUT NOT PRE-REGISTERED. Audit item A16 concedes that the cases permit per-language choices of 'parsing facility, immutability form, string length member, index-type construction, growable-sequence substitution', requiring only that they be recorded afterwards in `authoring_choices`. Recording a choice makes it auditable; it does not make it reproducible. ADV-16's 'STRONGEST form for expressing that the value must never change' (Java `final` vs a compile-time constant; C++ `const` vs `constexpr`; Kotlin `val` vs `const val`) and ADV-09's unsigned-index construction can each move a row between Compile-time Detection (100) and Silent Bug (0).

**Required fix**

Add a frozen `authoring_choice_table` with the same shape as `type_binding_table`: one named construct per (choice category, language), with a documentation citation, filled in for all ten languages before measurement. Change A16 to read: 'Every per-language authoring choice was taken from the frozen `authoring_choice_table`; `authoring_choices` records the table row used, not a free selection.'

---

## [MINOR] Finding 20
*Spec section:* 8 (double-counting); 18

**Issue**

TWO INACCURATE SELF-DESCRIPTIONS. (a) `metrics_computed_from_this_evidence.note` asserts 'no quantity below is counted twice inside the Standard score', but the same 34 stage scores feed Early Error Detection, Boundary Value Safety, Adversarial Input Robustness, Silent Bug Resistance, Runtime Safety and (as `detection_stage`, at weight 1/6) Debuggability — six metrics inside one 25%-weighted Safety/Robustness category. Spec 18 mandates most of that reuse, so the practice is correct and the sentence is wrong. (b) `outcome_classes.ladder_has_eight_rungs` says the `class` field is 'one of the seven classes of spec 17', while `observation_record_schema.fields.class` allows an eighth value, 'Prevented By Construction', which D0 and D6b assign.

**Required fix**

(a) Replace the note with: 'Spec 18 directs this one body of evidence to Early Error Detection, Runtime Safety, Boundary Value Safety, Silent Bug Resistance and Debuggability, and this document additionally derives Adversarial Input Robustness and Compiler / Interpreter Robustness from it. The metrics deliberately share evidence; each is computed once, by the formula given here, and the overlap is a property of spec 8's metric list, not a double count introduced here.' (b) Change `ladder_has_eight_rungs` to say the `class` field carries one of the seven spec-17 classes or the registered label `Prevented By Construction`, which denotes the absence of a defect rather than a class of defect.

---

## [MINOR] Finding 21
*Spec section:* 7; 17

**Issue**

AUTHORING RULE A3 DOES NOT COVER TWO OF THE TEN LANGUAGES. `A3_shortest_direct_use_rule` enumerates the canonical form for Rust, Go, Java, C++, Swift, Kotlin, TypeScript and Python, and introduces the list with 'stated so that it cannot be argued about later' — but it omits Zig and Quidra. Zig is exactly the language where the choice matters most: on ADV-11 (huge allocation) `try` in a `!void` main surfaces `error: OutOfMemory`, which matches L_ALLOC and scores Runtime Safe Detection (75), whereas `catch unreachable` panics with 'reached unreachable code', which matches nothing and scores Crash (20). The same 55-point fork appears on ADV-23 and ADV-25.

**Required fix**

Extend A3's enumeration to all ten: 'Zig: `try` inside a function whose return type is `!void`, never `catch unreachable`, never `orelse`, never `catch |e| ...`. Quidra: the shortest direct-use form Quidra's documentation designates, named and cited in the frozen Quidra binding amendment before any Quidra program is written.' Add an audit item requiring every Zig program to be checked against this clause.

---

## [MINOR] Finding 22
*Spec section:* 8; 25.4

**Issue**

SUBSET MEMBERSHIP HAS NO STATED PREDICATE. `Boundary_Value_Safety` and `Adversarial_Input_Robustness` are defined by hand-listed case ids with no membership criterion. The choices are arguable in both directions: ADV-06 (NaN) and ADV-07 (Infinity) are floating-point boundary values but are excluded from Boundary Value Safety; ADV-19 and ADV-20 (recursion depth) and ADV-21/ADV-22 (parser nesting, malformed source) are counted as 'adversarial input' although no external input is involved; and ADV-14 (type mismatch) and ADV-26 (invalid program state) belong to neither subset. Because the lists are frozen, nothing was chosen after results — but an independent analyst cannot re-derive them, and that is the reproducibility standard the document sets for itself.

**Required fix**

State the membership predicate above each list and confirm the list follows from it, e.g. 'Boundary Value Safety contains every row whose hazard is a value at or beyond a representable boundary of a numeric or index domain' — which brings ADV-06 and ADV-07 in — and 'Adversarial Input Robustness contains every row whose hazardous stimulus is supplied to the toolchain or the program from outside the program's own value domain (source text, file bytes, stdin, or unbounded resource demand).' If a predicate would change a list, change the list now, before measurement, and note the change.

---
