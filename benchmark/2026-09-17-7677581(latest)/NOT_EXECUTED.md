# N/A and Not Executed

Spec §33: *"If a technically impossible or unavailable item prevents completion, do not invent a result.
Record it explicitly as N/A or Not Executed with the exact reason."*
Spec §32 forbids reporting an unexecuted benchmark as executed.

Nothing in this document is estimated, extrapolated or inferred. Every item below was **not run**, or was
run and found **not sound enough to publish**, and says which.

---

## 1. Not Executed — LLM Intrinsic Learnability (Primary Evaluation 3)

**Status:** Not Executed. **Scope:** entire evaluation — I1 through I6, all 10 languages, 0 of ~160 cells.

**Reason:** session usage budget. The evaluation requires, per the frozen design in
`methodology/10_intrinsic_design.md`: forward *and* inverse transformers per language per condition,
transformation manifests, round-trip validation, the §10.4 pre-flight triad, Intrinsic Reference Packs at
matched token budgets, then ≥5 seeds for I1/I2, ≥3 transformation sets for I3, and I4/I5/I6 across ten
languages. Estimated ≈160 scored cells plus the transformer infrastructure, on the order of 30M+ tokens.
The session reached its usage limit three times during the run; the remaining budget could not cover it.

**What exists:** the complete frozen design (`methodology/10_intrinsic_design.md`, 1,616 lines), audited
and remediated. A future run can execute it without redoing the design or the integrity work.

**Consequence for the report:** no I1–I6 scores, no LLM Intrinsic Learnability Score, no ranking. The
§28.1 table is published empty with this reason, not filled with placeholders.

---

## 2. Not Executed — LLM Practical Effectiveness (Primary Evaluation 4)

**Status:** Not Executed. **Scope:** entire evaluation — 0 of 290 scored trials.

**Reason:** session usage budget, as above. The frozen allocation in
`methodology/09_llm_run_config.json` requires 290 initial trials (5 per language for Correct@1 and for
each of three Prompt Robustness variations, plus the scenario A/B tasks), each with up to 3 repair turns
driven by real diagnostics — up to 1,160 generations, ≈30M tokens.

**What exists:** the immutable run configuration, written before any scored generation as §6.2 requires,
including the honest record that temperature, top-p and seed are **provider-controlled / unavailable**
in this client and were not emulated. The task set, oracles, weight table and trial-preservation layout
are frozen. Golden outputs for SVM, GMM and LightGrad **are** built and checksummed, so the oracles a
future run needs already exist.

**Consequence:** no §28.2 table, no LLM Practical Effectiveness Score, no ranking, and no
familiarity-drop diagnostic (§10.7), which requires both LLM tracks.

---

## 3. Not Executed — Adversarial / Safety case set

**Status:** Not Executed. **Scope:** 0 of 374 programs (34 frozen case-variants × 11 configurations).

**Reason:** session usage budget.

**Consequence — this one has knock-on effects that must be stated.** The following Standard metrics draw
their evidence from this case set and are therefore **Not Executed**, not merely unscored:

Type Safety · Memory Safety · Runtime Safety · Boundary Value Safety · Adversarial Input Robustness ·
Early Error Detection · Debuggability · Silent Bug Resistance · Implementation Robustness

That is the whole Safety/Robustness category, which carries **25% of the Standard Overall Score** — the
largest single category weight. **A Standard Overall Score is therefore NOT published.** Renormalising
the remaining categories to 100% would silently redefine the metric; §26 permits renormalisation for a
genuinely inapplicable metric, not for one that was simply not run.

**What exists:** the complete frozen case set (`methodology/08_adversarial_cases.json`, 26 hazards × C/R
variants, mandatory program skeleton, 7-class outcome procedure, 8-rung stage ladder).

**One safety finding was obtained outside this set and IS reported:** the Quidra SIGSEGV documented in
`QUIDRA_DEFECT_string_concat_stack_leak.md`, found during MB-08 and traced to
`src/llvm_backend.cpp:770`/`:785`. It is reported as an independent defect, not as a scored Silent Bug
Resistance measurement, because the frozen case set that would make such a score comparable was not run.

---

## 4. Not Executed — Standard rubric metrics (family G)

