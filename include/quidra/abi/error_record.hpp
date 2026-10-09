#pragma once

// Planned structured error-value ABI layout and validation helpers.
// This header does NOT yet change the compiler/runtime representation of
// T | error: the current runtime still carries error text as a pointer.
// The constructor, propagation, reporting and ownership paths must be
// migrated together before ErrorRecord can become the active ABI.
// Keep this header independent of the runtime allocator and LLVM backend.
#include "quidra/abi/runtime_failure.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <type_traits>

namespace quidra::abi {

struct ErrorHopList;

// The detail slots immediately follow this header (eight bytes per slot,
// in the order specified by the code's schema). No string allocations or
// side tables are needed to read an error's message.
struct ErrorRecord {
    const char* message;
    const char* code; // null means the user code ERROR
    std::uint32_t schema; // 0: user/package code, otherwise ErrorValueCode
    std::uint32_t column; // origin column, one-based bytes
    const ErrorRecord* cause;
    std::uint64_t line; // (file index << 32) | line, zero if no exact source
    const void* statement; // retained statement location for package errors
    ErrorHopList* hops;
};

static_assert(std::is_standard_layout_v<ErrorRecord>);
static_assert(sizeof(void*) == 8, "Quidra's error-record ABI requires 64-bit pointers");
static_assert(sizeof(ErrorRecord) == 56);
static_assert(alignof(ErrorRecord) == 8);
static_assert(offsetof(ErrorRecord, message) == 0);
static_assert(offsetof(ErrorRecord, code) == 8);
static_assert(offsetof(ErrorRecord, schema) == 16);
static_assert(offsetof(ErrorRecord, column) == 20);
static_assert(offsetof(ErrorRecord, cause) == 24);
static_assert(offsetof(ErrorRecord, line) == 32);
static_assert(offsetof(ErrorRecord, statement) == 40);
static_assert(offsetof(ErrorRecord, hops) == 48);

struct ErrorDetailSlot { std::uint64_t bits; };
static_assert(sizeof(ErrorDetailSlot) == 8);
static_assert(alignof(ErrorDetailSlot) == 8);

enum class ErrorDetailKind : std::uint8_t { text, name, integer };
struct ErrorDetailSchema {
    std::string_view name;
    ErrorDetailKind kind;
    bool identity;
    bool optional;
};

// Schema 0 is reserved for a user or package code, whose sole identity
// detail is its message text. Core schema IDs are stable ABI identifiers.
enum class ErrorValueCode : std::uint32_t {
    file_read = 1, file_write, file_open, file_seek, file_status,
    file_list, file_remove, file_copy, file_move, file_mkdir,
    numeric_parse, bin_parse, utf8_decode, input_end, input_read,
    input_format, output_write, numeric_conversion, json_parse,
    json_value, http_request
};

struct ErrorValueCodeInfo {
    ErrorValueCode code;
    std::string_view spelling;
    std::span<const ErrorDetailSchema> details;
};

inline constexpr ErrorDetailSchema detail_path{"path", ErrorDetailKind::text, true, true};
inline constexpr ErrorDetailSchema detail_reason{"reason", ErrorDetailKind::name, true, false};
inline constexpr std::array<ErrorDetailSchema, 2> path_and_reason{detail_path, detail_reason};
inline constexpr std::array<ErrorDetailSchema, 3> open_details{
    ErrorDetailSchema{"path", ErrorDetailKind::text, true, false},
    ErrorDetailSchema{"mode", ErrorDetailKind::name, true, false}, detail_reason};
inline constexpr std::array<ErrorDetailSchema, 3> transfer_details{
    ErrorDetailSchema{"path", ErrorDetailKind::text, true, false},
    ErrorDetailSchema{"destination", ErrorDetailKind::text, true, false}, detail_reason};
inline constexpr std::array<ErrorDetailSchema, 2> parse_details{
    ErrorDetailSchema{"type", ErrorDetailKind::name, true, false}, detail_reason};
inline constexpr std::array<ErrorDetailSchema, 4> input_format_details{
    detail_reason,
    ErrorDetailSchema{"literal", ErrorDetailKind::text, true, true},
    ErrorDetailSchema{"type", ErrorDetailKind::name, true, true},
    ErrorDetailSchema{"input", ErrorDetailKind::text, true, true}};
inline constexpr std::array<ErrorDetailSchema, 3> conversion_details{
    ErrorDetailSchema{"type", ErrorDetailKind::name, true, false},
    ErrorDetailSchema{"subject", ErrorDetailKind::name, true, false}, detail_reason};
inline constexpr std::array<ErrorDetailSchema, 2> json_parse_details{
    ErrorDetailSchema{"problem", ErrorDetailKind::text, true, false},
    ErrorDetailSchema{"offset", ErrorDetailKind::integer, true, false}};
inline constexpr std::array<ErrorDetailSchema, 1> problem_details{
    ErrorDetailSchema{"problem", ErrorDetailKind::text, true, false}};
inline constexpr std::array<ErrorDetailSchema, 1> reason_details{detail_reason};
inline constexpr std::array<ErrorDetailSchema, 0> no_details{};

// The table is ordered by the ABI schema ID. Adding a code appends a row;
// renumbering an existing row would misinterpret stored error records.
inline constexpr std::array error_value_codes{
    ErrorValueCodeInfo{ErrorValueCode::file_read, "FILE_READ", path_and_reason},
    ErrorValueCodeInfo{ErrorValueCode::file_write, "FILE_WRITE", path_and_reason},
    ErrorValueCodeInfo{ErrorValueCode::file_open, "FILE_OPEN", open_details},
    ErrorValueCodeInfo{ErrorValueCode::file_seek, "FILE_SEEK", path_and_reason},
    ErrorValueCodeInfo{ErrorValueCode::file_status, "FILE_STATUS", path_and_reason},
    ErrorValueCodeInfo{ErrorValueCode::file_list, "FILE_LIST", path_and_reason},
    ErrorValueCodeInfo{ErrorValueCode::file_remove, "FILE_REMOVE", path_and_reason},
    ErrorValueCodeInfo{ErrorValueCode::file_copy, "FILE_COPY", transfer_details},
    ErrorValueCodeInfo{ErrorValueCode::file_move, "FILE_MOVE", transfer_details},
    ErrorValueCodeInfo{ErrorValueCode::file_mkdir, "FILE_MKDIR", path_and_reason},
    ErrorValueCodeInfo{ErrorValueCode::numeric_parse, "NUMERIC_PARSE", parse_details},
    ErrorValueCodeInfo{ErrorValueCode::bin_parse, "BIN_PARSE", no_details},
    ErrorValueCodeInfo{ErrorValueCode::utf8_decode, "UTF8_DECODE", no_details},
    ErrorValueCodeInfo{ErrorValueCode::input_end, "INPUT_END", no_details},
    ErrorValueCodeInfo{ErrorValueCode::input_read, "INPUT_READ", reason_details},
    ErrorValueCodeInfo{ErrorValueCode::input_format, "INPUT_FORMAT", input_format_details},
    ErrorValueCodeInfo{ErrorValueCode::output_write, "OUTPUT_WRITE", no_details},
    ErrorValueCodeInfo{ErrorValueCode::numeric_conversion, "NUMERIC_CONVERSION", conversion_details},
    ErrorValueCodeInfo{ErrorValueCode::json_parse, "JSON_PARSE", json_parse_details},
    ErrorValueCodeInfo{ErrorValueCode::json_value, "JSON_VALUE", no_details},
    ErrorValueCodeInfo{ErrorValueCode::http_request, "HTTP_REQUEST", problem_details}
};

constexpr const ErrorValueCodeInfo* error_value_schema(std::uint32_t id) {
    if (id == 0 || id > error_value_codes.size()) return nullptr;
    const auto& value = error_value_codes[id - 1];
    return static_cast<std::uint32_t>(value.code) == id ? &value : nullptr;
}

constexpr bool valid_user_error_code(std::string_view spelling) {
    if (spelling.empty() || spelling.front() < 'A' || spelling.front() > 'Z')
        return false;
    bool after_underscore = false;
    for (const char c : spelling) {
        if (c == '_') {
            if (after_underscore) return false;
            after_underscore = true;
        } else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            after_underscore = false;
        } else {
            return false;
        }
    }
    return !after_underscore;
}

