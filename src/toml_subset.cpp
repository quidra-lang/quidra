#include "quidra/toml_subset.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace quidra {
namespace {

std::string_view trim(std::string_view text) {
    const auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r';
    };
    while (!text.empty() && is_space(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && is_space(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

[[noreturn]] void fail(std::string_view origin, std::size_t line,
                       std::string_view message) {
    std::ostringstream out;
    out << origin << " line " << line << ": " << message;
    throw std::runtime_error(out.str());
}

std::string parse_value(std::string_view text, std::string_view origin,
                        std::size_t line) {
    if (text.starts_with('[')) {
        fail(origin, line, "arrays are not accepted in project metadata");
    }
    if (text.starts_with('"')) {
        if (text.size() < 2 || !text.ends_with('"')) {
            fail(origin, line, "unterminated string");
        }
        const auto body = text.substr(1, text.size() - 2);
        if (body.find('"') != std::string_view::npos ||
            body.find('\\') != std::string_view::npos) {
            fail(origin, line,
                 "strings may not contain quotes or backslashes");
        }
        return std::string(body);
    }
    if (text == "true" || text == "false") return std::string(text);
    const bool numeric =
        !text.empty() &&
        text.find_first_not_of("0123456789") == std::string_view::npos;
    if (numeric) return std::string(text);
    fail(origin, line, "unsupported value: " + std::string(text));
}

} // namespace

const std::string* TomlDocument::find(std::string_view table,
                                      std::string_view key) const {
    const auto section = tables.find(std::string(table));
    if (section == tables.end()) return nullptr;
    const auto entry = section->second.find(std::string(key));
    if (entry == section->second.end()) return nullptr;
    return &entry->second;
}

TomlDocument parse_toml_subset(std::string_view text, std::string_view origin) {
    TomlDocument document;
    std::map<std::string, std::string>* table = nullptr;
    std::size_t line_number = 0;

    std::istringstream input{std::string(text)};
    std::string raw;
    while (std::getline(input, raw)) {
        ++line_number;
        const auto line = trim(raw);
        if (line.empty() || line.front() == '#') continue;

        if (line.starts_with("[[")) {
            fail(origin, line_number,
                 "arrays of tables are not accepted in project metadata");
        }
        if (line.front() == '[') {
            if (!line.ends_with(']')) {
                fail(origin, line_number, "unterminated [table] header");
            }
            const auto name = trim(line.substr(1, line.size() - 2));
            if (name.empty()) fail(origin, line_number, "empty [table] header");
            const auto [entry, inserted] =
                document.tables.emplace(std::string(name),
                                        std::map<std::string, std::string>{});
            if (!inserted) {
                fail(origin, line_number,
                     "duplicate table: " + std::string(name));
            }
            table = &entry->second;
            continue;
        }

        const auto equal = line.find('=');
        if (equal == std::string_view::npos) {
            fail(origin, line_number, "expected 'key = value'");
        }
        const auto key = trim(line.substr(0, equal));
        const auto value = trim(line.substr(equal + 1));
        if (key.empty()) fail(origin, line_number, "empty key");
        if (!table) {
            fail(origin, line_number,
                 "key outside any table: " + std::string(key));
        }
        if (!table->emplace(std::string(key),
                            parse_value(value, origin, line_number))
                 .second) {
            fail(origin, line_number, "duplicate key: " + std::string(key));
        }
    }
    return document;
}

std::optional<TomlDocument> try_read_toml_subset(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return parse_toml_subset(buffer.str(), path.string());
}

} // namespace quidra
