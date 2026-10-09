#include "toolchain/driver_output.hpp"

#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace quidra::toolchain {
namespace {

std::vector<std::string_view> lines_of(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
        start = end + 1;
    }
    return lines;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.substr(text.size() - suffix.size()) == suffix;
}

template <class T>
void append_unique(std::vector<T>& items, T item) {
    if (std::find(items.begin(), items.end(), item) == items.end()) {
        items.push_back(std::move(item));
    }
}

// The arguments of one job line: double-quoted, with a backslash escaping
// the next character (clang escapes '"', '\' and '$'). nullopt when the
// line is not a job.
std::optional<std::vector<std::string>> job_arguments(std::string_view line) {
    const auto body = trim(line);
    if (body.empty() || body.front() != '"') return std::nullopt;
    std::vector<std::string> arguments;
    std::size_t index = 0;
    while (index < body.size()) {
        while (index < body.size() && std::isspace(static_cast<unsigned char>(body[index]))) {
            ++index;
        }
        if (index >= body.size()) break;
        if (body[index] != '"') return std::nullopt;
        ++index;
        std::string argument;
        bool closed = false;
        while (index < body.size()) {
            const char c = body[index++];
            if (c == '\\' && index < body.size()) {
                argument.push_back(body[index++]);
            } else if (c == '"') {
                closed = true;
                break;
            } else {
                argument.push_back(c);
            }
        }
        if (!closed) return std::nullopt;
        arguments.push_back(std::move(argument));
    }
    if (arguments.empty()) return std::nullopt;
    return arguments;
}

// Whether `inner` lies strictly below `directory`, and the relative name.
std::optional<fs::path> relative_below(const fs::path& directory, const fs::path& inner) {
    if (directory.empty()) return std::nullopt;
    const auto relative = inner.lexically_relative(directory);
    if (relative.empty() || relative == "." || relative.is_absolute()) return std::nullopt;
    const auto first = *relative.begin();
    if (first == "..") return std::nullopt;
    return relative;
}

} // namespace

DriverPlan parse_driver_plan(std::string_view text) {
    DriverPlan plan;
    for (const auto line : lines_of(text)) {
        constexpr std::string_view configuration = "Configuration file: ";
        if (starts_with(line, configuration)) {
            plan.configuration_files.emplace_back(
                std::string(trim(line.substr(configuration.size()))));
            continue;
        }
        if (auto arguments = job_arguments(line)) plan.jobs.push_back(std::move(*arguments));
    }
    return plan;
}

std::optional<std::string> job_option(
    const std::vector<std::string>& job, std::string_view option) {
    const auto values = job_options(job, option);
    if (values.empty()) return std::nullopt;
    return values.front();
}

std::vector<std::string> job_options(
    const std::vector<std::string>& job, std::string_view option) {
    std::vector<std::string> values;
    const bool joined = ends_with(option, "=");
    for (std::size_t index = 0; index < job.size(); ++index) {
        const std::string_view argument = job[index];
        if (joined) {
            if (starts_with(argument, option)) {
                values.emplace_back(argument.substr(option.size()));
            }
        } else if (argument == option && index + 1 < job.size()) {
            values.push_back(job[index + 1]);
            ++index;
        }
    }
    return values;
}

std::optional<IncludeSearch> parse_include_search(std::string_view text) {
    enum class Part { before, quote, angle, done };
    Part part = Part::before;
    IncludeSearch search;
    for (const auto line : lines_of(text)) {
        if (starts_with(line, "#include \"...\" search starts here:")) {
            part = Part::quote;
            continue;
        }
        if (starts_with(line, "#include <...> search starts here:")) {
            part = Part::angle;
            continue;
        }
        if (starts_with(line, "End of search list.")) {
            if (part == Part::angle) part = Part::done;
            break;
        }
        if (part != Part::quote && part != Part::angle) continue;
        if (line.empty() || !std::isspace(static_cast<unsigned char>(line.front()))) continue;
        auto entry = trim(line);
        for (const std::string_view suffix : {" (framework directory)", " (headermap)"}) {
            if (ends_with(entry, suffix)) entry.remove_suffix(suffix.size());
        }
        if (entry.empty()) continue;
        auto& list = part == Part::quote ? search.quote : search.angle;
        list.push_back(fs::path(std::string(entry)).lexically_normal());
    }
    if (part != Part::done) return std::nullopt;
    return search;
}

std::optional<LibrarySearch> parse_linker_search(std::string_view text) {
    enum class Part { none, library, framework };
    Part part = Part::none;
    bool seen = false;
    LibrarySearch search;
    for (const auto line : lines_of(text)) {
        if (line == "Library search paths:") {
            part = Part::library;
            seen = true;
            continue;
        }
        if (line == "Framework search paths:") {
            part = Part::framework;
            continue;
        }
        if (part == Part::none) continue;
        if (line.empty() || (line.front() != '\t' && line.front() != ' ')) {
            part = Part::none;
            continue;
        }
        const auto entry = trim(line);
        if (entry.empty()) continue;
        auto& list = part == Part::library ? search.library : search.framework;
        list.push_back(fs::path(std::string(entry)).lexically_normal());
    }
    if (!seen) return std::nullopt;
    return search;
}

