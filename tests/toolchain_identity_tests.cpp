// The toolchain and compiler identity of a cached run: the parsers of what
// the toolchain prints (canned outputs of clang, ld64, GNU ld and lld), the
// snapshot and its checks with fake drivers (shell scripts that print
// canned --version, -### and -E -v output and write the depfile and trace a
// run-mode link records), the dependency records with their shadowing
// paths, the host toolchain's snapshot, the environment table, and the
// compiler build id with its memo.

#include "platform/environment.hpp"
#include "platform/private_directory.hpp"
#include "platform/sha256.hpp"
#include "toolchain/compile_environment.hpp"
#include "toolchain/compiler_identity.hpp"
#include "toolchain/driver_output.hpp"
#include "toolchain/native_link_recipe.hpp"
#include "toolchain/toolchain_identity.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifndef _WIN32
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

template <class T>
std::string shown(const std::vector<T>& items) {
    std::string out = "[";
    for (const auto& item : items) {
        if (out.size() > 1) out += ", ";
        if constexpr (std::is_same_v<T, fs::path>) out += item.generic_string();
        else out += item;
    }
    return out + "]";
}

std::vector<fs::path> paths(std::initializer_list<const char*> items) {
    std::vector<fs::path> result;
    for (const auto* item : items) result.emplace_back(item);
    return result;
}

void driver_plans() {
    const auto plan = parse_driver_plan(
        "Apple clang version 17.0.0 (clang-1700.6.3.2)\n"
        "Target: arm64-apple-darwin25.4.0\n"
        "Thread model: posix\n"
        "InstalledDir: /X/usr/bin\n"
        "Configuration file: /X/usr/bin/clang++.cfg\n"
        " \"/X/usr/bin/clang\" \"-cc1\" \"-triple\" \"arm64-apple-macosx26.0.0\" "
        "\"-target-sdk-version=26.4\" \"-target-cpu\" \"apple-m1\" \"-target-feature\" \"+zcm\" "
        "\"-target-feature\" \"+neon\" \"-resource-dir\" \"/X/lib/clang/17\" \"-isysroot\" "
        "\"/SDK\" \"-D\" \"Q=\\\"a b\\\"\" \"-x\" \"ir\" \"p.ll\"\n"
        " (in-process)\n"
        " \"/X/usr/bin/ld\" \"-syslibroot\" \"/SDK\" \"-L/usr/local/lib\" \"-o\" \"out\"\n");
    check(plan.configuration_files == paths({"/X/usr/bin/clang++.cfg"}), "configuration file");
    check(plan.jobs.size() == 2, "two jobs");
    if (plan.jobs.size() != 2) return;
    const auto& compile = plan.jobs[0];
    check(compile[0] == "/X/usr/bin/clang" && compile[1] == "-cc1", "the compile job's program");
    check(job_option(compile, "-triple") == "arm64-apple-macosx26.0.0", "triple");
    check(job_option(compile, "-target-cpu") == "apple-m1", "cpu");
    check(job_options(compile, "-target-feature") == std::vector<std::string>{"+zcm", "+neon"},
          "features");
    check(job_option(compile, "-target-sdk-version=") == "26.4", "SDK version");
    check(job_option(compile, "-isysroot") == "/SDK", "sysroot");
    check(job_option(compile, "-D") == "Q=\"a b\"", "escaped quotes in an argument");
    check(!job_option(compile, "-missing"), "a missing option");
    check(plan.jobs[1][0] == "/X/usr/bin/ld", "the link job's program");
}

