# Quidra Comprehensive Benchmark — run `2026-09-17-7677581`

Evaluated target: **Quidra `7677581`** on branch `develop`, working tree clean at benchmark start.
Specification: `prompt.md` (an immutable copy of `benchmark/master_prompt.md` as it stood at run start;
sha256 verified identical at copy time).

This run measures four **independent** primary evaluations and deliberately never combines them:

1. Semantic Compression
2. Standard
3. LLM Intrinsic / Unknown-Language Learnability
4. LLM Standard / Knowledge-Dependent Performance

Every normalized score obeys **100 = best, 0 = worst, higher is always better**. `N/A` is never rendered
as zero. No cross-evaluation total or combined ranking exists anywhere in this run; `scripts/report.py`
actively refuses to render one.

---

## Read this first

Two documents matter more than the score tables:

- **`methodology/CORRECTIONS.md`** — every defect found during the run, including **D-7**, which records
  that the first-draft methodology was **systematically biased toward Quidra** (10 of 10 documents
  flagged; 35 blockers) and was remediated before any score was computed.
- **`methodology/00_cross_language_constraints.md`** — the ten frozen cross-language constraints
  (C-1…C-10), each resolving a fairness hazard that would otherwise have measured the harness instead of
  the languages.

An independent finding about the implementation under test is recorded separately in
**`QUIDRA_DEFECT_string_concat_stack_leak.md`**: a reproducible SIGSEGV in Quidra 0.2.0.

---

## Layout

```
prompt.md                         immutable specification actually used
README.md                         this file
environment/environment.json      host, toolchains, BOTH frozen recipe sets, reference SHAs
methodology/                      the frozen methodology (10 documents) + corrections
  _pre_remediation_snapshot/      first-draft text + SHA256SUMS (superseded, preserved as evidence)
  _audit_findings/                the full adversarial audit that forced the remediation
micro/                            micro-benchmark sources, reference oracles, golden output, raw results
algo/                             SVM / GMM / LightGrad references and frozen goldens
semantic-compression/             probe fragments, raw counts, scores
  _superseded_run1/               abandoned first probe pass (not used in any score)
standard/                         adversarial programs, rubric evidence, raw results
llm-practical/ llm-intrinsic/     trial prompts, generations, repair histories, raw results
scoring/ results/ charts/         normalization outputs, final tables, figures
scripts/                          every measurement and scoring script (below)
```

## Scripts

| Script | Role |
|---|---|
| `langs.py` | the only place that knows how each language builds and runs; 11 execution targets |
| `measure.py` | wall/CPU/peak-RSS capture; median over repeats; macOS `time -l` RSS is **bytes** |
| `check_micro.py` | micro correctness gate at the frozen tolerance |
| `run_micro_suite.py` | build → correctness gate → timing, serially; correctness gates timing |
| `verify.py` | build/run/classify oracle; distinguishes **silent bug** from failure |
| `tokenize_probe.py` | the frozen language-neutral tokenizer (authoritative for all ten) |
| `make_token_profiles.py` | generates the ten lexical profiles |
| `retokenize_all.py` | single authoritative token pass (required by CORRECTIONS D-4) |
| `score.py` | the frozen normalization families A–G, N/A policy, ranking |
| `report.py` | renders the §27/§28 tables; refuses any combined score |

Every one of these self-tests. Run them:

```bash
cd scripts
python3 score.py                  # all frozen formulas
python3 tokenize_probe.py --selftest
python3 langs.py /tmp/selftest    # compiles + runs hello-world in all 11 targets
python3 verify.py                 # proves the oracle both passes correct and rejects wrong
```

## Reproducing a measurement

```bash
export JAVA_HOME=/opt/homebrew/opt/openjdk      # required for Java and Kotlin

# micro: verify correctness only (safe under load)
python3 scripts/run_micro_suite.py --verify-only

# micro: timing — MUST run with no other load on the host
python3 scripts/run_micro_suite.py --repeats 5 --warmups 2

# semantic compression: one authoritative token pass over every probe
python3 scripts/retokenize_all.py
```

**Timing is only valid on an unloaded host.** Agent fan-out and timed measurement never overlap in this
run; every timing figure was taken serially.

## Validator soundness

Spec §10.4(c) requires every validator be able to **both** pass a correct input and reject a corrupted
one — a gate that cannot fail measures nothing, and one that cannot pass is just as broken (that second
failure mode really occurred here; see CORRECTIONS D-5). Verified:

- `check_micro.py` — passes golden; rejects a single-digit integer change; rejects empty output;
  accepts a float nudged within tolerance.
- `verify.py` — in all 11 targets, passes a correct program and classifies a wrong-but-clean-exit
  program as `silent_bug`.
- Algorithm goldens — each verified to pass itself and reject a perturbed copy.

## Provenance of expected values

No expected value rests on a single implementation:

- **Micro**: an independent C reference and an independent Python reference agree **byte-for-byte on all
  11 workloads**, including every float printed to 17 significant digits.
- **SVM / GMM / LightGrad**: two independent references per workload, agreeing byte-for-byte; the frozen
  golden is checksummed. **LightGrad is additionally verified against closed-form derivatives**
  (y=82, y′=113, y″=108, y‴=54, y⁗=0), so it is checked against mathematics rather than self-consistency.

## Git

Per spec §31, nothing in this run was committed, pushed, merged or rebased, and the Quidra
implementation was not modified. Results live only in the working tree.

## Honest scope

Items not executed are listed with their reasons in the results document under **N/A and Not Executed**,
never estimated and never fabricated. Where a cell was measured at a lower trial count than the
specification's five, the actual count is stated and differences of a few points between languages are
explicitly **not** claimed to be resolved by the measurement (spec §6.2).
