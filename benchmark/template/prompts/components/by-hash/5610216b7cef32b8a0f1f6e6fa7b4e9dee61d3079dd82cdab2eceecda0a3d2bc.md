# Task Packet: worker-sc-metrics-local--part-2--typescript

Evaluation: semantic_compression

Parent: RUNNER
Goal: Produce validated requirement-level evidence for semantic_compression: metric.semantic_locality. Assigned language set: TypeScript. Write result.json.

Frozen Primary requirement IDs served by this work unit:
- metric.semantic_locality

Canonical fragment contract:
- Use exactly the supplied catalog (7b6737309b52a1419a01b3336b24dc03de273282713d78e5583bd3266acf1f4a).
- Do not author, substitute, or rewrite a probe fragment.
- A NONE catalog entry has no measurable fragment; do not emit a numeric per-probe A/B/C/D value for it.
- Write evidence.canonical_fragment_catalog_sha256 with exactly the catalog SHA-256 above.

## Assigned requirement IDs
- metric.semantic_locality

## Assigned languages
- TypeScript

## Readable paths
- /quidra-benchmark/template/methodology-assets/semantic_compression
- /quidra-benchmark/work/audit/semantic-compression/canonical_fragments_typescript.json

## Writable path
- /quidra-benchmark/work/agents/worker-sc-metrics-local--part-2--typescript

## Expected outputs
- /quidra-benchmark/work/agents/worker-sc-metrics-local--part-2--typescript/result.json

## Validation
`python3 /quidra-benchmark/template/scripts/benchmark.py result-check --workspace /quidra-benchmark --id worker-sc-metrics-local--part-2--typescript`

## Execution permissions
- Network: disabled
- Further delegation depth remaining: 1

## Worker isolation
- Worker mode: packet-only
- Local filesystem, shell, process, editor, IDE, and host-application tools: forbidden
- All permitted local source inputs are embedded in this packet.
- Provider-level network retrieval: disabled
- Return exactly one JSON Worker Response envelope; do not write files directly.
- Return result.json as the expected task output.
- Worker Response transport schema: {"schema_version":1,"task_id":"worker-sc-metrics-local--part-2--typescript","files":[{"path":"result.json","content":"<UTF-8 text>"}]}

## Rules
- This packet plus its embedded inputs (packet-only) or listed sandbox paths (sandbox-agent) is the complete task context.
- Do not depend on the parent conversation or hidden context.
- In packet-only mode, do not invoke local filesystem/shell/process/editor/application tools; return files only through the Worker Response JSON.
- In sandbox-agent mode, do not write outside the writable path and do not access paths outside the listed readable paths.
- Do not read sibling agent outputs unless explicitly listed above.
- Preserve machine-readable evidence required by the methodology.
- Use commands for mechanical work when a reusable command exists.
- If delegating, create a new self-contained Task Packet under the same policy.
- Do not mark the task COMPLETE unless the validation command succeeds.
- For a primary-evaluation task, the compact worker rules, selected methodology sections, assigned requirement IDs, and frozen primary configuration embedded below are authoritative.
- Do not read historical benchmark run directories.
- Packet-only workers return result.json through the Worker Response; sandbox-agent workers write result.json in the writable directory.
- A child agent must receive its own persisted self-contained Task Packet. Do not pass implicit parent conversation state.