void search_lists() {
    const auto include = parse_include_search(
        "ignoring nonexistent directory \"/nope\"\n"
        "#include \"...\" search starts here:\n"
        " /proj/include\n"
        "#include <...> search starts here:\n"
        " /usr/local/include\n"
        " /X/lib/clang/17/include\n"
        " /SDK/usr/include\n"
        " /SDK/System/Library/Frameworks (framework directory)\n"
        "End of search list.\n");
    check(include.has_value(), "include search parses");
    if (include) {
        check(include->quote == paths({"/proj/include"}), "quote list: " + shown(include->quote));
        check(include->angle == paths({"/usr/local/include", "/X/lib/clang/17/include",
                                       "/SDK/usr/include", "/SDK/System/Library/Frameworks"}),
              "angle list: " + shown(include->angle));
    }
    check(!parse_include_search("#include <...> search starts here:\n /a\n"),
          "an unterminated include search is unknown");

    const auto library = parse_linker_search(
        "@(#)PROGRAM:ld PROJECT:ld-1230.1\n"
        "Library search paths:\n"
        "\t/usr/local/lib\n"
        "\t/SDK/usr/lib\n"
        "Framework search paths:\n"
        "\t/SDK/System/Library/Frameworks\n"
        "Undefined symbols for architecture arm64:\n");
    check(library.has_value(), "ld64 search lists parse");
    if (library) {
        check(library->library == paths({"/usr/local/lib", "/SDK/usr/lib"}),
              "library list: " + shown(library->library));
        check(library->framework == paths({"/SDK/System/Library/Frameworks"}),
              "framework list: " + shown(library->framework));
    }
    check(!parse_linker_search("@(#)PROGRAM:ld\n"), "a version line alone has no search lists");
}

void depfiles_and_traces() {
    check(parse_depfile("native-0.o: /src/a.c \\\n  /inc/b.h /inc/with\\ space.h \\\n"
                        "  rel/c.h /inc/d$$.h /inc/b.h\n") ==
              paths({"/src/a.c", "/inc/b.h", "/inc/with space.h", "rel/c.h", "/inc/d$.h"}),
          "a depfile's prerequisites, unescaped and without repeats");
    check(parse_depfile("C:\\work\\native-0.obj: C:\\src\\a.c C:\\inc\\b.h\r\n") ==
              paths({"C:\\src\\a.c", "C:\\inc\\b.h"}),
          "a Windows depfile keeps its drive letters and backslashes");
    check(parse_depfile("") .empty(), "an empty depfile");

    check(parse_link_trace("/tmp/x.o\n"
                           "/R/libclang_rt.osx.a(truncsfbf2.c.o)\n"
                           "/R/libclang_rt.osx.a(chkstk.S.o)\n"
                           "/SDK/usr/lib/libSystem.tbd\n") ==
              paths({"/tmp/x.o", "/R/libclang_rt.osx.a", "/SDK/usr/lib/libSystem.tbd"}),
          "an ld64 trace");
    check(parse_link_trace("/usr/bin/ld: mode elf_x86_64\n"
                           "/usr/lib/crt1.o\n"
                           "-lc (/usr/lib/x86_64-linux-gnu/libc.so)\n"
                           "(/usr/lib/libc_nonshared.a)elf-init.oS\n") ==
              paths({"/usr/lib/crt1.o", "/usr/lib/x86_64-linux-gnu/libc.so",
                     "/usr/lib/libc_nonshared.a"}),
          "a GNU ld trace");
}

void shadowing() {
    check(header_shadowing_paths(paths({"/a", "/b", "/c"}), "/c/sys/x.h") ==
              paths({"/a/sys/x.h", "/b/sys/x.h"}),
          "a header is shadowed in every earlier directory");
    check(header_shadowing_paths(paths({"/p", "/p/inc"}), "/p/inc/x.h") == paths({"/p/x.h"}),
          "every split of a header's path counts");
    check(header_shadowing_paths(paths({"/a"}), "/a/x.h").empty(),
          "the first directory has no shadow");
    check(header_shadowing_paths(paths({"/a"}), "/elsewhere/x.h").empty(),
          "a header outside the search lists has no shadow");

    check(library_shadowing_paths(paths({"/l1", "/l2"}), "/l2/libfoo.so", true) ==
              paths({"/l1/libfoo.tbd", "/l1/libfoo.dylib", "/l1/libfoo.so", "/l1/libfoo.a",
                     "/l2/libfoo.tbd", "/l2/libfoo.dylib"}),
          "Apple library candidates");
    check(library_shadowing_paths(paths({"/l1", "/l2"}), "/l2/libfoo.a", false) ==
              paths({"/l1/libfoo.so", "/l1/libfoo.a", "/l2/libfoo.so"}),
          "ELF library candidates");
    check(library_shadowing_paths(paths({"/l1"}), "/x/libfoo.so", false).empty(),
          "an explicit library path has no shadow");
    check(library_shadowing_paths(paths({"/l1", "/l2"}), "/l2/libc.so.6", false).empty(),
          "a versioned file name is no -l candidate");
    check(framework_shadowing_paths(paths({"/f1", "/f2"}), "/f2/Metal.framework/Metal.tbd") ==
              paths({"/f1/Metal.framework"}),
          "a framework is shadowed by an earlier framework of its name");
}

