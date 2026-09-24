// WebAssembly bridge for the Quidra compiler frontend.
//
// This file is transport, not semantics. Every answer it produces comes from
// the same quidra_core entry points the native CLI uses -- quidra::check,
// quidra::format_source, quidra::ir::lower/dump, quidra::inspect_source_json
// and quidra::apply_source_patch. There is deliberately no second parser,
// checker, formatter or IR here, and no execution of any kind.
//
// The surface is two C functions:
//
//   char* quidra_wasm_invoke(const char* request_json);
//   void  quidra_wasm_free(char* result);
//
// invoke always returns an owned, NUL-terminated JSON envelope, and the caller
// always releases it with quidra_wasm_free. A C++ exception must never reach a
// JavaScript frame, so every operation runs inside a full catch ladder and
// failures are reported as data.

#include "quidra/checker.hpp"
#include "quidra/compiler.hpp"
#include "quidra/diagnostic.hpp"
#include "quidra/formatter.hpp"
#include "quidra/ir.hpp"
#include "quidra/project.hpp"
#include "quidra/source_patch.hpp"
#include "quidra/source_tools.hpp"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef QUIDRA_CORE_COMMIT
#define QUIDRA_CORE_COMMIT "unknown"
#endif

namespace {

// The request/response contract version. It is deliberately independent of the
// product version: the compiler may reach 0.4.0 while this envelope is still 1.
// Bump it only when the shape below changes incompatibly.
constexpr int wasm_schema_version = 1;

// ---------------------------------------------------------------------------
// JSON output
// ---------------------------------------------------------------------------

std::string escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (raw) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (byte < 0x20) {
                static const char* digits = "0123456789abcdef";
                out += "\\u00";
                out += digits[(byte >> 4) & 0xF];
                out += digits[byte & 0xF];
            } else {
                out += raw;
            }
            break;
        }
    }
    return out;
}

std::string json_string(std::string_view text) { return "\"" + escape(text) + "\""; }

std::string span_json(const quidra::SourceSpan& span) {
    return std::string("{\"start\":{\"offset\":") + std::to_string(span.start.offset) +
           ",\"line\":" + std::to_string(span.start.line) +
           ",\"column\":" + std::to_string(span.start.column) +
           "},\"end\":{\"offset\":" + std::to_string(span.end.offset) +
           ",\"line\":" + std::to_string(span.end.line) +
           ",\"column\":" + std::to_string(span.end.column) + "}}";
}

// Diagnostics carry a stable code, a message, and a byte-offset range that also
// reports line/column, so a client never has to re-derive positions or parse
// human text to recover meaning.
std::string diagnostic_json(const quidra::Diagnostic& diagnostic) {
    return std::string("{\"severity\":\"error\",\"code\":") + json_string(diagnostic.code) +
           ",\"message\":" + json_string(diagnostic.message) +
           ",\"span\":" + span_json(diagnostic.span) + "}";
}

std::string diagnostics_json(const std::vector<quidra::Diagnostic>& diagnostics) {
    std::string out = "[";
    for (std::size_t i = 0; i < diagnostics.size(); ++i) {
        if (i != 0) out += ",";
        out += diagnostic_json(diagnostics[i]);
    }
    out += "]";
    return out;
}

std::string envelope_head(std::string_view operation, bool ok) {
    return std::string("{\"schema_version\":") + std::to_string(wasm_schema_version) +
           ",\"ok\":" + (ok ? "true" : "false") + ",\"operation\":" + json_string(operation);
}

std::string success(std::string_view operation, const std::string& body) {
    return envelope_head(operation, true) + (body.empty() ? "" : "," + body) + "}";
}

std::string failure(std::string_view operation,
                    std::string_view kind,
                    std::string_view message,
                    const std::string& extra = {}) {
    return envelope_head(operation, false) + ",\"error\":{\"kind\":" + json_string(kind) +
           ",\"message\":" + json_string(message) + (extra.empty() ? "" : "," + extra) + "}}";
}

// ---------------------------------------------------------------------------
// JSON input
//
// Only the request envelope is parsed here. This is a transport reader for a
// flat object of scalars; it is not, and must not grow into, a general JSON
// facility that competes with the patch protocol's own reader.
// ---------------------------------------------------------------------------

class RequestError final : public std::runtime_error {
public:
    explicit RequestError(const std::string& message) : std::runtime_error(message) {}
};

class RequestReader {
public:
    explicit RequestReader(std::string_view text) : text_(text) {}

    void parse_object() {
        skip_ws();
        expect('{');
        skip_ws();
        if (peek() == '}') {
            ++pos_;
            return;
        }
        while (true) {
            skip_ws();
            const std::string key = parse_string();
            skip_ws();
            expect(':');
            read_member(key);
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            expect('}');
            return;
        }
    }

