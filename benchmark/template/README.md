# Reusable Benchmark Template

This directory is the complete reusable input for a new benchmark run.

## Layout

- `config/benchmark_metadata.json` — single source of truth for the five Primary evaluation IDs, their display names and the fixed evaluated-language set.
- `config/primary.json` — frozen Primary replication/timing and runner recovery policy.
- `config/quidra_representation_schema.json` — exact current-run output contract for Quidra micro representation/API resolution.
- `config/evaluation_requirements.json` — mandatory Primary coverage IDs.
- `config/work_plan_templates.json` — deterministic decomposition into leaf work.
- `config/aggregation.json` — runner-owned score/ranking formulas.
- `config/inference_gateway.json` — frozen credential-less inference protocol: socket path, accepted request kinds, refused request fields, size limits and the credential names/paths that must never appear inside the sandbox.
- `config/sandbox_agent.json` — frozen sandbox-agent runtime limits: turn budget, read/write byte ceilings, the subprocess allowlist, the action-turn output cap and the per-unit attempt limit (runtime values that must not re-key the certified cache live here rather than in `primary.json`).
- `methodology/worker_core.md` — compact leaf-worker rules.
- `methodology/{semantic_compression,llm_learnability,language_quality,ecosystem,llm_proficiency}.md` — scientific evaluation specifications.
- `methodology/execution_policy.md` — execution/recovery/isolation policy.
- `methodology/orchestration.md` — root-only state-machine rules.
- `scripts/benchmark.py` — deterministic orchestration CLI.
- `scripts/inference_gateway.py` — trusted model-inference broker. Runs outside the scored sandbox, holds the only provider credential, and serves the Unix socket.
- `scripts/gateway_client.py` — credential-less client used by scored code. Holds no key and has no fallback path.
- `scripts/sandbox_agent.py` — in-sandbox agent runtime for `worker_mode=sandbox-agent`, with read/write/exec limits enforced in code.
- `scripts/sandbox_launcher.py` — container launcher that creates the hardened sandbox, starts the gateway sidecar and emits the isolation attestations.
- `scripts/synthetic_run.py` — offline end-to-end harness that drives both worker modes through a real gateway with the deterministic fake provider.
- `runtime/Dockerfile`, `runtime/install_toolchains.sh`, `runtime/toolchains.json`, `runtime/verify_toolchains.py` — the reproducible ten-language Linux image and its pinned, build-time-verified toolchain identity.
- `runtime/check_pins.py` — fast reachability check for every pinned artifact, so upstream pin rot fails in seconds instead of midway through an image build.
- `scripts/check_metadata.py` — template-maintenance check that every restatement of the evaluation IDs, display names and language set still agrees with `config/benchmark_metadata.json`.
- `scripts/micro_measure.py` — runner-owned correctness/build/startup/timing/RSS/source/artifact measurement for the Language Quality startup + micro suite.
- `programs/`, `fixtures/`, `validators/`, `workloads/`, `methodology-assets/` — reusable current-run inputs tracked directly in this template.
- `reuse/catalog.json` — metadata and toolchain-currency fingerprints for those in-template assets.

## No historical-run dependency

A new run must not read an older run directory. Past runs contain results/audit history only. Reusable source, prompt, fixture, validator, workload and methodology inputs belong directly in this template; reusable measurements may enter only through the explicit certified cache under `benchmark/cache/`.

Measurements, scores, prior LLM generations, repair histories and run-specific environment data do not belong here.

Comparison-language benchmark sources may be reusable template assets after the frozen currency audit. Quidra program sources are different: they live with the compiler in the evaluated snapshot under `tests/benchmark/quidra`, are re-audited mechanically against every evaluated commit, and never come from the reusable comparison-program catalog or a historical run. The runner builds the evaluated Quidra compiler from `/quidra-benchmark/repo`, validates those snapshot-owned files against the frozen workload/oracle and representation pins, and owns repeatable measurement and normalization. No scored leaf authors replacement Quidra benchmark programs during the run.

The active template is immutable during the frozen measurement window. Content-addressed prompt components/manifests are promoted only after successful finalization. Certified non-Quidra result-cache records are independent: after the scored sandbox exits, any COMPLETE unit with a current-validator PASS and the required cache certification may be checkpointed even when the overall Primary evaluation is still partial. All other template changes require an explicit maintenance commit; a scored run never rewrites its own methodology, workloads, validators, or reusable source catalog.

## Workspace

Trusted host bootstrap runs `benchmark.py init --source-repo <source-repo> --sandbox-mode <mode>` and creates the physical staging directory `<source-repo>/.quidra-benchmark` from Git-tracked current source/template content. Init also writes a private host-only sentinel outside the staging directory.

The trusted outer orchestrator may remain on the host, but it is not a scored worker. It may bootstrap/clean up the workspace, start the sandbox and the inference gateway, transport rendered packets and responses, and start sandbox-bound agent runtimes. It must never give a scored leaf arbitrary host Read/Glob/Bash/editor/process access, and it must never place a provider credential inside the sandbox.

