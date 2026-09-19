# Audit findings for `10_intrinsic_design.md`

**Verdict:** needs_fix  
**Quidra bias found:** True  

## [BLOCKER] Finding 1
*Spec section:* §10.5 I1/I2/I3/I4, §10.1

**Issue**

THE CENTRAL DEFECT: in I1, I2, I3 and I4 a submission that ignores the transformation entirely and emits ordinary, familiar real source still PASSES. §6.1 states the inverse transformer 'treats any WORD equal to a mapped pseudo-word as a role token and any other WORD as an identifier', so a real `if`/`for`/`return` written from memory passes through the inverse unchanged and compiles. Identically for I3: the inverse maps `@ka`→`(`, and a literal `(` the model wrote is passed through, so real-shaped source builds and satisfies the §4.2 oracle. For I4 the overlay is semantics-neutral by construction (§7.4), so ignoring it changes nothing observable. The only penalty is one hallucination event (8%) plus a diluted item inside Specification Compliance (10%). That leaves 65% of CTES (Compile 7 + Correct@1 18 + Correct@N 8 + TPR 15 + Silent-bug 10 + Unseen 7) obtainable purely from pretraining recall — the exact advantage the track exists to remove. I6 is the only condition structurally immune (its transformed alphabet IS the real alphabet, so π⁻¹ scrambles a prior-role program into a build failure), which is why §7.7 can define F1_PRIOR at all; I1–I4 have no such immunity and none is added.

**Required fix**

Add a frozen §4.2a 'Transformed-source conformance gate', applied to the model's raw pre-inverse submission at every turn, before inverse mapping (which §10.5 I4 already mandates for the overlay): I1/I2 — zero H_REAL events (no WORD token of the submission is a reserved word of the real language, other than tokens the pack documents untransformed); I3 — zero H_STRUCT_REAL events (no original token of a perturbed S-dimension appears); I4 — zero overlay-checker violations; I5 — zero Rule-C violations (A and B are already oracle-visible); I6 — zero prior-role occurrences per the §7.7 role-position audit. Write: 'A submission that fails the gate is FAIL for that task at that turn regardless of its output; the gate verdict is recorded per turn, the submission is preserved, and the repair prompt states which described rule was violated in language-neutral prose without revealing the real spelling.' Add a check unit `CONFORMANT` to every transformed condition's check-unit list so Test Pass Rate is also sensitive. Keep the hallucination metrics as separate diagnostics.

---

## [BLOCKER] Finding 2
*Spec section:* §10.5 I4, §10.6

**Issue**

I4 carries 20% of the Intrinsic score but is ~98% insensitive to whether the novel rule was acquired. §7.4 routes the overlay checker's verdict into Specification Compliance (10% weight), where §11.3.7 makes it ONE checklist item among 'output-line-count; output-field-format per line class; presence of a required declaration kind; absence of constructs the task forbids' summed across three tasks. With ~5 items per task × 3 tasks, a model that ignores N-ALPHA/N-BETA completely loses roughly 3/15 of 10% ≈ 2 CTES points and keeps the other 98. 'Novel-rule Generalization' would therefore report almost exactly ordinary correctness.

**Required fix**

Two changes. (1) Make overlay validation a precondition of the I4 oracle (see the conformance gate above). (2) Replace the single checklist item with a rate: 'Overlay Compliance = compliant sites / applicable sites, where an applicable site is each function definition (A1/B1), each named-value introduction (A2) and each loop control variable (B2); for I4 the task checklist is 50% ordinary items and 50% Overlay Compliance.' Also fix the cross-reference: §7.4 cites §11.4 (hallucination events) where it means §11.3.7 (Specification Compliance).

---

## [BLOCKER] Finding 3
*Spec section:* §7.3, §10.4

**Issue**

