# Benchmark Execution Policy

## 1. Fixed sandbox view, trusted gateway and outer orchestrator

The trusted outer orchestrator may run on the host, but it is outside the scored worker population. Its authority is limited to bootstrap/cleanup, sandbox lifecycle, running the inference gateway, packet/response transport and starting in-sandbox agent runtimes. It must not perform scored judgments or expose arbitrary host filesystem/shell/application tools to a scored leaf.

All scored local processes operate against a real filesystem sandbox rooted at `/quidra-benchmark`, created by `scripts/sandbox_launcher.py`. The scored container is non-root, uses `--network none`, drops all capabilities, sets `no-new-privileges`, runs on a read-only root filesystem, mounts `/quidra-benchmark/repo` and `/quidra-benchmark/template` read-only, and receives only `HOME`, `TMPDIR`, `PWD`, locale and the isolation attestations. Host home directories, SSH material, credentials, agent-platform configuration, unrelated projects, ignored files and untracked checkout content must not be visible.

Provider credentials exist only outside the scored sandbox. `scripts/inference_gateway.py` runs on the trusted side, holds the run's API key, OAuth token or local agent session, and is the only process with provider network access. It serves a Unix domain socket shared with the scored container through a container-engine volume, which is what allows the scored side to have no network namespace at all. Passing `ANTHROPIC_API_KEY`, `CLAUDE_CODE_MESSAGING_TOKEN`, `~/.claude`, an SSH agent socket or any equivalent into the sandbox is forbidden, and widening the sensitive-environment checks to tolerate one is equally forbidden.

The gateway is a pure model-inference broker. It accepts a health handshake and an inference request and refuses every other request kind, together with any field that could describe a tool, file, command, working directory, environment, endpoint or credential. It never exposes host filesystem, shell, repository or agent-platform tools to the model, and it strips host paths and credential-shaped text from anything it returns. Providers are pluggable behind one provider-agnostic request/response protocol, and a deterministic offline `fake` provider makes the whole path testable in CI without a provider account. Per-task network ceilings come from the trusted side, derived from the frozen manifest; a worker asking for more network than its task was granted is refused rather than silently downgraded.

Two leaf modes are frozen in the manifest, and both reach a model only through that socket. `packet-only` is the default: the runner embeds all permitted local text inputs into the rendered packet, the worker has no local filesystem/shell/process/editor/host-application tools, and outputs return only as structured JSON. `sandbox-agent` is reserved for work requiring a larger local corpus or local tools; it runs through `scripts/sandbox_agent.py` inside the sandbox and may never be a host-side tool-capable subagent.

`preflight` judges the running process, not its declarations. It requires an unprivileged uid, a loopback-only network namespace, an empty capability bounding set, `NoNewPrivs`, read-only snapshot and template mounts, no unexpected or host-home mounts, no provider credential in the environment or on disk, a live gateway socket that refuses forbidden requests when actually probed, and a launcher contract that agrees with every one of those observations. The launcher emits the `QUIDRA_BENCHMARK_*_ATTESTED` variables only after applying the corresponding restrictions, so an attestation records what was enforced; setting one without the restriction fails. A bind mount or equivalent namespace mapping is acceptable; string substitution, a symlink without isolation, a host-side subagent with host tools, or a forged attestation is not.

The gateway declares the provider-side tools it may enable, and `preflight` records that declaration rather than demanding silence. A network-enabled task may reach a retrieval tool the trusted side froze in advance; what must stay true is that the sandbox cannot select or configure any tool and that nothing on the surface grants host access. A gateway that declared an empty surface while its provider attached a tool would turn this attestation into a claim nobody checked.

## 2. Current-template-only rule

Every reusable source, prompt, fixture, validator, workload and methodology input required by a new run is tracked directly under `benchmark/template/`. Reusable measurements are separate and may enter only through the certified cache under `benchmark/cache/`.

New runs must not read previous benchmark directories, old methodology snapshots, or ad-hoc prior scores/generations/repair histories. Historical run directories are output/audit material only. The sole exception for prior measurements is the explicit certified cache under `benchmark/cache/`, which the trusted runner may hydrate only after a complete input-fingerprint match and current-validator revalidation. Workers never receive the cache as a readable path.

Reusable source/harness/fixture assets may be copied from the current template into the sandbox. Source reuse alone does not imply measurement reuse: mechanical measurements are re-executed where required, while eligible comparison-language judgment measurements may be satisfied by a certified cache HIT.

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

Each agent work unit freezes `worker_mode` as either `packet-only` or `sandbox-agent`. Packet-only is preferred whenever the frozen readable inputs fit the embedding limits. `task-render` snapshots those permitted UTF-8 inputs, records hashes and embeds them in the self-contained prompt. The worker receives no local filesystem/shell/process/editor/host-application tools. `benchmark.py task-infer` renders the packet, asks the credential-less gateway and hands the reply to the same importer `task-apply` uses, which validates task identity, relative paths, file/byte limits and expected outputs before writing anything. `task-render` plus `task-apply` remain available when a response is produced outside the sandbox.

Sandbox-agent mode is used only where the task needs a larger local corpus or local tools. `scripts/sandbox_agent.py` runs inside `/quidra-benchmark` and enforces the packet in code rather than by instruction:

- reads resolve only inside the Task Packet's declared read paths and the worker's own directory, with symlinks resolved so a planted link cannot widen them;
- writes and subprocess working directories are confined to `/quidra-benchmark/work/agents/<agent-id>/`, and the runner-owned files in it are not writable by the worker;
- subprocesses run with `shell=False` and an `argv[0]` drawn from the frozen allowlist in `config/sandbox_agent.json`;
- turn count, read bytes, write bytes and subprocess output are bounded by that same frozen configuration.

