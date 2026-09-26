# Versioned Quidra benchmark results

Quidra is versioned separately from the nine comparison languages.

- The only version SSOT is `project.toml [project].version`.
- A recorded generation is named `quidra_vX.Y.Z`.
- Re-running with the same Quidra project version reuses that generation instead of overwriting it.
- The first recorded Quidra generation occupies the original Quidra slot. Each later project version is appended after the nine fixed peers, so public score/ranking tables grow 10, 11, 12, 13, ... with no fixed upper bound.
- The original evidence and detailed scientific breakdown remain unchanged for auditability. The existing compact run's public `summary.json` and `rankings.json` are relabeled to `quidra_v0.3.0`; future public results use the same versioned identifier scheme.
- Model/sampling, benchmark-owned rubrics/workloads/config, cache epoch, comparison toolchains, and workload contracts still invalidate reuse; implementation/document drift inside the same declared Quidra version does not create a new generation. If the retained generation fails the current free validator, the run stops and requires a version bump instead of buying another result for the same version.