The reserved structural marker `@` is not available. §7.3 asserts '`@` can start nothing else' and PF-10 requires zero `@` in every reference solution in all ten languages — but Zig 0.16, whose stdout path this run pins to `std.Io` (methodology 00, C-4), reaches it via `@import("std")`, and `@intCast`/`@as` are ordinary Zig builtins. Zig's reference solutions therefore fail PF-10 by construction and the whole run is blocked; worse, the frozen transformed-lexer rule ('`@` followed by exactly two lowercase letters, terminating after the second') lexes `@import` as `@im` + `port`, silently corrupting Zig source. Java/Kotlin annotations (`@Override`), Swift attributes (`@main`, `@available`) and TypeScript decorators are further collisions if any pack example needs them.

**Required fix**

Replace the fixed marker with a verified one: 'The structural marker is the first entry of the frozen priority list `%%`, `~~`, `¤` for which PF-10 finds zero occurrences in every task statement, reference solution, expected output and pack example in all ten languages, and which is not a lexical prefix in any of the ten grammars; the selected marker is published in preflight_report.json and used identically in all ten. `@` is excluded a priori because Zig builtins (`@import`, `@intCast`) and Java/Kotlin/Swift/TypeScript attribute syntax use it.' Restate the maximal-munch rule against the selected marker.

---

## [BLOCKER] Finding 4
*Spec section:* §7.7, §10.4, §32

**Issue**

I6's one permuted operator pair — exchanging strict-less-than with strict-greater-than — is defended as safe because the two 'share a precedence class and an arity'. That is false for delimiter use: `<` and `>` are generic-argument brackets in C++, Rust, Java, Kotlin and TypeScript, and lex10 is explicitly a lexer, not a parser (§6.1), so the forward transformer cannot restrict the swap to comparison sites. `Vec<i64>` becomes `Vec>i64<`, `List<Rec>` becomes `List>Rec<`, and the model must reproduce that to be inverse-mapped correctly. Quidra, Go, Swift, Python and Zig express the same task-set types without angle brackets (`Rec[]`, `[]Rec`, `[Rec]`, `std.ArrayList(Rec)`), so five languages absorb a large extra burden that five — including Quidra — escape entirely. This is the clearest Quidra-favouring asymmetry in the document and it is also a §10.4 hazard (the transformed grammar is not describable within the pack budget).

**Required fix**

Exchange `<=` with `>=` instead of `<` with `>`. They have the same precedence class and arity, neither is ever a delimiter in any of the ten languages, and maximal munch already keeps `<=>`/`<<`/`>>` intact. Write: 'the non-strict less-than-or-equal and greater-than-or-equal operators are exchanged; the strict `<`/`>` pair is deliberately NOT permuted because it also delimits generic arguments in five of the ten languages and a lexer cannot separate the two uses.' Add a PF-09-style check that the chosen pair is a comparison operator in all ten binding tables and appears in no delimiter position.

---

## [BLOCKER] Finding 5
*Spec section:* §10.5 I4 ('impose equivalent reasoning difficulty across languages')

**Issue**

A1, B1 and Rule C say 'every function the program defines' must carry a sigil, with no exemption for the entry point. In Java the entry point must be named `main` (and Kotlin, Swift, C++, Rust, Go, Zig likewise fix the entry-point name); renaming it `main_ka` makes the program unbuildable or unrunnable, while H3/H4 only adjust file names and wrap top-level code. Python and TypeScript need no named entry point and are unaffected. As written, I4 and I5 are either unbuildable for up to eight languages or require the model to guess an unstated exemption — in a track whose §9.1 principle is that unstated conventions are the harness's responsibility. A2 has the same problem for a fixed-signature parameter such as Java's `String[] args`.

**Required fix**

Add to §7.4 and §7.6, and to P11 of every pack in identical wording for all ten languages: 'The compilation unit's required entry point, and any parameter whose name or signature the toolchain fixes, are exempt from A1, A2, B1, B2 and Rule C. Every other function and named value the program defines is subject to them.' Record the entry-point token per language in the binding table (`entry_point_name`) so the checker applies the exemption mechanically rather than by inspection.

---

## [BLOCKER] Finding 6
*Spec section:* §5.5, §6.4 R1

**Issue**

