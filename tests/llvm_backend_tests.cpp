#include "llvm_backend/runtime_prelude.hpp"
#include "llvm_backend/bare_integer.hpp"
#include "llvm_backend/small_rational.hpp"
#include "quidra/compiler.hpp"

#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>

// Gate Gprel (tests/golden/README.md): runtime_prelude_text() occurs exactly
// once, verbatim, in the LLVM module the compiler emits for a minimal program
// (tests/golden/probes/minimal.qui), through the path that `quidra llvm`
// takes. The module is emitted afresh on every run, so a prelude that the
// backend leaves out, writes twice or rewrites on its way into the module
// fails here.
//
// With --runtime-symbols, prints the symbol of every runtime entry point the
// prelude declares, and of those the support blocks of the exact type `real`
// and of the arbitrary-precision integers declare, one per line, for
// tests/runtime_abi_symbols.py, which checks that the runtime library
// defines each of them.

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--runtime-symbols") {
        for (const auto* callee : quidra::llvm_backend::runtime_prelude_declarations()) {
            const auto symbol = callee->symbol();
            std::printf("%.*s\n", static_cast<int>(symbol.size()), symbol.data());
        }
        for (const auto* callee : quidra::llvm_backend::exact_real_support_declarations()) {
            const auto symbol = callee->symbol();
            std::printf("%.*s\n", static_cast<int>(symbol.size()), symbol.data());
        }
        for (const auto* callee : quidra::llvm_backend::bare_integer_support_declarations()) {
            const auto symbol = callee->symbol();
            std::printf("%.*s\n", static_cast<int>(symbol.size()), symbol.data());
        }
        return 0;
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: quidra_llvm_backend_tests MINIMAL_QUI | --runtime-symbols\n");
        return 2;
    }

    std::string module;
    try {
        module = quidra::compile_file(std::filesystem::path(argv[1])).llvm;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "llvm backend test: cannot compile %s: %s\n", argv[1], error.what());
        return 2;
    }

    const std::string_view prelude = quidra::llvm_backend::runtime_prelude_text();
    if (prelude.empty()) {
        std::fprintf(stderr, "llvm backend test failed: the runtime prelude is empty\n");
        return 1;
    }
    std::size_t occurrences = 0;
    for (auto at = module.find(prelude); at != std::string::npos; at = module.find(prelude, at + 1)) {
        ++occurrences;
    }
    if (occurrences != 1) {
        std::fprintf(stderr,
                     "llvm backend test failed: the runtime prelude (%zu bytes) occurs %zu times in the "
                     "module emitted for %s, expected exactly once\n",
                     prelude.size(), occurrences, argv[1]);
        return 1;
    }
    return 0;
}
