# Ecosystem Specification

This is the authoritative detailed specification for **Primary Evaluation 4 — Ecosystem** in new benchmark runs.

## Required runner coverage IDs

The deterministic runner assigns these IDs to frozen work units before measurement. Across non-aggregation work units, every ID below must be covered by the current run plan. These IDs are orchestration metadata; they do not change the scoring definition.

- `gate.objective_rubrics_frozen`
- `gate.evidence_window_frozen`
- `coverage.all_10_languages`
- `metric.third_party_library_availability_domain_coverage`
- `metric.package_ecosystem_activity_maintenance`
- `metric.third_party_tool_availability`
- `metric.production_adoption_deployment_evidence`
- `metric.community_public_knowledge_availability`
- `metric.core_tooling_availability_quality`
- `metric.package_dependency_management_quality`
- `metric.ide_editor_support_quality`
- `metric.debugger_profiler_support`
- `metric.build_test_integration`
- `metric.documentation_quality`
- `metric.installation_distribution_experience`
- `metric.toolchain_stability_release_maturity`
- `metric.implemented_platform_coverage`
- `metric.external_integration_coverage`

The deterministic plan is validated mechanically before `manifest-merge`; missing or unknown requirement IDs are fatal pre-measurement errors.


Ordinary workers receive only the compact worker rules, frozen Primary configuration, assigned requirement IDs, and selected sections of this specification needed for those requirements. They do not need the root prompt, root conversation, historical runs, sibling outputs, or other evaluation specifications.

The frozen Primary configuration overrides only replication/execution counts. Evaluation meaning, language-neutral fairness rules, metric definitions, formulas and capability universes below remain binding unless explicitly changed before the run.

## 8.2 Ecosystem Evaluation

This section implements **Primary Evaluation 4 — Ecosystem**.

Ecosystem measures the surrounding infrastructure, maturity and real-world evidence that substantially accumulate around a language over time. It is deliberately separate from Language Quality so that intrinsic language/implementation quality is not conflated with age, installed base, surrounding investment or network effects.

A young language may score poorly here even when its Language Quality is high. That is intentional: Ecosystem is the place where present-day maturity disadvantages remain visible.

Evaluate two fixed categories.

### A. External Ecosystem / Adoption

- Third-party Library Availability / Domain Coverage
- Package Ecosystem Activity / Maintenance
- Third-party Tool Availability
- Production Adoption / Deployment Evidence
- Community / Public Knowledge Availability

### B. Toolchain / Developer Ecosystem Maturity

- Core Tooling Availability / Quality
- Package / Dependency Management Quality
- IDE / Editor Support Quality
- Debugger / Profiler Support
- Build / Test Integration
- Documentation Quality
- Installation / Distribution Experience
- Toolchain Stability / Release Maturity
- Implemented Platform Coverage
- External Integration Coverage

Keep mixed concepts split from Language Quality:

- language-level portability / platform-neutral design is scored in Language Quality; implemented target/platform breadth is scored here;
- FFI / interoperability design is scored in Language Quality; breadth and reliability of working external integrations is scored here;
- dependency simplicity/semantics are scored in Language Quality; package-manager maturity and package ecosystem activity are scored here;
- code-level diagnostics/debuggability are scored in Language Quality; debugger/profiler product maturity is scored here.

Do **not** move these maturity, availability, adoption or network-effect factors back into Language Quality merely because some of them are first-party. Whether a capability is first-party or third-party is not the boundary; whether it primarily measures intrinsic language/implementation quality or accumulated surrounding maturity is.

For each Ecosystem metric, define and freeze an objective rubric or proxy before scoring any language. The same evidence sources, snapshot date or observation window, query rules, thresholds, and 0–100 conversion must be applied unchanged to all 10 languages.

For **Toolchain Stability / Release Maturity**, language age, first-release date, and elapsed years are contextual metadata only. They must not be a threshold, direct score, or automatic penalty/bonus. Score frozen, currently observable evidence such as release reproducibility, versioning/support policy, compatibility guarantees, supported artifact availability, maintenance/release cadence, and documented stability commitments, using the same rubric for all languages.

### Frozen runner-owned rubric contract

The authoritative Ecosystem scoring rubric is the machine-readable file:

`template/methodology-assets/ecosystem/rubrics.json`

