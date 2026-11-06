#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace quidra {

struct IntegerRadixMember {
    std::uint32_t radix;
    bool uppercase;
};

// Recognize only canonical b2..b36 / B2..B36 spellings. These are members
// of integer values, not reserved identifiers: class fields keep their names.
inline constexpr std::optional<IntegerRadixMember> integer_radix_member(std::string_view name) {
    if ((name.size() != 2 && name.size() != 3) ||
        (name.front() != 'b' && name.front() != 'B') ||
        name[1] < '1' || name[1] > '9' ||
        (name.size() == 3 && (name[2] < '0' || name[2] > '9')))
        return std::nullopt;
    const auto radix = static_cast<std::uint32_t>(
        name.size() == 2 ? name[1] - '0' : (name[1] - '0') * 10 + name[2] - '0');
    if (radix < 2 || radix > 36) return std::nullopt;
    return IntegerRadixMember{radix, name.front() == 'B'};
}

} // namespace quidra
