#include "toolchain/run_cache_metadata.hpp"

#include "platform/sha256.hpp"
#include "platform/strict_json.hpp"

#include <array>
#include <charconv>
#include <fstream>
#include <initializer_list>
#include <system_error>
#include <utility>
#include <variant>

namespace fs = std::filesystem;

namespace quidra::toolchain {
namespace {

using platform::JsonValue;

constexpr std::string_view entry_kind = "quidra-run-cache-entry";
constexpr std::string_view index_kind = "quidra-run-cache-index";

// ---- Writing --------------------------------------------------------------

std::string path_text(const fs::path& path) {
    const auto encoded = path.u8string();
    return std::string(encoded.begin(), encoded.end());
}

fs::path text_path(const std::string& text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string json_string(std::string_view text) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else if (byte < 0x20U) {
            out += "\\u00";
            out.push_back(hex[byte >> 4U]);
            out.push_back(hex[byte & 0xfU]);
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

// Builds one JSON object, member by member, in the order they are added.
class ObjectWriter {
public:
    ObjectWriter& raw(std::string_view name, std::string_view json) {
        out_ += out_.size() > 1 ? "," : "";
        out_ += json_string(name);
        out_ += ":";
        out_ += json;
        return *this;
    }
    ObjectWriter& text(std::string_view name, std::string_view value) {
        return raw(name, json_string(value));
    }
    ObjectWriter& path(std::string_view name, const fs::path& value) {
        return text(name, path_text(value));
    }
    ObjectWriter& integer(std::string_view name, std::int64_t value) {
        return raw(name, std::to_string(value));
    }
    ObjectWriter& optional_text(std::string_view name, const std::optional<std::string>& value) {
        return value ? text(name, *value) : raw(name, "null");
    }
    ObjectWriter& optional_path(std::string_view name, const std::optional<fs::path>& value) {
        return value ? path(name, *value) : raw(name, "null");
    }
    std::string done() { return out_ + "}"; }

private:
    std::string out_ = "{";
};

template <class Items, class Write>
std::string json_array(const Items& items, Write write) {
    std::string out = "[";
    for (const auto& item : items) {
        if (out.size() > 1) out += ",";
        out += write(item);
    }
    return out + "]";
}

std::string paths_json(const std::vector<fs::path>& paths) {
    return json_array(paths, [](const fs::path& path) { return json_string(path_text(path)); });
}

std::string texts_json(const std::vector<std::string>& texts) {
    return json_array(texts, [](const std::string& text) { return json_string(text); });
}

std::string identity_json(const platform::FileIdentity& identity) {
    return ObjectWriter()
        .text("device", std::to_string(identity.device))
        .text("file", std::to_string(identity.file))
        .text("file_high", std::to_string(identity.file_high))
        .text("size", std::to_string(identity.size))
        .text("modified_ns", std::to_string(identity.modified_ns))
        .text("changed_ns", std::to_string(identity.changed_ns))
        .done();
}

std::string identified_json(const IdentifiedFile& file) {
    return ObjectWriter().path("path", file.path).raw("identity", identity_json(file.identity)).done();
}

std::string tool_json(const ToolIdentity& tool) {
    return ObjectWriter()
        .raw("program", identified_json(tool.program))
        .text("version_sha256", tool.version_sha256)
        .text("version_line", tool.version_line)
        .done();
}

std::string optional_tool_json(const std::optional<ToolIdentity>& tool) {
    return tool ? tool_json(*tool) : std::string("null");
}

std::string toolchain_json(const ToolchainSnapshot& snapshot) {
    const auto& driver = snapshot.driver;
    const auto driver_json =
        ObjectWriter()
            .text("requested", driver.requested)
            .raw("tool", tool_json(driver.tool))
            .raw("directory", identified_json(driver.directory))
            .raw("compiler", identified_json(driver.compiler))
            .raw("compiler_directory", identified_json(driver.compiler_directory))
            .raw("configuration_files",
                 json_array(driver.configuration_files, identified_json))
            .raw("resource_directory", driver.resource_directory
                                           ? identified_json(*driver.resource_directory)
                                           : std::string("null"))
            .done();
    const auto& target = snapshot.target;
    const auto target_json = ObjectWriter()
                                 .text("triple", target.triple)
                                 .text("cpu", target.cpu)
                                 .raw("features", texts_json(target.features))
                                 .optional_path("sysroot", target.sysroot)
                                 .text("sdk_version", target.sdk_version)
                                 .optional_text("sdk_settings_sha256", target.sdk_settings_sha256)
                                 .optional_path("developer_directory", target.developer_directory)
                                 .done();
    const auto library_search =
        snapshot.library_search
            ? ObjectWriter()
                  .raw("library", paths_json(snapshot.library_search->library))
                  .raw("framework", paths_json(snapshot.library_search->framework))
                  .done()
            : std::string("null");
    const auto include_searches = json_array(snapshot.include_searches, [](const auto& search) {
        return ObjectWriter()
            .raw("quote", paths_json(search.quote))
            .raw("angle", paths_json(search.angle))
            .done();
    });
    return ObjectWriter()
        .raw("driver", driver_json)
        .raw("target", target_json)
        .raw("linker", optional_tool_json(snapshot.linker))
        .raw("library_search", library_search)
        .raw("include_searches", include_searches)
        .raw("system_directories", paths_json(snapshot.system_directories))
        .raw("pkg_config", optional_tool_json(snapshot.pkg_config))
        .raw("pkg_config_modules", texts_json(snapshot.pkg_config_modules))
        .raw("pkg_config_cflags", texts_json(snapshot.pkg_config_cflags))
        .raw("pkg_config_libs", texts_json(snapshot.pkg_config_libs))
        .raw("nvcc", optional_tool_json(snapshot.nvcc))
        .done();
}

std::string dependency_json(const RunCacheDependency& dependency) {
    ObjectWriter object;
    object.text("kind", run_cache_dependency_kind_name(dependency.kind));
    switch (dependency.kind) {
        case RunCacheDependencyKind::absent:
            object.path("path", dependency.path).text("state", dependency.state);
            break;
        case RunCacheDependencyKind::local_import:
            object.path("importer", dependency.importer)
                .text("target", dependency.target)
                .path("path", dependency.path);
            break;
        case RunCacheDependencyKind::package:
            object.text("name", dependency.name).path("main", dependency.path);
            break;
        case RunCacheDependencyKind::package_tree:
            object.path("main", dependency.path).text("sha256", dependency.sha256);
            break;
        case RunCacheDependencyKind::native_header:
            object.path("path", dependency.path).text("sha256", dependency.sha256);
            break;
        case RunCacheDependencyKind::system_file:
            object.path("path", dependency.path).raw("identity", identity_json(dependency.identity));
            break;
        default:
            object.path("path", dependency.path)
                .integer("size", static_cast<std::int64_t>(dependency.size))
                .text("sha256", dependency.sha256);
            break;
    }
    return object.done();
}

std::string stored_json(const RunCacheStoredFile& file) {
    return ObjectWriter()
        .text("file", file.file)
        .integer("size", static_cast<std::int64_t>(file.size))
        .text("sha256", file.sha256)
        .done();
}

// ---- Reading --------------------------------------------------------------

[[noreturn]] void corrupt(const std::string& what) {
    throw RunCacheCorruptError("corrupt run cache metadata: " + what);
}

// One JSON object with exactly the members a caller names.
class ObjectReader {
public:
    ObjectReader(const JsonValue& value, std::initializer_list<std::string_view> members,
                 std::string_view where)
        : where_(where) {
        object_ = std::get_if<JsonValue::Object>(&value.data);
        if (!object_) corrupt(std::string(where) + " is not an object");
        if (object_->size() != members.size()) corrupt(std::string(where) + " has other members");
        for (const auto member : members) {
            if (!object_->contains(std::string(member))) {
                corrupt(std::string(where) + " has no " + std::string(member));
            }
        }
    }

    const JsonValue& member(std::string_view name) const {
        return object_->at(std::string(name));
    }
    bool null(std::string_view name) const {
        return std::holds_alternative<std::nullptr_t>(member(name).data);
    }
    const std::string& text(std::string_view name) const {
        const auto* value = std::get_if<std::string>(&member(name).data);
        if (!value) corrupt(where_ + "." + std::string(name) + " is not a string");
        return *value;
    }
    std::string digest(std::string_view name) const {
        const auto& value = text(name);
        if (!run_cache_digest_text(value)) corrupt(where_ + "." + std::string(name) + " is not a digest");
        return value;
    }
    fs::path path(std::string_view name) const { return text_path(text(name)); }
    std::int64_t integer(std::string_view name) const {
        const auto* value = std::get_if<std::int64_t>(&member(name).data);
        if (!value) corrupt(where_ + "." + std::string(name) + " is not an integer");
        return *value;
    }
    std::uint64_t size(std::string_view name) const {
        const auto value = integer(name);
        if (value < 0) corrupt(where_ + "." + std::string(name) + " is negative");
        return static_cast<std::uint64_t>(value);
    }
    std::optional<std::string> optional_text(std::string_view name) const {
        if (null(name)) return std::nullopt;
        return text(name);
    }
    std::optional<fs::path> optional_path(std::string_view name) const {
        if (null(name)) return std::nullopt;
        return path(name);
    }
    const JsonValue::Array& array(std::string_view name) const {
        const auto* value = std::get_if<JsonValue::Array>(&member(name).data);
        if (!value) corrupt(where_ + "." + std::string(name) + " is not an array");
        return *value;
    }
    std::vector<std::string> texts(std::string_view name) const {
        std::vector<std::string> result;
        for (const auto& item : array(name)) {
            const auto* value = std::get_if<std::string>(&item.data);
            if (!value) corrupt(where_ + "." + std::string(name) + " holds a non-string");
            result.push_back(*value);
        }
        return result;
    }
    std::vector<fs::path> paths(std::string_view name) const {
        std::vector<fs::path> result;
        for (auto& text_value : texts(name)) result.push_back(text_path(text_value));
        return result;
    }
    const std::string& where() const { return where_; }

private:
    const JsonValue::Object* object_{};
    std::string where_;
};

template <class Number>
Number decimal(const std::string& text, const std::string& where) {
    Number value{};
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(begin, end, value);
    if (text.empty() || error != std::errc() || stop != end ||
        (text.size() > 1 && text[0] == '0') || text[0] == '+') {
        corrupt(where + " is not a decimal number");
    }
    return value;
}

platform::FileIdentity read_identity(const JsonValue& value, const std::string& where) {
    const ObjectReader object(
        value, {"device", "file", "file_high", "size", "modified_ns", "changed_ns"}, where);
    platform::FileIdentity identity;
    identity.device = decimal<std::uint64_t>(object.text("device"), where + ".device");
    identity.file = decimal<std::uint64_t>(object.text("file"), where + ".file");
    identity.file_high = decimal<std::uint64_t>(object.text("file_high"), where + ".file_high");
    identity.size = decimal<std::uint64_t>(object.text("size"), where + ".size");
    identity.modified_ns = decimal<std::int64_t>(object.text("modified_ns"), where + ".modified_ns");
    identity.changed_ns = decimal<std::int64_t>(object.text("changed_ns"), where + ".changed_ns");
    return identity;
}

IdentifiedFile read_identified(const JsonValue& value, const std::string& where) {
    const ObjectReader object(value, {"path", "identity"}, where);
    return IdentifiedFile{object.path("path"), read_identity(object.member("identity"), where)};
}

ToolIdentity read_tool(const JsonValue& value, const std::string& where) {
    const ObjectReader object(value, {"program", "version_sha256", "version_line"}, where);
    ToolIdentity tool;
    tool.program = read_identified(object.member("program"), where + ".program");
    tool.version_sha256 = object.digest("version_sha256");
    tool.version_line = object.text("version_line");
    return tool;
}

std::optional<ToolIdentity> read_optional_tool(const ObjectReader& parent, std::string_view name) {
    if (parent.null(name)) return std::nullopt;
    return read_tool(parent.member(name), parent.where() + "." + std::string(name));
}

ToolchainSnapshot read_toolchain(const JsonValue& value) {
    const ObjectReader object(
        value,
        {"driver", "target", "linker", "library_search", "include_searches",
         "system_directories", "pkg_config", "pkg_config_modules", "pkg_config_cflags",
         "pkg_config_libs", "nvcc"},
        "toolchain");
    ToolchainSnapshot snapshot;
    {
        const ObjectReader driver(
            object.member("driver"),
            {"requested", "tool", "directory", "compiler", "compiler_directory",
             "configuration_files", "resource_directory"},
            "toolchain.driver");
        snapshot.driver.requested = driver.text("requested");
        snapshot.driver.tool = read_tool(driver.member("tool"), "toolchain.driver.tool");
        snapshot.driver.directory =
            read_identified(driver.member("directory"), "toolchain.driver.directory");
        snapshot.driver.compiler =
            read_identified(driver.member("compiler"), "toolchain.driver.compiler");
        snapshot.driver.compiler_directory = read_identified(
            driver.member("compiler_directory"), "toolchain.driver.compiler_directory");
        for (const auto& file : driver.array("configuration_files")) {
            snapshot.driver.configuration_files.push_back(
                read_identified(file, "toolchain.driver.configuration_files"));
        }
        if (!driver.null("resource_directory")) {
            snapshot.driver.resource_directory = read_identified(
                driver.member("resource_directory"), "toolchain.driver.resource_directory");
        }
    }
    {
        const ObjectReader target(
            object.member("target"),
            {"triple", "cpu", "features", "sysroot", "sdk_version", "sdk_settings_sha256",
             "developer_directory"},
            "toolchain.target");
        snapshot.target.triple = target.text("triple");
        snapshot.target.cpu = target.text("cpu");
        snapshot.target.features = target.texts("features");
        snapshot.target.sysroot = target.optional_path("sysroot");
        snapshot.target.sdk_version = target.text("sdk_version");
        snapshot.target.sdk_settings_sha256 = target.optional_text("sdk_settings_sha256");
        if (snapshot.target.sdk_settings_sha256 &&
            !run_cache_digest_text(*snapshot.target.sdk_settings_sha256)) {
            corrupt("toolchain.target.sdk_settings_sha256 is not a digest");
        }
        snapshot.target.developer_directory = target.optional_path("developer_directory");
    }
    snapshot.linker = read_optional_tool(object, "linker");
    if (!object.null("library_search")) {
        const ObjectReader search(
            object.member("library_search"), {"library", "framework"},
            "toolchain.library_search");
        snapshot.library_search = LibrarySearch{search.paths("library"), search.paths("framework")};
    }
    for (const auto& item : object.array("include_searches")) {
        const ObjectReader search(item, {"quote", "angle"}, "toolchain.include_searches");
        snapshot.include_searches.push_back(
            IncludeSearch{search.paths("quote"), search.paths("angle")});
    }
    snapshot.system_directories = object.paths("system_directories");
    snapshot.pkg_config = read_optional_tool(object, "pkg_config");
    snapshot.pkg_config_modules = object.texts("pkg_config_modules");
    snapshot.pkg_config_cflags = object.texts("pkg_config_cflags");
    snapshot.pkg_config_libs = object.texts("pkg_config_libs");
    snapshot.nvcc = read_optional_tool(object, "nvcc");
    snapshot.complete = true;
    return snapshot;
}

RunCacheDependency read_dependency(const JsonValue& value) {
    const auto* object = std::get_if<JsonValue::Object>(&value.data);
    if (!object) corrupt("a dependency is not an object");
    const auto found = object->find("kind");
    const auto* kind_text =
        found == object->end() ? nullptr : std::get_if<std::string>(&found->second.data);
    if (!kind_text) corrupt("a dependency has no kind");
    const auto kind = run_cache_dependency_kind(*kind_text);
    if (!kind) corrupt("unknown dependency kind " + *kind_text);
    const std::string where = "dependency " + *kind_text;
    RunCacheDependency dependency;
    dependency.kind = *kind;
    switch (*kind) {
        case RunCacheDependencyKind::absent: {
            const ObjectReader reader(value, {"kind", "path", "state"}, where);
            dependency.path = reader.path("path");
            dependency.state = reader.text("state");
            if (dependency.state != "missing" && dependency.state != "other") {
                corrupt(where + " has an unknown state");
            }
            break;
        }
        case RunCacheDependencyKind::local_import: {
            const ObjectReader reader(value, {"kind", "importer", "target", "path"}, where);
            dependency.importer = reader.path("importer");
            dependency.target = reader.text("target");
            dependency.path = reader.path("path");
            break;
        }
        case RunCacheDependencyKind::package: {
            const ObjectReader reader(value, {"kind", "name", "main"}, where);
            dependency.name = reader.text("name");
            dependency.path = reader.path("main");
            break;
        }
        case RunCacheDependencyKind::package_tree: {
            const ObjectReader reader(value, {"kind", "main", "sha256"}, where);
            dependency.path = reader.path("main");
            dependency.sha256 = reader.digest("sha256");
            break;
        }
        case RunCacheDependencyKind::native_header: {
            const ObjectReader reader(value, {"kind", "path", "sha256"}, where);
            dependency.path = reader.path("path");
            dependency.sha256 = reader.digest("sha256");
            break;
        }
        case RunCacheDependencyKind::system_file: {
            const ObjectReader reader(value, {"kind", "path", "identity"}, where);
            dependency.path = reader.path("path");
            dependency.identity = read_identity(reader.member("identity"), where + ".identity");
            break;
        }
        default: {
            const ObjectReader reader(value, {"kind", "path", "size", "sha256"}, where);
            dependency.path = reader.path("path");
            dependency.size = reader.size("size");
            dependency.sha256 = reader.digest("sha256");
            break;
        }
    }
    return dependency;
}

RunCacheStoredFile read_stored(const ObjectReader& parent, std::string_view name) {
    const ObjectReader object(parent.member(name), {"file", "size", "sha256"}, std::string(name));
    RunCacheStoredFile file;
    file.file = object.text("file");
    file.size = object.size("size");
    file.sha256 = object.digest("sha256");
    if (file.file.empty() || file.file.find('/') != std::string::npos ||
        file.file.find('\\') != std::string::npos || file.file == "." || file.file == "..") {
        corrupt(std::string(name) + ".file is not a file name");
    }
    return file;
}

JsonValue parse(std::string_view text, std::string_view document) {
    try {
        return platform::read_strict_json(text, {document, 16});
    } catch (const platform::JsonReadError& error) {
        throw RunCacheCorruptError(error.what());
    }
}

bool missing_path(const fs::path& path) {
    std::error_code error;
    const auto status = fs::status(path, error);
    if (error) {
        return error == std::errc::no_such_file_or_directory ||
               error == std::errc::not_a_directory;
    }
    return status.type() == fs::file_type::not_found;
}

} // namespace

std::string write_run_cache_metadata(const RunCacheEntry& entry) {
    const auto& material = entry.material;
    const auto invocation =
        ObjectWriter()
            .path("entry", material.entry)
            .path("cwd", material.working_directory)
            .raw("driver", ObjectWriter()
                               .text("requested", material.driver_requested)
                               .path("path", material.driver_path)
                               .done())
            .path("runtime_library", material.runtime_library)
            .optional_path("native_include_directory", material.native_include_directory)
            .raw("link_inputs", json_array(material.link_inputs,
                                           [](const RunCacheLinkInput& input) {
                                               return ObjectWriter()
                                                   .text("given", input.given)
                                                   .path("path", input.path)
                                                   .done();
                                           }))
            .raw("options", json_array(material.options,
                                       [](const RunCacheOption& option) {
                                           return ObjectWriter()
                                               .text("name", option.name)
                                               .text("value", option.value)
                                               .done();
                                       }))
            .text("recipe_sha256", entry.recipe_sha256)
            .done();
    const auto environment = json_array(material.environment, [](const auto& variable) {
        return ObjectWriter()
            .text("name", variable.first)
            .optional_text("value", variable.second)
            .done();
    });
    return ObjectWriter()
               .integer("schema", run_cache_metadata_schema)
               .text("kind", entry_kind)
               .text("key", entry.key)
               .text("pre_key", entry.pre_key)
               .raw("compiler", ObjectWriter()
                                    .text("version", material.compiler_version)
                                    .integer("abi", material.abi_version)
                                    .text("ir", material.ir_version)
                                    .text("build_id", material.build_id)
                                    .done())
               .raw("host", ObjectWriter()
                                .text("platform", material.platform)
                                .text("os", material.os_version)
                                .done())
               .raw("invocation", invocation)
               .raw("environment", environment)
               .raw("toolchain", toolchain_json(entry.toolchain))
               .raw("dependencies", json_array(entry.dependencies, dependency_json))
               .raw("program", stored_json(entry.program))
               .raw("toolchain_output", stored_json(entry.toolchain_output))
               .integer("created_unix_ns", entry.created_unix_ns)
               .integer("build_wall_ms", entry.build_wall_ms)
               .done() +
           "\n";
}

RunCacheEntry read_run_cache_metadata(std::string_view text) {
    const auto document = parse(text, "run cache metadata");
    const ObjectReader root(
        document,
        {"schema", "kind", "key", "pre_key", "compiler", "host", "invocation", "environment",
         "toolchain", "dependencies", "program", "toolchain_output", "created_unix_ns",
         "build_wall_ms"},
        "metadata");
    if (root.integer("schema") != run_cache_metadata_schema) corrupt("unknown schema");
    if (root.text("kind") != entry_kind) corrupt("unknown kind");
    RunCacheEntry entry;
    entry.key = root.digest("key");
    entry.pre_key = root.digest("pre_key");
    auto& material = entry.material;
    {
        const ObjectReader compiler(
            root.member("compiler"), {"version", "abi", "ir", "build_id"}, "compiler");
        material.compiler_version = compiler.text("version");
        material.abi_version = compiler.integer("abi");
        material.ir_version = compiler.text("ir");
        material.build_id = compiler.digest("build_id");
    }
    {
        const ObjectReader host(root.member("host"), {"platform", "os"}, "host");
        material.platform = host.text("platform");
        material.os_version = host.text("os");
    }
    {
        const ObjectReader invocation(
            root.member("invocation"),
            {"entry", "cwd", "driver", "runtime_library", "native_include_directory",
             "link_inputs", "options", "recipe_sha256"},
            "invocation");
        material.entry = invocation.path("entry");
        material.working_directory = invocation.path("cwd");
        const ObjectReader driver(
            invocation.member("driver"), {"requested", "path"}, "invocation.driver");
        material.driver_requested = driver.text("requested");
        material.driver_path = driver.path("path");
        material.runtime_library = invocation.path("runtime_library");
        material.native_include_directory = invocation.optional_path("native_include_directory");
        for (const auto& item : invocation.array("link_inputs")) {
            const ObjectReader input(item, {"given", "path"}, "invocation.link_inputs");
            material.link_inputs.push_back(RunCacheLinkInput{input.text("given"), input.path("path")});
        }
        for (const auto& item : invocation.array("options")) {
            const ObjectReader option(item, {"name", "value"}, "invocation.options");
            material.options.push_back(RunCacheOption{option.text("name"), option.text("value")});
        }
        entry.recipe_sha256 = invocation.digest("recipe_sha256");
    }
    for (const auto& item : root.array("environment")) {
        const ObjectReader variable(item, {"name", "value"}, "environment");
        material.environment.emplace_back(variable.text("name"), variable.optional_text("value"));
    }
    entry.toolchain = read_toolchain(root.member("toolchain"));
    for (const auto& item : root.array("dependencies")) {
        entry.dependencies.push_back(read_dependency(item));
    }
    entry.program = read_stored(root, "program");
    entry.toolchain_output = read_stored(root, "toolchain_output");
    entry.created_unix_ns = root.integer("created_unix_ns");
    entry.build_wall_ms = root.integer("build_wall_ms");
    return entry;
}

std::string write_run_cache_index(const RunCacheIndex& index) {
    return ObjectWriter()
               .integer("schema", run_cache_metadata_schema)
               .text("kind", index_kind)
               .text("pre_key", index.pre_key)
               .raw("keys", texts_json(index.keys))
               .done() +
           "\n";
}

std::optional<RunCacheIndex> read_run_cache_index(std::string_view text) {
    try {
        const auto document = parse(text, "run cache index");
        const ObjectReader root(document, {"schema", "kind", "pre_key", "keys"}, "index");
        if (root.integer("schema") != run_cache_metadata_schema || root.text("kind") != index_kind) {
            return std::nullopt;
        }
        RunCacheIndex index;
        index.pre_key = root.digest("pre_key");
        index.keys = root.texts("keys");
        if (index.keys.size() > run_cache_index_limit) return std::nullopt;
        for (const auto& key : index.keys) {
            if (!run_cache_digest_text(key)) return std::nullopt;
        }
        return index;
    } catch (const RunCacheCorruptError&) {
        return std::nullopt;
    }
}

RunCacheIndex run_cache_index_with(RunCacheIndex index, const std::string& key) {
    std::vector<std::string> keys{key};
    for (auto& existing : index.keys) {
        if (existing != key && keys.size() < run_cache_index_limit) keys.push_back(std::move(existing));
    }
    index.keys = std::move(keys);
    return index;
}

std::optional<RunCacheContentDigest> run_cache_file_digest(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    platform::Sha256 hash;
    std::array<char, 1 << 16> buffer{};
    std::uint64_t size = 0;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = in.gcount();
        if (count > 0) {
            hash.update(buffer.data(), static_cast<std::size_t>(count));
            size += static_cast<std::uint64_t>(count);
        }
    }
    if (!in.eof()) return std::nullopt;
    return RunCacheContentDigest{size, hash.finish_hex()};
}

RunCacheRecheck recheck_dependency(const RunCacheDependency& dependency) {
    const auto result = [](bool unchanged) {
        return unchanged ? RunCacheRecheck::unchanged : RunCacheRecheck::changed;
    };
    switch (dependency.kind) {
        case RunCacheDependencyKind::local_import:
        case RunCacheDependencyKind::package:
        case RunCacheDependencyKind::package_tree:
            return RunCacheRecheck::not_checked_here;
        case RunCacheDependencyKind::absent: {
            if (dependency.state == "missing") return result(missing_path(dependency.path));
            std::error_code error;
            const auto status = fs::status(dependency.path, error);
            const bool other = error ? !missing_path(dependency.path)
                                     : status.type() != fs::file_type::not_found &&
                                           status.type() != fs::file_type::regular;
            return result(other);
        }
        case RunCacheDependencyKind::system_file:
            return result(platform::file_identity(dependency.path) ==
                          std::optional(dependency.identity));
        case RunCacheDependencyKind::native_header: {
            const auto digest = run_cache_file_digest(dependency.path);
            return result(digest && digest->sha256 == dependency.sha256);
        }
        default: {
            const auto digest = run_cache_file_digest(dependency.path);
            return result(digest && digest->size == dependency.size &&
                          digest->sha256 == dependency.sha256);
        }
    }
}

} // namespace quidra::toolchain
