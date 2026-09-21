# Quidra Benchmark Master Prompt

This file is the root orchestration contract for new benchmark runs. Detailed scientific rules live under `benchmark/template/`.

The goal is a reproducible comparison of Quidra, Python, C++, Rust, Go, Java, TypeScript, Kotlin, Swift and Zig. Never change conditions to improve Quidra's position.

## 1. Single current source of truth

A new run may use only:

- the evaluated Quidra snapshot;
- `benchmark/master_prompt.md`;
- the current `benchmark/template/`.

Past run directories are output/audit artifacts only. They are never inputs to a new run. The template therefore contains every reusable source, fixture, validator, workload and configuration needed for bootstrap.

## 2. Five independent Primary evaluations

1. Semantic Compression
2. LLM Learnability
3. Language Quality
4. Ecosystem
5. LLM Proficiency

Never create a cross-evaluation combined score or overall winner.

Each Primary evaluation has its own score and ranking only when its scientific/integrity gates pass. If it is scoreable, the runner must calculate and publish the ranking mechanically; a missing ranking is an orchestration error.

## 3. Sandbox and worker isolation

The trusted outer orchestrator may run on the host. Its authority is limited to host bootstrap/cleanup, starting the isolation boundary, transporting self-contained Task Packets, importing structured worker responses, and launching explicitly sandbox-bound agent runtimes. It is not a scored worker and must not perform scored judgment.

All scored local processes run against a real filesystem sandbox rooted at `/quidra-benchmark`. Any leaf with local filesystem, shell, editor, compiler, or process tools uses `worker_mode=sandbox-agent`, and the agent process itself must run inside that attested sandbox. A host-side Claude Code/subagent with host tools is not a valid scored leaf.

The default leaf mode is `packet-only`. Its permitted local UTF-8 inputs are embedded in the rendered Task Packet. The LLM has no local filesystem/shell/process/editor/host-application tools and returns only a structured Worker Response consumed by `benchmark.py task-apply`. Provider-level network retrieval may be exposed only when the frozen Task Packet permits network access; it must never provide host filesystem or environment access.

The evaluated source is `/quidra-benchmark/repo`; the immutable current template is `/quidra-benchmark/template`. Host home directories, credentials, unrelated repositories and untracked host files must not be visible. `preflight` requires the real sandbox attestation plus worker-gateway, packet-local-tool-disablement and in-sandbox-agent-launcher attestations. Merely changing path strings or forging attestations does not satisfy the isolation requirement.

## 4. Command-first lifecycle

Trusted host bootstrap and sandbox mapping happen before this prompt is used. From this point onward, mechanical and scored work belongs to the sandbox CLI, not to an LLM:

```bash
python3 /quidra-benchmark/template/scripts/benchmark.py <command>
```

Canonical in-sandbox lifecycle:

```bash
benchmark.py prepare
```

