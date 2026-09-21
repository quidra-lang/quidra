#include "device_cli.hpp"
#include "device_backend.hpp"
#include "quidra/project.hpp"

#include <iostream>

namespace quidra::cli {

int run_gpu_cli(bool verbose) {
    const auto& all = device::devices();
    if (verbose) {
        std::cout << quidra::language_name << " " << quidra::compiler_version << "\n";
        std::cout << "CPU backend: native LLVM\n";
    }
    if (all.empty()) {
        std::cout << "No supported GPUs found.\n";
        return 0;
    }

    for (const auto& gpu : all) {
        std::cout << "GPU " << gpu.index << "\n";
        std::cout << "  name: " << gpu.name << "\n";
        std::cout << "  backend: " << device::backend_display_name(gpu.backend) << "\n";
        if (!gpu.driver.empty())
            std::cout << "  driver: " << gpu.driver << "\n";
        if (!gpu.runtime.empty())
            std::cout << "  runtime: " << gpu.runtime << "\n";
        std::cout << "  " << quidra::language_name << " "
                  << device::backend_display_name(gpu.backend)
                  << " backend: " << quidra::compiler_version << "\n";
        if (gpu.backend == device::Backend::Cuda) {
            std::cout << "  CUDA Toolkit dependency: none (CUDA Driver API only)\n";
        }
        std::cout << "  status: supported\n";
        std::cout << "  placement: explicit\n";
        std::cout << "  implicit CPU fallback: disabled\n";
    }
    return 0;
}

} // namespace quidra::cli
