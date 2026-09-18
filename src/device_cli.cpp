#include "device_cli.hpp"
#include "device_backend.hpp"
#include "quidra/version.hpp"

#include <iostream>

namespace quidra::cli {

int run_gpu_cli(bool verbose) {
    const auto& all = device::devices();
    if (verbose) {
        std::cout << "Quidra " << quidra::compiler_version << "\n";
        std::cout << "CPU backend: native LLVM\n";
    }
    if (all.empty()) {
        std::cout << "No supported GPU devices detected.\n";
        return 0;
    }

    for (const auto& gpu : all) {
        std::cout << "GPU " << gpu.index << "\n";
        std::cout << "  name: " << gpu.name << "\n";
        std::cout << "  backend: " << device::backend_name(gpu.backend) << "\n";
        if (!gpu.driver.empty())
            std::cout << "  driver: " << gpu.driver << "\n";
        if (!gpu.runtime.empty())
            std::cout << "  runtime: " << gpu.runtime << "\n";
        std::cout << "  placement: supported\n";
        std::cout << "  implicit CPU fallback: disabled\n";
    }
    return 0;
}

} // namespace quidra::cli
