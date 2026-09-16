#include "runtime_internal.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" char* quidra_runtime_copy_text(
    const char* data, unsigned long long raw_size);

namespace {

[[noreturn]] void runtime_allocation_failure() {
    std::fprintf(stderr, "Quidra runtime error: allocation failed\n");
    std::exit(101);
}

char* runtime_copy_string(const std::string& value) {
    return quidra_runtime_copy_text(
        value.data(), static_cast<unsigned long long>(value.size()));
}

} // namespace

namespace {
enum class JsonKind { Null, Bool, Number, String, Array, Object };

struct JsonNode {
    JsonKind kind{JsonKind::Null};
    bool boolean{};
    double number{};
    std::string number_text;
    std::string text;
    std::vector<JsonNode*> array;
    std::vector<std::pair<std::string, JsonNode*>> object;
};

struct JsonDocument {
    std::vector<std::unique_ptr<JsonNode>> nodes;
    JsonNode* root{};

    JsonNode* make_node() {
        nodes.push_back(std::make_unique<JsonNode>());
        return nodes.back().get();
    }
};

struct JsonHandle {
    std::shared_ptr<JsonDocument> document;
    JsonNode* node{};
};

thread_local std::string json_last_error;

void append_utf8(std::string& out, unsigned codepoint) {
    if (codepoint <= 0x7fU) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        out.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        out.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        out.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
}

class JsonParser {
public:
    explicit JsonParser(const char* source)
        : start_(source), cursor_(source), document_(std::make_shared<JsonDocument>()) {}

    std::shared_ptr<JsonDocument> parse() {
        if (!cursor_) {
            fail("null JSON source");
            return {};
        }
        skip_space();
        auto* value = parse_value(0);
        if (!value) return {};
        skip_space();
        if (*cursor_ != '\0') {
            fail("unexpected trailing JSON content");
            return {};
        }
        document_->root = value;
        return document_;
    }

    const std::string& error() const { return error_; }

private:
    const char* start_{};
    const char* cursor_{};
    std::shared_ptr<JsonDocument> document_;
    std::string error_;

    JsonNode* make_node() { return document_->make_node(); }

    JsonNode* fail(const std::string& message) {
        if (error_.empty()) {
            const auto offset = static_cast<unsigned long long>(cursor_ - start_);
            error_ = message + " at byte " + std::to_string(offset);
        }
        return nullptr;
    }

    void skip_space() {
        while (*cursor_ == ' ' || *cursor_ == '\t' || *cursor_ == '\n' || *cursor_ == '\r') ++cursor_;
    }

    bool consume(char c) {
        if (*cursor_ != c) return false;
        ++cursor_;
        return true;
    }

    int hex_digit(char c) const {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return -1;
    }

    bool unicode_escape(unsigned& value) {
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = hex_digit(*cursor_);
            if (digit < 0) return false;
            value = (value << 4U) | static_cast<unsigned>(digit);
            ++cursor_;
        }
        return true;
    }

