#pragma once

// A strict JSON reader for documents with a fixed schema: source patches
// and the run cache's own files.
//
// It accepts one JSON value surrounded by white space and nothing else, with
// these restrictions, which the schemas it serves never need:
//   - numbers are integers that fit in 64 bits (no fraction, no exponent);
//   - an object must not repeat a key;
//   - values nest at most `max_depth` deep.
// Strings are decoded to UTF-8, surrogate pairs included; an unpaired
// surrogate is an error.
//
// Errors throw JsonReadError, whose message names the document as the
// caller asks ("Unexpected end of patch JSON."), so each caller keeps the
// messages it has always given.
//
// The language server keeps its own reader (src/lsp_server.cpp): LSP
// messages carry fractional numbers, and that reader accepts them, lets a
// repeated key replace the earlier one and nests up to 128 deep, so it
// cannot share this one without changing what the server accepts.

#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace quidra::platform {

struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;
    using Data = std::variant<std::nullptr_t, bool, std::int64_t, std::string, Array, Object>;
    Data data;
};

class JsonReadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct JsonReadOptions {
    // The document's name in messages, as it reads inside a sentence
    // ("patch JSON"); a sentence that starts with it capitalizes it.
    std::string_view document{"JSON"};
    std::size_t max_depth{64};
};

JsonValue read_strict_json(std::string_view text, const JsonReadOptions& options = {});

} // namespace quidra::platform
