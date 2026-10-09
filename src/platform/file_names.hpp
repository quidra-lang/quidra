#pragma once

// How the host platform names the files a toolchain produces.

#include <string>
#include <string_view>

namespace quidra::platform {

// "stem.exe" on Windows, "stem" elsewhere.
std::string executable_file_name(std::string_view stem);
// "stem.obj" on Windows, "stem.o" elsewhere.
std::string object_file_name(std::string_view stem);
// The file name of a static library: "lib" + stem + ".a", stem + ".lib" on
// Windows.
std::string static_library_file_name(std::string_view stem);
// "name.dll" on Windows, "libname.dylib" on macOS, "libname.so" elsewhere.
std::string shared_library_file_name(std::string_view name);

} // namespace quidra::platform
