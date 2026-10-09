#include "quidra/source_patch.hpp"

#include "quidra/compiler.hpp"
#include "quidra/formatter.hpp"
#include "quidra/source_tools.hpp"

#include "platform/strict_json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace quidra {
namespace {

constexpr std::string_view kPatchSchema = R"QUIDRA_SCHEMA({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "Quidra source patch",
  "description": "Revision-safe structural source edits for quidra patch.",
  "x-quidra-supported-versions": [1, 2, 3],
  "x-quidra-preferred-version": 3,
  "$defs": {
    "v1_operation": {
      "type": "object",
      "additionalProperties": false,
      "required": ["op", "node_id", "expected_hash", "replacement"],
      "properties": {
        "op": {"const": "replace_node"},
        "node_id": {"type": "string", "minLength": 1},
        "expected_hash": {"type": "string", "pattern": "^[0-9a-f]{64}$"},
        "replacement": {"type": "string"}
      }
    },
    "v2_edit_operation": {
      "type": "object",
      "additionalProperties": false,
      "required": ["op", "node_id", "expected_hash", "expected_kind", "replacement"],
      "properties": {
        "op": {"enum": ["replace_node", "insert_before", "insert_after"]},
        "node_id": {"type": "string", "minLength": 1},
        "expected_hash": {"type": "string", "pattern": "^[0-9a-f]{64}$"},
        "expected_kind": {"type": "string", "minLength": 1},
        "replacement": {"type": "string"}
      }
    },
    "v2_delete_operation": {
      "type": "object",
      "additionalProperties": false,
      "required": ["op", "node_id", "expected_hash", "expected_kind"],
      "properties": {
        "op": {"const": "delete_node"},
        "node_id": {"type": "string", "minLength": 1},
        "expected_hash": {"type": "string", "pattern": "^[0-9a-f]{64}$"},
        "expected_kind": {"type": "string", "minLength": 1}
      }
    },
    "structural_node": {
      "type": "object",
      "additionalProperties": false,
      "required": ["kind", "source"],
      "properties": {
        "kind": {"type": "string", "minLength": 1},
        "source": {"type": "string"}
      }
    },
    "v3_edit_operation": {
      "type": "object",
      "additionalProperties": false,
      "required": ["op", "node_id", "expected_hash", "expected_kind"],
      "properties": {
        "op": {"enum": ["replace_node", "insert_before", "insert_after"]},
        "node_id": {"type": "string", "minLength": 1},
        "expected_hash": {"type": "string", "pattern": "^[0-9a-f]{64}$"},
        "expected_kind": {"type": "string", "minLength": 1},
        "replacement": {"type": "string"},
        "replacement_node": {"$ref": "#/$defs/structural_node"}
      },
      "oneOf": [
        {"required": ["replacement"], "not": {"required": ["replacement_node"]}},
        {"required": ["replacement_node"], "not": {"required": ["replacement"]}}
      ]
    }
  },
  "oneOf": [
    {
      "type": "object",
      "additionalProperties": false,
      "required": ["schema_version", "base_revision", "operations"],
      "properties": {
        "schema_version": {"const": 1},
        "base_revision": {"type": "string", "pattern": "^[0-9a-f]{64}$"},
        "operations": {
          "type": "array",
          "minItems": 1,
          "items": {"$ref": "#/$defs/v1_operation"}
        }
      }
    },
    {
      "type": "object",
      "additionalProperties": false,
      "required": ["schema_version", "base_revision", "operations"],
      "properties": {
        "schema_version": {"const": 2},
        "base_revision": {"type": "string", "pattern": "^[0-9a-f]{64}$"},
        "operations": {
          "type": "array",
          "minItems": 1,
          "items": {
            "oneOf": [
              {"$ref": "#/$defs/v2_edit_operation"},
              {"$ref": "#/$defs/v2_delete_operation"}
            ]
          }
        }
      }
    },
    {
      "type": "object",
      "additionalProperties": false,
      "required": ["schema_version", "base_revision", "operations"],
      "properties": {
        "schema_version": {"const": 3},
        "base_revision": {"type": "string", "pattern": "^[0-9a-f]{64}$"},
        "operations": {
          "type": "array",
          "minItems": 1,
          "items": {
            "oneOf": [
              {"$ref": "#/$defs/v3_edit_operation"},
              {"$ref": "#/$defs/v2_delete_operation"}
            ]
          }
        }
      }
    }
  ]
})QUIDRA_SCHEMA";

