# Audit findings for `09_llm_run_config.json`

**Verdict:** needs_fix  
**Quidra bias found:** True  

## [BLOCKER] Finding 1
*Spec section:* 32 (outsourcing core computation for one language); run constraint C-6 in methodology/00_cross_language_constraints.md; spec 15

**Issue**

The LightGrad tasks (PE-P5, PE-P6) do not carry the run's own frozen C-6 rule. `grep -n "neural\|C-6\|autodiff" 09_llm_run_config.json` returns exactly one hit, in a workload_summary. C-6 states that Quidra's standard namespaces include `neural` (a built-in reverse-mode autodiff facility: `neural.track`, `neural.grad`, `Parameter`, `Gradients`) and that every language must hand-build the tape. But this document's frozen system prompt, rule 2, says only 'Use only the target language's standard library and the toolchain as it is installed on this machine.' Under the frozen prompt text Quidra's trial agent is explicitly PERMITTED to call its built-in autodiff on the one workload whose stated purpose (spec 15) is measuring how well a language expresses the construction of such an engine, while the other nine must implement it. The PE-P5 oracle (forward values to 6dp, gradients to 6dp, 'ERROR:SHAPE') would not detect the substitution: a built-in engine produces the same first-order gradients. C-6's own conformance argument (the `mul(h,h)` double-visit case pinned by methodology 07 section 4.4) is never referenced by PE-P5's oracle or compliance checklist, so nothing in the scored pipeline enforces it.

**Required fix**

Add to the PE-P5/PE-P6 task entries a `prohibited_facilities` block reproducing C-6 verbatim for all ten languages (Quidra: the entire `neural` namespace and the `dnn` package; Python: torch/jax/autograd/tensorflow/numpy; all others: any autodiff, tensor or linear-algebra library, first- or third-party). Add a fixed compliance item 'implemented the differentiation engine in the workload source using general-purpose facilities only' to PE-P5/PE-P6's checklist, checked by scripts/check_compliance.py against a per-language frozen banned-symbol list. Add the `mul(h,h)` double-visit case from methodology 07 section 4.4 to the PE-P5 oracle_schema as a scored field, so a delegated engine fails numerically and not only by inspection. Amend system-prompt rule 2 to read '...standard library and the toolchain as installed, EXCEPT the facilities listed as prohibited in your task specification.'

---

## [BLOCKER] Finding 2
*Spec section:* 6.2 (infrastructure failures must be recorded separately from language/model failures)

**Issue**

`agent_timeout` (initial generation exceeding 1800 s) and `context_exhausted` are both listed in `classified_as_infrastructure`, are retried up to 3 times from a fresh context, and 'never converted into a generation failure or an incorrect generation'; if all retries fail the cell is marked N/A with reason `infrastructure_exhausted` rather than scored. A model that burns 30 minutes of wall-clock, or fills its context, while floundering in a language it does not know is exhibiting a model/language outcome, not a provider or transport fault. Quidra is by construction the language the model has least prior exposure to, so it is the language most likely to hit both conditions. As written, Quidra's hardest failures are the ones most likely to be deleted from the denominator instead of scored. Spec 6.2 separates *provider/network/transport/infrastructure* failure from language/model failure; agent self-exhaustion is on the wrong side of that line.

**Required fix**

Split the two classes mechanically from logs/timeline.jsonl and logs/agent_tool_calls.jsonl. (a) If the transcript shows no model turn or tool call completing in the last 300 s before the deadline (a provider or transport stall), keep it as infrastructure and retry. (b) Otherwise the model was actively generating or tool-calling until the limit: classify the trial from whatever exists at `<trial>/generated/` at the deadline using the ordinary ladder (NO_ARTIFACT if no file), score it, and record subclass `agent_budget_exhausted`. Same rule for `context_exhausted`. Publish per-language counts of (a) and (b) separately in results/llm_infrastructure_events.md, and state that (b) counts entered the metrics.

---

## [BLOCKER] Finding 3
*Spec section:* 6.2 ('write an immutable run configuration'); 25.4; 32

**Issue**

The document is declared frozen but does not actually freeze the inputs it depends on. `llm-practical/tasks/` is empty on disk: no task_spec.md, golden/expected_stdout.txt, tolerance.json, oracle_schema.json, workload.json or subset.json exists, and no SHA-256 for any of them appears anywhere in this document. Six of the twelve UG cases (UG-05, UG-06, UG-07, UG-08, UG-11, UG-12) are defined only by reference to values 'given in task_spec.md'; UG-01's 'the 10th value of the frozen LCG' does not say whether that is the 10th state or the 10th unit float; the E6 example role is 'a fixed, specified failing operation' with the operation never specified — and the choice of operation decides whether C++, Swift and Zig can even express the role. An independent analyst holding this document cannot reproduce a single UG number or author a single example. Readiness item C2 says hashes will be 'recorded' but names no location and no value.

