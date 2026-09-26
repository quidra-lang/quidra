# Versioned Quidra benchmark results

Quidra is versioned separately from the nine comparison languages.

- The only version SSOT is `project.toml [project].version`.
- A recorded generation is named `quidra_vX.Y.Z`.
- Re-running with the same Quidra project version reuses that generation instead of overwriting it.
- A new Quidra project version is appended as another comparison row, so public score/ranking tables grow from 10 to 11, 12, ... rows.
- The original compact benchmark run remains unchanged for auditability.
- Model/sampling, benchmark config, cache epoch, comparison toolchains, and workload contracts still invalidate reuse; implementation/document drift inside the same declared Quidra version does not create a new generation.