    bool parse_string_value(std::string& out) {
        if (!consume('"')) return false;
        while (*cursor_ && *cursor_ != '"') {
            const unsigned char c = static_cast<unsigned char>(*cursor_++);
            if (c < 0x20U) {
                fail("control character in JSON string");
                return false;
            }
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            const char escape = *cursor_++;
            switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned first = 0;
                    if (!unicode_escape(first)) {
                        fail("invalid JSON unicode escape");
                        return false;
                    }
                    unsigned codepoint = first;
                    if (first >= 0xd800U && first <= 0xdbffU) {
                        if (cursor_[0] != '\\' || cursor_[1] != 'u') {
                            fail("missing low surrogate in JSON string");
                            return false;
                        }
                        cursor_ += 2;
                        unsigned second = 0;
                        if (!unicode_escape(second) || second < 0xdc00U || second > 0xdfffU) {
                            fail("invalid low surrogate in JSON string");
                            return false;
                        }
                        codepoint = 0x10000U + ((first - 0xd800U) << 10U) + (second - 0xdc00U);
                    } else if (first >= 0xdc00U && first <= 0xdfffU) {
                        fail("unexpected low surrogate in JSON string");
                        return false;
                    }
                    append_utf8(out, codepoint);
                    break;
                }
                default:
                    fail("invalid JSON string escape");
                    return false;
            }
        }
        if (!consume('"')) {
            fail("unterminated JSON string");
            return false;
        }
        return true;
    }

    JsonNode* parse_value(int depth) {
        if (depth > 256) return fail("JSON nesting exceeds 256 levels");
        skip_space();
        if (*cursor_ == '"') {
            auto* node = make_node();
            node->kind = JsonKind::String;
            if (!parse_string_value(node->text)) return nullptr;
            return node;
        }
        if (*cursor_ == '{') return parse_object(depth + 1);
        if (*cursor_ == '[') return parse_array(depth + 1);
        if (std::strncmp(cursor_, "true", 4) == 0) {
            cursor_ += 4;
            auto* node = make_node();
            node->kind = JsonKind::Bool;
            node->boolean = true;
            return node;
        }
        if (std::strncmp(cursor_, "false", 5) == 0) {
            cursor_ += 5;
            auto* node = make_node();
            node->kind = JsonKind::Bool;
            node->boolean = false;
            return node;
        }
        if (std::strncmp(cursor_, "null", 4) == 0) {
            cursor_ += 4;
            return make_node();
        }
        if (*cursor_ == '-' || (*cursor_ >= '0' && *cursor_ <= '9')) return parse_number();
        return fail("expected JSON value");
    }

    JsonNode* parse_number() {
        const char* begin = cursor_;
        if (*cursor_ == '-') ++cursor_;
        if (*cursor_ == '0') {
            ++cursor_;
            if (*cursor_ >= '0' && *cursor_ <= '9') return fail("leading zero in JSON number");
        } else {
            if (*cursor_ < '1' || *cursor_ > '9') return fail("invalid JSON number");
            while (*cursor_ >= '0' && *cursor_ <= '9') ++cursor_;
        }
        if (*cursor_ == '.') {
            ++cursor_;
            if (*cursor_ < '0' || *cursor_ > '9') return fail("missing JSON fractional digits");
            while (*cursor_ >= '0' && *cursor_ <= '9') ++cursor_;
        }
        if (*cursor_ == 'e' || *cursor_ == 'E') {
            ++cursor_;
            if (*cursor_ == '+' || *cursor_ == '-') ++cursor_;
            if (*cursor_ < '0' || *cursor_ > '9') return fail("missing JSON exponent digits");
            while (*cursor_ >= '0' && *cursor_ <= '9') ++cursor_;
        }

        std::string token(begin, static_cast<std::size_t>(cursor_ - begin));
        errno = 0;
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (errno == ERANGE || !end || *end != '\0' || !std::isfinite(value)) {
            return fail("JSON number is outside float range");
        }
        auto* node = make_node();
        node->kind = JsonKind::Number;
        node->number = value;
        node->number_text = std::move(token);
        return node;
    }

    JsonNode* parse_array(int depth) {
        consume('[');
        auto* node = make_node();
        node->kind = JsonKind::Array;
        skip_space();
        if (consume(']')) return node;
        while (true) {
            auto* item = parse_value(depth);
            if (!item) return nullptr;
            node->array.push_back(item);
            skip_space();
            if (consume(']')) return node;
            if (!consume(',')) return fail("expected ',' or ']' in JSON array");
            skip_space();
        }
    }

    JsonNode* parse_object(int depth) {
        consume('{');
        auto* node = make_node();
        node->kind = JsonKind::Object;
        std::unordered_set<std::string> keys;
        skip_space();
        if (consume('}')) return node;
        while (true) {
            if (*cursor_ != '"') return fail("expected JSON object key");
            std::string key;
            if (!parse_string_value(key)) return nullptr;
            if (!keys.insert(key).second) return fail("duplicate JSON object key");
            skip_space();
            if (!consume(':')) return fail("expected ':' after JSON object key");
            auto* value = parse_value(depth);
            if (!value) return nullptr;
            node->object.emplace_back(std::move(key), value);
            skip_space();
            if (consume('}')) return node;
            if (!consume(',')) return fail("expected ',' or '}' in JSON object");
            skip_space();
        }
    }
};

