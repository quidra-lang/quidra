#include "platform/process.hpp"
#include "platform/environment.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#else
#include "platform/file_descriptor.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#endif

namespace fs = std::filesystem;

namespace quidra::platform {

namespace {
void validate_environment(const RuntimeEnvironment& environment) {
    for (const auto& [name, value] : environment)
        if (name.empty() || name.find('=') != std::string::npos || name.find('\0') != std::string::npos ||
                (value && value->find('\0') != std::string::npos))
            throw std::runtime_error("invalid child environment name or value");
}
} // namespace

#ifdef _WIN32
namespace {

struct WindowsEnvironmentLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const {
        return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    }
};
std::vector<wchar_t> child_environment(const RuntimeEnvironment& overrides) {
    validate_environment(overrides);
    std::map<std::wstring, std::wstring, WindowsEnvironmentLess> values;
    for (const auto& record : environment_native_snapshot()) {
        // Windows stores drive working directories as =C:=..., with the
        // separator at the second '='. Keep those inherited records intact.
        const auto separator = record.find(L'=', record.front() == L'=' ? 1 : 0);
        if (separator == std::wstring::npos)
            throw std::runtime_error("invalid inherited process environment");
        values.insert_or_assign(record.substr(0, separator), record.substr(separator + 1));
    }
    for (const auto& [name, value] : overrides) {
        const auto key = native_text(name);
        if (value) values.insert_or_assign(key, native_text(*value));
        else values.erase(key);
    }
    std::vector<wchar_t> block;
    for (const auto& [name, value] : values) {
        block.insert(block.end(), name.begin(), name.end()); block.push_back(L'=');
        block.insert(block.end(), value.begin(), value.end()); block.push_back(L'\0');
    }
    if (block.empty()) block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
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

} // namespace

int run_native_program(
    const fs::path& program,
    const std::vector<NativeText>& arguments,
    const std::optional<fs::path>& stdout_path,
    const std::optional<fs::path>& stderr_path,
    const std::optional<fs::path>& stdin_path,
    const RuntimeEnvironment& environment) {
    std::vector<wchar_t> environment_block;
    if (!environment.empty()) environment_block = child_environment(environment);
    std::wstring command = windows_quote_wide(program.native());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += windows_quote_wide(argument);
    }
    command.push_back(L'\0');

    HANDLE input = INVALID_HANDLE_VALUE;
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

