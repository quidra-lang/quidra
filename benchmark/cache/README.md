# Certified benchmark cache

This directory contains the current certified measurement cache.

## Self-contained certified state

The v1 tree is the reusable source of truth. A normal same-generation rerun must not depend on a dated run directory, migration fixture, or historical artifact. Every retained leaf is revalidated by the current runner before it becomes COMPLETE. Generation metadata carries the compact normalization raw required for deterministic re-normalization.

## Certified evidence correction

The certified Quidra v0.3.0 Learnability I2 evidence contains seed scores
62, 62 and 48 and explicitly reports their condition mean as 57.33, while the
worker's requirement field incorrectly reported 71. The certified Quidra
generation therefore uses 57.33 for I2 and 71.966 for the aggregate. This is a
deterministic correction from already-preserved evidence and requires no model
or paid API re-execution.

Current result validation rejects a material (>0.5 point) disagreement between a
Learnability condition's explicit evidence mean and its reported requirement
score, while tolerating legacy whole-point presentation rounding.

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
Quality Family-C metrics, Semantic Compression min-max metrics, and the five
comparison-dependent LLM Proficiency efficiency metrics are therefore
re-normalized from immutable raw values across all published generations before
Primary scores and rankings are recomputed.

For LLM Proficiency, each generation stores only the minimum per-trial raw needed
for that re-normalization: source tokens, task-completion tokens, generated-code
runtime, peak memory, compile time, and whether a fully correct source exists,
plus the frozen contract/trial-set hashes and mechanical measurement epoch. It
does not store generated source, provider conversations, or full trial traces.
Every certified generation stores this compact raw directly with the generation itself. No migration source or dated run directory is required for reuse.