    std::optional<std::string> string_field(const std::string& key) const {
        for (const auto& entry : strings_) {
            if (entry.first == key) return entry.second;
        }
        return std::nullopt;
    }

    std::optional<double> number_field(const std::string& key) const {
        for (const auto& entry : numbers_) {
            if (entry.first == key) return entry.second;
        }
        return std::nullopt;
    }

    std::optional<bool> bool_field(const std::string& key) const {
        for (const auto& entry : bools_) {
            if (entry.first == key) return entry.second;
        }
        return std::nullopt;
    }

private:
    std::string_view text_;
    std::size_t pos_{};
    std::size_t depth_{};
    std::vector<std::pair<std::string, std::string>> strings_;
    std::vector<std::pair<std::string, double>> numbers_;
    std::vector<std::pair<std::string, bool>> bools_;

    bool eof() const { return pos_ >= text_.size(); }
    char peek() const { return eof() ? '\0' : text_[pos_]; }

    [[noreturn]] void fail(const std::string& message) const { throw RequestError(message); }

    void expect(char expected) {
        if (eof() || text_[pos_] != expected) {
            fail(std::string("expected '") + expected + "' in request JSON");
        }
        ++pos_;
    }

    void skip_ws() {
        while (!eof() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' ||
                          text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    void read_member(const std::string& key) {
        skip_ws();
        const char c = peek();
        if (c == '"') {
            strings_.emplace_back(key, parse_string());
        } else if (c == 't' || c == 'f') {
            bools_.emplace_back(key, parse_bool());
        } else if (c == 'n') {
            expect_literal("null");
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            numbers_.emplace_back(key, parse_number());
        } else if (c == '{' || c == '[') {
            // Nested values are skipped rather than modelled: the envelope's
            // own fields are all scalars, and nested structures reaching this
            // reader are payloads for someone else's parser.
            skip_value();
        } else {
            fail("unsupported value in request JSON");
        }
    }

    void expect_literal(std::string_view literal) {
        if (text_.compare(pos_, literal.size(), literal) != 0) {
            fail("invalid literal in request JSON");
        }
        pos_ += literal.size();
    }

    bool parse_bool() {
        if (peek() == 't') {
            expect_literal("true");
            return true;
        }
        expect_literal("false");
        return false;
    }

    double parse_number() {
        const std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        while (!eof() && ((text_[pos_] >= '0' && text_[pos_] <= '9') || text_[pos_] == '.' ||
                          text_[pos_] == 'e' || text_[pos_] == 'E' || text_[pos_] == '+' ||
                          text_[pos_] == '-')) {
            ++pos_;
        }
        if (start == pos_) fail("invalid number in request JSON");
        try {
            return std::stod(std::string(text_.substr(start, pos_ - start)));
        } catch (...) {
            fail("invalid number in request JSON");
        }
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (true) {
            if (eof()) fail("unterminated string in request JSON");
            const char c = text_[pos_++];
            if (c == '"') return out;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (eof()) fail("unterminated escape in request JSON");
            const char escaped = text_[pos_++];
            switch (escaped) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': out += parse_unicode_escape(); break;
            default: fail("invalid escape in request JSON");
            }
        }
    }

    // UTF-16 escapes, surrogate pairs included, encoded back out as UTF-8.
    std::string parse_unicode_escape() {
        unsigned int code = parse_hex4();
        if (code >= 0xD800 && code <= 0xDBFF && text_.compare(pos_, 2, "\\u") == 0) {
            const std::size_t saved = pos_;
            pos_ += 2;
            const unsigned int low = parse_hex4();
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            } else {
                pos_ = saved;
            }
        }
        std::string out;
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
        return out;
    }

    unsigned int parse_hex4() {
        unsigned int value = 0;
        for (int i = 0; i < 4; ++i) {
            if (eof()) fail("truncated unicode escape in request JSON");
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<unsigned int>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned int>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned int>(c - 'A' + 10);
            else fail("invalid unicode escape in request JSON");
        }
        return value;
    }

    void skip_value() {
        if (++depth_ > 64) fail("request JSON nesting is too deep");
        skip_ws();
        const char c = peek();
        if (c == '"') {
            (void)parse_string();
        } else if (c == '{' || c == '[') {
            const char close = (c == '{') ? '}' : ']';
            ++pos_;
            skip_ws();
            if (peek() == close) {
                ++pos_;
            } else {
                while (true) {
                    skip_value();
                    skip_ws();
                    if (peek() == ',') {
                        ++pos_;
                        continue;
                    }
                    expect(close);
                    break;
                }
            }
        } else if (c == ':') {
            ++pos_;
            skip_value();
        } else if (c == 't' || c == 'f') {
            (void)parse_bool();
        } else if (c == 'n') {
            expect_literal("null");
        } else {
            (void)parse_number();
        }
        --depth_;
    }
};