    if (stdin_path) {
        input = CreateFileW(
            stdin_path->c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (input == INVALID_HANDLE_VALUE)
            throw std::runtime_error("cannot open process stdin file");
    }
    output = open_redirect(stdout_path);
    if (stdout_path && output == INVALID_HANDLE_VALUE) {
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        throw std::runtime_error("cannot open process stdout file");
    }
    error = open_redirect(stderr_path);
    if (stderr_path && error == INVALID_HANDLE_VALUE) {
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        throw std::runtime_error("cannot open process stderr file");
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    BOOL inherit = FALSE;
    if (stdin_path || stdout_path || stderr_path) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput =
            input != INVALID_HANDLE_VALUE ? input : GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput =
            output != INVALID_HANDLE_VALUE ? output : GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError =
            error != INVALID_HANDLE_VALUE ? error : GetStdHandle(STD_ERROR_HANDLE);
        inherit = TRUE;
    }

    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        nullptr, command.data(), nullptr, nullptr, inherit,
        environment.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT,
        environment.empty() ? nullptr : environment_block.data(), nullptr,
        &startup, &process);
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();

    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
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
#else
namespace {
struct PosixEnvironment {
    std::vector<std::string> storage;
    std::vector<char*> pointers;
    PosixEnvironment(const fs::path& program, const RuntimeEnvironment& overrides) {
        if (overrides.empty()) return;
        validate_environment(overrides);
        if (!program.is_absolute())
            throw std::runtime_error("child environment overrides require an absolute executable");
        storage = environment_native_snapshot();
        for (const auto& [name, value] : overrides) {
            const auto prefix = name + '=';
            std::erase_if(storage, [&](const std::string& entry) { return entry.starts_with(prefix); });
            if (value) storage.push_back(prefix + *value);
        }
        pointers.reserve(storage.size() + 1);
        for (auto& entry : storage) pointers.push_back(entry.data());
        pointers.push_back(nullptr);
    }
};
} // namespace
int run_native_program(
    const fs::path& program,
    const std::vector<NativeText>& arguments,
    const std::optional<fs::path>& stdout_path,
    const std::optional<fs::path>& stderr_path,
    const std::optional<fs::path>& stdin_path,
    const RuntimeEnvironment& environment) {
    const PosixEnvironment child_environment(program, environment);
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
        const auto redirect = [](const std::optional<fs::path>& path, int target, int flags) {
            if (!path) return;
            const int fd = ::open(path->c_str(), flags, 0666);
            if (fd < 0 || ::dup2(fd, target) < 0) _exit(126);
            if (fd != target) ::close(fd);
        };
        redirect(stdin_path, STDIN_FILENO, O_RDONLY);
        redirect(stdout_path, STDOUT_FILENO, O_WRONLY | O_CREAT | O_TRUNC);
        redirect(stderr_path, STDERR_FILENO, O_WRONLY | O_CREAT | O_TRUNC);
        if (environment.empty()) ::execvp(program.c_str(), argv.data());
        else ::execve(program.c_str(), argv.data(), child_environment.pointers.data());
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

int run_native_program(
    const fs::path& program,
    const std::vector<NativeText>& arguments,
    const std::optional<fs::path>& stdout_path,
    const std::optional<fs::path>& stderr_path) {
    return run_native_program(program, arguments, stdout_path, stderr_path, std::nullopt);
}

int run_program(
    const fs::path& program,
    const std::vector<std::string>& arguments,
    const std::optional<fs::path>& stdout_path,
    const std::optional<fs::path>& stderr_path,
    const RuntimeEnvironment& environment) {
#ifdef _WIN32
    std::vector<NativeText> native_arguments;
    native_arguments.reserve(arguments.size());
    for (const auto& argument : arguments) native_arguments.push_back(native_text(argument));
    return run_native_program(program, native_arguments, stdout_path, stderr_path, std::nullopt, environment);
#else
    return run_native_program(program, arguments, stdout_path, stderr_path, std::nullopt, environment);
#endif
}

#ifdef _WIN32
ProgramRun run_program_reporting_start(
    const fs::path& program, const std::vector<std::string>& arguments,
    const RuntimeEnvironment& environment) {
    try {
        return ProgramRun{true, run_program(program, arguments, std::nullopt, std::nullopt, environment), {}};
    } catch (const std::runtime_error& error) {
        const std::string_view message = error.what();
        if (message.rfind("cannot start process", 0) != 0) throw;
        return ProgramRun{false, 1, std::string(message)};
    }
}
#else
ProgramRun run_program_reporting_start(
    const fs::path& program, const std::vector<std::string>& arguments,
    const RuntimeEnvironment& environment) {
    const PosixEnvironment child_environment(program, environment);
    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1);
    storage.push_back(program.string());
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& argument : storage) argv.push_back(argument.data());
    argv.push_back(nullptr);

    int report[2];
    if (::pipe(report) != 0) throw std::runtime_error("failed to fork process");
    for (const int fd : report) ::fcntl(fd, F_SETFD, FD_CLOEXEC);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(report[0]);
        ::close(report[1]);
        throw std::runtime_error("failed to fork process");
    }
    if (pid == 0) {
        ::close(report[0]);
        if (environment.empty()) ::execvp(program.c_str(), argv.data());
        else ::execve(program.c_str(), argv.data(), child_environment.pointers.data());
        const int error = errno;
        (void)!::write(report[1], &error, sizeof(error));
        _exit(error == ENOENT ? 127 : 126);
    }
    ::close(report[1]);
    int exec_error = 0;
    std::size_t received = 0;
    while (received < sizeof(exec_error)) {
        const auto count = ::read(
            report[0], reinterpret_cast<char*>(&exec_error) + received,
            sizeof(exec_error) - received);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        received += static_cast<std::size_t>(count);
    }
    ::close(report[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        throw std::runtime_error("failed to wait for process");
    }
    ProgramRun run;
    if (WIFEXITED(status)) run.status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) run.status = 128 + WTERMSIG(status);
    else run.status = 1;
    if (received == sizeof(exec_error)) {
        run.started = false;
        run.failure = std::strerror(exec_error);
    }
    return run;
}
#endif

bool write_standard_error(std::string_view data) {
#ifdef _WIN32
    const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return false;
    while (!data.empty()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(data.size(), 1u << 30));
        DWORD written = 0;
        if (!WriteFile(handle, data.data(), chunk, &written, nullptr) || written == 0)
            return false;
        data.remove_prefix(written);
    }
    return true;
#else
    return write_all(STDERR_FILENO, data);
#endif
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

} // namespace quidra::platform