The emission rule ('surrounded by the whitespace that was already there, and if there was none, by a single space') inserts a leading space when a bound token begins a line at column 0 — the preceding token is NEWLINE, not whitespace. For Python that turns `def f():` into ` <pw> f():` and produces an IndentationError; Quidra is likewise indentation-significant (its lexer emits Indent/Dedent tokens and it has no block delimiters or statement terminator). Separately, R1 requires `canon(inverse(forward(F)))` to be byte-identical to `canon(F_nocomments)` while canon only 'collapses runs of spaces to one' — it cannot delete an inserted space, so any real source containing `if(`, `return(`, `while(` fails R1 and blocks the run at PF-02, while a fixture author can dodge it by never writing that form.

**Required fix**

Amend §5.5: 'No whitespace is inserted when the preceding or following token is NEWLINE or INDENT_RUN, nor anywhere inside a leading-whitespace run in a language whose profile sets indentation_significant.' Amend §6.4: 'The forward transformer records every inserted whitespace character in the unit position map (the same map §8.2 uses for diagnostics); the inverse deletes exactly those insertions. R1 is then plain byte-identity against F_nocomments with no canon relaxation for spacing.'

---

## [MAJOR] Finding 7
*Spec section:* §10.5 I1, and the artifact's own fairness preamble

**Issue**

The preamble claims the design anonymizes 'all word keywords a language uses rather than a fixed quota', but §3.1/§7.1 anonymize only the 27 frozen K-roles. Every other real keyword survives untouched and remains usable: Rust `loop`/`match`/`as`, Kotlin `when`, Swift `guard`/`repeat`, C++ `switch`/`auto`/`static`/`const`/`new`, Go `switch`, TypeScript ternaries, Java `new`. PF-03 does not remove them — it routes them into a hand-authored per-language `unbound_word_tokens` array 'with a reason'. That list is an unbounded, judgement-based escape hatch whose size determines how much familiar surface each language keeps, and it is neither published nor bounded. Quidra's reserved set is 21 words (class, override, import, super, const, return, if, elif, else, while, for, in, match, try, break, continue, true, false, not, and, or), most of which map onto K-roles, so Quidra has few escape hatches while C++/Java/Rust have many — direction here is not pro-Quidra, but the mechanism is arbitrary either way.

**Required fix**

Extend the I1/I2 domain: 'Every token that appears in the language's frozen reserved-word list AND occurs in that language's reference solutions or pack examples is anonymized, whether or not it maps to a K-role. Non-role reserved words receive role-token key `U:<token>` and are documented in P11 with language-neutral prose for their job. `unbound_word_tokens` must therefore be empty; a non-empty array blocks the run at PF-03.' This makes `anonymized_token_count` a mechanically derived quantity, removes the escape hatch, and makes the preamble's claim true.

---

## [MAJOR] Finding 8
*Spec section:* §10.2, §10.4, §6.1.7 (audit-publication analogue)

**Issue**

The document is labelled FROZEN, but every artifact that actually determines the numbers is deferred to files that do not yet exist: `role_bindings.json` (the single largest driver of per-language difficulty), all ten `lex_profiles.json` entries (operator tables, number patterns), `tasks/<task>/compliance.json` (10% weight), the T2 twelve-entry table and T4 word line, `leak_terms.txt`, `stderr_allow.json`, `pack_template.md`, and the I5 example variants. Only the three word lists get a SHA-256. Two independent analysts will bind K21a to `int` vs `long long` vs `int64_t` vs `auto`, will disagree on whether Rust `mut` is K03 or K24, and will produce different anonymization loads and different scores. §3.4's tie-break ('a role may bind more than one token only when the language genuinely requires distinct spellings') is itself a judgement call.

**Required fix**