Every refusal is returned to the model as an observation and recorded in `agent_trace.json`, so a run that repeatedly tried to leave its sandbox is visible to an auditor instead of silently retried. `task-finish` requires that trace to name the same frozen packet, to have ended deliberately, to leave no expected output missing, and to record a credential-less gateway with no exposed host tool surface.

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

Sandbox-agent packets are preceded by the runtime contract the in-sandbox agent loop enforces: the available actions, the declared read paths, the single writable directory, the subprocess allowlist and the turn budget. That text describes limits the runtime already applies; it is not the mechanism.

Do not embed unrelated evaluation specifications or hidden parent conversation state.

## 8. Reuse and toolchain currency

Comparison-language reusable sources live in `template/programs/`; shared fixtures/validators/workloads live in their template subtrees.

`reuse-status` compares the current toolchain fingerprint to the recorded validation fingerprint. Changed toolchains generate explicit capability-currency audit work units in the deterministic plan. Quidra program sources are never reused across evaluated commits.

Quidra benchmark programs (micro suite, adversarial case set, representation pins and type-binding amendment) are maintained inside the evaluated snapshot under `tests/benchmark/quidra` and are re-audited mechanically against that commit's compiler by the quidra-audit unit before measurement; no run authors or edits them. Quidra program outputs and any Quidra-containing measurements, timings, scores or scored LLM outputs are never reused across evaluated commits; only eligible non-Quidra comparison-language judgment measurements may be restored through the certified cache contract above.

For eligible non-Quidra measurement work, certification is unit-scoped rather than Primary-scoped: ledger state must be COMPLETE and the unit's frozen validator must have recorded PASS, but sibling units and the Primary aggregate may still be unfinished. After the scored sandbox exits, the trusted host may checkpoint those records into `benchmark/cache/` even when the paid driver stopped for budget, provider or wall-clock reasons. The running sandbox sees only the cache snapshot copied at initialization, so a checkpoint can never feed a result back into the run that produced it.

## 9. Primary-first budget

The frozen plan records maximum model calls and estimated token envelopes where relevant. If an exposed hard quota makes the Primary plan impossible, stop before measurement and change the language-neutral configuration for a future run. Never reduce only one language or change replication after seeing comparative results.

## 10. Scoreability

Only COMPLETE evaluations publish scores/rankings.

Requirement workers produce raw evidence and normalized requirement-level values. The runner applies `config/aggregation.json` and generates the final language ranking.

A scoreable evaluation left without a ranking is an orchestration defect. A failed comparability/integrity gate instead yields WITHDRAWN/PARTIAL with no fabricated ranking.

## 11. Network and timing

Network is disabled unless a Task Packet explicitly allows it, and the scored sandbox has no network namespace in any case: the only channel out of it is the gateway socket. The trusted side derives each task's ceiling from the frozen manifest and hands it to the gateway, which refuses a request that asks for more. When a task's ceiling is `disabled`, no network-capable model tooling is enabled on the provider side either. Do not browse during timing measurements.

Timing warm-ups, repetitions, cache policy and recovery are frozen before measurement and applied symmetrically.

The decoding state is frozen the same way, but it is a declaration rather than a
value. The evaluated model family removed `temperature`, `top_p` and `top_k` and
rejects a request carrying one, so a fixed sampling value is not a control that
exists to be set. `primary.json` therefore records that no sampling parameters are
sent, that decoding is provider-controlled, and which effort level pins depth;
`preflight` rejects a section that claims otherwise. Section 6.2 of the LLM
Proficiency specification asks for exactly this: identical decoding controls *or*
the same recorded provider-controlled state for all languages. The gateway refuses
any decoding field the sandbox tries to supply and records the state it used on
every brokered request, so a finished run carries evidence of how it was decoded
rather than an assumption. Where decoding is deterministic, both the LLM
Proficiency and LLM Learnability specifications still require duplicate outputs
across independent trials to be preserved and reported rather than disguised with
ad-hoc prompt noise.

## 12. Privacy and retention

Privacy scanning is required before dispatch, before finalization, and once more over the exact retained set before import. Keep machine-readable final results, exact prompts/hashes, run identity, required raw measurements/audits, leaf outputs, frozen manifest/ledger/plans and runner command results. Drop caches/intermediates, the evaluated source snapshot, template copy, temporary home/files, micro build products and personal/host-specific data.

The gateway's per-request audit log records request identity, task identity, timing, usage and a response hash. It lives on the trusted side and is never written into the scored workspace or the retained set.

After successful `finalize` and sandbox exit, only the trusted outer runner may expose the clean isolated `benchmark` checkout to `post-run`. The command reads the fixed physical `<source-repo>/.quidra-benchmark` staging directory, requires the checkout-local Git-private host sentinel outside that directory to match the finalized run identity, stages the retained set under `benchmark/<run-id>/`, verifies every retained file hash, atomically installs the run directory, revalidates the sentinel immediately before cleanup, and deletes the host staging directory followed by the sentinel only after verification succeeds. A failed import or sentinel mismatch never deletes the workspace. An abandoned run is removed with `discard-workspace --source-repo <checkout>`, which validates only the fixed workspace path and host-only sentinel contract and deliberately does not trust or require `run.json` or an absolute source path. Both operations remain valid if the checkout is moved or renamed.

## 13. Git freeze

The no-write measurement window begins when `manifest-merge` freezes the manifest/ledger and lasts until the scored production sandbox exits. A normal complete run exits after `finalize`; a deliberately bounded or failed run may exit earlier and then enter trusted-host checkpoint recovery.

Before that window, template/spec fixes may be committed. After the scored sandbox exits, completed run import, unit-level certified-cache checkpointing and template maintenance are trusted outer-runner operations and must be committed separately from scored work.
