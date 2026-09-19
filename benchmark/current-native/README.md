# Current native micro diagnostic

This directory is deliberately separate from the frozen 2026-09-17 benchmark.
It is **not** part of the primary benchmark publication gate and never rewrites
historical sources or results.

The runner reuses the frozen correctness oracle, C++/Rust/Go sources, build
recipes, and measurement code. For Quidra it overlays only workloads whose
historical source encoded a capability gap that no longer exists:

- MB03: native `XOR` replaces the historical arithmetic XOR synthesis.
- MB08: `uint8[]` working bytes plus explicit `string.from_utf8(bin)` replace
  the historical per-byte glyph-array + `join` text reconstruction.
- MB09: the frozen local identifier `bin` is renamed because `bin` is now a reserved built-in type name; the workload itself is unchanged.
- MB10: current linear sole-owner string append replaces the historical
  million-element `string[]` staging workaround.
- MB11: public `map.Map.remove` replaces the historical custom fallback map.

The other seven Quidra sources are byte-for-byte inherited from the frozen
suite. Correctness is gated against the same golden output before timing.

Typical invocation:

```text
QUIDRA_BIN=/path/to/quidra python3 benchmark/current-native/run.py \
  --configs quidra_native,cpp,rust,go --repeats 5 --warmups 2 --out result.json
```

The historical config called `quidra_interpreter` actually invokes
`quidra run`, which includes compile + link + execute. This diagnostic exposes
that same recipe as `quidra_compile_execute`; it is not claimed to be an
interpreter or a persistent REPL.
