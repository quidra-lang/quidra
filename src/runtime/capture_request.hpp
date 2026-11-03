#pragma once

#include "platform/process.hpp"
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace quidra::profile {
inline std::optional<std::uint64_t> parse_capture_step(std::string_view request) noexcept {
    if (!request.starts_with("step:") || request.size() == 5) return std::nullopt;
    std::uint64_t step{};
    for (const char c : request.substr(5)) {
        if (c < '0' || c > '9') return std::nullopt;
        const auto digit = static_cast<unsigned>(c - '0');
        if (step > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            return std::nullopt;
        step = step * 10 + digit;
    }
    return step;
}

// This list is used only to start the generated program. Compilation and
// cache identity retain the inherited process environment.
inline platform::RuntimeEnvironment capture_child_environment(
        const platform::RuntimeEnvironment& inherited_overrides,
        std::string_view request, bool apple_host) {
    auto result = inherited_overrides;
    if (apple_host && parse_capture_step(request))
        result.emplace_back("MTL_CAPTURE_ENABLED", "1");
    return result;
}
} // namespace quidra::profile