(1) Add: 'Every file under config/ is frozen before PF-01 and its SHA-256 is recorded in preflight_report.json; the scoring pipeline refuses to run if any recorded hash changes.' (2) Make binding mechanical and citable: 'A token is BOUND to role R in language L only if it occurs in L's reference solutions or pack examples and is either listed in L's normative reserved-word list or named as the spelling for that role in L's normative reference; each binding records the document and section it is taken from. Where a language offers several spellings for one role, the reference solution uses the one named first in that citation, and the alternatives are bound as additional tokens of the same role.' (3) Freeze a reference-solution style rule (e.g. 'explicit type annotations are written wherever the language permits them') so type-inference idioms cannot silently shrink one language's anonymization load.

---

## [MAJOR] Finding 9
*Spec section:* §2.6, §10.1, §13.2

**Issue**

§2.6 bullet 3 rejects any pack containing 'a package path that identifies the ecosystem', with no 'outside a code example' qualifier. But §7.1 leaves all V-roles real in I1, so every I1 pack's P8/P9 must spell real standard-library names and namespace paths (`System.out.println`, `std::cout`, `java.util`, `std.ArrayList`). PF-07 would therefore reject every I1 pack and block the run. The same fact makes §13.2's residual inaccurate: it says I1 and I2 both 'remove the word-level surface', when I1 leaves the entire library surface — the most identity-revealing part of the pack — intact.

**Required fix**

Amend §2.6 bullet 3: '…that is not itself a transformed token. In I1 the V-role spellings and the standard namespace path are untransformed by design (§7.1) and are exempt from this bullet; the resulting identity exposure is recorded as an anonymity residual.' Amend §13.2 to state plainly: 'I1 anonymizes grammar words only. Standard-library names and namespace paths remain real, so I1 packs are substantially more recognizable than I2 packs, and the I1 row of the §28.1 table must carry that statement explicitly.'

---

## [MAJOR] Finding 10
*Spec section:* §25.1 family C, §25.2, §6.2

**Issue**

Two token-counting defects. (a) §2.4/§5.4/§11.3.11-12 specify 'model-tokenizer tokens, characters if unavailable', but the sibling FROZEN doc 09 (same run, same spec §6.2) states the model tokenizer is not exposed by this client for arbitrary strings and freezes a lexical source tokenizer that 'governs both tracks' — so this document contradicts a frozen sibling on a scored metric and its fallback (characters) is not the frozen fallback (the lexical counter). (b) §11.3.11 does not say whether Source Token Efficiency counts the transformed source the model wrote (full of 6-character pseudo-words) or the inverse-mapped real source that is compiled; the two differ systematically by anonymized_token_count, which differs per language.

**Required fix**

(a) Replace with: 'Source Token Efficiency is counted by the frozen lexical tokenizer of 09_llm_run_config §source_token_counting, whose SHA-256 is recorded; harness-reported prompt/completion counts are published beside it and never substituted for it. Pack budgets and pseudo-word residuals are reported in characters and in lexical tokens.' (b) Add: 'Source Token Efficiency is computed on the inverse-mapped source actually compiled, for every condition; the transformed-source count is published as a residual.'

---

## [MAJOR] Finding 11
*Spec section:* §25.2

**Issue**

§11.3.11 takes the median over 'the unit's passing tasks' and then sets family-C `best_positive_raw` to the minimum across the ten languages. The languages being compared have different passing-task sets, so a language that passes only the short task T3 is compared against a language that passes all five — and wins Source Token Efficiency for failing more. §25.2 requires normalizing 'at the lowest comparable workload level first'.

**Required fix**

Rewrite: 'Source Token Efficiency is normalized per task: for each (condition, replicate index, task), best_positive_raw is the minimum final-source token count among the languages that passed that task; a language's unit score is the unweighted mean of its per-task normalized scores over the tasks it passed. If it passed none, the metric is N/A with reason NO_PASSING_SOURCE.' Apply the same per-task normalization to Total Token Efficiency.

---

## [MAJOR] Finding 12
*Spec section:* §10.5 I4, §10.4 requirement 3

**Issue**

