# Operating the benchmark

`master_prompt.md` is the contract every run follows. This page is the operator's
checklist: what to push, in which order, and what to do when a run does not
finish. It assumes nothing beyond a clone of this repository, `git` and
the GitHub Actions page of the repository.

## Before a run

1. `develop` must be green: `ci`, `benchmark-template` and `native-performance` on
   the commit you are about to evaluate.

   ```
   gh run list --commit "$(git rev-parse HEAD)" --limit 20
   ```

   Pass the full SHA. `gh run list --commit` does not resolve an abbreviated one:
   it prints nothing and exits zero, exactly as it does for a commit that has no
   runs at all. Check that each workflow you need is *present and successful*,
   never that "nothing is still running" - an empty list satisfies that and tells
   you nothing. `benchmark-template` only runs when `benchmark/template/**`
   changed; when it did not, its last run on the commit that did still counts.
2. No `benchmark-production` run may be in flight. Check the Actions page (or
   `gh run list --workflow benchmark-production --limit 1`). A push to `develop`
   while a run is inside its frozen window turns that run's result into a workflow
   artifact instead of a commit.
3. `python3 benchmark/template/scripts/benchmark.py cache-impact --source-repo .`
   lists which certified records the current tree keeps. Records that go invalid
   are measured again and paid for; see `cache/README.md` for what keys them.
4. Request the smoke on the same commit: edit any line below the comment header of
   `benchmark/.run-smoke` (for example `requested_at_utc`), commit, push to
   `develop`, and wait for `benchmark-smoke` to pass. It costs about two cents and
   proves the paid request shape, the prompt cache and the output ceiling against
   the live provider.

## Requesting a run

The request is a push to `develop` that changes `benchmark/.run-production`.
Nothing else starts paid inference. Edit the fields below the comment header:

| Field | Full run | One evaluation | Rehearsal of named units |
| --- | --- | --- | --- |
| `requested_at_utc` | now, in UTC | now | now |
| `reason` | one line | one line | one line |
| `model` | `claude-sonnet-5` | same | same |
| `evaluation` | `all` or omit | `semantic_compression`, `llm_learnability`, `language_quality`, `ecosystem` or `llm_proficiency` | omit (derived from the units) |
| `units` | omit | omit | comma-separated work unit ids of one evaluation |
| `budget_usd` | omit (100 USD soft guard) or up to 150 | same | at most 1.0 |

Do not push anything to `develop` until the run has finished. Do not force-push
`develop` while the marker requests anything you would not want re-run: GitHub
treats a forced update as a change to the marker and starts the workflow again.

## Reading the result

- A run imports a compact summary under `benchmark/<run-id>/` and pushes it to
  `develop` when `develop` has not moved; rankings are in `rankings.json`.
- `breakdown/<evaluation>.md` decomposes each published ranking: every measured
  requirement's score per language, that requirement's own ranking, the frozen
  weights that combined them, and a reconstruction check. The check recomputes
  the published score through the same functions the aggregate used and reports
  the largest disagreement; anything but zero to floating-point means the
  breakdown and the published score no longer describe the same run.
  `breakdown/<evaluation>.json` is the same content for tooling, and
  `breakdown/skipped.json` appears only when an evaluation could not be
  rendered, with the reason.
- `evidence/<evaluation>/<agent>.json` keeps what each worker actually wrote:
  its per-requirement results and the reasoning it gave for them. This is the
  only durable copy - the run artifact holding the agent traces expires.
- A ranking is published only for an evaluation whose ten languages are all
  `COMPLETE`. One `BLOCKED` unit withholds that evaluation's ranking and records
  the blocker instead; the other evaluations are unaffected.
- Every `COMPLETE` unit with a passing validator is certified into `cache/` by the
  checkpoint step, which runs even when the run fails later (finalize, import).
  Paid work is never lost to a late failure.
- The workflow artifact `benchmark-<run-id>` holds the full evidence: the ledger,
  every agent trace, the gateway audit log and the API cost breakdown.

## When a run does not finish

1. Read the checkpoint commit (`Checkpoint certified benchmark cache for ...`) and
   the artifact's `work/root/ledger.json`: each `BLOCKED` unit carries its blocker
   text and attempt history.
2. Fix the cause on `develop` (a validator rule, a runtime limit, a toolchain), keep
   `primary.json` untouched unless you accept re-measuring everything it keys, and
   check `cache-impact` again.
3. Request the run again. Every unit that completed before hydrates from the cache,
   so the second run pays only for the units that failed.

Rehearsals under one dollar (`units:` plus `budget_usd: 1.0`) are the way to try a
fix on one unit before paying for the rest.
