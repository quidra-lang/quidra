#include "package_cli.hpp"

#include "quidra/compiler.hpp"
#include "quidra/import_path.hpp"
#include "quidra/frontend.hpp"
#include "quidra/package_lock.hpp"
#include "quidra/package_manifest.hpp"
#include "quidra/project.hpp"
#include "native_build.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace quidra::cli {
namespace {

bool valid_package_name(std::string_view name) {
    return is_importable_package_name(name);
}

bool same_version(const SemanticVersion& left, const SemanticVersion& right) {
    return left.major == right.major && left.minor == right.minor &&
           left.patch == right.patch;
}

bool version_newer(const SemanticVersion& left, const SemanticVersion& right) {
    if (left.major != right.major) return left.major > right.major;
    if (left.minor != right.minor) return left.minor > right.minor;
    return left.patch > right.patch;
}

fs::path package_root() {
#ifdef _WIN32
    const auto home = import_environment_value("USERPROFILE");
#else
    const auto home = import_environment_value("HOME");
#endif
    if (!home || home->empty()) {
        throw std::runtime_error(
            "cannot determine user home for the Quidra package store");
    }
    return fs::path(*home) / ".quidra" / "packages";
}

std::string read_text_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

std::string json_escape(std::string_view value) {
    std::ostringstream output;
    const char* hex = "0123456789abcdef";
    for (const unsigned char c : value) {
        switch (c) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (c < 0x20U) {
                    output << "\\u00" << hex[(c >> 4U) & 0xfU] << hex[c & 0xfU];
                } else {
                    output << static_cast<char>(c);
                }
        }
    }
    return output.str();
}

void write_json_optional(
    std::ostream& output, const std::optional<std::string>& value) {
    if (value) output << '"' << json_escape(*value) << '"';
    else output << "null";
}

void write_lock_file(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write " + path.string());
    output << text;
    output.flush();
    if (!output) {
        throw std::runtime_error("cannot finish writing " + path.string());
    }
}

fs::path unique_temp_path(std::string_view prefix) {
    std::random_device random;
    for (int attempt = 0; attempt < 32; ++attempt) {
        const auto path =
            fs::temp_directory_path() /
            (std::string(prefix) + std::to_string(random()) + "-" +
             std::to_string(random()));
        std::error_code error;
        if (!fs::exists(path, error) && !error) return path;
    }
    throw std::runtime_error("cannot allocate temporary package path");
}

