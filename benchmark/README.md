# Operating the benchmark

master_prompt.md is the contract every run follows. This page is the operator's
checklist. A paid benchmark is isolated on a temporary branch named **benchmark**;
**develop** never starts **benchmark-production** or **benchmark-smoke**.

## Lifecycle

The normal full-run lifecycle is:

1. The user asks to start the benchmark.
2. Read this file before touching the repository.
3. Confirm the preconditions below.
4. Create **benchmark** from the exact **develop** commit to evaluate, create
   benchmark/.run-production only on **benchmark**, commit that request there,
   and push it. The request marker must never exist on **develop**.
5. GitHub Actions runs **benchmark-production** on **benchmark**. Free preparation
   hydrates every currently provable certified cache hit and verifies the remaining
   budget before a single provider smoke. The five Primary evaluations then run in
   order: **Semantic Compression → LLM Learnability → Language Quality → Ecosystem
   → LLM Proficiency**. Each Primary has an initial slice plus two bounded recovery
   slices; completed evaluations fast-forward those recovery jobs without rebuilding
   the runtime or making paid calls. A separate **finalize** job follows. A scoped
   one-evaluation/rehearsal request still uses the single scoped job.
6. Do not write to **benchmark** while the workflow runs. **develop** may continue
   moving independently. The workflow itself may advance **benchmark** between
   evaluation jobs, but those commits contain only certified cache/prompt
   artifacts; every job still evaluates the exact original trigger SHA.
7. Each evaluation job checkpoints every newly certified reusable unit before
   the next job starts and uploads a compressed workspace handoff for the next
   top-level job. Estimated API spend is carried in that trusted handoff, so the
   request's budget is one workflow-wide soft guard rather than five independent
   budgets. The final **finalize** job receives the fifth handoff and publishes a
   formal compact result only when all five Primary evaluations are
   **COMPLETE**. An incomplete attempt never creates benchmark/<run-id>/; its
   diagnostic handoff/workspace evidence remains in Actions artifacts and its
   valid completed work remains reusable through benchmark/cache/.
8. Reconcile only benchmark-generated artifacts into the then-current
   **develop**. Never merge or fast-forward the disposable **benchmark** branch
   itself: it contains the paid-run request marker and workflow durability
   commits that are not source history.
9. Verify that benchmark/.run-production is absent on **develop**. Only after the
   useful cache and any formal result are safely present there, delete the remote
   and local **benchmark** branch. The benchmark is then finished.

Never leave a completed run stranded only on **benchmark**, and never delete
**benchmark** before its result has been reconciled.

## Before a run

1. Fetch the latest refs and make sure there is no existing remote **benchmark**
   branch.

~~~sh
git fetch origin --prune
if git ls-remote --exit-code --heads origin benchmark >/dev/null 2>&1; then
  echo "benchmark already exists; inspect and reconcile it before starting another run"
  exit 1
fi
~~~

An existing **benchmark** branch may contain an in-flight or completed paid run.
Never overwrite or force-push it just to start a new run.

2. **develop** must be green for the source snapshot being evaluated: ci,
   benchmark-template, and native-performance must have passed where applicable.

~~~sh
git checkout develop
git pull --ff-only origin develop
test ! -e benchmark/.run-production || {
  echo "stale benchmark/.run-production must not live on develop"
  exit 1
}
gh run list --commit "$(git rev-parse HEAD)" --limit 20
~~~

Pass the full SHA. gh run list --commit does not resolve an abbreviated one.
benchmark-template only runs when its paths changed, so its latest applicable
successful run is sufficient.

3. No benchmark-production run may already be in flight.

~~~sh
gh run list --workflow benchmark-production --limit 5
~~~

4. Check the known structural cache impact before paying for anything.

~~~sh
python3 benchmark/template/scripts/benchmark.py cache-impact --source-repo .
~~~

This reports certified records already known to be invalid from the current
checkout: configuration, methodology, readable inputs, toolchain pins, declared
cache epoch, mechanical scripts, Quidra declared version, and the target execution identity. It does **not**
guess an exact Task Packet hash when that packet depends on a new upstream result
that does not exist yet. During a run, deterministic preparation and cache
hydration resolve each fingerprint before the corresponding paid unit is
dispatched, and each Primary job prints the then-known exact HIT/MISS state.
Therefore a downstream packet whose bytes depend on a newly produced uncached
result cannot have its final cache decision before that dependency exists. See
cache/README.md for the cache keys.