`scripts/sandbox_launcher.py` maps the staging directory into the real filesystem sandbox as exactly `/quidra-benchmark`. All runner commands, validators, compilers and scored local processes execute against that canonical path. Path rewriting alone is not a sandbox.

## Isolation topology

    trusted side                         scored side
    ------------                         -----------
    gateway container                    scored container
      provider credentials                 no credentials
      network: provider egress             network: none
      /gateway (shared volume) <--socket--> /quidra-benchmark/gateway
                                           /quidra-benchmark          rw
                                           /quidra-benchmark/repo     ro
                                           /quidra-benchmark/template ro

The scored container is non-root, uses `--network none`, drops all capabilities, sets `no-new-privileges`, runs on a read-only root filesystem, and receives `HOME=/quidra-benchmark/home`, `TMPDIR=/quidra-benchmark/tmp` and `PWD=/quidra-benchmark`. It inherits nothing from the launcher's environment. Host home directories, SSH agent sockets, provider tokens and agent-platform configuration are never mounted.

The two containers share one Unix domain socket on a container-engine volume rather than a host bind mount, because a socket created by a macOS process inside a shared folder is not usable from inside a Colima or Docker Desktop VM. Provider egress belongs to the gateway alone.

Before scored work starts on a new machine, run the one check that covers the
assumption the whole topology rests on:

```bash
sandbox_launcher.py selftest
```

It creates the shared volume, starts the gateway, and completes a handshake from
a container with `--network none`. That is the step most likely to behave
differently between engines, and on macOS it is the reason the socket lives on a
container-engine volume rather than a host bind mount. `sandbox_launcher.py run`
additionally probes, before any scored command starts, that the container really
sees the staging directory; on Colima a checkout outside a shared path otherwise
mounts as an empty directory and fails much later with a confusing error.

`scripts/inference_gateway.py` is a pure model-inference broker. It accepts a health handshake and an inference request and refuses every other request kind, along with any field that could describe a tool, file, command, endpoint or credential. Providers sit behind one provider-agnostic protocol: `fake` (deterministic and offline, used by CI), `exec` (a trusted local command, which is how an existing authenticated agent CLI session is reused without exposing it), and `anthropic-messages` (a direct provider call using gateway-only credentials).

The `exec` provider receives the rendered conversation on stdin and returns
assistant text on stdout. It runs on the credential side and must have its own
tools disabled, because the gateway brokers inference only. With Claude Code
that is:

```bash
sandbox_launcher.py run --source-repo <checkout> --provider exec \
  --exec-command "claude -p --output-format text --disallowed-tools '*'" \
  -- benchmark.py prepare
```

There are two supported placements for a gateway that shells out to a local CLI.
On Linux, `--gateway external --gateway-socket-dir <dir>` lets the gateway run as
a host process against the operator's existing session, and the scored container
bind-mounts the socket directory. On macOS a socket created by a host process is
not usable from inside the engine's VM, so the gateway runs as a container and
the CLI plus its session are mounted into it with `--gateway-mount`. That option
applies to the gateway container only: the scored container's mounts come from
the launcher contract and cannot be extended. Per-task network ceilings are supplied by the trusted side from the frozen manifest; a worker asking for more than its task was granted is refused rather than downgraded.

Leaf work has two frozen modes, and both reach a model only through that socket:

- `packet-only` (default): `task-render` embeds every permitted local UTF-8 input into the Task Packet. The worker has no local filesystem, shell, process, editor or host-application tools. `benchmark.py task-infer` renders the packet, asks the gateway and hands the reply to the same importer `task-apply` uses; `task-render` plus `task-apply` remain available when the response is produced elsewhere.
- `sandbox-agent`: used when a task genuinely needs a larger local corpus or local tools. `scripts/sandbox_agent.py` runs inside the sandbox and enforces its permissions in code: reads only the packet's declared paths, writes only `/quidra-benchmark/work/agents/<agent-id>/`, subprocesses with `shell=False`, an allowlisted `argv[0]` and a working directory inside that same agent directory. Every refusal is recorded in `agent_trace.json`, and `task-finish` requires a trace that matches the frozen packet and records a credential-less gateway.

`preflight` verifies the running process rather than any declaration. It checks an unprivileged uid, a loopback-only network namespace, an empty capability bounding set, `NoNewPrivs`, read-only `repo` and `template` mounts, the absence of unexpected or host-home mounts, the absence of provider credentials in the environment and on disk, a live gateway socket that refuses forbidden requests when actually probed, and a launcher contract that agrees with all of it. Setting the attestation variables without applying the restrictions fails, and the sensitive-environment checks must not be widened to tolerate a credential.

The gateway declares the provider-side tools it may enable, and `preflight` records that declaration rather than demanding silence. A network-enabled task may reach a retrieval tool the trusted side froze in advance; what must stay true is that the sandbox cannot select or configure any tool and that nothing on the surface grants host access. A gateway that declared an empty surface while its provider attached a tool would turn this attestation into a claim nobody checked.

## Production benchmark trigger

Paid inference is intentionally opt-in and isolated from normal `develop`
pushes. The operator creates the disposable `benchmark` branch from the exact
green `develop` commit to evaluate and changes `benchmark/.run-production`
there. `benchmark-production` listens only to that marker on `benchmark`.
Normal source pushes, pull requests, scheduled CI, runtime-image CI and
benchmark-template CI never call the Anthropic API.