**Required fix**

Add a top-level `frozen_input_digests` object filled before the first scored trial, mapping every dependent file to its SHA-256: per task_spec.md, golden/expected_stdout.txt, golden/provenance.json, tolerance.json, oracle_schema.json, workload.json/subset.json, every pack file per language, and scripts/classify_version.py, scripts/check_compliance.py and the governing tokenizer. State that the run is invalid if any digest changes. Separately, inline into this document the literal fixed inputs of UG-05 through UG-12 and the exact operation E6 must attempt (choose one expressible with normal facilities in all ten — e.g. parsing the text '12x' as base-10 — and say so), and disambiguate UG-01 as 'the 10th state produced by the recurrence, i.e. state_10 with state_0 = 1'.

---

## [MAJOR] Finding 4
*Spec section:* 10.4 Mandatory pre-flight validation, item 3 ('every validator can both pass and fail'); 9 (Syntax Hallucination Resistance)

**Issue**

The third leg of the mandated pre-flight triad is absent. Readiness items C1-C10 verify that examples build (leg 1) and that a dry run completes (partial leg 2), but nothing requires that any validator be shown to both pass and fail. This bites hardest on the H1/H2 and runtime-error signature lists, which drive Syntax Hallucination Resistance (7%) and the SILENT_BUG/OUTPUT_MISMATCH split feeding Silent Bug Resistance (12%). Quidra's lists are unanchored English substrings ('unknown identifier', 'undefined name', 'no such member', 'unknown module', 'unresolved', 'parse error', 'syntax error', 'unexpected token') asserted to have been 'written from each toolchain's documented fault output' with no evidence preserved. If Quidra 0.2.0 actually emits, say, `error[E_NAME]: cannot resolve name 'x'`, only 'unresolved' happens to match; if it emits `cannot find name 'x'`, none match and Quidra records zero hallucinations and scores 100 on a 7% metric by instrument failure. The lists are also not calibrated to equal strictness: C++'s H1 includes 'no matching function for call to' and Go's includes 'is not a type', which fire on ordinary overload/type errors against symbols that do exist — i.e. non-hallucinations counted as hallucinations — while Quidra's list contains no analogous over-broad pattern.

**Required fix**

Add readiness item 'C11. Signature-list validation. For each of the ten languages and each of its H1, H2 and runtime-error lists, build and run two fixtures: one that must produce a matching diagnostic and one correct program that must produce none. Preserve the verbatim diagnostic text and the match result at llm-practical/config/signature_validation/<language>.json. A list with no demonstrated positive match blocks the run until the real diagnostic text is recorded.' Separately, remove 'no matching function for call to' from the C++ H1 list and 'is not a type' from Go's (they detect type/overload errors, not nonexistent names), or add the equivalent overload/arity-mismatch pattern for every one of the ten so the classes are symmetric.

---

## [MAJOR] Finding 5
*Spec section:* 9.1 (Silent Bug Resistance, 12%); 17; 18

**Issue**

SilentBugRate(L) = silent_bug_versions / incorrect_versions, where incorrect_versions includes NO_ARTIFACT and COMPILE_FAIL versions. A version that never compiled can never be a silent bug, so every compile failure enlarges the denominator and dilutes the rate, raising the score. The joint-heaviest metric in the table is therefore partly a reward for failing to compile — and the language whose generated code will compile-fail most is the one with least pretraining exposure, i.e. Quidra. The document's stated rationale addresses the opposite error (dividing by all versions) and the two published framings (silent/incorrect and silent/total) share the same defect, since total also grows with compile failures.

**Required fix**

Keep the predeclared primary raw (changing it post-hoc is forbidden), but add a third mandatory published figure `silent_bug_versions / versions_that_reached_execution` (build succeeded or not applicable, and the process actually ran), which is the framing free of the compile-failure dilution. Add to `mandatory_published_raw`: per language, compile_fail_versions and the correlation between compile-failure rate and Silent Bug Resistance across the ten, plus the frozen sentence 'This metric's denominator includes versions that never ran; a language with more compile failures is diluted upward. Read the third framing beside it.'

---

## [MAJOR] Finding 6
*Spec section:* 25.1 family C / 25.2; run constraint frozen_tolerance.json (ALGO-TOL-1); C-2

**Issue**

