#include "run_cache.hpp"

#include "native_build.hpp"
#include "run_artifact.hpp"
#include "run_cache_options.hpp"

#include "quidra/compile_inputs.hpp"
#include "quidra/compiler.hpp"
#include "quidra/import_path.hpp"
#include "quidra/package_lock.hpp"
#include "quidra/package_manifest.hpp"
#include "quidra/project.hpp"

#include "platform/cache_files.hpp"
#include "platform/executable.hpp"
#include "platform/file_identity.hpp"
#include "platform/os_version.hpp"
#include "platform/private_directory.hpp"
#include "platform/process.hpp"
#include "platform/sha256.hpp"
#include "platform/user_cache_directory.hpp"

#include "toolchain/compile_environment.hpp"
#include "toolchain/compiler_identity.hpp"
#include "toolchain/installation_layout.hpp"
#include "toolchain/llvm_discovery.hpp"
#include "toolchain/native_link_recipe.hpp"
#include "toolchain/run_cache_eviction.hpp"
#include "toolchain/run_cache_key.hpp"
#include "toolchain/run_cache_metadata.hpp"
#include "toolchain/run_cache_store.hpp"
#include "toolchain/toolchain_identity.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace fs = std::filesystem;

namespace quidra::run_cache {
namespace {

using namespace toolchain;

constexpr std::size_t metadata_limit = 64U * 1024U * 1024U;
constexpr std::size_t build_log_limit = 64U * 1024U * 1024U;
// A file that changed this close before the build started may keep an
// identity it no longer deserves (the file system's time resolution).
constexpr std::int64_t racy_window_ns = 2'000'000'000;

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string path_text(const fs::path& path) {
    const auto encoded = path.u8string();
    return std::string(encoded.begin(), encoded.end());
}

CompileOptions compile_options(const DirectRun& run) {
    CompileOptions options{run.max_errors, false};
    options.source_display_path = path_text(run.source);
    return options;
}

// What a run with this compiler, toolchain, entry and environment looks up.
struct Context {
    RunCacheStore store;
    RunCachePreKeyMaterial material;
    std::string pre_key;
    fs::path working_directory;
};

std::optional<Context> prepare(const DirectRun& run) {
    try {
        const auto root = platform::user_cache_directory();
        if (!root) return std::nullopt;
        const auto target = package_host_platform();
        if (!target) return std::nullopt;
        auto store = RunCacheStore::open(*root, std::string(version), *target);
        if (!store) return std::nullopt;

        RunCachePreKeyMaterial material;
        const auto compiler = compiler_identity(
            CompilerVersions{std::string(version), abi_version, std::string(ir_version)},
            store->layout().root);
        if (!compiler) return std::nullopt;
        material.compiler_version = compiler->versions.version;
        material.abi_version = compiler->versions.abi;
        material.ir_version = compiler->versions.ir;
        material.build_id = compiler->build_id;
        material.platform = *target;
        material.os_version = platform::os_version();
        material.driver_requested = clang_driver();
        const auto driver = platform::command_path(material.driver_requested);
        if (!driver) return std::nullopt;
        material.driver_path = *driver;
        material.runtime_library = runtime_library();
        material.native_include_directory = native_extension_include_directory();

        const auto working_directory = fs::absolute(fs::current_path()).lexically_normal();
        material.entry = (run.source.is_absolute() ? run.source : working_directory / run.source)
                             .lexically_normal();
        material.working_directory = working_directory;
        for (const auto& input : run.link_inputs) {
            material.link_inputs.push_back(
                RunCacheLinkInput{path_text(input), fs::absolute(input).lexically_normal()});
        }
        material.options = cache_key_options(compile_options(run), LinkOptions{false, true, {}, {}});
        // The entry as the command spelled it.
        material.options.push_back(RunCacheOption{"invocation.source", path_text(run.source)});
        for (const auto& [name, value] : compile_environment_values()) {
            material.environment.emplace_back(std::string(name), value);
        }
        auto pre_key = run_cache_pre_key(material);
        return Context{std::move(*store), std::move(material), std::move(pre_key), working_directory};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool dependency_holds(const RunCacheDependency& dependency, const fs::path& working_directory) {
    switch (recheck_dependency(dependency)) {
        case RunCacheRecheck::unchanged: return true;
        case RunCacheRecheck::changed: return false;
        case RunCacheRecheck::not_checked_here: break;
    }
    try {
        switch (dependency.kind) {
            case RunCacheDependencyKind::local_import:
                return resolve_local_import_path(
                           dependency.target, dependency.importer, working_directory)
                           .path == dependency.path;
            case RunCacheDependencyKind::package:
                return resolve_installed_package_path(dependency.name) ==
                       std::optional(dependency.path);
            case RunCacheDependencyKind::package_tree:
                return package_tree_sha256(dependency.path) == dependency.sha256;
            default:
                return false;
        }
    } catch (const std::exception&) {
        return false;
    }
}

enum class Verdict { hit, stale, corrupt };

struct Hit {
    fs::path program;
    std::string build_log;
};

Verdict verify(const Context& context, const std::string& key, const fs::path& directory, Hit& hit) {
    const auto text = platform::read_private_file(
        directory / std::string(run_cache_metadata_file), metadata_limit);
    if (!text) return Verdict::corrupt;
    RunCacheEntry entry;
    try {
        entry = read_run_cache_metadata(*text);
    } catch (const std::exception&) {
        return Verdict::corrupt;
    }
    if (entry.key != key || run_cache_pre_key(entry.material) != entry.pre_key ||
        entry.program.file != run_cache_program_file() ||
        entry.toolchain_output.file != run_cache_build_log_file ||
        run_cache_key(entry.material, entry.toolchain, entry.recipe_sha256, entry.dependencies) !=
            key) {
        return Verdict::corrupt;
    }
    if (entry.pre_key != context.pre_key || !(entry.material == context.material)) {
        return Verdict::stale;
    }
    if (!toolchain_unchanged(entry.toolchain)) return Verdict::stale;
    for (const auto& dependency : entry.dependencies) {
        if (!dependency_holds(dependency, context.working_directory)) return Verdict::stale;
    }
    const auto program = directory / entry.program.file;
    const auto digest = platform::private_file_digest(program);
    if (!digest || digest->size != entry.program.size || digest->sha256 != entry.program.sha256) {
        return Verdict::corrupt;
    }
    auto log = platform::read_private_file(directory / entry.toolchain_output.file, build_log_limit);
    if (!log || log->size() != entry.toolchain_output.size ||
        platform::sha256_hex(*log) != entry.toolchain_output.sha256) {
        return Verdict::corrupt;
    }
    hit = Hit{program, std::move(*log)};
    return Verdict::hit;
}

RunCacheDependency record_of(const CompileInput& input) {
    RunCacheDependency dependency;
    std::visit(
        [&](const auto& record) {
            using Record = std::decay_t<decltype(record)>;
            if constexpr (std::is_same_v<Record, InputFile>) {
                switch (record.kind) {
                    case InputFileKind::source: dependency.kind = RunCacheDependencyKind::source; break;
                    case InputFileKind::manifest: dependency.kind = RunCacheDependencyKind::manifest; break;
                    case InputFileKind::project: dependency.kind = RunCacheDependencyKind::project; break;
                    case InputFileKind::descriptor:
                        dependency.kind = RunCacheDependencyKind::descriptor;
                        break;
                    case InputFileKind::lock: dependency.kind = RunCacheDependencyKind::lock; break;
                }
                dependency.path = record.path;
                dependency.size = record.size;
                dependency.sha256 = record.sha256;
            } else if constexpr (std::is_same_v<Record, AbsentInput>) {
                dependency.kind = RunCacheDependencyKind::absent;
                dependency.path = record.path;
                dependency.state = input_path_state_name(record.state);
            } else if constexpr (std::is_same_v<Record, LocalImportInput>) {
                dependency.kind = RunCacheDependencyKind::local_import;
                dependency.importer = record.importer;
                dependency.target = record.target;
                dependency.path = record.path;
            } else if constexpr (std::is_same_v<Record, PackageInput>) {
                dependency.kind = RunCacheDependencyKind::package;
                dependency.name = record.name;
                dependency.path = record.main;
            } else {
                dependency.kind = RunCacheDependencyKind::package_tree;
                dependency.path = record.main;
                dependency.sha256 = record.sha256;
            }
        },
        input);
    return dependency;
}

// Whether a native source or header asks for the time of its build, which
// makes every build of it different (as ccache, never cached).
bool uses_build_time(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return true;
    std::ostringstream text;
    text << in.rdbuf();
    const auto content = text.str();
    for (const std::string_view macro : {"__DATE__", "__TIME__", "__TIMESTAMP__"}) {
        if (content.find(macro) != std::string::npos) return true;
    }
    return false;
}

struct NativeFile {
    fs::path path;
    RunCacheDependencyKind kind;
    std::optional<RunCacheContentDigest> before;
};

// The entry a finished build may publish, or nullopt when something it read
// cannot be described or may have changed while it built.
std::optional<RunCacheEntry> describe_build(
    const Context& context, const CompileInputs& inputs, const NativeLinkRecipe& recipe,
    const std::vector<NativeFile>& native_files, const std::vector<std::string>& pkg_config_modules,
    const fs::path& llvm, std::int64_t build_start_ns) {
    if (!inputs.complete()) return std::nullopt;
    const auto limit = build_start_ns - racy_window_ns;
    const auto recent = [&](const platform::FileIdentity& identity) {
        return identity.modified_ns >= limit || identity.changed_ns >= limit;
    };
    const auto recently_changed = [&](const fs::path& path) {
        const auto identity = platform::file_identity(path);
        return !identity || recent(*identity);
    };
    // The racy-input rule for the files the build itself read comes first:
    // a build of files just written, as after every edit, is never
    // published, so it need not describe the toolchain at all.
    for (const auto& record : inputs.records()) {
        if (const auto* file = std::get_if<InputFile>(&record);
            file && recently_changed(file->path)) {
            return std::nullopt;
        }
    }
    for (const auto& file : native_files) {
        if (recently_changed(file.path)) return std::nullopt;
    }
    auto snapshot = snapshot_toolchain(recipe, pkg_config_modules);
    if (!snapshot.complete) return std::nullopt;
    std::vector<fs::path> by_content;
    for (const auto& file : native_files) by_content.push_back(file.path);
    by_content.push_back(context.material.runtime_library);
    by_content.push_back(llvm);
    bool complete = false;
    std::string reason;
    auto toolchain_records =
        toolchain_dependencies(snapshot, recipe, by_content, complete, reason);
    if (!complete) return std::nullopt;
    // The linker loads libraries in parallel, so its trace lists them in an
    // order that varies between two links of the same program. The records,
    // one per path, enter the key in path order.
    std::sort(toolchain_records.begin(), toolchain_records.end(),
              [](const ToolchainDependency& left, const ToolchainDependency& right) {
                  return left.path < right.path;
              });

    RunCacheEntry entry;
    entry.material = context.material;
    entry.pre_key = context.pre_key;
    entry.toolchain = std::move(snapshot);
    entry.recipe_sha256 = platform::sha256_hex(recipe.template_text());
    for (const auto& record : inputs.records()) entry.dependencies.push_back(record_of(record));
    for (const auto& file : native_files) {
        const auto after = run_cache_file_digest(file.path);
        if (!file.before || !after || after->size != file.before->size ||
            after->sha256 != file.before->sha256) {
            return std::nullopt;
        }
        if (file.kind == RunCacheDependencyKind::native_source && uses_build_time(file.path)) {
            return std::nullopt;
        }
        RunCacheDependency dependency;
        dependency.kind = file.kind;
        dependency.path = fs::absolute(file.path).lexically_normal();
        dependency.size = after->size;
        dependency.sha256 = after->sha256;
        entry.dependencies.push_back(std::move(dependency));
    }
    {
        const auto digest = run_cache_file_digest(context.material.runtime_library);
        if (!digest) return std::nullopt;
        RunCacheDependency dependency;
        dependency.kind = RunCacheDependencyKind::runtime_library;
        dependency.path = context.material.runtime_library;
        dependency.size = digest->size;
        dependency.sha256 = digest->sha256;
        entry.dependencies.push_back(std::move(dependency));
    }
    for (const auto& record : toolchain_records) {
        RunCacheDependency dependency;
        dependency.path = record.path;
        switch (record.kind) {
            case ToolchainDependency::Kind::header:
                if (uses_build_time(record.path)) return std::nullopt;
                dependency.kind = RunCacheDependencyKind::native_header;
                dependency.sha256 = record.sha256;
                break;
            case ToolchainDependency::Kind::system_file:
                dependency.kind = RunCacheDependencyKind::system_file;
                dependency.identity = record.identity;
                break;
            case ToolchainDependency::Kind::absent:
                dependency.kind = RunCacheDependencyKind::absent;
                dependency.state = "missing";
                break;
        }
        entry.dependencies.push_back(std::move(dependency));
    }

    // The racy-input rule for every dependency.
    for (const auto& dependency : entry.dependencies) {
        if (dependency.kind == RunCacheDependencyKind::system_file) {
            if (recent(dependency.identity)) return std::nullopt;
        } else if (run_cache_content_kind(dependency.kind) ||
                   dependency.kind == RunCacheDependencyKind::native_header) {
            if (recently_changed(dependency.path)) return std::nullopt;
        }
    }
    const auto& toolchain = entry.toolchain;
    std::vector<const IdentifiedFile*> identified{
        &toolchain.driver.tool.program, &toolchain.driver.directory, &toolchain.driver.compiler,
        &toolchain.driver.compiler_directory};
    for (const auto& file : toolchain.driver.configuration_files) identified.push_back(&file);
    if (toolchain.driver.resource_directory) identified.push_back(&*toolchain.driver.resource_directory);
    for (const auto* tool : {&toolchain.linker, &toolchain.pkg_config, &toolchain.nvcc}) {
        if (*tool) identified.push_back(&(*tool)->program);
    }
    for (const auto* file : identified) {
        if (recent(file->identity)) return std::nullopt;
    }
    return entry;
}

// Fills the staging directory with the entry's own files and publishes it.
// Returns the program to run: the published one, or the staging one when the
// entry is not published.
fs::path publish(Context& context, RunCacheStaging& staging, RunCacheEntry& entry,
                 const fs::path& program, const fs::path& llvm, const fs::path& work,
                 std::chrono::steady_clock::time_point wall_start) {
    const auto& directory = staging.directory();
    std::error_code ignored;
    fs::remove(llvm, ignored);
    fs::remove_all(work, ignored);
    const auto log = directory / std::string(run_cache_build_log_file);
#ifndef _WIN32
    fs::permissions(program, fs::perms::owner_all, fs::perm_options::replace, ignored);
    if (ignored) return program;
    fs::permissions(log, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ignored);
    if (ignored) return program;
#endif
    const auto program_digest = platform::private_file_digest(program);
    const auto log_text = platform::read_private_file(log, build_log_limit);
    if (!program_digest || !log_text) return program;
    entry.program = RunCacheStoredFile{run_cache_program_file(), program_digest->size,
                                       program_digest->sha256};
    entry.toolchain_output = RunCacheStoredFile{std::string(run_cache_build_log_file),
                                                log_text->size(), platform::sha256_hex(*log_text)};
    entry.created_unix_ns = now_ns();
    entry.build_wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - wall_start)
                              .count();
    entry.key = run_cache_key(entry.material, entry.toolchain, entry.recipe_sha256, entry.dependencies);
    if (!platform::replace_private_file(
            directory / std::string(run_cache_metadata_file), write_run_cache_metadata(entry)) ||
        !platform::replace_private_file(directory / std::string(run_cache_used_file), "")) {
        return program;
    }
    switch (context.store.publish(staging, entry.key)) {
        case RunCachePublishOutcome::published: {
            (void)context.store.add_to_index(context.pre_key, entry.key);
            const auto published = context.store.layout().entry(entry.key);
            // The miss is slow already; a due scan runs before the program.
            evict_run_cache_if_due(context.store, run_cache_entry_size(published));
            return published / entry.program.file;
        }
        case RunCachePublishOutcome::already_present:
            (void)context.store.add_to_index(context.pre_key, entry.key);
            return program;
        case RunCachePublishOutcome::failed:
            return program;
    }
    return program;
}

// The status of a program that ran; a program that could not be started
// reports what an uncached run reports.
int finished(const platform::ProgramRun& run) {
#ifdef _WIN32
    if (!run.started) throw std::runtime_error(run.failure);
#endif
    return run.status;
}

int build_and_run(Context& context, const DirectRun& run, const std::function<int()>& uncached) {
    std::optional<run_artifact::TemporaryArtifact> artifact;
    artifact.emplace(run.source);
    auto staging = context.store.create_staging();
    if (!staging) {
        artifact.reset();
        return uncached();
    }
    const auto wall_start = std::chrono::steady_clock::now();
    const auto directory = staging->directory();
    // The build's start in the file system's own time, which the racy-input
    // rule compares the dependencies' times with.
    const auto started = directory / "started";
    std::optional<std::int64_t> build_start_ns;
    if (platform::replace_private_file(started, "")) {
        if (const auto identity = platform::file_identity(started)) {
            build_start_ns = std::max(identity->modified_ns, identity->changed_ns);
        }
    }
    const auto llvm = directory / "program.ll";
    const auto program = directory / run_cache_program_file();
    const auto work = directory / "work";
    const auto log = directory / std::string(run_cache_build_log_file);

    CompileInputs inputs;
    const auto compilation =
        compile_file(run.source, compile_options(run), fs::current_path(), &inputs);
    {
        std::ofstream out(llvm, std::ios::binary);
        out << compilation.llvm;
        out.close();
        const auto work_directory = platform::open_private_subdirectory(
            context.store.layout().root, fs::path("staging") / staging->id() / "work");
        if (!out || !work_directory.path) {
            staging.reset();
            artifact.reset();
            return uncached();
        }
    }

    const auto package_inputs = native::package_native_build_inputs(compilation.packages, &inputs);
    std::vector<NativeFile> native_files;
    std::vector<fs::path> files;
    for (const auto& input : package_inputs.inputs) {
        native_files.push_back(NativeFile{
            input.path,
            input.source ? RunCacheDependencyKind::native_source
                         : RunCacheDependencyKind::native_library,
            std::nullopt});
        files.push_back(input.path);
    }
    for (const auto& input : run.link_inputs) {
        native_files.push_back(NativeFile{input, RunCacheDependencyKind::link_input, std::nullopt});
        files.push_back(input);
    }
    const auto recipe = native_link_recipe(
        llvm, program, LinkOptions{false, true, files, package_inputs.pkg_config_modules},
        LinkRecording{work, log});
    for (auto& file : native_files) file.before = run_cache_file_digest(file.path);
    if (recipe.execute() != 0) return 1;

    std::optional<RunCacheEntry> entry;
    if (build_start_ns) {
        try {
            entry = describe_build(context, inputs, recipe, native_files,
                                   package_inputs.pkg_config_modules, llvm, *build_start_ns);
        } catch (const std::exception&) {
            entry.reset();
        }
    }
    {
        std::error_code ignored;
        fs::remove(started, ignored);
    }
    auto runnable = program;
    if (entry) {
        try {
            runnable = publish(context, *staging, *entry, program, llvm, work, wall_start);
        } catch (const std::exception&) {
            runnable = program;
        }
    }

    const auto executable = artifact->executable();
    if (!platform::link_or_copy_file(runnable, executable)) {
        // Nothing can run beside the source from here; the uncached path
        // reports why exactly as it always has.
        staging.reset();
        artifact.reset();
        return uncached();
    }
    // The program must not hold the staging lease.
    staging.reset();
    return finished(platform::run_program_reporting_start(executable, run.program_arguments));
}

std::string mebibytes(std::uint64_t bytes) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    out << static_cast<double>(bytes) / (1024.0 * 1024.0);
    return out.str();
}

} // namespace

int clean_cache(std::ostream& out, std::ostream& err) {
    const auto root = platform::user_cache_directory();
    if (!root) {
        err << "quidra: cannot clean the run cache: there is no user cache directory "
               "(set QUIDRA_CACHE_DIR, or HOME to an absolute path)\n";
        return 1;
    }
    std::error_code error;
    const auto status = fs::symlink_status(*root, error);
    if (status.type() == fs::file_type::not_found) {
        out << "removed 0 entries (0.0 MiB)\n";
        return 0;
    }
    std::string refusal;
    const auto store = RunCacheStore::open(
        *root, std::string(version), package_host_platform().value_or("unknown"), &refusal);
    if (!store) {
        err << "quidra: cannot clean the run cache: " << refusal << "\n";
        return 1;
    }
    const auto clean = clean_run_cache(*store);
    out << "removed " << clean.removed.entries
        << (clean.removed.entries == 1 ? " entry (" : " entries (")
        << mebibytes(clean.removed.bytes) << " MiB)\n";
    if (clean.files_left != 0) {
        out << clean.files_left << (clean.files_left == 1 ? " file" : " files")
            << " in use " << (clean.files_left == 1 ? "was" : "were")
            << " left; they are removed later\n";
    }
    return 0;
}

int run_cached(const DirectRun& run, const std::function<int()>& uncached) {
    auto context = prepare(run);
    if (!context) return uncached();
    for (const auto& key : context->store.index_keys(context->pre_key)) {
        const auto directory = context->store.entry_directory(key);
        if (!directory) continue;
        Hit hit;
        Verdict verdict = Verdict::stale;
        try {
            verdict = verify(*context, key, *directory, hit);
        } catch (const std::exception&) {
            verdict = Verdict::stale;
        }
        if (verdict == Verdict::corrupt) {
            context->store.discard(key);
            continue;
        }
        if (verdict == Verdict::stale) continue;

        std::optional<run_artifact::TemporaryArtifact> artifact;
        artifact.emplace(run.source);
        if (!platform::link_or_copy_file(hit.program, artifact->executable())) {
            artifact.reset();
            return uncached();
        }
        (void)platform::touch_file(*directory / std::string(run_cache_used_file));
        if (!hit.build_log.empty()) (void)platform::write_standard_error(hit.build_log);
        const auto result =
            platform::run_program_reporting_start(artifact->executable(), run.program_arguments);
        if (result.started) return result.status;
        // A cached program that cannot be started is discarded and built
        // once more.
        artifact.reset();
        context->store.discard(key);
        break;
    }
    return build_and_run(*context, run, uncached);
}

} // namespace quidra::run_cache
