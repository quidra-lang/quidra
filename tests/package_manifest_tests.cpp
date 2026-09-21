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

    const auto version = parse_semantic_version("0.2.3");
    assert(version.major == 0);
    assert(version.minor == 2);
    assert(version.patch == 3);
    assert(version.str() == "0.2.3");

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
        manifest.requirements.at("quidra")
            .matches(parse_semantic_version("0.2.5")));
    assert(
        manifest.requirements.at("vision")
            .matches(parse_semantic_version("0.1.9")));

    // A package without project.toml keeps working: the richer names are
    // optional and older packages predate the file.
    assert(!manifest.project);

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
            << "abi = 1\n";
    }

    const auto described = read_package_manifest(root);
    assert(described.project);
    assert(described.project->distribution_name == "quidra-sample");
    assert(described.project->import_name == "sample");
    assert(described.project->display_name == "Quidra Sample");
    assert(described.project->abi_requirement &&
           *described.project->abi_requirement == 1);

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