**Status:** Not Executed. **Scope:** 0 of 17 rubrics × 10 languages.

**Reason:** session usage budget. Each rubric requires primary evidence (E1 execution on this host, E2/E3
official documentation and repositories, E4 package-index counts) gathered identically for all ten
languages, with URLs, timestamps and verbatim excerpts.

**Consequence:** Readability, Functionality/Expressiveness, Diagnostics, Dependency Simplicity,
Portability, FFI/Interoperability, Concurrency, and the entire Ecosystem/Production Maturity category
(20% weight) are unscored. Combined with item 3, **45% of the Standard weight has no evidence**, which is
the second and independent reason no Standard Overall Score is published.

**What exists:** the frozen rubrics (`methodology/05_standard_rubrics.json`, 17 metrics with observable
level criteria and evidence sources), audited and remediated — including the explicit prohibition on any
"young language" allowance that §7 forbids.

---

## 5. Not Executed — SVM / GMM / LightGrad implementations in the ten languages

**Status:** Not Executed. **Scope:** 0 of 60 implementations (3 workloads × 10 languages × 2 scenarios).

**Reason:** session usage budget. These are also the LLM Practical scenario A/B artifacts (item 2).

**What exists, and it is not nothing:** two **independent** reference implementations per workload (C and
Python), verified byte-identical, with frozen checksummed goldens. LightGrad is additionally verified
against **closed-form derivatives** (y=82, y′=113, y″=108, y‴=54, y⁗=0) — checked against mathematics,
not against itself. A future run has working oracles on day one.

---

## 6. Measured but NOT PUBLISHABLE AS A RANKING — Semantic Compression (Primary Evaluation 1)

**Status:** fully executed; ranking **not established**. This is a different category from the items
above and must not be read as "not run".

**What was executed:** 44 probes × 10 languages, every fragment compiled and run on the real toolchain;
a cross-language consistency audit (one agent per probe, all ten languages side by side); a correction
pass applying its findings in both directions; and four independent spotchecks.

**Why no ranking:** all four spotchecks returned *not comparable across languages*, with residual bias
*still favouring Quidra*. The cause is structural, quoted from the Semantic Density spotcheck:

> the ledgers contain essentially only COUNT rows (1.1–3.8% non-recoverable), so **annotation effort,
> not language semantics, sets `fact_count`**

The corrected score table is published as raw evidence with
`semantic-compression/COMPARABILITY_VERDICT.md` attached. The Go / Quidra / Zig ordering
(86.32 / 81.44 / 81.08) is **not resolved** by this measurement: the spread is smaller than the residual
annotation-depth effect. See `methodology/CORRECTIONS.md` **D-8**.

**What would fix it:** a single annotator or one fixed script performing the site-matrix walk to
identical depth in all ten languages, with the per-site row set fixed mechanically before any language is
annotated. That is a re-run of the annotation phase under a stricter protocol, not a re-scoring.

---

## 7. What IS published as a measured result

| Result | Evidence |
|---|---|
| Micro-benchmark correctness, all 11 configurations | 121/121 cells reproduce a golden that two independent references agree on byte-for-byte |
| Micro-benchmark performance (Native, Long-running, Compile, Startup, Memory, Artifact size) | serial timing on an unloaded host, 5 repeats + 2 warm-ups, median per §25.2 |
| Quidra capability gaps | measured directly: no bitwise operators, no function values, no object identity, no concurrency facility, no open extension — each verified against the compiler |
| Quidra implementation defect | reproducible SIGSEGV with root cause in the compiler source |
| Methodology integrity | 10/10 documents found pro-Quidra biased and corrected before any scoring; pre-remediation text preserved under SHA-256 |

---

## Summary of the four primary evaluations

| # | Evaluation | Outcome |
|---|---|---|
| 1 | Semantic Compression | Executed in full; **ranking not established** (comparability) |
| 2 | Standard | **Partially executed**: performance measured; Safety/Robustness (25%) and Ecosystem rubrics (20%) Not Executed → **no Overall Score** |
| 3 | LLM Intrinsic Learnability | **Not Executed** (budget) |
| 4 | LLM Practical Effectiveness | **Not Executed** (budget) |

No cross-evaluation combined score or overall ranking is published — prohibited by spec §6, §32 and §35
independently of anything above.
