# Audit findings for `02_fact_taxonomy_and_density.md`

**Verdict:** needs_fix  
**Quidra bias found:** True  

## [BLOCKER] Finding 1
*Spec section:* §6.1.4.A / §6.1.4.C (artifact §2.2, §2.3 vs methodology 03 §2.2.2, §2.7)

**Issue**

Artifact §2.3 asserts "The fact ledger is shared. Metrics A (Density) and C (Locality) are computed from the same annotated rows, so the two metrics cannot disagree about a fact." This is false against the frozen sibling document 03_determinacy_and_locality.md, which defines Locality independently and contradicts 02 on two rules. (1) Stdlib hops: 02 §2.2 grants a free pass — a row is COUNT iff `hops_decl == 0` and `hops_stdlib <= 1`, and §2.3 says such a row contributes "0 required declaration-graph hops to Semantic Locality"; 03 Rule 2.2.2 says "Standard-library declarations are hops on the same terms as user declarations." (2) Unresolvable facts: 02 §2.3 says an `INDET` row contributes "0 to Locality"; 03 Rule 2.7.1 says an unresolvable fact saturates the whole probe to `H_i = SAT = 6`, the worst bounded value, explicitly because counting it as small "is a perverse incentive and would corrupt the metric." Two frozen documents in one run give opposite rules for the same rows, and 03 also uses a per-probe minimum-resolving-set algorithm with a different record schema, so no shared ledger exists at all. An analyst cannot compute Locality from 02's ledger or Density consistent with 03.

**Required fix**

Delete the shared-ledger claim in §2.3 and replace it with an explicit boundary: "Metric A is computed from the per-(site,fact) ledger defined here. Metric C is computed by methodology 03's per-probe algorithm and is authoritative for all hop counts; the `hops_decl`/`hops_stdlib` integers recorded here are annotation aids and inputs to the COUNT test only, and are never summed into `H_i`." Then resolve the substantive disagreement in one direction and state it in both files: either (a) drop the `hops_stdlib <= 1` free pass and make a stdlib contract consultation a `LOOKUP` for Density too, matching 03 Rule 2.2.2; or (b) keep it for Density and add to 03 §2.2.2 an explicit exception naming 02 §2.2. Add to both files: "An `INDET` fact contributes 0 to Density and triggers 03 Rule 2.7.1 for Locality."

---

## [BLOCKER] Finding 2
*Spec section:* §6.1.7 (freeze probes before measuring); artifact §4.2 R1, §2.6, §5.1

**Issue**

Three incompatible probe-identifier schemes are frozen across the same run, and the probe-region delimiter mechanism does not exist in the probe document. Artifact §5.1 says "the 40 frozen probes `P01…P40` defined in the frozen capability/probe document" and §2.6's ledger example uses `"probe_id": "P07"`; 01_capability_universe_and_probes.json actually uses `F01.P1 … F20.P2`; 03_determinacy_and_locality.md uses a third form, `SC-F05-P1`. The `F01`-style prefix in 01 also collides with this document's `F01–F13` fact-kind labels, so `F09` is both "overflow/exceptional numeric behavior" and family 9's probe prefix. Separately, §4.2 R1 makes the tokenizer abort unless each probe file contains literal `BEGIN PROBE <id>` / `END PROBE <id>` marker comment lines, but 01 defines every boundary in prose (`measured_fragment`) and 01 R3_minimality requires fragments with "no comments". No document assigns responsibility for inserting the markers, so as frozen the tokenizer aborts on all 400 fragments.

**Required fix**

Pick 01's `F<nn>.P<k>` as the single authoritative id, rewrite §2.6, §5.1 and §5.5 to use it, and rename the fact kinds to a non-colliding prefix (`K01–K13`) throughout §1–§6 and the ledger schema. Add to §4.2 R1: "The measurement harness writes each frozen fragment into its runnable wrapper and emits the two marker comment lines itself; the marker lines are harness output, not fragment content, and are removed by R2 before counting. 01 R3_minimality applies to fragment content only."