``prepare` mechanically performs preflight, toolchain scan, reuse-status,
privacy gate, deterministic planning, manifest freeze, conversion of missing
toolchains into explicit infrastructure blockers, strict plan validation, and
the first `advance`. A missing comparison toolchain does not abort unrelated
Primary evaluations. The component commands remain available for audit
and diagnosis, but a normal run does not need an LLM to sequence them.

`deterministic-plan` expands the frozen work-plan template and mandatory coverage IDs. No planning LLM is used.

`advance` is the state-machine driver. It reclaims stale work, runs deterministic command units, creates/reuses dependency-ready leaf packets, emits `results/dispatch_queue.json`, and derives current Primary status.

For each queued leaf, the outer runner reads `worker_mode` from `results/dispatch_queue.json` and begins with:

```bash
benchmark.py task-start --id <work-unit-id>
benchmark.py task-render --id <agent-id>
```

For `packet-only`, send the rendered packet to a model session with local filesystem/shell/process/application tools disabled, capture exactly one JSON Worker Response, and import it through the sandbox:

```bash
benchmark.py task-apply --id <agent-id> < worker-response.json
benchmark.py task-finish --id <work-unit-id>
benchmark.py advance
```

For `sandbox-agent`, launch the tool-capable agent process itself inside the attested `/quidra-benchmark` sandbox, never as a host-side tool-capable subagent, then run `task-finish` and `advance`.

Long-running workers periodically call:

```bash
benchmark.py heartbeat --id <work-unit-id>
```

Repeat `advance` until all five evaluations are COMPLETE or have explicit legitimate blockers, then run:

```bash
benchmark.py ledger-reconcile
benchmark.py score-status --strict
benchmark.py privacy-check
benchmark.py finalize
benchmark.py post-run --source-repo /path/to/trusted/quidra-checkout
```

`post-run` is a trusted outer-runner operation after the scored sandbox work is over.
After the scored sandbox has exited, the trusted outer runner imports only compact retained run artifacts into `benchmark/<run-id>/` in a clean local `develop` checkout, verifies every copied file by SHA-256, and only then deletes its host-side staging directory. The checkout is never exposed to leaf workers.

## 5. LLMs only where judgment is required

The runner owns planning, dependency release, retries, state transitions, validation dispatch, repeatable measurement, score aggregation, ranking, consistency checks and finalization.

For Language Quality micro workloads, Quidra source authoring and measurement are deliberately separate. One narrow leaf freezes the evaluated commit's required Quidra representations/APIs, then four independent authoring leaves create only three fresh `.qui` programs each from the current Quidra documentation plus the frozen language-neutral workload/validator. No such leaf may read historical Quidra benchmark programs or reusable comparison-language implementations. After validation, the runner builds the Quidra compiler from the evaluated snapshot and mechanically owns correctness runs, compilation, timing, peak RSS, artifact sizing, diagnostic source-byte collection, normalization and requirement-level result emission for the metrics the frozen micro methodology explicitly owns.

Leaf workers handle only tasks that require language/evidence/model judgment. Every manifest unit freezes a worker mode. Packet-only leaves receive embedded permitted inputs and return files only through the structured response importer; sandbox-agent leaves receive narrow sandbox read paths and one sandbox writable directory. Both receive exact requirement IDs, compact worker rules, selected methodology sections, frozen Primary configuration, exact validator and network permission. No scored leaf is a host-side tool-capable subagent.

A multi-language leaf may own at most the frozen runner limit of Primary requirement IDs (currently 3). Larger bundles are rejected mechanically. The only exception is a leaf expanded to exactly one assigned language when several metrics intentionally derive from the same isolated trial history; splitting that history would duplicate scored trials and change the experiment.

Workers never need this root prompt, the parent conversation, another evaluation specification, sibling output, or a historical run.

## 6. Recovery

Ledger state is authoritative. Work units use `PENDING`, `RUNNING`, `COMPLETE`, `BLOCKED`, or `INVALID`.

A worker crash must not strand the run permanently. RUNNING work has a heartbeat lease. When stale, the runner returns it to PENDING and redispatches the same frozen Task Packet. After the frozen maximum attempts, it becomes an explicit infrastructure blocker instead of remaining silently stuck.

COMPLETE requires real evidence plus a passing validator.

## 7. Deterministic score/ranking publication

Workers never hand-author final rankings.

Each requirement worker writes normalized requirement results plus raw evidence. `aggregate-primary` applies the frozen formulas from `template/config/aggregation.json`, calculates every language's Primary score, and generates a deterministic ranking.

A COMPLETE evaluation must contain all required validated work, passing comparability/integrity gates, scores for all applicable languages, and a runner-generated ranking.

If those conditions are not met, the evaluation is PARTIAL/WITHDRAWN/NOT_EXECUTED with exact blockers and no fabricated score.

## 8. Reuse

Reusable comparison-language source/harness/fixtures live directly under `benchmark/template/`. Reuse never means reusing measurements, scores or LLM generations. Every run rebuilds/checks, executes, validates and measures again.

Quidra is the changing target. Quidra benchmark program source is run-specific: it must be authored or re-audited against the evaluated commit and must never enter the reusable program catalog.

## 9. Frozen measurement window

The manifest/ledger freeze begins at `manifest-merge` and ends after `finalize`. During that interval, do not commit/push benchmark/template changes or alter the evaluated snapshot.

Template maintenance happens before freeze or after finalization.

## 10. Retention and privacy

Machine-readable results are the single source of truth. Markdown/CSV/charts are generated from them.

Retain only compact reproducible run artifacts. The retained set is limited to run identity, results, required raw evidence, exact prompts, leaf outputs, frozen plans/manifest/ledger, and runner command results. Build caches, the evaluated repository snapshot, template copy, temporary home, temporary files and micro build products are not imported. Never retain credentials, personal email addresses, host home paths or source-checkout paths outside `/quidra-benchmark`.

A successful run has attempted all five Primary evaluations, mechanically aggregated every scoreable evaluation, emitted a ranking for every COMPLETE evaluation, recorded exact blockers for all others, and passed reconciliation/privacy/finalization gates.
