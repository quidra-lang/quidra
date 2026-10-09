#pragma once

// NativeText: text in the form the operating system's API takes it, the
// string type of std::filesystem::path: UTF-16 (std::wstring) on Windows,
// bytes (std::string) elsewhere. A path's native() is native text; so are
// the arguments of run_native_program (process.hpp).

#include <filesystem>
#include <string_view>

namespace quidra::platform {

using NativeText = std::filesystem::path::string_type;

// UTF-8 text as native text: converted to UTF-16 on Windows (throws
// std::runtime_error for text that is not valid UTF-8), the same bytes
// elsewhere.
NativeText native_text(std::string_view value);

} // namespace quidra::platform
