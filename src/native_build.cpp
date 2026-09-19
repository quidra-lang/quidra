#include "native_build.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <optional>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace quidra::native {
namespace {

std::optional<std::string> environment_value(const char* name) {
#ifdef _WIN32
    char* raw = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&raw, &size, name) != 0 || !raw) return std::nullopt;
    std::string value(raw, size > 0 ? size - 1 : 0);
    std::free(raw);
    return value;
#else
    if (const char* raw = std::getenv(name)) return std::string(raw);
    return std::nullopt;
#endif
}

#ifdef _WIN32
std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (needed <= 0) throw std::runtime_error("invalid UTF-8 in process argument");
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            wide.data(), needed) != needed) {
        throw std::runtime_error("cannot convert process argument to UTF-16");
    }
    return wide;
}

std::wstring windows_quote_wide(std::wstring_view value) {
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::string windows_quote(std::string_view value) {
    const auto wide = windows_quote_wide(utf8_to_wide(value));
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) throw std::runtime_error("cannot quote Windows process argument");
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        out.data(), needed, nullptr, nullptr);
    return out;
}

int windows_process(
    const fs::path& program,
    const std::vector<std::wstring>& arguments,
    const std::optional<fs::path>& stdout_path = std::nullopt,
    const std::optional<fs::path>& stderr_path = std::nullopt) {
    std::wstring command = windows_quote_wide(program.native());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += windows_quote_wide(argument);
    }
    command.push_back(L'\0');

    HANDLE output = INVALID_HANDLE_VALUE;
    HANDLE error = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    auto open_redirect = [&](const std::optional<fs::path>& path) -> HANDLE {
        if (!path) return INVALID_HANDLE_VALUE;
        return CreateFileW(
            path->c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    };

    output = open_redirect(stdout_path);
    if (stdout_path && output == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("cannot open process stdout file");
    }
    error = open_redirect(stderr_path);
    if (stderr_path && error == INVALID_HANDLE_VALUE) {
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        throw std::runtime_error("cannot open process stderr file");
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    BOOL inherit = FALSE;
    if (stdout_path || stderr_path) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput =
            output != INVALID_HANDLE_VALUE ? output : GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError =
            error != INVALID_HANDLE_VALUE ? error : GetStdHandle(STD_ERROR_HANDLE);
        inherit = TRUE;
    }

    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        nullptr, command.data(), nullptr, nullptr, inherit, 0, nullptr, nullptr,
        &startup, &process);
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();

    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (error != INVALID_HANDLE_VALUE) CloseHandle(error);

    if (!created) {
        throw std::runtime_error(
            "cannot start process (Windows error " + std::to_string(create_error) + ")");
    }

    const DWORD wait = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    const BOOL got_exit =
        wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (!got_exit) throw std::runtime_error("cannot wait for process completion");
    return static_cast<int>(exit_code);
}
#endif

fs::path executable_path() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        throw std::runtime_error("cannot locate the Quidra executable");
    }
    buffer.resize(length);
    return fs::path(buffer);
#elif defined(__APPLE__)
    std::uint32_t size = 4096;
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.resize(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
            throw std::runtime_error("cannot locate the Quidra executable");
        }
    }
    std::error_code ec;
    const auto canonical = fs::weakly_canonical(fs::path(buffer.data()), ec);
    return ec ? fs::path(buffer.data()) : canonical;
#else
    std::array<char, 4096> buffer{};
    const auto count = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (count <= 0) throw std::runtime_error("cannot locate the Quidra executable");
    return fs::path(std::string(buffer.data(), static_cast<std::size_t>(count)));
#endif
}

