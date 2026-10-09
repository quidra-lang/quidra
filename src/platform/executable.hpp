#pragma once

// Executables: the path of the running one, and the path of a command as the
// platform's search finds it.

#include <filesystem>
#include <optional>
#include <string_view>

namespace quidra::platform {

// The path of the running executable; throws std::runtime_error when the
// platform cannot tell it.
std::filesystem::path executable_path();

// The executable a command names: a path with a directory part must be
// executable itself; a bare name is searched for in PATH (SearchPathW on
// Windows). nullopt when there is none.
std::optional<std::filesystem::path> command_path(std::string_view candidate);
bool command_available(const char* candidate);

} // namespace quidra::platform