constexpr bool is_core_error_code(std::string_view spelling) {
    if (spelling == "ERROR") return true;
    for (const auto& value : error_value_codes)
        if (value.spelling == spelling) return true;
    for (const auto& value : runtime_failure_codes)
        if (value.spelling == spelling) return true;
    return false;
}

constexpr bool valid_custom_error_code(std::string_view spelling,
                                       std::string_view required_prefix = {}) {
    if (!valid_user_error_code(spelling) || is_core_error_code(spelling)) return false;
    if (required_prefix.empty()) return true;
    return spelling.size() > required_prefix.size() &&
           spelling.substr(0, required_prefix.size()) == required_prefix &&
           spelling[required_prefix.size()] == '_';
}

static_assert(error_value_codes.size() ==
              static_cast<std::uint32_t>(ErrorValueCode::http_request));
static_assert(valid_custom_error_code("DATASET_LOAD"));
static_assert(valid_custom_error_code("NN_CHECKPOINT", "NN"));
static_assert(!valid_custom_error_code("NN__BAD"));
static_assert(!valid_custom_error_code("ERROR"));
static_assert(!valid_custom_error_code("FILE_READ"));
static_assert(!valid_custom_error_code("OTHER_SHAPE", "NN"));

// Materialized details, in schema order. An absent text/name detail is
// different from a present but empty one. Integer details cannot be absent.
// These are plain values: the runtime may pass managed strings to the renderer
// without transferring ownership to this ABI helper.
struct ErrorDetailValue {
    bool present{};
    ErrorDetailKind kind{ErrorDetailKind::text};
    std::string_view text{};
    std::int64_t integer{};

