#include "platform/strict_json.hpp"

#include <cctype>
#include <climits>
#include <cstdint>
#include <utility>

namespace quidra::platform {
namespace {

class StrictJsonReader {
public:
    StrictJsonReader(std::string_view text, const JsonReadOptions& options)
        : text_(text), noun_(options.document), max_depth_(options.max_depth) {
        sentence_noun_ = noun_;
        if (!sentence_noun_.empty()) {
            sentence_noun_[0] = static_cast<char>(
                std::toupper(static_cast<unsigned char>(sentence_noun_[0])));
        }
    }

    JsonValue read() {
        skip_ws();
        auto value = parse_value();
        skip_ws();
        if (!eof()) fail("Unexpected data after JSON value.");
        return value;
    }

private:
    std::string_view text_;
    std::string noun_;
    std::string sentence_noun_;
    std::size_t max_depth_;
    std::size_t pos_{};
    std::size_t depth_{};

    struct DepthScope {
        explicit DepthScope(std::size_t& depth) : depth_(depth) { ++depth_; }
        ~DepthScope() { --depth_; }
        DepthScope(const DepthScope&) = delete;
        DepthScope& operator=(const DepthScope&) = delete;

    private:
        std::size_t& depth_;
    };

    bool eof() const { return pos_ >= text_.size(); }
    char peek() const { return eof() ? '\0' : text_[pos_]; }
    char take() { return eof() ? '\0' : text_[pos_++]; }

    [[noreturn]] void fail(const std::string& message) const { throw JsonReadError(message); }
    // "<what> in <document>."
    [[noreturn]] void fail_in(std::string_view what) const {
        fail(std::string(what) + " in " + noun_ + ".");
    }
    // "<Document> <what>."
    [[noreturn]] void fail_about(std::string_view what) const {
        fail(sentence_noun_ + " " + std::string(what) + ".");
    }

    void skip_ws() {
        while (!eof() && std::isspace(static_cast<unsigned char>(peek()))) ++pos_;
    }

    void expect(char expected) {
        if (take() != expected) fail_in(std::string("Expected '") + expected + "'");
    }

    bool consume(std::string_view token) {
        if (text_.substr(pos_, token.size()) != token) return false;
        pos_ += token.size();
        return true;
    }

    void append_utf8(std::string& out, std::uint32_t cp) const {
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
            fail_in("Invalid Unicode code point");
        }
    }

    std::uint32_t hex4() {
        if (pos_ + 4 > text_.size()) fail_in("Incomplete Unicode escape");
        std::uint32_t value{};
        for (unsigned i = 0; i < 4; ++i) {
            const char c = take();
            value <<= 4U;
            if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else fail_in("Invalid Unicode escape");
        }
        return value;
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (!eof()) {
            const char c = take();
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20U) {
                fail("Control character in " + noun_ + " string.");
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (eof()) fail("Incomplete escape in " + noun_ + " string.");
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
                        if (low < 0xdc00U || low > 0xdfffU) fail_in("Invalid low surrogate");
                        cp = 0x10000U + ((cp - 0xd800U) << 10U) + (low - 0xdc00U);
                    } else if (cp >= 0xdc00U && cp <= 0xdfffU) {
                        fail_in("Unexpected low surrogate");
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: fail("Invalid escape in " + noun_ + " string.");
            }
        }
        fail("Unterminated " + noun_ + " string.");
    }

    std::int64_t parse_integer() {
        const auto start = pos_;
        bool negative = false;
        if (peek() == '-') { negative = true; ++pos_; }
        if (eof() || !std::isdigit(static_cast<unsigned char>(peek()))) fail_in("Invalid number");
        std::int64_t value{};
        while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) {
            const auto digit = static_cast<unsigned>(take() - '0');
            if (value > (INT64_MAX - digit) / 10) fail_about("integer is out of range");
            value = value * 10 + static_cast<std::int64_t>(digit);
        }
        if (!eof() && (peek() == '.' || peek() == 'e' || peek() == 'E')) {
            fail_about("requires integer schema values");
        }
        if (negative) value = -value;
        if (pos_ == start) fail_in("Invalid number");
        return value;
    }

    JsonValue parse_array() {
        expect('['); skip_ws();
        JsonValue::Array values;
        if (peek() == ']') { take(); return JsonValue{std::move(values)}; }
        while (true) {
            skip_ws(); values.push_back(parse_value()); skip_ws();
            if (peek() == ']') { take(); break; }
            expect(',');
        }
        return JsonValue{std::move(values)};
    }

    JsonValue parse_object() {
        expect('{'); skip_ws();
        JsonValue::Object values;
        if (peek() == '}') { take(); return JsonValue{std::move(values)}; }
        while (true) {
            skip_ws();
            if (peek() != '"') fail_about("object keys must be strings");
            auto key = parse_string(); skip_ws(); expect(':'); skip_ws();
            if (!values.emplace(std::move(key), parse_value()).second) {
                fail("Duplicate key in " + noun_ + " object.");
            }
            skip_ws();
            if (peek() == '}') { take(); break; }
            expect(',');
        }
        return JsonValue{std::move(values)};
    }

    JsonValue parse_value() {
        skip_ws();
        if (depth_ >= max_depth_) fail_about("nesting is too deep");
        DepthScope scope(depth_);
        if (eof()) fail("Unexpected end of " + noun_ + ".");
        if (peek() == '"') return JsonValue{parse_string()};
        if (peek() == '{') return parse_object();
        if (peek() == '[') return parse_array();
        if (peek() == '-' || std::isdigit(static_cast<unsigned char>(peek()))) {
            return JsonValue{parse_integer()};
        }
        if (consume("true")) return JsonValue{true};
        if (consume("false")) return JsonValue{false};
        if (consume("null")) return JsonValue{nullptr};
        fail_in("Invalid value");
    }
};

} // namespace

JsonValue read_strict_json(std::string_view text, const JsonReadOptions& options) {
    return StrictJsonReader(text, options).read();
}

} // namespace quidra::platform
