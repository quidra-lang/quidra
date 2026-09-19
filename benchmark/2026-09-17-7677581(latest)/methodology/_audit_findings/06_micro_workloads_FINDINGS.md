# Audit findings for `06_micro_workloads.md`

**Verdict:** needs_fix  
**Quidra bias found:** True  

## [BLOCKER] Finding 1
*Spec section:* 06 §4 MB-00 + §8.1 Step 2; prompt §8 (Startup / REPL Latency), §11, §26

**Issue**

QUIDRA BIAS — the REPL sub-probe is worth half as much for Quidra as for every other REPL-bearing language. MB-00 defines a language's Startup/REPL raw value as the arithmetic mean of its available sub-probe medians, and §8.1 Step 2 then averages Quidra's two modes. Quidra Native has no REPL (P1 only); Quidra Interpreter contributes (P1+P2)/2. So Quidra_raw = (P1_native + (P1_int+P2_int)/2)/2, i.e. P2 carries weight 0.25, whereas Python/TypeScript/Java/Kotlin/Swift carry P2 at weight 0.5. Since P2 >> P1 for every toolchain (jshell/kotlinc/swift repl are seconds; a bare exec is milliseconds), the two-mode averaging systematically lowers Quidra's raw on a lower-is-better metric purely through the aggregation shape. Quidra's REPL compiles every submission natively (README: each submission is parsed, specialized, checked, lowered to LLVM IR, compiled and executed), so this is exactly the metric where dilution pays. The document's own claim that the rule 'neither rewards nor punishes having a REPL' is false on its face — adding a larger P2 into an arithmetic mean always worsens a family-C score.

**Required fix**

Replace the 'mean of available sub-probe medians' rule with sub-score averaging: normalize P1 across all languages with family C, normalize P2 across all languages with family C, then average the normalized sub-scores. Define Quidra's P1 as the mean of the two modes' P1 (both modes genuinely start programs) and Quidra's P2 as the interpreter's P2 at full weight (Native contributes no P2 because it has no REPL, exactly as C++ contributes none). Write the resulting formula explicitly in §6 so it cannot be re-derived differently.

---

## [BLOCKER] Finding 2
*Spec section:* 06 §4 MB-00 (P2), §9 row 6; prompt §26

**Issue**