class TempDirectory {
public:
    explicit TempDirectory(std::string_view prefix)
        : path_(unique_temp_path(prefix)) {
        fs::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void require_package_source(const fs::path& source) {
    std::error_code error;
    if (!fs::is_directory(source, error) || error) {
        throw std::runtime_error(
            "package source must be a directory: " + source.string());
    }
    const auto main = source / "main.qui";
    if (!fs::is_regular_file(main, error) || error) {
        throw std::runtime_error("package source must contain main.qui");
    }
}

void copy_package_tree(
    const fs::path& source, const fs::path& destination) {
    fs::create_directories(destination);
    for (fs::recursive_directory_iterator iterator(source), end;
         iterator != end; ++iterator) {
        const auto relative = fs::relative(iterator->path(), source);
        if (!relative.empty() && *relative.begin() == ".git") {
            if (iterator->is_directory()) iterator.disable_recursion_pending();
            continue;
        }

        const auto status = iterator->symlink_status();
        if (fs::is_symlink(status)) {
            throw std::runtime_error(
                "package install rejects symbolic links: " +
                iterator->path().string());
        }

        const auto target = destination / relative;
        if (fs::is_directory(status)) {
            fs::create_directories(target);
        } else if (fs::is_regular_file(status)) {
            fs::create_directories(target.parent_path());
            fs::copy_file(
                iterator->path(), target,
                fs::copy_options::overwrite_existing);
        } else {
            throw std::runtime_error(
                "package contains unsupported filesystem entry: " +
                iterator->path().string());
        }
    }
}

void publish_package(
    const fs::path& source, const std::string& name, bool replace) {
    const auto root = package_root();
    fs::create_directories(root);
    const auto target = root / name;

    std::error_code error;
    if (fs::exists(target, error) && !error && !replace) {
        throw std::runtime_error(
            "package is already installed; pass --force to replace it");
    }

    std::random_device random;
    const auto suffix =
        std::to_string(random()) + "-" + std::to_string(random());
    const auto temporary =
        root / ("." + name + ".install-" + suffix);
    const auto backup =
        root / ("." + name + ".backup-" + suffix);

    try {
        copy_package_tree(source, temporary);
        if (!fs::is_regular_file(temporary / "main.qui")) {
            throw std::runtime_error("copied package lost main.qui");
        }

        const bool had_target = fs::exists(target);
        if (had_target) fs::rename(target, backup);
        try {
            fs::rename(temporary, target);
        } catch (...) {
            if (had_target && fs::exists(backup) &&
                !fs::exists(target)) {
                fs::rename(backup, target);
            }
            throw;
        }
        if (had_target) fs::remove_all(backup);
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(temporary, ignored);
        if (fs::exists(backup, ignored) &&
            !fs::exists(target, ignored)) {
            std::error_code restore_error;
            fs::rename(backup, target, restore_error);
        }
        throw;
    }
}

SemanticVersion current_quidra_version() {
    return parse_semantic_version(compiler_version);
}

void validate_manifest_name(const PackageManifest& manifest) {
    if (!valid_package_name(manifest.name)) {
        throw std::runtime_error(
            "package manifest name must be an importable Quidra identifier "
            "and must not be a standard namespace");
    }
}

const VersionRequirement& require_quidra_requirement(
    const PackageManifest& manifest) {
    const auto found = manifest.requirements.find("quidra");
    if (found == manifest.requirements.end()) {
        throw std::runtime_error(
            "release package manifest must declare requires.quidra");
    }
    return found->second;
}

void require_current_quidra(const PackageManifest& manifest) {
    const auto found = manifest.requirements.find("quidra");
    if (found == manifest.requirements.end()) return;

    if (!found->second.matches(current_quidra_version())) {
        throw std::runtime_error(
            manifest.name + " " + manifest.version.str() +
            " requires Quidra " + found->second.text +
            "; installed Quidra is " + std::string(compiler_version));
    }
}

void require_package_dependencies(const PackageManifest& manifest) {
    for (const auto& [name, requirement] : manifest.requirements) {
        if (name == "quidra") continue;

        if (!valid_package_name(name)) {
            throw std::runtime_error(
                "invalid package dependency name in quidra.package: " +
                name);
        }

        const auto root = package_root() / name;
        const auto dependency = try_read_package_manifest(root);
        if (!dependency) {
            throw std::runtime_error(
                manifest.name + " " + manifest.version.str() +
                " requires package " + name + " " + requirement.text +
                "; install a compatible " + name + " release first");
        }

        if (!requirement.matches(dependency->version)) {
            throw std::runtime_error(
                manifest.name + " " + manifest.version.str() +
                " requires package " + name + " " + requirement.text +
                "; installed version is " +
                dependency->version.str());
        }
    }
}

std::string run_command_capture(
    const fs::path& program,
    const std::vector<std::string>& arguments,
    std::string_view failure) {
    TempDirectory temp("quidra-command-");
    const auto output = temp.path() / "stdout.txt";
    const auto error = temp.path() / "stderr.txt";

    const int status =
        native::run_program(program, arguments, output, error);
    if (status != 0) {
        auto detail = read_text_file(error);
        if (detail.empty()) detail = read_text_file(output);
        while (!detail.empty() &&
               (detail.back() == '\n' || detail.back() == '\r')) {
            detail.pop_back();
        }
        throw std::runtime_error(
            std::string(failure) +
            (detail.empty() ? std::string() : ": " + detail));
    }

    return read_text_file(output);
}

std::string run_git_capture(
    const std::vector<std::string>& arguments) {
    return run_command_capture(
        fs::path("git"), arguments, "git command failed");
}

std::optional<std::string> package_asset_platform() {
#if defined(_WIN32)
#  if defined(_M_X64) || defined(__x86_64__)
    return "windows-x86_64";
#  elif defined(_M_ARM64) || defined(__aarch64__)
    return "windows-arm64";
#  else
    return std::nullopt;
#  endif
#elif defined(__APPLE__)
#  if defined(__aarch64__)
    return "macos-arm64";
#  elif defined(__x86_64__)
    return "macos-x86_64";
#  else
    return std::nullopt;
#  endif
#elif defined(__linux__)
#  if defined(__x86_64__)
    return "linux-x86_64";
#  elif defined(__aarch64__)
    return "linux-arm64";
#  else
    return std::nullopt;
#  endif
#else
    return std::nullopt;
#endif
}

std::string asset_filename(std::string_view url) {
    const auto query = url.find_first_of("?#");
    if (query != std::string_view::npos) url = url.substr(0, query);
    const auto slash = url.find_last_of('/');
    const auto name =
        std::string(slash == std::string_view::npos ? url : url.substr(slash + 1));
    if (name.empty() || name == "." || name == "..") {
        throw std::runtime_error("package asset URL has no safe filename");
    }
    for (const unsigned char c : name) {
        if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_')) {
            throw std::runtime_error(
                "package asset filename contains an unsafe character: " + name);
        }
    }
    return name;
}

void validate_asset_url(std::string_view url) {
    if (url.starts_with("https://")) return;
    if (url.starts_with("file://")) {
        const auto allowed =
            import_environment_value("QUIDRA_ALLOW_FILE_PACKAGE_ASSETS");
        if (allowed && *allowed == "1") return;
    }
    throw std::runtime_error(
        "package release assets must use HTTPS");
}

void validate_asset_archive_listing(std::string_view listing) {
    std::istringstream lines{std::string(listing)};
    std::string entry;
    std::size_t count = 0;
    while (std::getline(lines, entry)) {
        if (!entry.empty() && entry.back() == '\r') entry.pop_back();
        if (entry.empty()) continue;
        ++count;

        while (!entry.empty() && entry.back() == '/') entry.pop_back();
        if (entry.empty() || entry.front() == '/' || entry.front() == '\\' ||
            entry.find('\\') != std::string::npos ||
            entry.find(':') != std::string::npos) {
            throw std::runtime_error(
                "package asset archive contains an unsafe path");
        }

        std::istringstream parts(entry);
        std::string part;
        bool first = true;
        while (std::getline(parts, part, '/')) {
            if (part.empty() || part == "." || part == "..") {
                throw std::runtime_error(
                    "package asset archive contains an unsafe path");
            }
            if (first) {
                if (part == ".git" || part == "main.qui" ||
                    part == "quidra.package" || part == "project.toml" ||
                    part == "quidra.lock") {
                    throw std::runtime_error(
                        "package asset archive may not replace package identity files");
                }
                first = false;
            }
        }
    }
    if (count == 0) {
        throw std::runtime_error("package asset archive is empty");
    }
}

void validate_asset_archive_types(std::string_view verbose_listing) {
    std::istringstream lines{std::string(verbose_listing)};
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const char type = line.front();
        if (type != '-' && type != 'd') {
            throw std::runtime_error(
                "package asset archive may contain only regular files and directories");
        }
    }
}

