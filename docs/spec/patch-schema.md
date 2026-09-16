# Source Patch Schema

`quidra patch FILE.qui PATCH.json` applies revision-safe replacements to typed source nodes. A patch is accepted only if its base revision matches the inspected source, every target node and hash still matches, operations do not overlap, and the resulting source passes the normal frontend and backend validation path.

## Schema version 1

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

The root object has exactly three fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `schema_version` | integer | Must be `1`. |
| `base_revision` | string | The `revision` returned by `quidra inspect` for the source being edited. |
| `operations` | non-empty array | Ordered set of node replacements. |

Each operation has exactly four fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `op` | string | Must be `"replace_node"`. |
| `node_id` | string | A node identifier returned by `quidra inspect`. |
| `expected_hash` | string | The target node's `source_hash` from the same inspection. |
| `replacement` | string | Quidra source text that replaces exactly that node span. |

Unknown or missing fields are rejected rather than ignored.

## Workflow

1. Run `quidra inspect program.qui > inspect.json`.
2. Read the root `revision`, then select a node's `node_id` and `source_hash`.
3. Build a schema-version-1 patch.
4. Run `quidra patch program.qui patch.json` to validate and print the resulting source without writing it.
5. Run `quidra patch program.qui patch.json --write` to atomically replace the source file.

The write form preserves file permissions and validates the updated source before replacement.

## Patch diagnostics

| Code | Meaning |
| --- | --- |
| `INVALID_PATCH` | The JSON is malformed, has the wrong schema, missing/unknown fields, an unsupported operation, or an empty operations array. |
| `STALE_REVISION` | `base_revision` no longer matches the source. Re-inspect before patching. |
| `UNKNOWN_NODE` | `node_id` is not present in the inspected revision. |
| `PATCH_HASH_MISMATCH` | The node exists but its source text no longer matches `expected_hash`. |
| `PATCH_OVERLAP` | Two requested replacements overlap in source space. |

Syntax, type, initialization, effect, module, and backend validation failures after replacement use the same diagnostics as ordinary compilation. There is no patch-only type system.
