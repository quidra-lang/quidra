#include "toolchain/run_cache_key.hpp"

#include <array>

namespace fs = std::filesystem;

namespace quidra::toolchain {
namespace {

constexpr std::array<std::pair<RunCacheDependencyKind, const char*>, 15> kind_names{{
    {RunCacheDependencyKind::source, "source"},
    {RunCacheDependencyKind::manifest, "manifest"},
    {RunCacheDependencyKind::project, "project"},
    {RunCacheDependencyKind::descriptor, "descriptor"},
    {RunCacheDependencyKind::lock, "lock"},
    {RunCacheDependencyKind::absent, "absent"},
    {RunCacheDependencyKind::local_import, "local_import"},
    {RunCacheDependencyKind::package, "package"},
    {RunCacheDependencyKind::package_tree, "package_tree"},
    {RunCacheDependencyKind::native_source, "native_source"},
    {RunCacheDependencyKind::native_library, "native_library"},
    {RunCacheDependencyKind::link_input, "link_input"},
    {RunCacheDependencyKind::runtime_library, "runtime_library"},
    {RunCacheDependencyKind::native_header, "native_header"},
    {RunCacheDependencyKind::system_file, "system_file"},
}};

std::string path_text(const fs::path& path) {
    const auto encoded = path.u8string();
    return std::string(encoded.begin(), encoded.end());
}

void add_identified(KeyDocument& document, std::string_view tag, const IdentifiedFile& file) {
    document.field(tag);
    document.path(file.path);
    document.identity(file.identity);
}

void add_tool(KeyDocument& document, std::string_view tag, const ToolIdentity& tool) {
    add_identified(document, tag, tool.program);
    document.text(tool.version_sha256);
    document.text(tool.version_line);
}

void add_optional_tool(
    KeyDocument& document, std::string_view tag, const std::optional<ToolIdentity>& tool) {
    document.field(tag);
    document.boolean(tool.has_value());
    if (tool) add_tool(document, tag, *tool);
}

void add_paths(KeyDocument& document, std::string_view tag, const std::vector<fs::path>& paths) {
    document.field(tag);
    document.count(paths.size());
    for (const auto& path : paths) document.path(path);
}

void add_texts(
    KeyDocument& document, std::string_view tag, const std::vector<std::string>& texts) {
    document.field(tag);
    document.count(texts.size());
    for (const auto& text : texts) document.text(text);
}

void add_material(KeyDocument& document, const RunCachePreKeyMaterial& material) {
    // Every field of the material, by name: a new field fails to bind here
    // until the key says what it does with it.
    const auto& [compiler_version, abi_version, ir_version, build_id, platform, os_version,
                 driver_requested, driver_path, runtime_library, native_include_directory,
                 entry, working_directory, link_inputs, options, environment] = material;
    document.field("compiler.version");
    document.text(compiler_version);
    document.field("compiler.abi");
    document.integer(abi_version);
    document.field("compiler.ir");
    document.text(ir_version);
    document.field("compiler.build_id");
    document.text(build_id);
    document.field("host.platform");
    document.text(platform);
    document.field("host.os");
    document.text(os_version);
    document.field("driver.requested");
    document.text(driver_requested);
    document.field("driver.path");
    document.path(driver_path);
    document.field("runtime_library");
    document.path(runtime_library);
    document.field("native_include_directory");
    document.optional_path(native_include_directory);
    document.field("invocation.entry");
    document.path(entry);
    document.field("invocation.cwd");
    document.path(working_directory);
    document.field("invocation.link_inputs");
    document.count(link_inputs.size());
    for (const auto& input : link_inputs) {
        document.text(input.given);
        document.path(input.path);
    }
    document.field("invocation.options");
    document.count(options.size());
    for (const auto& option : options) {
        document.text(option.name);
        document.text(option.value);
    }
    document.field("environment");
    document.count(environment.size());
    for (const auto& [name, value] : environment) {
        document.text(name);
        document.optional_text(value);
    }
}

void add_toolchain(KeyDocument& document, const ToolchainSnapshot& snapshot) {
    const auto& driver = snapshot.driver;
    document.field("toolchain.driver.requested");
    document.text(driver.requested);
    add_tool(document, "toolchain.driver.tool", driver.tool);
    add_identified(document, "toolchain.driver.directory", driver.directory);
    add_identified(document, "toolchain.driver.compiler", driver.compiler);
    add_identified(document, "toolchain.driver.compiler_directory", driver.compiler_directory);
    document.field("toolchain.driver.configuration_files");
    document.count(driver.configuration_files.size());
    for (const auto& file : driver.configuration_files) {
        document.path(file.path);
        document.identity(file.identity);
    }
    document.field("toolchain.driver.resource_directory");
    document.boolean(driver.resource_directory.has_value());
    if (driver.resource_directory) {
        add_identified(document, "toolchain.driver.resource_directory", *driver.resource_directory);
    }

    const auto& target = snapshot.target;
    document.field("toolchain.target.triple");
    document.text(target.triple);
    document.field("toolchain.target.cpu");
    document.text(target.cpu);
    add_texts(document, "toolchain.target.features", target.features);
    document.field("toolchain.target.sysroot");
    document.optional_path(target.sysroot);
    document.field("toolchain.target.sdk_version");
    document.text(target.sdk_version);
    document.field("toolchain.target.sdk_settings_sha256");
    document.optional_text(target.sdk_settings_sha256);
    document.field("toolchain.target.developer_directory");
    document.optional_path(target.developer_directory);

    add_optional_tool(document, "toolchain.linker", snapshot.linker);
    document.field("toolchain.library_search");
    document.boolean(snapshot.library_search.has_value());
    if (snapshot.library_search) {
        add_paths(document, "toolchain.library_search.library", snapshot.library_search->library);
        add_paths(document, "toolchain.library_search.framework",
                  snapshot.library_search->framework);
    }
    document.field("toolchain.include_searches");
    document.count(snapshot.include_searches.size());
    for (const auto& search : snapshot.include_searches) {
        add_paths(document, "toolchain.include_search.quote", search.quote);
        add_paths(document, "toolchain.include_search.angle", search.angle);
    }
    add_paths(document, "toolchain.system_directories", snapshot.system_directories);
    add_optional_tool(document, "toolchain.pkg_config", snapshot.pkg_config);
    add_texts(document, "toolchain.pkg_config.modules", snapshot.pkg_config_modules);
    add_texts(document, "toolchain.pkg_config.cflags", snapshot.pkg_config_cflags);
    add_texts(document, "toolchain.pkg_config.libs", snapshot.pkg_config_libs);
    add_optional_tool(document, "toolchain.nvcc", snapshot.nvcc);
    document.field("toolchain.complete");
    document.boolean(snapshot.complete);
    document.text(snapshot.incomplete_reason);
}

void add_dependency(KeyDocument& document, const RunCacheDependency& dependency) {
    document.field(run_cache_dependency_kind_name(dependency.kind));
    switch (dependency.kind) {
        case RunCacheDependencyKind::absent:
            document.path(dependency.path);
            document.text(dependency.state);
            return;
        case RunCacheDependencyKind::local_import:
            document.path(dependency.importer);
            document.text(dependency.target);
            document.path(dependency.path);
            return;
        case RunCacheDependencyKind::package:
            document.text(dependency.name);
            document.path(dependency.path);
            return;
        case RunCacheDependencyKind::package_tree:
        case RunCacheDependencyKind::native_header:
            document.path(dependency.path);
            document.text(dependency.sha256);
            return;
        case RunCacheDependencyKind::system_file:
            document.path(dependency.path);
            document.identity(dependency.identity);
            return;
        default:
            document.path(dependency.path);
            document.unsigned_integer(dependency.size);
            document.text(dependency.sha256);
            return;
    }
}

} // namespace

bool run_cache_content_kind(RunCacheDependencyKind kind) {
    switch (kind) {
        case RunCacheDependencyKind::source:
        case RunCacheDependencyKind::manifest:
        case RunCacheDependencyKind::project:
        case RunCacheDependencyKind::descriptor:
        case RunCacheDependencyKind::lock:
        case RunCacheDependencyKind::native_source:
        case RunCacheDependencyKind::native_library:
        case RunCacheDependencyKind::link_input:
        case RunCacheDependencyKind::runtime_library:
            return true;
        default:
            return false;
    }
}

const char* run_cache_dependency_kind_name(RunCacheDependencyKind kind) {
    for (const auto& [value, name] : kind_names) {
        if (value == kind) return name;
    }
    return "source";
}

std::optional<RunCacheDependencyKind> run_cache_dependency_kind(std::string_view name) {
    for (const auto& [value, text] : kind_names) {
        if (name == text) return value;
    }
    return std::nullopt;
}

KeyDocument::KeyDocument(std::string_view schema) { value('S', schema); }

void KeyDocument::value(char type, std::string_view bytes) {
    std::array<std::uint8_t, 9> header{};
    header[0] = static_cast<std::uint8_t>(type);
    auto size = static_cast<std::uint64_t>(bytes.size());
    for (int index = 8; index >= 1; --index) {
        header[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(size & 0xffU);
        size >>= 8U;
    }
    hash_.update(header.data(), header.size());
    hash_.update(bytes);
}

void KeyDocument::field(std::string_view tag) { value('F', tag); }

void KeyDocument::text(std::string_view text_value) { value('T', text_value); }

void KeyDocument::integer(std::int64_t number) { value('I', std::to_string(number)); }

void KeyDocument::unsigned_integer(std::uint64_t number) {
    value('U', std::to_string(number));
}

void KeyDocument::boolean(bool flag) { value('B', flag ? "1" : "0"); }

void KeyDocument::path(const fs::path& path_value) { value('P', path_text(path_value)); }

void KeyDocument::optional_text(const std::optional<std::string>& text_value) {
    if (text_value) value('T', *text_value);
    else value('N', {});
}

void KeyDocument::optional_path(const std::optional<fs::path>& path_value) {
    if (path_value) path(*path_value);
    else value('N', {});
}

void KeyDocument::identity(const platform::FileIdentity& file) {
    value('D', std::to_string(file.device) + ":" + std::to_string(file.file) + ":" +
                   std::to_string(file.file_high) + ":" + std::to_string(file.size) + ":" +
                   std::to_string(file.modified_ns) + ":" + std::to_string(file.changed_ns));
}

void KeyDocument::count(std::size_t number) { value('C', std::to_string(number)); }

std::string KeyDocument::finish() { return hash_.finish_hex(); }

std::string run_cache_pre_key(const RunCachePreKeyMaterial& material) {
    KeyDocument document(run_cache_key_schema);
    document.field("pre-key");
    add_material(document, material);
    return document.finish();
}

std::string run_cache_key(
    const RunCachePreKeyMaterial& material, const ToolchainSnapshot& toolchain,
    std::string_view recipe_sha256, const std::vector<RunCacheDependency>& dependencies) {
    KeyDocument document(run_cache_key_schema);
    document.field("key");
    add_material(document, material);
    add_toolchain(document, toolchain);
    document.field("recipe");
    document.text(recipe_sha256);
    document.field("dependencies");
    document.count(dependencies.size());
    for (const auto& dependency : dependencies) add_dependency(document, dependency);
    return document.finish();
}

bool run_cache_digest_text(std::string_view text) {
    if (text.size() != 64) return false;
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

} // namespace quidra::toolchain
