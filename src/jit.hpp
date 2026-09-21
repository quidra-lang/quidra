#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace quidra::jit {

int run_llvm(
    std::string_view llvm_ir,
    std::string_view argv0,
    const std::vector<std::string>& arguments = {});

// Hidden persistent worker used by the REPL. It keeps LLVM/ORC initialized
// across submissions while the parent REPL process remains isolated from
// runtime exit()/abort paths.
int run_server(
    const std::filesystem::path& stdout_path,
    const std::filesystem::path& stderr_path);

} // namespace quidra::jit
