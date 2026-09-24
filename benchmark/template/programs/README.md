# Reusable Benchmark Programs

This subtree stores hand-written, run-independent comparison-language source/harness artifacts that are valid inputs to new runs.

Recommended layout:

```text
programs/
  python/<workload>/
  cpp/<workload>/
  rust/<workload>/
  go/<workload>/
  java/<workload>/
  typescript/<workload>/
  kotlin/<workload>/
  swift/<workload>/
  zig/<workload>/
```

## Reuse rule

For Python, C++, Rust, Go, Java, TypeScript, Kotlin, Swift and Zig, source/harness reuse is allowed when the frozen workload/contract and validator still match and the relevant toolchain is current or has passed a capability-currency audit.

Source/harness reuse by itself never reuses a measurement: execution-heavy mechanical measurements are rebuilt, checked, executed, validated and remeasured as required by their methodology. Separately, comparison-language LLM/judgment measurements may be restored from `benchmark/cache/` only when the complete certified input fingerprint matches and the current validator accepts the cached normalized result.

Quidra is the changing target. Its benchmark implementation is re-audited against every evaluated commit and regenerated when current capabilities make an older form stale or non-idiomatic.

## Storage model

Reusable sources are tracked directly here. A new benchmark run therefore needs only the current template; it never reconstructs benchmark inputs from a historical run directory.

`reuse/catalog.json` records current in-template asset IDs, paths, object hashes, workload/language metadata and last-validated toolchain fingerprints.