The standalone benchmark-smoke workflow remains available for diagnostics, but
it also runs only on **benchmark**. A normal full run does not need it because
benchmark-production performs the same provider smoke before expensive work.

## Requesting a full run

Start from the exact current **develop** head and create the disposable branch.

~~~sh
git fetch origin develop --prune
git checkout -B benchmark origin/develop
~~~

Create benchmark/.run-production on the disposable branch, then commit and push
**benchmark**. The file is deliberately absent from **develop** so creating the
temporary branch alone can never carry a stale paid-run request forward.

| Field | Full run | One evaluation | Rehearsal of named units |
| --- | --- | --- | --- |
| requested_at_utc | now, in UTC | now | now |
| reason | one line | one line | one line |
| model | claude-sonnet-5 | same | same |
| evaluation | all or omit | semantic_compression, llm_learnability, language_quality, ecosystem or llm_proficiency | omit (derived from the units) |
| units | omit | omit | comma-separated work unit ids of one evaluation |
| budget_usd | omit (100 USD soft guard) or up to 150 | same | at most 1.0 |

~~~sh
git add benchmark/.run-production
git commit -m "Request benchmark run"
git push -u origin benchmark
~~~

Only a push to **benchmark** that creates or changes benchmark/.run-production
starts the production workflow. It does **not** immediately spend the requested
budget. The prepare job first runs deterministic planning, hydrates every
certified cache record whose exact fingerprint is already materializable, and
computes a conservative remaining-work budget from the unresolved units. If the
requested budget is below that recommendation, or the free preparation finds a
terminal blocker, the workflow stops before the provider is called. Only after
those free gates pass does one small provider smoke run; the five Primary jobs
then inherit that prepared workspace. A push to **develop** cannot start paid
work, and the marker must never be reconciled back to **develop**.

Do not modify, rebase, merge into, or force-push **benchmark** while the run is
in flight. The production workflow itself is the only writer allowed during
that window. Changes on **develop** are safe and do not disturb the frozen
benchmark snapshot.

## What the workflow writes

For a full run, the prepare job checks out the exact commit that triggered the
workflow, stages that immutable snapshot once, runs the no-provider deterministic
preparation and cache hydration, verifies the conservative budget, then packages
**benchmark-<run-id>-prepared**. Semantic Compression starts from that artifact;
each later Primary restores the previous job's compressed **.quidra-benchmark**
workspace handoff and recreates only the Git-private host guard. The scored
workspace therefore advances across the five jobs without ever changing the
evaluated source SHA. Cache checkpoint commits on the remote **benchmark** branch
are durability copies, not a new source snapshot.

A planned wall-clock slice is a checkpoint, not a benchmark failure. After the
normal five Primary jobs, each Primary has two bounded continuation passes, each
with its own long execution window. An evaluation already marked **COMPLETE**
skips the image build and paid dispatch entirely and simply forwards the handoff;
only unresolved work can consume a continuation window or API budget. Terminal
scientific or validator blockers are never silently converted into scores.

- Validated cache is checkpointed into benchmark/cache and pushed to
  **benchmark** after each of the five evaluation jobs. The full scored workspace
  is also handed to the next job as an Actions artifact. This makes a failure in
  job 3 resumable without losing paid work from jobs 1–2.
- Only an all-five-Primary **COMPLETE** full run is imported under
  benchmark/<run-id>/. A PARTIAL/BLOCKED/NOT_EXECUTED attempt is not a formal
  repository result; it keeps its diagnostic evidence in the workflow artifact
  while validated completed units are still checkpointed to benchmark/cache/.
- Promoted reusable prompts under benchmark/template/prompts are included as
  needed for a publishable complete run.
- The workflow never pushes benchmark output directly to **develop**.
- If another writer unexpectedly moves **benchmark**, the workflow stops racing
  the branch and preserves the local result in the workflow artifact.
- A full run keeps the prepared handoff, one normal handoff per evaluation and
  two continuation handoffs per evaluation. Every handoff carries the workspace,
  cumulative budget ledger and API-cost evidence for that slice. **finalize**
  consumes benchmark-<run-id>-recovery2-llm-proficiency, and only if all five
  evaluations are **COMPLETE** does it write benchmark-<run-id> with the combined
  formal result. A scoped request writes benchmark-<run-id>-scoped.