This document freezes a different, far tighter tolerance for the same algorithms than the run's own frozen ALGO-TOL-1, and appears to transpose abs and rel. ALGO-TOL-1 (applies_to WL-SVM, WL-GMM, WL-LG, all ten languages) is abs 1e-9 + rel 1e-6 with a print_ulp floor, justified because 'rel 1e-6 absorbs FMA contraction under the frozen build recipes and the per-runtime differences in log/exp/sqrt implementations across 10 toolchains.' PE-P1 here uses abs 1e-6 OR rel 1e-9, and PE-P3 abs 1e-5 OR rel 1e-8, with no print_ulp floor. For a GMM log-likelihood of magnitude ~1e4 that is roughly 100x tighter than the run's own frozen figure, and the rationale offered ('the condition number keeps every printed field stable well inside 1e-06') is asserted, never measured, and directly contradicted by ALGO-TOL-1's rationale. Because the golden file is produced by the C++ reference on this host, every other language pays for libm and FMA differences against C++'s; the tight tolerance systematically advantages C++ and disadvantages the nine. Worse, the integer fields (support-vector count, correct-classification count, GMM iteration count, hard-assignment counts) require EXACT equality, and SMO over 1000 sweeps and EM over 60 iterations can change a working-set selection or a convergence step from a 1-ulp difference, making an exact-match integer field non-reproducible across toolchains for reasons unrelated to correctness.

**Required fix**

Adopt ALGO-TOL-1 verbatim for PE-P1 through PE-P6 (abs 1e-9 + rel 1e-6, predicate `abs(actual-golden) <= max(abs + rel*abs(golden), print_ulp[field_kind])`, print_ulp F6 = 1e-6), or state in this document why the LLM track diverges and attach the per-field margin measurements that support the tighter number. Add a mandatory pre-flight: implement each workload's reference in at least three of the ten languages, run all three under the frozen recipes, and confirm every float field agrees within the chosen tolerance AND every exact-match integer field agrees exactly. If an integer field is not stable across all three, it must be removed from the exact-match set or the workload reparameterised before freezing.

---

## [MAJOR] Finding 7
*Spec section:* 6.2 (tool permissions identical across languages); this document's own `tool_permissions.statement`

**Issue**

`bash_deny_list_semantics` matches on raw substrings of the command text: 'A Bash command whose text contains any deny-list pattern is refused.' Several patterns are bare substrings of ordinary words. 'nc' matches `func`, `since`, `increment`, `concat`, `encoding`. 'apt ' matches `adapt `. This produces false denials that are correlated with the target language: `func` is the function keyword in Go and Swift, so `grep func generated/main.go` or `sed -n '/func main/p'` is refused for Go and Swift trials but the equivalent command is not refused for Python, Java or Quidra. The document asserts 'Nothing in the tool set names a language, and nothing is added or removed for any language'; the enforcement mechanism silently breaks that claim, and the affected trials lose real debugging capability inside a fixed wall-clock budget.

**Required fix**

Replace substring matching with argv matching: parse the command, and refuse only when argv[0]'s basename equals a denied program name ('curl', 'wget', 'nc', 'ssh', 'scp', 'rsync', 'ftp', 'telnet', 'brew', 'apt-get', 'apt', 'port', 'gradle', 'mvn', 'npx', 'yarn', 'pnpm'), or when (argv[0], argv[1]) matches a denied pair ('git clone', 'git fetch', 'git pull', 'git remote', 'pip install', 'npm install', 'cargo add', 'cargo install', 'cargo fetch', 'go get', 'go install', 'zig fetch', 'swift package'), applied across every segment of a pipeline or `&&`/`;` chain. Add a pre-flight that runs a fixed corpus of 50 legitimate commands per language through the matcher and publishes the false-denial count per language; the list is not usable until that count is zero for all ten.

---

## [MAJOR] Finding 8
*Spec section:* 6.2 (frozen build/run recipe applied identically); 10.4 pre-flight item 2 (harness conventions satisfied by the harness)

**Issue**

Every frozen build command writes into `build/` (`clang++ ... -o build/main`, `rustc -O ... -o build/main`, `swiftc -O ... -o build/main`, `zig build-exe ... -femit-bin=build/main`, `quidra build ... -o build/main`, `go build -o build/main`, `kotlinc ... -d build/main.jar`, `tsc --outDir build`), but nothing in the document says the orchestrator creates that directory. `context_isolation` says 'The trial working directory is created empty immediately before the trial', and `human_edit_prohibition.what_is_not_a_human_edit` lists exactly what the orchestrator creates: the empty trial directory, the pack, the prompts. `clang++ -o build/main`, `rustc -o build/main`, `swiftc -o build/main` and `zig ... -femit-bin=build/main` all fail with 'no such file or directory' when `build/` is absent; `javac -d build` and `tsc --outDir build` create it. As written, the orchestrator's own scored build fails for at least five compiled languages and not for Java, TypeScript or Python — an infrastructure defect that presents exactly as COMPILE_FAIL, which is the failure mode spec 10.4 warns about.

