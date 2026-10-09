// The run cache's parts below the CLI: keys, metadata and index files, the
// checks of a hit's records, the store's atomic publish (concurrent
// publishers of one key, a publisher killed in each phase, injected rename
// failures, dead staging, trash), and eviction and cleaning with an
// injected limit, clock and draw.

#include "platform/cache_files.hpp"
#include "platform/file_lock.hpp"
#include "platform/private_directory.hpp"
#include "platform/sha256.hpp"
#include "run_cache_options.hpp"
#include "toolchain/run_cache_eviction.hpp"
#include "toolchain/run_cache_key.hpp"
#include "toolchain/run_cache_metadata.hpp"
#include "toolchain/run_cache_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <iostream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace quidra;
using namespace quidra::toolchain;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

class Scratch {
public:
    Scratch() {
        std::random_device random;
        path_ = fs::temp_directory_path() /
                ("quidra-run-cache-unit-" + std::to_string(random()) + std::to_string(random()));
        fs::create_directories(path_);
#ifndef _WIN32
        fs::permissions(path_, fs::perms::owner_all, fs::perm_options::replace);
#endif
    }
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::string digest(char c) { return std::string(64, c); }

platform::FileIdentity identity(std::uint64_t seed) {
    return platform::FileIdentity{seed, seed + 1, 0, seed + 2,
                                  static_cast<std::int64_t>(seed) * 1000,
                                  static_cast<std::int64_t>(seed) * 1000 + 7};
}

RunCachePreKeyMaterial sample_material() {
    RunCachePreKeyMaterial material;
    material.compiler_version = "0.5.0";
    material.abi_version = 3;
    material.ir_version = "7";
    material.build_id = digest('a');
    material.platform = "macos-arm64";
    material.os_version = "15.4 (24E248)";
    material.driver_requested = "clang++";
    material.driver_path = "/usr/bin/clang++";
    material.runtime_library = "/opt/quidra/lib/libquidra_runtime.a";
    material.native_include_directory = fs::path("/opt/quidra/include");
    material.entry = "/work/app/main.qui";
    material.working_directory = "/work/app";
    material.link_inputs = {{"extra.c", "/work/app/extra.c"}};
    material.options = run_cache::cache_key_options(CompileOptions{}, LinkOptions{});
    material.environment = {{"QUIDRA_CLANGXX", std::nullopt}, {"SDKROOT", std::string("/SDK")}};
    return material;
}

ToolchainSnapshot sample_snapshot() {
    ToolchainSnapshot snapshot;
    snapshot.driver.requested = "clang++";
    snapshot.driver.tool = ToolIdentity{{"/usr/bin/clang++", identity(10)}, digest('b'), "clang 17"};
    snapshot.driver.directory = {"/usr/bin", identity(11)};
    snapshot.driver.compiler = {"/X/bin/clang", identity(12)};
    snapshot.driver.compiler_directory = {"/X/bin", identity(13)};
    snapshot.driver.configuration_files = {{"/X/bin/clang++.cfg", identity(14)}};
    snapshot.driver.resource_directory = IdentifiedFile{"/X/lib/clang/17", identity(15)};
    snapshot.target.triple = "arm64-apple-macosx15.0.0";
    snapshot.target.cpu = "apple-m1";
    snapshot.target.features = {"+neon", "+zcm"};
    snapshot.target.sysroot = fs::path("/SDK");
    snapshot.target.sdk_version = "15.4";
    snapshot.target.sdk_settings_sha256 = digest('c');
    snapshot.target.developer_directory = fs::path("/Applications/Xcode.app/Contents/Developer");
    snapshot.linker = ToolIdentity{{"/X/bin/ld", identity(16)}, digest('d'), "ld64"};
    snapshot.library_search = LibrarySearch{{"/SDK/usr/lib"}, {"/SDK/System/Library/Frameworks"}};
    snapshot.include_searches = {IncludeSearch{{"/q"}, {"/a", "/b"}}};
    snapshot.system_directories = {"/SDK", "/X/lib/clang/17"};
    snapshot.pkg_config = ToolIdentity{{"/opt/bin/pkg-config", identity(17)}, digest('e'), "0.29"};
    snapshot.pkg_config_modules = {"zlib"};
    snapshot.pkg_config_cflags = {"-I/opt/include"};
    snapshot.pkg_config_libs = {"-lz"};
    return snapshot;
}

std::vector<RunCacheDependency> sample_dependencies() {
    std::vector<RunCacheDependency> dependencies;
    const auto content = [&](RunCacheDependencyKind kind, const char* path, char c) {
        RunCacheDependency dependency;
        dependency.kind = kind;
        dependency.path = path;
        dependency.size = 12;
        dependency.sha256 = digest(c);
        dependencies.push_back(dependency);
    };
    content(RunCacheDependencyKind::source, "/work/app/main.qui", '1');
    content(RunCacheDependencyKind::manifest, "/pkg/nn/quidra.package", '2');
    content(RunCacheDependencyKind::project, "/pkg/nn/project.toml", '3');
    content(RunCacheDependencyKind::descriptor, "/pkg/nn/graph.toml", '4');
    content(RunCacheDependencyKind::lock, "/work/app/quidra.lock", '5');
    RunCacheDependency absent;
    absent.kind = RunCacheDependencyKind::absent;
    absent.path = "/root1/nn/main.qui";
    absent.state = "missing";
    dependencies.push_back(absent);
    RunCacheDependency local;
    local.kind = RunCacheDependencyKind::local_import;
    local.importer = "/work/app/main.qui";
    local.target = "util.qui";
    local.path = "/work/app/util.qui";
    dependencies.push_back(local);
    RunCacheDependency package;
    package.kind = RunCacheDependencyKind::package;
    package.name = "nn";
    package.path = "/pkg/nn/main.qui";
    dependencies.push_back(package);
    RunCacheDependency tree;
    tree.kind = RunCacheDependencyKind::package_tree;
    tree.path = "/pkg/nn/main.qui";
    tree.sha256 = digest('6');
    dependencies.push_back(tree);
    content(RunCacheDependencyKind::native_source, "/pkg/nn/native/a.cpp", '7');
    content(RunCacheDependencyKind::native_library, "/pkg/nn/lib/libnn.a", '8');
    content(RunCacheDependencyKind::link_input, "/work/app/extra.c", '9');
    content(RunCacheDependencyKind::runtime_library, "/opt/quidra/lib/libquidra_runtime.a", 'a');
    RunCacheDependency header;
    header.kind = RunCacheDependencyKind::native_header;
    header.path = "/pkg/nn/native/a.h";
    header.sha256 = digest('b');
    dependencies.push_back(header);
    RunCacheDependency system;
    system.kind = RunCacheDependencyKind::system_file;
    system.path = "/SDK/usr/include/stdio.h";
    system.identity = identity(20);
    dependencies.push_back(system);
    return dependencies;
}

RunCacheEntry sample_entry() {
    RunCacheEntry entry;
    entry.material = sample_material();
    entry.toolchain = sample_snapshot();
    entry.recipe_sha256 = digest('f');
    entry.dependencies = sample_dependencies();
    entry.pre_key = run_cache_pre_key(entry.material);
    entry.key = run_cache_key(entry.material, entry.toolchain, entry.recipe_sha256, entry.dependencies);
    entry.program = RunCacheStoredFile{run_cache_program_file(), 1234, digest('0')};
    entry.toolchain_output = RunCacheStoredFile{"build.log", 0, platform::sha256_hex("")};
    entry.created_unix_ns = 1700000000000000000;
    entry.build_wall_ms = 812;
    return entry;
}

std::string key_of(const RunCachePreKeyMaterial& material, const ToolchainSnapshot& snapshot,
                   const std::string& recipe, const std::vector<RunCacheDependency>& dependencies) {
    return run_cache_key(material, snapshot, recipe, dependencies);
}

void keys() {
    const auto material = sample_material();
    const auto snapshot = sample_snapshot();
    const auto dependencies = sample_dependencies();
    const auto recipe = digest('f');
    const auto pre_key = run_cache_pre_key(material);
    const auto key = key_of(material, snapshot, recipe, dependencies);
    check(run_cache_digest_text(pre_key) && run_cache_digest_text(key), "keys are digests");
    check(pre_key != key, "the pre-key and the key differ");
    check(run_cache_pre_key(sample_material()) == pre_key, "the pre-key is deterministic");
    check(key_of(material, snapshot, recipe, dependencies) == key, "the key is deterministic");

    // Every component of the pre-key changes both keys (K1-K8, K13-K15,
    // K19-K23).
    std::vector<std::pair<std::string, std::function<void(RunCachePreKeyMaterial&)>>> changes{
        {"K2 version", [](auto& m) { m.compiler_version = "0.5.1"; }},
        {"K4 abi", [](auto& m) { m.abi_version = 4; }},
        {"K4 ir", [](auto& m) { m.ir_version = "8"; }},
        {"K3 build id", [](auto& m) { m.build_id = digest('b'); }},
        {"K5 platform", [](auto& m) { m.platform = "linux-x86_64"; }},
        {"K6 os", [](auto& m) { m.os_version = "15.5 (24F74)"; }},
        {"K8 driver name", [](auto& m) { m.driver_requested = "clang++-20"; }},
        {"K8 driver path", [](auto& m) { m.driver_path = "/opt/bin/clang++"; }},
        {"K13 runtime", [](auto& m) { m.runtime_library = "/other/libquidra_runtime.a"; }},
        {"K14 include", [](auto& m) { m.native_include_directory = std::nullopt; }},
        {"K14 include path", [](auto& m) { m.native_include_directory = fs::path("/x"); }},
        {"K15 entry", [](auto& m) { m.entry = "/work/other/main.qui"; }},
        {"K23 cwd", [](auto& m) { m.working_directory = "/work"; }},
        {"K19 link input text", [](auto& m) { m.link_inputs[0].given = "./extra.c"; }},
        {"K19 link input path", [](auto& m) { m.link_inputs[0].path = "/w/extra.c"; }},
        {"K19 new link input", [](auto& m) { m.link_inputs.push_back({"b.o", "/w/b.o"}); }},
        {"K19 link input order",
         [](auto& m) {
             m.link_inputs.push_back({"b.o", "/w/b.o"});
             std::reverse(m.link_inputs.begin(), m.link_inputs.end());
         }},
        {"K20 option", [](auto& m) { m.options[0].value = "true"; }},
        {"K21 value set", [](auto& m) { m.environment[0].second = std::string("clang++"); }},
        {"K21 value empty", [](auto& m) { m.environment[0].second = std::string(); }},
        {"K22 value changed", [](auto& m) { m.environment[1].second = std::string("/SDK2"); }},
        {"K22 value unset", [](auto& m) { m.environment[1].second = std::nullopt; }},
    };
    std::set<std::string> seen{pre_key};
    for (const auto& [name, change] : changes) {
        auto changed = material;
        change(changed);
        const auto changed_pre_key = run_cache_pre_key(changed);
        check(changed_pre_key != pre_key, name + " changes the pre-key");
        check(seen.insert(changed_pre_key).second, name + " gives a pre-key of its own");
        check(key_of(changed, snapshot, recipe, dependencies) != key, name + " changes the key");
    }

    // The toolchain snapshot (K5 triple and CPU, K7-K12), the recipe (K20)
    // and every dependency record change the key but not the pre-key.
    std::vector<std::pair<std::string, std::function<void(ToolchainSnapshot&)>>> toolchain_changes{
        {"K8 driver identity", [](auto& s) { s.driver.tool.program.identity.changed_ns++; }},
        {"K8 driver version", [](auto& s) { s.driver.tool.version_sha256 = digest('9'); }},
        {"K8 compiler", [](auto& s) { s.driver.compiler.path = "/Y/clang"; }},
        {"K8 configuration file", [](auto& s) { s.driver.configuration_files.clear(); }},
        {"K8 resource directory", [](auto& s) { s.driver.resource_directory.reset(); }},
        {"K5 triple", [](auto& s) { s.target.triple = "x86_64-apple-macosx15.0.0"; }},
        {"K5 cpu", [](auto& s) { s.target.cpu = "apple-m2"; }},
        {"K5 features", [](auto& s) { s.target.features.push_back("+sha3"); }},
        {"K7 sysroot", [](auto& s) { s.target.sysroot = fs::path("/SDK2"); }},
        {"K7 sdk version", [](auto& s) { s.target.sdk_version = "15.5"; }},
        {"K7 sdk settings", [](auto& s) { s.target.sdk_settings_sha256.reset(); }},
        {"K7 developer directory", [](auto& s) { s.target.developer_directory.reset(); }},
        {"K9 linker", [](auto& s) { s.linker->program.identity.size++; }},
        {"K12 library search", [](auto& s) { s.library_search->library.push_back("/L"); }},
        {"K12 framework search", [](auto& s) { s.library_search->framework.clear(); }},
        {"K12 include search", [](auto& s) { s.include_searches[0].angle.pop_back(); }},
        {"K12 system directories", [](auto& s) { s.system_directories.pop_back(); }},
        {"K11 pkg-config", [](auto& s) { s.pkg_config->version_line = "0.30"; }},
        {"K11 cflags", [](auto& s) { s.pkg_config_cflags.push_back("-DX"); }},
        {"K11 libs", [](auto& s) { s.pkg_config_libs.clear(); }},
        {"K11 modules", [](auto& s) { s.pkg_config_modules.push_back("png"); }},
        {"K10 nvcc", [](auto& s) { s.nvcc = s.linker; }},
    };
    for (const auto& [name, change] : toolchain_changes) {
        auto changed = snapshot;
        change(changed);
        check(key_of(material, changed, recipe, dependencies) != key, name + " changes the key");
    }
    check(key_of(material, snapshot, digest('e'), dependencies) != key, "K20 recipe changes the key");
    for (std::size_t index = 0; index < dependencies.size(); ++index) {
        const auto kind = run_cache_dependency_kind_name(dependencies[index].kind);
        auto changed = dependencies;
        changed[index].path += "x";
        check(key_of(material, snapshot, recipe, changed) != key,
              std::string(kind) + " path changes the key");
        changed = dependencies;
        changed.erase(changed.begin() + static_cast<std::ptrdiff_t>(index));
        check(key_of(material, snapshot, recipe, changed) != key,
              std::string(kind) + " record removed changes the key");
        changed = dependencies;
        auto& record = changed[index];
        switch (record.kind) {
            case RunCacheDependencyKind::absent: record.state = "other"; break;
            case RunCacheDependencyKind::local_import: record.target = "u2.qui"; break;
            case RunCacheDependencyKind::package: record.name = "nn2"; break;
            case RunCacheDependencyKind::system_file: record.identity.modified_ns++; break;
            default: record.sha256 = digest('5' == record.sha256[0] ? '6' : '5'); break;
        }
        check(key_of(material, snapshot, recipe, changed) != key,
              std::string(kind) + " content changes the key");
        if (run_cache_content_kind(record.kind)) {
            changed = dependencies;
            changed[index].size++;
            check(key_of(material, snapshot, recipe, changed) != key,
                  std::string(kind) + " size changes the key");
        }
    }
    auto reordered = dependencies;
    std::swap(reordered[0], reordered[1]);
    check(key_of(material, snapshot, recipe, reordered) != key, "record order changes the key");

    // Excluded inputs: max_errors, the link inputs and pkg-config modules of
    // LinkOptions (keyed as records), run-time state.
    CompileOptions few;
    few.max_errors = 1;
    CompileOptions many;
    many.max_errors = 500;
    LinkOptions with_inputs;
    with_inputs.inputs = {"/a.c"};
    with_inputs.pkg_config_modules = {"zlib"};
    check(run_cache::cache_key_options(few, LinkOptions{}) ==
              run_cache::cache_key_options(many, with_inputs),
          "max_errors, link inputs and pkg-config modules are not options of the key");
    CompileOptions debug_info;
    debug_info.debug_info = true;
    CompileOptions lowering;
    lowering.lowering.copy_every_value_loop_at_entry = true;
    LinkOptions debug;
    debug.debug = true;
    LinkOptions unoptimized;
    unoptimized.optimize = false;
    const auto base = run_cache::cache_key_options(CompileOptions{}, LinkOptions{});
    check(run_cache::cache_key_options(debug_info, LinkOptions{}) != base, "debug_info is an option");
    check(run_cache::cache_key_options(lowering, LinkOptions{}) != base, "lowering is an option");
    check(run_cache::cache_key_options(CompileOptions{}, debug) != base, "debug is an option");
    check(run_cache::cache_key_options(CompileOptions{}, unoptimized) != base, "optimize is an option");
    CompileOptions spelled;
    spelled.source_display_path = "./prog.qui";
    check(run_cache::cache_key_options(spelled, LinkOptions{}) != base,
          "the display spelling of the root is an option");
    CompileOptions library;
    library.artifact = CompileArtifact::Library;
    check(run_cache::cache_key_options(library, LinkOptions{}) != base,
          "a library artifact is an option");
    CompileOptions executable;
    executable.artifact = CompileArtifact::Executable;
    check(run_cache::cache_key_options(executable, LinkOptions{}) == base,
          "an executable, which every run builds, adds no option");
}

void metadata_round_trip() {
    const auto entry = sample_entry();
    const auto text = write_run_cache_metadata(entry);
    RunCacheEntry read_back;
    try {
        read_back = read_run_cache_metadata(text);
    } catch (const std::exception& error) {
        check(false, std::string("metadata reads back: ") + error.what());
        return;
    }
    check(write_run_cache_metadata(read_back) == text, "metadata round trip is exact");
    check(read_back.material == entry.material, "material round trip");
    check(read_back.dependencies == entry.dependencies, "dependency round trip");
    check(read_back.program == entry.program && read_back.toolchain_output == entry.toolchain_output,
          "stored files round trip");
    check(run_cache_key(read_back.material, read_back.toolchain, read_back.recipe_sha256,
                        read_back.dependencies) == entry.key,
          "the key recomputed from read metadata is the entry's key");
    check(run_cache_pre_key(read_back.material) == entry.pre_key, "the pre-key recomputes");

    // Paths and texts with characters JSON escapes.
    auto unusual = entry;
    unusual.material.entry = fs::path("/work/a \"b\"\\c\td\x01/main.qui");
    unusual.material.environment[0].second = std::string("line\nbreak");
    unusual.toolchain.driver.tool.version_line = "Apple clang \xc3\xa9";
    try {
        const auto back = read_run_cache_metadata(write_run_cache_metadata(unusual));
        check(back.material == unusual.material, "escaped texts round trip");
        check(back.toolchain.driver.tool.version_line == unusual.toolchain.driver.tool.version_line,
              "UTF-8 round trips");
    } catch (const std::exception& error) {
        check(false, std::string("escaped metadata reads back: ") + error.what());
    }
    // Large identity numbers stay exact.
    auto large = entry;
    large.dependencies.back().identity.device = 18446744073709551615ULL;
    large.dependencies.back().identity.modified_ns = -5;
    try {
        const auto back = read_run_cache_metadata(write_run_cache_metadata(large));
        check(back.dependencies.back().identity == large.dependencies.back().identity,
              "identity numbers round trip");
    } catch (const std::exception& error) {
        check(false, std::string("identity metadata reads back: ") + error.what());
    }
}

void replace_once(std::string& text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    if (at == std::string::npos) {
        check(false, "corruption pattern not found: " + from);
        return;
    }
    text.replace(at, from.size(), to);
}

void corrupt_metadata() {
    const auto text = write_run_cache_metadata(sample_entry());
    std::vector<std::pair<std::string, std::function<void(std::string&)>>> forms{
        {"empty", [](std::string& t) { t.clear(); }},
        {"truncated", [](std::string& t) { t.resize(t.size() / 2); }},
        {"not JSON", [](std::string& t) { t = "program"; }},
        {"trailing data", [](std::string& t) { t += "{}"; }},
        {"higher schema", [](std::string& t) { replace_once(t, "\"schema\":1", "\"schema\":2"); }},
        {"other kind",
         [](std::string& t) { replace_once(t, "quidra-run-cache-entry", "quidra-run-cache-index"); }},
        {"missing member", [](std::string& t) { replace_once(t, "\"build_wall_ms\":812", "\"x\":1"); }},
        {"extra member",
         [](std::string& t) { replace_once(t, "\"build_wall_ms\":812", "\"build_wall_ms\":812,\"y\":1"); }},
        {"duplicate member",
         [](std::string& t) {
             replace_once(t, "\"build_wall_ms\":812", "\"build_wall_ms\":812,\"build_wall_ms\":812");
         }},
        {"unknown dependency kind",
         [](std::string& t) { replace_once(t, "\"kind\":\"lock\"", "\"kind\":\"lockfile\""); }},
        {"dependency missing a field",
         [](std::string& t) { replace_once(t, ",\"state\":\"missing\"", ""); }},
        {"unknown absent state",
         [](std::string& t) { replace_once(t, "\"state\":\"missing\"", "\"state\":\"gone\""); }},
        {"short digest", [](std::string& t) { replace_once(t, std::string(64, '1'), "11"); }},
        {"upper-case digest",
         [](std::string& t) { replace_once(t, std::string(64, 'f'), std::string(64, 'F')); }},
        {"negative size", [](std::string& t) { replace_once(t, "\"size\":1234", "\"size\":-1"); }},
        {"size as text", [](std::string& t) { replace_once(t, "\"size\":1234", "\"size\":\"1234\""); }},
        {"fractional number", [](std::string& t) { replace_once(t, "\"size\":1234", "\"size\":12.5"); }},
        {"identity not decimal",
         [](std::string& t) { replace_once(t, "\"device\":\"20\"", "\"device\":\"0x14\""); }},
        {"identity with leading zero",
         [](std::string& t) { replace_once(t, "\"device\":\"20\"", "\"device\":\"020\""); }},
        {"program file with a directory",
         [](std::string& t) { replace_once(t, "\"file\":\"build.log\"", "\"file\":\"../build.log\""); }},
        {"null where text", [](std::string& t) { replace_once(t, "\"cpu\":\"apple-m1\"", "\"cpu\":null"); }},
    };
    for (const auto& [name, mutate] : forms) {
        auto changed = text;
        mutate(changed);
        bool rejected = false;
        try {
            (void)read_run_cache_metadata(changed);
        } catch (const RunCacheCorruptError&) {
            rejected = true;
        } catch (const std::exception& error) {
            check(false, name + " throws something other than RunCacheCorruptError: " + error.what());
            rejected = true;
        }
        check(rejected, "corrupt metadata is rejected: " + name);
    }
}

void index_files() {
    RunCacheIndex index{digest('1'), {}};
    for (char c : std::string("23456")) index = run_cache_index_with(index, digest(c));
    check(index.keys.size() == run_cache_index_limit, "the index keeps at most 4 keys");
    check(index.keys.front() == digest('6') && index.keys.back() == digest('3'),
          "the index lists the newest key first");
    index = run_cache_index_with(index, digest('4'));
    check(index.keys.front() == digest('4') && index.keys.size() == 4 &&
              std::count(index.keys.begin(), index.keys.end(), digest('4')) == 1,
          "a key already listed moves to the front");
    const auto text = write_run_cache_index(index);
    const auto back = read_run_cache_index(text);
    check(back && back->pre_key == index.pre_key && back->keys == index.keys, "index round trip");
    for (const auto& bad : {std::string("{}"), std::string("[]"), text.substr(0, 20),
                            std::string("{\"schema\":1,\"kind\":\"quidra-run-cache-index\",\"pre_key\":\"") +
                                digest('1') + "\",\"keys\":[\"zz\"]}"}) {
        check(!read_run_cache_index(bad), "an invalid index is ignored: " + bad);
    }
}

void recheck_records() {
    Scratch scratch;
    const auto file = scratch.path() / "a.qui";
    write(file, "print(1)\n");
    RunCacheDependency source;
    source.kind = RunCacheDependencyKind::source;
    source.path = file;
    const auto digest_now = run_cache_file_digest(file);
    check(digest_now && digest_now->size == 9 && digest_now->sha256 == platform::sha256_hex("print(1)\n"),
          "file digest");
    source.size = digest_now->size;
    source.sha256 = digest_now->sha256;
    check(recheck_dependency(source) == RunCacheRecheck::unchanged, "an unchanged source holds");
    const auto time = fs::last_write_time(file);
    write(file, "print(2)\n");
    fs::last_write_time(file, time);
    check(recheck_dependency(source) == RunCacheRecheck::changed,
          "a source changed with its size and mtime kept is seen");
    fs::remove(file);
    check(recheck_dependency(source) == RunCacheRecheck::changed, "a removed source is seen");

    RunCacheDependency absent;
    absent.kind = RunCacheDependencyKind::absent;
    absent.path = scratch.path() / "missing" / "main.qui";
    absent.state = "missing";
    check(recheck_dependency(absent) == RunCacheRecheck::unchanged, "an absent path holds");
    write(absent.path, "");
    check(recheck_dependency(absent) == RunCacheRecheck::changed, "a created path is seen");
    RunCacheDependency other;
    other.kind = RunCacheDependencyKind::absent;
    other.path = scratch.path() / "missing";
    other.state = "other";
    check(recheck_dependency(other) == RunCacheRecheck::unchanged, "a directory stays other");
    fs::remove_all(other.path);
    check(recheck_dependency(other) == RunCacheRecheck::changed, "other that disappears is seen");

    const auto system = scratch.path() / "stdio.h";
    write(system, "int x;\n");
    RunCacheDependency system_file;
    system_file.kind = RunCacheDependencyKind::system_file;
    system_file.path = system;
    system_file.identity = *platform::file_identity(system);
    check(recheck_dependency(system_file) == RunCacheRecheck::unchanged, "a system file holds");
    const auto replaced = scratch.path() / "stdio.h.new";
    write(replaced, "int x;\n");
    fs::last_write_time(replaced, fs::last_write_time(system));
    fs::rename(replaced, system);
    check(recheck_dependency(system_file) == RunCacheRecheck::changed,
          "a system file replaced by an equal one is seen by identity");

    RunCacheDependency header;
    header.kind = RunCacheDependencyKind::native_header;
    header.path = system;
    header.sha256 = platform::sha256_hex("int x;\n");
    check(recheck_dependency(header) == RunCacheRecheck::unchanged, "a header holds by content");
    for (const auto kind : {RunCacheDependencyKind::local_import, RunCacheDependencyKind::package,
                            RunCacheDependencyKind::package_tree}) {
        RunCacheDependency record;
        record.kind = kind;
        check(recheck_dependency(record) == RunCacheRecheck::not_checked_here,
              "resolutions are the caller's to check");
    }
}

// ---- The store ----------------------------------------------------------------

void fill_entry(const RunCacheStaging& staging, const std::string& program) {
    write(staging.directory() / run_cache_program_file(), program);
    write(staging.directory() / std::string(run_cache_metadata_file), "{}\n");
    write(staging.directory() / std::string(run_cache_build_log_file), "");
    write(staging.directory() / std::string(run_cache_used_file), "");
}

std::vector<fs::path> children(const fs::path& directory) {
    std::vector<fs::path> result;
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        result.push_back(it->path());
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<fs::path> entries_of(const RunCacheStore& store) {
    std::vector<fs::path> result;
    for (const auto& path : children(store.layout().entries())) {
        if (path.filename() != "index") result.push_back(path);
    }
    return result;
}

bool complete_entry(const fs::path& entry, const std::string& program) {
    return read(entry / run_cache_program_file()) == program &&
           fs::is_regular_file(entry / std::string(run_cache_metadata_file)) &&
           fs::is_regular_file(entry / std::string(run_cache_build_log_file)) &&
           fs::is_regular_file(entry / std::string(run_cache_used_file));
}

void store_basics() {
    Scratch scratch;
    std::string refusal;
    auto store = RunCacheStore::open(scratch.path() / "root", "0.5.0", "macos-arm64", &refusal);
    check(store.has_value(), "the store opens: " + refusal);
    if (!store) return;
    check(!RunCacheStore::open(scratch.path() / "root2", "..", "x", &refusal),
          "a version that leaves the directory is refused");
    const auto key = digest('1');
    check(!store->entry_directory(key), "no entry before publishing");
    fs::path staging_directory;
    {
        auto staging = store->create_staging();
        check(staging != nullptr, "staging is made");
        if (!staging) return;
        staging_directory = staging->directory();
        check(fs::is_directory(staging_directory), "the staging directory exists");
        check(fs::exists(store->layout().staging() / (staging->id() + ".lease")), "its lease exists");
        fill_entry(*staging, "program-1");
        check(store->publish(*staging, key) == RunCachePublishOutcome::published, "publish");
    }
    check(!fs::exists(staging_directory), "the staging directory was renamed away");
    check(children(store->layout().staging()).empty(), "the lease is removed with the staging");
    const auto entry = store->entry_directory(key);
    check(entry && complete_entry(*entry, "program-1"), "the published entry is complete");
    check(!fs::exists(store->layout().entry(digest('2'))) && !store->entry_directory(digest('2')),
          "looking up a missing entry creates nothing");
    {
        auto staging = store->create_staging();
        fill_entry(*staging, "program-1");
        check(store->publish(*staging, key) == RunCachePublishOutcome::already_present,
              "a second publish of one key finds the first");
    }
    check(children(store->layout().staging()).empty(), "the losing staging is removed");
    check(entries_of(*store).size() == 1, "one entry for one key");

    check(store->index_keys(digest('9')).empty(), "no index yet");
    check(store->add_to_index(digest('9'), key), "index written");
    check(store->add_to_index(digest('9'), digest('3')), "index updated");
    const auto keys = store->index_keys(digest('9'));
    check(keys.size() == 2 && keys[0] == digest('3') && keys[1] == key, "the index lists newest first");
    write(store->layout().index(digest('8')), "not an index");
    check(store->index_keys(digest('8')).empty(), "a corrupt index is ignored");
    check(store->add_to_index(digest('8'), key) && store->index_keys(digest('8')).size() == 1,
          "a corrupt index is replaced");

    store->discard(key);
    check(!store->entry_directory(key) && !fs::exists(store->layout().entry(key)), "discarded");
    check(children(store->layout().trash()).empty(), "the trash is emptied after a discard");

#ifndef _WIN32
    // A symbolic link in place of an entry is refused.
    fs::create_directories(scratch.path() / "elsewhere");
    fs::create_directory_symlink(scratch.path() / "elsewhere", store->layout().entry(digest('4')));
    check(!store->entry_directory(digest('4')), "an entry that is a symbolic link is refused");
#endif
}

// A file system whose renames fail as a test says.
class InjectedFileSystem : public RunCacheFileSystem {
public:
    std::vector<std::error_code> failures;
    int renames = 0;
    std::error_code rename_directory(const fs::path& from, const fs::path& to) override {
        ++renames;
        if (!failures.empty()) {
            const auto failure = failures.front();
            failures.erase(failures.begin());
            if (failure == std::errc::no_such_file_or_directory) {
                // A concurrent clean removed run/.
                std::error_code ignored;
                fs::remove_all(to.parent_path().parent_path().parent_path(), ignored);
            }
            return failure;
        }
        return RunCacheFileSystem::rename_directory(from, to);
    }
};

void injected_failures() {
    Scratch scratch;
    InjectedFileSystem file_system;
    auto store = RunCacheStore::open(scratch.path() / "root", "0.5.0", "t", nullptr, &file_system);
    if (!store) {
        check(false, "store opens with an injected file system");
        return;
    }
    {
        file_system.failures = {std::make_error_code(std::errc::cross_device_link)};
        auto staging = store->create_staging();
        fill_entry(*staging, "p");
        check(store->publish(*staging, digest('1')) == RunCachePublishOutcome::failed,
              "a failed rename publishes nothing");
    }
    check(children(store->layout().staging()).empty() && !store->entry_directory(digest('1')),
          "a failed publish leaves no staging and no entry");
    {
        file_system.failures = {std::make_error_code(std::errc::no_such_file_or_directory)};
        file_system.renames = 0;
        auto staging = store->create_staging();
        fill_entry(*staging, "p");
        check(store->publish(*staging, digest('2')) == RunCachePublishOutcome::published,
              "a rename racing a clean is retried once");
        check(file_system.renames == 2, "the retry renames once more");
    }
    {
        file_system.failures = {std::make_error_code(std::errc::no_such_file_or_directory),
                                std::make_error_code(std::errc::no_such_file_or_directory)};
        auto staging = store->create_staging();
        fill_entry(*staging, "p");
        check(store->publish(*staging, digest('3')) == RunCachePublishOutcome::failed,
              "only one retry");
    }
    {
        file_system.failures = {std::make_error_code(std::errc::directory_not_empty)};
        auto staging = store->create_staging();
        fill_entry(*staging, "p");
        check(store->publish(*staging, digest('4')) == RunCachePublishOutcome::already_present,
              "a lost race is reported as present");
    }
    check(children(store->layout().staging()).empty(), "no staging is left behind");
}

void dead_staging_and_trash() {
    Scratch scratch;
    auto store = RunCacheStore::open(scratch.path() / "root", "0.5.0", "t");
    if (!store) {
        check(false, "store opens");
        return;
    }
    // Leftovers of a process that died: a lease nobody holds with its
    // directory, and a directory without a lease.
    write(store->layout().staging() / "dead.lease", "");
    write(store->layout().staging() / "dead" / "program", "x");
    write(store->layout().staging() / "orphan" / "program", "x");
    auto live = store->create_staging();
    fill_entry(*live, "live");
    store->remove_dead_staging();
    const auto left = children(store->layout().staging());
    check(left.size() == 2 && fs::exists(live->directory()) &&
              fs::exists(store->layout().staging() / (live->id() + ".lease")),
          "dead staging is removed and live staging is kept");
    write(store->layout().trash() / "old" / "a" / "b", "x");
    check(store->empty_trash() == 0 && children(store->layout().trash()).empty(), "trash emptied");
}

#ifndef _WIN32

// Runs `body` in a child process; returns its wait status.
int in_child(const std::function<int()>& body) {
    std::cout.flush();
    std::cerr.flush();
    const pid_t pid = ::fork();
    if (pid == 0) _exit(body());
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
    }
    return status;
}

void parallel_publishers() {
    Scratch scratch;
    const auto root = scratch.path() / "root";
    const auto key = digest('7');
    std::vector<pid_t> children_started;
    for (int index = 0; index < 8; ++index) {
        const pid_t pid = ::fork();
        if (pid == 0) {
            auto store = RunCacheStore::open(root, "0.5.0", "t");
            if (!store) _exit(2);
            auto staging = store->create_staging();
            if (!staging) _exit(3);
            fill_entry(*staging, "same program");
            const auto outcome = store->publish(*staging, key);
            if (outcome == RunCachePublishOutcome::failed) _exit(4);
            if (!store->add_to_index(digest('8'), key)) _exit(5);
            staging.reset();
            _exit(outcome == RunCachePublishOutcome::published ? 10 : 11);
        }
        children_started.push_back(pid);
    }
    int published = 0;
    int present = 0;
    for (const auto pid : children_started) {
        int status = 0;
        ::waitpid(pid, &status, 0);
        const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (code == 10) ++published;
        else if (code == 11) ++present;
        else check(false, "a parallel publisher failed with " + std::to_string(code));
    }
    check(published == 1 && present == 7, "exactly one of 8 parallel publishers wins");
    auto store = RunCacheStore::open(root, "0.5.0", "t");
    const auto entries = entries_of(*store);
    check(entries.size() == 1 && complete_entry(entries.front(), "same program"),
          "8 parallel publishers leave one complete entry");
    check(children(store->layout().staging()).empty(), "no staging is left behind");
    check(store->index_keys(digest('8')) == std::vector<std::string>{key}, "the index names the key");
}

class KillAt : public RunCacheFileSystem {
public:
    explicit KillAt(RunCachePublishPhase phase) : phase_(phase) {}
    void reached(RunCachePublishPhase phase) override {
        if (phase == phase_) ::kill(::getpid(), SIGKILL);
    }

private:
    RunCachePublishPhase phase_;
};

void killed_publishers() {
    for (const auto phase : {RunCachePublishPhase::staged, RunCachePublishPhase::synced,
                             RunCachePublishPhase::renamed}) {
        Scratch scratch;
        const auto root = scratch.path() / "root";
        const auto key = digest('5');
        const int status = in_child([&] {
            KillAt kill_at(phase);
            auto store = RunCacheStore::open(root, "0.5.0", "t", nullptr, &kill_at);
            if (!store) return 2;
            auto staging = store->create_staging();
            if (!staging) return 3;
            fill_entry(*staging, "program");
            (void)store->publish(*staging, key);
            return 4;
        });
        const auto name = "phase " + std::to_string(static_cast<int>(phase));
        check(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, name + ": the publisher was killed");
        auto store = RunCacheStore::open(root, "0.5.0", "t");
        const auto entries = entries_of(*store);
        if (phase == RunCachePublishPhase::renamed) {
            check(entries.size() == 1 && complete_entry(entries.front(), "program"),
                  name + ": a renamed entry is complete");
        } else {
            check(entries.empty(), name + ": nothing is published before the rename");
        }
        check(!children(store->layout().staging()).empty(), name + ": the dead staging is visible");
        store->remove_dead_staging();
        check(children(store->layout().staging()).empty(), name + ": dead staging is removed");
        auto staging = store->create_staging();
        fill_entry(*staging, "program");
        const auto outcome = store->publish(*staging, key);
        check(outcome == (phase == RunCachePublishPhase::renamed
                              ? RunCachePublishOutcome::already_present
                              : RunCachePublishOutcome::published),
              name + ": the next publish succeeds");
    }
}

#endif

// ---- Eviction and cleaning ----------------------------------------------------

using FileTime = fs::file_time_type;

void publish_entry(const RunCacheStore& store, char key, std::size_t bytes, FileTime used) {
    auto staging = store.create_staging();
    fill_entry(*staging, std::string(bytes, key));
    check(store.publish(*staging, digest(key)) == RunCachePublishOutcome::published,
          std::string("publish ") + key);
    fs::last_write_time(store.layout().entry(digest(key)) / std::string(run_cache_used_file), used);
}

RunCacheEvictionEnvironment fixed(FileTime now, unsigned draw) {
    return RunCacheEvictionEnvironment{[now] { return now; }, [draw](unsigned) { return draw; }};
}

void eviction() {
    Scratch scratch;
    auto store = RunCacheStore::open(scratch.path() / "root", "0.5.0", "t");
    if (!store) {
        check(false, "store opens");
        return;
    }
    const auto now = FileTime::clock::now();
    // Five entries of 64 KiB, used 5 to 1 hours ago (key '1' the oldest).
    for (int index = 0; index < 5; ++index) {
        publish_entry(*store, static_cast<char>('1' + index), 64 * 1024,
                      now - std::chrono::hours(5 - index));
    }
    check(store->add_to_index(digest('a'), digest('1')), "index of the oldest entry");
    check(store->add_to_index(digest('b'), digest('5')), "index of the newest entry");
    write(store->layout().staging() / "dead.lease", "");
    write(store->layout().staging() / "dead" / "program", "x");
    const auto usage = run_cache_usage(*store);
    check(usage.entries == 5 && usage.bytes >= 5 * 64 * 1024, "usage counts entries and bytes");
    const auto entry_bytes = run_cache_entry_size(store->layout().entry(digest('3')));
    check(entry_bytes >= 64 * 1024 && entry_bytes * 5 <= usage.bytes + 4 * 1024 * 5,
          "an entry's size is its files' allocated bytes");

    RunCacheEvictionPolicy roomy;
    roomy.limit_bytes = usage.bytes;
    auto scan = scan_run_cache(*store, roomy, fixed(now, 1));
    check(scan.scanned && scan.evicted == 0 && scan.after.entries == 5,
          "nothing is evicted at the limit");
    check(!fs::exists(store->layout().staging() / "dead") &&
              !fs::exists(store->layout().staging() / "dead.lease"),
          "a scan removes dead staging");
    check(fs::exists(store->layout().run() / "last-scan"), "a scan stamps run/last-scan");

    // Above the limit: the least recently used go until at most 80 % is left.
    RunCacheEvictionPolicy tight;
    tight.limit_bytes = usage.bytes - 1;
    scan = scan_run_cache(*store, tight, fixed(now, 1));
    check(scan.evicted == 2, "two entries go to reach 80 % (" + std::to_string(scan.evicted) + ")");
    check(!store->entry_directory(digest('1')) && !store->entry_directory(digest('2')) &&
              store->entry_directory(digest('3')) && store->entry_directory(digest('5')),
          "the least recently used entries go first");
    check(scan.after.bytes <= tight.limit_bytes / 100 * 80, "at most 80 % is left");
    check(store->index_keys(digest('a')).empty() && !fs::exists(store->layout().index(digest('a'))),
          "an index whose keys are gone is dropped");
    check(store->index_keys(digest('b')) == std::vector<std::string>{digest('5')},
          "an index with a live key stays");
    check(children(store->layout().trash()).empty(), "the trash is emptied");

    // Entries used in the last minute are never evicted.
    for (const char key : std::string("345")) {
        fs::last_write_time(store->layout().entry(digest(key)) / std::string(run_cache_used_file),
                            now - std::chrono::seconds(30));
    }
    RunCacheEvictionPolicy none;
    none.limit_bytes = 1;
    scan = scan_run_cache(*store, none, fixed(now, 1));
    check(scan.evicted == 0 && scan.after.entries == 3, "recently used entries are kept");
    scan = scan_run_cache(*store, none, fixed(now + std::chrono::seconds(31), 1));
    check(scan.evicted == 3 && scan.after.entries == 0, "they go once a minute has passed");
    check(!fs::exists(store->layout().entries()), "empty target and version directories go");

    // Another process scanning: this one skips.
    {
        fs::create_directories(store->layout().run());
        platform::LockedFile held(store->layout().run() / "evict.lock",
                                  platform::LockOpenMode::open_always, false);
        scan = scan_run_cache(*store, none, fixed(now, 1));
        check(!scan.scanned, "a scan skips while another holds the lock");
    }

    // When a scan is due.
    RunCacheEvictionPolicy policy;
    policy.limit_bytes = 1000;
    fs::remove(store->layout().run() / "last-scan");
    check(run_cache_scan_due(*store, 10, policy, fixed(now, 1)), "due without a last scan");
    write(store->layout().run() / "last-scan", "");
    fs::last_write_time(store->layout().run() / "last-scan", now - std::chrono::minutes(30));
    check(!run_cache_scan_due(*store, 10, policy, fixed(now, 1)), "not due after a recent scan");
    check(run_cache_scan_due(*store, 10, policy, fixed(now, 0)), "due on a winning draw");
    check(run_cache_scan_due(*store, 101, policy, fixed(now, 1)), "due for an entry over 10 %");
    check(run_cache_scan_due(*store, 10, policy, fixed(now + std::chrono::minutes(31), 1)),
          "due an hour after the last scan");
}

void cleaning() {
    Scratch scratch;
    auto store = RunCacheStore::open(scratch.path() / "root", "0.5.0", "t");
    auto other = RunCacheStore::open(scratch.path() / "root", "0.4.0", "t");
    if (!store || !other) {
        check(false, "stores open");
        return;
    }
    const auto now = FileTime::clock::now();
    publish_entry(*store, '1', 1024, now);
    publish_entry(*store, '2', 1024, now);
    publish_entry(*other, '3', 1024, now);
    write(store->layout().root / "compiler-id" / "memo.json", "{}");
    write(store->layout().staging() / "dead.lease", "");
    write(store->layout().staging() / "dead" / "program", "x");
    auto live = store->create_staging();
    fill_entry(*live, "live");
    const auto before = run_cache_usage(*store);
    // Another holder of the lock delays the clean by the timeout only.
    fs::create_directories(store->layout().run());
    auto held = std::make_unique<platform::LockedFile>(
        store->layout().run() / "evict.lock", platform::LockOpenMode::open_always, false);
    const auto clean = clean_run_cache(*store, std::chrono::milliseconds(100));
    held.reset();
    check(clean.removed.entries == 3 && clean.removed.bytes == before.bytes,
          "clean reports every entry of every version");
    check(clean.files_left == 0, "nothing is left in use");
    check(!fs::exists(store->layout().run()), "run/ is gone");
    check(!fs::exists(store->layout().root / "compiler-id"), "the build id memos are gone");
    check(!fs::exists(store->layout().staging() / "dead"), "dead staging is gone");
    check(fs::exists(live->directory()), "live staging is kept");
    check(children(store->layout().trash()).empty(), "the trash is empty");
    live.reset();
    check(clean_run_cache(*store).removed.entries == 0, "a second clean removes nothing");
}

#ifndef _WIN32

// Readers verifying and linking entries while scans evict everything: a
// reader sees an entry or a miss, never a partial entry or an error.
void readers_against_evictor() {
    Scratch scratch;
    const auto root = scratch.path() / "root";
    auto store = RunCacheStore::open(root, "0.5.0", "t");
    const pid_t evictor = ::fork();
    if (evictor == 0) {
        auto own = RunCacheStore::open(root, "0.5.0", "t");
        RunCacheEvictionPolicy none;
        none.limit_bytes = 1;
        none.recent_use = std::chrono::seconds(0);
        for (int round = 0; round < 200; ++round) {
            (void)scan_run_cache(*own, none, run_cache_eviction_environment());
        }
        _exit(0);
    }
    int seen = 0;
    int missed = 0;
    for (int round = 0; round < 200; ++round) {
        const char key = static_cast<char>('a' + round % 6);
        {
            auto staging = store->create_staging();
            fill_entry(*staging, std::string(4096, key));
            (void)store->publish(*staging, digest(key));
        }
        const auto entry = store->entry_directory(digest(key));
        const auto linked = scratch.path() / ("linked-" + std::to_string(round));
        const auto program_digest =
            entry ? platform::private_file_digest(*entry / run_cache_program_file()) : std::nullopt;
        if (program_digest && platform::link_or_copy_file(*entry / run_cache_program_file(), linked)) {
            check(read(linked) == std::string(4096, key), "a linked program is complete");
            check(program_digest->size == 4096, "a verified program is complete");
            ++seen;
        } else {
            ++missed;
        }
    }
    int status = 0;
    ::waitpid(evictor, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the evictor finished");
    check(seen + missed == 200, "every reader saw an entry or a miss");
}

#endif

void cache_files() {
    Scratch scratch;
    const auto file = scratch.path() / "program";
    write(file, "abc");
    const auto digest_value = platform::private_file_digest(file);
    check(digest_value && digest_value->size == 3 && digest_value->sha256 == platform::sha256_hex("abc"),
          "private file digest");
    check(platform::allocated_size(file).has_value(), "allocated size");
    const auto old = fs::last_write_time(file) - std::chrono::hours(2);
    fs::last_write_time(file, old);
    check(platform::touch_file(file) && fs::last_write_time(file) > old, "touch");
    check(platform::link_or_copy_file(file, scratch.path() / "linked") &&
              read(scratch.path() / "linked") == "abc",
          "link or copy");
    check(!platform::link_or_copy_file(file, scratch.path() / "linked"), "an existing name is kept");
#ifndef _WIN32
    fs::create_symlink(file, scratch.path() / "symlink");
    check(!platform::private_file_digest(scratch.path() / "symlink"),
          "a symbolic link is not read as a private file");
#endif
    fs::create_directory(scratch.path() / "from");
    write(scratch.path() / "from" / "f", "x");
    write(scratch.path() / "to" / "g", "y");
    check(platform::rename_directory(scratch.path() / "from", scratch.path() / "to") ==
              std::errc::directory_not_empty ||
              platform::rename_directory(scratch.path() / "from", scratch.path() / "to") ==
                  std::errc::file_exists,
          "a directory is not renamed onto a populated one");
    check(!platform::rename_directory(scratch.path() / "from", scratch.path() / "new"), "rename");
    check(platform::sync_directory_files(scratch.path() / "new"), "sync");
}

} // namespace

int main() {
    // Keep a breadcrumb on stderr for crashes (notably optimized Clang builds).
    // CTest suppresses these in successful runs and displays them on failure.
    const auto step = [](const char* name, const auto& run) {
        std::cerr << "run-cache-unit: " << name << std::endl;
        run();
    };
    step("keys", keys);
    step("metadata_round_trip", metadata_round_trip);
    step("corrupt_metadata", corrupt_metadata);
    step("index_files", index_files);
    step("recheck_records", recheck_records);
    step("cache_files", cache_files);
    step("store_basics", store_basics);
    step("injected_failures", injected_failures);
    step("dead_staging_and_trash", dead_staging_and_trash);
    step("eviction", eviction);
    step("cleaning", cleaning);
#ifndef _WIN32
    step("parallel_publishers", parallel_publishers);
    step("killed_publishers", killed_publishers);
    step("readers_against_evictor", readers_against_evictor);
#endif
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "run cache unit tests passed\n";
    return 0;
}
