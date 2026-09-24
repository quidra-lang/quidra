#include "quidra/package_lock.hpp"
#include "quidra/project.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out);
    out << text;
    assert(out);
}

} // namespace

int main() {
    using namespace quidra;

    const auto nonce =
        std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto root =
        fs::temp_directory_path() /
        ("quidra-package-lock-" + std::to_string(nonce));
    const auto package = root / "sample";
    fs::create_directories(package);

    const auto main = package / package_entrypoint_filename();
    write_text(main, "int value()\n    return 1\n");
    write_text(
        package / std::string(package_manifest_filename),
        "name = sample\n"
        "version = 1.2.3\n"
        "repository = https://example.invalid/sample\n");
    write_text(
        package / std::string(package_project_filename),
        "[package]\n"
        "name = \"quidra-sample\"\n"
        "import = \"sample\"\n"
        "display_name = \"Quidra Sample\"\n"
        "version = \"1.2.3\"\n"
        "repository = \"https://example.invalid/sample\"\n");

    const auto text =
        package_lock_text(std::map<std::string, fs::path>{{"sample", main}});
    const auto header = current_lockfile_header();
    assert(text.starts_with(header + "\n"));

    write_text(root / std::string(package_lock_filename), text);
    const auto current = read_package_lock(root);
    assert(current);
    assert(current->at("sample").distribution_name == "quidra-sample");
    assert(current->at("sample").version == "1.2.3");

    const std::string digest(64, '0');
    write_text(
        root / std::string(package_lock_filename),
        "quidra-lock-v1\n"
        "sample " + digest + "\n");
    const auto legacy_v1 = read_package_lock(root);
    assert(legacy_v1);
    assert(legacy_v1->at("sample").distribution_name == "sample");
    assert(legacy_v1->at("sample").version == "-");

    write_text(
        root / std::string(package_lock_filename),
        "quidra-lock-v2\n"
        "sample 1.2.3 " + digest + "\n");
    const auto legacy_v2 = read_package_lock(root);
    assert(legacy_v2);
    assert(legacy_v2->at("sample").distribution_name == "sample");
    assert(legacy_v2->at("sample").version == "1.2.3");

    fs::remove_all(root);
    return 0;
}