JsonHandle* json_handle(void* wrapper) {
    if (!wrapper) return nullptr;
    std::uintptr_t bits = 0;
    std::memcpy(&bits, wrapper, sizeof(bits));
    return reinterpret_cast<JsonHandle*>(bits);
}

JsonNode* json_node(void* wrapper) {
    auto* handle = json_handle(wrapper);
    return handle ? handle->node : nullptr;
}

void* json_wrapper(const std::shared_ptr<JsonDocument>& document, JsonNode* node) {
    if (!document || !node) return nullptr;
    JsonHandle* handle = nullptr;
    try {
        handle = new JsonHandle{document, node};
    } catch (...) {
        runtime_allocation_failure();
    }
    auto* wrapper = quidra_managed_alloc(sizeof(std::uintptr_t));
    const auto bits = reinterpret_cast<std::uintptr_t>(handle);
    std::memcpy(wrapper, &bits, sizeof(bits));
    return wrapper;
}

void json_escape_string(const std::string& value, std::string& out) {
    out.push_back('"');
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20U) {
                    out += "\\u00";
                    out.push_back(hex[c >> 4U]);
                    out.push_back(hex[c & 0x0fU]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void json_encode_node(const JsonNode* node, std::string& out) {
    switch (node->kind) {
        case JsonKind::Null: out += "null"; return;
        case JsonKind::Bool: out += node->boolean ? "true" : "false"; return;
        case JsonKind::Number: out += node->number_text; return;
        case JsonKind::String: json_escape_string(node->text, out); return;
        case JsonKind::Array:
            out.push_back('[');
            for (std::size_t i = 0; i < node->array.size(); ++i) {
                if (i) out.push_back(',');
                json_encode_node(node->array[i], out);
            }
            out.push_back(']');
            return;
        case JsonKind::Object:
            out.push_back('{');
            for (std::size_t i = 0; i < node->object.size(); ++i) {
                if (i) out.push_back(',');
                json_escape_string(node->object[i].first, out);
                out.push_back(':');
                json_encode_node(node->object[i].second, out);
            }
            out.push_back('}');
            return;
    }
}

bool json_equal_node(const JsonNode* left, const JsonNode* right) {
    if (left->kind != right->kind) return false;
    switch (left->kind) {
        case JsonKind::Null: return true;
        case JsonKind::Bool: return left->boolean == right->boolean;
        case JsonKind::Number: return left->number == right->number;
        case JsonKind::String: return left->text == right->text;
        case JsonKind::Array:
            if (left->array.size() != right->array.size()) return false;
            for (std::size_t i = 0; i < left->array.size(); ++i) {
                if (!json_equal_node(left->array[i], right->array[i])) return false;
            }
            return true;
        case JsonKind::Object:
            if (left->object.size() != right->object.size()) return false;
            for (const auto& [key, value] : left->object) {
                const auto found = std::find_if(
                    right->object.begin(), right->object.end(),
                    [&](const auto& item) { return item.first == key; });
                if (found == right->object.end() || !json_equal_node(value, found->second)) return false;
            }
            return true;
    }
    return false;
}
}

extern "C" void* quidra_json_parse_raw(const char* text) {
    json_last_error.clear();
    try {
        JsonParser parser(text);
        auto document = parser.parse();
        if (!document) {
            json_last_error = parser.error().empty() ? "invalid JSON" : parser.error();
            return nullptr;
        }
        return json_wrapper(document, document->root);
    } catch (const std::bad_alloc&) {
        runtime_allocation_failure();
    }
}

extern "C" void quidra_json_drop(void* value) {
    auto* handle = json_handle(value);
    if (!handle) return;
    std::uintptr_t zero = 0;
    std::memcpy(value, &zero, sizeof(zero));
    delete handle;
}

extern "C" char* quidra_json_last_error_copy() {
    return runtime_copy_string(json_last_error.empty() ? "invalid JSON" : json_last_error);
}

extern "C" const char* quidra_json_kind(void* value) {
    const auto* node = json_node(value);
    if (!node) return "invalid";
    switch (node->kind) {
        case JsonKind::Null: return "null";
        case JsonKind::Bool: return "bool";
        case JsonKind::Number: return "number";
        case JsonKind::String: return "string";
        case JsonKind::Array: return "array";
        case JsonKind::Object: return "object";
    }
    return "invalid";
}

extern "C" long long quidra_json_size(void* value) {
    const auto* node = json_node(value);
    if (!node) return -1;
    if (node->kind == JsonKind::Array) return static_cast<long long>(node->array.size());
    if (node->kind == JsonKind::Object) return static_cast<long long>(node->object.size());
    return -1;
}

extern "C" bool quidra_json_is_object(void* value) {
    const auto* node = json_node(value);
    return node && node->kind == JsonKind::Object;
}

extern "C" bool quidra_json_is_array(void* value) {
    const auto* node = json_node(value);
    return node && node->kind == JsonKind::Array;
}

extern "C" void* quidra_json_get(void* value, const char* key) {
    auto* handle = json_handle(value);
    auto* node = handle ? handle->node : nullptr;
    if (!handle || !node || node->kind != JsonKind::Object || !key) return nullptr;
    for (const auto& item : node->object) {
        if (item.first == key) return json_wrapper(handle->document, item.second);
    }
    return nullptr;
}

extern "C" void* quidra_json_at(void* value, long long index) {
    auto* handle = json_handle(value);
    auto* node = handle ? handle->node : nullptr;
    if (!handle || !node || node->kind != JsonKind::Array || index < 0 ||
        static_cast<std::size_t>(index) >= node->array.size()) return nullptr;
    return json_wrapper(handle->document, node->array[static_cast<std::size_t>(index)]);
}

extern "C" char* quidra_json_text(void* value) {
    auto* node = json_node(value);
    return node && node->kind == JsonKind::String ? runtime_copy_string(node->text) : nullptr;
}

extern "C" bool quidra_json_integer_ok(void* value) {
    auto* node = json_node(value);
    if (!node || node->kind != JsonKind::Number || node->number_text.find_first_of(".eE") != std::string::npos) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    (void)std::strtoll(node->number_text.c_str(), &end, 10);
    return errno != ERANGE && end && *end == '\0';
}

extern "C" long long quidra_json_integer(void* value) {
    auto* node = json_node(value);
    return node ? std::strtoll(node->number_text.c_str(), nullptr, 10) : 0;
}

extern "C" bool quidra_json_number_ok(void* value) {
    auto* node = json_node(value);
    return node && node->kind == JsonKind::Number;
}

extern "C" double quidra_json_number(void* value) {
    auto* node = json_node(value);
    return node ? node->number : 0.0;
}

extern "C" bool quidra_json_boolean_ok(void* value) {
    auto* node = json_node(value);
    return node && node->kind == JsonKind::Bool;
}

extern "C" bool quidra_json_boolean(void* value) {
    auto* node = json_node(value);
    return node && node->boolean;
}

extern "C" char* quidra_json_encode(void* value) {
    auto* node = json_node(value);
    if (!node) return runtime_copy_string("null");
    std::string encoded;
    json_encode_node(node, encoded);
    return runtime_copy_string(encoded);
}

extern "C" bool quidra_json_equal(void* left, void* right) {
    auto* left_node = json_node(left);
    auto* right_node = json_node(right);
    return left_node && right_node && json_equal_node(left_node, right_node);
}



