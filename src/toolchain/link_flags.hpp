#pragma once

// The link flags of a Quidra program beyond its objects and the runtime
// library, in one place for the two links that need them: the executable
// link (native_link_recipe.hpp), which names them after its output, and a C
// linker that links a Quidra library archive into a host program
// (`quidra build --lib --print-link-flags`).
//
// Owns: the system libraries every program links (the math library, threads,
// the Apple frameworks of the Metal backend or libdl), the flags that follow
// from a program's dependencies (pkg-config's libraries, the CUDA runtime,
// libcurl when the program calls the HTTP runtime), and the C++ standard
// library that a C linker, unlike the clang++ driver, does not add itself.

#include <filesystem>
#include <string>
#include <vector>

namespace quidra::toolchain {

// Whether the generated LLVM IR calls the runtime's HTTP entry points, so
// that the program links libcurl.
bool llvm_uses_http(const std::filesystem::path& llvm);

// The system libraries, in the order the executable link names them; empty
// on Windows, whose link names its libraries itself.
std::vector<std::string> system_link_libraries();

// The flags that follow from the program's dependencies, in the order the
// executable link names them after the system libraries: pkg-config's
// `--libs` output, the CUDA runtime for CUDA sources, libcurl for HTTP.
std::vector<std::string> dependency_link_flags(const std::vector<std::string>& pkg_config_libraries,
                                               bool uses_cuda, bool uses_http);

// What a C linker needs besides a Quidra library archive: the C++ standard
// library, the system libraries and the dependency flags.
std::vector<std::string> c_host_link_flags(const std::vector<std::string>& pkg_config_libraries,
                                           bool uses_cuda, bool uses_http);

} // namespace quidra::toolchain
