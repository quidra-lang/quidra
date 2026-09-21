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

Trusted host bootstrap runs `benchmark.py init --source-repo <source-repo> --sandbox-mode <mode>` and creates the physical staging directory `<source-repo>/.quidra-benchmark` from Git-tracked current source/template content. Init also writes a private host-only sentinel outside the staging directory.

The trusted outer orchestrator may remain on the host, but it is not a scored worker. It may bootstrap/clean up the workspace, start the sandbox, transport rendered packets and responses, and launch sandbox-bound agent runtimes. It must never give a scored leaf arbitrary host Read/Glob/Bash/editor/process access.

The outer runner maps the staging directory into the real filesystem sandbox as exactly `/quidra-benchmark`. All runner commands, validators, compilers and scored local processes execute against that canonical path. Path rewriting alone is not a sandbox.

Leaf work has two frozen modes:

- `packet-only` (default): `task-render` embeds every permitted local UTF-8 input into the Task Packet. The LLM runs without local filesystem, shell, process, editor or host-application tools. If network is permitted, only provider/gateway network retrieval may be exposed. The worker returns one JSON response containing relative output files; the outer runner pipes it to `benchmark.py task-apply --id <agent-id>` inside the sandbox.
- `sandbox-agent`: used when a task genuinely needs a larger local corpus or interactive local tools. The agent process itself must be launched inside the attested `/quidra-benchmark` sandbox and may read only the packet's sandbox paths and write only its agent directory. Host credentials, SSH sockets and host home directories must not be mounted into it.

`preflight` requires the real sandbox attestation plus `packet-gateway-v1`, packet-local-tools-disabled and in-sandbox-agent-launcher attestations. It continues to reject HOME/TMPDIR/PWD outside the workspace and sensitive host environment variables.

On macOS, `init` by itself is not sufficient. If no container, VM, namespace-equivalent mechanism or other trusted isolation layer can present the staging directory as `/quidra-benchmark`, scored work must not start. Do not substitute a symlink or forged attestation.

After `finalize` and sandbox exit, the trusted outer runner uses `benchmark.py post-run --source-repo <checkout>`. If a run is abandoned before successful `finalize`, use `benchmark.py discard-workspace --source-repo <checkout>` instead of manual `rm -rf`.