// ---------------------------------------------------------------------------
// Operations. Each one delegates to quidra_core and shapes the answer.
// ---------------------------------------------------------------------------

std::string default_filename() { return quidra::source_filename("main"); }

quidra::CompileOptions options_from(const RequestReader& request) {
    quidra::CompileOptions options;
    if (const auto max_errors = request.number_field("max_errors");
        max_errors && *max_errors >= 1) {
        options.max_errors = static_cast<std::size_t>(*max_errors);
    }
    return options;
}

std::string compile_failure(std::string_view operation,
                            const std::vector<quidra::Diagnostic>& diagnostics,
                            bool truncated) {
    return failure(operation, "compile_failed", "The source did not pass checking.",
                   std::string("\"diagnostics\":") + diagnostics_json(diagnostics) +
                       ",\"truncated\":" + (truncated ? "true" : "false"));
}

std::string run_check(const std::string& source, const RequestReader& request) {
    try {
        (void)quidra::check(source, options_from(request));
    } catch (const quidra::CompileErrors& errors) {
        return success("check", std::string("\"valid\":false,\"diagnostics\":") +
                                    diagnostics_json(errors.diagnostics()) + ",\"truncated\":" +
                                    (errors.truncated() ? "true" : "false"));
    } catch (const quidra::CompileError& error) {
        return success("check", std::string("\"valid\":false,\"diagnostics\":") +
                                    diagnostics_json({error.diagnostic()}) +
                                    ",\"truncated\":false");
    }
    return success("check", "\"valid\":true,\"diagnostics\":[],\"truncated\":false");
}

std::string run_format(const std::string& source) {
    std::string formatted;
    try {
        formatted = quidra::format_source(source);
    } catch (const quidra::CompileErrors& errors) {
        return compile_failure("format", errors.diagnostics(), errors.truncated());
    } catch (const quidra::CompileError& error) {
        return compile_failure("format", {error.diagnostic()}, false);
    }
    return success("format", std::string("\"source\":") + json_string(formatted) + ",\"changed\":" +
                                 (formatted == source ? "false" : "true"));
}

std::string run_ir(const std::string& source, const RequestReader& request) {
    try {
        // IR is only meaningful for a program that checks, so the checker runs
        // first and its diagnostics are what a failing request reports.
        auto checked = quidra::check(source, options_from(request));
        const auto module = quidra::ir::lower(checked);
        return success("ir", std::string("\"text\":") + json_string(quidra::ir::dump(module)) +
                                 ",\"ir_version\":" + json_string(quidra::ir_version));
    } catch (const quidra::CompileErrors& errors) {
        return compile_failure("ir", errors.diagnostics(), errors.truncated());
    } catch (const quidra::CompileError& error) {
        return compile_failure("ir", {error.diagnostic()}, false);
    }
}

std::string run_inspect(const std::string& source,
                        const std::string& filename,
                        const RequestReader& request) {
    quidra::InspectOptions inspect;
    if (const auto include_source = request.bool_field("include_source")) {
        inspect.include_source = *include_source;
    }
    if (const auto include_effects = request.bool_field("include_effects")) {
        inspect.include_effects = *include_effects;
    }
    if (const auto kind = request.string_field("kind")) inspect.kind = *kind;
    if (const auto depth = request.number_field("max_depth"); depth && *depth >= 0) {
        inspect.max_depth = static_cast<std::size_t>(*depth);
    }
    try {
        auto checked = quidra::check(source, options_from(request));
        // inspect_source_json already is the machine contract the CLI emits;
        // it is spliced in verbatim rather than restated in another shape.
        return success("inspect",
                       std::string("\"inspection\":") +
                           quidra::inspect_source_json(source, checked, filename,
                                                       std::move(inspect)));
    } catch (const quidra::CompileErrors& errors) {
        return compile_failure("inspect", errors.diagnostics(), errors.truncated());
    } catch (const quidra::CompileError& error) {
        return compile_failure("inspect", {error.diagnostic()}, false);
    }
}

