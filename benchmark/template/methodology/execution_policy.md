# Benchmark Execution Policy

## 1. Fixed sandbox view and trusted outer orchestrator

The trusted outer orchestrator may run on the host, but it is outside the scored worker population. Its authority is limited to bootstrap/cleanup, sandbox lifecycle, packet/response transport and launching explicitly sandbox-bound agents. It must not perform scored judgments or expose arbitrary host filesystem/shell/application tools to a scored leaf.

All scored local processes operate against a real filesystem sandbox rooted at `/quidra-benchmark`. Host home directories, SSH material, credentials, unrelated projects, ignored files and untracked checkout content must not be visible.

Two leaf modes are frozen in the manifest. `packet-only` is the default: the runner embeds all permitted local text inputs into the rendered packet, the LLM has no local filesystem/shell/process/editor/host-application tools, and outputs return only as structured JSON through `task-apply`. `sandbox-agent` is reserved for work requiring a larger local corpus or interactive local tools; its tool-capable agent process itself must run inside the attested sandbox and may never be a host-side tool-capable subagent.

The outer runner enforces the actual container/chroot/namespace boundary and injects matching sandbox and worker-runtime attestations. A bind mount or equivalent namespace mapping is acceptable; string substitution, a symlink without isolation, a host-side Claude Code/subagent with host tools, or falsified attestation is not. `preflight` rejects unattested/noncanonical sandboxes, missing worker-gateway/launcher attestations, packet workers whose local tools are not disabled, and sensitive host environment variables.

## 2. Current-template-only rule

Every reusable input required by a new run is tracked directly under `benchmark/template/`.

New runs must not read previous benchmark directories, previous scores, prior LLM generations, prior repair histories, or old methodology snapshots. Historical runs are output/audit material only.

Reusable source/harness/fixture assets may be copied from the current template into the sandbox. Every run rebuilds, executes, validates and remeasures them.

## 3. Command-driven execution

Canonical flow:

```bash
benchmark.py prepare
```

`prepare` executes the deterministic readiness/planning/freeze sequence and
emits the first dispatch queue. The component commands remain available for
audit, debugging and explicit recovery.

The deterministic plan is generated from current template configuration. No planning LLM is used.

The runner owns bookkeeping, dependency scheduling, retry/recovery, environment blocker conversion, repeatable measurement, aggregation, ranking, status derivation and final gates. A missing comparison toolchain is recorded as an infrastructure blocker for affected execution work; it does not stop independent Primary evaluations from advancing. Repeated mechanical work must be implemented as a command rather than prose delegated to an LLM.

The Language Quality micro suite is the concrete command-first model. A current-run leaf authors only the fresh Quidra programs; once those sources pass the frozen validator, a runner command builds the compiler from the evaluated snapshot and performs correctness, compilation, cold/steady timing, peak-RSS and artifact-size work symmetrically across the fixed languages; source bytes may be retained as diagnostic evidence but are not promoted into Source Code Size unless its frozen owner says so.

## 4. Leaf agents and worker modes

Agents are used only for tasks requiring judgment, model interaction, evidence interpretation, annotation, implementation or other work that cannot be made deterministic without changing the evaluation.

Each agent work unit freezes `worker_mode` as either `packet-only` or `sandbox-agent`. Packet-only is preferred whenever the frozen readable inputs fit the embedding limits. `task-render` snapshots those permitted UTF-8 inputs, records hashes and embeds them in the self-contained prompt. The worker receives no local filesystem/shell/process/editor/host-application tools. If the task permits network access, provider/gateway retrieval may be used, but it must not expose host files or environment. The worker returns one JSON response; `task-apply` validates task identity, relative paths, file/byte limits and expected outputs before writing anything.

Sandbox-agent mode is used only where the task needs a larger local corpus or interactive local tools. The agent runtime itself must be started inside `/quidra-benchmark`. Its reads remain limited to the packet's narrow sandbox paths and its writes to one agent directory. Host Read/Glob/Bash/editor/process access, host credentials and SSH sockets are forbidden.

A worker never needs the root conversation or a historical run. Independent scored LLM trials must not read one another's generations or repairs.

## 5. Retry and lease policy

Runner defaults are frozen in `config/primary.json`.