## Reconciling after the workflow finishes

Always reconcile artifacts selectively. This rule is the same whether or not
**develop** moved during the run. The disposable branch contains
benchmark/.run-production and workflow checkpoint commits, so merging or
fast-forwarding the whole branch would turn a one-shot paid-run request into
source history.

Fetch both branches and compute the fork point, then start from current
**develop**.

~~~sh
git fetch origin develop benchmark --prune
base="$(git merge-base origin/develop origin/benchmark)"

git checkout develop
git reset --hard origin/develop

tmp="$(mktemp)"
git diff --name-only "$base"..origin/benchmark -- benchmark \
  | grep -E '^benchmark/(cache/|template/prompts/|20[0-9]{2}-[0-9]{2}-[0-9]{2}-[^/]+/)' \
  > "$tmp"

test -s "$tmp" || {
  echo "no benchmark-generated artifacts found; do not delete benchmark"
  exit 1
}

while IFS= read -r path; do
  if git cat-file -e "origin/benchmark:$path" 2>/dev/null; then
    git restore --source=origin/benchmark -- "$path"
  else
    git rm -f --ignore-unmatch -- "$path"
  fi
done < "$tmp"

git add -f benchmark/cache benchmark/template/prompts benchmark/20??-??-??-* 2>/dev/null || true
test ! -e benchmark/.run-production || {
  echo "refusing to import the paid-run request marker into develop"
  exit 1
}
git diff --cached --quiet && {
  echo "nothing staged; do not delete benchmark"
  exit 1
}

git commit -m "Import benchmark results from isolated benchmark branch"
git push origin develop
rm -f "$tmp"
~~~

This preserves current **develop** source/template changes, imports only reusable
cache/prompts and a publishable dated result, and deliberately excludes the
request marker. If the selective import conflicts semantically with newer cache
or prompt material on **develop**, resolve that deliberately before pushing.

## Deleting the temporary branch

Delete **benchmark** only after the successful **develop** push above and after
verifying the imported run exists on origin/develop.

~~~sh
git fetch origin develop
# Inspect the expected benchmark/<run-id>/import_manifest.json on origin/develop.
git show origin/develop:benchmark/.run-production >/dev/null 2>&1 && {
  echo "request marker leaked into develop; do not delete benchmark"
  exit 1
} || true
git push origin --delete benchmark
git branch -D benchmark 2>/dev/null || true
~~~

A failed workflow is different: keep **benchmark** until its cache/evidence has
been inspected and any useful checkpoint has been reconciled. Never delete a
failed-run branch merely to get back to a clean branch list.

## Reading the result

- Rankings are in benchmark/<run-id>/rankings.json.
- breakdown/<evaluation>.md decomposes each published ranking: every measured
  requirement's score per language, that requirement's ranking, the frozen
  weights that combined them, and a reconstruction check.
- breakdown/<evaluation>.json contains the same data for tooling, and
  breakdown/skipped.json appears only when an evaluation could not be rendered.
- evidence/<evaluation>/<agent>.json keeps each worker's per-requirement results
  and reasoning.
- A ranking is published only for an evaluation whose ten languages are all
  COMPLETE. One BLOCKED unit withholds that evaluation's ranking; other
  evaluations are unaffected.
- Every COMPLETE unit with a passing validator can be certified into cache/ by
  the checkpoint step even when a later step fails.

## When a run does not finish

An incomplete attempt is deliberately **not** a formal benchmark result. It does
not create benchmark/<run-id>/ in Git, so the next complete run can remain the
first published result while still reusing every certified unit from earlier
attempts.

1. Keep **benchmark** until its cache checkpoint is reconciled. Read the
   checkpoint commit and the workflow artifact's
   work/root/ledger.json; each BLOCKED unit carries its blocker and attempt
   history.
2. Fix the cause on **develop**. Do not rewrite the failed benchmark branch into
   a new source snapshot.
3. Reconcile any useful certified cache from the failed **benchmark** branch
   into **develop**, then delete **benchmark**.
4. Start a fresh **benchmark** branch from the fixed **develop** and request the
   run again. Valid certified records hydrate from cache, so only invalid or
   unfinished units are paid again.

Rehearsals under one dollar (units plus budget_usd: 1.0) remain the safe way to
test one work unit before paying for the rest.
