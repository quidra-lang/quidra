# Audit findings for `05_standard_rubrics.json`

**Verdict:** needs_fix  
**Quidra bias found:** True  

## [BLOCKER] Finding 1
*Spec section:* 7, 8 (Ecosystem / Production Maturity), 32

**Issue**

THE ONE REAL QUIDRA TILT: the "first-party only" publisher gate on Tooling (T1-T6), Build/Test Integration (V1-V5) and IDE/Editor Support (I1-I6) erases exactly the third-party ecosystem that constitutes incumbency, which spec 7 forbids ('If an established language objectively has better package availability, IDE support, debugger integration ... that advantage must be scored normally rather than discounted as an incumbency effect'). Worked examples against the frozen official_project_table: Java's official set is OpenJDK+Oracle, so Maven, Gradle, JUnit, google-java-format and jdt.ls are all third-party -> T1 no, T2 no, T4 no, T6 no, leaving T={-Xlint, javadoc}=2 -> Tooling 20, and V1/V3/V4/V5 all fail -> Build/Test 0-20. C++'s set is WG21+LLVM+Apple, so CMake, Ninja, Doxygen, vcpkg, Conan and gtest do not count -> T~2-3. Python's set is PSF/CPython, which publishes no LSP, so I1 fails and I2/I3 (which are written as 'that implementation's ... feature list') fail with it -> IDE 0-20. Now compare Quidra 0.2.0, whose single binary ships build, fmt, check, package (with quidra.lock + SHA-256 tree pinning) and lsp, all published by quidra-lang: Quidra plausibly scores Tooling 40-60 and IDE 20, i.e. at or above Java, C++ and Python on the three metrics whose entire purpose is to measure ecosystem maturity. The Ecosystem category is 20% of Standard Overall and Tooling + Build/Test + IDE are 3 of its 10 equally weighted members, so this is ~6 points of Standard Overall handed to a monolithic-CLI young language purely by the definition of admissible publisher. Note the gate is not spec-derived: spec 8 names the metric 'Tooling', not 'First-party Tooling', and spec 25.1.G only requires 'a published objective rubric or proxy with explicit observable criteria'.

**Required fix**

Widen the admissibility test for T1-T6, V1-V5 and I1/I4/I6 from 'published by a member of official_project_set' to a frozen, language-neutral three-clause test applied identically to all 10: a tool is admissible if (a) it is published by a member of official_project_set, OR (b) the language's official documentation (E2) names it as a/the way to perform that task, OR (c) the official implementation repository's own CI configuration (E3) invokes it. Record which clause each tool satisfied. This admits Maven/Gradle/JUnit for Java, CMake/vcpkg/gtest where clang or Apple docs name them, Prettier/npm for TypeScript, and pytest/pip where python.org names them, without admitting arbitrary third-party tools, and it leaves every Quidra indicator exactly where it is. Additionally, decouple I2/I3 from I1 by scoring them against the best language server admissible under the widened test, and state explicitly that if no admissible server exists, I1-I3 are all unsatisfied.

---

## [BLOCKER] Finding 2
*Spec section:* 25.1.G, 25.4

**Issue**

Dependency Simplicity's level set is NOT exhaustive, contradicting global_rules.application_order.5 ('Levels are mutually exclusive and exhaustive by construction'). The case D_total = 0 AND offline_build FAILS matches no level: 100 and 80 both require offline_build to succeed; 60, 40 and 20 all require D_total >= 1; and 0 requires D_total > 50 or an unpinnable-dependency build failure, which cannot apply when D_total = 0. A language whose stdlib-only corpus builds online but fails the cache-emptied offline check (a realistic outcome for Kotlin via the Gradle/daemon path, for TypeScript if tsc is resolved through npm, or for any toolchain that touches the network on first build) leaves the scorer with no defined score and therefore a free judgement call at scoring time, which spec 25.4 prohibits.

**Required fix**

Amend the level-20 criteria to read: '11 <= D_total <= 50, OR (1 <= D_total <= 50 AND pinning is not available), OR (1 <= D_total <= 50 AND offline_build fails), OR (D_total = 0 AND offline_build fails)'. Then re-verify exhaustiveness for every (D_total band x pinning x offline_build x install_units) combination and record the verification in the document.