std::string run_patch(const std::string& source, const RequestReader& request) {
    const auto patch = request.string_field("patch");
    if (!patch) {
        return failure("patch", "invalid_request", "A patch request requires a 'patch' field.");
    }
    quidra::CheckedProgram checked;
    try {
        checked = quidra::check(source, options_from(request));
    } catch (const quidra::CompileErrors& errors) {
        return compile_failure("patch", errors.diagnostics(), errors.truncated());
    } catch (const quidra::CompileError& error) {
        return compile_failure("patch", {error.diagnostic()}, false);
    }

    try {
        // The validator is what makes a patch trustworthy: a patch whose result
        // does not check is refused, and the caller's source is left untouched.
        const auto result = quidra::apply_source_patch(
            source, checked, *patch,
            [](std::string_view updated) { (void)quidra::check(updated); });
        return success("patch", std::string("\"base_revision\":") + json_string(result.base_revision) +
                                    ",\"revision\":" + json_string(result.revision) +
                                    ",\"source\":" + json_string(result.source));
    } catch (const quidra::PatchError& error) {
        return failure("patch", "patch_failed", error.what(),
                       std::string("\"code\":") + json_string(error.code()) +
                           ",\"span\":" + span_json(error.span()) +
                           ",\"node_id\":" + json_string(error.node_id()));
    } catch (const quidra::CompileErrors& errors) {
        // The validator rejected the patched source.
        return failure("patch", "patch_rejected",
                       "The patched source does not pass checking.",
                       std::string("\"diagnostics\":") + diagnostics_json(errors.diagnostics()));
    } catch (const quidra::CompileError& error) {
        return failure("patch", "patch_rejected",
                       "The patched source does not pass checking.",
                       std::string("\"diagnostics\":") + diagnostics_json({error.diagnostic()}));
    }
}

// Everything a client needs to state exactly which compiler answered, without
// guessing. The product version comes from project.toml by way of project.hpp.
std::string run_metadata() {
    return success("metadata",
                   std::string("\"metadata\":{\"product_version\":") +
                       json_string(quidra::compiler_version) +
                       ",\"language_version\":" + json_string(quidra::language_version) +
                       ",\"ir_version\":" + json_string(quidra::ir_version) +
                       ",\"core_commit\":" + json_string(QUIDRA_CORE_COMMIT) +
                       ",\"wasm_schema_version\":" + std::to_string(wasm_schema_version) +
                       ",\"language_name\":" + json_string(quidra::language_name) +
                       ",\"source_extension\":" + json_string(quidra::source_extension) +
                       ",\"default_filename\":" + json_string(default_filename()) +
                       ",\"tagline\":" + json_string(quidra::tagline) + "}");
}

std::string run_patch_schema() {
    return success("patch_schema",
                   std::string("\"schema\":") + std::string(quidra::patch_schema_json()));
}

std::string dispatch(const char* request_json) {
    RequestReader request(request_json ? std::string_view(request_json) : std::string_view());
    request.parse_object();

    const auto operation_field = request.string_field("operation");
    const std::string operation = operation_field ? *operation_field : std::string();
    if (operation.empty()) {
        return failure("unknown", "invalid_request", "A request requires an 'operation' field.");
    }

    if (const auto version = request.number_field("schema_version");
        version && static_cast<int>(*version) != wasm_schema_version) {
        return failure(operation, "unsupported_schema_version",
                       "This build speaks request schema version " +
                           std::to_string(wasm_schema_version) + ".");
    }

    if (operation == "metadata") return run_metadata();
    if (operation == "patch_schema") return run_patch_schema();

    const auto source_field = request.string_field("source");
    if (!source_field) {
        return failure(operation, "invalid_request", "This operation requires a 'source' field.");
    }
    const std::string& source = *source_field;
    const auto filename_field = request.string_field("filename");
    const std::string filename =
        (filename_field && !filename_field->empty()) ? *filename_field : default_filename();

    if (operation == "check") return run_check(source, request);
    if (operation == "format") return run_format(source);
    if (operation == "ir") return run_ir(source, request);
    if (operation == "inspect") return run_inspect(source, filename, request);
    if (operation == "patch") return run_patch(source, request);

    return failure(operation, "unknown_operation", "This build does not implement '" + operation +
                                                       "'. Execution is not offered.");
}

char* give(const std::string& text) {
    char* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out == nullptr) return nullptr;
    std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

} // namespace

extern "C" {

// Returns an owned JSON envelope, or nullptr only if the result could not be
// allocated. No exception escapes: the ladder below is the boundary.
char* quidra_wasm_invoke(const char* request_json) {
    try {
        return give(dispatch(request_json));
    } catch (const RequestError& error) {
        return give(failure("unknown", "invalid_request", error.what()));
    } catch (const std::exception& error) {
        return give(failure("unknown", "internal", error.what()));
    } catch (...) {
        return give(failure("unknown", "internal", "An unknown failure occurred."));
    }
}

void quidra_wasm_free(char* result) { std::free(result); }

} // extern "C"