N-BETA is not mechanically decidable as written. B2 keys on 'loop control variable' and on whether its 'iteration count is a compile-time constant of the program' — that needs dataflow, not the four lexical site kinds §7.4 promises, and it is not even well-posed in languages whose idiomatic loop has no numeric control variable (`for x in xs` in Python/TS/Swift/Kotlin/Quidra vs `for(int i=0;i<8;i++)` in C++/Java/Go). A2 has a parallel hole: 'assigned a new value at least once' never says whether `i++`, `+=` or the implicit rebinding of an induction variable counts, and the answer decides the sigil for the most common variable in every task — differently per language idiom, which is exactly the 'equivalent reasoning difficulty' §10.5 I4 forbids breaking.

**Required fix**

Add to §7.4: 'A loop control variable is the identifier introduced at a bounded-iteration site in the position the language's binding table records as `loop_var_position`; a conditional-iteration (K08) loop has none and is out of scope for B2. Its iteration count is compile-time constant iff every operand of its bound expression is an integer literal or an identifier whose only assignments in the program are from literal-only expressions.' And: 'An assignment site is an occurrence of the S7 simple-assignment operator, of any compound-assignment or increment/decrement operator listed in the language's binding table under `assignment_site_forms`, or the implicit rebinding of a bounded-iteration control variable, which counts as reassignment in all ten languages.'

---

## [MAJOR] Finding 13
*Spec section:* §9.1, §10.4 requirement 2

**Issue**

§9.1 states the pack 'states everything about the language' and the harness carries only facts the pack withholds — but H2, H3, H4 and H5 supply things the pack does state. P1 ('where the entry point goes') documents the entry-point scaffold that H4 adds; P9 documents imports; P10 plus role K27 document the error-propagation marker that H5 adds — and in I1 that marker is an anonymized pseudo-word the model was supposed to learn. So a model that fails to apply a documented, anonymized rule is silently rescued, but only in the languages that need scaffolding, while a model that fails to apply the anonymized `if` is not. PF-13's 'the fixup set is extended until it does [pass]' makes this open-ended.

**Required fix**

Add to §9.3: 'No fixup may supply a token, declaration or scaffold that the pack documents. H2–H5 apply only to material absent from the pack; if a language's pack documents its entry-point shape, package declaration or error-propagation marker, its absence from a submission is a model failure and the trial proceeds to build as submitted. PF-13's bare-body fixture is constructed from the pack, so it contains every pack-documented element and omits only unstated conventions (file name, output directory, jar name).' Publish per-language fixup counts split into pack-documented (must be zero) and unstated.

---

## [MAJOR] Finding 14
*Spec section:* §11.3.7, §10.4 requirement 3

**Issue**

Specification Compliance is 10% of every condition, yet its checklist kinds include predicates that lex10 cannot decide and that the document never defines per language: 'presence of a required declaration kind (a record type for T2, a nested sequence for T5)'. A nested sequence is `[[...]]` in Python (no declaration at all), `[][]int` in Go, `Vec<Vec<i64>>` in Rust, `Rec[]`-style in Quidra/Java/TypeScript; 'a record type' is `class` in Java/Kotlin/Quidra, `struct` in C++/Rust/Go/Swift/Zig, `@dataclass` or a tuple in Python. PF-05 only tests that whatever is implemented is non-vacuous; it does not make the rule reproducible.

**Required fix**

Freeze the predicates per language in the binding table and cite them from §11.3.7: 'A required-declaration item is satisfied iff the token bound to the relevant role (K12 for a record type) occurs at a declaration site, per the per-language predicate recorded in role_bindings.json under `declaration_predicates`; a nested-sequence item is satisfied iff the language's `nested_sequence_predicate` matches. Both predicates are frozen before PF-05, which must exercise each with a positive and a mutated negative fixture in every one of the ten languages.'

---

## [MAJOR] Finding 15
*Spec section:* §11.4, §10.6

**Issue**

H_API (the only hallucination event for I0/I4/I5, 8% weight) is 'determined from the build diagnostic class (unresolved name / unknown member)'. For Python the build step is `python3 -m py_compile`, which never reports unresolved names — so Python can essentially never register a hallucination event and scores ~100 on Hallucination Resistance in I0, I4 and I5 by construction. TypeScript's coverage is partial; the compiled languages are fully exposed. No frozen mapping from toolchain diagnostics to the two classes exists for any language.