void environment_table() {
    std::set<std::string_view> names;
    for (const auto& variable : compile_environment) {
        check(names.insert(variable.name).second,
              "compile_environment names " + std::string(variable.name) + " twice");
    }
    const auto values = compile_environment_values();
    const auto keyed = std::count_if(
        compile_environment.begin(), compile_environment.end(),
        [](const auto& variable) { return variable.key == CacheKeyClass::value; });
    check(values.size() == static_cast<std::size_t>(keyed), "every value variable is read");
    check(std::any_of(values.begin(), values.end(),
                      [](const auto& item) { return item.first == "QUIDRA_CLANGXX"; }),
          "QUIDRA_CLANGXX is keyed by value");
    check(std::none_of(values.begin(), values.end(),
                       [](const auto& item) { return item.first == "PATH"; }),
          "PATH is keyed by its effect, not its value");
}

#ifndef _WIN32

#ifdef __APPLE__
constexpr bool apple = true;
#else
constexpr bool apple = false;
#endif

void write_file(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

void write_script(const fs::path& path, const std::string& text) {
    write_file(path, text);
    fs::permissions(path, fs::perms::owner_all);
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void set_environment(const char* name, const std::optional<std::string>& value) {
    if (value) ::setenv(name, value->c_str(), 1);
    else ::unsetenv(name);
}

// Keeps a set of environment variables and restores them.
class EnvironmentGuard {
public:
    explicit EnvironmentGuard(std::initializer_list<const char*> names) {
        for (const auto* name : names) saved_.emplace_back(name, platform::environment_value(name));
    }
    ~EnvironmentGuard() {
        for (const auto& [name, value] : saved_) set_environment(name, value);
    }

private:
    std::vector<std::pair<const char*, std::optional<std::string>>> saved_;
};

std::string fake_driver(const fs::path& scratch, const std::string& version) {
    const auto s = scratch.string();
    return "#!/bin/sh\n"
           "S='" + s + "'\n"
           "for a in \"$@\"; do\n"
           "  if [ \"$a\" = --version ]; then echo 'fake clang version 1.0 (" + version + ")';"
           " echo 'Target: fake'; exit 0; fi\n"
           "  if [ \"$a\" = '-###' ]; then\n"
           "    [ -f \"$S/bin/fake-clang.cfg\" ] && echo \"Configuration file: $S/bin/fake-clang.cfg\" >&2\n"
           "    echo ' \"'\"$S\"'/cc1\" \"-cc1\" \"-triple\" \"arm64-apple-macosx15.0.0\""
           " \"-target-sdk-version=15.4\" \"-target-cpu\" \"apple-m1\" \"-target-feature\" \"+neon\""
           " \"-target-feature\" \"+sha2\" \"-resource-dir\" \"'\"$S\"'/resource\" \"-isysroot\""
           " \"'\"$S\"'/sdk\" \"-internal-isystem\" \"'\"$S\"'/sdk/usr/include\" \"-x\" \"ir\" \"p.ll\"' >&2\n"
           "    echo ' \"'\"$S\"'/bin/fake-ld\" \"-syslibroot\" \"'\"$S\"'/sdk\" \"-L'\"$S\"'/libs1\""
           " \"-L'\"$S\"'/libs2\" \"-o\" \"out\"' >&2\n"
           "    exit 0\n"
           "  fi\n"
           "  if [ \"$a\" = -E ]; then\n"
           "    printf '#include \"...\" search starts here:\\n %s\\n %s\\n' \"$S/inc1\" \"$S/inc2\" >&2\n"
           "    printf '#include <...> search starts here:\\n %s\\n %s (framework directory)\\n' "
           "\"$S/sdk/usr/include\" \"$S/fw\" >&2\n"
           "    echo 'End of search list.' >&2\n"
           "    exit 0\n"
           "  fi\n"
           "done\n"
           "out=; mf=; trace=0; prev=\n"
           "for a in \"$@\"; do\n"
           "  [ \"$prev\" = -o ] && out=$a\n"
           "  [ \"$prev\" = -MF ] && mf=$a\n"
           "  [ \"$a\" = -Wl,-t ] && trace=1\n"
           "  [ \"$a\" = -Wl,--trace ] && trace=1\n"
           "  prev=$a\n"
           "done\n"
           "[ -n \"$out\" ] && : > \"$out\"\n"
           "[ -n \"$mf\" ] && printf '%s: %s \\\\\\n  %s %s\\n' \"$out\" \"$S/pkg/native.c\" "
           "\"$S/inc2/util.h\" \"$S/sdk/usr/include/sys.h\" > \"$mf\"\n"
           "if [ $trace = 1 ]; then\n"
           "  echo \"$S/rec/native-0.o\"; echo \"$S/libs2/libfoo.so\";"
           " echo \"$S/libs2/libarch.a(member.o)\"; echo \"$S/fw/Thing.framework/Thing.tbd\"\n"
           "fi\n"
           "exit 0\n";
}

std::string fake_linker(const fs::path& scratch, const std::string& version) {
    const auto s = scratch.string();
    return "#!/bin/sh\n"
           "S='" + s + "'\n"
           "for a in \"$@\"; do\n"
           "  if [ \"$a\" = -v ]; then\n"
           "    echo '@(#)PROGRAM:fake-ld PROJECT:" + version + "' >&2\n"
           "    case \" $* \" in *' -lSystem '*) printf 'Library search paths:\\n\\t%s\\n\\t%s\\n"
           "Framework search paths:\\n\\t%s\\n' \"$S/libs1\" \"$S/libs2\" \"$S/fw\" >&2;; esac\n"
           "    exit 0\n"
           "  fi\n"
           "  if [ \"$a\" = --verbose ]; then echo 'SEARCH_DIR(\"=/nonexistent-search-dir\");'; exit 0; fi\n"
           "done\n"
           "exit 0\n";
}

bool has(const std::vector<ToolchainDependency>& dependencies, ToolchainDependency::Kind kind,
         const fs::path& path) {
    return std::any_of(dependencies.begin(), dependencies.end(), [&](const auto& dependency) {
        return dependency.kind == kind && dependency.path == path;
    });
}

const ToolchainDependency* find(const std::vector<ToolchainDependency>& dependencies,
                                const fs::path& path) {
    for (const auto& dependency : dependencies) {
        if (dependency.path == path) return &dependency;
    }
    return nullptr;
}

void fake_toolchain(const fs::path& s) {
    EnvironmentGuard guard{"QUIDRA_CLANGXX", "QUIDRA_CLANG", "QUIDRA_RUNTIME_LIBRARY",
                           "QUIDRA_PKG_CONFIG"};
    write_script(s / "bin" / "fake-clang", fake_driver(s, "one"));
    write_script(s / "bin" / "fake-ld", fake_linker(s, "fake-1"));
    write_script(s / "cc1", "#!/bin/sh\nexit 0\n");
    write_file(s / "sdk" / "SDKSettings.json", "{}");
    write_file(s / "sdk" / "usr" / "include" / "sys.h", "#define SYS 1\n");
    fs::create_directories(s / "resource");
    fs::create_directories(s / "libs1");
    write_file(s / "libs2" / "libfoo.so", "foo");
    write_file(s / "libs2" / "libarch.a", "arch");
    write_file(s / "fw" / "Thing.framework" / "Thing.tbd", "thing");
    fs::create_directories(s / "inc1");
    write_file(s / "inc2" / "util.h", "#define UTIL 1\n");
    write_file(s / "pkg" / "native.c", "#include \"util.h\"\n#include <sys.h>\n");
    write_file(s / "runtime.a", "runtime");
    write_file(s / "program.ll", "; empty module\n");
    fs::create_directories(s / "rec");
    fs::create_directories(s / "out");
    set_environment("QUIDRA_CLANGXX", (s / "bin" / "fake-clang").string());
    set_environment("QUIDRA_RUNTIME_LIBRARY", (s / "runtime.a").string());

    const auto make_recipe = [&](std::vector<fs::path> inputs, std::vector<std::string> modules) {
        return native_link_recipe(
            s / "program.ll", s / "out" / "program",
            LinkOptions{false, true, std::move(inputs), std::move(modules)},
            LinkRecording{s / "rec", s / "rec" / "toolchain.log"});
    };
    const auto recipe = make_recipe({s / "pkg" / "native.c"}, {});
    check(recipe.execute() == 0, "the fake link runs");

    const auto snapshot = snapshot_toolchain(recipe, {});
    check(snapshot.complete, "the fake toolchain's snapshot is complete: " +
                                 snapshot.incomplete_reason);
    check(snapshot.driver.requested == (s / "bin" / "fake-clang").string(), "the driver as named");
    check(snapshot.driver.tool.program.path == s / "bin" / "fake-clang", "the driver resolves");
    check(snapshot.driver.tool.version_line == "fake clang version 1.0 (one)",
          "the driver's version line: " + snapshot.driver.tool.version_line);
    check(snapshot.driver.compiler.path == s / "cc1", "the compiler behind the driver");
    check(snapshot.driver.configuration_files.empty(), "no configuration file");
    check(snapshot.driver.resource_directory &&
              snapshot.driver.resource_directory->path == s / "resource",
          "the resource directory");
    check(snapshot.target.triple == "arm64-apple-macosx15.0.0", "the triple");
    check(snapshot.target.cpu == "apple-m1", "the CPU");
    check(snapshot.target.features == std::vector<std::string>{"+neon", "+sha2"}, "the features");
    check(snapshot.target.sysroot == s / "sdk", "the sysroot");
    check(snapshot.target.sdk_version == "15.4", "the SDK version");
    check(snapshot.target.sdk_settings_sha256 == platform::sha256_hex("{}"), "SDKSettings.json");
    check(snapshot.linker && snapshot.linker->program.path == s / "bin" / "fake-ld" &&
              snapshot.linker->version_line == "@(#)PROGRAM:fake-ld PROJECT:fake-1",
          "the linker and its version");
    const auto expected_libraries =
        apple ? std::vector<fs::path>{s / "libs1", s / "libs2"}
              : std::vector<fs::path>{s / "libs1", s / "libs2", "/nonexistent-search-dir"};
    check(snapshot.library_search && snapshot.library_search->library == expected_libraries,
          "the library search list: " +
              (snapshot.library_search ? shown(snapshot.library_search->library) : "none"));
    check(snapshot.include_searches.size() == 1, "one include search per native compile");
    if (snapshot.include_searches.size() == 1) {
        check(snapshot.include_searches[0].quote == std::vector<fs::path>{s / "inc1", s / "inc2"},
              "the quote search");
        check(snapshot.include_searches[0].angle ==
                  std::vector<fs::path>{s / "sdk" / "usr" / "include", s / "fw"},
              "the angle search");
    }
    check(toolchain_unchanged(snapshot), "a fresh snapshot is unchanged");

    bool complete = false;
    std::string reason;
    const auto dependencies = toolchain_dependencies(
        snapshot, recipe, {s / "runtime.a", s / "program.ll"}, complete, reason);
    check(complete, "the dependencies are complete: " + reason);
    using Kind = ToolchainDependency::Kind;
    check(has(dependencies, Kind::header, s / "inc2" / "util.h"), "a user header by content");
    check(has(dependencies, Kind::system_file, s / "sdk" / "usr" / "include" / "sys.h"),
          "a system header by identity");
    check(has(dependencies, Kind::absent, s / "inc1" / "util.h"),
          "an earlier quote directory must not gain the header");
    check(has(dependencies, Kind::absent, s / "pkg" / "util.h"),
          "the source's directory must not gain the header");
    check(has(dependencies, Kind::absent, s / "inc1" / "sys.h"),
          "an earlier directory must not gain the system header");
    check(has(dependencies, Kind::system_file, s / "libs2" / "libfoo.so"), "a traced library");
    check(has(dependencies, Kind::system_file, s / "libs2" / "libarch.a"),
          "an archive for its member");
    check(has(dependencies, Kind::absent, s / "libs1" / "libfoo.so") &&
              has(dependencies, Kind::absent, s / "libs1" / "libfoo.a"),
          "an earlier library directory must not gain the library");
    check(has(dependencies, Kind::absent, s / "libs2" / "libarch.so"),
          "an earlier candidate name in the same directory");
    if (apple) {
        check(has(dependencies, Kind::absent, s / "libs2" / "libfoo.tbd"),
              "a .tbd precedes a .so on Apple platforms");
    }
    check(has(dependencies, Kind::system_file, s / "fw" / "Thing.framework" / "Thing.tbd"),
          "a framework");
    check(!find(dependencies, s / "rec" / "native-0.o"), "the recipe's own objects are left out");
    check(!find(dependencies, s / "pkg" / "native.c"), "the native source is left out");
    check(std::all_of(dependencies.begin(), dependencies.end(),
                      [](const auto& dependency) { return dependency_unchanged(dependency); }),
          "every fresh dependency is unchanged");

    // Shadowing, content and identity changes are seen.
    write_file(s / "inc1" / "util.h", "#define SHADOW 1\n");
    check(!dependency_unchanged(*find(dependencies, s / "inc1" / "util.h")),
          "a header created in an earlier directory");
    fs::remove(s / "inc1" / "util.h");
    write_file(s / "inc2" / "util.h", "#define UTIL 2\n");
    check(!dependency_unchanged(*find(dependencies, s / "inc2" / "util.h")),
          "a changed user header");
    write_file(s / "libs1" / "libfoo.a", "shadow");
    check(!dependency_unchanged(*find(dependencies, s / "libs1" / "libfoo.a")),
          "a library created in an earlier directory");
    fs::remove(s / "libs1" / "libfoo.a");
    ::usleep(20000);
    write_file(s / "libs2" / "libfoo.so", "foo");
    check(!dependency_unchanged(*find(dependencies, s / "libs2" / "libfoo.so")),
          "a rewritten library");

    // Each toolchain change is seen.
    const auto changed = [&](const std::string& what, auto&& change) {
        const auto before = snapshot_toolchain(recipe, {});
        check(before.complete && toolchain_unchanged(before), what + ": fresh snapshot");
        ::usleep(20000);
        change();
        check(!toolchain_unchanged(before), what + " is seen");
    };
    changed("a configuration file next to the driver", [&] {
        write_file(s / "bin" / "fake-clang.cfg", "-O0\n");
    });
    {
        const auto with_configuration = snapshot_toolchain(recipe, {});
        check(with_configuration.driver.configuration_files.size() == 1,
              "the configuration file is recorded");
        fs::remove(s / "bin" / "fake-clang.cfg");
    }
    changed("a new driver version", [&] {
        write_script(s / "bin" / "fake-clang", fake_driver(s, "two"));
    });
    changed("a new linker", [&] {
        write_script(s / "bin" / "fake-ld", fake_linker(s, "fake-2"));
    });
    changed("a new SDKSettings.json", [&] {
        write_file(s / "sdk" / "SDKSettings.json", "{\"Version\":\"15.5\"}");
    });
    changed("a changed resource directory", [&] {
        write_file(s / "resource" / "new-file", "x");
    });
    changed("another driver path", [&] {
        fs::create_directories(s / "bin2");
        fs::copy_file(s / "bin" / "fake-clang", s / "bin2" / "fake-clang",
                      fs::copy_options::overwrite_existing);
        set_environment("QUIDRA_CLANGXX", (s / "bin2" / "fake-clang").string());
    });
    set_environment("QUIDRA_CLANGXX", (s / "bin" / "fake-clang").string());

    // pkg-config's program and output are part of the toolchain.
    const auto pkg_config = [&](const std::string& include) {
        write_script(s / "bin" / "fake-pkg-config",
                     "#!/bin/sh\n"
                     "case \"$1\" in\n"
                     "  --version) echo 0.29.2 ;;\n"
                     "  --cflags) echo '-I" + include + "' ;;\n"
                     "  --libs) echo '-L/pc/lib -lpc' ;;\n"
                     "esac\n");
    };
    pkg_config("/pc/include");
    set_environment("QUIDRA_PKG_CONFIG", (s / "bin" / "fake-pkg-config").string());
    const auto with_modules = make_recipe({}, {"pcmod"});
    const auto pkg_snapshot = snapshot_toolchain(with_modules, {"pcmod"});
    check(pkg_snapshot.complete, "a snapshot with pkg-config: " + pkg_snapshot.incomplete_reason);
    check(pkg_snapshot.pkg_config && pkg_snapshot.pkg_config->version_line == "0.29.2",
          "pkg-config's version");
    check(pkg_snapshot.pkg_config_cflags == std::vector<std::string>{"-I/pc/include"} &&
              pkg_snapshot.pkg_config_libs == std::vector<std::string>{"-L/pc/lib", "-lpc"},
          "pkg-config's flags");
    check(toolchain_unchanged(pkg_snapshot), "a fresh pkg-config snapshot is unchanged");
    ::usleep(20000);
    pkg_config("/pc/other");
    check(!toolchain_unchanged(pkg_snapshot), "a changed pkg-config is seen");

    // What a snapshot cannot describe makes it incomplete.
    write_file(s / "pkg" / "plain.s", "nop\n");
    const auto assembly = snapshot_toolchain(make_recipe({s / "pkg" / "plain.s"}, {}), {});
    check(!assembly.complete && !toolchain_unchanged(assembly),
          "plain assembly makes the snapshot incomplete");
    set_environment("QUIDRA_CLANGXX", (s / "bin" / "no-such-driver").string());
    const auto missing = snapshot_toolchain(make_recipe({}, {}), {});
    check(!missing.complete, "a driver that does not resolve makes the snapshot incomplete");
}

void host_toolchain(const fs::path& s) {
    EnvironmentGuard guard{"QUIDRA_CLANGXX", "QUIDRA_CLANG", "QUIDRA_PKG_CONFIG"};
    set_environment("QUIDRA_CLANGXX", std::nullopt);
    set_environment("QUIDRA_CLANG", std::nullopt);
    set_environment("QUIDRA_PKG_CONFIG", std::nullopt);
    write_file(s / "host" / "program.ll", "; empty module\n");
    fs::create_directories(s / "host" / "rec");
    const auto recipe = native_link_recipe(
        s / "host" / "program.ll", s / "host" / "program", LinkOptions{},
        LinkRecording{s / "host" / "rec", s / "host" / "rec" / "toolchain.log"});
    const auto snapshot = snapshot_toolchain(recipe, {});
    check(snapshot.complete, "the host toolchain's snapshot is complete: " +
                                 snapshot.incomplete_reason);
    check(!snapshot.target.triple.empty(), "the host triple is known");
    check(fs::is_regular_file(snapshot.driver.compiler.path), "the host compiler exists");
    check(snapshot.linker && fs::is_regular_file(snapshot.linker->program.path),
          "the host linker exists");
    check(snapshot.library_search && !snapshot.library_search->library.empty(),
          "the host linker searches libraries");
    check(toolchain_unchanged(snapshot), "the host toolchain is unchanged");
    if (snapshot.complete) {
        std::cout << "host toolchain: " << snapshot.driver.compiler.path.string() << " "
                  << snapshot.target.triple << ", linker "
                  << (snapshot.linker ? snapshot.linker->version_line : std::string()) << "\n";
    }
}

void compiler_build_ids(const fs::path& s) {
    const auto executable = s / "quidra-copy";
    write_file(executable, "binary content A");
    const auto later = std::chrono::system_clock::now() + std::chrono::seconds(10);
    const auto memo_root = platform::open_private_root(s / "cache");
    check(memo_root.path.has_value(), "a memo root opens");
    if (!memo_root.path) return;
    const auto memos = platform::open_private_subdirectory(*memo_root.path, "compiler-id");
    check(memos.path.has_value(), "the memo directory opens");
    if (!memos.path) return;
    const auto memo_files = [&] {
        std::vector<fs::path> files;
        for (const auto& item : fs::directory_iterator(*memos.path)) files.push_back(item.path());
        return files;
    };

    check(compiler_build_id(executable, std::nullopt) == platform::sha256_hex("binary content A"),
          "the build id is the executable's SHA-256");
    check(compiler_build_id(executable, memos.path) == platform::sha256_hex("binary content A"),
          "the build id with a memo directory");
    check(memo_files().empty(), "no memo inside the racy window");

    check(compiler_build_id(executable, memos.path, later) ==
              platform::sha256_hex("binary content A"),
          "the build id once the executable is old enough");
    const auto files = memo_files();
    check(files.size() == 1, "one memo is written");
    if (files.size() != 1) return;
    const auto memo = files.front();
    auto text = read_file(memo);
    const auto digest = platform::sha256_hex("binary content A");
    const auto position = text.find(digest);
    check(position != std::string::npos, "the memo holds the digest");
    if (position == std::string::npos) return;
    const std::string planted(64, 'f');
    text.replace(position, digest.size(), planted);
    check(platform::replace_private_file(memo, text), "the memo can be replaced");
    check(compiler_build_id(executable, memos.path, later) == planted,
          "a memo that matches the executable's identity is used");

    ::usleep(20000);
    write_file(executable, "binary content B");
    check(compiler_build_id(executable, memos.path, later) ==
              platform::sha256_hex("binary content B"),
          "a changed executable is hashed again");
    check(read_file(memo).find(platform::sha256_hex("binary content B")) != std::string::npos,
          "the memo is renewed");

    check(platform::replace_private_file(memo, "not json"), "the memo can be corrupted");
    check(compiler_build_id(executable, memos.path, later) ==
              platform::sha256_hex("binary content B"),
          "a corrupt memo is ignored");
    check(!compiler_build_id(s / "no-such-executable", memos.path, later),
          "a missing executable has no build id");

    const CompilerVersions versions{"0.0.0-test", 7, "9"};
    const auto identity = compiler_identity(versions, memo_root.path);
    check(identity && identity->versions == versions && identity->build_id.size() == 64,
          "the running compiler's identity");
}

#endif

} // namespace

int main() {
    driver_plans();
    search_lists();
    depfiles_and_traces();
    shadowing();
    environment_table();

#ifndef _WIN32
    auto pattern = (fs::temp_directory_path() / "quidra-toolchain-tests-XXXXXX").string();
    const char* made = ::mkdtemp(pattern.data());
    if (!made) {
        std::cerr << "cannot create a scratch directory\n";
        return 1;
    }
    const fs::path scratch = fs::canonical(made);
    fake_toolchain(scratch);
    host_toolchain(scratch);
    compiler_build_ids(scratch);
    std::error_code ignored;
    fs::remove_all(scratch, ignored);
#endif

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "toolchain identity tests passed\n";
    return 0;
}