---

## [BLOCKER] Finding 3
*Spec section:* 25.1.G, 33.42

**Issue**

The commands needed to verify roughly a third of the indicators are not frozen anywhere. global_rules.host_verification.build_recipes mandates using environment.json -> frozen_toolchain_recipes 'unchanged, except where an indicator explicitly requires adding one documented first-party flag', but those recipes are single-file invocations (clang++ -std=c++20 -O2 FILE.cpp -o BIN, rustc -O FILE.rs -o BIN, javac -d OUT FILE.java, tsc FILE.ts, zig build-exe ...). No recipe exists for: V1/V3/V4/V5 (multi-module manifest build, test discovery, coverage, machine-readable test output - i.e. cargo/go test/swift test/npm), DEP-1's cache-emptied offline build, FFI-2 (producing a linkable library: --crate-type=cdylib, -emit-library, build-lib, plus the clang link line), FFI-4 (binding generator), TEST-2 (two modules plus a test module), CONC-1 (thread/link flags), and PORT-1 (cross-target flags). Under a literal reading every one of those indicators is 'a check that cannot be executed' and global_rules.indicator_model.unverifiable_rule turns it into NOT SATISFIED for all 10 languages - which silently zeroes Build/Test Integration and caps FFI at the level reachable from FFI-1 alone. Under a loose reading each scorer invents commands, and two analysts get different numbers. Either way the document is not mechanically reproducible.

**Required fix**

Add a frozen 'rubric_verification_recipes' block to this document, parallel to frozen_toolchain_recipes, giving the exact command line per language per probe family (TEST-1/TEST-2 build+run+coverage+machine-readable output, DEP-1 cache-clear and offline build, FFI-1..FFI-4 build and link, CONC-1 1-worker and 4-worker build, DBG-1 debug build and debugger/profiler invocation, PORT-1 cross-target). Where a language genuinely has no such command, write 'none - indicator unsatisfied by construction' in the table now, before scoring, rather than discovering it at scoring time. Then rewrite host_verification.build_recipes to point at that table instead of at the single-file recipes.

---

## [BLOCKER] Finding 4
*Spec section:* 25.4, 32

**Issue**

Ecosystem Breadth's fixed_search_procedure defers its single most outcome-determining input to scoring time: 'The identical keyword list is used for all 10 languages and must be written into the evidence record before the first language is searched.' The keywords are not in this frozen document. Domain coverage for 8 of the 12 domains depends entirely on which string is typed into each index's search box ('http' vs 'http server' vs 'web framework' returns different first results and different coverage verdicts), so an independent analyst cannot reproduce N. This is aggravated by global_rules.application_order.3, which fixes evidence collection in the fixed language column order - and Quidra is column 1. The keyword list is therefore chosen while looking at Quidra's standard library, which is exactly the derivation this document's own fairness_declaration forbids.

**Required fix**

Enumerate the 12 exact query strings in this document now, one per domain id B01-B12, plus the exact sort order and the tie-break when two results are equally ranked. Separately, change application_order.3 to collect evidence for Quidra LAST (or in a recorded randomized order), so that no threshold, keyword or procedure can be tuned against Quidra's observed result first.

---

## [MAJOR] Finding 5
*Spec section:* 25.1.G

**Issue**

'a maintainer of a major implementation of that language' is load-bearing in E5 and in I1, I4, I6, Q6, C5, F5 and Library Availability clause (iii), and is nowhere defined. 'Major' is precisely the holistic judgement the document claims to have eliminated ('There is no holistic judgement step anywhere in this document'). Concretely: does Eclipse (ecj/jdt.ls) count as a major Java implementation, making Java's IDE score 100 instead of 20? Does Microsoft count for C++ (MSVC), admitting vcpkg for T6 and the C/C++ extension for I4? Does Pyright/Pylance count for Python? Each answer moves a metric by 20-80 points, and each is currently the scorer's call.

**Required fix**

