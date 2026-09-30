#include "quidra/package_manifest.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

int main() {
    using namespace quidra;

    const auto parsed_version = parse_semantic_version("0.2.3");
    assert(parsed_version.major == 0);
    assert(parsed_version.minor == 2);
    assert(parsed_version.patch == 3);
    assert(parsed_version.str() == "0.2.3");

    const auto requirement =
        parse_version_requirement(">=0.2.0 <0.3.0");
    assert(requirement.matches(parse_semantic_version("0.2.0")));
    assert(requirement.matches(parse_semantic_version("0.2.9")));
    assert(!requirement.matches(parse_semantic_version("0.1.9")));
    assert(!requirement.matches(parse_semantic_version("0.3.0")));

    bool rejected = false;
    try {
        (void)parse_semantic_version("0.2");
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);

    const auto nonce =
        std::chrono::high_resolution_clock::now()
            .time_since_epoch()
            .count();
    const auto root =
        fs::temp_directory_path() /
        ("quidra-package-manifest-" + std::to_string(nonce));
    fs::create_directories(root);

    {
        std::ofstream out(root / "quidra.package");
        out
            << "name = sample\n"
            << "version = 0.4.1\n"
            << "repository = https://github.com/example/sample\n"
            << "description = Example package metadata\n"
            << "license = MIT\n"
            << "homepage = https://example.invalid/sample\n"
            << "asset.linux-x86_64 = https://example.invalid/sample-linux.tar.xz\n"
            << "asset.default = https://example.invalid/sample-portable.tar.gz\n"
            << "native.linux-x86_64 = native/libsample-linux.so\n"
            << "native.default = native/libsample-portable.so\n"
            << "native.source.bridge = native/bridge.cpp\n"
            << "native.source.macos-arm64.metal = native/metal.mm\n"
            << "native.source.linux-x86_64.cuda = native/cuda.cu\n"
            << "native.pkg.compression = zlib\n"
            << "requires.quidra = >=0.2.0 <0.3.0\n"
            << "requires.vision = >=0.1.0 <0.2.0\n";
    }

    const auto manifest = read_package_manifest(root);
    assert(manifest.name == "sample");
    assert(manifest.version.str() == "0.4.1");
    assert(
        manifest.repository &&
        *manifest.repository ==
            "https://github.com/example/sample");
    assert(manifest.description && *manifest.description == "Example package metadata");
    assert(manifest.license && *manifest.license == "MIT");
    assert(manifest.homepage && *manifest.homepage == "https://example.invalid/sample");
    assert(
        manifest.assets.at("linux-x86_64") ==
        "https://example.invalid/sample-linux.tar.xz");
    assert(
        manifest.assets.at("default") ==
        "https://example.invalid/sample-portable.tar.gz");
    assert(
        manifest.native_libraries.at("linux-x86_64") ==
        "native/libsample-linux.so");
    assert(
        manifest.native_libraries.at("default") ==
        "native/libsample-portable.so");
    assert(manifest.native_sources.at("bridge") == "native/bridge.cpp");
    assert(manifest.native_platform_sources.at("macos-arm64").at("metal") == "native/metal.mm");
    assert(manifest.native_platform_sources.at("linux-x86_64").at("cuda") == "native/cuda.cu");
    assert(manifest.native_pkg_config.at("compression") == "zlib");

    fs::create_directories(root / "native");
    {
        std::ofstream out(root / "native" / "libsample-linux.so");
        out << "fixture";
    }
    {
        std::ofstream out(root / "native" / "libsample-portable.so");
        out << "fixture";
    }
    {
        std::ofstream out(root / "native" / "bridge.cpp");
        out << "extern \"C\" int bridge(){return 7;}\n";
    }
    if (const auto platform = package_host_platform()) {
        if (*platform == "macos-arm64") {
            std::ofstream out(root / "native" / "metal.mm");
            out << "extern \"C\" int metal_source(){return 1;}\n";
        } else if (*platform == "linux-x86_64") {
            std::ofstream out(root / "native" / "cuda.cu");
            out << "extern \"C\" int cuda_source(){return 1;}\n";
        }
    }
    const auto native_sources = package_native_source_paths(root, manifest);
    const auto host_platform = package_host_platform();
    const std::size_t expected_native_sources =
        host_platform && (*host_platform == "macos-arm64" || *host_platform == "linux-x86_64") ? 2 : 1;
    assert(native_sources.size() == expected_native_sources);
    assert(
        native_sources.front() ==
        (fs::absolute(root) / "native" / "bridge.cpp").lexically_normal());
    const auto native_path = package_native_library_path(root, manifest);
    assert(native_path);
    assert(fs::is_regular_file(*native_path));

    assert(
        manifest.requirements.at("quidra")
            .matches(parse_semantic_version("0.2.5")));
    assert(
        manifest.requirements.at("vision")
            .matches(parse_semantic_version("0.1.9")));

    // A package without project.toml keeps working: the richer names are
    // optional and older packages predate the file.
    assert(!manifest.project);

    fs::create_directories(root / "compiler");
    {
        std::ofstream out(root / "compiler" / "optimize.toml");
        out << "[extension]\n"
            << "version = 1\n";
    }
    {
        std::ofstream out(root / "project.toml");
        out << "[package]\n"
            << "name = \"quidra-sample\"\n"
            << "import = \"sample\"\n"
            << "display_name = \"Quidra Sample\"\n"
            << "version = \"0.4.1\"\n"
            << "repository = \"https://github.com/example/sample\"\n"
            << "\n"
            << "[requires]\n"
            << "quidra = \">=0.2.0 <0.3.0\"\n"
            << "abi = 1\n"
            << "\n"
            << "[compiler.extension]\n"
            << "optimize = \"compiler/optimize.toml\"\n";
    }

    const auto described = read_package_manifest(root);
    assert(described.project);
    assert(described.project->distribution_name == "quidra-sample");
    assert(described.project->import_name == "sample");
    assert(described.project->display_name == "Quidra Sample");
    assert(described.project->repository == "https://github.com/example/sample");
    assert(package_distribution_name(described) == "quidra-sample");
    assert(package_import_name(described) == "sample");
    assert(package_display_name(described) == "Quidra Sample");
    assert(is_distribution_package_name("quidra-sample"));
    assert(!is_distribution_package_name("Quidra Sample"));
    assert(described.project->abi_requirement &&
           *described.project->abi_requirement == 1);
    assert(described.project->compiler_extensions.at("optimize") ==
           "compiler/optimize.toml");
    const auto compiler_extensions =
        package_compiler_extension_paths(root, described);
    assert(compiler_extensions.size() == 1);
    assert(compiler_extensions.at("optimize") ==
           (fs::absolute(root) / "compiler" / "optimize.toml").lexically_normal());

    // quidra.package is generated from project.toml, so a disagreement means
    // one of them was hand-edited.
    {
        std::ofstream out(root / "project.toml");
        out << "[package]\n"
            << "name = \"quidra-sample\"\n"
            << "import = \"sample\"\n"
            << "display_name = \"Quidra Sample\"\n"
            << "version = \"0.5.0\"\n";
    }
    bool drift_rejected = false;
    try {
        (void)read_package_manifest(root);
    } catch (const std::runtime_error&) {
        drift_rejected = true;
    }
    assert(drift_rejected);

    fs::remove_all(root);
    return 0;
}
