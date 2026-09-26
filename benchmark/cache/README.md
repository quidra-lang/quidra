# Certified benchmark cache

This directory contains the current certified measurement cache.

## Canonical baseline

2026-09-25-5dc8989-gh25 is the canonical baseline. It completed all five Primary evaluations. Every record retained under v1 was either hydrated and accepted by the current validator in that run or newly certified by that run.

The cache is self-contained. Reuse does not require an older run directory, historical cache record, migration snapshot, recertification report, or preserved source record. A record is reused only at its exact current fingerprint, and the stored result is still passed through the current validator before the unit becomes COMPLETE.

Records measuring Quidra additionally require an explicit trusted compiler/runtime execution identity. If benchmark inputs change, only records whose exact current fingerprint or Quidra execution identity changes are remeasured. There is no compatibility-migration fallback.


## Versioned Quidra generations

The fixed scientific run still evaluates the same ten language names. Every
single-language cache leaf is stored under an immutable versioned generation,
for example `python_v3.12.3`, `cpp_v20`, or `quidra_v0.3.0`; Semantic
Compression appends its probe suffix to that generation ID. Non-Quidra versions
come from `benchmark/config.json`. Quidra is the deliberate exception: its
version value is resolved only from `project.toml [project].version`.

A version bump creates a new generation and leaves every older generation
intact. Only shards assigned to the changed language become cold. Rubrics,
prompts, validators, workloads, model/sampling identity and toolchain
fingerprints remain independent scientific cache dependencies.