**Required fix**

Add to `human_edit_prohibition.what_is_not_a_human_edit` and to the timeline: 'The orchestrator creates `<trial>/build/` empty before the trial and empties it before each scored build and before each timed build in Generated Code Compile Performance.' Add readiness item C10b requiring the dry run to confirm, for each of the ten, that the frozen build command succeeds against a freshly created trial directory with no prior `build/` contents.

---

## [MAJOR] Finding 9
*Spec section:* 9 (pretraining exposure deliberately included); 32 (do not alter conditions to make Quidra score higher)

**Issue**

The language_reference slot takes each language's official reference in document order and stops before exceeding 20,000 tokens. `quidra_pack` anticipates that 'Quidra's reference is shorter than 20,000 tokens' and in that case 'the whole document is used'. The consequence is not symmetric: Quidra plausibly receives 100 percent of its language definition, while Python, C++, Rust, Java and Swift receive a document-order prefix of a few percent of theirs — for the Python Language Reference and the C++ working paper, the first 20k tokens are lexical analysis and the data/object model, not the material needed to write an SVM. The nine are expected to fall back on pretraining, which is spec 9's intent; but the combination means Quidra is the only language given complete documentation, and this document presents the rule as neutral ('The identical procedure, budget, and stopping rule is applied to all ten languages... No section is chosen because it helps or hurts a language'). Identical procedure is not identical information value, and the residual tilts toward Quidra on every metric.

**Required fix**

The rule is frozen and should not be changed post-freeze, but the disclosure must be. `language_reference_sections_included` and `language_reference_sections_available` are already published fields — add a mandatory reporting obligation: 'O14. Publish, beside the LLM Practical Effectiveness table, a reference-coverage column giving sections_included / sections_available and tokens_included / tokens_available for each of the ten languages, with the frozen sentence: The 20,000-token document-order budget gives Quidra approximately N percent of its language definition and the incumbent languages a prefix of M percent of theirs. This asymmetry is uncorrected and favours the language the model has least prior exposure to.' Fill N and M from the published counts.

---

## [MAJOR] Finding 10
*Spec section:* CORRECTIONS.md D-4; this document's `source_token_counting.governing_implementation`

**Issue**

`source_token_counting.rule` says the governing implementation is the Semantic Compression tokenizer and that 'this section states the behaviour it must have' — but the behaviour stated contradicts the behaviour implemented. The rule's clause (1) is 'a string or character literal including its delimiters, counted as ONE token regardless of length', with no mention of interpolation. Read literally, `print("a={v}")` counts as 4 tokens and `f"x={c}"` as 1 — exactly the flat one-token-per-hole exemption that CORRECTIONS.md D-4 identified as QUIDRA BIAS and removed, because it discounted Python, TypeScript, Kotlin, Swift and Quidra relative to C++, Go, Java, Rust and Zig. The governing implementation (scripts/tokenize_probe.py, lines 284-322) correctly emits HOLE_OPEN/HOLE_CLOSE for braced forms and a single HOLE for bare sigils. None of the three conformance vectors in this document exercises interpolation, so the vectors cannot detect the discrepancy. An independent analyst implementing Source Token Efficiency (3%) from this frozen document alone reproduces the removed bias.

**Required fix**

Amend clause (1) to: 'a string or character literal is decomposed as in methodology 02 section 4.4(a) as corrected in CORRECTIONS.md D-4 — the literal shell counts as ONE token regardless of length, and each interpolation hole pays one token per delimiter actually written (braced or parenthesised forms such as {c}, ${c}, \(c) cost two; a bare sigil form such as $c costs one), plus the tokens of the embedded expression.' Add three conformance vectors: Quidra `print("a={v}")` = 7; Python `f"x={c}"` = 4; Kotlin `"x=$c"` = 3.

---

## [MAJOR] Finding 11
*Spec section:* 9.1 (Silent Bug Resistance, 12%); 7 (release/optimized builds); 17

**Issue**

The frozen release recipes decide the arithmetic-fault behaviour that Silent Bug Resistance measures, and they do so differently per language: `zig build-exe -OReleaseFast` disables Zig's safety checks so integer overflow wraps or is illegal behaviour rather than panicking (ReleaseSafe would trap); `rustc -O` leaves debug overflow checks off so Rust wraps; `clang++ -O2` leaves signed overflow undefined; Go wraps by definition; Swift traps even optimized; Python and TypeScript cannot overflow in the same sense. Quidra is, per C-1, the one language whose release build keeps integer arithmetic overflow-checked. On the joint-heaviest metric (12%), a wrong-but-silent result is therefore much more available to Zig, Rust, C++ and Go than to Quidra — as a property of the chosen build flags, not of the languages' cores. Nothing in the document discloses this.