**Required fix**

Add a frozen `config/diagnostic_classes.json` with per-language regexes for the classes UNRESOLVED_NAME and UNKNOWN_MEMBER, covering both build and run output, and rewrite the H_API definition as: 'an unresolved-name or unknown-member diagnostic from the build step, or the equivalent runtime diagnostic class (e.g. NameError/AttributeError on stderr) for languages whose build step does not perform name resolution.' Require PF-05 to exercise both a positive and a negative per language.

---

## [MAJOR] Finding 16
*Spec section:* §10.5 I3

**Issue**

I3 matches the budget in dimensions, not in sites. Five S-dimensions cost 5 SPP everywhere, but perturbing S2 rewrites two token types in a brace language and one in a colon language (and Quidra has neither — it uses Indent/Dedent with no block introducer, so S2 will fall out of the eligible set for all ten at PF-09 and all three frozen sets will need the fallback). S5 hits array *type* syntax in Go/Java/TypeScript/Swift/Quidra and only indexing elsewhere. The realized number of perturbed characters can differ by an order of magnitude between Python/Quidra and C++/Rust, and no residual records it. Two further ambiguities: S1 is described as 'call argument lists and expression precedence' though a lexer must perturb every parenthesis (definitions, control-flow headers, casts), and nothing says whether compound assignments (`+=`, `-=`) belong to S7.

**Required fix**

Add to §7.3: 'SPP matches dimensions, not occurrences. transforms/I3/<set>/manifest.json publishes, per language, the count of perturbed sites and the sites-per-100-tokens density in that language's reference solutions, and the results narrative states that the budget is matched by axis and not by density.' And: 'A perturbed dimension replaces EVERY occurrence of its token, whatever syntactic role it plays — parentheses in definitions, control-flow headers and grouping alike, brackets in type syntax as well as indexing — and P11 says so in the same sentence for all ten languages. Compound assignment operators are distinct operators, are not part of S7, and are never perturbed.'

---

## [MAJOR] Finding 17
*Spec section:* §10.2, §10.5 I2

**Issue**

The twelve-section pack template has no slot for value interpolation inside a text literal, which Quidra, Python, Kotlin, Swift, TypeScript, Rust and C# all provide (Quidra's own documentation shows `print("count: {count}")` as the idiomatic output form). Two consequences. (1) If interpolation is undocumented, those languages are described unidiomatically; if it is squeezed into P2, the slot set is no longer identical across the ten. (2) §1 consequence 2 forbids transforming literals, so in I2 an interpolating language routes integer-to-text (V10) and concatenation (V11) inside an untransformed string literal and never has to learn their anonymized spellings, while C++, Java, Go and Zig must use the pseudo-worded facilities. I2's anonymization pressure is therefore materially lighter for Quidra and the other interpolating languages, and no residual records it.

**Required fix**

