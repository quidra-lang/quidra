# Benchmark Execution Policy

## 1. Fixed sandbox view

All benchmark agents operate inside a real filesystem sandbox rooted at `./.quidra-benchmark`.

The visible tree contains the current evaluated source, the current self-contained template, isolated work/results/prompts/home/tmp directories, and run metadata. Host home directories, SSH material, credentials, unrelated projects, ignored files and untracked checkout content must not be visible.

The outer runner enforces the actual container/chroot/namespace boundary and injects matching sandbox attestation. `preflight` rejects an unattested workspace.

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

## 4. Leaf agents

Agents are used only for tasks requiring judgment, model interaction, evidence interpretation, annotation, implementation or other work that cannot be made deterministic without changing the evaluation.

Each leaf gets one persisted Task Packet with exact requirement IDs, narrow read paths, one writable directory, expected result schema, exact validator, network permission and selected methodology sections.

Multi-language leaves are mechanically capped at the frozen runner requirement-ID limit. If a larger requirement bundle genuinely shares one scored LLM trial history, deterministic planning must shard it to exactly one language per leaf instead of duplicating the trial across several workers.

A worker must not need the root conversation or a historical run. Independent scored LLM trials must not read one another's generations or repairs.
LLM-heavy work is sharded by language when trials are independent, so one worker never accumulates the full ten-language generation/repair history. The runner merges only validated disjoint language shards.

## 5. Retry and lease policy

Runner defaults are frozen in `config/primary.json`.

A RUNNING agent periodically renews its heartbeat. On lease expiry, `reclaim-stale` returns the unit to PENDING if attempts remain. The same frozen Task Packet is redispatched.

After the maximum attempts, the unit becomes BLOCKED with an infrastructure blocker. It must never remain indefinitely RUNNING.

Validator failure follows the same bounded retry policy. A valid scientific negative result is written as evidence and should still pass the structural validator.

## 6. Agent path ownership

Each agent writes only under `./.quidra-benchmark/work/agents/<agent-id>/`. Shared state is written only by runner commands.

Retries reuse the same content-addressed packet and agent directory. Outputs should be atomic where practical.

## 7. Prompt minimization

Ordinary Task Packets embed `methodology/worker_core.md`, only the selected sections of the applicable evaluation spec, the frozen Primary configuration, and exact requirement IDs.

Do not embed unrelated sections or all five evaluation specifications.

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

After successful `finalize`, only the trusted outer runner may expose a clean local `develop` checkout to `post-run`. The command stages the retained set under `benchmark/<run-id>/`, verifies every retained file hash, atomically installs the run directory, and deletes `./.quidra-benchmark` only after verification succeeds. A failed import never deletes the workspace.

## 13. Git freeze

The no-write measurement window begins when `manifest-merge` freezes the manifest/ledger and ends after `finalize`.

Before that window, template/spec fixes may be committed. After it, completed run import and template maintenance may be committed separately.
