#include "toolchain/toolchain_identity.hpp"

#include "platform/environment.hpp"
#include "platform/executable.hpp"
#include "platform/native_text.hpp"
#include "platform/process.hpp"
#include "platform/sha256.hpp"
#include "platform/temporary_directory.hpp"
#include "toolchain/llvm_discovery.hpp"
#include "toolchain/native_link_recipe.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace quidra::toolchain {
namespace {

using platform::NativeText;

#ifdef __APPLE__
constexpr bool apple_host = true;
#else
constexpr bool apple_host = false;
#endif

std::string utf8(const NativeText& text) {
    const auto encoded = fs::path(text).u8string();
    return std::string(encoded.begin(), encoded.end());
}

std::optional<std::string> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

std::string first_line(std::string_view text) {
    const auto end = text.find('\n');
    auto line = text.substr(0, end);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    return std::string(line);
}

bool path_missing(const fs::path& path) {
    std::error_code error;
    const auto status = fs::status(path, error);
    if (error) {
        return error == std::errc::no_such_file_or_directory ||
               error == std::errc::not_a_directory;
    }
    return status.type() == fs::file_type::not_found;
}

std::optional<IdentifiedFile> identified(const fs::path& path) {
    const auto identity = platform::file_identity(path);
    if (!identity) return std::nullopt;
    return IdentifiedFile{path, *identity};
}

bool still_identified(const IdentifiedFile& file) {
    return platform::file_identity(file.path) == std::optional(file.identity);
}

bool under(const fs::path& directory, const fs::path& path) {
    if (directory.empty()) return false;
    const auto relative = path.lexically_relative(directory);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

fs::path normalized(const fs::path& path) {
    std::error_code error;
    const auto absolute = fs::absolute(path, error);
    return (error ? path : absolute).lexically_normal();
}

struct ProbeResult {
    int status{1};
    std::string out;
    std::string err;
};

// Runs one probe with its stdout and stderr captured in a scratch
// directory.
class Prober {
public:
    Prober()
        : scratch_("quidra-toolchain-probe-",
                   "cannot create a temporary directory for toolchain probes") {}

    ProbeResult run(const fs::path& program, const std::vector<NativeText>& arguments) {
        const auto stem = "probe-" + std::to_string(count_++);
        const auto out = scratch_.path() / (stem + ".out");
        const auto err = scratch_.path() / (stem + ".err");
        ProbeResult result;
        try {
            result.status = platform::run_native_program(program, arguments, out, err);
        } catch (const std::exception&) {
            result.status = 1;
        }
        result.out = read_bytes(out).value_or("");
        result.err = read_bytes(err).value_or("");
        std::error_code ignored;
        fs::remove(out, ignored);
        fs::remove(err, ignored);
        return result;
    }

private:
    platform::TemporaryDirectory scratch_;
    unsigned count_{0};
};

std::vector<NativeText> native_arguments(std::initializer_list<std::string_view> arguments) {
    std::vector<NativeText> result;
    for (const auto argument : arguments) result.push_back(platform::native_text(argument));
    return result;
}

std::optional<ToolIdentity> tool_identity(Prober& prober, const fs::path& program,
                                          std::string_view version_option) {
    auto file = identified(program);
    if (!file) return std::nullopt;
    const auto version = prober.run(program, native_arguments({version_option}));
    ToolIdentity tool;
    tool.program = std::move(*file);
    // Both streams: ld64 and GNU ld print their version on stderr.
    tool.version_sha256 = platform::sha256_hex(version.out + '\0' + version.err);
    tool.version_line = first_line(version.out.empty() ? version.err : version.out);
    return tool;
}

// The pkg-config program the recipe runs: QUIDRA_PKG_CONFIG, else
// pkg-config on PATH, or the file the variable names.
std::optional<fs::path> pkg_config_program() {
    const auto configured = platform::environment_value("QUIDRA_PKG_CONFIG");
    const std::string program =
        configured && !configured->empty() ? *configured : "pkg-config";
    if (const auto found = platform::command_path(program)) return *found;
    std::error_code error;
    if (fs::is_regular_file(fs::path(program), error)) return normalized(fs::path(program));
    return std::nullopt;
}

#ifndef __APPLE__
// GNU ld's built-in search directories, from the SEARCH_DIR commands of
// its default linker script (`ld --verbose`); "=" stands for the sysroot.
std::vector<fs::path> linker_script_search_directories(
    std::string_view text, const std::string& sysroot) {
    std::vector<fs::path> directories;
    constexpr std::string_view command = "SEARCH_DIR(\"";
    std::size_t position = 0;
    while ((position = text.find(command, position)) != std::string_view::npos) {
        position += command.size();
        const auto end = text.find("\")", position);
        if (end == std::string_view::npos) break;
        auto directory = std::string(text.substr(position, end - position));
        if (!directory.empty() && directory.front() == '=') {
            directory = sysroot + directory.substr(1);
        }
        directories.push_back(fs::path(directory).lexically_normal());
        position = end;
    }
    return directories;
}
#endif

// The library search lists of the link job `link`.
std::optional<LibrarySearch> library_search(
    Prober& prober, const fs::path& linker, const std::vector<std::string>& link) {
#ifdef __APPLE__
    // ld64 prints the lists with -v once it has a library to look for.
    std::vector<NativeText> arguments{platform::native_text("-v")};
    for (std::size_t index = 1; index < link.size(); ++index) {
        const std::string_view argument = link[index];
        std::size_t values = 0;
        if (argument == "-syslibroot" || argument == "-arch") values = 1;
        else if (argument == "-platform_version") values = 3;
        else if (argument == "-Z" || argument.substr(0, 2) == "-L" ||
                 argument.substr(0, 2) == "-F") {
            values = (argument == "-L" || argument == "-F") ? 1 : 0;
        } else {
            continue;
        }
        if (index + values >= link.size()) break;
        for (std::size_t offset = 0; offset <= values; ++offset) {
            arguments.push_back(platform::native_text(link[index + offset]));
        }
        index += values;
    }
    for (const auto* argument : {"-dylib", "-o", "/dev/null", "-lSystem"}) {
        arguments.push_back(platform::native_text(argument));
    }
    const auto probe = prober.run(linker, arguments);
    return parse_linker_search(probe.out + probe.err);
#else
    LibrarySearch search;
    for (std::size_t index = 1; index < link.size(); ++index) {
        const std::string_view argument = link[index];
        if (argument == "-L" && index + 1 < link.size()) {
            search.library.push_back(fs::path(link[++index]).lexically_normal());
        } else if (argument.substr(0, 2) == "-L" && argument.size() > 2) {
            search.library.push_back(fs::path(std::string(argument.substr(2))).lexically_normal());
        }
    }
    std::string sysroot;
    if (const auto value = job_option(link, "--sysroot=")) sysroot = *value;
    const auto verbose = prober.run(linker, native_arguments({"--verbose"}));
    for (auto& directory : linker_script_search_directories(verbose.out, sysroot)) {
        search.library.push_back(std::move(directory));
    }
    return search;
#endif
}

// The directories a compile job reads system and SDK headers from: the
// sysroot, the resource directory and the include and framework
// directories the driver adds on its own.
void add_system_directories(std::vector<fs::path>& directories,
                            const std::vector<std::string>& job) {
    const auto add = [&](const std::string& directory) {
        auto path = fs::path(directory).lexically_normal();
        if (std::find(directories.begin(), directories.end(), path) == directories.end()) {
            directories.push_back(std::move(path));
        }
    };
    for (const auto* option : {"-isysroot", "-resource-dir"}) {
        if (const auto value = job_option(job, option)) add(*value);
    }
    for (const auto* option :
         {"-internal-isystem", "-internal-externc-isystem", "-internal-iframework",
          "-iframework"}) {
        for (const auto& directory : job_options(job, option)) add(directory);
    }
}

} // namespace

ToolchainSnapshot snapshot_toolchain(
    const NativeLinkRecipe& recipe, const std::vector<std::string>& pkg_config_modules) {
    ToolchainSnapshot snapshot;
    const auto fail = [&](std::string reason) {
        if (snapshot.complete) {
            snapshot.complete = false;
            snapshot.incomplete_reason = std::move(reason);
        }
    };
    const NativeStep* link = nullptr;
    for (const auto& step : recipe.steps()) {
        if (step.kind == NativeStep::Kind::link) link = &step;
    }
    if (!link) {
        fail("the native link recipe has no link step");
        return snapshot;
    }
#ifdef _WIN32
    fail("a Windows link records no trace of the files it opens");
    return snapshot;
#else
    snapshot.driver.requested = link->program.string();
    const auto driver = platform::command_path(snapshot.driver.requested);
    if (!driver) {
        fail("the C++ driver does not resolve: " + snapshot.driver.requested);
        return snapshot;
    }
    Prober prober;
    auto driver_tool = tool_identity(prober, *driver, "--version");
    auto driver_directory = identified(driver->parent_path());
    if (!driver_tool || !driver_directory) {
        fail("the C++ driver cannot be identified: " + driver->string());
        return snapshot;
    }
    snapshot.driver.tool = std::move(*driver_tool);
    snapshot.driver.directory = std::move(*driver_directory);

    std::vector<NativeText> plan_arguments{platform::native_text("-###")};
    plan_arguments.insert(plan_arguments.end(), link->arguments.begin(), link->arguments.end());
    const auto plan_probe = prober.run(*driver, plan_arguments);
    const auto plan = parse_driver_plan(plan_probe.err + plan_probe.out);
    const std::vector<std::string>* compile_job = nullptr;
    const std::vector<std::string>* link_job = nullptr;
    for (const auto& job : plan.jobs) {
        const bool compiles = job.size() > 1 && (job[1] == "-cc1" || job[1] == "-cc1as");
        if (compiles && !compile_job) compile_job = &job;
        if (!compiles) link_job = &job;
    }
    if (plan_probe.status != 0 || !compile_job || !link_job) {
        fail("the C++ driver's plan (-###) names no compile and link job");
        return snapshot;
    }

    const fs::path compiler((*compile_job)[0]);
    auto compiler_file = identified(compiler);
    auto compiler_directory = identified(compiler.parent_path());
    if (!compiler_file || !compiler_directory) {
        fail("the compiler the driver runs cannot be identified: " + compiler.string());
        return snapshot;
    }
    snapshot.driver.compiler = std::move(*compiler_file);
    snapshot.driver.compiler_directory = std::move(*compiler_directory);
    for (const auto& configuration : plan.configuration_files) {
        auto file = identified(configuration);
        if (!file) {
            fail("a configuration file of the driver cannot be identified: " +
                 configuration.string());
            return snapshot;
        }
        snapshot.driver.configuration_files.push_back(std::move(*file));
    }
    if (const auto resource = job_option(*compile_job, "-resource-dir")) {
        auto directory = identified(fs::path(*resource));
        if (!directory) {
            fail("the driver's resource directory cannot be identified: " + *resource);
            return snapshot;
        }
        snapshot.driver.resource_directory = std::move(*directory);
    }

    auto& target = snapshot.target;
    target.triple = job_option(*compile_job, "-triple").value_or("");
    target.cpu = job_option(*compile_job, "-target-cpu").value_or("");
    target.features = job_options(*compile_job, "-target-feature");
    target.sdk_version = job_option(*compile_job, "-target-sdk-version=").value_or("");
    if (const auto sysroot = job_option(*compile_job, "-isysroot")) {
        target.sysroot = fs::path(*sysroot).lexically_normal();
        if (const auto settings = read_bytes(*target.sysroot / "SDKSettings.json")) {
            target.sdk_settings_sha256 = platform::sha256_hex(*settings);
        }
    }
    if constexpr (apple_host) {
        std::error_code error;
        const auto developer = fs::read_symlink("/var/db/xcode_select_link", error);
        if (!error) target.developer_directory = developer;
    }
    add_system_directories(snapshot.system_directories, *compile_job);

    const fs::path linker((*link_job)[0]);
    snapshot.linker = tool_identity(prober, linker, "-v");
    if (!snapshot.linker) {
        fail("the linker cannot be identified: " + linker.string());
        return snapshot;
    }
    snapshot.library_search = library_search(prober, linker, *link_job);
    if (!snapshot.library_search) {
        fail("the linker's library search lists are unknown");
        return snapshot;
    }

    std::vector<std::pair<std::vector<NativeText>, IncludeSearch>> probed;
    for (const auto& step : recipe.steps()) {
        if (step.kind != NativeStep::Kind::compile) continue;
        if (cuda_native_source(step.source)) {
            snapshot.nvcc = tool_identity(prober, step.program, "--version");
            fail("the header search of CUDA sources is not recorded");
            return snapshot;
        }
        const auto& arguments = step.arguments;
        if (arguments.size() < 4 || utf8(arguments[arguments.size() - 4]) != "-c" ||
            utf8(arguments[arguments.size() - 2]) != "-o") {
            fail("a native compile step has an unexpected shape");
            return snapshot;
        }
        std::vector<NativeText> flags(arguments.begin(), arguments.end() - 4);
        const bool plain_assembly =
            std::find(flags.begin(), flags.end(), platform::native_text("assembler")) !=
            flags.end();
        if (plain_assembly) {
            fail("the .include files of plain assembly sources are not recorded");
            return snapshot;
        }
        const auto known = std::find_if(probed.begin(), probed.end(),
                                        [&](const auto& item) { return item.first == flags; });
        if (known != probed.end()) {
            snapshot.include_searches.push_back(known->second);
            continue;
        }
        auto probe_arguments = flags;
        for (const auto* argument : {"-E", "-v", "/dev/null", "-o", "/dev/null"}) {
            probe_arguments.push_back(platform::native_text(argument));
        }
        const auto probe = prober.run(step.program, probe_arguments);
        auto search = parse_include_search(probe.err + probe.out);
        if (probe.status != 0 || !search) {
            fail("the include search of a native compile is unknown");
            return snapshot;
        }
        // The compile's own plan names the directories the driver adds for
        // system and SDK headers with these flags.
        auto plan_arguments = std::vector<NativeText>{platform::native_text("-###")};
        plan_arguments.insert(plan_arguments.end(), flags.begin(), flags.end());
        for (const auto* argument : {"-c", "/dev/null", "-o", "/dev/null"}) {
            plan_arguments.push_back(platform::native_text(argument));
        }
        const auto compile_plan_probe = prober.run(step.program, plan_arguments);
        const auto compile_plan = parse_driver_plan(compile_plan_probe.err + compile_plan_probe.out);
        const auto native_job = std::find_if(
            compile_plan.jobs.begin(), compile_plan.jobs.end(), [](const auto& job) {
                return job.size() > 1 && job[1] == "-cc1";
            });
        if (compile_plan_probe.status != 0 || native_job == compile_plan.jobs.end()) {
            fail("the plan (-###) of a native compile names no compile job");
            return snapshot;
        }
        add_system_directories(snapshot.system_directories, *native_job);
        probed.emplace_back(std::move(flags), *search);
        snapshot.include_searches.push_back(std::move(*search));
    }

    if (!pkg_config_modules.empty()) {
        snapshot.pkg_config_modules = pkg_config_modules;
        const auto program = pkg_config_program();
        if (program) snapshot.pkg_config = tool_identity(prober, *program, "--version");
        if (!snapshot.pkg_config) {
            fail("pkg-config cannot be identified");
            return snapshot;
        }
        try {
            snapshot.pkg_config_cflags = pkg_config_flags(pkg_config_modules, "--cflags");
            snapshot.pkg_config_libs = pkg_config_flags(pkg_config_modules, "--libs");
        } catch (const std::exception& error) {
            fail(std::string("pkg-config failed: ") + error.what());
            return snapshot;
        }
    }
    return snapshot;
#endif
}

std::vector<ToolchainDependency> toolchain_dependencies(
    const ToolchainSnapshot& snapshot, const NativeLinkRecipe& recipe,
    const std::vector<fs::path>& recorded_by_content,
    bool& complete, std::string& incomplete_reason) {
    complete = true;
    incomplete_reason.clear();
    std::vector<ToolchainDependency> dependencies;
    const auto fail = [&](std::string reason) {
        if (complete) {
            complete = false;
            incomplete_reason = std::move(reason);
        }
    };
    if (!snapshot.complete) {
        fail(snapshot.incomplete_reason);
        return dependencies;
    }
    if (!recipe.recording()) {
        fail("the native link recipe was not made in run mode");
        return dependencies;
    }
    const auto work = normalized(*recipe.work_directory());
    std::set<fs::path> excluded;
    for (const auto& path : recorded_by_content) excluded.insert(normalized(path));
    for (const auto& step : recipe.steps()) {
        if (step.kind == NativeStep::Kind::compile) excluded.insert(normalized(step.source));
        for (const auto& input : step.required_inputs) excluded.insert(normalized(input));
    }
    const auto skipped = [&](const fs::path& path) {
        return excluded.contains(path) || under(work, path);
    };

    std::set<fs::path> seen;
    const auto add_header = [&](const fs::path& path) {
        if (!seen.insert(path).second) return;
        const auto content = read_bytes(path);
        if (!content) {
            fail("a header a native compile read cannot be read: " + path.string());
            return;
        }
        dependencies.push_back(ToolchainDependency{
            ToolchainDependency::Kind::header, path, platform::sha256_hex(*content), {}});
    };
    const auto add_identity = [&](const fs::path& path) {
        if (!seen.insert(path).second) return;
        const auto identity = platform::file_identity(path);
        if (!identity) {
            fail("a file the toolchain used cannot be identified: " + path.string());
            return;
        }
        dependencies.push_back(ToolchainDependency{
            ToolchainDependency::Kind::system_file, path, {}, *identity});
    };
    // A shadowing path: absent, as it should be, or (when the search would
    // not have looked there after all) whatever is there, by identity.
    const auto add_shadow = [&](const fs::path& path) {
        if (seen.contains(path)) return;
        if (path_missing(path)) {
            seen.insert(path);
            dependencies.push_back(ToolchainDependency{
                ToolchainDependency::Kind::absent, path, {}, {}});
            return;
        }
        add_identity(path);
    };
    const auto in_system_directory = [&](const fs::path& path) {
        return std::any_of(snapshot.system_directories.begin(),
                           snapshot.system_directories.end(),
                           [&](const fs::path& directory) { return under(directory, path); });
    };

    std::size_t compile_index = 0;
    for (const auto& step : recipe.steps()) {
        if (step.kind != NativeStep::Kind::compile) continue;
        const auto* search = compile_index < snapshot.include_searches.size()
            ? &snapshot.include_searches[compile_index]
            : nullptr;
        ++compile_index;
        if (!search || !step.depfile) {
            fail("a native compile has no recorded header list");
            return dependencies;
        }
        const auto text = read_bytes(*step.depfile);
        if (!text) {
            fail("a native compile wrote no depfile: " + step.depfile->string());
            return dependencies;
        }
        const auto source = normalized(step.source);
        std::vector<fs::path> headers;
        std::vector<fs::path> order{source.parent_path()};
        for (const auto& prerequisite : parse_depfile(*text)) {
            const auto path = normalized(prerequisite);
            if (path == source) continue;
            headers.push_back(path);
            if (skipped(path)) continue;
            if (in_system_directory(path)) {
                add_identity(path);
            } else {
                add_header(path);
                if (std::find(order.begin(), order.end(), path.parent_path()) == order.end()) {
                    order.push_back(path.parent_path());
                }
            }
        }
        // A quoted include is looked up beside its includer first: the
        // directories of the source and of every non-system header precede
        // the search lists.
        order.insert(order.end(), search->quote.begin(), search->quote.end());
        order.insert(order.end(), search->angle.begin(), search->angle.end());
        for (const auto& header : headers) {
            for (const auto& shadow : header_shadowing_paths(order, header)) add_shadow(shadow);
        }
    }

    for (const auto& step : recipe.steps()) {
        if (step.kind != NativeStep::Kind::link) continue;
        if (!step.trace) {
            fail("the link recorded no trace");
            return dependencies;
        }
        const auto text = read_bytes(*step.trace);
        if (!text) {
            fail("the link wrote no trace: " + step.trace->string());
            return dependencies;
        }
        const auto temporary = normalized(fs::temp_directory_path());
        for (const auto& opened : parse_link_trace(*text)) {
            const auto path = normalized(opened);
            if (skipped(path)) continue;
            if (path_missing(path)) {
                // The driver's own temporary objects are gone by now.
                if (under(temporary, path)) continue;
                fail("a file the linker opened no longer exists: " + path.string());
                return dependencies;
            }
            add_identity(path);
            if (snapshot.library_search) {
                for (const auto& shadow : library_shadowing_paths(
                         snapshot.library_search->library, path, apple_host)) {
                    add_shadow(shadow);
                }
                for (const auto& shadow : framework_shadowing_paths(
                         snapshot.library_search->framework, path)) {
                    add_shadow(shadow);
                }
            }
        }
    }
    return dependencies;
}

bool toolchain_unchanged(const ToolchainSnapshot& snapshot) {
    if (!snapshot.complete) return false;
    std::string requested;
    try {
        requested = clang_driver();
    } catch (const std::exception&) {
        return false;
    }
    if (requested != snapshot.driver.requested) return false;
    const auto driver = platform::command_path(requested);
    if (!driver || *driver != snapshot.driver.tool.program.path) return false;
    const auto& identity = snapshot.driver;
    if (!still_identified(identity.tool.program) || !still_identified(identity.directory) ||
        !still_identified(identity.compiler) || !still_identified(identity.compiler_directory)) {
        return false;
    }
    for (const auto& file : identity.configuration_files) {
        if (!still_identified(file)) return false;
    }
    if (identity.resource_directory && !still_identified(*identity.resource_directory)) {
        return false;
    }
    if (snapshot.linker && !still_identified(snapshot.linker->program)) return false;
    if (snapshot.nvcc && !still_identified(snapshot.nvcc->program)) return false;

    const auto& target = snapshot.target;
    if (target.sysroot) {
        const auto settings = read_bytes(*target.sysroot / "SDKSettings.json");
        const std::optional<std::string> digest =
            settings ? std::optional(platform::sha256_hex(*settings)) : std::nullopt;
        if (digest != target.sdk_settings_sha256) return false;
    }
    if constexpr (apple_host) {
        std::error_code error;
        const auto developer = fs::read_symlink("/var/db/xcode_select_link", error);
        const std::optional<fs::path> current =
            error ? std::nullopt : std::optional(developer);
        if (current != target.developer_directory) return false;
    }

    if (snapshot.pkg_config) {
        const auto program = pkg_config_program();
        if (!program || *program != snapshot.pkg_config->program.path ||
            !still_identified(snapshot.pkg_config->program)) {
            return false;
        }
        try {
            if (pkg_config_flags(snapshot.pkg_config_modules, "--cflags") !=
                    snapshot.pkg_config_cflags ||
                pkg_config_flags(snapshot.pkg_config_modules, "--libs") !=
                    snapshot.pkg_config_libs) {
                return false;
            }
        } catch (const std::exception&) {
            return false;
        }
    }
    return true;
}

bool dependency_unchanged(const ToolchainDependency& dependency) {
    switch (dependency.kind) {
        case ToolchainDependency::Kind::header: {
            const auto content = read_bytes(dependency.path);
            return content && platform::sha256_hex(*content) == dependency.sha256;
        }
        case ToolchainDependency::Kind::system_file:
            return platform::file_identity(dependency.path) ==
                   std::optional(dependency.identity);
        case ToolchainDependency::Kind::absent:
            return path_missing(dependency.path);
    }
    return false;
}

} // namespace quidra::toolchain