void hydrate_release_asset(
    const PackageManifest& manifest,
    const fs::path& source) {
    const auto platform = package_asset_platform();

    const std::string* url = nullptr;
    if (platform) {
        const auto found = manifest.assets.find(*platform);
        if (found != manifest.assets.end()) url = &found->second;
    }
    if (!url) {
        const auto fallback = manifest.assets.find("default");
        if (fallback != manifest.assets.end()) url = &fallback->second;
    }
    if (!url) return;

    validate_asset_url(*url);
    TempDirectory download("quidra-asset-");
    const auto archive = download.path() / asset_filename(*url);

    std::vector<std::string> curl_args{
        "--fail", "--location", "--silent", "--show-error",
        "--output", archive.string(), *url};
    (void)run_command_capture(
        fs::path("curl"), curl_args,
        "package release asset download failed");

    const auto listing = run_command_capture(
        fs::path("tar"), {"-tf", archive.string()},
        "package release asset listing failed");
    validate_asset_archive_listing(listing);

    const auto verbose = run_command_capture(
        fs::path("tar"), {"-tvf", archive.string()},
        "package release asset type inspection failed");
    validate_asset_archive_types(verbose);

    TempDirectory extracted("quidra-asset-extract-");
    (void)run_command_capture(
        fs::path("tar"),
        {"-xf", archive.string(), "-C", extracted.path().string()},
        "package release asset extraction failed");

    copy_package_tree(extracted.path(), source);
}

