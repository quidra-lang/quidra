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

## 3. Sandbox

All agents and scored processes run inside a real filesystem sandbox rooted at `./.quidra-benchmark`.

The evaluated source is `./.quidra-benchmark/repo`; the immutable current template is `./.quidra-benchmark/template`. Host home directories, credentials, unrelated repositories and untracked host files must not be visible.

## 4. Command-first lifecycle

Mechanical work belongs to the CLI, not to an LLM:

```bash
python3 ./.quidra-benchmark/template/scripts/benchmark.py <command>
```

Canonical lifecycle:

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

For each queued agent task, the outer agent runner normally performs only:

```bash
benchmark.py task-start --id <work-unit-id>
benchmark.py task-render --id <agent-id>
# dispatch the rendered packet to the worker
benchmark.py task-finish --id <work-unit-id>
benchmark.py advance
```

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
It copies only compact retained run artifacts into `benchmark/<run-id>/` in a clean
local `develop` checkout, verifies every copied file by SHA-256, and only then
deletes `./.quidra-benchmark`. The checkout is never exposed to leaf workers.

## 5. LLMs only where judgment is required

The runner owns planning, dependency release, retries, state transitions, validation dispatch, repeatable measurement, score aggregation, ranking, consistency checks and finalization.

For Language Quality micro workloads, Quidra source authoring and measurement are deliberately separate. One narrow leaf freezes the evaluated commit's required Quidra representations/APIs, then four independent authoring leaves create only three fresh `.qui` programs each from the current Quidra documentation plus the frozen language-neutral workload/validator. No such leaf may read historical Quidra benchmark programs or reusable comparison-language implementations. After validation, the runner builds the Quidra compiler from the evaluated snapshot and mechanically owns correctness runs, compilation, timing, peak RSS, artifact sizing, diagnostic source-byte collection, normalization and requirement-level result emission for the metrics the frozen micro methodology explicitly owns.

Leaf workers handle only tasks that require language/evidence/model judgment. They receive a self-contained Task Packet containing exact requirement IDs, narrow read paths, one writable directory, compact worker rules, only the selected methodology sections needed by that task, frozen Primary configuration, exact validator and network permission.

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

Retain only compact reproducible run artifacts. The retained set is limited to run identity, results, required raw evidence, exact prompts, leaf outputs, frozen plans/manifest/ledger, and runner command results. Build caches, the evaluated repository snapshot, template copy, temporary home, temporary files and micro build products are not imported. Never retain credentials, personal email addresses, host home paths or source-checkout paths outside `./.quidra-benchmark`.

A successful run has attempted all five Primary evaluations, mechanically aggregated every scoreable evaluation, emitted a ranking for every COMPLETE evaluation, recorded exact blockers for all others, and passed reconciliation/privacy/finalization gates.