MECHANICAL/N-A ESCAPE — 'Startup / REPL Latency' is a metric that intentionally tests REPL behaviour, yet §9 classifies a missing REPL as 'Not applicable — excluded from that language's sub-probe mean only'. That is precisely the escape §26 forbids ('A missing capability must not automatically escape scoring by being labeled N/A'), and it hands C++, Rust, Go and Zig a raw value consisting only of a ~2 ms exec. Worse, the same clause is outcome-dependent: 'If a listed command cannot be executed non-interactively on this host … P2 is treated as non-existent.' `swift repl` and `kotlinc` on piped stdin are exactly the commands most likely to refuse a non-TTY, so a language whose REPL is slow AND unscriptable has its slow number deleted after the fact, improving its score. Nothing in the document verifies that any P2 command except Quidra's works non-interactively (Quidra's is verified by its own README: '`quidra repl` … is useful for automated tests').

**Required fix**

State one rule before running: (a) a language with no first-party REPL in the frozen toolchain receives the rubric-defined worst P2 sub-score (0) rather than an exclusion, or — if the benchmark prefers exclusion — say so and justify it against §26 in writing; (b) a language that HAS a REPL which refuses piped stdin must be driven through a pty (`script -q /dev/null`, `expect`, or `unbuffer`) and its real latency recorded; treating it as non-existent is prohibited. Pre-verify and record the exit status of all six P2 commands before freezing, not during measurement.

---

## [BLOCKER] Finding 3
*Spec section:* 06 §8.1 Step 1 (final row) and §8.2; prompt §11, §32

**Issue**

QUIDRA BIAS — the default mode-role rule ('Any other Standard metric → Native only, unless the metric's definition explicitly concerns interactive/REPL/interpreter behaviour') silently removes Quidra Interpreter from every metric not named in the table: Type Safety, Memory Safety, Runtime Safety, Boundary Value Safety, Adversarial Input Robustness, Early Error Detection, Debuggability, Silent Bug Resistance, Compiler/Interpreter Robustness, Diagnostics, Tooling — i.e. the entire 25%-weighted Safety/Robustness category. 08_adversarial_cases.json actually executes `quidra_interpreter` for every case and publishes a native-vs-interpreter divergence table, so interpreter evidence of defects will exist and will be excluded from scoring by this default. The table has no principled discriminator: Long-running Performance and Memory Efficiency take BOTH modes because 'nothing in the metric's definition restricts it', but the identical argument applies to Runtime Safety and Silent Bug Resistance, where the default flips to Native only. The two justifications in §8.2 are mutually exclusive ('treats Quidra like the other nine, which contribute one configuration' vs 'both modes are in scope where the definition does not restrict them'), and the metrics where the pro-Quidra reading is used are the ones where the interpreter could reveal defects.

**Required fix**

Adopt a single stated discriminator and apply it to the whole table. Recommended, and consistent with 08's already-frozen rule: 'Quidra's scored value for every metric is taken from Native mode, except metrics whose definition is about interactive/interpreter execution (Interactive/Interpreter Performance, the P2 half of Startup/REPL Latency), which are taken from Interpreter mode; Interpreter results are published in full as non-scoring rows for all other metrics, with a divergence table.' If instead both-mode averaging is kept for Long-running/Memory/Runtime Overhead, then apply both-mode combination to Safety/Robustness too, combining defect-class outcomes by the WORSE of the two modes (a defect reachable in either mode is a real defect). Do not mix the two policies metric by metric.

---

## [MAJOR] Finding 4
*Spec section:* 06 §2.5(3,8), §4 MB-05/MB-08/MB-10/MB-11; prompt §7, §12 ('keep fixed: algorithm, input data, workload size')

**Issue**

MECHANICAL REPRODUCIBILITY — the container/representation choice is left to the implementer in at least four workloads, and each choice changes the measured time by a multiple: MB-05 explicitly says Python may use a `list` of floats OR `array('d')` and TypeScript may use `Float64Array` OR `number[]` ('the implementation must use one and record which'); MB-08 offers every language a free choice between its `string` type and a byte/char buffer; MB-10 says 'the language's normal BUFFERED writer/line reader' without naming an API, which is undefined for Node (no synchronous buffered line writer; `fs.createWriteStream` accumulates unboundedly, which the same section forbids) and for Zig 0.16 (the new `std.Io` writer requires an explicit, sized buffer); MB-11 names `std.AutoHashMap`/`std.ArrayList` for Zig but never pins the allocator (DebugAllocator vs SmpAllocator vs c_allocator vs arena differ by several times on 10^6 operations). Two independent analysts will produce different numbers for Python, TypeScript, Zig and Java on these workloads.

**Required fix**

Add a frozen table §4.x 'Pinned data representation and API per configuration' with one row per (workload, config_id) naming the exact type and call: e.g. MB-05 python=`array('d')`, typescript=`Float64Array`; MB-08 every config's exact byte/char sequence type and whether the string→buffer conversion happens once before the timed region; MB-10 the exact writer/reader class per config plus a single frozen buffer size (e.g. 65536 bytes) for every language, with an explicit permission for languages lacking a standard buffered writer to use a manual fixed 64 KiB buffer flushed with the language's ordinary write call; MB-11 zig allocator = `std.heap.c_allocator` (or DebugAllocator) named explicitly. No 'record which' options.

---

## [MAJOR] Finding 5
*Spec section:* 06 §5.2, §5.4, §5.7, §9 (timeout row), §3 footnote 1

**Issue**

The 1800 s timeout is per PROCESS, but a steady process executes the whole workload seven times, so a configuration hits the timeout at one-seventh the per-run cost that cold mode tolerates. The sizing footnote only bounds this against CPython ('7 × 46 s ≈ 324 s worst case') and never considers `quidra_interpreter`, whose per-run cost is unknown and could plausibly be an order of magnitude above CPython on MB-04 — 7 × 460 s = 3220 s, i.e. a guaranteed timeout. Worse, §9 then censors the whole steady measurement at 1800.0 s even though the program has already printed complete `ITER k elapsed_ns` lines to stdout: valid per-iteration data that exists is thrown away and replaced by a censored constant, which is then averaged into Quidra's Long-running raw by §8.1 Step 2.

**Required fix**

Rewrite the steady timeout rule: 'A steady process killed at the per-process timeout contributes every complete ITER line it emitted; the first two per process are discarded as warm-up and the remainder are kept samples. Only if fewer than one kept sample was emitted is the steady raw recorded as censored at 1800.0 s.' Additionally either raise the steady-mode budget to 7 × the cold timeout or reduce steady iterations for configurations whose measured cold median exceeds 1800/7 ≈ 257 s, using a rule fixed now (before results) rather than chosen later.

---

## [MAJOR] Finding 6
*Spec section:* 06 §11 coverage of prompt §11; prompt §8 (Interactive / Interpreter Performance)

**Issue**

SPEC COMPLIANCE — this document claims authority over specification §11 and owns the Quidra mode probes, but two §11-mandated measurements exist nowhere in the methodology set. §11 Native mode requires 'Compile + Execute Time'; §11 Interpreter mode requires 'Repeated Expression Latency' (and 'Error Recovery'). A grep over the whole methodology directory returns no definition of either 'Compile + Execute' or 'Repeated Expression'. MB-00 defines only P1 (program startup) and P2 (first response latency). Separately, §6 declares 'Interactive / Interpreter Performance' out of scope, deferring it to 'the dedicated Native-vs-Interpreter evidence of specification §11' — but no methodology document defines that metric for the other nine languages either, so a scored Standard metric currently has no owner.

**Required fix**

Add to MB-00: a `P3` sub-probe 'repeated expression latency' — feed a frozen script of 20 identical expressions to the REPL, record the per-expression wall time from the 3rd onward, median representative, same 6 configurations, same non-existence rule as P2; and add a derived field `compile_plus_execute = median(compile wall) + median(cold_once wall)` per (workload, config) published in the raw tables (it is a §11 requirement even if it feeds no new metric). Then either define 'Interactive / Interpreter Performance' here (e.g. family C over P2 and P3 for interpreted/REPL-capable configurations, with a stated rule for compiled languages) or name, by filename, the document that owns it.

---

## [MAJOR] Finding 7
*Spec section:* 06 §4 MB-08; prompt §12 ('strings' category), §7

**Issue**

QUIDRA-RELEVANT TILT — MB-08 is the mandated 'strings' category, but all five measured passes operate on an ASCII byte/char buffer, and the language's own string type is exercised only in the untimed build phase. Quidra has no `char` type at all (README/docs: 'There is no char type. Text uses string; a one-byte numeric value uses uint8'), so it cannot index a string by character and must use `bytes`. The workload's design makes that limitation costless and unmeasured, while the category header implies the language's string abstraction was measured. The document justifies this as 'available to all', which is true, but the effect is not symmetric: it neutralises a real, language-specific gap for any language lacking character access.

**Required fix**

Either (a) keep one of the five passes on each language's native string type — e.g. pass 5 (word count) must iterate the language's `string` using its own character/scalar access API, with the per-config API pinned in the representation table — so the category measures what its name claims; or (b) keep the byte-buffer design and rename the reported category 'Strings (byte-level scanning)' with an explicit results note that this workload does not measure the languages' string abstractions, plus a published per-language line stating which representation was used and whether the language offers character indexing at all.

---

## [MAJOR] Finding 8
*Spec section:* 06 'Fairness declaration' (header); cross-reference 00_cross_language_constraints.md §C-1

**Issue**

The fairness declaration states: 'Nothing in this document was derived from Quidra's syntax, operators, type system, standard library, or feature set.' That is contradicted by the sibling frozen document 00_cross_language_constraints.md §C-1, which records that the Park–Miller/MINSTD generator frozen in §2.3 was chosen because a conventional 64-bit wraparound LCG is 'inexpressible in Quidra' ('in Quidra, `state * 6364136223846793005` on a uint64 raises runtime error[INTEGER_OVERFLOW] rather than wrapping'). §2.3's own list of 'properties that make this choice language-neutral' omits the constraint that actually triggered the decision. The choice itself is defensible and is independently justified for Python and TypeScript, but the declaration as written is false and an auditor checking only this file would be misled.

**Required fix**

Replace the sentence with: 'One global choice in this document — the MINSTD generator of §2.3 — was made in the knowledge that Quidra's integer arithmetic is overflow-checked and cannot express 64-bit wraparound (00_cross_language_constraints.md §C-1). The same choice is independently required by Python's arbitrary-precision integers and TypeScript's binary64 `number`, and Quidra's overflow-check cost remains fully exposed in MB-02, MB-03, MB-07 and MB-11 and is probed directly in the §17 adversarial set. No workload, size, or scoring rule was chosen with reference to Quidra.' Add the same cross-reference inside §2.3.

---

## [MAJOR] Finding 9
*Spec section:* 06 §5.6; prompt §8 ('Source size, artifact size, and deployment footprint must remain separate metrics'), §26

**Issue**

SPEC COMPLIANCE — §5.6 assigns Python's Binary/Artifact Size the size of `FILE.py`, i.e. its source bytes, which is the raw value of the separate Source Code Size metric. §8 of the spec explicitly requires those metrics to remain separate; measuring one with the other's raw value double-counts and hands Python a near-certain 100 on artifact size. The same situation (no build product) is then handled in the OPPOSITE way one row below, where `quidra_interpreter` is declared 'not applicable as an artifact'. Additionally, Kotlin's artifact is a `-include-runtime` fat jar (~5 MB) while Java's is bare `.class` files (~2 KB) — a ~1000x gap produced by the recipe pair, not by the languages; the `bundles_runtime` boolean is recorded as metadata but the scored raw is not accompanied by any comparable Java number.

**Required fix**

Use one rule for build-less configurations: 'A configuration with no build product has artifact size = 0 bytes, a legitimate zero, scored with the family-C shifted form and a predeclared `epsilon_artifact` (state the value and derive it from filesystem block size, e.g. 4096 bytes).' Apply it to both `python` and `quidra_interpreter`; never substitute source bytes. For Kotlin/Java, additionally publish `kotlinc -d OUT` class-file bytes and `java` classes-in-a-jar bytes as a paired diagnostic row so the recipe artifact is visible, and state in the results that the scored Kotlin number includes the bundled Kotlin runtime while the scored Java number excludes the JDK.

---

## [MAJOR] Finding 10
*Spec section:* 06 §5.3, §4 MB-00

**Issue**

MECHANICAL REPRODUCIBILITY — §5.3 says wall time is the harness's own measurement around subprocess spawn→exit, and in the same bullet list says CPU time and peak RSS 'come from wrapping the run as `/usr/bin/time -l <run command>`'. It never says whether the five timed runs are the wrapped ones or a separate unwrapped set. That matters most where it is least tolerable: MB-00 P1 medians are in the 2–40 ms range and feed a scored family-C metric, while `/usr/bin/time`'s own fork+exec adds a per-run constant of the same order as the fastest configurations' entire startup. Two analysts implementing this paragraph differently will rank the fast starters (quidra_native, cpp, rust, go, zig) differently.

**Required fix**

Write explicitly: 'Wall time is measured in a dedicated set of 5 unwrapped runs. CPU time and peak RSS come from a separate set of 5 runs wrapped in `/usr/bin/time -l`, whose wall times are discarded. MB-00 P1/P2/P3 are always unwrapped.' Also record `/usr/bin/time -l /usr/bin/true` median on the host in environment metadata so the wrapper overhead is documented.

---

## [MINOR] Finding 11
*Spec section:* 06 §5.3, §5.4, §5.9, §10

**Issue**

MECHANICAL REPRODUCIBILITY — several representative statistics are over even-sized sample sets with no stated convention: median of the 10 kept steady samples, median of the 10 startup runs, and the reported `iqr` over 10 samples. 'Median' of an even sample can be the mean of the two central order statistics or the lower one; IQR depends on the quantile method (linear interpolation, nearest rank, Tukey hinges). Different libraries default differently (numpy vs statistics vs R type-7).

**Required fix**

Add to §5.9: 'median = arithmetic mean of the two central order statistics for even n; quartiles and IQR use the linear-interpolation method (numpy.percentile default, R type 7); MAD = median(|x_i − median(x)|) with the same median convention; stdev is the sample standard deviation with ddof=1.'

---

## [MINOR] Finding 12
*Spec section:* 06 §5.5; prompt §25.1 family C ('define epsilon from measurement resolution')

**Issue**

`epsilon_compile = 0.001 s` is justified as 'the resolution below which the harness does not claim precision', but §5.3 states wall time is recorded 'in seconds with 6 decimal places' from `time.perf_counter_ns()`, i.e. the harness claims microsecond resolution. The epsilon is therefore not derived from measurement resolution as §25.1 requires, and it is decisive: with epsilon = 1 ms, a 2 s Kotlin compile scores 0.05 while Python scores 100; with epsilon = 100 ms it scores 4.8. The value is defensible but the stated derivation is not.

**Required fix**

Derive epsilon from a measured quantity and record it: e.g. 'epsilon_compile = median wall time of `/usr/bin/true` over 20 runs on this host, rounded up to the next power of ten (measured: X s)', or justify 1 ms as the observed run-to-run jitter of the fastest build in the set. State the measured number in §10's freeze record.

---

## [MINOR] Finding 13
*Spec section:* 06 §7.2, §9 rows 1–3

**Issue**

MECHANICAL REPRODUCIBILITY — §7.2 permits unlimited repair of a failing hand-written implementation ('it is repaired and re-verified before any timing run') and gives no stop rule for deciding that a language 'cannot produce a conforming implementation', which is the branch that awards a 0 for that workload in every affected metric. Whether Zig or Swift ends up with a 0 on MB-10 or MB-11 is therefore a judgement call an independent analyst cannot reproduce.

**Required fix**

Fix a budget and a stop rule: 'At most 3 independent implementation attempts per (workload, configuration), each logged in micro/IMPLEMENTATION_LOG.md with its diff and failure mode. A configuration is declared non-conforming only when the failure is attributable to a named, cited language or toolchain limitation (quote the compiler diagnostic or the language reference), never to implementer difficulty. If 3 attempts fail without such a citation, the workload is recorded as `implementation_incomplete` and excluded from that metric with a written reason under §26 — not scored 0.'

---

## [MINOR] Finding 14
*Spec section:* 06 §5.7

**Issue**

MECHANICAL REPRODUCIBILITY — the load-average gate says that if the 1-minute load average stays ≥ 2.0 for 10 minutes the harness 'records the batch as `deferred` and moves on'. Nothing in §5.7, §6 or §9 says what a deferred batch scores, whether it is retried at the end of the suite, or what happens if it is never measured. A deferred batch is currently an undefined hole in the per-workload mean of §6.

**Required fix**

Add: 'A deferred batch is re-queued and retried after the last scheduled batch, up to 3 times. A batch still deferred after the final retry is recorded with `na_reason: "host_contention_unresolved"`, excluded from that metric's workload mean for that language only (an infrastructure N/A under §26, documented), and the exclusion is listed in the results file. A deferred batch is never scored 0.'

---

## [MINOR] Finding 15
*Spec section:* 06 §2.2 (FMA bullet), §4 MB-04 and MB-09 correctness notes, §7.3

**Issue**

Two unsupported claims. (a) §2.2 states FMA contraction 'is permitted for every language equally … it is available to all' — it is not: Java and Kotlin are specified to evaluate `a*b+c` strictly and cannot contract, and Rust does not contract by default. The permission is neutral as a policy but its beneficiaries are determined by toolchain, so the FP workloads (MB-04/05/06/09) carry a systematic, undisclosed advantage for the configurations whose compilers contract. (b) MB-04 claims the tolerance is 'roughly 10^6 times wider than the worst-case FMA-contraction difference' and MB-09 claims 'the observed cross-implementation spread from FMA contraction on this field is below 1e-18' — but §7.3 records that BOTH reference implementations excluded contraction (`clang -O2 -ffp-contract=off` and CPython, which has no FMA), so no contraction spread was ever observed. (I independently rebuilt MB-09 with `-ffp-contract=fast` and with `=off`: all eight fields were bit-identical, so the risk is low — but the document's provenance does not support the sentence it writes.)

**Required fix**

(a) Replace 'available to all' with a measured statement: publish a per-configuration boolean `fp_contraction_observed`, determined before measurement by compiling a one-line `a*b+c` probe and inspecting the emitted instruction, and list it as a declared confound beside every MB-04/05/06/09 result. (b) Either add a third provenance build (`clang -O2 -ffp-contract=fast`) to §7.3 and report the actual per-field deltas, or delete the words 'observed' and 'worst-case' and state the tolerance margin as a bound derived from ulp analysis.

---

## [MINOR] Finding 16
*Spec section:* 06 §2.5(1,7), §1 recipe table; prompt §7 ('normal, idiomatic, production-reasonable best practices')

**Issue**

The frozen recipes are not at equivalent optimisation or safety settings, yet §2.5 presents them as symmetric ('no additional flag … in either direction', 'deliberately pessimising any language is prohibited exactly as strongly as optimising one'). Zig runs at `-OReleaseFast`, which disables bounds and overflow checking and is the most aggressive tier; C++ runs at `-O2` (not `-O3`); `rustc -O` is opt-level 2, whereas Rust's production release profile (`cargo build --release`) is opt-level 3; Go takes no optimisation flag at all; Quidra's `quidra build` default tier is unstated. §2.5(7) explicitly blesses Zig's removal of checks while forbidding Rust any unsafe indexing. These are real recipe asymmetries affecting MB-05/06/07/11 most.

**Required fix**

Publish a confound table in §1 with one row per configuration: optimisation tier, bounds checking on/off, integer-overflow checking on/off, LTO on/off, and whether the recipe matches that language's ordinary production build. Reference it from every Performance and Memory results table. If the recipes can still be amended, align Rust to `rustc -C opt-level=3` (its release-profile equivalent) and state Quidra's build tier explicitly; otherwise record `rustc -O` as a documented handicap relative to cargo release.

---

## [MINOR] Finding 17
*Spec section:* 06 §4 MB-11 fallback, §5.2 fallback, §9 row 5; prompt §26

**Issue**

Two fallbacks are written as if they applied evenly to all eleven configurations, but both are inert for the ten incumbents and exist only for Quidra — and the document does not say so. (a) The MB-11 'no standard hash map' fallback prescribes a hand-written open-addressing map with a multiplicative hash and no boxing; such a map is materially FASTER than Java/Kotlin's boxed `HashMap<Long,Long>` and Rust's SipHash `HashMap`, which the same section forbids those languages from replacing. As written, a language that lacks a standard associative container would be rewarded for lacking it, which inverts §26's intent even though the workload is technically 'not N/A'. (b) The §5.2 'no monotonic clock' fallback likewise cannot fire: Quidra has `time.now()` on a monotonic clock. Both fallbacks are dead code for this run and should say so rather than imply a live symmetric rule.

**Required fix**

Add one sentence to each: 'Verified before freezing: all eleven configurations have a standard hash map, hash set and growable array (Quidra: `map.Map<K,V>`), so this fallback does not fire in this run' and 'Verified before freezing: all eleven configurations expose a monotonic clock (Quidra: `time.now()`), so the steady proxy does not fire in this run.' Additionally, tighten the MB-11 fallback so it cannot reward a deficit: 'A configuration scored via the fallback is published in a separate, clearly marked row and is additionally recorded as a missing-capability result in Capability Coverage; it does not enter the Collections comparison as if the language shipped the container.'

---

## [MINOR] Finding 18
*Spec section:* 06 §9 row 2; prompt §13, §8 (Silent Bug Resistance), §32

**Issue**

§9 routes a hand-written micro implementation that exits 0 with wrong output into the scored Silent Bug Resistance metric ('the occurrence is recorded for the Silent Bug Resistance metric owned by the adversarial methodology'). These implementations are written by the benchmark author, not produced by the language or by an LLM under test, so an author's arithmetic slip in, say, the Swift MB-07 merge would be scored against Swift's safety. The adversarial methodology (08) has its own frozen case set and stage table; importing uncontrolled micro-suite failures into it contaminates a 25%-weighted category with authorship error.

**Required fix**

Change the row to: 'Recorded as `implementation_silent_mismatch` in micro/IMPLEMENTATION_LOG.md and published. It enters Silent Bug Resistance only if 08_adversarial_cases.json's own classification criteria independently classify the observed behaviour as a Silent Bug for that language; otherwise it is an implementation failure and is excluded from every Safety/Robustness metric.'

---

## [MINOR] Finding 19
*Spec section:* 06 §9 row 1

**Issue**

Wording defect with a scoring consequence: 'the workload is excluded from `best_positive_raw`'. `best_positive_raw` under §25.1 is the minimum over applicable LANGUAGES within one workload; excluding 'the workload' is not a defined operation and could be read as dropping the workload from every language's mean.

**Required fix**

Rewrite as: 'that language's raw value for that workload is not eligible to be `best_positive_raw`, and the workload remains in every other language's per-workload set and in the unweighted mean of §6.'

---