void clone_release(
    const std::string& repository,
    const SemanticVersion& version,
    const fs::path& destination) {
    const auto tag = "v" + version.str();
    (void)run_git_capture({
        "clone",
        "--quiet",
        "--depth",
        "1",
        "--single-branch",
        "--branch",
        tag,
        repository,
        destination.string()});
}

std::vector<SemanticVersion> release_versions(
    const std::string& repository) {
    const auto output =
        run_git_capture({"ls-remote", "--tags", "--refs", repository});

    std::vector<SemanticVersion> versions;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        const auto marker = line.find("refs/tags/v");
        if (marker == std::string::npos) continue;

        const auto text =
            line.substr(
                marker + std::string("refs/tags/v").size());
        try {
            versions.push_back(parse_semantic_version(text));
        } catch (const std::exception&) {
            // Only exact stable vMAJOR.MINOR.PATCH tags are installable.
        }
    }

    std::sort(versions.begin(), versions.end(), version_newer);
    versions.erase(
        std::unique(
            versions.begin(), versions.end(), same_version),
        versions.end());
    return versions;
}

struct RemoteSpec {
    std::string repository;
    std::string expected_name;
    std::optional<SemanticVersion> requested_version;
};

std::string repository_basename(std::string value) {
    while (!value.empty() &&
           (value.back() == '/' || value.back() == '\\')) {
        value.pop_back();
    }

    const auto slash = value.find_last_of("/\\:");
    if (slash != std::string::npos) {
        value = value.substr(slash + 1);
    }

    if (value.ends_with(".git")) {
        value.resize(value.size() - 4);
    }
    return value;
}

RemoteSpec parse_remote_spec(std::string spec) {
    std::optional<SemanticVersion> requested;

    const auto at = spec.rfind('@');
    if (at != std::string::npos && at + 1 < spec.size()) {
        std::string version_text = spec.substr(at + 1);
        if (!version_text.empty() &&
            version_text.front() == 'v') {
            version_text.erase(version_text.begin());
        }

        try {
            requested = parse_semantic_version(version_text);
            spec.resize(at);
        } catch (const std::exception&) {
            // '@' can also be part of an SSH Git URL.
        }
    }

    if (spec.empty()) {
        throw std::runtime_error(
            "remote package source cannot be empty");
    }

    std::string repository;
    if (spec.find("://") != std::string::npos ||
        spec.starts_with("git@")) {
        repository = spec;
    } else if (spec.find('/') != std::string::npos) {
        repository = "https://github.com/" + spec;
        if (!repository.ends_with(".git")) {
            repository += ".git";
        }
    } else {
        if (!valid_package_name(spec)) {
            throw std::runtime_error(
                "invalid package name: " + spec);
        }
        repository =
            "https://github.com/quidra-lang/" + spec + ".git";
    }

    const auto expected_name = repository_basename(spec);
    if (!valid_package_name(expected_name)) {
        throw std::runtime_error(
            "repository name is not an importable Quidra package "
            "name: " +
            expected_name);
    }

    return RemoteSpec{
        repository, expected_name, requested};
}

void validate_release_manifest(
    const PackageManifest& manifest,
    const std::string& expected_name,
    const SemanticVersion& tag_version) {
    validate_manifest_name(manifest);

    if (manifest.name != expected_name) {
        throw std::runtime_error(
            "release manifest name '" + manifest.name +
            "' does not match repository package name '" +
            expected_name + "'");
    }

    if (!same_version(manifest.version, tag_version)) {
        throw std::runtime_error(
            "release tag v" + tag_version.str() +
            " does not match quidra.package version " +
            manifest.version.str());
    }

    (void)require_quidra_requirement(manifest);
}

