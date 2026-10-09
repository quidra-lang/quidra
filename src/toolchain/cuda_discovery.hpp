#pragma once

// Where the CUDA toolkit that builds package-owned .cu sources is found.
// Each lookup takes its override first and throws std::runtime_error with a
// message naming it when nothing is found.

#include <filesystem>
#include <string>

namespace quidra::toolchain {

// QUIDRA_NVCC (which must be executable), then nvcc on PATH.
std::string cuda_driver();
// QUIDRA_CUDA_HOME, CUDA_HOME or CUDA_PATH, else the directory above nvcc's.
std::filesystem::path cuda_toolkit_root();
// The directory of the CUDA runtime library under the toolkit root.
std::filesystem::path cuda_runtime_library_directory();

} // namespace quidra::toolchain
