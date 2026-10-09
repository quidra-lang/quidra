#include "toolchain/cuda_discovery.hpp"

#include "platform/environment.hpp"
#include "platform/executable.hpp"

#include <stdexcept>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace quidra::toolchain {

std::string cuda_driver() {
    if (const auto configured = platform::environment_value("QUIDRA_NVCC");
        configured && !configured->empty()) {
        if (!platform::command_path(*configured)) {
            throw std::runtime_error(
                "QUIDRA_NVCC is not executable or not on PATH: " + *configured);
        }
        return *configured;
    }
#ifdef _WIN32
    for (const char* candidate : {"nvcc.exe"}) {
#else
    for (const char* candidate : {"nvcc"}) {
#endif
        if (platform::command_available(candidate)) return candidate;
    }
    throw std::runtime_error(
        "package-owned .cu sources require NVIDIA nvcc; install the CUDA toolkit "
        "or set QUIDRA_NVCC");
}

fs::path cuda_toolkit_root() {
    for (const char* name : {"QUIDRA_CUDA_HOME", "CUDA_HOME", "CUDA_PATH"}) {
        if (const auto configured = platform::environment_value(name);
            configured && !configured->empty()) {
            const auto root = fs::absolute(*configured).lexically_normal();
            if (!fs::is_directory(root)) {
                throw std::runtime_error(
                    std::string(name) + " does not name a CUDA toolkit directory: " +
                    root.string());
            }
            return root;
        }
    }
    const auto driver = platform::command_path(cuda_driver());
    if (!driver) {
        throw std::runtime_error(
            "cannot locate nvcc to derive the CUDA toolkit root");
    }
    const auto root = driver->parent_path().parent_path().lexically_normal();
    if (!fs::is_directory(root)) {
        throw std::runtime_error(
            "cannot derive the CUDA toolkit root from nvcc: " + driver->string());
    }
    return root;
}

fs::path cuda_runtime_library_directory() {
    const auto root = cuda_toolkit_root();
#ifdef _WIN32
    const std::vector<fs::path> candidates{
        root / "lib" / "x64",
        root / "lib64",
        root / "lib"
    };
    const std::vector<std::string> names{"cudart.lib"};
#elif defined(__APPLE__)
    const std::vector<fs::path> candidates{
        root / "lib64",
        root / "lib"
    };
    const std::vector<std::string> names{"libcudart.dylib", "libcudart.a"};
#else
    const std::vector<fs::path> candidates{
        root / "lib64",
        root / "lib",
        root / "targets" / "x86_64-linux" / "lib",
        root / "targets" / "aarch64-linux" / "lib"
    };
    const std::vector<std::string> names{"libcudart.so", "libcudart_static.a"};
#endif
    for (const auto& directory : candidates) {
        for (const auto& name : names) {
            std::error_code error;
            if (fs::is_regular_file(directory / name, error) && !error)
                return fs::absolute(directory).lexically_normal();
        }
    }
    throw std::runtime_error(
        "CUDA runtime library was not found under " + root.string() +
        "; set QUIDRA_CUDA_HOME to the toolkit root");
}

} // namespace quidra::toolchain