void install_remote(std::string_view raw_spec) {
    const auto spec =
        parse_remote_spec(std::string(raw_spec));
    const auto versions =
        release_versions(spec.repository);

    if (versions.empty()) {
        throw std::runtime_error(
            "repository has no installable "
            "vMAJOR.MINOR.PATCH release tags");
    }

    std::vector<SemanticVersion> candidates;
    if (spec.requested_version) {
        const auto found =
            std::find_if(
                versions.begin(), versions.end(),
                [&](const auto& version) {
                    return same_version(
                        version, *spec.requested_version);
                });

        if (found == versions.end()) {
            throw std::runtime_error(
                "release tag v" +
                spec.requested_version->str() +
                " does not exist");
        }

        candidates.push_back(*found);
    } else {
        candidates = versions;
    }

    TempDirectory temp("quidra-install-");
    std::optional<PackageManifest> selected_manifest;
    std::optional<SemanticVersion> selected_version;
    fs::path selected_source;
    std::string newest_requirement;

    for (const auto& version : candidates) {
        const auto source =
            temp.path() / ("source-" + version.str());

        clone_release(
            spec.repository, version, source);
        require_package_source(source);

        auto manifest =
            read_package_manifest(source);
        validate_release_manifest(
            manifest, spec.expected_name, version);

        const auto& quidra_requirement =
            require_quidra_requirement(manifest);

        if (!quidra_requirement.matches(
                current_quidra_version())) {
            if (newest_requirement.empty()) {
                newest_requirement =
                    quidra_requirement.text;
            }

            std::error_code ignored;
            fs::remove_all(source, ignored);

            if (spec.requested_version) {
                throw std::runtime_error(
                    manifest.name + " " +
                    manifest.version.str() +
                    " requires Quidra " +
                    quidra_requirement.text +
                    "; installed Quidra is " +
                    std::string(compiler_version));
            }
            continue;
        }

        selected_manifest = std::move(manifest);
        selected_version = version;
        selected_source = source;
        break;
    }

    if (!selected_manifest || !selected_version) {
        std::string message =
            "no release of " + spec.expected_name +
            " is compatible with Quidra " +
            std::string(compiler_version);

        if (!newest_requirement.empty()) {
            message +=
                "; newest release requires Quidra " +
                newest_requirement;
        }
        throw std::runtime_error(message);
    }

    require_package_dependencies(*selected_manifest);
    (void)check_file(
        selected_source / "main.qui", {}, selected_source);
    hydrate_release_asset(*selected_manifest, selected_source);

    publish_package(
        selected_source, selected_manifest->name, true);

    std::cout
        << "installed " << selected_manifest->name
        << " " << selected_manifest->version.str()
        << " -> "
        << (package_root() / selected_manifest->name).string()
        << "\n";
}

void install_local(
    const fs::path& source,
    std::optional<std::string> requested_name,
    bool force) {
    const auto absolute =
        fs::absolute(source).lexically_normal();
    require_package_source(absolute);

    const auto manifest =
        try_read_package_manifest(absolute);

    std::string name;
    if (manifest) {
        validate_manifest_name(*manifest);
        require_current_quidra(*manifest);
        require_package_dependencies(*manifest);

        if (requested_name &&
            *requested_name != manifest->name) {
            throw std::runtime_error(
                "--name does not match quidra.package name '" +
                manifest->name + "'");
        }

        name = manifest->name;
    } else if (requested_name) {
        name = *requested_name;
    } else {
        name = absolute.filename().string();
        if (name.empty()) {
            name =
                absolute.parent_path().filename().string();
        }
    }

    if (!valid_package_name(name)) {
        throw std::runtime_error(
            "package name must be an importable Quidra "
            "identifier and must not be a standard namespace");
    }

    (void)check_file(
        absolute / "main.qui", {}, absolute);

    publish_package(
        absolute, name, force);

    std::cout << "installed " << name;
    if (manifest) {
        std::cout << " " << manifest->version.str();
    }
    std::cout
        << " -> " << (package_root() / name).string()
        << "\n";
}

void remove_package(std::string_view name) {
    if (!valid_package_name(name)) {
        throw std::runtime_error("invalid package name");
    }

    const auto target =
        package_root() / std::string(name);
    std::error_code error;

    if (!fs::is_directory(target, error) || error) {
        throw std::runtime_error(
            "package is not installed: " +
            std::string(name));
    }

    fs::remove_all(target, error);
    if (error) {
        throw std::runtime_error(
            "cannot remove package: " + error.message());
    }

    std::cout << "removed " << name << "\n";
}