`benchmark-smoke` follows the same explicit-request rule for provider
diagnostics. Production provider smoke runs only when the preflight plan still
contains paid dispatch; a fully cache-satisfied replay performs no paid smoke. A production request never adds its marker to `develop`, so the
evaluated compiler/program snapshot remains the exact pre-request commit.

The production workflow evaluates that frozen snapshot with
`claude-sonnet-5`. The repository secret `ANTHROPIC_API_KEY` exists only in
trusted Actions/gateway steps. The scored container remains credential-less and
network-isolated.

Before paid dispatch, deterministic preparation hydrates exact certified-cache
hits, restores exact-fingerprint paid leaf inference state from the durable
`benchmark/cache/partial-paid/` store (with Actions cache as a speed mirror),
writes `results/cache_impact.json`, and freezes
`results/execution-plan-preflight.json`. The plan lists cache reuse,
invalidation/re-evaluation, new paid work, dependency-deferred work, expected
paid-call upper bounds and the conservative spend bound. Each Primary also keeps
its own execution plan plus an actual-vs-plan record after dispatch.

The run freezes provider pricing in the template snapshot and includes web-search
charges only for work units whose trusted policy permits live retrieval. The
request marker supplies the whole-run budget; the trusted gateway and budget
ledger refuse dispatch beyond it. Token usage, web-search count and estimated
cost are retained in a redacted audit log.

COMPLETE+PASS leaves are checkpointed independently into the certified result
cache. Paid responses/trials are checkpointed separately in the content-addressed
`benchmark/cache/partial-paid/` store, optionally mirrored in Actions cache, and
can be restored in a later run only when the complete dependency fingerprint and
Task Packet hash match. This remains true after the leaf becomes COMPLETE, so a
later current-validator re-evaluation can reuse already-paid model calls instead
of repurchasing them.
A diagnostic finalize records exact missing required leaves, but only
`formal_complete=true` can be imported as a formal result. If `develop` moves
during the isolated run, reconciliation never silently changes the evaluated
snapshot.

## Runtime image

`runtime/Dockerfile` builds the reproducible Linux image in two stages. `base` carries the OS, Python and the native dependencies needed to build the Quidra compiler from the evaluated snapshot; `toolchains` adds the nine pinned comparison-language toolchains. `runtime/toolchains.json` is the single source of truth for those pins, and `runtime/verify_toolchains.py` runs at build time: if an upstream repository serves a different build than the pin names, the image fails to build rather than shipping an unrecorded toolchain into a measurement run. The observed fingerprints are written into the image at `/opt/quidra-benchmark/toolchains-observed.json`.

The Quidra compiler is deliberately absent from the image. It is built during the run from `/quidra-benchmark/repo`, so the image cannot pin the thing under evaluation.

`runtime/check_pins.py` verifies that every pinned artifact still resolves, for
each supported architecture, without building anything. The full image is built
and verified by the `benchmark-runtime-image` workflow, which runs when the image
definition changes, on demand, and weekly so upstream drift surfaces on its own
rather than during a measurement run.

These Linux toolchain versions are not expected to equal the fingerprints recorded in `reuse/catalog.json`, which were validated on a different operating system. That difference is resolved by the existing currency mechanism: `reuse-status` reports `AUDIT_REQUIRED` and the deterministic plan adds capability-currency audit work. Neither side is edited to make the numbers agree.

On macOS, `init` by itself is not sufficient. If no container, VM, namespace-equivalent mechanism or other trusted isolation layer can present the staging directory as `/quidra-benchmark`, scored work must not start. Do not substitute a symlink or forged attestation.

After sandbox exit, the trusted outer runner uses `benchmark.py post-run --source-repo <checkout>` only when `finalization.json.formal_complete` is true. Diagnostic/partial finalization deliberately cannot be imported. If a run is abandoned, use `benchmark.py discard-workspace --source-repo <checkout>` instead of manual `rm -rf`; COMPLETE+PASS certified cache records and exact-fingerprint paid-leaf inference checkpoints remain independently reusable.

## Files that key certified-cache records

Some template files are readable inputs of scored units and are hashed into their certified-cache
records: `workloads/` and `programs/<language>/` for the Language Quality language-development
units, `methodology-assets/` for Semantic Compression and Ecosystem, and `config/primary.json`,
`config/benchmark_metadata.json` and `methodology/` for every unit. Editing one of them re-keys the
records that read it and the next run pays to measure them again. Run
`benchmark.py cache-impact --source-repo .` before pushing a template change to see which records
it would invalidate. The Zig build command in `workloads/micro.md` is documented without `-lc`
for exactly this reason; `scripts/micro_measure.py` adds the flag. The mechanical Language Quality
records additionally key on `scripts/micro_measure.py`, `scripts/adversarial_measure.py`,
`programs/`, `fixtures/`, `validators/micro/` and the snapshot's `tests/benchmark/quidra`, so an
edit to any of those re-runs the six-hour measurement on the next run.
