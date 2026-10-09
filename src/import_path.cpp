#include "quidra/import_path.hpp"
#include "quidra/compile_inputs.hpp"
#include "platform/environment.hpp"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace quidra {

std::optional<std::filesystem::path> resolve_installed_package_path(
    std::string_view package_name, CompileInputs* inputs) {
    if (!is_importable_package_name(package_name)) {
        throw std::invalid_argument(
            "Package name must be an importable Quidra identifier and must not be a standard namespace.");
    }

    const auto candidate_from_root = [&](const std::filesystem::path& root)
        -> std::optional<std::filesystem::path> {
        if (root.empty()) return std::nullopt;
        const auto candidate =
            (root / std::string(package_name) / package_entrypoint_filename())
                .lexically_normal();
        if (probe_input_path(candidate, inputs) == InputPathState::regular_file) {
            if (inputs) inputs->record(PackageInput{std::string(package_name), candidate});
            return candidate;
        }
        return std::nullopt;
    };

    if (const auto configured = platform::environment_value("QUIDRA_PACKAGE_PATH")) {
        const std::string& paths = *configured;
#ifdef _WIN32
        constexpr char separator = ';';
#else
        constexpr char separator = ':';
#endif
        std::size_t start = 0;
        while (start <= paths.size()) {
            const auto end = paths.find(separator, start);
            const auto part = paths.substr(
                start, end == std::string::npos ? std::string::npos : end - start);
            if (!part.empty()) {
                if (auto resolved = candidate_from_root(std::filesystem::path(part))) {
                    return resolved;
                }
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }

#ifdef _WIN32
    const auto home = platform::environment_value("USERPROFILE");
#else
    const auto home = platform::environment_value("HOME");
#endif
    if (home) {
        if (auto resolved =
                candidate_from_root(
                    std::filesystem::path(*home) /
                    std::filesystem::path(std::string(package_store_relative)))) {
            return resolved;
        }
    }
    return std::nullopt;
}

} // namespace quidra