A RUNNING agent periodically renews its heartbeat. On lease expiry, `reclaim-stale` returns the unit to PENDING if attempts remain. The same frozen Task Packet is redispatched.

After the maximum attempts, the unit becomes BLOCKED with an infrastructure blocker. It must never remain indefinitely RUNNING.

Validator failure follows the same bounded retry policy. A valid scientific negative result is written as evidence and should still pass the structural validator.

## 6. Agent path and response ownership

Each sandbox-agent writes only under `/quidra-benchmark/work/agents/<agent-id>/`. Shared state is written only by runner commands.

Packet-only workers never write the filesystem directly. Their response contains relative UTF-8 file paths and contents; `task-apply` is the only command allowed to materialize those files, and it rejects traversal, duplicates, oversized responses and missing expected outputs.

Retries reuse the same content-addressed packet and worker mode. Failed attempts are archived before reset.

## 7. Prompt minimization

Ordinary Task Packets embed `methodology/worker_core.md`, only the selected sections of the applicable evaluation spec, the frozen Primary configuration, exact requirement IDs and the frozen worker mode.

Packet-only packets additionally embed the exact readable UTF-8 inputs with canonical sandbox paths and SHA-256 hashes. If that bundle exceeds the frozen file/byte limit, planning must narrow the packet or explicitly use sandbox-agent mode; the runner must not silently grant host filesystem access.

Do not embed unrelated evaluation specifications or hidden parent conversation state.

## 8. Reuse and toolchain currency

Comparison-language reusable sources live in `template/programs/`; shared fixtures/validators/workloads live in their template subtrees.

`reuse-status` compares the current toolchain fingerprint to the recorded validation fingerprint. Changed toolchains generate explicit capability-currency audit work units in the deterministic plan. Quidra program sources are never reused across evaluated commits.

When current-run Quidra program authoring is required, that worker may read the evaluated Quidra documentation and the language-neutral workload/validator, but not historical Quidra programs or reusable comparison-language source. Its output stays under the current run's agent directory. Measurements, timings, scores and scored LLM outputs are never reused.

## 9. Primary-first budget

The frozen plan records maximum model calls and estimated token envelopes where relevant. If an exposed hard quota makes the Primary plan impossible, stop before measurement and change the language-neutral configuration for a future run. Never reduce only one language or change replication after seeing comparative results.

## 10. Scoreability

Only COMPLETE evaluations publish scores/rankings.

Requirement workers produce raw evidence and normalized requirement-level values. The runner applies `config/aggregation.json` and generates the final language ranking.

A scoreable evaluation left without a ranking is an orchestration defect. A failed comparability/integrity gate instead yields WITHDRAWN/PARTIAL with no fabricated ranking.

## 11. Network and timing

Network is disabled unless a Task Packet explicitly allows it. Do not browse during timing measurements.

Timing warm-ups, repetitions, cache policy and recovery are frozen before measurement and applied symmetrically.

## 12. Privacy and retention

Privacy scanning is required before dispatch, before finalization, and once more over the exact retained set before import. Keep machine-readable final results, exact prompts/hashes, run identity, required raw measurements/audits, leaf outputs, frozen manifest/ledger/plans and runner command results. Drop caches/intermediates, the evaluated source snapshot, template copy, temporary home/files, micro build products and personal/host-specific data.

After successful `finalize` and sandbox exit, only the trusted outer runner may expose a clean local `develop` checkout to `post-run`. The command reads the fixed physical `<source-repo>/.quidra-benchmark` staging directory, requires the checkout-local Git-private host sentinel outside that directory to match the finalized run identity, stages the retained set under `benchmark/<run-id>/`, verifies every retained file hash, atomically installs the run directory, revalidates the sentinel immediately before cleanup, and deletes the host staging directory followed by the sentinel only after verification succeeds. A failed import or sentinel mismatch never deletes the workspace. An abandoned run is removed with `discard-workspace --source-repo <checkout>`, which validates only the fixed workspace path and host-only sentinel contract and deliberately does not trust or require `run.json` or an absolute source path. Both operations remain valid if the checkout is moved or renamed.

## 13. Git freeze

The no-write measurement window begins when `manifest-merge` freezes the manifest/ledger and ends after `finalize`.

Before that window, template/spec fixes may be committed. After it, completed run import and template maintenance may be committed separately.