bool command_available(const char* candidate) {
#ifdef _WIN32
    const auto wide = utf8_to_wide(candidate);
    std::wstring buffer(32768, L'\0');
    const DWORD length = SearchPathW(
        nullptr, wide.c_str(), nullptr, static_cast<DWORD>(buffer.size()),
        buffer.data(), nullptr);
    return length > 0 && length < buffer.size();
#else
    const fs::path requested(candidate);
    if (requested.has_parent_path()) return ::access(candidate, X_OK) == 0;
    const char* raw_path = std::getenv("PATH");
    if (!raw_path) return false;
    std::string_view path(raw_path);
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find(':', start);
        const auto part = path.substr(
            start, end == std::string_view::npos ? path.size() - start : end - start);
        const fs::path directory = part.empty() ? fs::path(".") : fs::path(std::string(part));
        const auto executable = directory / candidate;
        if (::access(executable.c_str(), X_OK) == 0) return true;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return false;
#endif
}


#ifndef _WIN32
int posix_process(
    const fs::path& program,
    const std::vector<std::string>& arguments,
    const std::optional<fs::path>& stdout_path = std::nullopt,
    const std::optional<fs::path>& stderr_path = std::nullopt) {
    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1);
    storage.push_back(program.string());
    storage.insert(storage.end(), arguments.begin(), arguments.end());

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& argument : storage) argv.push_back(argument.data());
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) throw std::runtime_error("failed to fork process");
    if (pid == 0) {
        const auto redirect = [](const std::optional<fs::path>& path, int target) {
            if (!path) return;
            const int fd = ::open(path->c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd < 0 || ::dup2(fd, target) < 0) _exit(126);
            ::close(fd);
        };
        redirect(stdout_path, STDOUT_FILENO);
        redirect(stderr_path, STDERR_FILENO);
        ::execvp(program.c_str(), argv.data());
        _exit(errno == ENOENT ? 127 : 126);
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        throw std::runtime_error("failed to wait for process");
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}
#endif

} // namespace

std::string shell_quote(std::string_view value) {
#ifdef _WIN32
    return windows_quote(value);
#else
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
#endif
}

std::string clang_driver() {
    if (const auto configured = environment_value("QUIDRA_CLANGXX");
        configured && !configured->empty()) {
        return *configured;
    }
    if (const auto configured = environment_value("QUIDRA_CLANG");
        configured && !configured->empty()) {
        return *configured;
    }
#ifdef _WIN32
    for (const char* candidate :
         {"clang++.exe", "clang++-20.exe", "clang++-19.exe", "clang++-18.exe", "clang++-17.exe"}) {
#else
    for (const char* candidate :
         {"clang++-20", "clang++-19", "clang++-18", "clang++-17", "clang++-16", "clang++-15", "clang++"}) {
#endif
        if (command_available(candidate)) return candidate;
    }
    throw std::runtime_error("Clang++ 15 or newer is required for native code generation");
}

std::string debugger_driver() {
    if (const auto configured = environment_value("QUIDRA_DEBUGGER");
        configured && !configured->empty()) {
        if (!command_available(configured->c_str())) {
            throw std::runtime_error(
                "QUIDRA_DEBUGGER is not executable or not on PATH: " + *configured);
        }
        return *configured;
    }
#ifdef _WIN32
    for (const char* candidate : {"lldb.exe", "gdb.exe"}) {
#else
    for (const char* candidate : {"lldb", "gdb"}) {
#endif
        if (command_available(candidate)) return candidate;
    }
    throw std::runtime_error(
        "no supported debugger found; install lldb/gdb or set QUIDRA_DEBUGGER");
}

fs::path runtime_library() {
    if (const auto configured = environment_value("QUIDRA_RUNTIME_LIBRARY");
        configured && !configured->empty()) {
        fs::path path = *configured;
        if (fs::is_regular_file(path)) return path;
        throw std::runtime_error("QUIDRA_RUNTIME_LIBRARY does not name a file: " + path.string());
    }

    const auto bin = executable_path().parent_path();
#ifdef _WIN32
    const char* library_name = "quidra_runtime.lib";
#else
    const char* library_name = "libquidra_runtime.a";
#endif

    const std::array<fs::path, 5> candidates{{
        bin / library_name,
        (bin / "../lib/quidra" / library_name).lexically_normal(),
        (bin / "../lib64/quidra" / library_name).lexically_normal(),
        (bin / "../lib/x86_64-linux-gnu/quidra" / library_name).lexically_normal(),
        (bin / "../lib/aarch64-linux-gnu/quidra" / library_name).lexically_normal(),
    }};
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && !ec) return candidate;
    }

    throw std::runtime_error(
        "cannot locate the Quidra runtime library; set QUIDRA_RUNTIME_LIBRARY explicitly");
}

