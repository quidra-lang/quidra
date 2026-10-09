#include "platform/process.hpp"

#include <algorithm>
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

#ifdef _WIN32
namespace {

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
    const std::optional<fs::path>& stderr_path) {
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
#else
int run_native_program(
    const fs::path& program,
    const std::vector<NativeText>& arguments,
    const std::optional<fs::path>& stdout_path,
    const std::optional<fs::path>& stderr_path) {
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

int run_program(
    const fs::path& program,
    const std::vector<std::string>& arguments,
    const std::optional<fs::path>& stdout_path,
    const std::optional<fs::path>& stderr_path) {
#ifdef _WIN32
    std::vector<NativeText> native_arguments;
    native_arguments.reserve(arguments.size());
    for (const auto& argument : arguments) native_arguments.push_back(native_text(argument));
    return run_native_program(program, native_arguments, stdout_path, stderr_path);
#else
    return run_native_program(program, arguments, stdout_path, stderr_path);
#endif
}

#ifdef _WIN32
ProgramRun run_program_reporting_start(
    const fs::path& program, const std::vector<std::string>& arguments) {
    try {
        return ProgramRun{true, run_program(program, arguments), {}};
    } catch (const std::runtime_error& error) {
        const std::string_view message = error.what();
        if (message.rfind("cannot start process", 0) != 0) throw;
        return ProgramRun{false, 1, std::string(message)};
    }
}
#else
ProgramRun run_program_reporting_start(
    const fs::path& program, const std::vector<std::string>& arguments) {
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
        ::execvp(program.c_str(), argv.data());
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