**Required fix**

Do not change the recipes (spec 7 requires release builds). Add to `silent_bug_definition_and_measurement.mandatory_published_raw` a per-language row stating whether that language's frozen release recipe leaves integer overflow checked, wrapping, or undefined, and the frozen sentence 'Silent Bug Resistance compares languages as configured by their frozen release recipes, not language cores; four of the ten have arithmetic checks disabled by their release flags and one (Quidra) does not.' Optionally publish, as a clearly labelled non-scoring diagnostic, the same SVM final versions rebuilt with `zig build-exe -OReleaseSafe` and `rustc -O -C overflow-checks=on`.

---

## [MAJOR] Finding 12
*Spec section:* 6.2 'Allocating the five trials'; 9.1 (Correct@1, 12%)

**Issue**

Two problems compound. (a) The `spec_basis` for PE-P2, PE-P3, PE-P4, PE-P5, PE-P6 reads 'spec 6.2: one trial per cell outside the two mandated cells.' Spec 6.2 contains no such rule. Its default is 'initial independent trials per task/scenario/condition: 5', and its allocation section authorises one trial per cell only 'in every Intrinsic condition that is already replicated across seeds or transformation sets'. Practical Effectiveness tasks outside the primary one are not covered. The document attributes a permission to the spec that the spec does not grant. (b) Correct@1 is defined as 'Computed per task, then averaged over tasks' across all ten scored tasks. Nine of the ten per-task components come from 1-trial or 3-trial cells whose resolution the document itself states as 100 and 33 percentage points; the one five-trial cell the spec specifically mandated for this metric contributes one tenth of a 12 percent weight. The precision the spec required is diluted to a tenth by this aggregation, and obligation O2 attaches the precision sentence to 'every table row backed by fewer than five trials per language', which does not clearly cover an aggregate row over mixed-N cells.

**Required fix**

(a) Relabel those five `spec_basis` strings as 'DECLARED DEVIATION from the spec 6.2 default of five trials, taken before any scored output for cost reasons', and record the deviation in _deviations.json under class trial_count_shortfall. (b) Either compute the scored Correct@1 from PE-P1-svm-specA alone (the cell spec 6.2 names) and publish the other tasks as a separate non-scoring table, or weight the per-task Correct@1 by executed trial count. In either case add obligation 'O2b. Any aggregated row whose per-task components have mixed trial counts must publish the per-task N vector and the effective resolution of the aggregate.'

---

## [MAJOR] Finding 13
*Spec section:* 6.2 (repair prompt template in the immutable config); 9

**Issue**

The `{{STAGE}}` fill rule admits 'exactly one of: PARSE_OR_COMPILE_FAILED, RUNTIME_FAILED, TIMED_OUT, OUTPUT_MISMATCH. Derived mechanically from the classification of the previous version.' The classifier produces seven classes. Three have no mapping: NO_ARTIFACT, SILENT_BUG and COMPLIANCE_FAIL. The COMPLIANCE_FAIL case is the worst: the oracle matched, so {{ORACLE_DIFF}} is 'N/A' and {{TEST_RESULT_SUMMARY}} reads 'checks passed: T of T', and the template has no field at all for the compliance result — the model is told 'Your program did not pass' alongside evidence that it did, with no information about the anti-hard-coding hit or the failing checklist item. The NO_ARTIFACT case has no program to build, so every build/run field is undefined. This is a judgement call left to whoever runs the orchestrator, and it changes repair outcomes for Repair Success (7%), Repair Efficiency (5%) and Diagnosis Efficiency (5%).

**Required fix**

Replace the enum with a total mapping table: NO_ARTIFACT -> NO_SOURCE_FILE_FOUND; COMPILE_FAIL -> PARSE_OR_COMPILE_FAILED (or BUILD_TIMED_OUT for the BUILD_TIMEOUT subclass); RUNTIME_FAIL -> RUNTIME_FAILED (or TIMED_OUT for RUN_TIMEOUT); SILENT_BUG and OUTPUT_MISMATCH -> OUTPUT_MISMATCH; COMPLIANCE_FAIL -> COMPLIANCE_FAILED. For NO_SOURCE_FILE_FOUND, fill every build/run/oracle field with the literal 'N/A (no source file was found at the required path)'. Add a `--- COMPLIANCE RESULT ---` block to the template carrying the mechanical line 'compliance items satisfied: P of K' and the verbatim ids of the failing items with no interpretation; state that it is filled with 'N/A (all compliance items were satisfied)' otherwise, so the block is present byte-identically for all ten languages.