using Json = platform::JsonValue;

// The patch schema is a fixed, shallow shape, so a nesting limit of 64 is
// about ten times its real need. Without one, 50,000 nested '[' exhaust the
// stack: patches are the one JSON input here that is untrusted.
Json parse_patch_json(std::string_view text) {
    try {
        return platform::read_strict_json(text, {"patch JSON", 64});
    } catch (const platform::JsonReadError& error) {
        throw PatchError("INVALID_PATCH", error.what());
    }
}

const Json::Object& as_object(const Json& value, std::string_view context) {
    if (const auto* object = std::get_if<Json::Object>(&value.data)) return *object;
    throw PatchError("INVALID_PATCH", std::string(context) + " must be a JSON object.");
}

const Json::Array& as_array(const Json& value, std::string_view context) {
    if (const auto* array = std::get_if<Json::Array>(&value.data)) return *array;
    throw PatchError("INVALID_PATCH", std::string(context) + " must be a JSON array.");
}

const std::string& as_string(const Json& value, std::string_view context) {
    if (const auto* string = std::get_if<std::string>(&value.data)) return *string;
    throw PatchError("INVALID_PATCH", std::string(context) + " must be a string.");
}

std::int64_t as_integer(const Json& value, std::string_view context) {
    if (const auto* integer = std::get_if<std::int64_t>(&value.data)) return *integer;
    throw PatchError("INVALID_PATCH", std::string(context) + " must be an integer.");
}

const Json& require(const Json::Object& object, std::string_view key) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) throw PatchError("INVALID_PATCH", "Patch is missing required field '" + std::string(key) + "'.");
    return it->second;
}

void require_exact_keys(const Json::Object& object, const std::vector<std::string>& keys, std::string_view context) {
    for (const auto& key : keys) {
        if (!object.contains(key)) {
            throw PatchError(
                "INVALID_PATCH", std::string(context) + " is missing field '" + key + "'.");
        }
    }
    for (const auto& [key, _] : object) {
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            throw PatchError(
                "INVALID_PATCH", std::string(context) + " contains unexpected field '" + key + "'.");
        }
    }
}

enum class OperationKind {
    ReplaceNode,
    InsertBefore,
    InsertAfter,
    DeleteNode,
};

struct Operation {
    OperationKind kind{OperationKind::ReplaceNode};
    std::string node_id;
    std::string expected_hash;
    std::optional<std::string> expected_kind;
    std::string replacement;
    std::optional<std::string> replacement_kind;
    bool structured{};
};

struct Patch {
    std::int64_t schema_version{};
    std::string base_revision;
    std::vector<Operation> operations;
};

OperationKind operation_kind(std::string_view name) {
    if (name == "replace_node") return OperationKind::ReplaceNode;
    if (name == "insert_before") return OperationKind::InsertBefore;
    if (name == "insert_after") return OperationKind::InsertAfter;
    if (name == "delete_node") return OperationKind::DeleteNode;
    throw PatchError("INVALID_PATCH", "Unsupported patch operation '" + std::string(name) + "'.");
}

