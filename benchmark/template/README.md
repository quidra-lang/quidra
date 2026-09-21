# Reusable Benchmark Template

This directory is the complete reusable input for a new benchmark run.

## Layout

- `config/benchmark_metadata.json` — single source of truth for the five Primary evaluation IDs, their display names and the fixed evaluated-language set.
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
- `scripts/check_metadata.py` — template-maintenance check that every restatement of the evaluation IDs, display names and language set still agrees with `config/benchmark_metadata.json`.
- `scripts/micro_measure.py` — runner-owned correctness/build/startup/timing/RSS/source/artifact measurement for the Language Quality startup + micro suite.
- `programs/`, `fixtures/`, `validators/`, `workloads/`, `methodology-assets/` — reusable current-run inputs tracked directly in this template.
- `reuse/catalog.json` — metadata and toolchain-currency fingerprints for those in-template assets.

## No historical-run dependency

A new run must not read an older run directory. Past runs contain results/audit history only. If an asset is reusable, it belongs directly in this template.

Measurements, scores, prior LLM generations, repair histories and run-specific environment data do not belong here.

Comparison-language benchmark sources may be reusable template assets after the frozen currency audit. Quidra program sources are different: they are never reusable across evaluated commits and therefore do not live in the reusable program catalog. Where a workload needs Quidra source (currently the Language Quality micro suite), one current-run leaf freezes the required Quidra representations/APIs and four source-authoring leaves each own exactly three workloads. Those leaves use only the evaluated snapshot documentation plus the frozen language-neutral workload/validator; none may read reusable comparison-language programs or historical Quidra benchmark source. The runner then builds the evaluated Quidra compiler from `/quidra-benchmark/repo`, validates those fresh files, and owns repeatable measurement and normalization.

The active template is immutable during the frozen measurement window. Improvements discovered during a run go to `results/template-candidate/` and are promoted only outside that window after validation/privacy review.

## Workspace

Trusted host bootstrap runs `benchmark.py init --source-repo <source-repo> --sandbox-mode <mode>` and creates the physical staging directory `<source-repo>/.quidra-benchmark` from Git-tracked current source/template content. The path must be Git-ignored. Init also writes a private host-only sentinel at the path returned by `git rev-parse --git-path quidra-benchmark-host.json`, outside the staging directory. The sentinel records the run ID, evaluated commit and canonical sandbox root; its checkout-local Git-private placement supplies the host association without embedding or authorizing against an absolute checkout path. The checkout may therefore be moved or renamed without stranding cleanup, while sandboxed scoring processes still cannot read, delete, or forge the guard.

The outer runner then maps that directory into the real filesystem sandbox as exactly `/quidra-benchmark`. All subsequent `prepare`/task/measurement/finalization commands run against that canonical sandbox path. Workers see only the canonical path; host checkout paths must never be embedded in scored prompts, run metadata or retained artifacts.

The host staging directory and the sandbox-visible path are deliberately different concepts: `./.quidra-benchmark` is a host implementation detail, while `/quidra-benchmark` is the stable benchmark namespace used for reproducible prompts, hashes and privacy checks. Path rewriting alone is not a sandbox.

On macOS, `init` by itself is not sufficient. If no container, VM, namespace-equivalent mechanism, or other trusted isolation layer can present the staging directory as `/quidra-benchmark`, scored work must not start. Do not substitute a plain symlink or forged sandbox attestation.

After `finalize` and sandbox exit, the trusted outer runner uses `benchmark.py post-run --source-repo <checkout>`. Its physical workspace is fixed at `<checkout>/.quidra-benchmark`; host-side `--workspace` overrides are intentionally unsupported. The command imports only compact retained artifacts into `benchmark/<run-id>/` of a clean local `develop` checkout, verifies the copied SHA-256 manifest, revalidates the host-only sentinel against `run.json`, and then deletes the host staging directory followed by the sentinel. It does not retain the workspace's `repo/`, `template/`, `home/`, `tmp/`, or micro build products.

If a run is abandoned before successful `finalize`, the trusted outer runner uses `benchmark.py discard-workspace --source-repo <checkout>`. This command intentionally does not trust or require sandbox-visible `run.json`: it validates the fixed host workspace path and the checkout-local Git-private sentinel contract, deletes the staging directory, then removes the sentinel. Absolute checkout paths are deliberately not an authorization input, so moving or renaming the checkout does not require a force mode. Use this command instead of manual `rm -rf` cleanup.
