#pragma once
#include "toolchain/native_link_recipe.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace quidra {
class CompileInputs;
}

namespace quidra::native {

using toolchain::LinkOptions;

// The native inputs of the packages a program imports (a compilation's
// package map), in build order: per package, by name, its prebuilt library
// for this platform and then its native sources; and the pkg-config modules
// they declare. With `inputs`, the manifests read are recorded.
struct PackageNativeInput {
    std::filesystem::path path;
    bool source{};
};

struct PackageNativeBuildInputs {
    std::vector<PackageNativeInput> inputs;
    std::vector<std::string> pkg_config_modules;
};

PackageNativeBuildInputs package_native_build_inputs(
    const std::map<std::string, std::filesystem::path>& packages,
    CompileInputs* inputs = nullptr);

struct JitOptions {
    bool optimize{true};
    std::string argv0{"quidra"};
    std::vector<std::filesystem::path> libraries;
    std::vector<std::filesystem::path> sources;
    std::vector<std::string> pkg_config_modules;
};

int run_llvm_jit(
    const std::filesystem::path& llvm,
    const std::vector<std::string>& arguments = {},
    JitOptions options = {},
    const std::optional<std::filesystem::path>& stdout_path = std::nullopt,
    const std::optional<std::filesystem::path>& stderr_path = std::nullopt);
// Builds the native link's recipe in build mode and executes it.
int link_llvm(
    const std::filesystem::path& llvm,
    const std::filesystem::path& output,
    LinkOptions options = {});
// The library build (quidra build --lib): a self-contained static archive
// of the module compiled to an object, the package native objects the
// executable link would compile (and the prebuilt package archives and
// --link objects or archives), and the members of the runtime library
// (toolchain/archiver.hpp). Throws std::runtime_error when an input cannot
// go into an archive (a shared library) or a tool fails.
void build_library(
    const std::filesystem::path& llvm,
    const std::filesystem::path& output,
    const LinkOptions& options);
// The flags a C linker needs to link that archive into a host program
// (toolchain/link_flags.hpp), from the same options.
std::vector<std::string> library_link_flags(
    const std::filesystem::path& llvm,
    const LinkOptions& options);
int run_debugger(
    const std::filesystem::path& program,
    const std::vector<std::string>& arguments = {});

} // namespace quidra::native
