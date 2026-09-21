#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace quidra {

// Reader for the TOML subset Quidra project metadata is written in. The same
// subset is described in scripts/toml_subset.py, and keeping both readers to one
// small grammar is what lets the tooling and the compiler agree on a file.
//
// Accepted: full-line `#` comments, `[table]` headers, and `key = value` where
// value is a double-quoted string, a decimal integer, or true/false. Arrays and
// [[arrays of tables]] are rejected here: package metadata does not use them, and
// silently ignoring a construct would be worse than refusing it.
struct TomlDocument {
    // table name -> key -> value, with strings already unquoted.
    std::map<std::string, std::map<std::string, std::string>> tables;

    const std::string* find(std::string_view table, std::string_view key) const;
};

// Throws std::runtime_error with the offending line number on malformed input.
TomlDocument parse_toml_subset(std::string_view text, std::string_view origin);
std::optional<TomlDocument> try_read_toml_subset(
    const std::filesystem::path& path);

} // namespace quidra