---

## [MAJOR] Finding 3
*Spec section:* §6.1.4.A ("so punctuation-heavy and word-heavy syntaxes are treated consistently"); artifact §4.1, §4.3, §4.4(a)

**Issue**

QUIDRA BIAS. §4.1 states the neutrality guarantee as "every lexeme is exactly one token… No class is weighted, discounted, or exempted," and §4.3 repeats it. §4.4(a) then breaks it: "The hole costs exactly one token regardless of how the language spells it." An interpolation hole is two delimiters in every language that has one — Python `{`…`}`, TypeScript `${`…`}`, Kotlin `${`…`}`, Swift `\(`…`)`, and Quidra `{`…`}` (confirmed in the Quidra reference, `"A{tab}B"`, and in CORRECTIONS D-3, `print("a={v}")` = 6). Counting them as one token is a flat one-token-per-hole discount to exactly five languages — Python, TypeScript, Kotlin, Swift and Quidra — while the five without interpolation (C++, Go, Java, Rust, Zig) pay every `(`, `,` and `)` of the call form separately under the same section's own rule. It is the only class exemption in §4, and Quidra is inside the favoured group.

**Required fix**

Change §4.4(a) to: "An interpolated string emits 1 `STR` token for the literal shell, and for each embedded expression 1 `HOLE_OPEN` token, the tokens of the embedded expression, and 1 `HOLE_CLOSE` token — matching the two delimiters every interpolating language writes and the two delimiters a call form pays." Update the §4.4(a) table (Python/TypeScript/Kotlin/Swift become 4) and the §4.5 step-6 fixtures. If the one-token hole is kept instead, delete the "no class is discounted" sentence from §4.1 and add: "§4.4(a) is a deliberate exemption favouring interpolating syntaxes; `density.json` must publish each language's hole count and the density recomputed at 2 tokens per hole."

---

## [MAJOR] Finding 4
*Spec section:* §6.1.1, §6.1.7 ("A language must be allowed to score well for semantics Quidra does not have"); artifact §0 invariant I5

**Issue**

QUIDRA BIAS. Invariant I5 freezes the information-content reading (a) — a fact COUNTs when a universal language rule pins it at zero token cost — and justifies it as self-handicapping: "Since Quidra is a language with explicit semantic markers, reading (b) would tilt the metric toward Quidra; choosing (a) removes that tilt." The premise is only half of Quidra's profile. Per the Quidra reference, Quidra is also among the most universal-rule-pinned languages in the set: arithmetic is always overflow-checked and "wraparound is not expressible" with no release mode that disables it (00_cross_language_constraints C-1; language.md "checked arithmetic… not disabled for speed"), implicit conversion is lossless-only, definite initialization is enforced, and writable aliasing requires a visible `&`. Under reading (a) Quidra keeps its explicit-marker facts *and* harvests F04/F06/F09/F03 as free universal-rule COUNTs, while Rust and Zig lose F09 at every plain arithmetic site to the compiler-mode `LOOKUP` rule (§2.5) and C++ loses it to `INDET`. Reading (a) therefore plausibly tilts *toward* Quidra, not away from it, and the document offers no evidence either way — only the assertion.

**Required fix**

The ledger already records `basis_class`, so reading (b) is computable at zero extra cost. Add to §5.5: "`density.json` must additionally publish, for every language, `raw_density_reading_b` and `normalized_score_reading_b`, computed as the sum of `COUNT` rows with `basis_class == 'local-marker'` over the same token denominator, plus the delta in rank order between the two readings. Reading (a) remains the primary and frozen metric; reading (b) is published as a mandatory sensitivity so the direction of I5's tilt is measured rather than asserted." Replace the unsupported claim in I5 with a pointer to that published comparison.

---

## [MAJOR] Finding 5
*Spec section:* §6.1.3, §6.1.4.A; artifact §1.4 (F04 vs F10, F13), §2.5

**Issue**

