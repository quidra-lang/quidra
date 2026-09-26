# Certified benchmark cache

This directory contains the current certified measurement cache.

## Canonical baseline

2026-09-25-5dc8989-gh25 is the canonical baseline. It completed all five Primary evaluations. Every record retained under v1 was either hydrated and accepted by the current validator in that run or newly certified by that run.

The certified leaf cache is self-contained for ordinary reuse. Records normally hydrate at their exact current fingerprint and are passed through the current validator before the unit becomes COMPLETE. Narrow generation-preserving migrations exist only where the repository carries an explicit certified bridge, such as the one-time Language Quality raw-shard migration.

Quidra's compiler/runtime execution identity is retained for provenance and diagnostics, but it does not create a second result inside the same `project.toml [project].version` generation. Same-version Quidra implementation drift therefore reuses the immutable generation; benchmark-owned scientific dependencies remain independently validated.


## Versioned language generations

The fixed scientific run still evaluates the same ten language names. Every
single-language cache leaf is stored under an immutable versioned generation,
for example `python_v3.12.3`, `cpp_v18.1.3`, or `quidra_v0.3.0`; Semantic
Compression appends its probe suffix to that generation ID. Non-Quidra versions
come from `benchmark/config.json`. Quidra is the deliberate exception: its
version value is resolved only from `project.toml [project].version`.

A version bump creates a new generation and leaves every older generation
intact. Only shards assigned to the changed language become cold. Rubrics,
prompts, validators, workloads, model/sampling identity and toolchain
fingerprints remain independent scientific cache dependencies.

## Historical normalization raw

Publication keeps the scientific measurement cohort fixed at the ten current
languages, but retains older language generations for comparison. Language
Quality Family-C metrics and Semantic Compression min-max metrics are therefore
re-normalized from immutable raw values across all published generations before
Primary scores and rankings are recomputed. The baseline raw-seed files under
`v1/language-quality` and `v1/semantic-compression` are migration-only bridges
for the canonical run that predates direct raw preservation; newly created
generations store their normalization raw with the generation itself.