It is frozen before evidence collection and contains exactly five equally weighted components for every Ecosystem metric, one shared 0–4 evidence level scale, the level-to-points mapping, the metric-specific evidence selection rule, and the common evidence-window/retrieval policy. Language-specific workers **must not invent, tune, or replace** these rubrics.

For every assigned metric, a language worker returns semantic evidence only:

- `rubric_id`: exact ID from the frozen asset;
- `component_levels`: exactly the five frozen component IDs, each an integer 0–4;
- `component_findings`: exactly the same five IDs, each with a concise evidence-based finding;
- `sources`: non-empty source identifiers or URLs supporting the findings;
- `snapshot_date`: the evidence snapshot date;
- `limitations`: known evidence limitations.

The trusted runner validates this structure and mechanically computes the 0–100 metric score from the frozen level-to-points mapping. A worker-supplied normalized score is not authoritative and is overwritten. Therefore the LLM performs the part that requires semantic judgment—finding and classifying evidence—while the runner owns rubric identity, component universe, weights, arithmetic, score range, and cross-language consistency.

A missing applicable capability is scored through the frozen rubric rather than silently changed to `N/A`. If a worker changes the rubric, omits a component, uses a non-integer/out-of-range level, or provides no evidence source, its result is invalid and must be retried rather than scored.

### Predeclared sampling

Do not select evidence by "first search result", "first alphabetical hit", or any other retrieval-order accident.

For every domain/library sampling cell, freeze a **named target or deterministic external selection rule before evidence collection**. Prefer criteria independent of the benchmark result, such as a declared package-index popularity/download rank within the domain, a library explicitly recommended by the language's official documentation, or another language-neutral externally observable criterion.

Record the full candidate universe or query needed to reproduce the selection, the selected target, the selection rule, retrieval route and snapshot time. Use the same number of retrieval routes and the same examination depth for every language.

Raw popularity indicators such as GitHub stars, search-result counts, download counts, or package counts must not be used as a single standalone proxy for the entire evaluation. They may be used as declared evidence within an individual metric when collected consistently for all languages and accompanied by the limitations of that proxy.

The two Ecosystem categories have fixed equal weight:

| Ecosystem category | Weight |
|---|---:|
| External Ecosystem / Adoption | 50% |
| Toolchain / Developer Ecosystem Maturity | 50% |

Within each category, every listed normalized metric has equal weight unless this specification explicitly defines a more specific sub-metric aggregation.

Calculate each category as the arithmetic mean of its applicable normalized metrics, then calculate:

**Ecosystem Score = 0.50*ExternalEcosystemAdoption + 0.50*ToolchainDeveloperEcosystemMaturity**

Do not alter these category weights after measurements begin.

Unsupported or absent ecosystem evidence intentionally covered by a metric receives the rubric-defined low score rather than `N/A`. Apply Section 26 only to genuinely inapplicable cases.

If any applicable Ecosystem metric is `Not Executed`, mark Primary Evaluation 4 `PARTIAL` and do **not** calculate or publish Ecosystem Score or Ranking.

---

## 28.2 Primary Evaluation 4 — Ecosystem

Produce an Ecosystem table with the following fixed columns:

| Metric | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Third-party Library Availability / Domain Coverage | | | | | | | | | | |
| Package Ecosystem Activity / Maintenance | | | | | | | | | | |
| Third-party Tool Availability | | | | | | | | | | |
| Production Adoption / Deployment Evidence | | | | | | | | | | |
| Community / Public Knowledge Availability | | | | | | | | | | |
| Core Tooling Availability / Quality | | | | | | | | | | |
| Package / Dependency Management Quality | | | | | | | | | | |
| IDE / Editor Support Quality | | | | | | | | | | |
| Debugger / Profiler Support | | | | | | | | | | |
| Build / Test Integration | | | | | | | | | | |
| Documentation Quality | | | | | | | | | | |
| Installation / Distribution Experience | | | | | | | | | | |
| Toolchain Stability / Release Maturity | | | | | | | | | | |
| Implemented Platform Coverage | | | | | | | | | | |
| External Integration Coverage | | | | | | | | | | |
| **Ecosystem Score** | | | | | | | | | | |

Create an independent **Ecosystem Ranking** based on Ecosystem Score only when the Ecosystem evaluation status is `COMPLETE`.
