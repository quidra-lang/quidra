// The strict JSON reader: the values it reads, its limits, and the messages
// it gives in the words of the document's caller (source patches read it as
// "patch JSON").

#include "platform/strict_json.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <variant>

using namespace quidra::platform;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::string error_of(const std::string& text, const JsonReadOptions& options) {
    try {
        (void)read_strict_json(text, options);
    } catch (const JsonReadError& error) {
        return error.what();
    }
    return "<no error>";
}

} // namespace

int main() {
    const JsonReadOptions patch{"patch JSON", 64};

    const auto value = read_strict_json(
        " {\"a\": [null, true, false, -12, \"x\\u00e9\\ud83d\\ude00\"], \"b\": {}} ", patch);
    const auto* object = std::get_if<JsonValue::Object>(&value.data);
    check(object && object->size() == 2, "an object with two members");
    if (object && object->contains("a")) {
        const auto& array = std::get<JsonValue::Array>(object->at("a").data);
        check(array.size() == 5, "an array of five values");
        check(std::holds_alternative<std::nullptr_t>(array[0].data), "null");
        check(std::get<bool>(array[1].data) && !std::get<bool>(array[2].data), "booleans");
        check(std::get<std::int64_t>(array[3].data) == -12, "a negative integer");
        check(std::get<std::string>(array[4].data) == "x\xc3\xa9\xf0\x9f\x98\x80",
              "escapes and a surrogate pair decode to UTF-8");
    }
    check(std::get<std::int64_t>(read_strict_json("9223372036854775807").data) == INT64_MAX,
          "the largest integer");

    const auto expect = [&](const std::string& text, const std::string& message) {
        const auto actual = error_of(text, patch);
        check(actual == message, "[" + text + "]: expected \"" + message + "\", got \"" +
                                     actual + "\"");
    };
    expect("", "Unexpected end of patch JSON.");
    expect("[1] x", "Unexpected data after JSON value.");
    expect("[1 2]", "Expected ',' in patch JSON.");
    expect("{\"a\":1,\"a\":2}", "Duplicate key in patch JSON object.");
    expect("{a:1}", "Patch JSON object keys must be strings.");
    expect("1.5", "Patch JSON requires integer schema values.");
    expect("9223372036854775808", "Patch JSON integer is out of range.");
    expect("-", "Invalid number in patch JSON.");
    expect("nul", "Invalid value in patch JSON.");
    expect("\"a", "Unterminated patch JSON string.");
    expect("\"\x01\"", "Control character in patch JSON string.");
    expect("\"\\q\"", "Invalid escape in patch JSON string.");
    expect("\"\\u12g4\"", "Invalid Unicode escape in patch JSON.");
    expect("\"\\u12\"", "Incomplete Unicode escape in patch JSON.");
    expect("\"\\ude00\"", "Unexpected low surrogate in patch JSON.");
    expect("\"\\ud83d\"", "High surrogate must be followed by a low surrogate.");
    expect("\"\\ud83d\\u0041\"", "Invalid low surrogate in patch JSON.");
    expect(std::string(64, '['), "Patch JSON nesting is too deep.");
    check(error_of(std::string(63, '[') + std::string(63, ']'), patch) == "<no error>",
          "63 levels are within the limit");
    check(error_of("[[1]]", {"run cache JSON", 2}) == "Run cache JSON nesting is too deep.",
          "the document's name and the depth are the caller's");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "strict JSON tests passed\n";
    return 0;
}