---

## [MAJOR] Finding 14
*Spec section:* 9 (Specification Compliance, 7%); 32

**Issue**

The anti-hardcoding check is the only mechanism that can force FAIL on an otherwise passing program, and it turns on an undefined judgement plus a self-contradiction. It searches for 'any literal that equals, to 6 decimals, a golden floating-point field not derivable as a task-specified constant' — 'derivable as a task-specified constant' is nowhere defined, and a golden accuracy of 0.950000 would be matched by the literal `0.95` that appears innocently in many implementations. The document then says both that a hit 'sets the version's classification to FAIL regardless of output match' and that 'its hit list is preserved for human audit so a false positive can be seen and is reported rather than silently applied.' Those are two different procedures: auto-FAIL, or human review. Which one runs decides scores, and the human-review branch would itself inject human judgement into a pipeline the document elsewhere insists is mechanical.

**Required fix**

Define the allowlist mechanically: 'A numeric literal is exempt if and only if it appears verbatim in that task's task_spec.md, or is one of 0, 1, 2, -1, 0.5, or one of the frozen LCG constants 48271 and 2147483647.' State one procedure and delete the other — recommended: 'A hit on a non-exempt literal sets the compliance item to fail and the classification to COMPLIANCE_FAIL, mechanically and with no human review. The hit list is preserved so a third party can audit the rule; a false positive is reported in _deviations.json and is not overturned within this run.' Add a pre-flight that runs the check against the benchmark's own reference implementation in three languages and confirms zero hits.

---

## [MAJOR] Finding 15
*Spec section:* 9 (Specification Compliance, 7%); 6.2 (tool permissions)

**Issue**

PE-P1's compliance checklist contains three items that are not mechanically checkable as written, and one that contradicts the tool permissions. 'no file written outside build/' directly conflicts with `tool_permissions`, which grants the trial agent Write and Edit on `<trial>/scratch/` — an agent that uses the scratch directory it was explicitly given fails a compliance item worth part of 7 percent. 'no stdin read' and 'no command-line arguments consulted' have no stated detection method (static source search? runtime observation? which symbols per language?). 'the specified LCG seed used (verified by the first printed field of a specified debug-free derived quantity included in the schema)' does not describe a check that can be implemented from this text.

**Required fix**

Rewrite as observable predicates on the program, not the agent: 'no file written outside build/ — verified by the harness recording the set of paths created or modified during the frozen run command; files the agent wrote at authoring time, including under scratch/, are out of scope'; 'no stdin read — verified by running the frozen run command with stdin connected to /dev/null and to a 1 MiB byte stream and confirming byte-identical stdout'; 'no command-line arguments consulted — verified by running the frozen run command with three extra ignored arguments appended and confirming byte-identical stdout'; and replace the LCG item with the concrete named schema field, e.g. 'the specified LCG seed used — verified because oracle_schema field <id> is the value of the 5th unit float of the generator and is compared exactly against the golden.'

---

## [MAJOR] Finding 16
*Spec section:* 25.2 ('normalize at the lowest comparable workload level first'; unweighted mean across the fixed applicable workload set)

**Issue**

Two family-C metrics make their workload set depend on the language, which breaks cross-language comparability. Generated Code Performance says both 'Measured on PE-P1-svm-specA' and 'where a language has more than one passing task, the unweighted mean of the per-task family-C scores is used' — two different aggregations in one sentence, and the second makes the averaged task set differ per language. Source Token Efficiency is worse: 'If a language has no passing version for a task, that task contributes no raw value for that language; the language's score is the mean over the tasks where it does have one.' A language is therefore scored only on the tasks it succeeded at. Failing the task on which it would have been most verbose raises its score. This is conditioning on success, and it is also inconsistent with the sibling rule two lines later that a language with no passing version anywhere scores 0 rather than N/A, and with Generated Code Performance's 'no passing version -> score 0'.

**Required fix**

Fix the workload set before the run and make it language-independent. For Generated Code Performance, Memory and Compile Performance: 'the applicable workload set is exactly {PE-P1-svm-specA}; a language with no passing version there scores 0.' For Source Token Efficiency: 'the applicable workload set is exactly {PE-P1, PE-P2, PE-P3, PE-P4, PE-P5, PE-P6}; a task with no passing version for a language contributes a per-task score of 0 for that language, not an omission.' Delete the 'mean over the tasks where it does have one' clause.

---

## [MAJOR] Finding 17
*Spec section:* 10.4 pre-flight item 2 (harness conventions satisfied by the harness, not demanded of the model); C-4

**Issue**

