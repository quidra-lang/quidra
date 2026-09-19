# Audit findings for `07_algorithm_workloads.md`

**Verdict:** needs_fix  
**Quidra bias found:** True  

## [BLOCKER] Finding 1
*Spec section:* prompt.md §32 (no workload favorable to one language), §13/14/15; artifact §1.9, §1.8, §5.1

**Issue**

QUIDRA BIAS — §1.9's "standard library only" clause bans, by name, `numpy`, BLAS, `Eigen`, `nalgebra`, `gonum`, `Accelerate` and parallel `java.util.stream` — i.e. it enumerates the hazard for six of the nine comparison languages, and names nothing belonging to the language under evaluation. Quidra's *standard namespaces* include `linear.dot` / `linear.matmul` (docs/spec/language.md:666, backed by a 'validated linear-algebra fast path' native kernel per docs/spec/architecture.md:67) and `neural` (`neural.track`, `neural.grad`, `neural.Parameter`, `neural.Gradients`), a built-in reverse-mode autodiff engine. Read literally, §1.9 permits Quidra to build WL-SVM's Gram matrix and inner dot products with a native linear-algebra kernel while Rust must hand-write the scalar loop that `nalgebra` is forbidden to supply, and permits Quidra to satisfy WL-LG by calling built-in autodiff while the other nine hand-build a tape. `00_cross_language_constraints.md` §C-6 patches exactly this hole — but only for LightGrad, and only in a document that §5.1 does not extract into the LLM prompts. §5.1 builds the Scenario A/B constraint block from "1.1, 1.3, 1.9", so the prompt actually shown to the model for Quidra would carry the unpatched sentence. §1.8's "uses ... a numerical library" is not a reliable backstop: `linear` and `neural` are language namespaces, not libraries, and a conforming `linear.dot` (Quidra documents ascending accumulation order) would also pass the comparator bit-exactly, so nothing but a reviewer's opinion would catch it.

**Required fix**

Rewrite §1.9's second bullet to be symmetric and name the language under evaluation first: "Standard library only, and in all three workloads every language implements the numeric kernels and, for WL-LG, the autodiff engine itself, from general-purpose facilities (scalar arithmetic, arrays, dynamic containers, maps, string formatting, sqrt/exp/log). Prohibited in all ten languages symmetrically: Quidra — the `linear` namespace (`linear.dot`, `linear.matmul`), the `neural` namespace in any form (`track`, `grad`, `Parameter`, `State`, `Gradients`, differentiable reductions, `save`/`load`) and the `dnn` package; Python — numpy, torch, jax, autograd, tensorflow; C++ — Eigen, BLAS; Rust — nalgebra, ndarray; Go — gonum; Swift — Accelerate; Java/Kotlin — any linear-algebra or autodiff library and parallel streams; TypeScript, Zig — any tensor/autodiff/BLAS package." Then add that sentence to the §5.1 item-4 extraction list so it reaches every prompt, and copy `00_cross_language_constraints.md` §C-6 into §4.1's "Dropped" table so the WL-LG rule is inside the frozen workload document rather than only beside it.

---

## [BLOCKER] Finding 2
*Spec section:* prompt.md §25.1 (family C shifted form, epsilon predeclared), §25.4, §33 item 60

**Issue**