    static constexpr ErrorDetailValue text_value(std::string_view value) {
        return {true, ErrorDetailKind::text, value, 0};
    }
    static constexpr ErrorDetailValue name_value(std::string_view value) {
        return {true, ErrorDetailKind::name, value, 0};
    }
    static constexpr ErrorDetailValue integer_value(std::int64_t value) {
        return {true, ErrorDetailKind::integer, {}, value};
    }
    static constexpr ErrorDetailValue absent(ErrorDetailKind kind) {
        return {false, kind, {}, 0};
    }
};

constexpr bool valid_error_details(const ErrorValueCodeInfo& schema,
                                   std::span<const ErrorDetailValue> values) {
    if (values.size() != schema.details.size()) return false;
    for (std::size_t i = 0; i != values.size(); ++i) {
        const auto& value = values[i];
        const auto& detail = schema.details[i];
        if (value.kind != detail.kind || (!value.present && !detail.optional))
            return false;
    }
    return true;
}

// A user-defined error has the text as its sole identity detail. The source
// location, propagation hops, and display path deliberately do not appear in
// this view, so they cannot accidentally affect equality.
struct ErrorValueIdentity {
    std::string_view code;
    std::span<const ErrorDetailValue> details;
    const ErrorValueIdentity* cause{};
};

inline bool same_error_identity(const ErrorValueIdentity& first,
                                const ErrorValueIdentity& second) {
    const ErrorValueIdentity* left = &first;
    const ErrorValueIdentity* right = &second;
    while (left != nullptr && right != nullptr) {
        if (left->code != right->code || left->details.size() != right->details.size())
            return false;
        for (std::size_t i = 0; i != left->details.size(); ++i) {
            const auto& a = left->details[i];
            const auto& b = right->details[i];
            if (a.present != b.present || a.kind != b.kind) return false;
            if (!a.present) continue;
            if (a.kind == ErrorDetailKind::integer) {
                if (a.integer != b.integer) return false;
            } else if (a.text != b.text) {
                return false;
            }
        }
        left = left->cause;
        right = right->cause;
    }
    return left == nullptr && right == nullptr;
}