QUIDRA BIAS. The document treats "the specification leaves it unspecified" inconsistently, in a direction that rewards deterministic-cleanup languages. F04 says an unspecified *value* is still a counted fact: "A form that determinately leaves a variable uninitialized therefore *counts* — the reader learns a real and important fact," and C++ `int x;` is `COUNT`. F13 and F10 take the opposite view of the identical structure: Java `Object o = new Object();` is `INDET`, Swift `let o = Obj()` under ARC is `INDET`, Python `c = 0` is `INDET` for F10 — even though "reclamation timing is not determined by the local form" is itself a determinate universal rule and therefore a `COUNT` under I5(a). With S6 scope-exit sites present in essentially every probe, this costs the six GC/ARC languages one to two facts per probe and gives them to C++, Rust, Zig — and to Quidra, whose "deterministic managed ownership with explicit retain/release" and scope-unpinned references (architecture.md) look like COUNT under these rules, while the one worked precedent for reference counting (Swift ARC) is marked INDET. Nothing binds the annotator to adjudicate Quidra's retain/release the way Swift's ARC was adjudicated, and I1 has Quidra annotated "last," after the other nine counts are known. The document's own §6.4 no-bias demonstration is fragile to this: flipping Python's four INDET rows moves it from 10/6 = 1.667 to 14/6 = 2.333, past Go.

**Required fix**

