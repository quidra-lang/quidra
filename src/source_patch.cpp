#include "quidra/source_patch.hpp"

#include "quidra/compiler.hpp"
#include "quidra/source_tools.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace quidra {
namespace {

struct Json {
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;
    using Data = std::variant<std::nullptr_t, bool, std::int64_t, std::string, Array, Object>;
    Data data;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    Json parse() {
        skip_ws();
        auto value = parse_value();
        skip_ws();
        if (!eof()) fail("Unexpected data after JSON value.");
        return value;
    }

private:
    std::string_view text_;
    std::size_t pos_{};

    bool eof() const { return pos_ >= text_.size(); }
    char peek() const { return eof() ? '\0' : text_[pos_]; }
    char take() { return eof() ? '\0' : text_[pos_++]; }

    [[noreturn]] void fail(const std::string& message) const {
        throw PatchError("INVALID_PATCH", message);
    }

    void skip_ws() {
        while (!eof() && std::isspace(static_cast<unsigned char>(peek()))) ++pos_;
    }

    void expect(char expected) {
        if (take() != expected) fail(std::string("Expected '") + expected + "' in patch JSON.");
    }

    bool consume(std::string_view token) {
        if (text_.substr(pos_, token.size()) != token) return false;
        pos_ += token.size();
        return true;
    }

    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp <= 0x7fU) out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7ffU) {
            out.push_back(static_cast<char>(0xc0U | (cp >> 6U)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
        } else if (cp <= 0xffffU) {
            out.push_back(static_cast<char>(0xe0U | (cp >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3fU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
        } else if (cp <= 0x10ffffU) {
            out.push_back(static_cast<char>(0xf0U | (cp >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3fU)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3fU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
        } else {
            throw PatchError("INVALID_PATCH", "Invalid Unicode code point in patch JSON.");
        }
    }

    std::uint32_t hex4() {
        if (pos_ + 4 > text_.size()) fail("Incomplete Unicode escape in patch JSON.");
        std::uint32_t value{};
        for (unsigned i = 0; i < 4; ++i) {
            const char c = take();
            value <<= 4U;
            if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else fail("Invalid Unicode escape in patch JSON.");
        }
        return value;
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (!eof()) {
            const char c = take();
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20U) fail("Control character in patch JSON string.");
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (eof()) fail("Incomplete escape in patch JSON string.");
            switch (take()) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    auto cp = hex4();
                    if (cp >= 0xd800U && cp <= 0xdbffU) {
                        if (!consume("\\u")) fail("High surrogate must be followed by a low surrogate.");
                        const auto low = hex4();
                        if (low < 0xdc00U || low > 0xdfffU) fail("Invalid low surrogate in patch JSON.");
                        cp = 0x10000U + ((cp - 0xd800U) << 10U) + (low - 0xdc00U);
                    } else if (cp >= 0xdc00U && cp <= 0xdfffU) {
                        fail("Unexpected low surrogate in patch JSON.");
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: fail("Invalid escape in patch JSON string.");
            }
        }
        fail("Unterminated patch JSON string.");
    }

    std::int64_t parse_integer() {
        const auto start = pos_;
        bool negative = false;
        if (peek() == '-') { negative = true; ++pos_; }
        if (eof() || !std::isdigit(static_cast<unsigned char>(peek()))) fail("Invalid number in patch JSON.");
        std::int64_t value{};
        while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) {
            const auto digit = static_cast<unsigned>(take() - '0');
            if (value > (INT64_MAX - digit) / 10) fail("Patch JSON integer is out of range.");
            value = value * 10 + static_cast<std::int64_t>(digit);
        }
        if (!eof() && (peek() == '.' || peek() == 'e' || peek() == 'E')) fail("Patch JSON requires integer schema values.");
        if (negative) value = -value;
        if (pos_ == start) fail("Invalid number in patch JSON.");
        return value;
    }

    Json parse_array() {
        expect('['); skip_ws();
        Json::Array values;
        if (peek() == ']') { take(); return Json{std::move(values)}; }
        while (true) {
            skip_ws(); values.push_back(parse_value()); skip_ws();
            if (peek() == ']') { take(); break; }
            expect(',');
        }
        return Json{std::move(values)};
    }

    Json parse_object() {
        expect('{'); skip_ws();
        Json::Object values;
        if (peek() == '}') { take(); return Json{std::move(values)}; }
        while (true) {
            skip_ws();
            if (peek() != '"') fail("Patch JSON object keys must be strings.");
            auto key = parse_string(); skip_ws(); expect(':'); skip_ws();
            if (!values.emplace(std::move(key), parse_value()).second) fail("Duplicate key in patch JSON object.");
            skip_ws();
            if (peek() == '}') { take(); break; }
            expect(',');
        }
        return Json{std::move(values)};
    }

    Json parse_value() {
        skip_ws();
        if (eof()) fail("Unexpected end of patch JSON.");
        if (peek() == '"') return Json{parse_string()};
        if (peek() == '{') return parse_object();
        if (peek() == '[') return parse_array();
        if (peek() == '-' || std::isdigit(static_cast<unsigned char>(peek()))) return Json{parse_integer()};
        if (consume("true")) return Json{true};
        if (consume("false")) return Json{false};
        if (consume("null")) return Json{nullptr};
        fail("Invalid value in patch JSON.");
    }
};

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

struct Operation {
    std::string node_id;
    std::string expected_hash;
    std::string replacement;
};

struct Patch {
    std::string base_revision;
    std::vector<Operation> operations;
};

Patch parse_patch(std::string_view text) {
    const auto root = JsonParser(text).parse();
    const auto& object = as_object(root, "Patch");
    require_exact_keys(object, {"schema_version", "base_revision", "operations"}, "Patch");
    if (as_integer(require(object, "schema_version"), "schema_version") != 1) {
        throw PatchError("INVALID_PATCH", "Patch schema_version must be 1.");
    }
    Patch patch;
    patch.base_revision = as_string(require(object, "base_revision"), "base_revision");
    const Json& operations_value = require(object, "operations");
    const auto& operations = as_array(operations_value, "operations");
    if (operations.empty()) throw PatchError("INVALID_PATCH", "Patch operations must be nonempty.");
    for (const auto& value : operations) {
        const auto& op = as_object(value, "Patch operation");
        require_exact_keys(op, {"op", "node_id", "expected_hash", "replacement"}, "Patch operation");
        if (as_string(require(op, "op"), "op") != "replace_node") {
            throw PatchError("INVALID_PATCH", "Only replace_node operations are supported.");
        }
        patch.operations.push_back(Operation{
            as_string(require(op, "node_id"), "node_id"),
            as_string(require(op, "expected_hash"), "expected_hash"),
            as_string(require(op, "replacement"), "replacement")});
    }
    return patch;
}

} // namespace

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

    struct Edit {
        std::size_t start{};
        std::size_t end{};
        std::string replacement;
        SourceSpan span{};
        std::string node_id;
    };
    std::vector<Edit> edits;
    edits.reserve(patch.operations.size());

    for (const auto& operation : patch.operations) {
        const auto it = nodes.find(operation.node_id);
        if (it == nodes.end()) {
            throw PatchError("UNKNOWN_NODE", "Patch references unknown node '" + operation.node_id + "'.");
        }
        const auto& node = *it->second;
        if (operation.expected_hash != node.source_hash) {
            throw PatchError("PATCH_HASH_MISMATCH", "Node source hash does not match expected_hash.", node.span, node.node_id);
        }
        edits.push_back(Edit{node.span.start.offset, node.span.end.offset, operation.replacement, node.span, node.node_id});
    }

    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) {
        return std::pair{a.start, a.end} < std::pair{b.start, b.end};
    });
    for (std::size_t i = 1; i < edits.size(); ++i) {
        if (edits[i].start < edits[i - 1].end) {
            throw PatchError("PATCH_OVERLAP", "Patch operations have overlapping node spans.", edits[i].span, edits[i].node_id);
        }
    }

    std::string updated(source);
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        updated.replace(it->start, it->end - it->start, it->replacement);
    }

    // A patch is accepted only when the resulting source still passes the full
    // frontend and IR/backend validation path. File-aware callers provide a validator
    // that preserves module path semantics; in-memory callers use the ordinary frontend.
    if (validator) validator(updated);
    else (void)compile(updated);
    return PatchResult{inspection.revision, sha256_hex(updated), std::move(updated)};
}

} // namespace quidra
