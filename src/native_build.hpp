#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace quidra::native {

std::string shell_quote(std::string_view value);
std::string clang_driver();
std::string jit_driver();
std::filesystem::path runtime_library();
std::filesystem::path self_executable();
std::filesystem::path jit_runtime_library();

struct LinkOptions {
    bool debug{};
    bool optimize{true};
    std::vector<std::filesystem::path> inputs;
};

std::string debugger_driver();
int system_status(int status);
int run_program(
    const std::filesystem::path& program,
    const std::vector<std::string>& arguments = {},
    const std::optional<std::filesystem::path>& stdout_path = std::nullopt,
    const std::optional<std::filesystem::path>& stderr_path = std::nullopt);

struct JitOptions {
    bool optimize{true};
    std::string argv0{"quidra"};
};

int run_llvm_jit(
    const std::filesystem::path& llvm,
    const std::vector<std::string>& arguments = {},
    JitOptions options = {},
    const std::optional<std::filesystem::path>& stdout_path = std::nullopt,
    const std::optional<std::filesystem::path>& stderr_path = std::nullopt);
int link_llvm(
    const std::filesystem::path& llvm,
    const std::filesystem::path& output,
    LinkOptions options = {});
int run_debugger(
    const std::filesystem::path& program,
    const std::vector<std::string>& arguments = {});

} // namespace quidra::native
