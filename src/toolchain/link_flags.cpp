#include "toolchain/link_flags.hpp"

#include "quidra/abi/symbols.hpp"
#include "toolchain/cuda_discovery.hpp"

#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace quidra::toolchain {

bool llvm_uses_http(const fs::path& llvm) {
    std::ifstream in(llvm, std::ios::binary);
    if (!in) throw std::runtime_error("cannot inspect generated LLVM IR: " + llvm.string());
    const auto prefix = "@" + std::string(abi::symbol_namespace::http_prefix);
    std::string line;
    while (std::getline(in, line)) {
        const auto call = line.find("call ");
        if (call != std::string::npos && line.find(prefix, call) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> system_link_libraries() {
#ifdef _WIN32
    return {};
#elif defined(__APPLE__)
    return {"-lm", "-pthread", "-framework", "Foundation", "-framework", "Metal",
            "-framework", "MetalPerformanceShaders"};
#else
    return {"-lm", "-pthread", "-ldl"};
#endif
}

std::vector<std::string> dependency_link_flags(const std::vector<std::string>& pkg_config_libraries,
                                               bool uses_cuda, bool uses_http) {
    std::vector<std::string> flags(pkg_config_libraries.begin(), pkg_config_libraries.end());
    if (uses_cuda) {
        const auto cuda_library = cuda_runtime_library_directory();
        flags.push_back("-L" + cuda_library.string());
#ifndef _WIN32
        flags.push_back("-Wl,-rpath," + cuda_library.string());
#endif
        flags.emplace_back("-lcudart");
    }
    if (uses_http) flags.emplace_back("-lcurl");
    return flags;
}

std::vector<std::string> c_host_link_flags(const std::vector<std::string>& pkg_config_libraries,
                                           bool uses_cuda, bool uses_http) {
    std::vector<std::string> flags;
#ifdef _WIN32
    flags.emplace_back("legacy_stdio_definitions.lib");
#elif defined(__APPLE__)
    flags.emplace_back("-lc++");
#else
    flags.emplace_back("-lstdc++");
#endif
    for (auto& flag : system_link_libraries()) flags.push_back(std::move(flag));
    for (auto& flag : dependency_link_flags(pkg_config_libraries, uses_cuda, uses_http))
        flags.push_back(std::move(flag));
    return flags;
}

} // namespace quidra::toolchain