Patch parse_patch(std::string_view text) {
    const auto root = parse_patch_json(text);
    const auto& object = as_object(root, "Patch");
    require_exact_keys(object, {"schema_version", "base_revision", "operations"}, "Patch");

    Patch patch;
    patch.schema_version = as_integer(require(object, "schema_version"), "schema_version");
    if (patch.schema_version != 1 && patch.schema_version != 2 && patch.schema_version != 3) {
        throw PatchError("INVALID_PATCH", "Patch schema_version must be 1, 2, or 3.");
    }
    patch.base_revision = as_string(require(object, "base_revision"), "base_revision");

    const auto& operations = as_array(require(object, "operations"), "operations");
    if (operations.empty()) throw PatchError("INVALID_PATCH", "Patch operations must be nonempty.");

    for (const auto& value : operations) {
        const auto& op = as_object(value, "Patch operation");
        const auto& op_name = as_string(require(op, "op"), "op");

        if (patch.schema_version == 1) {
            require_exact_keys(op, {"op", "node_id", "expected_hash", "replacement"}, "Patch operation");
            if (op_name != "replace_node") {
                throw PatchError("INVALID_PATCH", "Patch schema version 1 supports only replace_node.");
            }
            patch.operations.push_back(Operation{
                OperationKind::ReplaceNode,
                as_string(require(op, "node_id"), "node_id"),
                as_string(require(op, "expected_hash"), "expected_hash"),
                std::nullopt,
                as_string(require(op, "replacement"), "replacement"),
                std::nullopt,
                false});
            continue;
        }

        const auto kind = operation_kind(op_name);
        const auto& expected_kind = as_string(require(op, "expected_kind"), "expected_kind");
        if (expected_kind.empty()) {
            throw PatchError("INVALID_PATCH", "expected_kind must be nonempty in patch schema version 2 or 3.");
        }
        if (kind == OperationKind::DeleteNode) {
            require_exact_keys(
                op, {"op", "node_id", "expected_hash", "expected_kind"}, "Patch operation");
            patch.operations.push_back(Operation{
                kind,
                as_string(require(op, "node_id"), "node_id"),
                as_string(require(op, "expected_hash"), "expected_hash"),
                expected_kind, {}, std::nullopt, false});
            continue;
        }
        if (patch.schema_version == 2) {
            require_exact_keys(
                op, {"op", "node_id", "expected_hash", "expected_kind", "replacement"},
                "Patch operation");
            patch.operations.push_back(Operation{
                kind,
                as_string(require(op, "node_id"), "node_id"),
                as_string(require(op, "expected_hash"), "expected_hash"),
                expected_kind,
                as_string(require(op, "replacement"), "replacement"),
                std::nullopt, false});
            continue;
        }

        const bool has_source = op.contains("replacement");
        const bool has_node = op.contains("replacement_node");
        if (has_source == has_node) {
            throw PatchError("INVALID_PATCH",
                             "Patch schema version 3 edit requires exactly one of replacement or replacement_node.");
        }
        if (has_source) {
            require_exact_keys(
                op, {"op", "node_id", "expected_hash", "expected_kind", "replacement"},
                "Patch operation");
            patch.operations.push_back(Operation{
                kind,
                as_string(require(op, "node_id"), "node_id"),
                as_string(require(op, "expected_hash"), "expected_hash"),
                expected_kind,
                as_string(require(op, "replacement"), "replacement"),
                std::nullopt, false});
        } else {
            require_exact_keys(
                op, {"op", "node_id", "expected_hash", "expected_kind", "replacement_node"},
                "Patch operation");
            const auto& node = as_object(require(op, "replacement_node"), "replacement_node");
            require_exact_keys(node, {"kind", "source"}, "replacement_node");
            const auto& replacement_kind = as_string(require(node, "kind"), "replacement_node.kind");
            if (replacement_kind.empty())
                throw PatchError("INVALID_PATCH", "replacement_node.kind must be nonempty.");
            patch.operations.push_back(Operation{
                kind,
                as_string(require(op, "node_id"), "node_id"),
                as_string(require(op, "expected_hash"), "expected_hash"),
                expected_kind,
                as_string(require(node, "source"), "replacement_node.source"),
                replacement_kind, true});
        }
    }
    return patch;
}

struct Edit {
    std::size_t start{};
    std::size_t end{};
    std::string replacement;
    SourceSpan span{};
    std::string node_id;
};

bool contains_line_comment(std::string_view source) {
    bool in_string = false;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char c = source[i];
        if (c == '"') {
            in_string = !in_string;
            continue;
        }
        if (!in_string && c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            return true;
        }
    }
    return false;
}

bool edits_conflict(const Edit& left, const Edit& right) {
    const bool left_insert = left.start == left.end;
    const bool right_insert = right.start == right.end;
    if (left_insert && right_insert) return left.start == right.start;
    if (left_insert) return left.start >= right.start && left.start <= right.end;
    if (right_insert) return right.start >= left.start && right.start <= left.end;
    return left.start < right.end && right.start < left.end;
}

} // namespace

