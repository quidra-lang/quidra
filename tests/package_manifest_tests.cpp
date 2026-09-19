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
        manifest.requirements.at("quidra")
            .matches(parse_semantic_version("0.2.5")));
    assert(
        manifest.requirements.at("vision")
            .matches(parse_semantic_version("0.1.9")));

    fs::remove_all(root);
    return 0;
}