Add one frozen rule to §2.5: "Where a language universally specifies that an outcome is unspecified or non-deterministic, the row is `COUNT` with the value naming that rule (e.g. 'no deterministic release point; reclamation by GC at an unspecified time'), `basis_class = universal-rule`. `INDET` is reserved for undefined/erroneous behaviour and for runtime-data-dependent properties. This reading is applied identically to F04, F10 and F13." (Or freeze the opposite reading and change F04's C++/Zig rows to `INDET` — either is defensible, one must be chosen.) Additionally, add to F13's example table a Quidra-shaped row adjudicated in advance — "reference-counted / managed retain-release reclamation, Swift ARC and Quidra managed ownership: same disposition" — and require 100% (not 10%) two-analyst re-annotation of Quidra pairs, since Quidra is annotated last.

---

## [MAJOR] Finding 6
*Spec section:* §6.1.3, §6.1.4.A; artifact §1.1 E2, §3.1 A3.1

**Issue**

MECHANICAL REPRODUCIBILITY. E2 makes a call one S2 site and A3.1 caps it at one row per (site, fact kind), so a call with several arguments cannot record its argument-passing facts. `f(a, b, c)` where `a` is by value, `b` is a writable alias and `c` is implicitly converted has exactly one F03 row and one F06 row available, and the document never says whose answer goes in them, nor whether the row is COUNT when one argument is determinate and another needs the callee signature. Two analysts will produce different counts on every multi-argument call, and multi-argument calls are pervasive in the family 18/19/20 probes. The same gap affects composite/struct literals with several fields and multi-declarator statements.

**Required fix**

Add to §1.1: "**E7 — Argument sub-sites.** Each argument position of a call, each field initializer of a composite literal, and each declarator of a multi-declarator statement is its own S2 sub-site, identified `s<n>.a<k>`, bearing its own rows under the same matrix and gates. The call itself retains S2 rows for its result and its control/effect facts only." If sub-sites are rejected, instead state: "A row whose value differs across argument positions records the tuple of per-position answers and is `COUNT` only if every position is determinate; otherwise it takes the worst position's disposition."

---

## [MAJOR] Finding 7
*Spec section:* §6.1.7, §32 (anti-fabrication); artifact §0 invariant I4, §1.4 F07/F11/F12, §6.1

**Issue**

MECHANICAL REPRODUCIBILITY. Absence facts — "cannot fail" (F07), "no externally visible effect" (F11), "returns normally; cannot panic or diverge" (F12) — have no adjudication rule, yet they supply four of the eight COUNT rows the document's own worked example awards to a single stdlib call site (§6.1, s4). All four are tagged `basis_class = stdlib-contract`, but the cited contract (`core::num::i32::wrapping_add`) states only the wrapping result; it does not state no-panic, no-divergence or no-external-effect. That violates I4, which requires every counted fact to cite a clause that supports its value. Because §2.2 admits any stdlib name at `hops_stdlib = 1`, an idiomatic fragment written with more stdlib calls scores strictly higher density than the same task written with operators — up to eight facts for four or five tokens — and 01's R2_idiomatic does not pin which form the fragment author must use.

**Required fix**

Add to §1.4 before F01: "**Absence facts.** A row whose value asserts the absence of a behaviour (`cannot fail`, `no externally visible effect`, `returns normally`, `no allocation`) is `COUNT` only where the cited language specification clause or library contract states the absence, or where a universal language rule entails it. Where the absence is merely not mentioned, the row is `LOOKUP` with `hops_decl = 1` (the callee's definition)." Then re-adjudicate the §6.1 example accordingly and re-derive its totals. Separately, add to §5.1: "Where a language offers both an operator form and a standard-library call for the same frozen probe semantics, the probe document names the form to be measured before annotation begins."

---

## [MAJOR] Finding 8
*Spec section:* §6.1.5, §26, §27.1; artifact §5.3

**Issue**

FAIRNESS ACROSS ALL TEN. §5.3 excludes an unsupported probe from both the fact sum and the token sum. That is defensible for the overall score — §6.1.5 and the §6.1.6 harmonic mean apply the penalty through `C`, and the document correctly says so — but it means the *published Semantic Density row* of the §27.1 table compares numbers computed over different probe subsets with no annotation. Quidra is directly affected: the Quidra reference states "Concurrency, WASM, self-hosting… remain development areas," so the family-19 probes are likely NONE for Quidra, and 01's F20.P2 (exported C ABI with stable unmangled symbol) is constrained by Quidra's `extern` restrictions. Concurrency and FFI probes are token-heavy, hop-heavy and fact-sparse in every language, so dropping them mechanically raises the survivor's macro-ratio. Quidra's density would be computed on a smaller and systematically easier subset than Python's or Java's, and nothing in the table would say so.

**Required fix**

Add to §5.5 the required keys `probes_included` (integer) and `raw_density_common_subset` / `normalized_score_common_subset`, computed over only those probes for which all ten languages have a fragment, and add to §5.3: "The primary Semantic Density value remains the full-applicable-subset macro-ratio. The common-subset value is published beside it and the §27.1 Semantic Density row carries a footnote stating, per language, the number of probes its value was computed over."

---

## [MINOR] Finding 9
*Spec section:* §6.1.7, §32; artifact §2.7

**Issue**

MECHANICAL REPRODUCIBILITY. The §2.7 re-annotation audit is under-specified in three ways that decide numbers. (1) It never says which count enters the score after a disagreement: "the adjudication and both counts are published" leaves the scored value undefined. (2) There is no corpus-level failure criterion — if 30 of the 40 sampled pairs disagree by more than ±5%, nothing follows. (3) The re-annotation has no independence requirement beyond "without reading the first ledger"; the sibling document 03 §3.2 mandates two-analyst reconciliation for Determinacy and Locality, so the two metrics computed from what §2.3 calls the same rows are held to different process standards.

**Required fix**

Rewrite §2.7 to add: "The re-annotation is performed by a second analyst who did not produce the first ledger. Where re-adjudication finds the first annotation wrong under §1–§3, the corrected count is the scored value and the correction is applied to every non-sampled pair exhibiting the same pattern. If more than 20% of sampled pairs require re-adjudication, the full ledger is re-annotated for all ten languages before any score is computed. Quidra pairs are audited at 100%."

---

## [MINOR] Finding 10
*Spec section:* §6.1.4.A; artifact §4.2 R4, §4.6

**Issue**

MECHANICAL REPRODUCIBILITY. R4 removes a lexeme iff "(a) the language's own grammar permits omitting it at that position and (b) omitting it changes no fact *value* at any site under §1." Condition (b) is per-occurrence and semantic, but §4.6 implements it as a static per-language `optional_lexemes` list (`{"lexeme": ";", "removable": true}`), and §4.5 step 4 applies it as a blind post-pass over the token list. The two do not agree: TypeScript ASI and Swift's newline rules make `;` removability position-dependent, and condition (b) is circular anyway, since the fact annotation is performed on the token region that R4 helps define.

**Required fix**

Replace the mechanism with a normalization obligation on the fragments, which removes the circularity: add to §4.2 R6 "No fragment may contain a statement terminator its grammar permits omitting at that position; this is verified at freeze time," and change R4 to "R4 removes redundant grouping parentheses only; `optional_lexemes` is restricted to grouping parentheses." If blanket terminator removal is kept instead, state in §4.6 that `removable: true` is evaluated per occurrence by the tokenizer against the profile's ASI/newline rules and that every removal is justified in `removed_by_R4`.

---

## [MINOR] Finding 11
*Spec section:* §6.1.7; artifact §2.6, §5.5

**Issue**

MECHANICAL REPRODUCIBILITY. `basis_class` is mandatory for every COUNT row and §5.5 makes its histogram "the audit hook for invariant I5," but no rule says how to choose when more than one applies. The document's own examples show the conflict: §6.2's Go s1 F04 is justified as "explicitly initialized; also the zero-value rule" — `local-marker` and `universal-rule` both apply — and §6.1's Rust s4 rows mix `universal-rule` and `stdlib-contract` for facts a stdlib name and a language rule both pin. The histogram that the I5 sensitivity depends on is therefore annotator-dependent, and finding 4's proposed reading-(b) recomputation depends on the same field.

**Required fix**

Add to §2.6: "Where more than one basis applies, `basis_class` takes the first that applies in this fixed order: `local-marker` (a lexeme physically present at or governing the site selects the value), then `stdlib-contract` (a standard-library name present in the probe region states it), then `universal-rule`. Where a second basis independently pins the same value, it is recorded in `note`."

---

## [MINOR] Finding 12
*Spec section:* §6.1.4.C, §25.1; artifact §2.5, §1.4 F09

**Issue**

The "compiler-mode dependence is LOOKUP, not INDET" rule is faithful to §6.1.4.C, which lists "global configuration or compiler mode" as a lookup, and it is stated language-neutrally. But its measured effect is highly asymmetric and invisible in the published output: Rust and Zig lose F09 at every plain arithmetic site (`debug-assertions`/`overflow-checks`, Debug vs ReleaseFast), TypeScript loses F07 at every index (`noUncheckedIndexedAccess`), while Quidra has no such mode at all — its checked arithmetic is documented as not disabled for optimized builds. An auditor reading the density table cannot see how much of the spread this single rule produced.

**Required fix**

Add to §5.5 the required key `mode_driven_rows: {language: {fact_kind: count}}`, counting rows whose disposition is `LOOKUP` solely because of a compiler-mode dependency, and add to §2.5: "The mode-driven row counts are published with the density scores so the magnitude of this rule's effect per language is visible."

---

## [MINOR] Finding 13
*Spec section:* §6.1.2, §6.1.7; artifact §1.2

**Issue**

§1.2 claims "the set of annotated rows is identical across the 10 languages for a given probe's site structure," and relies on gates phrased as "some language in the fixed set." The qualifier makes the claim near-vacuous: site structure is exactly what differs between languages, because E2 splits a Rust `let … = c.wrapping_add(1)` into four sites where Go's `+=` yields three. An auditor reading the sentence as written will expect row-count parity across languages and find none.

**Required fix**

Replace the sentence with: "Gates depend only on the syntactic shape of the site and never on the language under annotation, so two sites of the same class and shape are annotated with the same rows in every language. Site *counts* legitimately differ between languages, because the number of constructs the language requires to express the probe is part of what the metric measures; `density.json` publishes each language's site count per probe so the difference is visible."

---