Add a 'major_implementations' array to each language's entry in official_project_table, frozen now, listing the implementations admitted for this run (e.g. C++: LLVM/clang, GCC, MSVC; Java: OpenJDK, Eclipse ecj; Python: CPython, PyPy; TypeScript: microsoft/TypeScript). Replace every occurrence of 'a maintainer of a major implementation' with 'a maintainer of an implementation listed in that language's major_implementations', and add major_implementations to the prohibited_at_scoring_time list.

---

## [MAJOR] Finding 6
*Spec section:* 4, 25.1.G, 32

**Issue**

The Concurrency metric contains a contradiction that prejudges a competitor's result before measurement. Probe CONC-1 says 'split across 4 workers using first-party parallelism' - Python's multiprocessing is first-party stdlib parallelism and would comfortably clear the 1.5x speedup bar on an 8-CPU host - but the metric's na_policy asserts the opposite outcome as settled fact: 'a runtime-level restriction on parallel execution (such as a global interpreter lock in the installed interpreter build) causes C1 to be unsatisfied on the measured evidence'. As written, whether Python scores 100 or 80 on Concurrency is decided by which of the two sentences the scorer reads first. (environment.json also records only 'Python 3.14.5' without saying whether the free-threaded build is the one under test, which independently changes C1.)

**Required fix**

Amend CONC-1 to state the worker model explicitly - either 'workers may be OS threads, processes, or any other first-party parallelism mechanism; record which was used' or 'workers must share one address space' - and then delete the prejudging clause from na_policy, replacing it with the neutral 'C1 is satisfied or not by the measured wall-clock ratio; no language is presumed to pass or fail it.' Also record in environment.json which CPython build (GIL or free-threaded) is under test.

---

## [MAJOR] Finding 7
*Spec section:* 8 (Readability), 25.1.G, 33.42

**Issue**

Readability R3-R6 are not yet mechanically determined. measurement_definitions delegates them to benchmark/2026-09-17-7677581/scripts/readability_proxy.py, which does not exist (scripts/ currently holds only check_micro.py, langs.py, make_token_profiles.py, measure.py, score.py, token_profiles.json, tokenize_probe.py, verify.py), and the per-language inputs it needs - the comment/string lexing table and the list of constructs that 'the language's official grammar states introduces a new nested statement sequence' - are explicitly deferred to the evidence record rather than frozen here. Two further free variables: (i) R3/R4/R6 are properties of code the benchmark authors write, and the 'no routine exceeds nesting depth 6 / 100 lines' conjuncts flip on a single outlier routine; (ii) corpus_definition.preprocessing measures languages that satisfy R1 on formatter output and the rest on hand-written source, so the measurement basis differs per language, and R1 partly determines R3 by construction (formatters enforce line-length limits), double-counting one property across two indicators of the same six.

**Required fix**

Freeze the per-language lexing table (comment syntax, string/char literal syntax, block-introducing constructs) inside this document, write readability_proxy.py before scoring and record its SHA-256 here, and change R4/R6 from 'no routine exceeds N' to a distribution statistic robust to one outlier (e.g. 'p95 per-routine nesting depth <= 5', 'p95 routine length <= 60'). Either drop R1 from the six scored indicators (keeping it only as the preprocessing switch) or state in proxy_limitations that R1 and R3 are correlated by construction. Independently: it is worth recording that I tested the one indicator that looked like a syntax-shape tilt - R5 sigil density <= 0.30 - by measuring the definition over ~690 KB of the Quidra compiler's own C++ (0.188) and Quidra's examples (0.171). Both are far below 0.30, so R5 is very likely satisfied by all 10 languages and carries no Quidra tilt; it also carries no information, and should either be tightened to a discriminating threshold or removed and replaced.

---

## [MAJOR] Finding 8
*Spec section:* 7, 8 (Diagnostics), 25.1.G

**Issue**

