---
name: benchmark-run
description: Run the full Quidra benchmark end to end - gates, smoke, production marker, monitoring and the result report. Use when the user asks to start, request or run the benchmark ("ベンチマーク開始して", "start the benchmark", "run the full benchmark"), or to check on a run that is already in flight.
---

# Running the benchmark to the end

Carry the whole procedure out without stopping to ask. The user asking for the
benchmark IS the authorization to spend up to the guard below. Ask only if a
gate fails in a way this file does not cover, or if the run would cost more
than the guard allows.

Work in the `quidra` repository. `benchmark/README.md` is the operator's
checklist and `benchmark/master_prompt.md` is the contract; this file is the
order to execute them in and the decisions already made.

## Standing decisions - do not re-ask

- **Scope**: a full run. `evaluation: all`, no `units:` line.
- **Guard**: `budget_usd: 5.0`. The default is 100; 5 is used because the
  certified cache covers everything but the units a change re-keyed. Raise it
  only if step 2 shows records going invalid whose re-measurement costs more,
  and say so in the report.
- **Model**: `claude-sonnet-5`.
- **Attribution**: commits are `koba-jon` alone. No `Co-Authored-By` trailer.

## 1. Gates

```
git fetch origin && git status --short          # tree clean, develop == origin/develop
gh run list --workflow benchmark-production --limit 1   # none in flight
gh run list --limit 10 --json headSha,name,status,conclusion
```

`gh run list --commit <sha>` returns nothing in this gh build - **filter by
`headSha` instead**. An `until` loop over the broken filter exits at once and
proves nothing.

`ci`, `native-performance` and `benchmark-template` must be green on the tip.
`benchmark-template` only triggers when `benchmark/template/**` changed; if it
did not, a green run on the last commit that touched it counts.

## 2. Cache impact

```
python3 benchmark/template/scripts/benchmark.py cache-impact --source-repo .
```

Report `valid` and `invalid_by_evaluation`. Invalid records are re-measured and
paid for. Known-dead old-epoch records are expected; a *rise* in invalid count
means a change re-keyed something - name it before continuing.

Never edit `config/primary.json`, `config/benchmark_metadata.json` or any
`methodology/*.md` to fix something: each re-keys every record that reads it.

## 3. Smoke

Edit any line below the comment header of `benchmark/.run-smoke` (at least
`requested_at_utc`, and a `reason` naming the snapshot), commit, push to
`develop`. Wait for `benchmark-smoke`: 12/12 PASS, about 0.02 USD. Then wait for
`ci` and `native-performance` on that commit to go green too.

## 4. Request the run

Edit `benchmark/.run-production`: `requested_at_utc`, a `reason` saying only what
this run is (not the fix history), `model`, `evaluation: all`, `budget_usd: 5.0`.
Commit, push to `develop`.

**Any edit to `benchmark/.run-production` or `benchmark/.run-smoke` that reaches
`develop` starts a run.** Never touch either file in a commit that is not itself
the request, and never force-push `develop` while a paid marker is in place - a
forced update re-triggers the workflow even with an identical marker.

## 5. Wait

Poll in the background until the run completes; do not block the session on a
foreground sleep.

```
until [ "$(gh run view <id> --json status -q .status)" = "completed" ]; do sleep 60; done
```

Watch the preparation step: it should finish in under ~15 minutes including the
image build. If it runs much longer the mechanical Language Quality records
failed to hydrate and the 5h19m micro suite is re-running - stop and report.

**Do not push anything to `develop` until the run finishes.** The measurement
window is frozen from manifest-merge to finalize, and a moved `develop` turns
the result into a workflow artifact instead of a commit.

## 6. Report

Pull the result commit and read `benchmark/<run-id>/`:

- `summary.json` / `rankings.json` - status and ranking per evaluation
- `breakdown/<evaluation>.md` - per-requirement scores, each requirement's own
  ranking, the weights, and a reconstruction check that must read `0.00e+00`
- `evidence/<evaluation>/<agent>.json` - the workers' own reasoning per cell

Report: spend and paid call count, cache hits/misses, the status of all five
evaluations, Quidra's rank and score in each, and every blocker with its stated
reason. If an evaluation is blocked, read the blocking unit's `result.json` in
the run artifact and say whether the blocker is a real finding or a wiring bug -
do not describe a blocked run as a success.

If the workflow succeeded but an evaluation is PARTIAL or WITHDRAWN, the run did
not produce a complete result. Say so plainly.
