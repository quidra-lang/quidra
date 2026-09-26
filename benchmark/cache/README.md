# Certified benchmark cache

This directory contains the current certified measurement cache.

## Canonical baseline

2026-09-25-5dc8989-gh25 is the canonical baseline. It completed all five Primary evaluations. Every record retained under v1 was either hydrated and accepted by the current validator in that run or newly certified by that run.

The cache is self-contained. Reuse does not require an older run directory, historical cache record, migration snapshot, recertification report, or preserved source record. A record is reused only at its exact current fingerprint, and the stored result is still passed through the current validator before the unit becomes COMPLETE.

Records measuring Quidra additionally require an explicit trusted compiler/runtime execution identity. If benchmark inputs change, only records whose exact current fingerprint or Quidra execution identity changes are remeasured. There is no compatibility-migration fallback.


## Versioned Quidra generations

The fixed scientific run still evaluates the language named `Quidra`.  Its
cache identity is versioned from `project.toml [project].version`: single-language
Quidra leaves live under `quidra_vX.Y.Z` (and semantic-compression probe leaves
under `quidra_vX.Y.Z--<probe>`).  Each Primary evaluation also stores one
immutable `quidra_vX.Y.Z/generation.json` beside those ordinary cache records.
There is no separate Quidra history tree; publication discovers generations from
these cache records and can grow 10, 11, 12, ... comparison rows without changing
the fixed ten-language measurement plan.
