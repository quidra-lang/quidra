#include "quidra/import_path.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

static void expect_path(
    std::string_view source,
    const fs::path& importer,
    const fs::path& cwd,
    quidra::ImportPathBase expected_base,
    const fs::path& expected) {
    const auto resolved = quidra::resolve_local_import_path(source, importer, cwd);
    if (resolved.base != expected_base || resolved.path != expected.lexically_normal()) {
        std::cerr << "unexpected resolution for " << source << "\n"
                  << "actual:   " << resolved.path << "\n"
                  << "expected: " << expected.lexically_normal() << "\n";
        std::exit(1);
    }
}

static void expect_bad(
    std::string_view source,
    const fs::path& importer,
    const fs::path& cwd) {
    try {
        (void)quidra::resolve_local_import_path(source, importer, cwd);
    } catch (const std::invalid_argument&) {
        return;
    }
    std::cerr << "unexpected import path acceptance: " << source << "\n";
    std::exit(1);
}

int main() {
    const fs::path base = fs::absolute(fs::path("quidra-import-path-test-root"));
    const fs::path cwd = base / "project";
    const fs::path importer = cwd / "src" / "main.qui";

    expect_path(
        "./geometry.qui",
        importer,
        cwd,
        quidra::ImportPathBase::ImporterDirectory,
        cwd / "src" / "geometry.qui");

    expect_path(
        "../shared/util.qui",
        importer,
        cwd,
        quidra::ImportPathBase::ImporterDirectory,
        cwd / "shared" / "util.qui");

    expect_path(
        "models@2026/a.qui",
        importer,
        cwd,
        quidra::ImportPathBase::ImporterDirectory,
        cwd / "src" / "models@2026" / "a.qui");

    expect_path(
        "./@/util.qui",
        importer,
        cwd,
        quidra::ImportPathBase::ImporterDirectory,
        cwd / "src" / "@" / "util.qui");

    expect_path(
        "@archive/util.qui",
        importer,
        cwd,
        quidra::ImportPathBase::ImporterDirectory,
        cwd / "src" / "@archive" / "util.qui");

    expect_path(
        "@/geometry.qui",
        importer,
        cwd,
        quidra::ImportPathBase::CommandWorkingDirectory,
        cwd / "geometry.qui");

    expect_path(
        "@/@/geometry.qui",
        importer,
        cwd,
        quidra::ImportPathBase::CommandWorkingDirectory,
        cwd / "@" / "geometry.qui");

    expect_path(
        "@/src/../geometry.qui",
        importer,
        cwd,
        quidra::ImportPathBase::CommandWorkingDirectory,
        cwd / "geometry.qui");

    expect_bad("", importer, cwd);
#ifdef _WIN32
    expect_bad("C:/absolute/file.qui", importer, cwd);
#else
    expect_bad("/absolute/file.qui", importer, cwd);
#endif
    expect_bad("@/", importer, cwd);
    expect_bad("@//absolute.qui", importer, cwd);
    expect_bad("@/../outside.qui", importer, cwd);
    expect_bad("./geometry.txt", importer, cwd);

    std::cout << "all import path tests passed\n";
}