std::string_view patch_schema_json() noexcept {
    return kPatchSchema;
}

PatchResult apply_source_patch(
    std::string_view source,
    const CheckedProgram& checked,
    std::string_view patch_json,
    std::function<void(std::string_view)> validator) {
    const auto patch = parse_patch(patch_json);
    const auto inspection = inspect_source(source, checked);
    if (patch.base_revision != inspection.revision) {
        throw PatchError("STALE_REVISION", "Patch base revision does not match the source.");
    }

    std::unordered_map<std::string, const SourceNode*> nodes;
    for (const auto& node : inspection.nodes) nodes.emplace(node.node_id, &node);

    std::vector<Edit> edits;
    edits.reserve(patch.operations.size());
    bool has_structured_edit = false;

    for (const auto& operation : patch.operations) {
        has_structured_edit = has_structured_edit || operation.structured;
        const auto it = nodes.find(operation.node_id);
        if (it == nodes.end()) {
            throw PatchError("UNKNOWN_NODE", "Patch references unknown node '" + operation.node_id + "'.");
        }
        const auto& node = *it->second;
        if (operation.expected_hash != node.source_hash) {
            throw PatchError(
                "PATCH_HASH_MISMATCH", "Node source hash does not match expected_hash.",
                node.span, node.node_id);
        }
        if (operation.expected_kind && *operation.expected_kind != node.kind) {
            throw PatchError(
                "PATCH_KIND_MISMATCH",
                "Node kind '" + node.kind + "' does not match expected_kind '" +
                    *operation.expected_kind + "'.",
                node.span, node.node_id);
        }

        if (operation.structured) {
            if (operation.kind == OperationKind::ReplaceNode &&
                operation.replacement_kind && *operation.replacement_kind != node.kind) {
                throw PatchError(
                    "PATCH_ROLE_MISMATCH",
                    "Structured replacement kind '" + *operation.replacement_kind +
                        "' does not match target role '" + node.kind + "'.",
                    node.span, node.node_id);
            }
            const auto original = source.substr(
                node.span.start.offset, node.span.end.offset - node.span.start.offset);
            if (contains_line_comment(original)) {
                throw PatchError(
                    "PATCH_TRIVIA_CONFLICT",
                    "Structured edit would replace a node containing comments; use a source replacement to preserve trivia explicitly.",
                    node.span, node.node_id);
            }
        }

        std::size_t start = node.span.start.offset;
        std::size_t end = node.span.end.offset;
        std::string replacement = operation.replacement;
        switch (operation.kind) {
            case OperationKind::ReplaceNode:
                break;
            case OperationKind::InsertBefore:
                end = start;
                break;
            case OperationKind::InsertAfter:
                start = end;
                break;
            case OperationKind::DeleteNode:
                replacement.clear();
                break;
        }
        edits.push_back(Edit{start, end, std::move(replacement), node.span, node.node_id});
    }

    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) {
        return std::pair{a.start, a.end} < std::pair{b.start, b.end};
    });
    for (std::size_t i = 0; i < edits.size(); ++i) {
        for (std::size_t j = i + 1; j < edits.size(); ++j) {
            if (edits_conflict(edits[i], edits[j])) {
                throw PatchError(
                    "PATCH_OVERLAP", "Patch operations have overlapping or ambiguous edit spans.",
                    edits[j].span, edits[j].node_id);
            }
        }
    }

    std::string updated(source);
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        updated.replace(it->start, it->end - it->start, it->replacement);
    }

    // A public structural-node payload represents structure rather than trivia.
    // Render the complete accepted result through the language's canonical
    // formatter before validation/write so source -> structure -> source has one
    // deterministic representation. Source-fragment-only patches intentionally
    // retain their historical byte-preserving behavior.
    if (has_structured_edit) updated = format_source(updated);

    // A patch is accepted only when the resulting source still passes the full
    // frontend and IR/backend validation path. File-aware callers provide a validator
    // that preserves module path semantics; in-memory callers use the ordinary frontend.
    if (validator) validator(updated);
    else (void)compile(updated);
    return PatchResult{inspection.revision, sha256_hex(updated), std::move(updated)};
}

} // namespace quidra
