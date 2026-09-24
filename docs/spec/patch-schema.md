# Source Patch Schema

`quidra patch FILE.qui PATCH.json` applies revision-safe edits to typed source nodes. A patch is accepted only if its base revision matches the inspected source, every target node still matches the expected source hash, operations do not conflict, and the resulting file passes the normal file-aware compiler validation path.

The compiler accepts schema versions **1** and **2**. Version 1 remains supported for compatibility. **Version 2 is preferred for new machine clients** because it records the expected structural node kind and supports insertion/deletion without asking a client to synthesize a larger textual replacement.

The authoritative machine-readable JSON Schema is available directly from the compiler:

```bash
quidra describe patch-schema
```

## Workflow

1. Run `quidra inspect program.qui > inspect.json`.
2. Read the root `revision`, then select node `node_id`, `kind`, and `source_hash` values.
3. Build a patch using schema version 2.
4. Run `quidra patch program.qui patch.json` to validate and print the resulting source without writing it.
5. Run `quidra patch program.qui patch.json --write` to atomically replace the source file.
6. Re-run `quidra check program.qui --json` or the program tests expected by the task.

`quidra inspect --no-source` retains revision, node identity, kind, source hash, hierarchy, inferred type/authority metadata, and effect summaries while omitting repeated source fragments.

## Schema version 2

The root object has exactly three fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `schema_version` | integer | Must be `2`. |
| `base_revision` | string | The `revision` returned by `quidra inspect`. |
| `operations` | non-empty array | Ordered structural edit requests. |

Every version-2 operation identifies the inspected structure with:

| Field | Type | Meaning |
| --- | --- | --- |
| `op` | string | `replace_node`, `insert_before`, `insert_after`, or `delete_node`. |
| `node_id` | string | A node identifier returned by `quidra inspect`. |
| `expected_hash` | string | That node's `source_hash` from the same inspection. |
| `expected_kind` | string | That node's `kind` from the same inspection. |
| `replacement` | string | Required for replace/insert; omitted for delete. |

Example:

```json
{
  "schema_version": 2,
  "base_revision": "<revision from quidra inspect>",
  "operations": [
    {
      "op": "replace_node",
      "node_id": "n000002",
      "expected_hash": "<source_hash from quidra inspect>",
      "expected_kind": "integer",
      "replacement": "42"
    },
    {
      "op": "insert_before",
      "node_id": "n000004",
      "expected_hash": "<source_hash from quidra inspect>",
      "expected_kind": "expression_statement",
      "replacement": "print(\"before\")\n"
    },
    {
      "op": "delete_node",
      "node_id": "n000007",
      "expected_hash": "<source_hash from quidra inspect>",
      "expected_kind": "expression_statement"
    }
  ]
}
```

`replace_node` replaces exactly the target span. `insert_before` inserts at the target span's start and `insert_after` inserts at its end. `delete_node` removes exactly the target span. Inserted/replacement text must include any separators or newlines needed at that boundary. This keeps the patch protocol syntax-neutral: the compiler validates the complete result rather than silently rewriting a fragment's meaning.

Operations that overlap, target nested conflicting spans, or request ambiguous insertion at a span being replaced/deleted are rejected. A client should re-inspect and produce a smaller set of independent operations rather than depending on edit ordering.

### Why both kind and hash?

`base_revision` rejects changes to the whole source revision. `node_id` identifies the structural target in that revision. `expected_hash` verifies its exact text, while `expected_kind` verifies that the client intended the same syntactic role. Keeping all four checks in version 2 makes machine edits fail closed instead of applying to a structurally surprising target.

## Schema version 1 compatibility

Version 1 remains valid and supports only source replacement:

```json
{
  "schema_version": 1,
  "base_revision": "<revision from quidra inspect>",
  "operations": [
    {
      "op": "replace_node",
      "node_id": "n000002",
      "expected_hash": "<source_hash from quidra inspect>",
      "replacement": "42"
    }
  ]
}
```

Version-1 operation objects have exactly the four fields shown above. Unknown or missing fields are rejected rather than ignored.

## Validation model

Patch application does not bypass Quidra semantics. After edits are applied in memory, the complete updated file is processed through the ordinary file-aware validation path. Syntax, types, module resolution, generic specialization, definite initialization, storage authority/effects, and backend validation therefore use the same implementation as normal compilation.

`--write` occurs only after that validation succeeds and uses atomic file replacement while preserving source permissions.

## Patch diagnostics

| Code | Meaning |
| --- | --- |
| `INVALID_PATCH` | JSON is malformed, has an unsupported schema/operation, missing/unknown fields, or an empty operations array. |
| `STALE_REVISION` | `base_revision` no longer matches the source. Re-inspect before patching. |
| `UNKNOWN_NODE` | `node_id` is not present in the inspected revision. |
| `PATCH_HASH_MISMATCH` | The node exists but its source text no longer matches `expected_hash`. |
| `PATCH_KIND_MISMATCH` | A version-2 node exists but its structural `kind` differs from `expected_kind`. |
| `PATCH_OVERLAP` | Requested edits overlap or have an ambiguous shared insertion/edit boundary. |

Syntax, type, initialization, effect, module, and backend validation failures after editing use the same diagnostics as ordinary compilation. There is no patch-only type system.

## Machine discovery

For a compact compiler-supported workflow description, use:

```bash
quidra describe llm
```

The language grammar itself is available with:

```bash
quidra describe grammar
```

These commands are intended for tooling so clients do not need to scrape prose documentation to discover the canonical grammar or patch protocol.
