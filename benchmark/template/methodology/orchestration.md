# Root Orchestration Specification

This file is for the root benchmark orchestrator. Leaf workers do not read it.

## 1. Inputs and run identity

A new run uses only the evaluated Quidra checkout plus the current `benchmark/master_prompt.md` and `benchmark/template/`. Past run directories are not valid inputs.

Trusted `init` requires a clean `develop` checkout, requires `<source-repo>/.quidra-benchmark` to be Git-ignored, records the exact commit identity, copies only Git-tracked current source/template content into that fixed host staging directory, and writes a host-only sentinel under the checkout's Git-private path returned by `git rev-parse --git-path quidra-benchmark-host.json`. The sentinel is outside the staging tree and records the run identity without binding authorization to the checkout's absolute path, so the checkout may be moved or renamed safely. The outer runner then maps only the physical staging directory to the canonical sandbox root `/quidra-benchmark` before any scored or delegated work begins.

## 2. Deterministic planning

Planning is runner-owned. A normal run starts with `benchmark.py prepare`,
which executes all deterministic readiness/planning/freeze steps and emits the
first dispatch queue. The lower-level commands remain independently invocable
for audit and recovery.


`benchmark.py deterministic-plan` expands `template/config/evaluation_requirements.json`, `template/config/work_plan_templates.json`, and current toolchain/reuse audit state. It writes five validated plans under `work/root/plans/`. No planner agent is involved.

`manifest-merge` freezes those plans into `work/root/manifest.json` and initializes `work/root/ledger.json`. The manifest must cover every mandatory requirement ID, contain at least one measurement unit per evaluation, and exactly one aggregation unit that transitively depends on every other unit in that evaluation.

## 3. State machine

The ledger is mutable execution state. Every unit is PENDING, RUNNING, COMPLETE, BLOCKED or INVALID.

Agent work has a frozen maximum attempt count and heartbeat lease. `task-start` claims a PENDING unit and increments its attempt. `heartbeat` renews the lease. `task-finish` runs the frozen validator and either marks COMPLETE or schedules a retry. `reclaim-stale` returns stale RUNNING work to PENDING until the attempt limit is exhausted, after which it records an infrastructure blocker.

Missing comparison toolchains are also infrastructure blockers, not bootstrap-fatal errors. After manifest freeze, `toolchain-blockers` marks only the affected execution-heavy Primary work terminal; `advance` propagates those blockers while independent Primary evaluations continue.

The same frozen Task Packet is reused for retries; retrying must not change the scientific contract.

## 4. Advance loop

`benchmark.py advance` reclaims stale work, executes dependency-ready runner-owned command units, materializes dependency-ready leaf Task Packets, writes `results/dispatch_queue.json`, derives current Primary status and reports the next mechanical action.

Every queued leaf includes its frozen `worker_mode`. The trusted outer orchestrator may transport packets but never substitutes a host-side tool-capable subagent for scored work. For `packet-only`, `benchmark.py task-infer` renders the packet, asks the credential-less gateway and hands the reply to the same importer `task-apply` uses; `task-render` plus `task-apply` remain available when the response is produced outside the sandbox. For `sandbox-agent`, `scripts/sandbox_agent.py` runs the agent loop inside the sandbox. `task-finish` validates the resulting files and state transition, and for sandbox-agent work it additionally requires an `agent_trace.json` naming the same frozen packet and recording a credential-less gateway.

Runner command units can emit validated requirement-level results, not only aggregates. In Language Quality, current-commit Quidra representation/authoring work completes before the runner-owned mechanical measurement unit.

The outer runtime does not invent work units, perform repeatable measurements by hand, or manually aggregate scores.

## 5. Worker context and isolation

A leaf packet embeds `methodology/worker_core.md`, selected evaluation sections, frozen Primary configuration, exact requirement IDs and its worker mode.

Before any scored work starts, `scripts/sandbox_launcher.py` creates the sandbox and the trusted gateway. The scored container is non-root, has `--network none`, drops all capabilities, sets `no-new-privileges`, runs on a read-only root filesystem, and mounts `/quidra-benchmark/repo` and `/quidra-benchmark/template` read-only. Provider credentials stay in the gateway process on the other side of a shared Unix domain socket, which is the scored sandbox's only channel to anything outside itself. The launcher inherits nothing from its own environment into the sandbox, and emits the isolation attestations only after applying the restrictions they name.

`packet-only` is preferred. The runner embeds every permitted local input and hashes it. The worker has no local filesystem/shell/process/editor/host-application tools and returns only a structured response. `sandbox-agent` is reserved for tasks whose local corpus or tool needs make packet-only impractical; its runtime enforces the packet's read paths, its single writable directory and its subprocess allowlist in code. Host-side Claude Code subagents with host tools are not valid workers in either mode.

Network permission is per task and set on the trusted side from the frozen manifest. The gateway refuses a request that claims more network than its task was granted, so a mis-scoped packet is a visible failure rather than a silent escalation.

Read paths remain narrow. A worker must not depend on the root conversation, sibling outputs, unrelated evaluations, or historical runs.

## 6. Aggregation and ranking

Aggregation units use `template/config/aggregation.json` and validated requirement-level results from both leaf workers and runner-owned measurement commands. They are command units, not LLM workers.

The runner calculates per-language evaluation scores and deterministic rankings. A COMPLETE evaluation without a ranking is invalid. A ranking is withheld only when scientific/integrity gates fail or required applicable work is incomplete/blocked.

No cross-evaluation overall score or ranking is allowed.

## 7. Blockers

Use explicit blocker classes: scientific, infrastructure, budget-plan-defect, ordinary-incomplete.

Transport/provider failures are infrastructure failures, never language/model failures. A scientific gate may validly produce WITHDRAWN after all work completed. Ordinary incomplete work may not be relabeled N/A.

## 8. Finalization

Before finalization no PENDING/RUNNING units remain, COMPLETE units have evidence and PASS validation, every scoreable evaluation has runner-generated score/ranking, every non-COMPLETE evaluation has exact blockers, and privacy/template-integrity checks pass.

The frozen measurement window starts at `manifest-merge` and ends after `finalize`.

After that window, the trusted outer runner executes `post-run` with a clean local
`develop` checkout. This is the only repository-import step: it retains the compact
audit/result set, verifies hashes, then removes the fixed host staging workspace and
its Git-private sentinel. If a run is abandoned before finalization, the outer runner
uses `discard-workspace`; discard is authorized by the checkout-local host-only
sentinel contract and fixed workspace path, intentionally does not depend on
sandbox-visible `run.json`, and remains valid after checkout relocation.