void list_packages() {
    const auto root = package_root();
    std::error_code error;
    if (!fs::is_directory(root, error) || error) return;

    std::vector<std::string> lines;
    for (const auto& entry :
         fs::directory_iterator(root)) {
        if (!entry.is_directory()) continue;

        const auto name =
            entry.path().filename().string();
        if (name.empty() || name.front() == '.' ||
            !fs::is_regular_file(
                entry.path() / "main.qui")) {
            continue;
        }

        auto line = name;
        if (const auto manifest =
                try_read_package_manifest(entry.path())) {
            line += " " + manifest->version.str();
        }
        lines.push_back(std::move(line));
    }

    std::sort(lines.begin(), lines.end());
    for (const auto& line : lines) {
        std::cout << line << "\n";
    }
}

void package_info(std::string_view raw_name, bool json) {
    if (!valid_package_name(raw_name)) {
        throw std::runtime_error("invalid package name");
    }
    const std::string name(raw_name);
    const auto root = package_root() / name;
    std::error_code error;
    if (!fs::is_directory(root, error) || error ||
        !fs::is_regular_file(root / "main.qui", error) || error) {
        throw std::runtime_error("package is not installed: " + name);
    }

    const auto manifest = try_read_package_manifest(root);
    if (json) {
        std::cout << "{\"name\":\""
                  << json_escape(manifest ? manifest->name : name)
                  << "\",\"version\":";
        if (manifest) std::cout << "\"" << manifest->version.str() << "\"";
        else std::cout << "null";
        std::cout << ",\"repository\":";
        write_json_optional(std::cout, manifest ? manifest->repository : std::optional<std::string>{});
        std::cout << ",\"description\":";
        write_json_optional(std::cout, manifest ? manifest->description : std::optional<std::string>{});
        std::cout << ",\"license\":";
        write_json_optional(std::cout, manifest ? manifest->license : std::optional<std::string>{});
        std::cout << ",\"homepage\":";
        write_json_optional(std::cout, manifest ? manifest->homepage : std::optional<std::string>{});
        std::cout << ",\"assets\":{";
        if (manifest) {
            bool first = true;
            for (const auto& [platform, url] : manifest->assets) {
                if (!first) std::cout << ',';
                first = false;
                std::cout << "\"" << json_escape(platform) << "\":\""
                          << json_escape(url) << "\"";
            }
        }
        std::cout << "},\"requirements\":{";
        if (manifest) {
            bool first = true;
            for (const auto& [dependency, requirement] : manifest->requirements) {
                if (!first) std::cout << ',';
                first = false;
                std::cout << "\"" << json_escape(dependency) << "\":\""
                          << json_escape(requirement.text) << "\"";
            }
        }
        std::cout << "},\"path\":\""
                  << json_escape(root.string()) << "\"}\n";
        return;
    }

    std::cout << "name = " << (manifest ? manifest->name : name) << "\n";
    if (manifest) {
        std::cout << "version = " << manifest->version.str() << "\n";
        if (manifest->description) std::cout << "description = " << *manifest->description << "\n";
        if (manifest->license) std::cout << "license = " << *manifest->license << "\n";
        if (manifest->homepage) std::cout << "homepage = " << *manifest->homepage << "\n";
        if (manifest->repository) std::cout << "repository = " << *manifest->repository << "\n";
        for (const auto& [platform, url] : manifest->assets) {
            std::cout << "asset." << platform << " = " << url << "\n";
        }
        for (const auto& [dependency, requirement] : manifest->requirements) {
            std::cout << "requires." << dependency << " = " << requirement.text << "\n";
        }
    } else {
        std::cout << "version = unversioned\n";
    }
    std::cout << "path = " << root.string() << "\n";
}

int lock_packages(
    const fs::path& source, bool check_only) {
    const auto absolute =
        fs::absolute(source).lexically_normal();
    std::error_code error;

    if (!fs::is_regular_file(absolute, error) ||
        error || absolute.extension() != ".qui") {
        throw std::runtime_error(
            "package lock requires an existing "
            ".qui root source file");
    }

    const auto cwd = fs::current_path();
    const auto packages =
        resolve_package_dependencies(absolute, cwd);
    const auto expected =
        package_lock_text(packages);
    const auto path =
        package_lock_path(cwd);

    if (check_only) {
        if (read_text_file(path) == expected) return 0;
        std::cerr
            << "quidra: quidra.lock is missing or out of date\n";
        return 1;
    }

    write_lock_file(path, expected);
    std::cout
        << "locked " << packages.size()
        << " package(s) -> " << path.string()
        << "\n";
    return 0;
}

