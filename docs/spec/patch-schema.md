# Source Patch Schema

`quidra patch FILE.qui PATCH.json` applies revision-safe edits to source nodes returned by `quidra inspect`. The patch protocol is public and versioned independently of the compiler's internal C++ AST.

The compiler accepts schema versions **1, 2, and 3**. Versions 1 and 2 remain supported for compatibility. **Version 3 is preferred for new machine clients** because an edit may carry either an ordinary Quidra source fragment or one public structural-node payload.

The authoritative machine-readable JSON Schema is available from the compiler:

```bash
quidra describe patch-schema
```

## Workflow

1. Run `quidra inspect program.qui > inspect.json`.
2. Read the root `revision`, then select the target `node_id`, `kind`, and `source_hash`.
3. Build a schema-version-3 patch.
4. Run `quidra patch program.qui patch.json` to validate and print the result without writing it.
5. Run `quidra patch program.qui patch.json --write` to atomically replace the source.
6. Re-inspect the new revision before issuing another node-addressed patch.
7. Run `quidra check program.qui --json` and the relevant tests.

Node IDs are **revision-local capabilities**, not persistent object identities. Any accepted source change invalidates the old revision and its node IDs.

`quidra inspect --no-source` keeps revision, node identity, kind, source hash, hierarchy, inferred type/authority metadata, and effect summaries while omitting source fragments.

## Schema version 3

The root object has exactly three fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `schema_version` | integer | Must be `3`. |
| `base_revision` | string | The SHA-256 `revision` returned by `quidra inspect`. |
| `operations` | non-empty array | Ordered edit requests. |

Every operation identifies the inspected target with `node_id`, `expected_hash`, and `expected_kind`. Supported operations are `replace_node`, `insert_before`, `insert_after`, and `delete_node`.

A replace/insert operation carries **exactly one** of these forms:

- `"replacement": "Quidra source fragment"`
- `"replacement_node": {"kind": "...", "source": "canonical Quidra source"}`

A structural replacement must use the target node's structural kind. Structural insertions are validated by the ordinary parser/checker at their insertion site. When a patch contains a structural-node payload, the complete edited file is rendered through the canonical formatter before file-aware validation and write; therefore source -> public structure -> source has the same deterministic representation as `quidra fmt`. Source-fragment-only patches retain their byte-preserving behavior. The complete edited file always passes through normal file-aware validation before it can be written.

Example:

```json
{
  "schema_version": 3,
  "base_revision": "<revision from quidra inspect>",
  "operations": [
    {
      "op": "replace_node",
      "node_id": "n000002",
      "expected_hash": "<source_hash>",
      "expected_kind": "integer",
      "replacement_node": {
        "kind": "integer",
        "source": "42"
      }
    }
  ]
}
```

Source-fragment edits remain first-class in v3:

```json
{
  "schema_version": 3,
  "base_revision": "<revision from quidra inspect>",
  "operations": [
    {
      "op": "insert_before",
      "node_id": "n000004",
      "expected_hash": "<source_hash>",
      "expected_kind": "expression_statement",
      "replacement": "print(\"before\")\n"
    }
  ]
}
```

`delete_node` has no replacement field.

### Trivia and comments

Structural edits must not silently discard comments. When the target span contains line-comment trivia, a structural edit is rejected with `PATCH_TRIVIA_CONFLICT`; the client should use a source-fragment edit that preserves the comment explicitly. Text such as `"https://..."` inside a string is not treated as comment trivia.

### Fail-closed identity checks

All four checks have distinct purposes:

- `base_revision` proves the patch was prepared for the current whole-file revision.
- `node_id` names one node within that revision.
- `expected_hash` proves the node's exact source fragment is unchanged.
- `expected_kind` proves the client still targets the intended structural role.

Unknown/stale/mismatched targets, overlapping edits, ambiguous edit boundaries, structured role mismatches, and invalid generated source are rejected before a write.

## Version 2 compatibility

Version 2 supports `replace_node`, `insert_before`, `insert_after`, and `delete_node`, but edit payloads use only the `replacement` source-fragment field. `expected_kind` is mandatory.

## Version 1 compatibility

Version 1 supports only `replace_node` and the source-fragment `replacement` field. It predates `expected_kind`; `base_revision`, `node_id`, and `expected_hash` remain mandatory.

Unknown or missing fields are rejected in every schema version rather than ignored.

## Validation model

Patch application never bypasses Quidra semantics. After edits are applied in memory, the complete updated file follows the ordinary file-aware validation path. Syntax, types, module resolution, generic specialization, definite initialization, storage authority/effects, and backend validation therefore use the same implementation as ordinary compilation.

`--write` occurs only after validation succeeds and uses atomic file replacement while preserving source permissions.

## Patch diagnostics

| Code | Meaning |
| --- | --- |
| `INVALID_PATCH` | JSON/schema/operation is malformed, unsupported, or contains missing/unknown fields. |
| `STALE_REVISION` | `base_revision` no longer matches the source. Re-inspect. |
| `UNKNOWN_NODE` | `node_id` is absent from the inspected revision. |
| `PATCH_HASH_MISMATCH` | The target source no longer matches `expected_hash`. |
| `PATCH_KIND_MISMATCH` | The target kind no longer matches `expected_kind`. |
| `PATCH_ROLE_MISMATCH` | A structural replacement's declared kind does not match its target role. |
| `PATCH_TRIVIA_CONFLICT` | A structural edit would risk dropping comment trivia; use a source edit. |
| `PATCH_OVERLAP` | Operations overlap or share an ambiguous edit boundary. |

Syntax, type, initialization, effect, module, and backend failures after editing use the same diagnostics as ordinary compilation.

## Machine discovery

`quidra describe llm` advertises inspect schema version 1, supported patch schema versions 1/2/3 with preferred version 3, structural syntax schema version 1, compiler diagnostic schema version 2, runtime provenance schema version 1, the grammar fingerprint, and supported patch operations. `quidra describe grammar` returns the authoritative grammar contract.