MECHANICAL REPRODUCIBILITY / SPEC COMPLIANCE — §6's metric matrix lists 17 metrics and their evidence sources but declares no normalization family, no epsilon, no clipping, no aggregation and no weight for any of them, and no mapping from these metrics to the Standard / LLM Practical scores. Every sibling methodology document does declare these (`03` epsilon=0.025, `04` epsilon=0.025, `06` §6 frozen family-C mapping table + epsilon_compile=0.001 s). Two consequences are concrete, not theoretical: (a) `Numerical Error` is defined in §1.6 as `max |actual-golden|/(1+|golden|)`, which is exactly `0.0` for any bit-exact language — §4.5 states Test A is *expected* to be bit-exact in all 10 — so the plain family-C form `100 * best_positive_raw / raw_i` divides by zero and the shifted form's `epsilon` is mandatory and must be fixed before the run, per §25.1.C; it is absent from both the document and `frozen_tolerance.json`. (b) Execution Time on these workloads spans ~400x (12.6 s CPython vs ~30 ms native, both this document's own figures), triggering §25.1's and §33 item 60's mandatory raw-value-and-ratio publication, which the document never states. Choosing any of this after the runs is precisely the post-result formula selection §25.4 forbids.

**Required fix**

Add a §6.1 "Frozen normalization" table before any measurement, in the shape of `06_micro_workloads.md` §6: Compile/Parse Success, Test Pass Rate, Numerical Match, Prediction/Assignment Match, Parameter Match, Forward Correctness, Backward Correctness, LLM Generation Success → family A; Silent Bug rate → family B; Execution Time, Peak RSS, Compile Time, Source Bytes, LOC, Source Tokens, Numerical Error → family C; Repair Count → family E. Declare in the same table and in `frozen_tolerance.json`: `epsilon_numerical_error = 1e-12` (the F12 print granularity — the resolution below which the comparator does not claim precision), `epsilon_compile = 0.001 s` (matching `06` §5.5), `epsilon_exec = 0.001 s`; per-workload normalization then unweighted arithmetic mean over {WL-SVM, WL-GMM, WL-LG} per §25.2; and state which Standard/LLM metric each row feeds, plus the §25.1 ratio-disclosure obligation for Execution Time, Compile Time and Peak RSS.

---

## [MAJOR] Finding 3
*Spec section:* prompt.md §26 (N/A policy), §5

**Issue**

SPEC COMPLIANCE — §9's `NA_NOT_PRODUCED` marks `Numerical Match`, `Numerical Error`, `Prediction Match` and `Forward/Backward Correctness` as N/A on any COMPILE_FAIL, RUNTIME_FAIL or TIMEOUT run. §26 requires distinguishing "not applicable" from "the language lacks a capability that the metric is intentionally testing", and states that a missing capability "must not automatically escape scoring by being labeled N/A". A program that does not compile has failed the exact thing Numerical Match tests. Because §26 also says N/A cells are dropped from the applicable-weight denominator and the remaining weights renormalized, a language that fails WL-GMM outright and matches on WL-SVM and WL-LG would be scored on Numerical Match as though it had matched everywhere — identical to a language that matched all three. Only Test Pass Rate captures the failure, and §13/§15 list these as separate scored metrics.

**Required fix**

Split the reason code. Keep `NA_NOT_PRODUCED` only for `Numerical Error` (an unbounded magnitude that genuinely does not exist without output), and add: "`FAIL_SCORED_ZERO` — the run produced no valid output because the implementation failed (COMPILE_FAIL, RUNTIME_FAIL, TIMEOUT, OUTPUT_CONTRACT_FAIL). All bounded correctness fractions (Numerical Match, Prediction/Assignment Match, Parameter Match, Forward Correctness, Backward Correctness) are scored 0.0 for that (language x workload x scenario x trial), never N/A. This is a measured failure of the capability under test, not an inapplicability (spec §26)." Also state that a TIMEOUT's Execution Time is a censored `1800.0` with `timed_out = true`, as `06_micro_workloads.md` §5.7 already requires, not N/A.

---

## [MAJOR] Finding 4
*Spec section:* prompt.md §4 (no fabricated values), §32; artifact §2.3, §1.11, §5.1

**Issue**

MECHANICAL REPRODUCIBILITY — §2.3's self-check constant is wrong. It states "the first training point is `x[0] ≈ (0.6836, 1.5163, 0.3262, -0.5479)`". Re-running the frozen LCG-PM (mult 48271, mod 2147483647, seed 1234567), Irwin-Hall(12) normal, exact draw order of §2.3, gives `x[0] = (0.8992, 0.1125, 0.9713, -0.3644)`. Every other informative value in the document reproduces exactly — I re-derived ERROR_LAST 19.782045, MAX_ABS_DELTA 3.246166, BETA 1.534149, NS_MARGIN 65, W, B, OBJECTIVE, ALPHA_SUM, ALPHA_Y_SUM 0.047792639128, ALPHA_CHECKSUM 126.024432, the accuracies, the first ten ALPHA and PRED entries, all five rows of §2.6's discrete-margin table, the entire §3.9 GMM block, and §4.7's STRESS_LOSS_0/50/100/FINAL, P_SUM, Q_SUM and the two closed-form gradients — so this is an isolated stale value, not a different RNG reading. It is also the single most damaging one to get wrong: it is the first constant an implementer checks, it sits in §2.3 (not in the §2.8 informative block that §5.1 excludes), and it therefore ships inside every Scenario A and Scenario B prompt for all 10 languages. A model that trusts it will burn repair turns 'fixing' a correct generator, or worse, distort the generator to match it and land a Silent Bug — inflating Repair Count uniformly and corrupting the metric.

**Required fix**

Replace the §2.3 line with the reproduced value and add the second point so the check is discriminating: "Informative: `x[0] = (0.899197, 0.112540, 0.971311, -0.364383)` and `x[1] = (0.858700, 1.140200, 1.266500, -1.292300)`; the dataset is linearly non-separable (the frozen run misclassifies 2 of 200 training points)." Per §1.11 and the document's own erratum clause, record the correction with its discovery timestamp. Then re-derive every informative constant in §2.3, §2.6, §2.8, §3.9 and §4.7 from the oracle of §8 before the goldens are frozen, and mark each with the oracle build that produced it, so no other stale value survives.

---

## [MAJOR] Finding 5
*Spec section:* prompt.md §16.A (Scenario A must not show the reference/answers); artifact §5.1, §2.6, §3.7, §4.7

**Issue**

MECHANICAL REPRODUCIBILITY / FAIRNESS — §5.1 promises the Scenario A prompt "must not contain ... the golden output files", then item 5 puts "the workload's invariants" into the prompt. The invariant sections carry exact golden field values as parentheticals: §2.6 gives `ALPHA_Y_SUM` = `0.047792639128` (a printed F12 output field, verbatim), §3.7 gives `ASSIGN_MIN_MARGIN` = `0.176425806929` (likewise) and pins `GAMMA_ROW_DEV_MAX` / `PI_SUM_DEV` to their printed golden strings, and §4.7's prose (inside section 4, which §5.1 includes; only §4.8 is excluded) gives `STRESS_GRAD_P0 = 0.282227` and `STRESS_GRAD_Q0 = 0.033203`. That is at least five golden fields handed to the model, some to full F12 precision, in a scenario whose point is that the answer is withheld. It also makes the SVM/GMM Scenario A prompts unequal in difficulty to what §5.1 describes, and the leak is invisible in the saved prompts unless an auditor knows the golden file.

**Required fix**

Add to §5.1: "Invariants are extracted as bounds only. Every measured parenthetical ('the frozen run gives ...', 'Frozen run: ...') is stripped by the extraction script, which fails loudly if any numeric literal in the extracted text also appears as a field value in that workload's golden file." Restate the invariants in bound form in §2.6/§3.7 — `|ALPHA_Y_SUM| <= 1.0`, `GAMMA_ROW_DEV_MAX` and `PI_SUM_DEV` must print as `0.000000000000`, `ASSIGN_MIN_MARGIN > 1e-6` — and move the measured margins into a clearly marked non-extracted block alongside §2.8/§3.9. Do the same for §4.7's two gradient values, or state explicitly that they are intentionally included as 'tests' under spec §16.A, as §5.1 already does for the §4.5/§4.6 closed forms.

---

## [MAJOR] Finding 6
*Spec section:* prompt.md §13 (Compile Time), §4 (reproducible tests); artifact §1.10, §6

**Issue**

MECHANICAL REPRODUCIBILITY / FAIRNESS — `Compile Time` is specified only as "frozen build recipe, 5 runs, median". The state of each toolchain's build cache between those 5 runs is never fixed, and it is worth one to two orders of magnitude for exactly the languages whose compile time is being compared: `go build` with a warm GOCACHE can return in milliseconds and cold in seconds; `rustc`, `javac`, `kotlinc` (which also builds a fat jar via `-include-runtime`) and `swiftc` (module cache) all differ similarly; `clang++`, `zig build-exe` and `python3` barely move. Whether the analyst clears the cache, or measures runs 2-5 warm after a cold run 1, changes Compile Time by more than any language difference the metric is trying to show — and it changes the median, which is the headline figure. An independent analyst cannot reproduce the number.

**Required fix**

Add to §1.10: "Every timed compile is a cold compile. Before each of the 5 timed builds the harness removes the output directory and the toolchain's user caches for that language — `GOCACHE`/`GOMODCACHE`, `CARGO_HOME`/`target`, the `javac`/`kotlinc` output dir and Gradle-free jar, the Swift module cache (`~/Library/Caches/org.swift.swiftpm`, `clang` module cache), the `zig` local and global cache dirs, `tsc`'s `.tsbuildinfo`, and `__pycache__` — using the identical clear-then-build sequence for all 10 languages; the clearing step is outside the timed window. Quidra Interpreter and Python report `Parse Time` under the same cold discipline. The exact clearing commands per language are frozen in `environment/environment.json` -> `frozen_toolchain_recipes.cache_clear`."

---

## [MAJOR] Finding 7
*Spec section:* prompt.md §13/§14/§15 (Source Tokens, LOC, Source Bytes); artifact §6

**Issue**

MECHANICAL REPRODUCIBILITY — §6 defines `Source Bytes / LOC / Source Tokens` only as "static analysis of the submitted single file". No tokenizer, no LOC rule. `02_fact_taxonomy_and_density.md` §4 does freeze a language-neutral tokenizer, but it is scoped to Semantic Compression probe files: its R1 counts only text between `BEGIN PROBE`/`END PROBE` markers (which whole-program submissions do not have), R4 deletes author-optional lexemes and R6 forbids dead code (neither is applicable to an LLM-generated 400-line program). So the one frozen instrument does not apply as written, and the artifact does not cite it anyway. LOC is worse: with no rule, brace-only lines, blank lines and comment-only lines are counted or not at the analyst's discretion, which alone moves C++/Rust/Java/Zig against Python by 20-30%. Both feed family-C scored metrics.

**Required fix**

Add to §6: "`Source Tokens` = the frozen tokenizer of `02_fact_taxonomy_and_density.md` §4.3-§4.6 applied to the entire submitted file, with R2 (comments/whitespace emit nothing), R3 (no synthesized virtual tokens) and R5 (identifier length free, qualified names split) retained, and R1 (probe region), R4 (optional-lexeme removal) and R6 (dead-code removal) explicitly disabled, since the unit of measurement here is a whole program as submitted. `LOC` = the number of lines containing at least one token after R2 removal (blank and comment-only lines excluded; a brace-only line counts). `Source Bytes` = the byte length of the file as submitted, LF-normalized. The same script, `scripts/count_source.py`, produces all three for all 10 languages and is run before any score is computed."

---

## [MAJOR] Finding 8
*Spec section:* prompt.md §4 (explicitly defined objective criteria), §32 (Silent Bug never a success); artifact §1.8 vs §2.9

**Issue**

MECHANICAL REPRODUCIBILITY — the §1.8 anti-subversion check is "a mandatory manual source review" recorded as `pass | fail` with a free-text note, and its outcome can flip a run from PASS to SILENT_BUG. It has no rubric, and its two clauses are open-ended ("uses a different algorithm that happens to agree", "uses ... a numerical library"). Worse, §1.8 and §2.9 contradict each other on a case §2.9 itself raises: §1.8 says a subverting implementation is SILENT_BUG "even if its output matches golden", while §2.9 says omitting `S_inside` from the `w` accumulation "is recorded as a latent defect rather than a `SILENT_BUG` if the output is identical". Two analysts reviewing the same source will produce different outcome classifications, and the outcome classification drives Test Pass Rate and Silent Bug rate for every language.

**Required fix**

Replace the free-text review with a closed checklist in §1.8: enumerate the subversion tests as yes/no questions (hard-coded output field? loop trip count not equal to the pinned constant? early exit on an undefined condition? algorithm substitution from the named list? thread/SIMD/library kernel from the §1.9 prohibition list? reads golden/reference/network at run time?), require each to be answered per submission with a source line citation, and state that `fail` on any question yields SILENT_BUG regardless of output. Then reconcile §2.9: add a third outcome, `LATENT_DEFECT` — "a deviation from the pinned algorithm that provably cannot change any printed field at the frozen constants (e.g. omitting `S_inside` when `NS_INSIDE == 0`). Recorded in the review record with its proof, does not change the run's outcome class, and is reported in its own column" — and cross-reference it from §1.8 so the two sections agree.

---

## [MAJOR] Finding 9
*Spec section:* prompt.md §11 (aggregation rule defined before results), §32; artifact §6 closing paragraph

**Issue**

QUIDRA BIAS / REPRODUCIBILITY — §6 says Quidra Native and Quidra Interpreter are collapsed into one column "using the aggregation rule frozen elsewhere in the methodology", which is `06_micro_workloads.md` §8. That rule was written for performance metrics: its table covers Execution Performance, Compile Performance, Binary Size, Memory, Startup, Deployment Footprint, and its catch-all is "any other Standard metric → Native only". It has no row for the correctness-class metrics §6 collects (Test Pass Rate, Numerical Match, Silent Bug occurrence, Forward/Backward Correctness, Repair Count, LLM Generation Success), and its Step 5 says that when one mode fails a correctness gate and the other passes, "the metric's Quidra raw for that workload is taken from the passing mode alone". Applied to this document's metrics that is best-of-two: Quidra gets two independent chances to pass WL-SVM/WL-GMM/WL-LG and to avoid a Silent Bug, and the loser is dropped, while the other nine languages get one. Separately, §6 says every cell has two Quidra rows including `LLM Generation Success` and `Repair Count`, and §5.1 item 4 puts "the frozen build recipe for the target language" in the prompt — of which Quidra has two — so it is undefined whether Quidra runs 5 LLM trials or 10 per cell.

**Required fix**

State the rule in §6 rather than deferring it: "For every correctness-class metric in this table (Compile/Parse Success, Test Pass Rate, Numerical Match, Prediction/Assignment Match, Parameter Match, Forward/Backward Correctness, Silent Bug occurrence), Quidra's language-level value is the *unweighted mean of the two modes'* values, and a Silent Bug or failure in either mode is reported in the Silent Bug column; `06` §8 Step 5's passing-mode substitution does not apply to correctness metrics, only to the performance metrics it was written for." And add: "LLM trials are generated once per language per cell. For Quidra, a single generated source file is built and run in both modes; `LLM Generation Success` and `Repair Count` are recorded once, against Native, with the Interpreter outcome reported as a separate diagnostic row. Quidra receives exactly 5 trials per cell, as every other language does."

---

## [MAJOR] Finding 10
*Spec section:* prompt.md §12 (account fairly for runtime characteristics), §15 (Memory Usage); artifact §1.10, §4.7

**Issue**

FAIRNESS ACROSS ALL TEN — §1.10 carefully requires a no-op `STARTUP_TIME` per language to be published beside wall time so the reader can separate runtime startup from algorithm time, and then provides no equivalent for memory, even though §4.7 makes Peak RSS on WL-LG a deliberately scored signal ("a program that never releases them holds ~150 MB; a correct one holds a few MB"). Peak RSS of a whole process is dominated by the runtime's own floor and by GC heap-growth policy, not by whether the program leaks: a correct JVM, Node, Go or CPython implementation will show a peak set by default heap sizing and lazy collection, while C++/Rust/Zig/Swift show close to the live set. Under family C that is a multiple-x penalty on five of the ten languages for a property of their runtime rather than of the submitted code — the exact asymmetry §1.10 corrects for time. On WL-SVM and WL-GMM, whose working sets are ~320 KB and ~1.6 MB, Peak RSS measures essentially nothing *but* the runtime floor.

**Required fix**

Extend §1.10's startup rule to memory: "The harness records `BASELINE_RSS` for each language from the same no-op program built with the identical recipe, and publishes it beside every Peak RSS figure, together with the ratio `peak_rss_i / best_positive_peak_rss`. Baseline RSS is never silently subtracted from a reported figure. No workload-specific heap, GC or arena flag may be added for any language (§0), so the baseline is the honest floor and is disclosed rather than corrected for." Add to §4.7 that the WL-LG memory signal is read as `peak_rss - baseline_rss` in the *narrative*, with both raw numbers published, and note in §6 that Peak RSS on WL-SVM and WL-GMM is dominated by the runtime floor and is reported for completeness rather than as a discriminator.

---

## [MINOR] Finding 11
*Spec section:* prompt.md §15, §25.2; artifact §4.9, §4.10, §6

**Issue**

MECHANICAL REPRODUCIBILITY — WL-LG's Test Pass Rate has no denominator. §4.9 says each of the five error runs "contributes one point to `Test Pass Rate`", and §4.10 says PASS requires the main run plus all five error runs plus `NODE_TYPES == 6`. So Test Pass Rate for WL-LG might be out of 5, 6, or 7, while for WL-SVM and WL-GMM it is a single boolean. §25.2 then takes an unweighted mean across the three workloads, so the choice changes every language's aggregate.

**Required fix**

State in §4.10: "`Test Pass Rate`(WL-LG) = (1 if the main run is `PASS` per 1.7, else 0, + the number of the five §4.9 error runs whose observable behaviour is exactly as specified) / 6. `NODE_TYPES == 6` is part of the main run's PASS condition and contributes no separate point." And in §2.9/§3.10: "`Test Pass Rate`(WL-SVM) and (WL-GMM) = 1 if the run is `PASS`, else 0 (denominator 1)."

---

## [MINOR] Finding 12
*Spec section:* prompt.md §13 (Accuracy), §14 (Parameter Match, Likelihood agreement); artifact §6, §5.3

**Issue**

MECHANICAL REPRODUCIBILITY — three metrics the spec names are collected but never given a numeric definition. §6 maps `Parameter Match` to "`COMP` lines", `Likelihood / objective agreement` to "output schema" and `Accuracy` to "`TRAIN_ACC`, `TEST_ACC`, per class", without saying whether each is a boolean, a fraction of fields within tolerance, or a deviation magnitude — and if `Accuracy` is the raw 0.99, every correct implementation ties and the metric discriminates nothing, while if it is agreement-with-golden it is a boolean. `Prediction Match` is defined in §1.6; these three are not.

**Required fix**

Add to §1.6, beside the Prediction Match definition: "`Parameter Match` (WL-GMM) = the fraction of the 52 `COMP` scalar fields (4 components x (1 PI + 3 MU + 9 SIGMA)) satisfying `numeric_match`; must be 1.000000 to pass. `Likelihood / objective agreement` = a boolean, `numeric_match` satisfied on `OBJECTIVE` (WL-SVM), on `LOGLIK` and `LOGLIK_PER_POINT` (WL-GMM), and on all four `STRESS_LOSS_*` (WL-LG), reported alongside the relative deviation of each. `Accuracy` (WL-SVM) = a boolean, exact equality of `TEST_CORRECT` and `numeric_match` on `TRAIN_ACC`/`TEST_ACC`/`TEST_ACC_C1`/`TEST_ACC_C2`; the raw accuracies are additionally published in their natural unit but are not normalized, since every conforming implementation produces the same value by construction."

---

## [MINOR] Finding 13
*Spec section:* prompt.md §12 (do not compare un-warmed JIT against steady-state native); artifact §1.10

**Issue**

FAIRNESS ACROSS ALL TEN — §1.10 produces cold whole-process timings only, and mitigates by publishing a no-op `STARTUP_TIME`. A no-op program's startup does not contain the workload's JIT warm-up, which is what §12's rule is about; for Java, Kotlin and TypeScript/node the hot loops of WL-SVM and WL-LG are interpreted for their first thousands of iterations. `06_micro_workloads.md` §6 solves this structurally for the micro suite by measuring `cold_once` and `steady` separately and never mixing them in one column; this document has no steady counterpart, so the algorithm workloads offer the Standard score only a cold number.

**Required fix**

Add to §1.10: "In addition to the 5 cold whole-process runs, the harness runs each workload in a second frozen configuration in which the entry point executes the whole workload body twice in one process and times only the second execution, reported as `EXEC_WARM` beside `EXEC_COLD`. The configuration is identical for all 10 languages, is selected by the frozen `--warm` flag, prints nothing on the first pass, and is excluded from Peak RSS. No table places a cold and a warm figure in the same column, and each published table names which it contains (spec §12)." If a warm variant is judged out of scope for these workloads, say so explicitly and state that algorithm Execution Time feeds only the cold-defined Standard metric, so the §12 separation is preserved by exclusion rather than by silence.

---

## [MINOR] Finding 14
*Spec section:* artifact §1.1

**Issue**

FAIRNESS / accuracy of a recorded justification — the binary64 deviation is justified because "three of the ten languages cannot express binary32 arithmetic in their standard language core", but only two are named (TypeScript, Python) and only two qualify. Java, Kotlin, Go, Swift, Rust, C++ and Zig all have native binary32, and Quidra does too (`float32` is IEEE-754 binary32, `docs/spec/language.md:11` and `numeric-and-bytes.md:11`). An unnamed third language in a fairness justification inside a frozen document invites exactly the suspicion an auditor is looking for — that a capability was dropped because the language under evaluation lacks it. It did not, but the text cannot be checked as written.

**Required fix**

Replace with: "Two of the ten languages — TypeScript (all numbers are binary64 under JavaScript semantics) and Python (no native single-precision scalar) — cannot express binary32 arithmetic without bit-manipulation shims. The other eight, including Quidra (`float32`), can. Requiring binary32 would impose a language-specific handicap on those two, so all arithmetic is binary64."

---

## [MINOR] Finding 15
*Spec section:* prompt.md §13 (Compile / Parse Success); artifact §1.7, §9

**Issue**

MECHANICAL REPRODUCIBILITY — §1.7 defines `COMPILE_FAIL` for Python and Quidra Interpreter as "the parse/import step fails", and §9 says `Parse Time` "is measured instead" for those two, but no parse step exists in `environment/environment.json` -> `frozen_toolchain_recipes`: `python` has only `run: python3 FILE.py` and `quidra_interpreter` only `run` and `repl`. Without a named command, one analyst measures `python3 -m py_compile`, another measures import time, another records nothing — and the Compile/Parse Success gate that separates COMPILE_FAIL from RUNTIME_FAIL is undefined for two of the eleven measured configurations.

**Required fix**

Name the commands and have them added to the frozen recipes: "`Compile / Parse Success` and `Parse Time` for Python are measured with `python3 -m py_compile FILE.py` (non-zero exit = `COMPILE_FAIL`), and for Quidra Interpreter with the toolchain's non-executing check command (`quidra check FILE.qui` if available, otherwise the parse-only mode recorded in `environment.json`; if neither exists, `Parse Time` is `N/A` with `NA_MEASUREMENT_UNAVAILABLE` and `COMPILE_FAIL` is determined from a run that exits with the interpreter's parse-error status before producing stdout). The parse step is run under the same cold-cache discipline as a compile."

---

## [MINOR] Finding 16
*Spec section:* prompt.md §10.4 / §33 item 53 (validators shown able to pass a correct input and reject a corrupted one); artifact §7, §8

**Issue**

SPEC COMPLIANCE — §8's golden procedure is strong (two independently written oracles, strict-band agreement, closed-form cross-checks, invariant assertions, no measurement before freeze), and §7 requires the comparator's unit tests to pass before any language is measured. But neither requires the negative half of the validation the specification demands elsewhere: nothing shows the comparator can *reject*. A comparator with an inverted predicate or an over-wide tolerance would pass its positive tests, pass the two-oracle check, and then silently classify every Silent Bug as PASS — the single failure mode that would invalidate all three workloads at once.

**Required fix**

Add to §7: "The comparator's frozen unit-test suite must include, per workload: (a) golden-vs-golden → PASS; (b) each field kind perturbed by just under the `numeric_match` band → PASS; (c) each field kind perturbed by just over it → `SILENT_BUG` naming that field; (d) one discrete field altered by 1 → `SILENT_BUG`; (e) a `nan`, an `inf`, a `-0.000000`, a `\r\n` terminator, a missing final newline, a dropped line and an extra line → `OUTPUT_CONTRACT_FAIL`; (f) each §2.6/§3.7/§4.10 invariant violated in isolation → `SILENT_BUG`. The suite is run and its output preserved before the goldens are frozen, and the manifest of §8 records its result hash."

---

## [MINOR] Finding 17
*Spec section:* artifact §3.5, §3.8, §4.7

**Issue**

MECHANICAL REPRODUCIBILITY — three small undefined points in the workload bodies. (1) §3.5 says the log-likelihood is "computed once before the loop and once after every iteration" without saying whether the value after iteration t is the E-step's `L_total` (which belongs to the parameters at the *start* of the iteration) or a fresh evaluation after the M-step; the two readings differ by one EM step, change `DELTA_LOGLIK_LAST`, `MONOTONE` and `CONVERGED`'s input, and differ by ~1/60 of the workload's runtime. I confirmed the post-M-step reading reproduces §3.9 exactly. (2) §4.7's loop records the loss at steps 0/50/100 but never says where `STRESS_GRAD_P0` / `STRESS_GRAD_Q0` are captured (the closed form implies step 0, after `loss.backward()`). (3) §4.8 requires `NODE_TYPES == 6` as "the number of distinct concrete `Function` subtypes the implementation defines", which has no meaning in Zig or Go-without-embedding, where the abstraction is a tagged union or a set of closures rather than subtypes — and it is trivially satisfiable by printing the literal 6, so only the §1.8 review actually checks it.

**Required fix**

(1) §3.5: "Each iteration is E-step then M-step. The log-likelihood reported for iteration t is evaluated *after* that iteration's M-step, with the updated parameters; `LOGLIK_INIT` is the same evaluation on the initial parameters. There are therefore 61 log-likelihood evaluations in total, and `LOGLIK` is the value after iteration 60." (2) §4.7: add `if step == 0: record grad_P[0] and grad_Q[0]` to the pseudocode, immediately after `loss.backward()` and before `optimizer.step()`. (3) §4.8: "`NODE_TYPES` is the number of distinct concrete realizations of the `Function` abstraction the implementation defines — subtypes/classes where the language has them, or variants of the tagged union / entries of the dispatch table where it does not (Zig, Go). It must be 6, each with its own `backward` and its own `typeName()`, and the §1.8 review verifies the count against the source; the printed value alone is not evidence."

---