Add a fixed line to slot P2 of the template, present in all ten packs: 'Embedding a value inside a text literal: <spelling>, or (this language has no construct in this category).' Add `V18` to the §3.2 inventory ('embed a value's text form inside a text literal, when the language provides one'), mark it NOT_TRANSFORMABLE with the reason 'its syntax lies inside a literal, which §1 never transforms', and require §13.1 to publish, per language, which bound V-roles the pack's interpolation form makes optional in I2.

---

## [MAJOR] Finding 18
*Spec section:* §10.5 I5, §10.4 requirement 2

**Issue**

I5 Rule B is stated as 'after the last line of a group of related lines, write one additional line total=<k>', but 'group' is never defined — not in Rule B, not in C1/C2/C3, and not in the §4.4 task statements as summarized. The expected outputs are frozen from reference solutions, so the harness knows the intended grouping while the model must guess an unstated convention. That is precisely what §9.1 forbids and what §10.4 requirement 2 calls a withheld fact, and it sits under 15% of the Intrinsic score.

**Required fix**

Either define the grouping in Rule B itself ('a group is the maximal run of lines the task statement names as one group; a program with a single group writes exactly one summary line, after its last line'), or require each of C1, C2 and C3 to name its groups verbatim in the task statement ('the ten triangular-number lines form one group'). Freeze the chosen wording in config/tasks/ and verify at PF-06 that no task leaves a grouping unstated.

---

## [MINOR] Finding 19
*Spec section:* §8.2, §10.4

**Issue**

Scrubbed diagnostics may point at harness-inserted scaffolding (H2–H5 lines the model never wrote). §8.2 step 2 only strips coordinates when they cannot be mapped, so the model receives a real error about code it did not author, with no location — and disproportionately in the languages that need the most scaffolding, whose Repair Success and Repair Efficiency then measure harness noise.

**Required fix**

Amend step 2: 'A diagnostic whose primary position falls inside harness-inserted scaffolding is suppressed entirely rather than emitted without coordinates, the suppression is logged, and the per-language suppression rate is published in raw/residuals.json beside the wholesale-replacement rate.'

---

## [MINOR] Finding 20
*Spec section:* §10.3, §10.5 I4/I6

**Issue**

Seed counts sit at the spec's minimum and two conditions may be below it. §10.3 requires '≥5 independent seeds for each lexical-randomization condition'; I4 draws its sigil alphabets with draw_pair from 2 seeds per rule set and I6 draws its permutation from 3 seeds, so both are randomized lexical conditions running under five. Separately, §7.4 and §7.6 require 'five distinct two-letter codes' but the drawing procedure (draw_pair keyed per k) has no redraw rule, so two arities can collide and make the sigil ambiguous.

**Required fix**

State the reading explicitly ('I4 and I6 are governed by the subtest-specific sentences of §10.5, which require multiple rule sets and multiple mappings rather than five seeds; §10.3's five-seed rule binds the pseudo-word conditions I1 and I2'), or raise I6 to 5 mappings and I4 to 3 seeds per rule set. Add: 'Codes are drawn with the §5.3 rejection loop against the already-used set; redraws are recorded in the manifest as collision_redraws.'

---

## [MINOR] Finding 21
*Spec section:* §6.2, §10.2

**Issue**

Several §6.2 fields the artifact inherits are never restated or given a frozen outcome: the 16,384-token per-generation and 65,536-token cumulative output caps, and the separation of provider/transport failures from language failures. §11.3.12 counts tokens but nothing says what happens when a trial hits a cap, so an exhausted trial's classification is undefined.

**Required fix**

Add to §0.3: 'Output caps are 16,384 tokens per generation and 65,536 cumulative per trial, per §6.2. A trial that exhausts either cap is terminated, scored FAIL with reason TOKEN_BUDGET_EXHAUSTED — never N/A — and the event is published per language. Provider, rate-limit and transport failures are retried per 09_llm_run_config and are recorded in _deviations.json separately from model failures; they are never converted into incorrect generations.'

---

## [MINOR] Finding 22
*Spec section:* §10.6, §21

**Issue**

Unseen-case Generalization (7%) is the pass rate on a single task (T5, or C3 for COMPOSITION) at turn 0 with one trial per cell, so each replicate contributes only 0 or 100 and a five-seed subtest score is quantized to multiples of 20. The same task is also counted in Correct@1, Correct@N and Test Pass Rate, so one coin flip moves roughly a quarter of CTES. §2.4's P1–P11 floor of 2,400 tokens is likewise stated with no defined consequence for a pack that falls below it.

**Required fix**

Either designate two unseen-case tasks per task set (add a second CORE task exercising a different underived combination, e.g. a function taking a function-valued parameter) or state in §13.4: 'Unseen-case Generalization is a single binary observation per replicate; per-language values are quantized to 1/(number of replicates) and differences below that step are not resolved.' For §2.4 add: 'A pack below the floor is never padded; the floor is a PF-06 review trigger that every slot was actually filled.'

---