std::vector<fs::path> parse_depfile(std::string_view text) {
    // Join continuation lines; a backslash before a line break continues the
    // rule.
    std::string joined;
    joined.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\\' && index + 1 < text.size() &&
            (text[index + 1] == '\n' ||
             (text[index + 1] == '\r' && index + 2 < text.size() && text[index + 2] == '\n'))) {
            index += text[index + 1] == '\r' ? 2 : 1;
            joined.push_back(' ');
            continue;
        }
        joined.push_back(text[index]);
    }

    std::vector<fs::path> prerequisites;
    for (const auto rule : lines_of(joined)) {
        // The targets end at the first ':' followed by white space or the
        // end of the rule; a drive letter ("C:\") is followed by neither.
        std::size_t colon = std::string_view::npos;
        for (std::size_t index = 0; index < rule.size(); ++index) {
            if (rule[index] == '\\' && index + 1 < rule.size()) {
                ++index;
                continue;
            }
            if (rule[index] == ':' &&
                (index + 1 == rule.size() ||
                 std::isspace(static_cast<unsigned char>(rule[index + 1])))) {
                colon = index;
                break;
            }
        }
        if (colon == std::string_view::npos) continue;
        std::string current;
        const auto flush = [&] {
            if (!current.empty()) {
                append_unique(prerequisites, fs::path(current));
                current.clear();
            }
        };
        for (std::size_t index = colon + 1; index < rule.size(); ++index) {
            const char c = rule[index];
            if (c == '\\' && index + 1 < rule.size() &&
                (rule[index + 1] == ' ' || rule[index + 1] == '#' || rule[index + 1] == '\t')) {
                current.push_back(rule[++index]);
            } else if (c == '$' && index + 1 < rule.size() && rule[index + 1] == '$') {
                current.push_back('$');
                ++index;
            } else if (std::isspace(static_cast<unsigned char>(c))) {
                flush();
            } else {
                current.push_back(c);
            }
        }
        flush();
    }
    return prerequisites;
}

std::vector<fs::path> parse_link_trace(std::string_view text) {
    std::vector<fs::path> files;
    for (const auto raw : lines_of(text)) {
        auto line = trim(raw);
        if (line.empty()) continue;
        // GNU ld: "-lc (/usr/lib/x86_64-linux-gnu/libc.so)".
        if (line.front() == '-') {
            const auto open = line.find(" (");
            if (open == std::string_view::npos || line.back() != ')') continue;
            line = line.substr(open + 2, line.size() - open - 3);
        } else if (line.front() == '(') {
            // GNU ld archive member: "(/usr/lib/libc.a)member.o".
            const auto close = line.find(')');
            if (close == std::string_view::npos) continue;
            line = line.substr(1, close - 1);
        } else if (line.back() == ')') {
            // ld64 and lld archive member: "/usr/lib/libc.a(member.o)".
            const auto open = line.rfind('(');
            if (open == std::string_view::npos || open == 0) continue;
            line = line.substr(0, open);
        }
        // A status line ("/usr/bin/ld: mode elf_x86_64") is no file.
        if (line.find(": ") != std::string_view::npos) continue;
        if (line.empty()) continue;
        append_unique(files, fs::path(std::string(line)).lexically_normal());
    }
    return files;
}

std::vector<std::string> library_file_names(std::string_view name, bool apple) {
    std::vector<std::string> names;
    const std::string stem = "lib" + std::string(name);
    if (apple) {
        names.push_back(stem + ".tbd");
        names.push_back(stem + ".dylib");
    }
    names.push_back(stem + ".so");
    names.push_back(stem + ".a");
    return names;
}

std::vector<fs::path> header_shadowing_paths(
    const std::vector<fs::path>& search, const fs::path& found) {
    const auto header = found.lexically_normal();
    std::vector<fs::path> paths;
    for (std::size_t j = 0; j < search.size(); ++j) {
        const auto relative = relative_below(search[j].lexically_normal(), header);
        if (!relative) continue;
        for (std::size_t i = 0; i < j; ++i) {
            auto candidate = (search[i].lexically_normal() / *relative).lexically_normal();
            if (candidate != header) append_unique(paths, std::move(candidate));
        }
    }
    return paths;
}

std::vector<fs::path> library_shadowing_paths(
    const std::vector<fs::path>& search, const fs::path& found, bool apple) {
    const auto library = found.lexically_normal();
    const auto directory = library.parent_path();
    const auto file = library.filename().string();
    std::vector<fs::path> paths;
    for (std::size_t j = 0; j < search.size(); ++j) {
        if (search[j].lexically_normal() != directory) continue;
        if (!starts_with(file, "lib")) return paths;
        // The NAME of -lNAME: the file name without "lib" and a candidate
        // extension.
        std::optional<std::string> name;
        for (const std::string_view extension : {".tbd", ".dylib", ".so", ".a"}) {
            if (ends_with(file, extension) && file.size() > 3 + extension.size()) {
                name = file.substr(3, file.size() - 3 - extension.size());
                break;
            }
        }
        if (!name) return paths;
        const auto names = library_file_names(*name, apple);
        if (std::find(names.begin(), names.end(), file) == names.end()) return paths;
        for (std::size_t i = 0; i < j; ++i) {
            for (const auto& candidate : names) {
                append_unique(paths, (search[i].lexically_normal() / candidate).lexically_normal());
            }
        }
        for (const auto& candidate : names) {
            if (candidate == file) break;
            append_unique(paths, (directory / candidate).lexically_normal());
        }
        return paths;
    }
    return paths;
}

std::vector<fs::path> framework_shadowing_paths(
    const std::vector<fs::path>& search, const fs::path& found) {
    const auto file = found.lexically_normal();
    std::vector<fs::path> paths;
    for (std::size_t j = 0; j < search.size(); ++j) {
        const auto relative = relative_below(search[j].lexically_normal(), file);
        if (!relative) continue;
        const auto bundle = relative->begin()->string();
        if (!ends_with(bundle, ".framework")) continue;
        for (std::size_t i = 0; i < j; ++i) {
            append_unique(paths, (search[i].lexically_normal() / bundle).lexically_normal());
        }
    }
    return paths;
}

} // namespace quidra::toolchain