The frozen TypeScript build command is `tsc --outDir build generated/main.ts` with no target, lib or module pinned and no tsconfig.json in the pack, so tsc runs at its default target. UG-03 requires printing 2^53 and 2^53+1 exactly and UG-04 requires -9223372036854775808 and 9223372036854775808 exactly; both are unrepresentable in a double and require BigInt, and BigInt literals and BigInt-typed arithmetic are rejected at the default target with 'BigInt literals are not available when targeting lower than ES2020'. TypeScript would fail two of twelve UG cases (6% metric) for a harness configuration reason, in exactly the category spec 10.4(b) says the harness must absorb. The same class of risk exists unverified for Java and Kotlin: `javac -d build generated/Main.java` followed by `java -cp build Main` breaks if the model declares a package, and the system prompt does not forbid one.

**Required fix**

Pin the TypeScript recipe explicitly in environment.json and here — `tsc --target es2022 --module commonjs --outDir build generated/main.ts` — and record the change under C-4 as a harness responsibility. For Java and Kotlin, add to the build_run_recipe.txt slot (which already exists for all ten languages) the line 'declare no package; the entry point must be the top-level class Main' / 'the top-level file main.kt'. Extend readiness item C10 to require that the dry run include, for all ten languages, a fixture exercising UG-03, UG-04 and UG-09 under the exact frozen recipe.

---

## [MAJOR] Finding 18
*Spec section:* 21; 16 B; 9

**Issue**

Two task definitions leave a pass/fail-deciding question open. (a) UG-09 requires that integer division by zero 'must be detected and reported... The program must continue and print the remaining cases and exit with status 0.' In C++ this is undefined behaviour and normally delivers SIGFPE, which cannot be caught portably and from which the program cannot continue; in Swift it is a non-catchable fatal error; in Zig it is illegal behaviour; in Rust it panics and is recoverable only via the non-idiomatic catch_unwind. The only construction that works in all ten is to test the divisor before dividing — which never attempts the division, so it is not an error-handling test at all. The document does not say whether a pre-check satisfies 'the failure must be detected and reported', and it asserts in `fairness_statement` that UG-09 is a case where Quidra's error model 'make[s] the case harder rather than easier', which is the reverse of the truth: a language with a catchable checked-division error passes naturally where C++, Swift and Zig cannot. (b) PE-P2/P4/P6 supply 'the pinned C++ reference', while the golden comes from 'the benchmark's own frozen reference implementation... reduced to the common workload'. Whether the source shown to the model is the upstream repository or the reduced implementation that produced the golden decides whether a faithful port passes at all.

**Required fix**

(a) Add to UG-09's requirement the frozen sentence 'A conforming implementation may either attempt the division and handle the resulting failure, or test the divisor and report the failure without attempting it; both satisfy this case. What is scored is the printed line ERROR:DIVZERO, the continuation, and exit status 0.' Delete UG-09 from the `fairness_statement` list of cases claimed to be harder for Quidra, or replace that claim with the accurate one. (b) State in PE-P2/P4/P6: 'The reference source shown is byte-identical to the frozen reduced reference implementation that generated this task's golden file; its SHA-256 is <digest> and it is the same bytes for all ten languages.'

---

## [MINOR] Finding 19
*Spec section:* 12 (do not compare un-warmed JIT against steady-state native without separating); 25.2

**Issue**

`managed_runtime_handling` says managed runtimes' 'cold-start and steady-state numbers are reported separately, never mixed', but never says which one enters the score. The measurement protocol given is '3 discarded warm-up runs then 7 measured runs, median' — at process granularity, so every measured run for Java, Kotlin, node and Python is a fresh process that includes runtime startup and JIT warm-up. Under that protocol no steady-state number exists to report, and 'never mixed' is unfalsifiable.

**Required fix**

State explicitly: 'Each of the 3 warm-up and 7 measured runs is a fresh process. The scored raw value therefore includes runtime startup and JIT warm-up for Java, Kotlin, TypeScript/node and Python, and includes process start for the compiled languages. This is the scored figure for all ten. A separate in-process steady-state figure is measured for the managed runtimes by <named protocol> and is published as a non-scoring diagnostic; it never enters any score.'

---

## [MINOR] Finding 20
*Spec section:* 11 ('Define the aggregation rule for combining Native and Interpreter evidence... before examining benchmark results')

**Issue**

Spec 11 asks for a rule that combines Quidra's native and interpreter evidence. This document instead selects one mode: native for the scored column, interpreter published as a raw row only. Native is the faster of the two, so the selection is the maximally Quidra-favourable option available for Generated Code Performance (3%) and Generated Code Memory Efficiency (1%). The justification offered (symmetry with other languages' release builds) is reasonable and the choice is predeclared, which satisfies the timing requirement — but the effect of the choice on Quidra's score is never made visible.

