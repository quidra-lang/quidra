#include "native_build.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <optional>
#include <random>
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

std::optional<fs::path> command_path(std::string_view candidate) {
#ifdef _WIN32
    const auto wide = utf8_to_wide(candidate);
    std::wstring buffer(32768, L'\0');
    const DWORD length = SearchPathW(
        nullptr, wide.c_str(), nullptr, static_cast<DWORD>(buffer.size()),
        buffer.data(), nullptr);
    if (length == 0 || length >= buffer.size()) return std::nullopt;
    buffer.resize(length);
    return fs::path(buffer);
#else
    const fs::path requested(candidate);
    if (requested.has_parent_path()) {
        if (::access(requested.c_str(), X_OK) != 0) return std::nullopt;
        std::error_code error;
        const auto canonical = fs::weakly_canonical(requested, error);
        return error ? fs::absolute(requested).lexically_normal() : canonical;
    }
    const char* raw_path = std::getenv("PATH");
    if (!raw_path) return std::nullopt;
    std::string_view path(raw_path);
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find(':', start);
        const auto part = path.substr(
            start, end == std::string_view::npos ? path.size() - start : end - start);
        const fs::path directory =
            part.empty() ? fs::path(".") : fs::path(std::string(part));
        const auto executable = directory / requested;
        if (::access(executable.c_str(), X_OK) == 0) {
            std::error_code error;
            const auto canonical = fs::weakly_canonical(executable, error);
            return error ? fs::absolute(executable).lexically_normal() : canonical;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return std::nullopt;
#endif
}

bool command_available(const char* candidate) {
    return command_path(candidate).has_value();
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

std::string cuda_driver() {
    if (const auto configured = environment_value("QUIDRA_NVCC");
        configured && !configured->empty()) {
        if (!command_path(*configured)) {
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
        if (command_available(candidate)) return candidate;
    }
    throw std::runtime_error(
        "package-owned .cu sources require NVIDIA nvcc; install the CUDA toolkit "
        "or set QUIDRA_NVCC");
}

fs::path cuda_toolkit_root() {
    for (const char* name : {"QUIDRA_CUDA_HOME", "CUDA_HOME", "CUDA_PATH"}) {
        if (const auto configured = environment_value(name);
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
    const auto driver = command_path(cuda_driver());
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

std::string jit_driver() {
    if (const auto configured = environment_value("QUIDRA_LLI");
        configured && !configured->empty()) {
        if (!command_available(configured->c_str()) && !fs::is_regular_file(fs::path(*configured))) {
            throw std::runtime_error(
                "QUIDRA_LLI is not executable or not on PATH: " + *configured);
        }
        return *configured;
    }
#ifdef _WIN32
    const fs::path installed = "C:/Program Files/LLVM/bin/lli.exe";
    if (fs::is_regular_file(installed)) return installed.string();
    for (const char* candidate :
         {"lli.exe", "lli-20.exe", "lli-19.exe", "lli-18.exe", "lli-17.exe", "lli-16.exe", "lli-15.exe"}) {
#elif defined(__APPLE__)
    for (const fs::path& installed : {
             fs::path("/opt/homebrew/opt/llvm/bin/lli"),
             fs::path("/usr/local/opt/llvm/bin/lli")}) {
        if (fs::is_regular_file(installed)) return installed.string();
    }
    for (const char* candidate :
         {"lli-20", "lli-19", "lli-18", "lli-17", "lli-16", "lli-15", "lli"}) {
#else
    for (const char* candidate :
         {"lli-20", "lli-19", "lli-18", "lli-17", "lli-16", "lli-15", "lli"}) {
#endif
        if (command_available(candidate)) return candidate;
    }
    throw std::runtime_error(
        "LLVM lli 15 or newer is required for JIT execution; install LLVM or set QUIDRA_LLI");
}

fs::path self_executable() {
    return executable_path();
}

#ifdef __APPLE__
std::vector<fs::path> llvm_runtime_roots(const std::string& lli_driver) {
    std::vector<fs::path> roots;
    const fs::path driver_path = lli_driver;
    if (driver_path.has_parent_path()) {
        const auto bin = driver_path.parent_path();
        if (bin.filename() == "bin") roots.push_back(bin.parent_path());
    }
    roots.emplace_back("/opt/homebrew/opt/llvm");
    roots.emplace_back("/usr/local/opt/llvm");
    return roots;
}

fs::path find_clang_runtime_library(
    const std::vector<fs::path>& roots,
    const std::vector<std::string_view>& names) {
    for (const auto& root : roots) {
        const auto clang_root = root / "lib" / "clang";
        std::error_code ec;
        if (!fs::is_directory(clang_root, ec) || ec) continue;
        for (fs::recursive_directory_iterator it(
                 clang_root, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec)) {
            if (!it->is_regular_file(ec) || ec) {
                ec.clear();
                continue;
            }
            const auto name = it->path().filename().string();
            for (const auto candidate : names) {
                if (name == candidate) return it->path();
            }
        }
    }
    return {};
}

fs::path orc_runtime_library(const std::string& lli_driver) {
    if (const auto configured = environment_value("QUIDRA_ORC_RUNTIME");
        configured && !configured->empty()) {
        const fs::path path = *configured;
        if (fs::is_regular_file(path)) return path;
        throw std::runtime_error(
            "QUIDRA_ORC_RUNTIME does not name a file: " + path.string());
    }

    const auto found = find_clang_runtime_library(
        llvm_runtime_roots(lli_driver),
        {"libclang_rt.orc_osx.a", "liborc_rt_osx.a", "liborc_rt.a"});
    if (!found.empty()) return found;

    throw std::runtime_error(
        "LLVM ORC runtime for macOS was not found; install Homebrew llvm or set "
        "QUIDRA_ORC_RUNTIME to libclang_rt.orc_osx.a");
}

fs::path compiler_rt_builtins_library(const std::string& lli_driver) {
    if (const auto configured = environment_value("QUIDRA_COMPILER_RT_BUILTINS");
        configured && !configured->empty()) {
        const fs::path path = *configured;
        if (fs::is_regular_file(path)) return path;
        throw std::runtime_error(
            "QUIDRA_COMPILER_RT_BUILTINS does not name a file: " + path.string());
    }

    const auto found = find_clang_runtime_library(
        llvm_runtime_roots(lli_driver), {"libclang_rt.osx.a"});
    if (!found.empty()) return found;

    throw std::runtime_error(
        "compiler-rt builtins for macOS were not found; install Homebrew llvm or set "
        "QUIDRA_COMPILER_RT_BUILTINS to libclang_rt.osx.a");
}
#endif

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

fs::path jit_runtime_library() {
    if (const auto configured = environment_value("QUIDRA_JIT_RUNTIME_LIBRARY");
        configured && !configured->empty()) {
        fs::path path = *configured;
        if (fs::is_regular_file(path)) return path;
        throw std::runtime_error(
            "QUIDRA_JIT_RUNTIME_LIBRARY does not name a file: " + path.string());
    }

    const auto bin = executable_path().parent_path();
#ifdef _WIN32
    const char* library_name = "quidra_runtime_jit.dll";
#elif defined(__APPLE__)
    const char* library_name = "libquidra_runtime_jit.dylib";
#else
    const char* library_name = "libquidra_runtime_jit.so";
#endif

    const std::array<fs::path, 6> candidates{{
        bin / library_name,
        (bin / "../lib/quidra" / library_name).lexically_normal(),
        (bin / "../lib64/quidra" / library_name).lexically_normal(),
        (bin / "../lib/x86_64-linux-gnu/quidra" / library_name).lexically_normal(),
        (bin / "../lib/aarch64-linux-gnu/quidra" / library_name).lexically_normal(),
        (bin / "../lib" / library_name).lexically_normal(),
    }};
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && !ec) return candidate;
    }

    throw std::runtime_error(
        "cannot locate the Quidra JIT runtime library; set QUIDRA_JIT_RUNTIME_LIBRARY explicitly");
}

std::optional<fs::path> native_extension_include_directory() {
    if (const auto configured = environment_value("QUIDRA_NATIVE_INCLUDE_DIR");
        configured && !configured->empty()) {
        fs::path root = fs::absolute(*configured).lexically_normal();
        if (fs::is_regular_file(root / "quidra" / "native_extension.h"))
            return root;
        throw std::runtime_error(
            "QUIDRA_NATIVE_INCLUDE_DIR must contain quidra/native_extension.h: " +
            root.string());
    }

    const auto bin = executable_path().parent_path();
    const std::array<fs::path, 3> candidates{{
        (bin / "../include").lexically_normal(),
        (bin / "../../include").lexically_normal(),
        (bin / "include").lexically_normal(),
    }};
    for (const auto& root : candidates) {
        std::error_code error;
        if (fs::is_regular_file(
                root / "quidra" / "native_extension.h", error) && !error)
            return fs::absolute(root).lexically_normal();
    }
    return std::nullopt;
}

class TemporaryJitNativeObjects {
public:
    TemporaryJitNativeObjects() {
        const auto base = fs::temp_directory_path();
        std::random_device random;
        const auto now = static_cast<unsigned long long>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            const auto nonce =
                (static_cast<unsigned long long>(random()) << 32U) ^
                random() ^ now ^ attempt;
            root_ = base / ("quidra-jit-native-" + std::to_string(nonce));
            std::error_code error;
            if (fs::create_directory(root_, error)) return;
        }
        throw std::runtime_error(
            "cannot create temporary directory for JIT native sources");
    }

    TemporaryJitNativeObjects(const TemporaryJitNativeObjects&) = delete;
    TemporaryJitNativeObjects& operator=(const TemporaryJitNativeObjects&) = delete;

    ~TemporaryJitNativeObjects() {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    fs::path object(std::size_t index) const {
#ifdef _WIN32
        return root_ / ("native-" + std::to_string(index) + ".obj");
#else
        return root_ / ("native-" + std::to_string(index) + ".o");
#endif
    }

    fs::path library() const {
#ifdef _WIN32
        return root_ / "native-package.dll";
#elif defined(__APPLE__)
        return root_ / "libnative-package.dylib";
#else
        return root_ / "libnative-package.so";
#endif
    }

private:
    fs::path root_;
};

bool cuda_native_source(const fs::path& source) {
    auto extension = source.extension().string();
    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return extension == ".cu";
}

std::vector<std::string> native_source_language_flags(
    const fs::path& source) {
    const auto original_extension = source.extension().string();
    auto extension = original_extension;
    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (extension == ".c") return {"-x", "c", "-std=c17"};
    if (extension == ".cc" || extension == ".cpp" ||
        extension == ".cxx" || extension == ".c++") {
        return {"-x", "c++", "-std=c++20"};
    }
#ifdef __APPLE__
    if (extension == ".mm")
        return {"-x", "objective-c++", "-std=c++20"};
#endif
    if (original_extension == ".S") return {"-x", "assembler-with-cpp"};
    if (extension == ".s") return {"-x", "assembler"};
    return {};
}

bool directly_compilable_native_source(const fs::path& source) {
    return cuda_native_source(source) ||
           !native_source_language_flags(source).empty();
}

void compile_jit_native_source(
    const fs::path& source,
    const fs::path& object,
    bool optimize,
    const std::vector<std::string>& extra_compile_flags = {}) {
    if (!fs::is_regular_file(source)) {
        throw std::runtime_error(
            "JIT native source is not a regular file: " + source.string());
    }

    std::vector<std::string> arguments;
    arguments.emplace_back(optimize ? "-O2" : "-O0");
    const bool cuda = cuda_native_source(source);
    if (cuda) {
        arguments.emplace_back("-std=c++20");
#ifndef _WIN32
        arguments.emplace_back("-Xcompiler=-fPIC");
#endif
    } else {
#ifndef _WIN32
        arguments.emplace_back("-fPIC");
#endif
        const auto language_flags = native_source_language_flags(source);
        if (language_flags.empty()) {
            throw std::runtime_error(
                "unsupported package native source type: " + source.string());
        }
        arguments.insert(
            arguments.end(), language_flags.begin(), language_flags.end());
    }
    if (const auto include = native_extension_include_directory()) {
        arguments.emplace_back("-I" + include->string());
    }
    arguments.insert(
        arguments.end(), extra_compile_flags.begin(), extra_compile_flags.end());
    arguments.emplace_back("-c");
    arguments.emplace_back(fs::absolute(source).lexically_normal().string());
    arguments.emplace_back("-o");
    arguments.emplace_back(object.string());

    const auto compiler = cuda ? cuda_driver() : clang_driver();
    if (run_program(fs::path(compiler), arguments) != 0 ||
        !fs::is_regular_file(object)) {
        throw std::runtime_error(
            "failed to compile package native source: " + source.string());
    }
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

std::vector<std::string> split_native_flags(std::string_view text) {
    std::vector<std::string> result;
    std::string current;
    bool single = false;
    bool quoted = false;
    bool escaped = false;
    const auto flush = [&]() {
        if (!current.empty()) {
            result.push_back(current);
            current.clear();
        }
    };
    for (const char ch : text) {
        if (escaped) {
            current.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\' && !single) {
            escaped = true;
            continue;
        }
        if (ch == '\'' && !quoted) {
            single = !single;
            continue;
        }
        if (ch == '"' && !single) {
            quoted = !quoted;
            continue;
        }
        if (!single && !quoted &&
            (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')) {
            flush();
            continue;
        }
        current.push_back(ch);
    }
    if (escaped || single || quoted)
        throw std::runtime_error("pkg-config returned malformed quoting");
    flush();
    return result;
}

std::vector<std::string> pkg_config_query(
    const std::vector<std::string>& modules,
    std::string_view option) {
    if (modules.empty()) return {};
    const auto configured = environment_value("QUIDRA_PKG_CONFIG");
    const std::string program =
        configured && !configured->empty() ? *configured : "pkg-config";
    if (!command_available(program.c_str()) &&
        !fs::is_regular_file(fs::path(program))) {
        throw std::runtime_error(
            "package native dependencies require pkg-config; install it or set QUIDRA_PKG_CONFIG");
    }

    const auto base = fs::temp_directory_path();
    std::random_device random;
    const auto output = base /
        ("quidra-pkg-config-" + std::to_string(random()) + ".txt");
    std::vector<std::string> arguments;
    arguments.emplace_back(option);
    arguments.insert(arguments.end(), modules.begin(), modules.end());
    const int status = run_program(fs::path(program), arguments, output);
    std::ifstream in(output, std::ios::binary);
    std::ostringstream contents;
    contents << in.rdbuf();
    std::error_code error;
    fs::remove(output, error);
    if (status != 0) {
        throw std::runtime_error(
            "pkg-config failed for package native dependencies");
    }
    return split_native_flags(contents.str());
}

void compile_jit_native_library(
    const std::vector<fs::path>& sources,
    const fs::path& output,
    bool optimize,
    const std::vector<std::string>& compile_flags,
    const std::vector<std::string>& link_flags) {
#ifdef _WIN32
    (void)sources;
    (void)output;
    (void)optimize;
    (void)compile_flags;
    (void)link_flags;
    throw std::runtime_error(
        "pkg-config native dependencies are not yet supported by the Windows REPL JIT; use AOT run/build");
#else
    std::vector<std::string> arguments;
    arguments.emplace_back(optimize ? "-O2" : "-O0");
    arguments.emplace_back("-shared");
    arguments.emplace_back("-fPIC");
#ifdef __APPLE__
    arguments.emplace_back("-Wl,-undefined,dynamic_lookup");
#endif
    if (const auto include = native_extension_include_directory())
        arguments.emplace_back("-I" + include->string());

    std::vector<fs::path> objects;
    objects.reserve(sources.size());
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto& source = sources[index];
        if (!fs::is_regular_file(source))
            throw std::runtime_error(
                "JIT native source is not a regular file: " + source.string());
#ifdef _WIN32
        const auto object =
            output.parent_path() / ("pkg-" + std::to_string(index) + ".obj");
#else
        const auto object =
            output.parent_path() / ("pkg-" + std::to_string(index) + ".o");
#endif
        compile_jit_native_source(
            source, object, optimize, compile_flags);
        objects.push_back(object);
    }
    for (const auto& object : objects)
        arguments.push_back(fs::absolute(object).lexically_normal().string());
    arguments.insert(arguments.end(), link_flags.begin(), link_flags.end());
    const bool uses_cuda = std::any_of(
        sources.begin(), sources.end(),
        [](const fs::path& source) { return cuda_native_source(source); });
    if (uses_cuda) {
        const auto cuda_library = cuda_runtime_library_directory();
        arguments.emplace_back("-L" + cuda_library.string());
        arguments.emplace_back("-Wl,-rpath," + cuda_library.string());
        arguments.emplace_back("-lcudart");
    }
    arguments.emplace_back("-o");
    arguments.emplace_back(output.string());
    if (run_program(fs::path(clang_driver()), arguments) != 0 ||
        !fs::is_regular_file(output)) {
        throw std::runtime_error(
            "failed to build JIT package native library");
    }
#endif
}

int run_llvm_jit(
    const fs::path& llvm,
    const std::vector<std::string>& arguments,
    JitOptions options,
    const std::optional<fs::path>& stdout_path,
    const std::optional<fs::path>& stderr_path) {
    if (!fs::is_regular_file(llvm)) {
        throw std::runtime_error("JIT input is not a regular LLVM IR file: " + llvm.string());
    }

    const auto driver = jit_driver();
    std::vector<std::string> jit_arguments;
    jit_arguments.reserve(arguments.size() + 8);
    // Explicitly select ORC so Quidra never silently falls back to LLVM's
    // interpreter execution mode.
    jit_arguments.emplace_back("--jit-kind=orc");
#ifdef __APPLE__
    // ORC lowers JITed thread_local globals to emulated TLS on Darwin.
    // Supplying compiler-rt's ORC runtime provides the TLS entry point and
    // lifecycle instead of relying on process-symbol lookup.
    jit_arguments.emplace_back("--jit-linker=jitlink");
    jit_arguments.emplace_back("--orc-runtime=" + orc_runtime_library(driver).string());
    // JITed TLS on Darwin is lowered through compiler-rt's emulated TLS ABI.
    // The ORC platform runtime does not provide __emutls_get_address itself,
    // so make the ordinary compiler-rt builtins archive available to JITLink.
    jit_arguments.emplace_back(
        "--extra-archive=" + compiler_rt_builtins_library(driver).string());
#endif
    jit_arguments.emplace_back(options.optimize ? "-O2" : "-O0");
    jit_arguments.emplace_back("--dlopen=" + jit_runtime_library().string());

    for (const auto& library : options.libraries) {
        if (!fs::is_regular_file(library)) {
            throw std::runtime_error(
                "JIT native library is not a regular file: " + library.string());
        }
        jit_arguments.emplace_back(
            "--dlopen=" + fs::absolute(library).lexically_normal().string());
    }

    std::optional<TemporaryJitNativeObjects> native_objects;
    if (!options.sources.empty()) {
        native_objects.emplace();
        const auto compile_flags =
            pkg_config_query(options.pkg_config_modules, "--cflags");
#ifdef _WIN32
        const bool uses_cuda = std::any_of(
            options.sources.begin(), options.sources.end(),
            [](const fs::path& source) { return cuda_native_source(source); });
        // Windows still uses direct ORC objects for dependency-free native
        // sources; the shared-library helper does not yet support that host.
        const bool link_native_library =
            !options.pkg_config_modules.empty() || uses_cuda;
#else
        // POSIX package-native C/C++ translation units may contain ordinary
        // platform TLS and C++ runtime relocations that ORC does not support
        // when a raw object is injected with --extra-object. Link them as a
        // shared library so the platform dynamic loader owns TLS/runtime
        // relocation while Core remains unaware of package semantics.
        const bool link_native_library = true;
#endif
        if (!link_native_library) {
            for (std::size_t index = 0; index < options.sources.size(); ++index) {
                const auto object = native_objects->object(index);
                compile_jit_native_source(
                    options.sources[index], object, options.optimize, compile_flags);
                jit_arguments.emplace_back(
                    "--extra-object=" + fs::absolute(object).lexically_normal().string());
            }
        } else {
            const auto library = native_objects->library();
            const auto link_flags =
                pkg_config_query(options.pkg_config_modules, "--libs");
            compile_jit_native_library(
                options.sources, library, options.optimize,
                compile_flags, link_flags);
            jit_arguments.emplace_back(
                "--dlopen=" + fs::absolute(library).lexically_normal().string());
        }
    }

    jit_arguments.emplace_back("--fake-argv0=" + options.argv0);
    jit_arguments.emplace_back(llvm.string());
    jit_arguments.insert(jit_arguments.end(), arguments.begin(), arguments.end());

    return run_program(
        fs::path(driver), jit_arguments, stdout_path, stderr_path);
}

int link_llvm(const fs::path& llvm, const fs::path& output, LinkOptions options) {
    const auto pkg_compile_flags =
        pkg_config_query(options.pkg_config_modules, "--cflags");
    const auto pkg_link_flags =
        pkg_config_query(options.pkg_config_modules, "--libs");

    // Package-owned C/C++ sources are compiled as translation units before the
    // final LLVM link. This gives native packages a stable language standard
    // independent of the host compiler default and keeps mixed C/C++ packages
    // valid. Prebuilt objects/libraries continue through unchanged.
    std::optional<TemporaryJitNativeObjects> native_objects;
    std::vector<fs::path> link_inputs;
    link_inputs.reserve(options.inputs.size());
    std::size_t source_index = 0;
    bool uses_cuda = false;
    for (const auto& input : options.inputs) {
        if (!directly_compilable_native_source(input)) {
            link_inputs.push_back(input);
            continue;
        }
        if (!native_objects) native_objects.emplace();
        uses_cuda = uses_cuda || cuda_native_source(input);
        const auto object = native_objects->object(source_index++);
        compile_jit_native_source(
            input, object, options.optimize && !options.debug,
            pkg_compile_flags);
        link_inputs.push_back(object);
    }
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
    if (const auto include = native_extension_include_directory()) {
        arguments.emplace_back(L"-I" + include->native());
    }
    for (const auto& flag : pkg_compile_flags)
        arguments.emplace_back(utf8_to_wide(flag));
    for (const auto& input : link_inputs) {
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
    for (const auto& flag : pkg_link_flags)
        arguments.emplace_back(utf8_to_wide(flag));
    if (uses_cuda) {
        const auto cuda_library = cuda_runtime_library_directory();
        arguments.emplace_back(L"-L" + cuda_library.native());
        arguments.emplace_back(L"-lcudart");
    }
    if (llvm_uses_http(llvm)) arguments.emplace_back(L"-lcurl");
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
    if (const auto include = native_extension_include_directory()) {
        arguments.push_back("-I" + include->string());
    }
    arguments.insert(
        arguments.end(), pkg_compile_flags.begin(), pkg_compile_flags.end());
    std::vector<fs::path> runtime_library_dirs;
    for (const auto& input : link_inputs) {
        if (!fs::is_regular_file(input))
            throw std::runtime_error("--link input is not a regular file: " + input.string());
        arguments.push_back(input.string());
        const auto filename = input.filename().string();
        const bool shared =
#ifdef __APPLE__
            input.extension() == ".dylib";
#else
            input.extension() == ".so" || filename.find(".so.") != std::string::npos;
#endif
        if (shared) {
            const auto directory = fs::absolute(input.parent_path()).lexically_normal();
            if (std::find(runtime_library_dirs.begin(), runtime_library_dirs.end(),
                          directory) == runtime_library_dirs.end()) {
                runtime_library_dirs.push_back(directory);
            }
        }
    }
    for (const auto& directory : runtime_library_dirs) {
        arguments.push_back("-Wl,-rpath," + directory.string());
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
    arguments.emplace_back("-framework");
    arguments.emplace_back("MetalPerformanceShaders");
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
    arguments.insert(
        arguments.end(), pkg_link_flags.begin(), pkg_link_flags.end());
    if (uses_cuda) {
        const auto cuda_library = cuda_runtime_library_directory();
        arguments.emplace_back("-L" + cuda_library.string());
        arguments.emplace_back("-Wl,-rpath," + cuda_library.string());
        arguments.emplace_back("-lcudart");
    }
    if (llvm_uses_http(llvm)) arguments.emplace_back("-lcurl");
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