bool llvm_calls_symbol_prefix(const fs::path& llvm, std::string_view prefix) {
    std::ifstream in(llvm, std::ios::binary);
    if (!in) throw std::runtime_error("cannot inspect generated LLVM IR: " + llvm.string());
    std::string line;
    while (std::getline(in, line)) {
        const auto call = line.find("call ");
        if (call != std::string::npos && line.find(prefix, call) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool llvm_uses_http(const fs::path& llvm) {
    return llvm_calls_symbol_prefix(llvm, "@quidra_http_");
}

bool llvm_uses_image(const fs::path& llvm) {
    return llvm_calls_symbol_prefix(llvm, "@quidra_image_");
}

int system_status(int status) {
    if (status == -1) return -1;
#ifdef _WIN32
    return status;
#else
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
#endif
}

int run_program(
    const fs::path& program,
    const std::vector<std::string>& arguments,
    const std::optional<fs::path>& stdout_path,
    const std::optional<fs::path>& stderr_path) {
#ifdef _WIN32
    std::vector<std::wstring> wide_arguments;
    wide_arguments.reserve(arguments.size());
    for (const auto& argument : arguments) wide_arguments.push_back(utf8_to_wide(argument));
    return windows_process(program, wide_arguments, stdout_path, stderr_path);
#else
    return posix_process(program, arguments, stdout_path, stderr_path);
#endif
}

int link_llvm(const fs::path& llvm, const fs::path& output, LinkOptions options) {
#ifdef _WIN32
    std::vector<std::wstring> arguments{
        (options.debug || !options.optimize) ? L"-O0" : L"-O3",
        L"-fms-runtime-lib=dll",
        L"-Xlinker",
        L"/NODEFAULTLIB:libcmt",
        L"-Wno-override-module",
        L"-x",
        L"ir",
        llvm.native(),
        L"-x",
        L"none",
        runtime_library().native(),
    };
    for (const auto& input : options.inputs) {
        if (!fs::is_regular_file(input))
            throw std::runtime_error("--link input is not a regular file: " + input.string());
        arguments.push_back(input.native());
    }
    arguments.insert(arguments.end(), {
        L"-Xlinker",
        L"/DEFAULTLIB:legacy_stdio_definitions",
        L"-o",
        output.native(),
    });
    if (options.debug) {
        arguments.emplace_back(L"-g");
        arguments.emplace_back(L"-fno-omit-frame-pointer");
    } else if (options.optimize) {
        arguments.emplace_back(L"-Xlinker");
        arguments.emplace_back(L"/OPT:REF");
    }
#ifdef QUIDRA_SANITIZE_GENERATED
    arguments.emplace_back(L"-fsanitize=address,undefined");
    arguments.emplace_back(L"-fno-omit-frame-pointer");
#endif
    if (llvm_uses_http(llvm)) arguments.emplace_back(L"-lcurl");
    if (llvm_uses_image(llvm)) {
        if (const auto configured = environment_value("QUIDRA_IMAGE_LIBRARY_PATH");
            configured && !configured->empty()) {
            arguments.emplace_back(L"-L" + utf8_to_wide(*configured));
        } else if (const auto vcpkg = environment_value("VCPKG_INSTALLATION_ROOT");
                   vcpkg && !vcpkg->empty()) {
            const auto library_dir =
                fs::path(*vcpkg) / "installed" / "x64-windows" / "lib";
            arguments.emplace_back(L"-L" + library_dir.native());
        }
        arguments.emplace_back(L"-lpng16");
        arguments.emplace_back(L"-ljpeg");
        arguments.emplace_back(L"-ltiff");
        arguments.emplace_back(L"-lwebp");
    }
    return windows_process(fs::path(clang_driver()), arguments);
#else
    std::vector<std::string> arguments{
        (options.debug || !options.optimize) ? "-O0" : "-O3",
        "-Wno-override-module",
        "-x",
        "ir",
        llvm.string(),
        "-x",
        "none",
        runtime_library().string(),
    };
    for (const auto& input : options.inputs) {
        if (!fs::is_regular_file(input))
            throw std::runtime_error("--link input is not a regular file: " + input.string());
        arguments.push_back(input.string());
    }
    arguments.insert(arguments.end(), {
        "-o",
        output.string(),
        "-lm",
        "-pthread",
    });
#ifdef __APPLE__
    arguments.emplace_back("-framework");
    arguments.emplace_back("Foundation");
    arguments.emplace_back("-framework");
    arguments.emplace_back("Metal");
#else
    arguments.emplace_back("-ldl");
#endif
    if (options.debug) {
        arguments.emplace_back("-g");
        arguments.emplace_back("-fno-omit-frame-pointer");
    } else if (options.optimize) {
#ifdef __APPLE__
        arguments.emplace_back("-Wl,-dead_strip");
#else
        arguments.emplace_back("-Wl,--gc-sections");
#endif
    }
#ifdef QUIDRA_SANITIZE_GENERATED
    arguments.emplace_back("-fsanitize=address,undefined");
    arguments.emplace_back("-fno-omit-frame-pointer");
#endif
    if (llvm_uses_http(llvm)) arguments.emplace_back("-lcurl");
    if (llvm_uses_image(llvm)) {
        if (const char* configured = std::getenv("QUIDRA_IMAGE_LIBRARY_PATH");
            configured && *configured) {
            arguments.emplace_back(std::string("-L") + configured);
        } else {
#ifdef __APPLE__
            for (const auto& directory : {
                     fs::path("/opt/homebrew/lib"),
                     fs::path("/usr/local/lib"),
                     fs::path("/opt/homebrew/opt/libpng/lib"),
                     fs::path("/opt/homebrew/opt/jpeg-turbo/lib"),
                     fs::path("/opt/homebrew/opt/libtiff/lib"),
                     fs::path("/opt/homebrew/opt/webp/lib"),
                     fs::path("/usr/local/opt/libpng/lib"),
                     fs::path("/usr/local/opt/jpeg-turbo/lib"),
                     fs::path("/usr/local/opt/libtiff/lib"),
                     fs::path("/usr/local/opt/webp/lib")}) {
                if (fs::is_directory(directory)) {
                    arguments.emplace_back("-L" + directory.string());
                }
            }
#endif
        }
        arguments.emplace_back("-lpng");
        arguments.emplace_back("-ljpeg");
        arguments.emplace_back("-ltiff");
        arguments.emplace_back("-lwebp");
    }
    return posix_process(fs::path(clang_driver()), arguments);
#endif
}


int run_debugger(
    const fs::path& program,
    const std::vector<std::string>& program_arguments) {
    const auto debugger=debugger_driver();
    const auto name=fs::path(debugger).filename().string();
#ifdef _WIN32
    std::vector<std::wstring> arguments;
    if (name.find("gdb") != std::string::npos) arguments.emplace_back(L"--args");
    else arguments.emplace_back(L"--");
    arguments.push_back(program.native());
    for (const auto& argument : program_arguments)
        arguments.push_back(utf8_to_wide(argument));
    return windows_process(fs::path(debugger),arguments);
#else
    std::vector<std::string> arguments;
    if (name.find("gdb") != std::string::npos) arguments.emplace_back("--args");
    else arguments.emplace_back("--");
    arguments.push_back(program.string());
    arguments.insert(arguments.end(),program_arguments.begin(),program_arguments.end());
    return posix_process(fs::path(debugger),arguments);
#endif
}

} // namespace quidra::native