**Required fix**

Relabel the rule honestly ('selection, not combination') and add a reporting obligation: 'O15. Publish Quidra's LLM Practical Effectiveness Score recomputed with the interpreter-mode values substituted for Generated Code Performance and Generated Code Memory Efficiency, beside the primary score, so the effect of the native-mode selection is visible rather than absorbed', mirroring the discipline spec 25.4 requires when a normalisation choice changes.

---

## [MINOR] Finding 21
*Spec section:* 6.2 ('Record the provider, public model name, exact model/version identifier..., API or client version, and benchmark date')

**Issue**

`api_version_string` is null with the reason 'This client does not expose a provider API version string to the caller.' That is a fair statement about the provider API version, but spec 6.2 asks for 'API or client version' — the disjunction is satisfiable. The client is identified only by name ('Claude Code agent harness (Claude Agent SDK)') with no version, so a mandatory 6.2 field is left unfilled when a value is obtainable.

**Required fix**

Add `client_version` recording the Claude Code CLI / Agent SDK version string as reported by the harness (e.g. from `claude --version` or the SDK package version), captured once before the first trial and repeated per trial in manifest.json. Keep `api_version_string: null` with its existing reason.

---

## [MINOR] Finding 22
*Spec section:* 32; run constraint C-1

**Issue**

`fairness_declaration.nothing_derived_from_quidra` states that no 'metric definition, tolerance, tool permission, or weight in this document was derived from Quidra's syntax, operators, types, standard library, or feature set.' `common_task_contract.determinism` adopts the Park-Miller LCG, and C-1 records in terms that the generator was chosen because 'in Quidra, state * 6364136223846793005 on a uint64 raises runtime error[INTEGER_OVERFLOW] rather than wrapping' — i.e. the choice was driven by Quidra's overflow-checked integer types. C-1's neutrality argument is sound (the generator is exact and idiomatic in all ten), but the blanket declaration in this document is contradicted by the run's own methodology, which is the first thing an auditor comparing the two files will find.

**Required fix**

Amend the clause to: '...was derived from Quidra's syntax, operators, types, standard library, or feature set, with one disclosed exception: the frozen Park-Miller data generator was selected under the constraint recorded in 00_cross_language_constraints.md C-1, which includes Quidra's overflow-checked integers among the ten languages the generator had to be exactly and idiomatically expressible in. C-1 records the neutrality argument and the verification that the generator requires no workaround and grants no shortcut in any of the ten.'

---

## [MINOR] Finding 23
*Spec section:* 23 (complete preservation of LLM trials)

**Issue**

`tool_permissions` grants Write and Edit on `<trial>/scratch/`, but `trial_preservation_layout.directory_tree` has no `scratch/` entry, so anything the agent worked out there is destroyed rather than preserved, weakening the spec 23 reconstruction guarantee. The `exact_system_prompt` also never tells the model the scratch directory exists or that it may write there, so the permission is effectively unusable and the trials are not identical in the affordances they actually offer.

**Required fix**

Add `<root>/<task_id>/<language_slug>/trial_<NN>/scratch/` to the preserved directory tree, with the note that it is preserved read-only and is not scored. Add one line to system-prompt rule 1: 'You may write working files under {{WORKDIR}}/scratch/; nothing there is scored, and it is preserved for audit.' Note that changing the system prompt requires a new benchmark run id under this document's own freeze rules, so make the change before the first scored generation or not at all.

---

## [MINOR] Finding 24
*Spec section:* 22; 9.1 (Prompt Robustness 5%, Correct@N 5%); 6.2 reporting

**Issue**

Two scored metrics have resolution worse than the document's published resolution floor, and the reporting obligations do not cover them. PromptRobustness_raw is the minimum of three rates each estimated from 5 trials; the minimum of three such estimates is biased downward and its resolution is coarser than the 20 percentage points the `resolution_floor` table states for a per-variation rate. Correct@N is a per-cell binary averaged over exactly four eligible cells, so it takes only the five values 0, 25, 50, 75, 100. Obligation O2 attaches the precision sentence only to rows 'backed by fewer than five trials per language', which neither case triggers.

**Required fix**

Add to `resolution_floor`: 'PromptRobustness (min of three 5-trial rates): coarser than 20 percentage points; the minimum of three binomial estimates is biased low and the bias is largest for mid-range languages' and 'Correct@N: 25 percentage points (four eligible cells, each contributing a binary outcome).' Extend O2 to: 'and to every row whose scored value can take fewer than ten distinct values, stating the attainable value set.'

---