// One renderer serves the backend's constant records and the runtime's
// managed records. Return false without modifying output for invalid slots.
inline bool render_error(const ErrorValueCodeInfo& schema,
                         std::span<const ErrorDetailValue> details,
                         std::string& output) {
    if (!valid_error_details(schema, details)) return false;
    const auto value = [&](std::string_view name) -> std::string_view {
        for (std::size_t i = 0; i != schema.details.size(); ++i)
            if (schema.details[i].name == name && details[i].present)
                return details[i].text;
        return {};
    };
    const auto number = [&](std::string_view name) -> std::int64_t {
        for (std::size_t i = 0; i != schema.details.size(); ++i)
            if (schema.details[i].name == name && details[i].present)
                return details[i].integer;
        return 0;
    };
    const auto words = [](std::string_view name) {
        std::string result;
        for (const char c : name) {
            if (c == '_') result += ' ';
            else if (c >= 'A' && c <= 'Z') result += static_cast<char>(c - 'A' + 'a');
            else result += c;
        }
        return result;
    };
    std::string result;
    const auto file_failure = [&](std::string_view verb) {
        result = "cannot ";
        result += verb;
        if (value("path").empty()) result += " the file handle";
        else {
            result += " \"";
            result += value("path");
            result += '"';
        }
        result += ": ";
        result += words(value("reason"));
    };

    switch (schema.code) {
        case ErrorValueCode::file_read: file_failure("read"); break;
        case ErrorValueCode::file_write: file_failure("write"); break;
        case ErrorValueCode::file_seek: file_failure("seek"); break;
        case ErrorValueCode::file_status: file_failure("inspect"); break;
        case ErrorValueCode::file_list: file_failure("list"); break;
        case ErrorValueCode::file_remove: file_failure("remove"); break;
        case ErrorValueCode::file_mkdir: file_failure("create directory"); break;
        case ErrorValueCode::file_open: {
            std::string_view verb = "open";
            if (value("mode") == "CREATE") verb = "create";
            else if (value("mode") == "APPEND") verb = "open for appending";
            file_failure(verb);
            break;
        }
        case ErrorValueCode::file_copy:
        case ErrorValueCode::file_move:
            result = schema.code == ErrorValueCode::file_copy ? "cannot copy \"" : "cannot move \"";
            result += value("path");
            result += "\" to \"";
            result += value("destination");
            result += "\": ";
            result += words(value("reason"));
            break;
        case ErrorValueCode::numeric_parse:
            result = "cannot parse numeric input as ";
            result += value("type");
            if (value("reason") == "OUT_OF_RANGE") result += ": value out of range";
            break;
        case ErrorValueCode::bin_parse: result = "cannot parse bin input"; break;
        case ErrorValueCode::utf8_decode: result = "invalid UTF-8 text"; break;
        case ErrorValueCode::input_end: result = "scan reached the end of input"; break;
        case ErrorValueCode::input_read:
            result = "input failed: ";
            result += words(value("reason"));
            break;
        case ErrorValueCode::input_format:
            if (value("reason") == "MISSING_TEXT") {
                result = "scan expected \"";
                result += value("literal");
                result += "\" in the input";
            } else if (value("reason") == "UNREADABLE_FIELD") {
                result = "scan could not read ";
                result += value("type");
                result += " from \"";
                result += value("input");
                result += '"';
            } else {
                result = "scan found unexpected input after the format: \"";
                result += value("input");
                result += '"';
            }
            break;
        case ErrorValueCode::output_write: result = "output failed"; break;
        case ErrorValueCode::numeric_conversion:
            result = "numeric conversion ";
            result += words(value("reason"));
            result += ": ";
            result += words(value("subject"));
            result += " cannot be represented as ";
            result += value("type");
            break;
        case ErrorValueCode::json_parse:
            result = value("problem");
            result += " at byte ";
            result += std::to_string(number("offset"));
            break;
        case ErrorValueCode::json_value:
            result = "JSON value has incompatible kind";
            break;
        case ErrorValueCode::http_request:
            result = value("problem");
            break;
    }
    output = std::move(result);
    return true;
}

} // namespace quidra::abi