The Diagnostics metric measures the frozen recipes' warning configuration as much as the toolchain. The recipes carry no warning flags at all (clang++ -std=c++20 -O2 with no -Wall, tsc FILE.ts with no --strict, rustc -O, javac with no -Xlint), and build_recipes permits adding a flag only 'where an indicator explicitly requires' it - no indicator requires warning flags. DIAG-5 (unused local that otherwise compiles) therefore produces no diagnostic at all for C++ and several others, failing all of D1-D5 for that probe on a flag choice rather than on diagnostic quality, while Go (which makes it an error) passes. Separately, D1 requires 'the source file, the line number, and the column (or character offset)' while D2 separately rewards a caret excerpt: javac prints file:line plus a caret but no numeric column, so whether Java satisfies D1 is undefined and worth 20 points.

**Required fix**

Add to this document a frozen 'diagnostic configuration' row per language stating the exact warning configuration used for the DIAG probes, chosen on one stated neutral principle applied to all 10 (recommended: the configuration the language's own official documentation presents as the normal development configuration - -Wall -Wextra for clang, --strict for tsc, -Xlint:all for javac, default for Go/Rust/Zig/Swift) and record the principle. Add to D1 the clarifying sentence: 'A numeric column or character offset must be printed; a caret or source-excerpt indication alone satisfies D2 but not D1.'

---

## [MAJOR] Finding 9
*Spec section:* 4, 7, 25.1.G

**Issue**

Host-provisioning accidents are converted into language scores with no way to tell them apart. indicator_model.unverifiable_rule scores any non-executable check as NOT SATISFIED, and no_installation_of_extra_software forbids installing anything - so Tooling's H count (worth the 100-vs-80 step), Package/Dependency Management's INSTALLED boolean (also the 100-vs-80 step), and Q2/Q3/Q4 (debugger session, aggregate rendering, CPU profile) turn on how this particular macOS/Homebrew host happens to be provisioned. Rust is installed from Homebrew rather than rustup, so whether rustfmt/clippy/cross-targets are present is a packaging accident; perf and valgrind do not exist on macOS/arm64, so Q4/Q5 outcomes for C++, Rust and Zig reflect the host OS. The asymmetry is that Quidra is the one language built from local source by the benchmark team with its maintainer present, so a Quidra indicator is never 'unverifiable' for provisioning reasons - the conservative rule is uniform in wording but not in effect.

**Required fix**

Add a required evidence field distinguishing three states: 'absent from the language's official distribution', 'present in the official distribution but absent from this host's repackaged install', and 'blocked by the host OS'. Score H and INSTALLED on the first state only (a tool documented as shipping with the official distribution counts as present when it is missing solely because of a repackaged install), and publish a 'host-limited indicators' table alongside the Standard results listing every indicator recorded unsatisfied for the second or third reason, per language, so readers can see which scores are host artefacts.

---

## [MINOR] Finding 10
*Spec section:* 7, 8 (Portability)

**Issue**

Portability P6 accepts 'a published language specification/reference standard ... that an independent implementation could be written against' with no quality, normativity or independence test. Quidra's docs/spec/language.md plus docs/spec/grammar.ebnf, self-published in the implementation's own repository and reviewed by nobody outside it, satisfies P6 on its face and scores identically to ISO/IEC 14882 for C++ or the Java Language Specification. That is 20 points on a Language/Development metric handed to a 0.2.0 language on the strength of having written markdown about itself.

**Required fix**

Split P6 into two indicators of the same weight: P6a 'a normative language specification exists that the implementation's own documentation designates as normative and that contains a conformance statement, OR the specification is published by a standards body independent of the implementation'; P6b 'at least two independently developed implementations exist and are acknowledged by official documentation'. Record which of the two each language satisfies.

---

## [MINOR] Finding 11
*Spec section:* 8 (Toolchain Stability / Release Maturity), 7

**Issue**

Two of the six Toolchain Stability indicators measure recent activity rather than maturity and are trivially satisfiable by a weeks-old project: Y1 needs only 3 tagged releases and Y5 only 2 releases in 12 months. A pre-1.0 toolchain can therefore reach 40 ('Developing') on a metric named Release Maturity while Go, Java and Python reach 100 - the gap understates a ~30-year difference in release history. This is not the forbidden 'young-language allowance' (the document contains none, and na_policy says so explicitly), but the indicator set does not discriminate maturity well at the low end.

**Required fix**

Raise Y1 to 'at least 10 distinct released versions with retrievable release notes, tags or dated download artifacts' and add a Y7-style age ladder in place of the binary Y3 (e.g. replace Y3 with 'at least 10 years elapsed since first public release'), keeping Y3's 3-year threshold as part of the level-20 floor. Apply the amended ladder identically to all 10 and re-verify the level bands still partition 0-6 satisfied indicators.

---

## [MINOR] Finding 12
*Spec section:* 8 (Dependency Simplicity), 25.1.G

**Issue**

Dependency Simplicity is very likely non-discriminating and its only live variable is a packaging accident. Because spec 7 bars outsourcing the core computation to external libraries, the E7 corpus is standard-library-only for every language, so D_total = 0 across the board and the metric collapses onto install_units - whose counting rule ('counting one official distribution as one unit') is undefined for the ambiguous cases that decide it: Xcode CLT bundles clang, Swift and XCTest (1 unit?), kotlinc needs a separate JRE (2), tsc needs node (2). Kotlin and TypeScript lose 20 points for how their vendors package downloads, not for dependency complexity. Note also that this is the one Language/Development metric where having a rich library ecosystem can only be a liability, which structurally favours a language that has none.

**Required fix**

Freeze the install_units count for all 10 languages in a table in this document now, together with the explicit counting rule ('a single downloadable distribution providing both compilation and execution of the corpus = 1; a language whose compiler requires a separately downloaded runtime or SDK = 2'), and add a sentence to the metric stating that D_total is expected to be 0 for all languages given the stdlib-only corpus, so readers are not misled into reading the resulting near-uniform scores as a measured ecosystem difference.

---

## [MINOR] Finding 13
*Spec section:* 8 (Ecosystem Breadth), 32

**Issue**

Two unresolved questions in Ecosystem Breadth, both of which move Quidra. (a) domain_satisfaction_rule admits a library that is 'part of the first-party standard library (E2)', but environment.json declares two Quidra first_party_packages (quidra-lang/dnn and quidra-lang/vision, pinned by SHA) that are first-party yet not part of the toolchain distribution; the Functionality metric excludes such packages explicitly, this metric does not. (b) Three of the twelve domains - B04 numerical/linear algebra, B05 machine learning, B06 image loading - are precisely the three areas where Quidra has first-party coverage (stdlib tensors and rank-2 matmul, the `image` module with PNG/JPEG/BMP/TIFF/WebP, and the dnn package), which is the difference between Quidra scoring 20 and 40 on this metric. The 12-domain list is asserted neutral rather than derived from any external source.

**Required fix**

State in domain_satisfaction_rule that 'first-party standard library' means only modules shipped inside the toolchain distribution under test, and that environment.json's first_party_packages do NOT count for any language unless they are distributed with the toolchain - matching the Functionality metric's rule. Separately, cite the provenance of the 12-domain list against an external pre-existing taxonomy (e.g. the top-level category list of a major package index, recorded with URL and accession date) rather than asserting neutrality, so the selection is auditable.

---

## [MINOR] Finding 14
*Spec section:* 25.3, 8.1

**Issue**

With 6 discrete levels {0,20,40,60,80,100} across 17 metrics, exact ties are not an edge case but the expected outcome - and spec 25.3's tie rule ('Ranking comparisons use the unrounded values; displayed rounding must not decide ties') does not help, because the unrounded values will be genuinely equal. The document gives no tie-handling rule, leaving a tie-break to be invented after results are visible, which spec 25.4 forbids. Related: the declared X19/X22 overlap is described as spanning 'Concurrency and Build/Test Integration', but Functionality/Expressiveness and Concurrency are BOTH in Language/Development, so X19 is double-counted inside a single category rather than across two.

**Required fix**

Add a global rule: 'Where two or more languages have identical Standard Overall Scores at full precision, they are reported at equal rank with the shared rank number, and no tie-break is applied.' Amend overlap_declaration to state correctly that X19's double count falls within the Language/Development category (not across categories), and quantify it: concurrency capability contributes to 2 of that category's 8 equally weighted metrics.

---