bool looks_like_local_path(std::string_view source) {
    return source.starts_with(".") ||
           source.starts_with("/") ||
           source.starts_with("\\") ||
           (source.size() >= 2 && source[1] == ':');
}

void usage() {
    std::cerr
        << "usage:\n"
        << "  quidra install PACKAGE[@VERSION]\n"
        << "  quidra install OWNER/PACKAGE[@VERSION]\n"
        << "  quidra install GIT_URL[@VERSION]\n"
        << "  quidra install DIR [--name NAME] [--force]\n"
        << "  quidra lock FILE.qui [--check]\n"
        << "  quidra remove NAME\n"
        << "  quidra list\n"
        << "  quidra package-info NAME [--json]\n"
        << "  quidra package-path\n";
}

} // namespace

int run_package_cli(int argc, char** argv) {
    try {
        if (argc < 1) {
            usage();
            return 2;
        }

        const std::string command = argv[0];

        if (command == "list") {
            if (argc != 1) {
                throw std::runtime_error(
                    "list takes no arguments");
            }
            list_packages();
            return 0;
        }

        if (command == "info" ||
            command == "package-info") {
            if (argc != 2 && argc != 3) {
                throw std::runtime_error(
                    "package-info expects NAME and optional --json");
            }
            bool json = false;
            if (argc == 3) {
                if (std::string(argv[2]) != "--json") {
                    throw std::runtime_error(
                        "unknown package-info option: " + std::string(argv[2]));
                }
                json = true;
            }
            package_info(argv[1], json);
            return 0;
        }

        if (command == "path" ||
            command == "package-path") {
            if (argc != 1) {
                throw std::runtime_error(
                    "package-path takes no arguments");
            }
            std::cout << package_root().string() << "\n";
            return 0;
        }

        if (command == "remove") {
            if (argc != 2) {
                throw std::runtime_error(
                    "remove requires exactly one package name");
            }
            remove_package(argv[1]);
            return 0;
        }

        if (command == "lock") {
            if (argc < 2 || argc > 3) {
                throw std::runtime_error(
                    "lock expects FILE.qui and optional --check");
            }

            bool check_only = false;
            if (argc == 3) {
                if (std::string(argv[2]) != "--check") {
                    throw std::runtime_error(
                        "unknown lock option: " +
                        std::string(argv[2]));
                }
                check_only = true;
            }

            return lock_packages(
                argv[1], check_only);
        }

        if (command == "install") {
            if (argc < 2) {
                throw std::runtime_error(
                    "install requires a package name, "
                    "repository, or source directory");
            }

            const std::string source_text = argv[1];
            std::optional<std::string> name;
            bool force = false;

            for (int index = 2; index < argc; ++index) {
                const std::string option = argv[index];
                if (option == "--force") {
                    force = true;
                } else if (
                    option == "--name" &&
                    index + 1 < argc) {
                    name = argv[++index];
                } else {
                    throw std::runtime_error(
                        "unknown install option: " +
                        option);
                }
            }

            std::error_code error;
            const fs::path source_path = source_text;
            const bool local =
                fs::is_directory(source_path, error) &&
                !error;

            if (local ||
                looks_like_local_path(source_text)) {
                install_local(
                    source_path,
                    std::move(name),
                    force);
            } else {
                if (name) {
                    throw std::runtime_error(
                        "--name is only valid for local "
                        "directory installs");
                }
                install_remote(source_text);
            }
            return 0;
        }

        throw std::runtime_error(
            "unknown package command: " + command);
    } catch (const CompileErrors& errors) {
        for (const auto& diagnostic :
             errors.diagnostics()) {
            std::cerr
                << "error[" << diagnostic.code
                << "] " << diagnostic.message << "\n";
        }
        return 1;
    } catch (const CompileError& error) {
        std::cerr
            << "error[" << error.diagnostic().code
            << "] " << error.diagnostic().message
            << "\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr
            << "quidra: " << error.what() << "\n";
        return 1;
    }
}

} // namespace quidra::cli
