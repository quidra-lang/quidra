# Benchmark Worker Core Rules

This is the compact rule set embedded into ordinary leaf-worker Task Packets.

1. Evaluate exactly the languages listed under **Assigned languages** in the Task Packet. If no subset is listed, evaluate the full fixed set: Quidra, Python, C++, Rust, Go, Java, TypeScript, Kotlin, Swift, Zig.
2. Apply one frozen contract symmetrically. Never alter a workload, rubric, timeout, evidence depth, prompt budget, or success criterion to favor or penalize one language.
3. Preserve raw evidence before normalization. Do not fabricate a value. If evidence is unavailable, record the blocker; unfinished applicable work is never `N/A`.
4. Normalized metric/condition scores use 0-100 with 100 best. Follow the selected methodology sections in this packet for the required raw measurement and normalization rule.
5. A genuine `N/A` is represented as `{"status":"N/A","reason":"..."}`. A missing capability intentionally tested by the metric is not automatically N/A.
6. Gate and coverage requirements return booleans supported by evidence. A valid negative gate result is evidence, not a validator failure; the runner will withhold the score mechanically.
7. Work only on the requirement IDs in this packet. Do not read the root conversation, historical run directories, sibling-agent outputs, or unrelated evaluation specifications.
8. Write only inside your assigned directory. Preserve supporting raw files there and write the required summary to `result.json`.
9. Network access is disabled unless the Task Packet explicitly enables it. Do not browse during timing measurements.
10. Independent scored LLM trials must not share previous trial generations, repairs, or hidden parent conversation state. Provider/transport failures are infrastructure failures, not incorrect language/model results.
11. Do not hand-copy a final ranking. The runner calculates Primary scores and rankings from validated requirement outputs.

## Standard result.json

For ordinary requirement workers:

```json
{
  "schema_version": 1,
  "evaluation": "<evaluation>",
  "requirements": {
    "<gate-or-coverage-id>": true,
    "<metric-or-condition-id>": {
      "Quidra": 0,
      "Python": 0,
      "C++": 0,
      "Rust": 0,
      "Go": 0,
      "Java": 0,
      "TypeScript": 0,
      "Kotlin": 0,
      "Swift": 0,
      "Zig": 0
    }
  },
  "evidence": {}
}
```

Metric/condition values are normalized 0-100 summaries. A language-sharded Task Packet returns only its assigned language keys; the runner merges disjoint shards and rejects overlap or missing languages before aggregation. Put raw measurements, formulas, source citations, diagnostics, and audit details under `evidence` or additional files in the worker directory.

For reusable-artifact currency audits with no Primary requirement IDs:

```json
{"schema_version":1,"evaluation":"<evaluation>","audit_pass":true,"evidence":{}}
```
