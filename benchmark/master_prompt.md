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

## 3. Sandbox, inference gateway and worker isolation

The trusted outer orchestrator may run on the host. Its authority is limited to host bootstrap/cleanup, starting the isolation boundary, running the trusted inference gateway, transporting self-contained Task Packets and importing structured worker responses. It is not a scored worker and must not perform scored judgment.

All scored local processes run against a real filesystem sandbox rooted at `/quidra-benchmark`, created by `template/scripts/sandbox_launcher.py`. The scored container is non-root, has `--network none`, drops all capabilities, sets `no-new-privileges`, uses a read-only root filesystem, mounts `/quidra-benchmark/repo` and `/quidra-benchmark/template` read-only, and receives `HOME=/quidra-benchmark/home`, `TMPDIR=/quidra-benchmark/tmp` and `PWD=/quidra-benchmark`. Host home directories, SSH agent sockets, provider tokens and agent-platform configuration are never mounted.

Provider credentials live only outside that sandbox. `template/scripts/inference_gateway.py` runs on the trusted side, holds whatever API key, OAuth token or local agent session the run uses, and is the only process with provider network access. It listens on a Unix domain socket shared with the scored sandbox through a container-engine volume, so the scored side needs no network of its own. The gateway is a pure model-inference broker: it answers a health handshake and an inference request, and refuses every request kind or field that could describe a tool, file, command, endpoint or credential. Host filesystem, shell, GitHub and agent-platform tools are never exposed to the model through it. Providers are pluggable behind one provider-agnostic protocol; a deterministic offline `fake` provider serves CI.

Scored work reaches a model only through that socket, with no credential of its own and no fallback path. Packet-only units use `benchmark.py task-infer`; sandbox-agent units use `template/scripts/sandbox_agent.py`. Passing `ANTHROPIC_API_KEY`, `CLAUDE_CODE_MESSAGING_TOKEN`, `~/.claude`, an SSH agent socket or any equivalent into the scored sandbox is forbidden, and so is widening the sensitive-environment checks to tolerate one.

Any leaf with local filesystem, shell, editor, compiler or process tools uses `worker_mode=sandbox-agent` and runs through the in-sandbox agent runtime. A host-side Claude Code subagent with host tools is not a valid scored leaf. The default leaf mode is `packet-only`: its permitted local UTF-8 inputs are embedded in the rendered Task Packet, it has no local tools at all, and it returns only a structured Worker Response consumed by `benchmark.py task-apply`.

The evaluated source is `/quidra-benchmark/repo`; the immutable current template is `/quidra-benchmark/template`. `preflight` verifies the observed process rather than any declaration: unprivileged uid, a loopback-only network namespace, an empty capability bounding set, `NoNewPrivs`, read-only snapshot and template mounts, no unexpected mounts, no provider credential in the environment or on disk, a live gateway socket that refuses forbidden requests when probed, and a launcher contract that matches all of it. Setting the attestation variables without applying the restrictions fails.

## 4. Starting a run, and the command-first lifecycle

A real run is started by the `benchmark-production` GitHub Actions workflow on `develop`, which listens for a push that changes `benchmark/.run-production`. Updating that marker is the request to run, and nothing else triggers paid inference. Start one only after the benchmark infrastructure's own CI is green, and compare the workflow's soft spend guard against the token envelope `benchmark.py plan` reports for the frozen manifest: a guard below that envelope stops the run partway and records the remainder as blocked rather than overspending.

Do not start a run on a workstation that is also doing other work. The Language Quality micro suite measures wall-clock time, build time, startup latency and peak RSS on the host it runs on, and `scripts/micro_measure.py` owns the frozen contention limit: it refuses to measure while the one-minute load average is at or above that limit, waits, retries, and finally records `host_contention_unresolved` instead of publishing a contended number. A container runtime takes its memory from that same machine. A loaded or memory-constrained host therefore does not produce worse results; it produces an infrastructure blocker. The four evaluations that do not depend on host timing are unaffected by the measurement host, so a contended run can still complete them.

A shared CI runner is an acceptable measurement host for a comparative ranking, because every language is measured on the same host in the same session and noise is not aimed at any one of them. It is not an acceptable source of absolute performance figures. Publishing those requires a quiet, dedicated Linux host; the launcher behaves identically there, so only the venue changes.

The rest of this section is what happens inside the sandbox once a run has started. Trusted host bootstrap and sandbox mapping happen before this prompt is used. From this point onward, mechanical and scored work belongs to the sandbox CLI, not to an LLM:

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

Before any scored work starts, the trusted side launches the sandbox and the gateway:

```bash
sandbox_launcher.py selftest
sandbox_launcher.py build-image
sandbox_launcher.py run --source-repo <checkout> --provider <provider> \
  --task-policy <network-ceilings.json> -- benchmark.py prepare
```

`selftest` proves on this machine that a container with no network can still
reach the gateway socket on the shared volume. If it fails, scored work must not
start: the sandbox would have no way to reach a model except one that breaks the
credential boundary.

The per-task network ceiling handed to the gateway is derived from the frozen manifest on the trusted side. The gateway never takes a worker's word for how much network its own task may use.

For each queued leaf, the outer runner reads `worker_mode` from `results/dispatch_queue.json` and begins with:

```bash
benchmark.py task-start --id <work-unit-id>
```

For `packet-only`, infer through the credential-less gateway and import the result:

```bash
benchmark.py task-infer --id <agent-id>
benchmark.py task-finish --id <work-unit-id>
benchmark.py advance
```

`task-infer` renders the frozen packet, asks the gateway, and hands the reply to the same importer `task-apply` uses; `task-render` plus `task-apply` remain available when a response is produced outside the sandbox. Either way the worker has no local filesystem, shell, process, editor or host-application tools.

For `sandbox-agent`, run the in-sandbox agent runtime, never a host-side tool-capable subagent:

```bash
sandbox_agent.py --id <agent-id>
benchmark.py task-finish --id <work-unit-id>
benchmark.py advance
```

The runtime enforces the packet's read paths, confines writes and subprocess working directories to `/quidra-benchmark/work/agents/<agent-id>/`, runs subprocesses with `shell=False` and an allowlisted `argv[0]`, and records every refusal in `agent_trace.json`. `task-finish` requires that trace to match the frozen packet and to record a credential-less gateway.

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

Leaf workers handle only tasks that require language/evidence/model judgment. Every manifest unit freezes a worker mode. Packet-only leaves receive embedded permitted inputs and return files only through the structured response importer; sandbox-agent leaves receive narrow sandbox read paths and one sandbox writable directory, enforced in code by the in-sandbox runtime. Both receive exact requirement IDs, compact worker rules, selected methodology sections, frozen Primary configuration, exact validator and network permission, and both reach a model only through the credential-less gateway socket. No scored leaf is a host-side tool-capable subagent.

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

Retain only compact reproducible run artifacts. The retained set is limited to run identity, results, required raw evidence, exact prompts, leaf outputs including sandbox-agent traces, frozen plans/manifest/ledger, and runner command results. Build caches, the evaluated repository snapshot, template copy, temporary home, temporary files and micro build products are not imported. The gateway's request audit log belongs to the trusted side and is never written into the scored workspace. Never retain credentials, personal email addresses, host home paths or source-checkout paths outside `/quidra-benchmark`.

A successful run has attempted all five Primary evaluations, mechanically aggregated every scoreable evaluation, emitted a ranking for every COMPLETE evaluation, recorded exact blockers for all others, and passed reconciliation/privacy/finalization gates.
