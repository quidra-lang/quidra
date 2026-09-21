# Reusable Benchmark Template

This directory is the complete reusable input for a new benchmark run.

## Layout

- `config/primary.json` — frozen Primary replication/timing and runner recovery policy.
- `config/quidra_representation_schema.json` — exact current-run output contract for Quidra micro representation/API resolution.
- `config/evaluation_requirements.json` — mandatory Primary coverage IDs.
- `config/work_plan_templates.json` — deterministic decomposition into leaf work.
- `config/aggregation.json` — runner-owned score/ranking formulas.
- `methodology/worker_core.md` — compact leaf-worker rules.
- `methodology/{semantic_compression,llm_learnability,language_quality,ecosystem,llm_proficiency}.md` — scientific evaluation specifications.
- `methodology/execution_policy.md` — execution/recovery/isolation policy.
- `methodology/orchestration.md` — root-only state-machine rules.
- `scripts/benchmark.py` — deterministic orchestration CLI.
- `scripts/micro_measure.py` — runner-owned correctness/build/startup/timing/RSS/source/artifact measurement for the Language Quality startup + micro suite.
- `programs/`, `fixtures/`, `validators/`, `workloads/`, `methodology-assets/` — reusable current-run inputs tracked directly in this template.
- `reuse/catalog.json` — metadata and toolchain-currency fingerprints for those in-template assets.

## No historical-run dependency

A new run must not read an older run directory. Past runs contain results/audit history only. If an asset is reusable, it belongs directly in this template.

Measurements, scores, prior LLM generations, repair histories and run-specific environment data do not belong here.

Comparison-language benchmark sources may be reusable template assets after the frozen currency audit. Quidra program sources are different: they are never reusable across evaluated commits and therefore do not live in the reusable program catalog. Where a workload needs Quidra source (currently the Language Quality micro suite), one current-run leaf freezes the required Quidra representations/APIs and four source-authoring leaves each own exactly three workloads. Those leaves use only the evaluated snapshot documentation plus the frozen language-neutral workload/validator; none may read reusable comparison-language programs or historical Quidra benchmark source. The runner then builds the evaluated Quidra compiler from `/quidra-benchmark/repo`, validates those fresh files, and owns repeatable measurement and normalization.

The active template is immutable during the frozen measurement window. Improvements discovered during a run go to `results/template-candidate/` and are promoted only outside that window after validation/privacy review.

## Workspace

Trusted bootstrap copies the current tracked Quidra snapshot and this current template into a sandbox-visible `/quidra-benchmark` workspace. Workers never need the host checkout or previous run directories.

After `finalize`, the trusted outer runner uses `benchmark.py post-run --source-repo <checkout>`. That command imports only compact retained artifacts into `benchmark/<run-id>/` of a clean local `develop` checkout, verifies the copied SHA-256 manifest, and then deletes `/quidra-benchmark`. It does not retain the workspace's `repo/`, `template/`, `home/`, `tmp/`, or micro build products.
